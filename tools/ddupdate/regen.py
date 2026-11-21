"""重新生成特征码和依据模式（修复工具用）。

特征码保持原条目的种类和语义：
  HOOK        从函数入口开始；steal 取不小于 5 字节的第一个指令边界，偷取范围不能有 rip 相对、分支、call、ret
              （sigs.check_hook 再完整检查一遍，含函数内跳进偷取范围的分支）；原模式把偷取字节写成 ?? 的照旧写 ??，
              有 DD_ENTRY 的给出新的入口字节
  CODE        从命中位置开始（函数入口，或者 MonsterBlockReturn 这类函数中间的固定点）；原模式开头有通配（入口可能
              被别的插件改写）的，照旧把不少于原通配长度的第一个指令边界之前写成 ??
  GLOBAL      从命中位置对应的指令开始，capture 重新计算；只有没有依据用这条特征码的命中位置（HIT、HIT_VALUE、
              WINDOW）时才允许往前扩展
  GLOBAL_ANY  规则照旧：所有命中解析到同一个地址；DD_RUNTIME_SELECT 的允许多个候选，但要包含定位到的地址，候选数不超过
              基线的候选数
rip 相对位移、分支的 rel 一律写成 ??，看起来是地址的位移和立即数（不小于 0x100000、落在映像某个节里的 32 位值，
例如以映像基址为基的 [rsi+rdi*8+RVA]；映像范围内的 64 位绝对地址）也写成 ??，其余字节原样保留；逐条加指令直到在 .text 里唯一（GLOBAL_ANY 是满足上面的规则），
而且至少 MIN_INSNS 条指令、MIN_FIXED 个非通配字节（U1a 的 suggest 生成的模式偏短，对下一次更新不够鲁棒），最长
MAX_BYTES 字节。不跨出命中所在的分块（没有异常目录表项的 thunk 除外，原 VectorDtor64b 的模式就跨过了填充字节）。
生成后用 sigs.resolve 和 check_hook 在新 exe 上自检，解析结果必须和定位结果一致。

依据模式：在依据的范围里从要读的指令开始（ENTRY 必须从起点开始），读的字段、rip 位移、分支 rel 写成 ??，扩展到在
范围里唯一，至少 EV_MIN_INSNS 条指令、EV_MIN_FIXED 个非通配字节（TEXT 范围用特征码的下限）；字段宽度变了（例如位移
从 disp8 变成 disp32）时读取方式跟着变。生成后用 evidence.evaluate 求值自检。
U1 修复 FB：先按原模式的范围生成（原模式里要读的指令前有 k 条、一共 n 条，新模式也从要读的指令往前 k 条开始、至少 n 条），
保住 FA 收紧依据时带上的上下文（同一串复制和界标、前后字段的解析），保不住时才退回上面的最短唯一模式，并返回一句说明。
"""
import dataclasses

from . import align
from . import evidence as evidence_mod
from . import sigs as sigs_mod
from .image import compile_pattern

MIN_INSNS = 6
MIN_FIXED = 12
MAX_BYTES = 96
EV_MIN_INSNS = 3
EV_MIN_FIXED = 6
MAX_BACK = 4
HOOK_MIN_STEAL = 5   # E9 rel32


RVA_LIKE_MIN = 0x100000   # 不小于它、又落在映像里的 32 位位移 / 立即数当作 RVA（结构偏移都远小于它）


def address_like(image, value, size):
    """位移或立即数是不是地址：映像里的 RVA（base+index*8+RVA 这种以映像基址为基的寻址），或者 64 位的绝对地址。"""
    if image is None:
        return False
    if size == 8:
        return image.base <= value < image.base + image.size
    if size == 4:
        value &= 0xFFFFFFFF
        return RVA_LIKE_MIN <= value < image.size and image.section_of(value) is not None
    return False


def masked(ins, wild=None, image=None):
    """指令字节：rip 相对位移、分支 rel、看起来是地址的位移和立即数（每个版本都会变）写成 None。"""
    out = list(ins.bytes)
    if ins.disp_size and (align.is_rip(ins) or address_like(image, ins.disp, ins.disp_size)):
        for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
            out[k] = None
    if ins.imm_size:
        imm = int.from_bytes(bytes(ins.bytes[ins.imm_offset:ins.imm_offset + ins.imm_size]), "little")
        if align.is_branch(ins) or address_like(image, imm, ins.imm_size):
            for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                out[k] = None
    if wild:
        for k in range(wild[0], wild[0] + wild[1]):
            out[k] = None
    return out


def to_text(values):
    return " ".join("??" if v is None else f"{v:02X}" for v in values)


def fixed(values):
    return sum(v is not None for v in values)


def leading_wild(pattern):
    n = 0
    for t in pattern.split():
        if t != "??":
            break
        n += 1
    return n


def linear(image, cs, start, allow_past_end=False):
    """从 start 线性解码到所在分块的结束（不在异常目录里、或允许越过时最多 2*MAX_BYTES 字节）。"""
    begin = image.entry_of(start)
    end = image.func_ends[begin] if begin is not None and not allow_past_end else start + 2 * MAX_BYTES
    return list(cs.disasm(image.bytes_at(start, end - start), start))


def text_hits(image, text):
    regex, _ = compile_pattern(text)
    return image.scan_text(regex)


def _grow(insns, lead, need_bytes, min_insns, min_fixed, accept, wild_at=None, image=None):
    """从 insns[0] 起逐条加指令，满足下限且 accept(模式) 为真时返回 (模式, 字节数)；wild_at=(指令下标, 偏移, 长度)。"""
    values = []
    for n, ins in enumerate(insns):
        if n and ins.address != insns[n - 1].address + insns[n - 1].size:
            break
        values += masked(ins, (wild_at[1], wild_at[2]) if wild_at and wild_at[0] == n else None, image)
        for k in range(min(lead, len(values))):
            values[k] = None
        if len(values) < need_bytes:
            continue
        trimmed = list(values)
        while len(trimmed) > need_bytes and trimmed[-1] is None:
            trimmed.pop()
        if n + 1 >= min_insns and fixed(trimmed) >= min_fixed and accept(to_text(trimmed)):
            return to_text(trimmed)
        if len(values) > MAX_BYTES:
            break
    return None


def _boundary_at_least(insns, count):
    pos = 0
    for ins in insns:
        if pos >= count:
            break
        pos += ins.size
    return pos


@dataclasses.dataclass
class SigPatch:
    pattern: str
    steal: int
    capture: int
    entry: str | None
    notes: list


def gen_sig(image, cs, sig, loc, base_entry, hit_bound, label=None, foreign=(), hit_check=None):
    """loc 是 locate.Location（已定位）。hit_bound：有依据用这条特征码的命中位置，不能往前扩展。
    hit_check(hit)：以 hit 为命中位置时成立的依据集合（GLOBAL_ANY 的命中位置是最低的模板实例，新模式最低的命中
    不是定位用的实例时，要求在定位用的实例上成立的依据在它上面也都成立）。返回 (SigPatch, None) 或 (None, 原因)。"""
    start = loc.hit
    notes = []
    if sig.kind == "HOOK":
        insns = linear(image, cs, start)
        steal = _boundary_at_least(insns, HOOK_MIN_STEAL)
        pos = 0
        for ins in insns:
            if pos >= steal:
                break
            if align.is_rip(ins) or ins.mnemonic in ("call", "jmp", "ret") or ins.mnemonic.startswith("j"):
                return None, f"hook 入口不安全：偷取范围里有 {ins.mnemonic} {ins.op_str}"
            pos += ins.size
        lead = steal if leading_wild(sig.pattern) >= sig.steal else 0
        entry = " ".join(f"{b:02X}" for b in image.bytes_at(start, steal)) if sig.entry is not None else None
        text = _grow(insns, lead, 0, MIN_INSNS, MIN_FIXED, lambda t: len(text_hits(image, t)) == 1, image=image)
        if text is None:
            return None, f"从入口起 {MAX_BYTES} 字节内生成不了唯一的模式"
        patch = SigPatch(text, steal, 0, entry, notes)
    elif sig.kind == "CODE":
        allow_past = not loc.insns or image.entry_of(start) is None
        insns = linear(image, cs, start, allow_past_end=allow_past)
        lead0 = leading_wild(sig.pattern)
        lead = _boundary_at_least(insns, lead0) if lead0 else 0
        text = _grow(insns, lead, 0, MIN_INSNS, MIN_FIXED, lambda t: len(text_hits(image, t)) == 1, image=image)
        if text is None:
            return None, f"从命中位置起 {MAX_BYTES} 字节内生成不了唯一的模式"
        patch = SigPatch(text, 0, 0, None, notes)
    else:
        cap_ins = loc.insns[loc.capture]
        span = align.field_span(cap_ins, loc.capture_field)
        if span is None:
            return None, "capture 指令没有对应的字段"
        old_count = len(base_entry.get("candidates") or []) if base_entry else 0

        def accept_for(begin):
            def accept(text):
                hits = text_hits(image, text)
                if sig.kind == "GLOBAL":
                    return len(hits) == 1
                if hit_bound and min(hits) != begin:
                    # 依据以最低的命中为起点（HIT）：新模式最低的命中换了实例时，原来成立的依据在新实例上也要成立
                    if hit_check is None or not hit_check(min(hits)) >= hit_check(begin):
                        return False
                capture = cap_ins.address + span[0] - begin
                targets = {h + capture + 4 + image.i32(h + capture) for h in hits}
                if not sig.runtime_select:
                    return targets == {loc.resolved}
                return loc.resolved in targets and len(targets) <= max(old_count, 1)
            return accept

        text, begin = None, start
        chunk = linear(image, cs, start)
        need =cap_ins.address + span[0] + span[1] - start
        text = _grow(chunk, 0, need, MIN_INSNS, MIN_FIXED, accept_for(start), image=image)
        if text is None and not hit_bound:
            chunk_begin = image.entry_of(start)
            if chunk_begin is not None:
                full = list(cs.disasm(image.bytes_at(chunk_begin, image.func_ends[chunk_begin] - chunk_begin), chunk_begin))
                idx = next((n for n, ins in enumerate(full) if ins.address == start), None)
                for back in range(1, MAX_BACK + 1):
                    if idx is None or idx - back < 0:
                        break
                    begin = full[idx - back].address
                    text = _grow(full[idx - back:], 0, cap_ins.address + span[0] + span[1] - begin, MIN_INSNS, MIN_FIXED,
                                 accept_for(begin), image=image)
                    if text:
                        notes.append(f"往前扩展了 {back} 条指令（这条特征码的命中位置没有依据在用）")
                        break
        if text is None:
            return None, "生成不了满足规则的模式" + ("（有依据用命中位置，不能往前扩展）" if hit_bound else "")
        patch = SigPatch(text, 0, cap_ins.address + span[0] - begin, None, notes)
        start = begin
    # 自检
    new_sig = dataclasses.replace(sig, pattern=patch.pattern, steal=patch.steal, capture=patch.capture, entry=patch.entry)
    res = sigs_mod.resolve(image, new_sig)
    sigs_mod.check_hook(image, cs, new_sig, res, list(foreign), label)
    if res.status != "ok":
        return None, "生成的模式自检不过：" + "；".join(res.problems)
    # GLOBAL_ANY 的命中位置是最低的那个模板实例，不一定是定位用的实例，只要求定位的实例在命中里
    hit_ok = start in res.hits if sig.kind == "GLOBAL_ANY" else res.hit == start
    if not hit_ok or res.resolved != loc.resolved:
        return None, (f"生成的模式命中 {[hex(h) for h in res.hits[:4]]}、解析到 {res.resolved:#x}，"
                      f"和定位结果（命中 {start:#x}、解析 {loc.resolved:#x}）不一致")
    patch.notes.append(f"{len(patch.pattern.split())} 字节，自检通过")
    return patch, None


def new_sig_result(image, cs, sig, patch, label=None, foreign=()):
    new_sig = dataclasses.replace(sig, pattern=patch.pattern, steal=patch.steal, capture=patch.capture, entry=patch.entry)
    res = sigs_mod.resolve(image, new_sig)
    sigs_mod.check_hook(image, cs, new_sig, res, list(foreign), label)
    return new_sig, res


# ---- 依据模式 ----

READ_BY_SIZE = {1: ("U8", "I8"), 2: ("U16", None), 4: ("U32", "I32"), 8: ("U64", None)}


def read_for(old_read, size):
    if old_read in ("DEREF_U32", "REL32", "NONE"):
        return old_read if size == 4 or old_read == "NONE" else None
    unsigned, signed = READ_BY_SIZE.get(size, (None, None))
    return signed if old_read in ("I8", "I32") else unsigned


def _original_span(ev, cs, kind):
    """原模式里要读的指令的下标和指令条数：(k, n)，解码不了时 None。"""
    try:
        _lead, pins = align.decode_pattern(ev.pattern, cs)
    except Exception:  # noqa: BLE001 - 原模式解码不了时不保范围
        return None
    if not pins:
        return None
    if kind:
        found = align.locate_in_pattern(ev.pattern, cs, ev.at)
        if found is None:
            return None
        return found[0], len(pins)
    return 0, len(pins)


def gen_evidence(image, cs, ev, origin, ranges, ins, kind, manifest_view, sig_results):
    """ins 是新 exe 里要读的指令，kind 是字段（disp / imm，read=NONE 时 None）。返回 (新的 Evidence, 说明或 None) 或
    (None, 原因)。说明非空时新模式没能保住原模式的范围（见文件开头）。"""
    span = align.field_span(ins, kind) if kind else None
    read = read_for(ev.read, span[1] if span else 0)
    if read is None:
        return None, f"字段宽度 {span[1] if span else 0} 和读取方式 {ev.read} 对不上"

    def count(text):
        regex, _ = compile_pattern(text)
        return sum(len(image.scan(regex, b, e)) for b, e in ranges)

    min_insns, min_fixed = (MIN_INSNS, MIN_FIXED) if ev.where == "TEXT" else (EV_MIN_INSNS, EV_MIN_FIXED)
    orig = _original_span(ev, cs, kind)
    if orig is not None and ev.where != "ENTRY":
        k, n = orig
        for b, e in ranges:
            if not b <= ins.address < e:
                continue
            full = list(cs.disasm(image.bytes_at(b, e - b), b))
            idx = next((x for x, y in enumerate(full) if y.address == ins.address), None)
            if idx is not None and idx - k >= 0:
                sub = full[idx - k:]
                start = sub[0].address
                wild = (k, span[0], span[1]) if span else None
                need = ins.address - start + (span[0] + span[1] if span else ins.size)
                text = _grow(sub, 0, need, max(n, min_insns), min_fixed, lambda t: count(t) == 1, wild, image=image)
                if text is not None:
                    at = (ins.address - start + span[0]) if span else 0
                    new_ev = dataclasses.replace(ev, pattern=text, at=at, read=read)
                    result = evidence_mod.evaluate(image, manifest_view, sig_results, new_ev)
                    if result.status == "ok":
                        return new_ev, None
            break
    lost = (f"保不住原模式的范围（原来要读的指令前 {orig[0]} 条、一共 {orig[1]} 条），退回从要读的指令起最短的唯一模式，"
            "原模式里的上下文（例如收紧时带上的同一串复制和界标）可能丢了，人工看一下") if orig is not None else None

    tries = []
    if ev.where == "ENTRY":
        tries.append(origin)
    else:
        tries.append(ins.address)
    text, begin = None, None
    for start in tries:
        insns = linear(image, cs, start)
        idx = next((n for n, x in enumerate(insns) if x.address == ins.address), None)
        if idx is None:
            continue
        wild = (idx, span[0], span[1]) if span else None
        need = ins.address - start + (span[0] + span[1] if span else ins.size)
        min_insns, min_fixed = (MIN_INSNS, MIN_FIXED) if ev.where == "TEXT" else (EV_MIN_INSNS, EV_MIN_FIXED)
        if ev.where == "ENTRY":
            accept = lambda t: True  # noqa: E731 - ENTRY 只在起点处匹配
            min_insns = min(min_insns, idx + 1)
        else:
            accept = lambda t: count(t) == 1  # noqa: E731
        text = _grow(insns, 0, need, min_insns, min_fixed, accept, wild, image=image)
        if text is not None:
            begin = start
            break
    if text is None and ev.where in ("FUNC", "WINDOW", "TEXT"):
        # 往前多取几条（和 suggest.build_pattern 一样）
        for b, e in ranges:
            if b <= ins.address < e:
                full = list(cs.disasm(image.bytes_at(b, e - b), b))
                idx = next((n for n, x in enumerate(full) if x.address == ins.address), None)
                for back in range(1, MAX_BACK + 1):
                    if idx is None or idx - back < 0:
                        break
                    start = full[idx - back].address
                    sub = full[idx - back:]
                    wild = (back, span[0], span[1]) if span else None
                    need = ins.address - start + (span[0] + span[1] if span else ins.size)
                    min_insns, min_fixed = (MIN_INSNS, MIN_FIXED) if ev.where == "TEXT" else (EV_MIN_INSNS, EV_MIN_FIXED)
                    text = _grow(sub, 0, need, min_insns, min_fixed, lambda t: count(t) == 1, wild, image=image)
                    if text:
                        begin = start
                        break
                break
    if text is None:
        return None, "在依据的范围里生成不了唯一的模式"
    at = (ins.address - begin + span[0]) if span else 0
    new_ev = dataclasses.replace(ev, pattern=text, at=at, read=read)
    result = evidence_mod.evaluate(image, manifest_view, sig_results, new_ev)
    if result.status != "ok":
        return None, f"生成的依据自检不过：{result.detail or result.status}"
    return new_ev, (lost if ev.where != "ENTRY" else None)

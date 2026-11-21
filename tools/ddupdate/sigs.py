"""特征码的解析和检查。

解析和 DDCore 的 game::Resolve 同义（GLOBAL_ANY 的多个候选只有 DD_RUNTIME_SELECT 的项允许，按内容选）。
检查是原 tools/check_sigs.py 的全部内容：唯一命中、GLOBAL_ANY 的规则、必需的 GLOBAL 指向 .data/.rdata、
HOOK 的偷取范围在指令边界且不含分支和 rip 相对、函数内没有分支跳进偷取范围、DD_ENTRY 和实际字节一致、
和 knowledge/dd1/hooks.md 清单不重叠（DD_CHAIN_TARGET 是有意串联，只记一笔）。U1d 加了一条：命中位置在异常目录记录的
函数里时，必须是函数入口（HOOK 装在函数中间时偷取范围本身可能看起来安全）。U1 修复 FB 加了一条：偷取字节数不小于 5
（放得下 jmp rel32；加载器是编译期断言，DDCore 的 hook 引擎装的时候拒绝，审核 A 备注 4）。

每条问题带一个类别（SigResult.kinds，和 problems 一一对应），C++/Python 对照测试（cpp_compare.py）和 accept 按它判断
运行时会不会自己发现这个失效（审核 A 的 S5、B2）：
  resolve  命中次数、解析出的地址、GLOBAL_ANY 的规则、特征码格式：DDCore 的 ResolveSignatures 同样判失效
  section  必需（REQUIRED、LOADER）的 GLOBAL 不在 .data/.rdata：DDCore 同样判失效
  entry    HOOK 的命中位置不是函数入口：DDCore（U1 修复 FA 起）和加载器同样判失效
  static   HOOK 偷取范围里的指令、偷取不在指令边界、分支跳进偷取范围、和 hooks.md 重叠、DD_ENTRY 不符或长度不对、
           偷取不到 5 字节、DD_ENTRY 用在 HOOK 之外：只有 verify 做，运行时不查
"""
import re
from dataclasses import dataclass, field

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from .image import compile_pattern
from .manifest import ROOT

HOOKS_MD = ROOT / "knowledge" / "dd1" / "hooks.md"
DEFAULT_PATCH_LEN = 16  # hooks.md 里没写字节数的内联 hook，按保守长度算
MIN_STEAL = 5           # jmp rel32 的长度

# 问题的类别（见文件开头）。RUNTIME_KINDS 是 DDCore 运行时自己也会判失效的，STATIC_KINDS 只有 verify 做
RUNTIME_KINDS = frozenset(("resolve", "section", "entry"))
STATIC_KINDS = frozenset(("static",))


def engine():
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    return cs


@dataclass
class SigResult:
    id: str
    kind: str
    need: str
    status: str = "ok"          # ok / fail
    hits: list = field(default_factory=list)
    hit: int | None = None       # 命中位置（GLOBAL_ANY 取最低的命中）
    resolved: int | None = None  # 解析结果：HOOK/CODE 是命中位置，GLOBAL 是全局地址，GLOBAL_ANY 是选中的候选
    candidates: list = field(default_factory=list)
    section: str | None = None
    problems: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    kinds: list = field(default_factory=list)   # 和 problems 一一对应的类别：resolve / section / entry / static

    def fail(self, text, kind="resolve"):
        self.status = "fail"
        self.problems.append(text)
        self.kinds.append(kind)

    @property
    def runtime_detectable(self):
        """失效的原因 DDCore 运行时也全都能发现（没有只在 verify 里做的静态检查）。"""
        return self.status != "ok" and all(k in RUNTIME_KINDS for k in self.kinds)

    @property
    def severity(self):
        return None if self.status == "ok" else ("required" if self.need == "REQUIRED" else "optional")


# ---- 运行时按内容选候选（DD_RUNTIME_SELECT），和 game.cpp 的 Resolve 一致 ----

def select_query_class_names(image, candidates):
    """Query 类名表：0x80 字节一项，全部以 Query 结尾的那张。返回 (地址, 项数) 或 None。"""
    for candidate in candidates:
        count, ok = 0, True
        for i in range(64):
            name = image.cstr(candidate + i * 0x80, 0x80)
            if name is None or len(name) == 0:
                break
            if len(name) < 5 or not name.endswith(b"Query"):
                ok = False
                break
            count += 1
        if ok and count > 0:
            return candidate, count
    return None


RUNTIME_SELECTORS = {"QueryClassNames": select_query_class_names}


def resolve(image, sig):
    res = SigResult(sig.id, sig.kind, sig.need)
    try:
        regex, _ = compile_pattern(sig.pattern)
    except ValueError as e:
        res.fail(f"特征码格式错误：{e}")
        return res
    res.hits = image.scan_text(regex)
    if sig.kind == "GLOBAL_ANY":
        if not res.hits:
            res.fail("0 次命中")
            return res
        targets = []
        for h in res.hits:
            t = h + sig.capture + 4 + image.i32(h + sig.capture)
            if t not in targets:
                targets.append(t)
        bad = [t for t in targets if not 0 <= t < image.size]
        if bad:
            res.fail(f"解析出的地址超出映像 {[hex(b) for b in bad[:4]]}")
            return res
        res.hit = min(res.hits)
        res.candidates = targets
        if len(targets) > 1 and not sig.runtime_select:
            res.fail(f"{len(res.hits)} 次命中解析出 {len(targets)} 个不同地址（需要 DD_RUNTIME_SELECT 或收紧特征码）")
            return res
        if len(targets) > 1:
            # 类名表这类内容由静态初始化在运行时填写，文件里通常是空的，离线选不出来时和原 check_sigs.py 一样接受多个候选
            selector = RUNTIME_SELECTORS.get(sig.id)
            chosen = selector(image, targets) if selector else None
            res.resolved = chosen[0] if chosen else targets[0]
            if chosen:
                res.notes.append(f"{len(res.hits)} 次命中，{len(targets)} 个候选，按内容选中 {chosen[0]:X}（{chosen[1]} 项）")
            else:
                res.notes.append(f"{len(res.hits)} 次命中，{len(targets)} 个候选（运行时按内容选，文件里的表是空的）")
        else:
            res.resolved = targets[0]
            res.notes.append(f"{len(res.hits)} 次命中，解析出 1 个地址")
        res.section = image.section_of(res.resolved)
        return res
    if len(res.hits) != 1:
        res.fail(f"{len(res.hits)} 次命中 {[hex(h) for h in res.hits[:4]]}，要求唯一")
        return res
    res.hit = res.hits[0]
    if sig.kind == "GLOBAL":
        at = res.hit + sig.capture
        target = at + 4 + image.i32(at)
        if not 0 <= target < image.size:
            res.fail(f"全局地址 {target:X} 超出映像")
            return res
        res.resolved = target
        res.section = image.section_of(target)
        # 必需和加载器的 GLOBAL（DDCore 的 ResolveSignatures 对 Required || Loader 同样检查，审核 A 备注 2）；
        # Sig.need 也把 LOADER 归为 REQUIRED，这里按 required 写明，免得读成只查 REQUIRED
        if sig.required in ("REQUIRED", "LOADER") and res.section not in (".data", ".rdata"):
            res.fail(f"解析出的全局 {target:X} 不在 .data/.rdata（{res.section}）", "section")
    else:
        res.resolved = res.hit
        res.section = image.section_of(res.hit)
    return res


# ---- HOOK 的安全检查 ----

def branch_targets(image, cs, rva):
    """包含 rva 的主函数所有分块里的分支、调用目标。"""
    chunks = image.function_ranges(rva) or []
    targets = set()
    for begin, end in chunks:
        for ins in cs.disasm(image.bytes_at(begin, end - begin), begin):
            if ins.mnemonic.startswith("j") or ins.mnemonic == "call":
                for op in ins.operands:
                    if op.type == X86_OP_IMM:
                        targets.add(op.imm)
    return targets, len(chunks)


def foreign_patches():
    """hooks.md 里 DDCore/加载器之外的区间（rva, 长度, 所属），只认带反引号的 RVA 表格行。
    「DDCore 与加载器」和「验证用 hook」两节排除，那里是我们自己的目标。"""
    text = HOOKS_MD.read_text(encoding="utf-8")
    sections = re.split(r"^## ", text, flags=re.M)
    kept = [sections[0]]
    for section in sections[1:]:
        head = section.split("\n", 1)[0]
        if head.startswith("DDCore") or head.startswith("验证用"):
            continue
        kept.append("## " + section)
    body = "".join(kept)
    spans = []
    for row in re.findall(r"^\|.*$", body, re.M):
        cells = [c.strip() for c in row.strip("|").split("|")]
        rvas = re.findall(r"`([0-9A-F]{5,7})`", cells[0]) if cells else []
        if not rvas:
            continue
        length = DEFAULT_PATCH_LEN
        owner = cells[-1] if cells else ""
        for cell in cells[1:]:
            if re.fullmatch(r"\d+", cell):
                length = int(cell)
                break
        spans += [(int(rva, 16), length, owner) for rva in rvas]
    return spans


def check_hook(image, cs, sig, res, foreign, label):
    """在 res 上追加 HOOK 的检查结论。foreign 是 hooks.md 的区间（RVA 是 Steam 版的，只在 Steam 版上比较）。"""
    if res.status != "ok" or sig.kind != "HOOK":
        return
    rva, steal = res.hit, sig.steal
    pos = 0
    for ins in cs.disasm(image.bytes_at(rva, 64), rva):
        if pos >= steal:
            break
        uses_rip = any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands)
        if uses_rip or ins.mnemonic in ("call", "jmp", "ret") or ins.mnemonic.startswith("j"):
            res.fail(f"偷取范围里有不安全指令 {ins.mnemonic} {ins.op_str}", "static")
        pos += ins.size
    if pos != steal:
        res.fail(f"偷取 {steal} 字节没有落在指令边界（下一边界 {pos}）", "static")
    if steal < MIN_STEAL:
        res.fail(f"偷取 {steal} 字节，小于 {MIN_STEAL}，放不下 jmp rel32", "static")
    # U1d：HOOK 要装在函数入口。编译器在原来的序言前面插了指令（栈探测、提前返回的判断）时，带通配的特征码可能命中
    # 挪下去的旧序言，偷取范围本身看起来安全，hook 却装在了函数中间
    begin = image.entry_of(rva)
    if begin is not None and not image.is_function_start(rva):
        primary = image.enclosing_function(rva)
        res.fail(f"命中位置不是函数入口（在 {primary:X} 开始的函数里，+{rva - primary:#x}）", "entry")
    targets, chunks = branch_targets(image, cs, rva)
    if chunks == 0:
        res.notes.append("入口不在异常目录记录的函数里，没有检查跳进偷取范围的分支")
    inside = sorted(t for t in targets if rva < t < rva + steal)
    if inside:
        res.fail(f"函数内有分支跳进偷取范围 {[hex(t) for t in inside]}", "static")
    res.notes.append(f"steal={steal} chunks={chunks}")
    if label == "steam":
        overlap = [f"{start:X}+{length}（{owner}）" for start, length, owner in foreign
                   if start < rva + steal and rva < start + length]
        if overlap and sig.chain:
            res.notes.append(f"有意串联（DD_CHAIN_TARGET），重叠 {overlap}")
        elif overlap:
            res.fail(f"和 hooks.md 清单重叠 {overlap}", "static")
    if sig.entry is not None:
        want = bytes(int(t, 16) for t in sig.entry.split())
        got = image.bytes_at(rva, len(want))
        if want != got:
            res.fail(f"入口字节 {got.hex()} 和 DD_ENTRY {want.hex()} 不符", "static")
        if len(want) != steal:
            res.fail(f"DD_ENTRY 长度 {len(want)} 不等于 steal {steal}", "static")


def resolve_all(image, manifest, label, foreign=None, cs=None):
    cs = cs or engine()
    foreign = foreign_patches() if foreign is None else foreign
    out = {}
    for sig in manifest.sigs.values():
        res = resolve(image, sig)
        check_hook(image, cs, sig, res, foreign, label)
        if sig.entry is not None and sig.kind != "HOOK":
            res.fail("DD_ENTRY 只用于 HOOK", "static")
        out[sig.id] = res
    return out

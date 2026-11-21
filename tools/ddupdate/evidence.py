"""可在运行时求值的依据（DD_EVIDENCE）的求值。语义的权威说明在 layout.inc 开头，DDCore 的运行时核对（U1b）
照同一份语义实现，两边对同一个映像必须给出相同的结论。

求值步骤：
  1. 起点：NOSIG 没有起点；其余要求 sig 解析成功（否则这条依据 blocked）。HIT 是命中位置（GLOBAL_ANY 取最低的命中），
     TARGET 是解析结果，HIT_VALUE 是命中位置 + 清单项的值 + arg（arg 可以为负）；VSLOT / VSLOT_VALUE 把解析结果当虚表，取第 arg 槽（VSLOT_VALUE 用清单项自己的值当槽号）的
     8 字节指针，减映像基址得到 RVA，必须落在 .text。
  2. 范围：ENTRY 只看起点处；WINDOW 是 [起点, 起点+span)；FUNC 是起点所在函数的全部分块（异常目录，见 image.py），
     起点不在任何表项里时失败；TEXT 是整个 .text。模式必须完整落在某一个范围里，可以重叠匹配，总共恰好命中一次。
  3. 读取：命中位置 +at 处按 read 读（小端）；DEREF_U32 是把 +at 处的 int32 当 rip 相对位移（位移是指令的最后一个字段），
     读目标处的 uint32；REL32 是同样算出的目标 RVA 本身（call/jmp rel32 的目标）。read=NONE 时只要求唯一命中。
  4. 比较：期望值 = 常量 * mul + rel 的值 + add。对象是特征码名字时常量按 0 算；rel 是 NONE 时为 0，是清单项时是它的常量，
     是特征码名字时是它的解析结果的 RVA（那条特征码失效时这条依据 blocked）。
     无符号读取零扩展、有符号读取符号扩展成 64 位整数后和期望值直接比较，不截断（期望值超出读取宽度时必然不成立，
     清单检查会报出来）。
"""
from dataclasses import dataclass, field

from .image import compile_pattern

WIDTH = {"NONE": 0, "U8": 1, "I8": 1, "U16": 2, "U32": 4, "I32": 4, "U64": 8, "DEREF_U32": 4, "REL32": 4}
SIGNED = {"I8", "I32"}


@dataclass
class EvidenceResult:
    status: str                     # ok / fail / blocked（依赖的特征码失效）
    origin: int | None = None
    hits: list = field(default_factory=list)
    hit: int | None = None
    captured: int | None = None
    expected: int | None = None
    implied: int | None = None      # 按捕获值反推的常量（mul 非 0 时），修复工具用
    detail: str = ""


def read_value(image, rva, read):
    """读不满宽度（超出映像）时返回 None，和 DDCore 的 layout_check.cpp 一样按「超出映像」判不成立（审核 A 备注 3：
    原来读到映像末尾时按读到的字节算值）。"""
    width = WIDTH[read]
    if rva < 0 or rva + width > image.size:
        return None
    if read in ("DEREF_U32", "REL32"):
        target = rva + 4 + image.i32(rva)
        if not 0 <= target <= image.size - 4:
            return None
        return target if read == "REL32" else image.u32(target)
    raw = int.from_bytes(image.bytes_at(rva, width), "little")
    if read in SIGNED and raw >= 1 << (8 * width - 1):
        raw -= 1 << (8 * width)
    return raw


def origin_of(image, manifest, sig_results, ev):
    """返回 (起点 RVA, 错误说明)。NOSIG 返回 (None, None)。"""
    if ev.origin == "NOSIG":
        return None, None
    res = sig_results.get(ev.sig)
    if res is None or res.status != "ok":
        return None, f"特征码 {ev.sig} 失效"
    if ev.origin == "HIT":
        return res.hit, None
    if ev.origin == "TARGET":
        return res.resolved, None
    if ev.origin == "HIT_VALUE":
        return res.hit + manifest.value(ev.item) + ev.arg, None
    slot = ev.arg if ev.origin == "VSLOT" else manifest.value(ev.item)
    table = res.resolved
    if not 0 <= table + slot * 8 <= image.size - 8:
        return None, f"虚表槽 {slot} 超出映像"
    rva = image.u64(table + slot * 8) - image.base
    if not image.in_text(rva):
        return None, f"虚表槽 {slot} 的指针 {rva + image.base:#x} 不在 .text"
    return rva, None


def ranges_of(image, ev, origin, length):
    if ev.where == "TEXT":
        return [(image.text_start, image.text_end)], None
    if ev.where == "ENTRY":
        return [(origin, origin + length)], None
    if ev.where == "WINDOW":
        return [(origin, origin + ev.span)], None
    chunks = image.function_ranges(origin)
    if chunks is None:
        return None, f"起点 {origin:X} 不在异常目录记录的函数里"
    return chunks, None


def rel_value(manifest, sig_results, name):
    """rel 的值：特征码名字是解析结果的 RVA（失效时 None），其余同 manifest.value。"""
    if name in manifest.sigs:
        res = sig_results.get(name)
        return res.resolved if res is not None and res.status == "ok" else None
    return manifest.value(name)


def expected_value(manifest, sig_results, ev):
    rel = rel_value(manifest, sig_results, ev.rel)
    return None if rel is None else manifest.value(ev.item) * ev.mul + rel + ev.add


def evaluate(image, manifest, sig_results, ev):
    origin, error = origin_of(image, manifest, sig_results, ev)
    if error:
        return EvidenceResult("blocked" if "失效" in error else "fail", detail=error)
    regex, length = compile_pattern(ev.pattern)
    ranges, error = ranges_of(image, ev, origin, length)
    if error:
        return EvidenceResult("fail", origin=origin, detail=error)
    hits = []
    for begin, end in ranges:
        hits += image.scan(regex, begin, end)
    result = EvidenceResult("fail", origin=origin, hits=hits)
    if len(hits) != 1:
        result.detail = f"命中 {len(hits)} 次，要求唯一" + (f" {[hex(h) for h in hits[:4]]}" if hits else "")
        return result
    result.hit = hits[0]
    if ev.read == "NONE":
        result.status = "ok"
        return result
    captured = read_value(image, result.hit + ev.at, ev.read)
    if captured is None:
        result.detail = "rip 相对的目标超出映像"
        return result
    result.captured = captured
    expected = expected_value(manifest, sig_results, ev)
    if expected is None:
        result.status = "blocked"
        result.detail = f"特征码 {ev.rel} 失效"
        return result
    result.expected = expected
    same = captured == expected
    rest = captured - rel_value(manifest, sig_results, ev.rel) - ev.add
    if ev.mul and rest % ev.mul == 0:
        result.implied = rest // ev.mul
    result.status = "ok" if same else "fail"
    if not same:
        result.detail = f"读到 {captured:#x}，期望 {expected:#x}"
        if result.implied is not None and ev.item in manifest.items:
            result.detail += f"（按读到的值，常量应为 {result.implied:#x}）"
    return result

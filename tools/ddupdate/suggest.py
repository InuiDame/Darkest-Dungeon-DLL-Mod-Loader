"""给清单项生成运行时依据的候选（suggest 命令，写清单时用；修复工具 U1c 的「重新生成依据」可以复用 build_pattern）。

在起点所在的范围里找位移或立即数等于期望值的指令，从这条指令开始取字节（命中的字段、rip 相对位移、分支的 rel 写成 ??），
逐条加后面的指令，直到在范围里唯一；还不唯一时往前多取几条。生成的依据在给出的每个 exe 上都求值一遍，全部通过的才输出。
"""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from . import evidence as evidence_mod
from . import manifest as manifest_mod
from . import sigs as sigs_mod
from .fingerprint import _BRANCH, engine
from .image import Image, compile_pattern
from .manifest import Evidence


def masked_bytes(ins, keep_field=None):
    """指令字节，rip 相对位移和分支 rel 写成 None；keep_field=(offset, size) 也写成 None（要读的字段）。"""
    out = list(ins.bytes)
    if ins.disp_size and any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands):
        for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
            out[k] = None
    if _BRANCH.match(ins.mnemonic) and ins.imm_size and any(op.type == X86_OP_IMM for op in ins.operands):
        for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
            out[k] = None
    if keep_field:
        for k in range(keep_field[0], keep_field[0] + keep_field[1]):
            out[k] = None
    return out


def to_text(values):
    return " ".join("??" if v is None else f"{v:02X}" for v in values)


def count_in(image, ranges, text):
    regex, _ = compile_pattern(text)
    return sum(len(image.scan(regex, b, e)) for b, e in ranges)


def build_pattern(image, insns, index, field, ranges, max_len=48, max_back=3):
    """从 insns[index] 开始（必要时往前 max_back 条）生成在 ranges 里唯一的模式，返回 (模式, 字段在模式里的偏移) 或 None。"""
    for back in range(0, max_back + 1):
        start = index - back
        if start < 0:
            break
        values = []
        at = None
        for k in range(start, len(insns)):
            ins = insns[k]
            if k > start and ins.address != insns[k - 1].address + insns[k - 1].size:
                break
            if k == index:
                at = len(values) + field[0]
                values += masked_bytes(ins, field)
            else:
                values += masked_bytes(ins)
            if k >= index and len(values) > max_len:
                break
            if k >= index and count_in(image, ranges, to_text(values)) == 1:
                while len(values) > at + field[1] and values[-1] is None:
                    values.pop()
                return to_text(values), at
    return None


def _field_match(raw_bytes, expected):
    """字段的无符号值等于期望值时返回无符号读取，有符号值等于时返回有符号读取，否则 None。"""
    size = len(raw_bytes)
    unsigned = int.from_bytes(raw_bytes, "little")
    signed = int.from_bytes(raw_bytes, "little", signed=True)
    if unsigned == expected:
        return {1: "U8", 2: "U16", 4: "U32", 8: "U64"}[size]
    if signed == expected and size in (1, 4):
        return {1: "I8", 4: "I32"}[size]
    return None


def fields_equal(ins, expected):
    """指令里等于期望值的位移、立即数字段 [(offset, size, 读取方式)]。"""
    out = []
    if ins.disp_size and not any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands):
        read = _field_match(ins.bytes[ins.disp_offset:ins.disp_offset + ins.disp_size], expected)
        if read:
            out.append((ins.disp_offset, ins.disp_size, read))
    if ins.imm_size and not _BRANCH.match(ins.mnemonic):
        read = _field_match(ins.bytes[ins.imm_offset:ins.imm_offset + ins.imm_size], expected)
        if read:
            out.append((ins.imm_offset, ins.imm_size, read))
    return out


def run(args, out):
    manifest = manifest_mod.load()
    from .verify import DEFAULT_EXES
    paths = args.exe or [p for _t, p in DEFAULT_EXES]
    images = [Image(p) for p in paths]
    results = [sigs_mod.resolve_all(img, manifest, manifest.known_builds.get(img.sha1), []) for img in images]
    item = args.item
    value = int(args.value, 0) if args.value else manifest.value(item)
    rel_value = manifest.value(args.rel) if args.rel != "NONE" else 0
    expected = value * args.mul + rel_value + args.add
    image, sig_results = images[0], results[0]
    probe = Evidence(item, args.sig, args.origin, args.slot, args.where, args.span, "90", 0, "NONE", 0, "NONE", 0, 0)
    origin, error = evidence_mod.origin_of(image, manifest, sig_results, probe)
    if error:
        out.write(f"起点：{error}\n")
        return 1
    ranges, error = evidence_mod.ranges_of(image, probe, origin, 64)
    if error:
        out.write(f"范围：{error}\n")
        return 1
    cs = engine()
    insns = []
    for b, e in ranges:
        insns += list(cs.disasm(image.bytes_at(b, e - b), b))
    shown = 0
    for i, ins in enumerate(insns):
        for offset, size, read in fields_equal(ins, expected):
            built = build_pattern(image, insns, i, (offset, size), ranges)
            if not built:
                out.write(f"  {ins.address:X} {ins.mnemonic} {ins.op_str}：找不到唯一的模式\n")
                continue
            pattern, at = built
            ev = Evidence(item, args.sig, args.origin, args.slot, args.where, args.span, pattern, at, read, args.mul, args.rel, args.add, 0)
            if args.value:
                ok = True
            else:
                ok = all(evidence_mod.evaluate(img, manifest, res, ev).status == "ok" for img, res in zip(images, results))
            mark = "OK " if ok else "-- "
            out.write(f"{mark}{ins.address:X} {ins.mnemonic} {ins.op_str}\n")
            if ok:
                out.write(f"DD_EVIDENCE({item}, {args.sig}, {args.origin}, {args.slot}, {args.where}, {args.span}, \"{pattern}\", {at}, {read}, "
                          f"{args.mul}, {args.rel}, {args.add})\n")
                shown += 1
            if shown >= args.limit:
                return 0
    if not shown:
        out.write(f"在 {len(insns)} 条指令里没有找到等于 {expected:#x} 的字段\n")
    return 0


def add_parser(sub):
    p = sub.add_parser("suggest", help="给清单项生成运行时依据的候选（写清单时用）")
    p.add_argument("item", help="清单项（或特征码）名字")
    p.add_argument("--sig", required=True)
    p.add_argument("--origin", default="TARGET", choices=("HIT", "TARGET", "VSLOT", "VSLOT_VALUE"))
    p.add_argument("--slot", type=int, default=0)
    p.add_argument("--where", default="FUNC", choices=("FUNC", "WINDOW", "ENTRY"))
    p.add_argument("--span", type=int, default=0)
    p.add_argument("--value", help="不用清单里的值（清单里还没有这一项时）")
    p.add_argument("--rel", default="NONE")
    p.add_argument("--mul", type=int, default=1)
    p.add_argument("--add", type=lambda s: int(s, 0), default=0)
    p.add_argument("--limit", type=int, default=3)
    p.add_argument("--exe", action="append")
    return p

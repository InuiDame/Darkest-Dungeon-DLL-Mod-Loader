"""函数指纹、相似度和调用关系。基线（baseline.py）、同一性检查（verify.py）和修复（U1c）共用。

指纹的内容：
  chunks    函数的分块（异常目录，见 image.py）；没有表项的叶函数从入口线性解码到第一个 ret/jmp/int3（最多 0x1000 字节）
  tokens    规范化的指令序列：助记符 + 操作数的种类和宽度（寄存器 r<位宽>、内存 m<位宽>、rip 相对内存 mr<位宽>、
            立即数 i、分支目标 L），不含寄存器编号、位移和立即数的值，编译器换寄存器、结构加字段时序列不变
  callees   直接调用的目标（RVA）；imports 间接调用的导入函数名
  strings   rip 相对引用的 C 字符串（可打印、至少 4 字节，截到 80 字节）
  globals   rip 相对引用的 .data/.rdata 地址（含字符串）
  imms      有区分度的立即数和内存位移（绝对值不小于 0x100，排除指向映像的值）
  thunk     函数只是一条 jmp 时，跳转目标的指纹

相似度：tokens 的 3-gram 多重集的 Dice 系数 2|A∩B| / (|A|+|B|)，不足 3 条指令时按整个序列比较。
阈值见 SIMILARITY_THRESHOLD，依据是 simstats 命令的数据（docs/u1/u1a-report.md）。
"""
import re
import struct
from collections import Counter

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RIP

# 同一函数在 Steam 版和离线版之间的相似度最低 0.95x，不同函数里最像的（同一个 exe 里的「最近冒充者」）中位数 0.6x、
# 少数模板实例和 thunk 能到 1.0（这种函数靠调用关系和依据区分），取 0.85，数据见 U1a 报告的「相似度」一节
SIMILARITY_THRESHOLD = 0.85
NGRAM = 3
LINEAR_LIMIT = 0x1000

_BRANCH = re.compile(r"^(j|call|loop)")


def engine():
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    return cs


def token(ins):
    branch = bool(_BRANCH.match(ins.mnemonic))
    ops = []
    for op in ins.operands:
        if op.type == X86_OP_REG:
            ops.append(f"r{op.size * 8}")
        elif op.type == X86_OP_IMM:
            ops.append("L" if branch else "i")
        elif op.type == X86_OP_MEM:
            ops.append(("mr" if op.mem.base == X86_REG_RIP else "m") + str(op.size * 8))
        else:
            ops.append("?")
    return f"{ins.mnemonic} {','.join(ops)}" if ops else ins.mnemonic


def decode_function(image, rva, cs):
    """返回 (chunks, 指令列表, 是否有异常目录表项)。rva 在某个函数里时取整个函数，否则从 rva 线性解码。"""
    chunks = image.function_ranges(rva)
    if chunks:
        insns = []
        for begin, end in chunks:
            insns += list(cs.disasm(image.bytes_at(begin, end - begin), begin))
        return chunks, insns, True
    insns = []
    for ins in cs.disasm(image.bytes_at(rva, LINEAR_LIMIT), rva):
        insns.append(ins)
        if ins.mnemonic in ("ret", "int3", "jmp", "ud2"):
            break
    end = insns[-1].address + insns[-1].size if insns else rva
    return [(rva, end)], insns, False


class Imports:
    """IAT 地址到导入函数名。"""

    def __init__(self, image):
        self.by_rva = {}
        pe = image.pe
        try:
            pe.parse_data_directories(directories=[1])  # IMAGE_DIRECTORY_ENTRY_IMPORT
        except Exception:  # noqa: BLE001 - 导入表坏了也不影响指纹的其他部分
            return
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode(errors="replace").lower()
            for imp in entry.imports:
                name = imp.name.decode(errors="replace") if imp.name else f"#{imp.ordinal}"
                self.by_rva[imp.address - image.base] = f"{dll}!{name}"


def _printable(text):
    return text and len(text) >= 4 and all(32 <= c < 127 or c in (9, 10, 13) for c in text)


def fingerprint(image, rva, cs, imports=None, follow_thunk=True, coderefs=None):
    """coderefs 是列表时，顺便收集 rip 相对引用的 .text 地址（函数指针，例如 lea r9,[函数]），修复工具的函数索引用；
    不影响返回的指纹。"""
    chunks, insns, pdata = decode_function(image, rva, cs)
    tokens, callees, imported, strings, globals_, imms = [], set(), set(), set(), set(), set()
    lo, hi = image.base, image.base + image.size
    for ins in insns:
        tokens.append(token(ins))
        branch = bool(_BRANCH.match(ins.mnemonic))
        for op in ins.operands:
            if op.type == X86_OP_IMM:
                if branch:
                    if ins.mnemonic == "call":
                        callees.add(op.imm)
                elif abs(op.imm) >= 0x100 and not lo <= op.imm < hi:
                    imms.add(op.imm & 0xFFFFFFFFFFFFFFFF)
            elif op.type == X86_OP_MEM:
                if op.mem.base == X86_REG_RIP:
                    target = ins.address + ins.size + op.mem.disp
                    if imports and target in imports.by_rva:
                        imported.add(imports.by_rva[target])
                        continue
                    section = image.section_of(target)
                    if section in (".rdata", ".data"):
                        globals_.add(target)
                        text = image.cstr(target, 120)
                        if _printable(text):
                            strings.add(text[:80].decode(errors="replace"))
                    elif coderefs is not None and image.in_text(target):
                        coderefs.append(target)
                elif abs(op.mem.disp) >= 0x100:
                    imms.add(op.mem.disp & 0xFFFFFFFFFFFFFFFF)
    fp = {
        "rva": image.enclosing_function(rva) if pdata else rva,
        "chunks": [list(c) for c in chunks],
        "pdata": pdata,
        "size": sum(e - b for b, e in chunks),
        "tokens": tokens,
        "callees": sorted(callees),
        "imports": sorted(imported),
        "strings": sorted(strings),
        "globals": sorted(globals_),
        "imms": sorted(imms),
        "thunk": None,
    }
    if follow_thunk and len(insns) <= 2 and insns and insns[-1].mnemonic == "jmp" and insns[-1].operands \
            and insns[-1].operands[0].type == X86_OP_IMM:
        target = insns[-1].operands[0].imm
        if image.in_text(target):
            fp["thunk"] = fingerprint(image, target, cs, imports, follow_thunk=False)
    return fp


def insn_refs(image, insns, imports=None):
    """每条指令引用的东西，按指令下标：[[下标, 种类, 值], ...]。种类：s 字符串（文本，规则同 strings）、i 导入函数名、
    c 直接调用的目标 RVA、g .data/.rdata 里不是字符串的全局 RVA、f rip 相对引用的 .text 地址（函数指针）。基线（format 2）存它，修复工具做指令对齐时把它并进
    token：字符串和导入名在版本之间不变，调用目标和全局按已经确认的新旧对应关系换算。不含位移和立即数的值（结构变化时
    正是它们在变）。"""
    out = []
    for i, ins in enumerate(insns):
        if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM:
            out.append([i, "c", ins.operands[0].imm])
            continue
        for op in ins.operands:
            if op.type != X86_OP_MEM or op.mem.base != X86_REG_RIP:
                continue
            target = ins.address + ins.size + op.mem.disp
            if imports and target in imports.by_rva:
                out.append([i, "i", imports.by_rva[target]])
            elif image.section_of(target) in (".rdata", ".data"):
                text = image.cstr(target, 120)
                if _printable(text):
                    out.append([i, "s", text[:80].decode(errors="replace")])
                else:
                    out.append([i, "g", target])
            elif image.in_text(target):
                out.append([i, "f", target])
    return out


def shingles(tokens, n=NGRAM):
    if len(tokens) < n:
        return Counter([tuple(tokens)])
    return Counter(tuple(tokens[i:i + n]) for i in range(len(tokens) - n + 1))


def similarity(a, b):
    """两个 token 序列（或已经算好的 shingles Counter）的相似度，0..1。"""
    sa = a if isinstance(a, Counter) else shingles(a)
    sb = b if isinstance(b, Counter) else shingles(b)
    total = sum(sa.values()) + sum(sb.values())
    if total == 0:
        return 1.0
    return 2 * sum((sa & sb).values()) / total


def fp_similarity(a, b):
    """两个指纹的相似度：thunk 比较跳转目标。"""
    score = similarity(a["tokens"], b["tokens"])
    if a.get("thunk") and b.get("thunk"):
        score = min(score, similarity(a["thunk"]["tokens"], b["thunk"]["tokens"]))
    elif bool(a.get("thunk")) != bool(b.get("thunk")):
        score = min(score, 0.0)
    return score


def jaccard(a, b):
    a, b = set(a), set(b)
    if not a and not b:
        return 1.0
    return len(a & b) / len(a | b)


class CallIndex:
    """整个 .text 里 E8 rel32 调用的索引：目标 -> 调用点。目标必须是函数入口（异常目录的主函数起点），
    或者落在异常目录空隙里、对齐或前面是填充字节的叶函数（和 knowledge/tools/dd_disasm.py 一致）。"""

    def __init__(self, image):
        self.image = image
        start, end = image.text_start, image.text_end
        data = image.bytes_at(start, end - start)
        self.calls = {}
        for m in re.finditer(rb"\xe8", data):
            site = start + m.start()
            if site + 5 > end:
                continue
            target = site + 5 + struct.unpack_from("<i", data, m.start() + 1)[0]
            if image.is_function_start(target) or self._gap_target(target):
                self.calls.setdefault(target, []).append(site)

    def _gap_target(self, target):
        image = self.image
        if not image.in_text(target) or image.enclosing_function(target) is not None:
            return False
        return target & 0xF == 0 or image.mem[target - 1] in (0xCC, 0xC3, 0x90)

    def callers(self, rva, cs):
        """真正调用 rva 的函数（解码调用者确认调用点在指令边界上），返回排序的函数起点列表。"""
        image = self.image
        found = set()
        for site in self.calls.get(rva, []):
            func = image.enclosing_function(site)
            if func is None:
                continue
            for begin, end in image.function_ranges(site):
                if begin <= site < end:
                    for ins in cs.disasm(image.bytes_at(begin, end - begin), begin):
                        if ins.address == site:
                            if ins.mnemonic == "call":
                                found.add(func)
                            break
                        if ins.address > site:
                            break
        return sorted(found)

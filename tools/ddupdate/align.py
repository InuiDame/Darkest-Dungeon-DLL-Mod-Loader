"""指令级的工具：解码特征码 / 依据模式、放宽模式、基线函数和新函数之间的指令对齐。

对齐（Aligner.map）以全局序列对齐为准，给不出唯一结果时返回 None（依据位置核对记为「没法对照」，修复工具按
「需要人工」处理）：
  1. 全局对齐：difflib.SequenceMatcher（不用 autojunk）对两个 token 序列求匹配块，i 落在某个匹配块里时给出对应下标。
  2. 插入、删除的滑动（U1 修复 FB，审核 A 的 B1）：匹配块之间的空隙是插入或删除。空隙两边是重复的 token 时（一串形状
     相同的字段复制里多了或少了一对），同样好的对齐不止一种：空隙可以往左、往右滑，滑过的旧指令在不同的对齐里对应到
     不同的新位置。这些旧指令判为「对齐有歧义」，不给结果。这是真正的序列对齐的结论，不靠某一种启发式恰好选中哪边。
  3. 唯一窗口：从旧指令 i 周围取最短的连续 token 窗口（长度 1..MAX_WINDOW，i 在窗口里的任意位置），要求它在旧函数和
     新函数里都恰好出现一次，新函数里出现的位置加上 i 在窗口里的偏移就是对应的下标。找到了就必须和 1 一致，不一致返回
     None；找不到时（i 周围是长段的重复代码）1 的匹配块不短于 MIN_BLOCK_ALONE 条、又没有 2 的歧义，才用 1 的结果
     （U1 修复 FB 之前这种情况一律给不出结果）。
token 是 fingerprint.token 的规范化指令，再并上 refs（基线 format 2）：字符串、导入函数名原样并入，调用目标和全局
只有在新旧对应关系已经确认时才换算成新地址并入（见 enriched）。不含位移和立即数的值，结构变化时它们正在变。
"""
import difflib

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from .fingerprint import _BRANCH, insn_refs, token

MAX_WINDOW = 24
MIN_BLOCK_ALONE = 8


# ---- 模式 ----

def _pattern_values(pattern):
    return [None if t == "??" else int(t, 16) for t in pattern.split()]


def decode_pattern(pattern, cs, entry=None):
    """解码模式里完整的指令，返回 (开头连续通配的字节数, [指令])，指令的 address 是在模式里的偏移。
    通配字节按 0 解码（位移、立即数、rel 的值不影响指令长度）；HOOK 开头的偷取字节有 DD_ENTRY 时用它的真实字节，
    没有时从开头连续通配之后开始解码（这段通配按约定落在指令边界上）。"""
    values = _pattern_values(pattern)
    lead = 0
    while lead < len(values) and values[lead] is None:
        lead += 1
    entry_bytes = [int(t, 16) for t in entry.split()] if entry else []
    data = bytearray()
    for i, v in enumerate(values):
        if v is None:
            v = entry_bytes[i] if i < len(entry_bytes) else 0
        data.append(v)
    start = 0 if (entry_bytes or lead == 0) else lead
    insns = []
    for ins in cs.disasm(bytes(data[start:]), start):
        if ins.address + ins.size > len(data):
            break
        insns.append(ins)
    return (0 if entry_bytes else lead), insns


def field_of(ins, offset):
    """offset（指令内）处的字段：disp（内存位移）、imm（立即数或分支 rel），都不是时 None。"""
    if ins.disp_size and ins.disp_offset == offset:
        return "disp"
    if ins.imm_size and ins.imm_offset == offset:
        return "imm"
    return None


def field_span(ins, kind):
    if kind == "disp" and ins.disp_size:
        return ins.disp_offset, ins.disp_size
    if kind == "imm" and ins.imm_size:
        return ins.imm_offset, ins.imm_size
    return None


def is_rip(ins):
    return any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands)


def is_branch(ins):
    return bool(_BRANCH.match(ins.mnemonic)) and any(op.type == X86_OP_IMM for op in ins.operands)


def relax_pattern(pattern, cs, entry=None):
    """把模式里每条完整指令的位移、立即数、rel 都写成 ??（操作码、ModRM、SIB、前缀保留），返回 (放宽后的模式, 指令条数)。
    开头的偷取字节保持通配，解码不了的尾部原样保留。"""
    values = _pattern_values(pattern)
    lead, insns = decode_pattern(pattern, cs, entry)
    out = list(values)
    for ins in insns:
        for kind in ("disp", "imm"):
            span = field_span(ins, kind)
            if span:
                for k in range(ins.address + span[0], ins.address + span[0] + span[1]):
                    out[k] = None
    return " ".join("??" if v is None else f"{v:02X}" for v in out), len(insns)


def pattern_tokens(pattern, cs, entry=None):
    return [token(ins) for ins in decode_pattern(pattern, cs, entry)[1]]


def locate_in_pattern(pattern, cs, offset, entry=None):
    """模式里 offset 处属于第几条指令、哪种字段：(指令序号, 指令, 字段, 字段在指令里的偏移) 或 None。"""
    _lead, insns = decode_pattern(pattern, cs, entry)
    for k, ins in enumerate(insns):
        if ins.address <= offset < ins.address + ins.size:
            return k, ins, field_of(ins, offset - ins.address), offset - ins.address
    return None


def unique_positions(seq, sub):
    """sub 在 seq 里出现的全部位置（列表比较，给基线没有 anchor 时用）。"""
    n = len(sub)
    return [p for p in range(len(seq) - n + 1) if seq[p:p + n] == sub]


# ---- 对齐 ----

def enriched(tokens, refs, translate=None):
    """token 序列并上 refs。translate(kind, value) 把旧地址换成新地址，换不了返回 None（这条 ref 不并入）；
    新函数一侧传入只认已确认地址的 translate。"""
    out = list(tokens)
    marks = {}
    for i, kind, value in refs or []:
        if kind in ("s", "i"):
            mark = f"{kind}:{value}"
        else:
            mapped = translate(kind, value) if translate else None
            if mapped is None:
                continue
            mark = f"{kind}:{mapped:X}"
        marks.setdefault(i, []).append(mark)
    for i, items in marks.items():
        if i < len(out):
            out[i] = out[i] + "#" + "|".join(sorted(items))
    return out


def new_refs(image, insns, imports):
    return insn_refs(image, insns, imports)


class Aligner:
    """一对函数（旧 token 序列、新 token 序列）之间的指令对齐，结果缓存在对象里。"""

    def __init__(self, old_seq, new_seq):
        alphabet = {}
        self.old_s = "".join(_char(alphabet, t) for t in old_seq)
        self.new_s = "".join(_char(alphabet, t) for t in new_seq)
        self._blocks = None
        self._ambiguous = None

    def blocks(self):
        if self._blocks is None:
            sm = difflib.SequenceMatcher(None, self.old_s, self.new_s, autojunk=False)
            self._blocks = [b for b in sm.get_matching_blocks() if b.size]
            self.ratio = sm.ratio()
        return self._blocks

    def by_blocks(self, i):
        for b in self.blocks():
            if b.a <= i < b.a + b.size:
                return b.b + (i - b.a), b.size
        return None, 0

    def ambiguous(self):
        """对齐有歧义的旧下标：匹配块之间的插入、删除能在重复的 token 上滑动时，滑过的旧下标（见文件开头第 2 条）。

        把匹配块展开成按顺序的匹配对 (i, j)，相邻两对之间旧、新两边没匹配上的 token 是一个空隙。空隙往左滑一格：
          新的一边有插入 new[s:e] 时，紧挨着的前一对 (pi, pj) 改成 (pi, e-1) 也成立，要求 new[pj] == new[e-1]；
          旧的一边有删除 old[s:e] 时，前一对改成 (e-1, pj) 也成立，要求 old[pi] == old[e-1]（old[pi] 和 old[e-1] 一个匹配、
          一个删除，两种对齐里身份互换）。
        滑动一直沿着连续的匹配对走，条件不成立就停；往右滑同理。空隙两边都有 token（替换）时两条都试，宁可多判歧义。"""
        if self._ambiguous is not None:
            return self._ambiguous
        old_s, new_s = self.old_s, self.new_s
        pairs = []
        for b in self.blocks():
            pairs += [(b.a + k, b.b + k) for k in range(b.size)]
        out = set()
        bounds = [(-1, -1)] + pairs + [(len(old_s), len(new_s))]
        for n in range(1, len(bounds)):
            (pi, pj), (ni, nj) = bounds[n - 1], bounds[n]
            ins = (pj + 1, nj) if nj > pj + 1 else None     # 新的一边没匹配上的 [s, e)
            dele = (pi + 1, ni) if ni > pi + 1 else None    # 旧的一边没匹配上的 [s, e)
            if not ins and not dele:
                continue
            # 往左滑：沿着空隙左边连续的匹配对
            k = n - 1
            step = 1
            while k >= 1 and (k == n - 1 or (bounds[k][0] + 1 == bounds[k + 1][0] and bounds[k][1] + 1 == bounds[k + 1][1])):
                i0, j0 = bounds[k]
                moved = False
                if ins and new_s[j0] == new_s[ins[1] - step]:
                    out.add(i0)
                    moved = True
                if dele and old_s[i0] == old_s[dele[1] - step]:
                    out.add(i0)
                    out.add(dele[1] - step)
                    moved = True
                if not moved:
                    break
                k -= 1
                step += 1
            # 往右滑：沿着空隙右边连续的匹配对
            k = n
            step = 0
            while k < len(bounds) - 1 and (k == n or (bounds[k - 1][0] + 1 == bounds[k][0] and bounds[k - 1][1] + 1 == bounds[k][1])):
                i0, j0 = bounds[k]
                moved = False
                if ins and new_s[j0] == new_s[ins[0] + step]:
                    out.add(i0)
                    moved = True
                if dele and old_s[i0] == old_s[dele[0] + step]:
                    out.add(i0)
                    out.add(dele[0] + step)
                    moved = True
                if not moved:
                    break
                k += 1
                step += 1
        self._ambiguous = out
        return out

    def by_window(self, i, max_window=MAX_WINDOW):
        old_s, new_s = self.old_s, self.new_s
        for length in range(1, max_window + 1):
            for o in range(length):
                s = i - o
                e = s + length
                if s < 0 or e > len(old_s):
                    continue
                w = old_s[s:e]
                if _count(old_s, w) != 1:
                    continue
                positions = _positions(new_s, w)
                if len(positions) == 1:
                    return positions[0] + o, length
        return None, 0

    def map(self, i):
        """(新下标, 说明) 或 (None, 原因)。规则见文件开头。"""
        if not 0 <= i < len(self.old_s):
            return None, f"旧指令下标 {i} 超出函数"
        j2, block = self.by_blocks(i)
        if j2 is None:
            return None, f"旧指令 {i} 不在全局对齐的匹配块里（这条指令在新函数里被删掉或换掉了）"
        if i in self.ambiguous():
            return None, (f"旧指令 {i} 旁边有插入或删除，落在重复的指令序列里，同样好的对齐不止一种（全局对齐给出新下标 {j2}，"
                          "但空隙可以滑动）")
        j1, window = self.by_window(i)
        if j1 is not None and j1 != j2:
            return None, f"唯一窗口（新下标 {j1}）和全局对齐（新下标 {j2}）不一致"
        if j1 is None:
            if block < MIN_BLOCK_ALONE:
                return None, (f"旧指令 {i} 周围 {MAX_WINDOW} 条以内找不到新旧函数里都唯一的指令序列，所在的匹配块只有 {block} 条"
                              f"（不到 {MIN_BLOCK_ALONE} 条）")
            return j2, f"匹配块 {block} 条、没有滑动的歧义（周围没有唯一窗口）"
        return j2, f"唯一窗口 {window} 条、匹配块 {block} 条"


def _char(alphabet, t):
    c = alphabet.get(t)
    if c is None:
        c = alphabet[t] = chr(0x100 + len(alphabet))
    return c


def _positions(s, w, limit=2):
    out = []
    p = s.find(w)
    while p >= 0 and len(out) < limit:
        out.append(p)
        p = s.find(w, p + 1)
    return out


def _count(s, w):
    return len(_positions(s, w))

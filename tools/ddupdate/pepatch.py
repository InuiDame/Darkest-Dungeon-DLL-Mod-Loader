"""改写 exe 副本的工具（U1d 的变异样本用）：模拟编译器输出的变化，修好所有受影响的引用，让结果像一个真实的构建。

只在 build/ 下的副本上用，不运行改出来的 exe。

  PeFile         可写的 PE 副本：按 RVA 读写；.text 末尾的空隙（.text 的 VirtualSize 到原始数据结尾和下一节之间，Steam 版
                 约 0x110 字节）放搬迁或复制的函数；.rdata 末尾原始数据里的空隙放新的 UNWIND_INFO；异常目录的增删改，写回时
                 保持按 BeginAddress 排序（运行时和 Python 都按排序的表二分）
  assemble       把一个函数的指令重新排到新地址：可以在任意指令前插入指令（可以带指向函数内或新标签的短跳转）、替换指令、
                 在末尾追加指令；修正函数内的分支、rip 相对位移、call/jmp rel32；短跳转放不下时改成近跳转，重复到布局稳定
  relocate       搬迁：assemble 到空隙，改写调用方的 call/jmp/jcc rel32、rip 相对引用（函数指针）、数据里指向它的绝对指针，
                 异常目录的表项指向新位置（序言变了时换一份新的 UNWIND_INFO），旧位置填 CC
  copy_function  复制（模板孪生、诱饵）：assemble 到空隙，加一条异常目录表项（和原函数共用 UNWIND_INFO：代码相同时序言也
                 相同，链接器合并相同的 unwind 数据也是这样），不改任何调用方
  grow_in_place  （U1 修复 FB，审核 A 的 S7）函数原地变长（插入新字段的代码）：后面的填充放不下时，紧跟着的几个函数依次
                 往后挪，每个挪到前一个的新结尾（不再按 16 字节对齐），挪到某个函数后面的填充够用为止，像重新链接时后面的
                 函数整体后移；这些函数之间的调用、对它们的调用和函数指针、虚表里的指针、异常目录的表项都改好。带 C++
                 异常处理信息的函数也挪：工具只读异常目录（.pdata）和 UNWIND_INFO，FuncInfo 里的 IP 到状态映射是 RVA，
                 不改（样本不运行）
  set_field      原地改一条指令的位移或立即数，宽度必须放得下（放不下时报错，不悄悄换编码）

不支持（遇到时报错，不硬改）：函数里有解码不了的字节、跳表（jmp 寄存器）、短跳转之外够不着的 loop/jrcxz、带 C++ 异常表的
函数的搬迁或复制（表里的 IP 到状态映射是 RVA；grow_in_place 例外，见上）。grow_in_place 挪动链式 unwind 的主函数时，分块
UNWIND_INFO 末尾的主函数副本跟着改。
"""
import re
import struct

import pefile
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from .fingerprint import engine

UNW_FLAG_EHANDLER, UNW_FLAG_UHANDLER, UNW_FLAG_CHAININFO = 1, 2, 4


class PatchError(Exception):
    pass


class PeFile:
    def __init__(self, path):
        self.raw = bytearray(open(path, "rb").read())
        pe = pefile.PE(data=bytes(self.raw), fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.sections = []
        for s in pe.sections:
            self.sections.append({"name": s.Name.rstrip(b"\0").decode(errors="replace"), "va": s.VirtualAddress,
                                  "vsize": s.Misc_VirtualSize, "raw": s.PointerToRawData, "rawsize": s.SizeOfRawData,
                                  "hdr": s.get_file_offset()})
        self.exc_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        self.exc_dir_off = self.exc_dir.get_file_offset()
        self.exc_va, self.exc_size = self.exc_dir.VirtualAddress, self.exc_dir.Size
        self.pe = pe

    # ---- 节和地址 ----

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        raise PatchError(f"没有节 {name}")

    def _next_va(self, sec):
        later = [s["va"] for s in self.sections if s["va"] > sec["va"]]
        return min(later) if later else None

    def off(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
                if rva - s["va"] >= s["rawsize"]:
                    raise PatchError(f"{rva:X} 在 {s['name']} 里但没有原始数据")
                return s["raw"] + rva - s["va"]
        raise PatchError(f"{rva:X} 不在任何节里")

    def read(self, rva, n):
        o = self.off(rva)
        return bytes(self.raw[o:o + n])

    def write(self, rva, data):
        o = self.off(rva)
        self.raw[o:o + len(data)] = data

    def u32(self, rva):
        return struct.unpack("<I", self.read(rva, 4))[0]

    def u64(self, rva):
        return struct.unpack("<Q", self.read(rva, 8))[0]

    def _set_vsize(self, sec, vsize):
        sec["vsize"] = vsize
        struct.pack_into("<I", self.raw, sec["hdr"] + 8, vsize)

    def text_tail(self):
        """.text 末尾可用的 [起点, 上限)：起点按 16 对齐，上限是原始数据的结尾和下一节的起点中小的那个。"""
        sec = self.section(".text")
        start = (sec["va"] + sec["vsize"] + 15) & ~15
        limit = min(sec["va"] + sec["rawsize"], self._next_va(sec) or 1 << 40)
        return start, limit

    def alloc_text(self, size):
        start, limit = self.text_tail()
        if start + size > limit:
            raise PatchError(f".text 末尾的空隙不够：要 {size:#x} 字节，只有 {limit - start:#x}")
        sec = self.section(".text")
        old_end = sec["va"] + sec["vsize"]
        self.write(old_end, b"\xcc" * (start - old_end))    # 函数之间的对齐填充
        self._set_vsize(sec, start + size - sec["va"])
        return start

    def alloc_rdata(self, size, align=4):
        sec = self.section(".rdata")
        start = (sec["va"] + sec["vsize"] + align - 1) & ~(align - 1)
        limit = min(sec["va"] + sec["rawsize"], self._next_va(sec) or 1 << 40)
        if start + size > limit:
            raise PatchError(f".rdata 末尾的空隙不够：要 {size:#x} 字节，只有 {limit - start:#x}")
        self._set_vsize(sec, start + size - sec["va"])
        return start

    # ---- 异常目录 ----

    def runtime_functions(self):
        out = []
        for off in range(self.exc_va, self.exc_va + self.exc_size - 11, 12):
            out.append(struct.unpack("<III", self.read(off, 12)))
        return out

    def write_runtime_functions(self, entries):
        entries = sorted(e for e in entries if e[0])
        size = len(entries) * 12
        sec = next(s for s in self.sections if s["va"] <= self.exc_va < s["va"] + s["rawsize"])
        if self.exc_va + size > sec["va"] + sec["rawsize"]:
            raise PatchError("异常目录所在的节放不下新的表项")
        data = b"".join(struct.pack("<III", *e) for e in entries)
        old = self.exc_size
        self.write(self.exc_va, data + b"\0" * max(0, old - size))
        self.exc_size = size
        struct.pack_into("<I", self.raw, self.exc_dir_off + 4, size)
        if self.exc_va + size > sec["va"] + sec["vsize"]:
            self._set_vsize(sec, self.exc_va + size - sec["va"])

    def entry_of(self, begin):
        for e in self.runtime_functions():
            if e[0] == begin:
                return e
        raise PatchError(f"异常目录里没有从 {begin:X} 开始的表项")

    def unwind_info(self, rva):
        """(flags, 序言长度, 代码槽数, 帧寄存器字节, 代码槽列表, 尾部（处理函数等）)。"""
        head = self.read(rva, 4)
        flags, prolog, count, frame = head[0] >> 3, head[1], head[2], head[3]
        codes = [struct.unpack("<H", self.read(rva + 4 + 2 * i, 2))[0] for i in range(count)]
        tail_off = rva + 4 + ((count + 1) & ~1) * 2
        tail = b""
        if flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER):
            tail = self.read(tail_off, 4)
        elif flags & UNW_FLAG_CHAININFO:
            tail = self.read(tail_off, 12)
        return {"version": head[0] & 7, "flags": flags, "prolog": prolog, "count": count, "frame": frame, "codes": codes,
                "tail": tail}

    def write_unwind(self, info):
        """在 .rdata 的空隙写一份新的 UNWIND_INFO，返回它的 RVA。"""
        count = len(info["codes"])
        head = bytes([info["version"] | (info["flags"] << 3), info["prolog"], count, info["frame"]])
        codes = b"".join(struct.pack("<H", c) for c in info["codes"])
        if count & 1:
            codes += b"\0\0"
        data = head + codes + info["tail"]
        rva = self.alloc_rdata(len(data))
        self.write(rva, data)
        return rva

    def save(self, path):
        with open(path, "wb") as f:
            f.write(self.raw)


# ---- unwind 代码 ----

def unwind_shift(info, delta):
    """序言前面插了 delta 字节：序言长度和每个代码槽的 CodeOffset 都加 delta（带参数的槽，例如 ALLOC_LARGE、SAVE_NONVOL 的
    第二个槽不是 CodeOffset，按操作码跳过）。"""
    codes = list(info["codes"])
    i = 0
    while i < len(codes):
        offset, op = codes[i] & 0xFF, codes[i] >> 8
        unwind_op, op_info = op & 0xF, op >> 4
        codes[i] = (op << 8) | (offset + delta)
        i += 1 + {1: 1 if op_info == 0 else 2, 4: 1, 5: 2, 8: 1, 9: 2}.get(unwind_op, 0)
    out = dict(info)
    out["codes"] = codes
    out["prolog"] = info["prolog"] + delta
    return out


def unwind_set_alloc(info, old_size, new_size):
    """把栈分配的大小从 old_size 改成 new_size（UWOP_ALLOC_SMALL / UWOP_ALLOC_LARGE），编码方式要放得下。"""
    codes = list(info["codes"])
    i = 0
    while i < len(codes):
        offset, op = codes[i] & 0xFF, codes[i] >> 8
        unwind_op, op_info = op & 0xF, op >> 4
        if unwind_op == 2 and op_info * 8 + 8 == old_size:
            if not 8 <= new_size <= 128 or new_size % 8:
                raise PatchError(f"ALLOC_SMALL 放不下 {new_size:#x}")
            codes[i] = (((new_size - 8) // 8) << 12) | (2 << 8) | offset
            break
        if unwind_op == 1 and op_info == 0 and codes[i + 1] * 8 == old_size:
            if new_size % 8 or new_size // 8 > 0xFFFF:
                raise PatchError(f"ALLOC_LARGE 放不下 {new_size:#x}")
            codes[i + 1] = new_size // 8
            break
        i += 1 + {1: 1 if op_info == 0 else 2, 4: 1, 5: 2, 8: 1, 9: 2}.get(unwind_op, 0)
    else:
        raise PatchError(f"unwind 代码里没有 {old_size:#x} 的栈分配")
    out = dict(info)
    out["codes"] = codes
    return out


# ---- 重新排布函数 ----

class Label:
    def __init__(self, name):
        self.name = name


_JCC_SHORT = re.compile(r"^j(?!mp$)")


class Unit:
    """一条要输出的指令。orig 是原来的指令（插入的为 None），target 是分支或 rip 相对的目标（原地址、Label 或外部地址）。"""

    def __init__(self, code, orig=None, kind="plain", target=None, label=None, disp_offset=None, short=None, opcode=None):
        self.code = bytes(code)
        self.orig = orig
        self.kind = kind            # plain / rel / rip
        self.target = target
        self.label = label
        self.disp_offset = disp_offset
        self.short = short
        self.opcode = opcode        # rel：'call' / 'jmp' / jcc 的条件码（0..15）
        self.addr = None

    @property
    def size(self):
        if self.kind != "rel":
            return len(self.code)
        if self.short:
            return 2
        return 6 if isinstance(self.opcode, int) else 5


def classify(ins, code=None):
    code = bytes(code if code is not None else ins.bytes)
    if ins.mnemonic in ("loop", "loope", "loopne", "jrcxz", "jecxz"):
        raise PatchError(f"{ins.address:X} {ins.mnemonic} 不支持")
    if ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type != X86_OP_IMM:
        if not (ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.base == X86_REG_RIP):
            raise PatchError(f"{ins.address:X} 间接跳转 {ins.op_str}（可能是跳表），不支持")
    if (ins.mnemonic in ("call", "jmp") or _JCC_SHORT.match(ins.mnemonic)) and ins.operands \
            and ins.operands[0].type == X86_OP_IMM:
        if ins.mnemonic == "call":
            opcode = "call"
        elif ins.mnemonic == "jmp":
            opcode = "jmp"
        else:
            b0 = code[0]
            opcode = (b0 - 0x70) if 0x70 <= b0 <= 0x7F else (code[1] - 0x80)
        return Unit(code, ins, "rel", target=ins.operands[0].imm, short=len(code) == 2, opcode=opcode)
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            if ins.disp_size != 4:
                raise PatchError(f"{ins.address:X} rip 相对位移不是 32 位")
            return Unit(code, ins, "rip", target=ins.address + ins.size + ins.disp, disp_offset=ins.disp_offset)
    return Unit(code, ins)


def decode_range(cs, data, begin, end):
    insns = list(cs.disasm(data, begin))
    if not insns or insns[-1].address + insns[-1].size != end:
        raise PatchError(f"{begin:X}..{end:X} 没能完整解码（函数里有数据或填充之外的字节）")
    return insns


def assemble(pe, cs, begin, end, base, insert=None, replace=None, append=None, drop=(), translate=None):
    """insert：{原指令地址: [Unit 或 bytes, ...]}，插在那条指令前面；replace：{原指令地址: bytes}，换掉那条指令（不能是分支或
    rip 相对指令）；append：[Unit 或 bytes]，追加在末尾；drop：去掉的原指令地址；translate(地址) 把函数外的目标换成新地址
    （一起挪动的别的函数，grow_in_place 用）。返回 (代码, {原指令地址: 新地址}, 标签表)。"""
    cs = cs or engine()
    insert, replace, append = insert or {}, replace or {}, append or []
    insns = decode_range(cs, pe.read(begin, end - begin), begin, end)
    starts = {i.address for i in insns}
    units = []

    def as_unit(u):
        if isinstance(u, Unit):
            return u
        ins = next(cs.disasm(bytes(u), 0))
        unit = classify(ins, u)
        unit.orig = None
        if unit.kind != "plain":
            raise PatchError("插入的原始字节里不能有分支或 rip 相对指令（用 Unit 写明目标）")
        return unit

    for ins in insns:
        for u in insert.get(ins.address, []):
            units.append(as_unit(u))
        if ins.address in drop:
            continue
        if ins.address in replace:
            new = replace[ins.address]
            probe = next(cs.disasm(bytes(new), ins.address))
            unit = classify(probe, new)
            if unit.kind != "plain":
                raise PatchError(f"{ins.address:X} 的替换指令不能是分支或 rip 相对指令")
            unit.orig = ins
            units.append(unit)
        else:
            units.append(classify(ins))
    for u in append:
        units.append(as_unit(u))

    def resolve(unit, amap, labels):
        t = unit.target
        if isinstance(t, Label):
            return labels[t.name]
        if begin <= t < end:
            if t not in starts or t not in amap:
                raise PatchError(f"目标 {t:X} 在函数里但不是指令起点（或那条指令被去掉了）")
            return amap[t]
        return translate(t) if translate else t

    for _ in range(64):
        addr = base
        amap, labels = {}, {}
        for u in units:
            u.addr = addr
            if u.orig is not None:
                amap[u.orig.address] = addr
            if u.label:
                labels[u.label] = addr
            addr += u.size
        changed = False
        for u in units:
            if u.kind == "rel" and u.short:
                disp = resolve(u, amap, labels) - (u.addr + 2)
                if not -128 <= disp <= 127:
                    u.short = False
                    changed = True
        if not changed:
            break
    else:
        raise PatchError("布局没有收敛")
    out = bytearray()
    for u in units:
        if u.kind == "plain":
            out += u.code
        elif u.kind == "rip":
            target = resolve(u, amap, labels)
            code = bytearray(u.code)
            struct.pack_into("<i", code, u.disp_offset, target - (u.addr + len(code)))
            out += code
        else:
            target = resolve(u, amap, labels)
            if u.short:
                op = 0xEB if u.opcode == "jmp" else 0x70 + u.opcode
                out += bytes([op]) + struct.pack("<b", target - (u.addr + 2))
            elif u.opcode == "call":
                out += b"\xe8" + struct.pack("<i", target - (u.addr + 5))
            elif u.opcode == "jmp":
                out += b"\xe9" + struct.pack("<i", target - (u.addr + 5))
            else:
                out += bytes([0x0F, 0x80 + u.opcode]) + struct.pack("<i", target - (u.addr + 6))
    return bytes(out), amap, labels


# ---- 引用 ----

class References:
    """在原 exe（image，按 RVA 映射的只读副本）里找指向某个地址的引用：call/jmp/jcc rel32、rip 相对（函数指针）、
    .rdata/.data 里的 8 字节绝对指针。rel32 和 rip 相对的引用要解码所在函数确认落在指令边界上。"""

    def __init__(self, image, cs=None, index=None):
        self.image = image
        self.cs = cs or engine()
        self.index = index
        self._starts = {}
        self._text = image.bytes_at(image.text_start, image.text_end - image.text_start)

    def _insn_starts(self, func):
        got = self._starts.get(func)
        if got is None:
            got = set()
            for b, e in self.image.function_ranges(func) or []:
                got.update(i.address for i in self.cs.disasm(self.image.bytes_at(b, e - b), b))
            self._starts[func] = got
        return got

    def _at_boundary(self, site):
        func = self.image.enclosing_function(site)
        if func is None:
            # 异常目录空隙里的 thunk：对齐或者前面是填充
            return site & 0xF == 0 or self.image.mem[site - 1] in (0xCC, 0xC3, 0x90)
        return site in self._insn_starts(func)

    def rel_sites(self, target):
        image, text, out = self.image, self._text, []
        for m in re.finditer(rb"[\xe8\xe9]|\x0f[\x80-\x8f]", text):
            n = len(m.group(0))
            if m.start() + n + 4 > len(text):
                continue
            p = image.text_start + m.start()
            if p + n + 4 + struct.unpack_from("<i", text, m.start() + n)[0] == target and self._at_boundary(p):
                out.append(p)
        return out

    def rip_sites(self, target):
        """rip 相对引用 target 的指令：[(指令地址, 位移在指令里的偏移, 指令长度)]。用函数索引的函数指针反查表找函数。"""
        out = []
        funcs = self.index.by_coderef.get(target, []) if self.index is not None else []
        for func in funcs:
            for b, e in self.image.function_ranges(func) or [(func, func + 0x1000)]:
                for ins in self.cs.disasm(self.image.bytes_at(b, e - b), b):
                    if ins.disp_size == 4 and any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands) \
                            and ins.address + ins.size + ins.disp == target:
                        out.append((ins.address, ins.disp_offset, ins.size))
        return out

    def rel_sites_into(self, ranges):
        """call/jmp/jcc rel32 的目标落在 ranges（[(起点, 结尾)]）里的引用：[(位置, 目标)]，落在指令边界上的。"""
        image, text, out = self.image, self._text, []
        for m in re.finditer(rb"[\xe8\xe9]|\x0f[\x80-\x8f]", text):
            n = len(m.group(0))
            if m.start() + n + 4 > len(text):
                continue
            p = image.text_start + m.start()
            target = p + n + 4 + struct.unpack_from("<i", text, m.start() + n)[0]
            if any(lo <= target < hi for lo, hi in ranges) and self._at_boundary(p):
                out.append((p, target))
        return out

    def data_pointers(self, target):
        image = self.image
        needle = struct.pack("<Q", image.base + target)
        out = []
        for name in (".rdata", ".data"):
            start, end = image.sections[name]
            data = image.bytes_at(start, end - start)
            at = data.find(needle)
            while at != -1:
                if at % 8 == 0:
                    out.append(start + at)
                at = data.find(needle, at + 1)
        return out


def retarget(pe, refs, old, new):
    """把指向 old 的引用改成指向 new。返回各类引用的个数。"""
    counts = {"rel32": 0, "rip": 0, "pointer": 0}
    for site in refs.rel_sites(old):
        code = pe.read(site, 2)
        n = 2 if code[0] == 0x0F else 1
        pe.write(site + n, struct.pack("<i", new - (site + n + 4)))
        counts["rel32"] += 1
    for addr, disp_off, size in refs.rip_sites(old):
        pe.write(addr + disp_off, struct.pack("<i", new - (addr + size)))
        counts["rip"] += 1
    for ptr in refs.data_pointers(old):
        pe.write(ptr, struct.pack("<Q", pe.base + new))
        counts["pointer"] += 1
    return counts


def check_movable(pe, image, begin):
    chunks = image.function_ranges(begin)
    if chunks is None or len(chunks) != 1:
        raise PatchError(f"{begin:X} 不是只有一个分块的函数")
    _b, _e, unwind = pe.entry_of(begin)
    info = pe.unwind_info(unwind)
    if info["flags"] & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER | UNW_FLAG_CHAININFO):
        raise PatchError(f"{begin:X} 带异常处理或链式 unwind 信息（表里有 RVA），不搬迁")
    return chunks[0], unwind, info


def relocate(pe, image, refs, begin, insert=None, replace=None, append=None, drop=(), unwind_edit=None, cs=None):
    """搬迁函数，返回 (新地址, 新长度, {原指令地址: 新地址}, 标签表, 各类引用的个数)。
    unwind_edit(info, shift) 返回改过的 UNWIND_INFO（序言变了时，写一份新的，原来那份可能和别的函数共用）；shift 是原来的
    第一条指令在新函数里后移了多少字节（入口前插了指令时，短跳转可能被改成近跳转，所以要按排布的结果算）。"""
    (b, e), unwind, info = check_movable(pe, image, begin)
    base, _limit = pe.text_tail()
    code, amap, labels = assemble(pe, cs, b, e, base, insert, replace, append, drop)
    new = pe.alloc_text(len(code))
    assert new == base
    pe.write(new, code)
    pe.write(b, b"\xcc" * (e - b))
    counts = retarget(pe, refs, begin, new)
    if unwind_edit is not None:
        unwind = pe.write_unwind(unwind_edit(info, amap[b] - new))
    entries = [x for x in pe.runtime_functions() if x[0] != b] + [(new, new + len(code), unwind)]
    pe.write_runtime_functions(entries)
    return new, len(code), amap, labels, counts


def copy_function(pe, image, begin, insert=None, replace=None, cs=None):
    """复制函数到空隙，加异常目录表项，返回 (新地址, 长度)。"""
    (b, e), unwind, _info = check_movable(pe, image, begin)
    base, _limit = pe.text_tail()
    code, _amap, _labels = assemble(pe, cs, b, e, base, insert, replace)
    new = pe.alloc_text(len(code))
    pe.write(new, code)
    pe.write_runtime_functions(pe.runtime_functions() + [(new, new + len(code), unwind)])
    return new, len(code)


def _gap_units(pe, cs, lo, hi):
    """[lo, hi) 是两个异常目录表项之间的空隙：里面除了填充（CC、90）可能还有叶函数（不需要 .pdata 表项的函数）。
    返回 [(起点, 结尾)]：每段非填充的字节从第一个非填充字节解码到最后一个非填充字节，要恰好解码完整。"""
    data = pe.read(lo, hi - lo)
    out = []
    i = 0
    while i < len(data):
        if data[i] in (0xCC, 0x90):
            i += 1
            continue
        j = len(data)
        while j > i and data[j - 1] in (0xCC, 0x90):
            j -= 1
        # 从 i 线性解码，到 ret / jmp 之后只剩填充为止（同一段空隙里可能有几个叶函数，一起挪）
        pos = i
        while pos < j:
            ins = next(cs.disasm(data[pos:j], lo + pos), None)
            if ins is None:
                raise PatchError(f"{lo + pos:X}：空隙里有解码不了的字节（数据？），不挪")
            pos += ins.size
        if pos != j:
            raise PatchError(f"{lo + i:X}..{lo + j:X}：空隙里的代码没能完整解码")
        out.append((lo + i, lo + j))
        i = j
    return out


def grow_in_place(pe, image, refs, begin, insert=None, replace=None, drop=(), cs=None):
    """函数原地重排（规则见文件开头）。后面的函数（含空隙里没有 .pdata 表项的叶函数）依次后移到前一个的新结尾，直到某段
    填充够用。返回 (新长度, [(原起点, 新起点, 长度)]：挪动的代码, 各类引用的改写个数)。"""
    cs = cs or engine()
    entries = sorted(pe.runtime_functions())
    b, e, unwind = pe.entry_of(begin)
    code, _amap, _labels = assemble(pe, cs, b, e, b, insert, replace, None, drop)
    new_end = b + len(code)
    # 链式 unwind：分块的 UNWIND_INFO 末尾有主函数 RUNTIME_FUNCTION 的副本（起点、结尾、unwind），主函数挪了要跟着改
    chain_tails = {}
    for _eb, _ee, eu in entries:
        if not eu & 1:
            info = pe.unwind_info(eu)
            if info["flags"] & UNW_FLAG_CHAININFO:
                parent = struct.unpack("<I", info["tail"][:4])[0]
                chain_tails.setdefault(parent, []).append(eu + 4 + ((info["count"] + 1) & ~1) * 2)
    # 要挪的代码单元：异常目录表项和空隙里的叶函数，按地址排好
    moves = []                       # (原起点, 原结尾, 是不是表项, 挪动的字节数)
    cursor = new_end
    prev = e
    following = [x for x in entries if x[0] >= e]
    done = False
    for fb, fe, _fu in following + [(image.text_end, image.text_end, 0)]:
        for lb, le in _gap_units(pe, cs, prev, fb):
            if lb >= cursor:
                done = True
                break
            moves.append((lb, le, False, cursor - lb))
            cursor = le + (cursor - lb)
        if done or fb >= cursor or fb >= image.text_end:
            break
        moves.append((fb, fe, True, cursor - fb))
        cursor = fe + (cursor - fb)
        prev = fe
    if cursor > image.text_end:
        raise PatchError("挪到 .text 末尾也放不下")
    span_end = moves[-1][1] if moves else e

    def translate(t):
        for mb, me, _ent, d in moves:
            if mb <= t < me:
                return t + d
        return t

    code, _amap, _labels = assemble(pe, cs, b, e, b, insert, replace, None, drop, translate=translate)
    if b + len(code) != new_end:
        raise PatchError("重排的长度不稳定")
    moved_code = []
    for mb, me, _ent, d in moves:
        c, _a, _l = assemble(pe, cs, mb, me, mb + d, translate=translate)
        if len(c) != me - mb:
            raise PatchError(f"{mb:X} 挪动后长度变了")
        moved_code.append((mb + d, c))
    pe.write(b, b"\xcc" * (max(span_end, cursor) - b))
    pe.write(b, code)
    for addr, c in moved_code:
        pe.write(addr, c)
    counts = {"rel32": 0, "rip": 0, "pointer": 0}
    inside = lambda site: b <= site < span_end  # noqa: E731 - 一起重排的代码里的引用已经由 translate 改好
    ranges = [(mb, me) for mb, me, _ent, _d in moves]
    for site, target in refs.rel_sites_into(ranges):
        if inside(site):
            continue
        head = pe.read(site, 2)
        n = 2 if head[0] == 0x0F else 1
        pe.write(site + n, struct.pack("<i", translate(target) - (site + n + 4)))
        counts["rel32"] += 1
    for mb, _me, _ent, d in moves:
        for addr, disp_off, size in refs.rip_sites(mb):
            if inside(addr):
                continue
            pe.write(addr + disp_off, struct.pack("<i", mb + d - (addr + size)))
            counts["rip"] += 1
        for ptr in refs.data_pointers(mb):
            pe.write(ptr, struct.pack("<Q", pe.base + mb + d))
            counts["pointer"] += 1
    shift = {mb: (mb + d, me + d) for mb, me, ent, d in moves if ent}
    for mb, (nb, ne) in shift.items():
        for tail in chain_tails.get(mb, []):
            pe.write(tail, struct.pack("<II", nb, ne))
    out = []
    for eb, ee, eu in pe.runtime_functions():
        if eb == b:
            out.append((b, new_end, eu))
        elif eb in shift:
            out.append((shift[eb][0], shift[eb][1], eu))
        else:
            out.append((eb, ee, eu))
    pe.write_runtime_functions(out)
    return len(code), [(mb, mb + d, me - mb) for mb, me, _ent, d in moves], counts


def edit_unwind(pe, begin, edit):
    """改 begin 这个函数的 UNWIND_INFO（edit(info) 返回新的，代码槽数不变）。只有这一个表项用它时原地改；和别的函数共用时
    写一份新的（带异常处理的不支持，处理函数的数据长度不定）。"""
    b, e, unwind = pe.entry_of(begin)
    info = pe.unwind_info(unwind)
    new = edit(info)
    if len(new["codes"]) != len(info["codes"]):
        raise PatchError("unwind 代码槽数变了")
    users = [x for x in pe.runtime_functions() if x[2] == unwind]
    if len(users) == 1:
        head = bytes([new["version"] | (new["flags"] << 3), new["prolog"], len(new["codes"]), new["frame"]])
        pe.write(unwind, head + b"".join(struct.pack("<H", c) for c in new["codes"]))
        return unwind
    if info["flags"] & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER):
        raise PatchError(f"{begin:X} 的 UNWIND_INFO 和别的函数共用又带异常处理，不改")
    rva = pe.write_unwind(new)
    pe.write_runtime_functions([x for x in pe.runtime_functions() if x[0] != b] + [(b, e, rva)])
    return rva


# ---- 原地改指令 ----

def set_field(pe, cs, addr, field, value):
    """把 addr 处指令的位移（field='disp'）或立即数（'imm'）改成 value，宽度不变。返回改之前的值。"""
    ins = next(cs.disasm(pe.read(addr, 16), addr))
    if field == "disp":
        offset, size = ins.disp_offset, ins.disp_size
    else:
        offset, size = ins.imm_offset, ins.imm_size
    if not size:
        raise PatchError(f"{addr:X} {ins.mnemonic} {ins.op_str} 没有 {field} 字段")
    raw = pe.read(addr + offset, size)
    old = int.from_bytes(raw, "little", signed=True)
    lo, hi = -(1 << (8 * size - 1)), (1 << (8 * size - 1)) - 1
    if size == 8 and field == "imm":
        lo, hi = -(1 << 63), (1 << 64) - 1
    if not lo <= value <= hi:
        raise PatchError(f"{addr:X} {ins.mnemonic} {ins.op_str}：{field} 宽 {size} 字节放不下 {value:#x}（要换编码，不改）")
    pe.write(addr + offset, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))
    return old


def function_insns(pe, cs, begin, end):
    """原地改用：函数的指令。函数末尾有 switch 的跳表（MSVC 把 4 字节 RVA 的表放在代码后面，同一个异常目录表项里）时，
    按 [基址 + 变址*4 + 表的 RVA] 这种操作数找到表的起点，只解码表之前的代码。"""
    data = pe.read(begin, end - begin)
    insns = list(cs.disasm(data, begin))
    if insns and insns[-1].address + insns[-1].size == end:
        return insns
    tables = []
    for ins in insns:
        op = mem_operand(ins)
        if op is not None and op.mem.index != 0 and op.mem.scale == 4 and begin <= op.mem.disp < end:
            tables.append(op.mem.disp)
    if not tables:
        raise PatchError(f"{begin:X}..{end:X} 没能完整解码（函数里有数据或填充之外的字节）")
    cut = min(tables)
    kept = [i for i in insns if i.address + i.size <= cut]
    if not kept or not any(i.address + i.size == cut for i in kept) and data[kept[-1].address + kept[-1].size - begin:cut - begin].strip(b"\xcc"):
        raise PatchError(f"{begin:X}..{end:X} 跳表 {cut:X} 之前的代码没能完整解码")
    return kept


def mem_operand(ins):
    for op in ins.operands:
        if op.type == X86_OP_MEM:
            return op
    return None


def shift_fields(pe, cs, begin, end, regs, lo, hi, delta, reg_name):
    """结构中间插了字段：函数里以 regs 为基址（无变址）、位移在 [lo, hi) 的内存操作数，位移加 delta。返回改了的指令地址。"""
    changed = []
    for ins in function_insns(pe, cs, begin, end):
        op = mem_operand(ins)
        if op is None or op.mem.index != 0 or op.mem.base == 0 or op.mem.base == X86_REG_RIP:
            continue
        if reg_name(op.mem.base) not in regs or not lo <= op.mem.disp < hi:
            continue
        set_field(pe, cs, ins.address, "disp", op.mem.disp + delta)
        changed.append(ins.address)
    return changed

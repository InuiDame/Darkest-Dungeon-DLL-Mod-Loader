"""PE 映像：按 RVA 访问、异常目录里的函数范围、特征码扫描。

和 DDCore 的运行时核对（U1b）是同一套语义，C++ 照着实现：
  - 映像按节映射到 RVA（和加载后的模块一样），.text 的范围是 VirtualAddress .. VirtualAddress + VirtualSize。
  - 函数范围只看异常目录（DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]）里的 RUNTIME_FUNCTION，不用 .pdata 节名，
    也不用 RtlLookupFunctionEntry。表项按 BeginAddress 排序，BeginAddress 为 0 的跳过。
  - 主函数：从表项出发，UnwindData 低位为 1 时是间接表项（指向另一个 RUNTIME_FUNCTION）；否则看 UNWIND_INFO 的
    UNW_FLAG_CHAININFO（Flags & 4），有时链到 unwind codes 之后的 RUNTIME_FUNCTION；最多跟 32 次，最后到达的表项
    的 BeginAddress 就是主函数。主函数相同的所有表项是同一个函数的分块。
  - 特征码匹配可以重叠（每个字节位置都试一次），命中次数和 common/pattern.h 的 ScanPattern 一致。
"""
import bisect
import hashlib
import re
import struct
from pathlib import Path

import pefile

UNW_FLAG_CHAININFO = 4


def compile_pattern(text):
    """"48 8B ?? 05" 形式的特征码编译成可重叠匹配的正则，返回 (regex, 长度)。格式不对时抛 ValueError。"""
    tokens = text.split()
    if not tokens:
        raise ValueError("空特征码")
    parts = []
    for token in tokens:
        if token == "??":
            parts.append(b".")
        elif re.fullmatch(r"[0-9A-Fa-f]{2}", token):
            parts.append(re.escape(bytes([int(token, 16)])))
        else:
            raise ValueError(f"特征码里有无效的字节 {token!r}")
    return re.compile(b"(?=(" + b"".join(parts) + b"))", re.S), len(tokens)


def pattern_bytes(text):
    """特征码的字节列表，通配是 None。"""
    return [None if t == "??" else int(t, 16) for t in text.split()]


class Image:
    """按 RVA 访问的 PE 映像（只读，不执行）。"""

    def __init__(self, path):
        self.path = str(path)
        raw = Path(path).read_bytes()
        self.file_size = len(raw)
        self.sha1 = hashlib.sha1(raw).hexdigest()
        pe = pefile.PE(data=raw, fast_load=True)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        headers = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.mem[:headers] = raw[:headers]
        self.sections = {}
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            data = s.get_data()
            end = min(s.VirtualAddress + len(data), self.size)
            self.mem[s.VirtualAddress:end] = data[:end - s.VirtualAddress]
            self.sections.setdefault(name, (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize))
        self.text_start, self.text_end = self.sections[".text"]
        self._text = bytes(self.mem[self.text_start:self.text_end])
        self._load_functions(pe)

    # ---- 函数表 ----

    def _load_functions(self, pe):
        directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
        start, size = directory.VirtualAddress, directory.Size
        entries = []
        for off in range(start, start + size - 11, 12):
            begin, end, unwind = struct.unpack_from("<III", self.mem, off)
            if begin == 0:
                continue
            entries.append((begin, end, unwind))
        entries.sort()
        self.func_starts = [e[0] for e in entries]
        self.func_ends = {e[0]: e[1] for e in entries}
        self._unwind = {e[0]: e[2] for e in entries}
        self.func_primary = {e[0]: self._primary_of(e) for e in entries}
        self.func_chunks = {}
        for begin, primary in self.func_primary.items():
            self.func_chunks.setdefault(primary, []).append((begin, self.func_ends[begin]))
        for chunks in self.func_chunks.values():
            chunks.sort()

    def _primary_of(self, entry):
        begin, _end, unwind = entry
        for _ in range(32):
            if unwind & 1:
                if unwind & ~1 >= self.size - 12:
                    break
                begin, _end, unwind = struct.unpack_from("<III", self.mem, unwind & ~1)
                continue
            if unwind + 4 > self.size:
                break
            flags = self.mem[unwind] >> 3
            if not flags & UNW_FLAG_CHAININFO:
                break
            count = self.mem[unwind + 2]
            chained = unwind + 4 + ((count + 1) & ~1) * 2
            if chained + 12 > self.size:
                break
            begin, _end, unwind = struct.unpack_from("<III", self.mem, chained)
        return begin

    def entry_of(self, rva):
        """包含 rva 的 RUNTIME_FUNCTION 的 BeginAddress，没有时 None。"""
        index = bisect.bisect_right(self.func_starts, rva) - 1
        if index < 0:
            return None
        begin = self.func_starts[index]
        return begin if rva < self.func_ends[begin] else None

    def enclosing_function(self, rva):
        """包含 rva 的主函数起点，不在任何表项里时 None。"""
        begin = self.entry_of(rva)
        return None if begin is None else self.func_primary[begin]

    def function_ranges(self, rva):
        """包含 rva 的函数的全部分块 [(begin, end), ...]，按地址排序；不在任何表项里时 None。"""
        primary = self.enclosing_function(rva)
        return None if primary is None else self.func_chunks.get(primary, [])

    def is_function_start(self, rva):
        return rva in self.func_ends and self.func_primary.get(rva) == rva

    # ---- 读取 ----

    def section_of(self, rva):
        for name, (start, end) in self.sections.items():
            if start <= rva < end:
                return name
        return None

    def in_text(self, rva):
        return self.text_start <= rva < self.text_end

    def u8(self, rva):
        return self.mem[rva]

    def u16(self, rva):
        return struct.unpack_from("<H", self.mem, rva)[0]

    def u32(self, rva):
        return struct.unpack_from("<I", self.mem, rva)[0]

    def i32(self, rva):
        return struct.unpack_from("<i", self.mem, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.mem, rva)[0]

    def cstr(self, rva, limit=512):
        if not 0 <= rva < self.size:
            return None
        end = self.mem.find(b"\0", rva, min(rva + limit, self.size))
        if end < 0:
            return None
        return bytes(self.mem[rva:end])

    def bytes_at(self, rva, count):
        return bytes(self.mem[rva:rva + count])

    # ---- 扫描 ----

    def scan(self, regex, start, end):
        """[start, end) 内完整落在范围里的全部命中（可重叠），返回 RVA 列表。"""
        start = max(start, 0)
        end = min(end, self.size)
        if end <= start:
            return []
        if start >= self.text_start and end <= self.text_end:
            data = self._text
            offset = self.text_start
            return [m.start() + offset for m in regex.finditer(data, start - offset, end - offset)
                    if m.end(1) <= end - offset]
        data = bytes(self.mem[start:end])
        return [m.start() + start for m in regex.finditer(data)]

    def scan_text(self, regex):
        return [m.start() + self.text_start for m in regex.finditer(self._text)]

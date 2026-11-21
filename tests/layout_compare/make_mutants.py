"""生成 C++/Python 对照测试（tools/ddupdate/cpp_compare.py）用的破坏样本（U1b），在 guest 上运行：

    python -B DD.ModLoader/tests/layout_compare/make_mutants.py [--exe Steam 版 Darkest.exe] [--out DD.ModLoader/build/u1b/mut]

这些样本只为覆盖依据求值的各条失败路径（两边对失败的判定要一样），不模拟真实的编译器输出（那是 U1d 的变异样本）。
每个样本在 Steam 版上改几个字节，地址按清单现场解析，不写死 RVA；样本只读、不运行。

  C_vtable_sig    HeroVtable 的特征码被破坏（mov rax,[rsi+0x1320] 的位移改掉）：以它为起点的依据全部「没法求值」（blocked）
  D_epsilon       DamageEpsilonBits 引用的 float 常量改掉：DEREF_U32 读到的值不对
  E_text_dup      RosterAlt 的 TEXT 模式在 .text 的 int3 填充里多出一份：命中 2 次
  F_vslot         Hero 虚表槽 40 的指针改成指向 .rdata：VSLOT 起点不在 .text
  G_anchor        锚点 BuffInstanceCtor 和 MonsterBlockReturn 被破坏：InstDuration 的一条、ListenerFunction（必需）等依据 blocked
  H_rel32         BattleGuidCounter 的 mov rdi,[Raid::Current] 位移改掉：REL32 读到的目标不是 RaidCurrent
  I_pdata         BuffGroupErase 在异常目录里的表项清零：FUNC 起点不在任何表项里
  J_entry_hooked  ApplyHit 的入口被改成 jmp（像旧式 DLL 的内联 hook）：Python 的静态检查判失效、运行时照常解析（对照的特殊路径）
"""
import argparse
import struct
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "tools"))

import pefile  # noqa: E402

from ddupdate import evidence as evidence_mod  # noqa: E402
from ddupdate import manifest as manifest_mod  # noqa: E402
from ddupdate import sigs as sigs_mod  # noqa: E402
from ddupdate.image import Image, compile_pattern  # noqa: E402

STEAM = r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe"


class Patcher:
    def __init__(self, path):
        self.raw = bytearray(Path(path).read_bytes())
        self.pe = pefile.PE(data=bytes(self.raw), fast_load=True)
        self.notes = []

    def offset(self, rva):
        return self.pe.get_offset_from_rva(rva)

    def write(self, rva, data, why):
        off = self.offset(rva)
        old = bytes(self.raw[off:off + len(data)])
        self.raw[off:off + len(data)] = data
        self.notes.append(f"{rva:X}: {old.hex(' ')} -> {bytes(data).hex(' ')}  {why}")

    def save(self, path):
        Path(path).write_bytes(bytes(self.raw))


def evidence_of(manifest, item, index=0):
    return [ev for ev in manifest.evidence if ev.item == item][index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=STEAM)
    parser.add_argument("--out", default=str(PROJECT / "build" / "u1b" / "mut"))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = manifest_mod.load(None, None, True)
    image = Image(args.exe)
    res = {sid: sigs_mod.resolve(image, sig) for sid, sig in manifest.sigs.items()}
    for sid in ("HeroVtable", "BuffInstanceCtor", "MonsterBlockReturn", "BuffGroupErase"):
        assert res[sid].status == "ok", sid

    def ev_result(item, index=0):
        return evidence_mod.evaluate(image, manifest, res, evidence_of(manifest, item, index))

    samples = {}

    # C：HeroVtable 的特征码 "48 8D 05 ?? ?? ?? ?? 49 89 06 48 8B 86 20 13 00 00 49 89 86"，+13 是位移 0x1320 的低字节
    p = Patcher(args.exe)
    p.write(res["HeroVtable"].hit + 13, b"\x28", "HeroVtable 特征码里的位移 0x1320 改成 0x1328")
    samples["C_vtable_sig"] = p

    # D：DamageEpsilonBits 的依据是 DEREF_U32，目标处的 float 1e-6 改成 2e-6
    r = ev_result("DamageEpsilonBits")
    at = r.hit + evidence_of(manifest, "DamageEpsilonBits").at
    target = at + 4 + image.i32(at)
    p = Patcher(args.exe)
    p.write(target, struct.pack("<f", 2e-6), "DamageEpsilonBits 引用的 float 改成 2e-6")
    samples["D_epsilon"] = p

    # E：RosterAlt 的 TEXT 模式命中处的字节复制一份，盖在一个和清单无关的函数中间（.text 里没有够长的 int3 填充）。
    # 选的函数不含任何特征码的命中和任何依据的命中，不影响别的项
    ev = evidence_of(manifest, "RosterAlt")
    r = ev_result("RosterAlt")
    _, length = compile_pattern(ev.pattern)
    chunk = image.bytes_at(r.hit, length)
    used = {x for s in res.values() for x in (s.hit, s.resolved) if x is not None}
    used |= {evidence_mod.evaluate(image, manifest, res, e).hit for e in manifest.evidence} - {None}
    victim = None
    for begin in image.func_starts:
        end = image.func_ends[begin]
        if begin >= 0x800000 and end - begin >= 256 and not any(begin <= u < end for u in used):
            victim = begin
            break
    assert victim is not None
    p = Patcher(args.exe)
    p.write(victim + 64, chunk, f"RosterAlt 的模式在无关函数 {victim:X} 中间多出一份")
    samples["E_text_dup"] = p

    # F：Hero 虚表槽 40 的指针改成虚表本身（.rdata）
    table = res["HeroVtable"].resolved
    p = Patcher(args.exe)
    p.write(table + 40 * 8, struct.pack("<Q", image.base + table), "Hero 虚表槽 40 指向 .rdata")
    samples["F_vslot"] = p

    # G：锚点 BuffInstanceCtor 的 "C6 41 04 00"（立即数在 +21）和 MonsterBlockReturn 的 "41 B8 31 00 00 00"（立即数在 +9）改掉
    p = Patcher(args.exe)
    p.write(res["BuffInstanceCtor"].hit + 21, b"\x01", "BuffInstanceCtor 的 mov byte [rcx+4],0 改成 1")
    p.write(res["MonsterBlockReturn"].hit + 9, b"\x32", "MonsterBlockReturn 的 mov r8d,0x31 改成 0x32")
    samples["G_anchor"] = p

    # H：BattleGuidCounter 的第二条依据（REL32）：mov rdi,[rip+X] 的位移加 8
    ev = evidence_of(manifest, "BattleGuidCounter", 1)
    r = ev_result("BattleGuidCounter", 1)
    at = r.hit + ev.at
    p = Patcher(args.exe)
    p.write(at, struct.pack("<i", image.i32(at) + 8), "mov rdi,[Raid::Current] 的位移加 8")
    samples["H_rel32"] = p

    # I：BuffGroupErase 所在的 RUNTIME_FUNCTION 清零（BeginAddress 为 0 的表项被跳过）
    directory = image.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    entry_rva = None
    for off in range(directory.VirtualAddress, directory.VirtualAddress + directory.Size - 11, 12):
        begin, end, _unwind = struct.unpack_from("<III", image.mem, off)
        if begin <= res["BuffGroupErase"].hit < end:
            entry_rva = off
            break
    assert entry_rva is not None
    p = Patcher(args.exe)
    p.write(entry_rva, b"\0" * 12, "BuffGroupErase 的 RUNTIME_FUNCTION 清零")
    samples["I_pdata"] = p

    # J：ApplyHit 的入口被改成 jmp rel32 + 2 个 nop（旧式 DLL 打的内联 hook 就是这样）。特征码的入口字节是通配，两边都能解析；
    # Python 的 DD_ENTRY 比较和偷取范围检查判它失效，运行时不做这两项（hook 引擎装 hook 时自己核对入口、和别人串联），
    # cpp_compare.py 把它列为「只在离线做的静态检查」，再用 --sig-fail 比较依据
    hit = res["ApplyHit"].hit
    p = Patcher(args.exe)
    p.write(hit, b"\xe9" + struct.pack("<i", 0x1000) + b"\x90\x90", "ApplyHit 入口改成 jmp rel32; nop; nop")
    samples["J_entry_hooked"] = p

    lines = []
    for name, patcher in samples.items():
        path = out / f"{name}.exe"
        patcher.save(path)
        lines.append(f"{name}.exe\n" + "".join(f"  {n}\n" for n in patcher.notes))
    (out / "README.txt").write_text("由 tests/layout_compare/make_mutants.py 生成，只读、不运行。\n\n" + "".join(lines), encoding="utf-8")
    sys.stdout.write("".join(lines))


if __name__ == "__main__":
    main()

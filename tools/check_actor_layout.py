"""离线核对 actor 句柄（core/src/game/actors.h 的 actor_layout）用到的结构偏移：不启动游戏，读 exe 的代码。

特征码只给出函数和全局的地址，偏移是 DDCore 里的常量，游戏换版本时要靠这里发现变化。逐项检查：
  - Hero / Monster 虚表（特征码同 signatures.inc）槽位 3、5、6 的实现：AsHero、id（Hero+0x137C、Monster+0x1320）、
    职业指针（Hero+0x1320、Monster+0x1468）；槽位 37（压力恢复）读写 +0x12FC
  - ActorMaxStat 唯一命中；治疗和伤害里 (Actor+0x768, [Actor+0x790], true) 调用它的形状都指向它
  - 名册：Campaign::Current 之后 +0x728（备用名册）、+0x20 / +0x28（m_HeroEntries）的分支只有一处
  - 战斗销毁时清空 Battle+0x38 / +0x40 的函数，AddMonsterToBattle 的 guid 计数（Raid+0x4B6C）
  - Battle::Create 先把 guid 计数 Battle+0x2D4 清零、再把状态 Battle+0x288 置 bs_create（战斗序号依赖这两个偏移，审核 B-B1）
  - 职业 id 在 ActorClass+8（hero_class_name_%s、str_monstername_%s 的参数）

用法（guest，工作目录 D:\\workplace）：
    python -B DD.ModLoader/tools/check_actor_layout.py [exe ...]
不带参数时检查 Steam 版和离线版，全部通过时退出码 0。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "knowledge" / "tools"))

from capstone import CS_ARCH_X86, CS_MODE_64, Cs  # noqa: E402
from dd_pe import Image  # noqa: E402

EXES = [
    ("steam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe"),
    ("nosteam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windowsnosteam\win64\Darkest.exe"),
]
ENGINE = Cs(CS_ARCH_X86, CS_MODE_64)


def regex(pattern):
    return re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pattern.split()), re.S)


def hits(image, pattern):
    start, end = image.sections[".text"]
    data = bytes(image.mem[start:end])
    return [m.start() + start for m in regex(pattern).finditer(data)]


def code(image, rva, count):
    out = []
    for ins in ENGINE.disasm(bytes(image.mem[rva:rva + 0x400]), rva):
        out.append(f"{ins.mnemonic} {ins.op_str}")
        if len(out) >= count or ins.mnemonic == "ret":
            break
    return out


def check_exe(tag, path):
    image = Image(path)
    print(f"== {tag} {path}\n   sha1={image.sha1}")
    failures = 0

    def expect(name, ok, detail=""):
        nonlocal failures
        print(f"  {'OK  ' if ok else 'FAIL'} {name}{('：' + detail) if detail else ''}")
        failures += 0 if ok else 1

    def vtable(pattern):
        found = hits(image, pattern)
        return (found[0] + 7 + image.i32(found[0] + 3)) if len(found) == 1 else None

    hero = vtable("48 8D 05 ?? ?? ?? ?? 49 89 06 48 8B 86 20 13 00 00 49 89 86")
    monster = vtable("48 8D 05 ?? ?? ?? ?? 48 89 06 89 9E 20 13 00 00 48 8D BE 28")
    expect("Hero / Monster 虚表特征码唯一命中", hero is not None and monster is not None)
    if hero is None or monster is None:
        return failures

    def slot(table, index, count=2):
        return code(image, image.u64(table + index * 8) - image.base, count)

    expect("Hero 槽位 3 返回 this", slot(hero, 3) == ["mov rax, rcx", "ret "])
    expect("Monster 槽位 3 返回 0", slot(monster, 3) == ["xor eax, eax", "ret "])
    expect("Hero 槽位 5：id 在 +0x137C", slot(hero, 5) == ["mov eax, dword ptr [rcx + 0x137c]", "ret "], str(slot(hero, 5)))
    expect("Monster 槽位 5：guid 在 +0x1320", slot(monster, 5) == ["mov eax, dword ptr [rcx + 0x1320]", "ret "], str(slot(monster, 5)))
    expect("Hero 槽位 6：职业指针在 +0x1320", slot(hero, 6) == ["mov rax, qword ptr [rcx + 0x1320]", "ret "], str(slot(hero, 6)))
    expect("Monster 槽位 6：职业指针在 +0x1468", slot(monster, 6) == ["mov rax, qword ptr [rcx + 0x1468]", "ret "], str(slot(monster, 6)))
    stress = slot(hero, 37, 60)
    expect("Hero 槽位 37（压力恢复）读写 +0x12FC", any("[rbx + 0x12fc]" in line for line in stress))

    max_stat = hits(image, "48 89 5C 24 08 48 89 7C 24 10 4C 8B 59 30 44 8B D2 48 8B 51 38 45 33 C9")
    expect("ActorMaxStat 唯一命中", len(max_stat) == 1, ", ".join(hex(h) for h in max_stat))
    calls = hits(image, "48 8D 8B 68 07 00 00 41 B0 01 8B 93 90 07 00 00 E8")
    targets = {c + 16 + 5 + image.i32(c + 17) for c in calls}
    expect("(Actor+0x768, [Actor+0x790], true) 的调用都指向 ActorMaxStat", len(calls) >= 3 and targets == set(max_stat),
        f"{len(calls)} 处")

    roster = hits(image, "E8 ?? ?? ?? ?? 48 8D A8 28 07 00 00 EB ?? E8 ?? ?? ?? ?? 48 8B E8 4C 8B 65 28 4C 2B 65 20")
    expect("名册分支：备用名册 +0x728，m_HeroEntries +0x20/+0x28", len(roster) == 1)
    battle = hits(image, "40 53 55 56 57 48 83 EC 28 48 8B 69 40 48 8B F1 48 8B 59 38 48 3B DD 74")
    expect("战斗销毁时清空 Battle+0x38/+0x40 的函数唯一", len(battle) == 1)
    counter = hits(image, "FF 87 6C 4B 00 00")
    expect("guid 计数 inc [Raid+0x4B6C] 唯一", len(counter) == 1)
    zero = hits(image, "41 89 8E D4 02 00 00")
    create = hits(image, "41 C7 86 88 02 00 00 01 00 00 00")
    same_fn = len(zero) == 1 and len(create) == 1 and image.enclosing_function(zero[0]) == image.enclosing_function(create[0])
    expect("Battle::Create：mov [r14+0x2D4], ecx（guid 计数清零）在 mov [r14+0x288], 1（bs_create）之前，同一个函数",
        same_fn and 0 < create[0] - zero[0] < 0x1000, f"清零 {', '.join(hex(h) for h in zero)}，置状态 {', '.join(hex(h) for h in create)}")
    embedded = hits(image, "48 8D ?? 98 48 00 00")
    expect("代码里有 lea [reg+0x4898]（Raid 内嵌的 Battle）", len(embedded) > 0, f"{len(embedded)} 处")

    hero_class = hits(image, "4D 8D 46 08 C6 44 24 30 00 48 8D 15")
    names = [image.cstr(h + 9 + 7 + image.i32(h + 12), 40) for h in hero_class]
    expect("HeroClass+8 是职业 id（hero_class_name_%s）", names == [b"hero_class_name_%s"], str(names))
    monster_class = hits(image, "48 8D 15 ?? ?? ?? ?? B8 08 00 00 00 4C 8D 04 38")
    names = [image.cstr(h + 7 + image.i32(h + 3), 40) for h in monster_class]
    expect("MonsterClass+8 是职业 id（str_monstername_%s）", names == [b"str_monstername_%s"], str(names))
    return failures


def main():
    exes = [(Path(p).parent.parent.name, p) for p in sys.argv[1:]] or EXES
    failures = sum(check_exe(tag, path) for tag, path in exes)
    print(f"结果: {'OK' if failures == 0 else 'FAIL'} （{failures} 个失败）")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())

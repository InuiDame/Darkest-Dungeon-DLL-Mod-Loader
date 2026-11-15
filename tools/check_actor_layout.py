"""离线核对 actor 句柄（core/src/game/actors.h 的 actor_layout）用到的结构偏移：不启动游戏，读 exe 的代码。

特征码只给出函数和全局的地址，偏移是 DDCore 里的常量，游戏换版本时要靠这里发现变化。逐项检查：
  - Hero / Monster 虚表（特征码同 signatures.inc）槽位 3、5、6 的实现：AsHero、id（Hero+0x137C、Monster+0x1320）、
    职业指针（Hero+0x1320、Monster+0x1468）；槽位 37（压力恢复）读写 +0x12FC
  - ActorMaxStat 唯一命中；治疗和伤害里 (Actor+0x768, [Actor+0x790], true) 调用它的形状都指向它
  - 名册：Campaign::Current 之后 +0x728（备用名册）、+0x20 / +0x28（m_HeroEntries）的分支只有一处
  - 战斗销毁时清空 Battle+0x38 / +0x40 的函数，AddMonsterToBattle 的 guid 计数（Raid+0x4B6C）
  - 队伍 Raid::Current+8 / +0x10 和怪物表 +0x48D0 / +0x48D8（F5 的 party_heroes、battle_monsters）
  - Battle::Create 先把 guid 计数 Battle+0x2D4 清零、再把状态 Battle+0x288 置 bs_create（战斗序号依赖这两个偏移，审核 B-B1）
  - 职业 id 在 ActorClass+8（hero_class_name_%s、str_monstername_%s 的参数）
  - P4a 的 combat 服务：四张枚举表的名字（从静态初始化函数还原）和来源表 +0x44/+0x46/+0x48 的标志与 core/src/combat/logic.cpp
    逐项一致；buff 表（Actor+0x460/+0x478、条目 0x418）、Instance 的默认值和规则、持续时间字段、Effect 字段的类型、
    技能表和 effect 列表的偏移（core/src/combat/combat.cpp 的 layout）

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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_sigs import load_sigs  # noqa: E402

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
    # F5 的枚举：按 .target 收集目标的函数（Steam 版 4C41F0）里，target_hero_random 用 Raid::Current+8/+0x10 的 vector<Actor*>
    # （队伍，kRaidHeroes），target_enemy_random 用 +0x48D0/+0x48D8（Battle+0x38/+0x40，怪物表），两处都先 mov r13, [Raid::Current]
    party = hits(image, "4C 8B 2D ?? ?? ?? ?? 4D 8B 7D 10 4D 2B 7D 08 49 C1 FF 03")
    enemies = hits(image, "4C 8B 2D ?? ?? ?? ?? 4D 8B BD D8 48 00 00 4D 2B BD D0 48 00 00 49 C1 FF 03")
    raid_global = sig_address(image, "RaidCurrent")
    bases = {at + 7 + image.i32(at + 3) for at in party + enemies}
    funcs = {image.enclosing_function(at) for at in party + enemies}
    expect("队伍在 Raid::Current+8/+0x10、怪物表在 +0x48D0/+0x48D8（同一个收集目标的函数，基址都是 Raid::Current）",
        len(party) == 1 and len(enemies) == 1 and raid_global is not None and bases == {raid_global} and len(funcs) == 1,
        f"队伍 {[hex(h) for h in party]}，怪物 {[hex(h) for h in enemies]}，函数 {[hex(f or 0) for f in funcs]}")

    hero_class = hits(image, "4D 8D 46 08 C6 44 24 30 00 48 8D 15")
    names = [image.cstr(h + 9 + 7 + image.i32(h + 12), 40) for h in hero_class]
    expect("HeroClass+8 是职业 id（hero_class_name_%s）", names == [b"hero_class_name_%s"], str(names))
    monster_class = hits(image, "48 8D 15 ?? ?? ?? ?? B8 08 00 00 00 4C 8D 04 38")
    names = [image.cstr(h + 7 + image.i32(h + 3), 40) for h in monster_class]
    expect("MonsterClass+8 是职业 id（str_monstername_%s）", names == [b"str_monstername_%s"], str(names))
    failures += check_combat(image, hero, monster, expect)
    return failures


# ---- P4a：combat 服务（core/src/combat/combat.cpp 的 layout、logic.cpp 的枚举表和来源标志）----

SIGS = {s["id"]: s for s in load_sigs()}


def sig_address(image, name):
    """signatures.inc 里一条 CODE/HOOK/GLOBAL 特征码在这个 exe 里解析出的地址，没有唯一命中时为 None。"""
    sig = SIGS[name]
    found = hits(image, sig["pattern"])
    if len(found) != 1:
        return None
    if sig["kind"] == "GLOBAL":
        at = found[0] + sig["capture"]
        return at + 4 + image.i32(at)
    return found[0]


def body(image, rva, limit=0x3000):
    """函数（从入口到 .pdata 记录的结尾）的反汇编文本列表。"""
    end = image.func_ends.get(rva, rva + limit)
    return [f"{ins.mnemonic} {ins.op_str}" for ins in ENGINE.disasm(bytes(image.mem[rva:end]), rva)]


def logic_tables():
    """logic.cpp 里的四张枚举表和来源标志（C++ 源码是唯一来源，这里按文本解析）。"""
    text = (Path(__file__).resolve().parents[1] / "core" / "src" / "combat" / "logic.cpp").read_text(encoding="utf-8")
    tables = {}
    for key, var in (("stat", "kStatNames"), ("source", "kSourceNames"), ("duration", "kDurationNames"), ("rule", "kRuleNames")):
        block = re.search(var + r"\[\] = \{(.*?)\};", text, re.S).group(1)
        tables[key] = re.findall(r'"([^"]*)"', block)
    block = re.search(r"kFlags\[\] = \{(.*?)\};", text, re.S).group(1)
    flags = []
    for line in block.splitlines():
        line = line.split("//")[0].strip().rstrip(",")
        if line:
            flags.append(0 if line == "0" else sum({"S": 1, "T": 2, "Y": 4}[part.strip()] for part in line.split("|")))
    return tables, flags


def find_string(image, text):
    data = bytes(image.mem)
    needle = text.encode() + b"\0"
    out, at = [], data.find(needle)
    while at != -1:
        if at == 0 or data[at - 1] == 0:
            out.append(at)
        at = data.find(needle, at + 1)
    return out


def init_tables(image, first, stride, count):
    """静态初始化函数里 strcpy_s(表 + i*stride, 名字) 和对表项的立即数写入，还原成名字列表和字节表。
    函数用「引用第一个名字的 lea」找，表的基址是第一个 strcpy 的目标。"""
    import struct
    start, end = image.sections[".text"]
    data = bytes(image.mem[start:end])
    targets = set(find_string(image, first))
    funcs = set()
    for i in range(0, len(data) - 7):
        if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8D and (data[i + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", data, i + 3)[0]
            if start + i + 7 + disp in targets:
                funcs.add(image.enclosing_function(start + i) or start + i)
    for func in sorted(funcs):
        regs, names, mem, base = {}, {}, {}, None
        for ins in ENGINE.disasm(bytes(image.mem[func:image.func_ends.get(func, func + 0x8000)]), func):
            ops = ins.op_str.split(", ")
            rip = re.search(r"\[rip \+ (0x[0-9a-f]+)\]", ins.op_str)
            addr = ins.address + ins.size + int(rip.group(1), 16) if rip else None
            if ins.mnemonic == "lea" and addr is not None:
                regs[ops[0]] = addr
            elif ins.mnemonic == "mov" and len(ops) == 2 and ops[1] in regs and not ops[0].startswith(("byte", "word", "dword", "qword")):
                regs[ops[0]] = regs[ops[1]]
            elif ins.mnemonic == "mov" and addr is not None and ops[0].startswith(("byte", "word", "dword")) and ops[1].startswith("0x") or (
                    ins.mnemonic == "mov" and addr is not None and ops[0].startswith(("byte", "word", "dword")) and ops[1].isdigit()):
                size = {"byte": 1, "word": 2, "dword": 4}[ops[0].split()[0]]
                value = int(ops[1], 0)
                for k in range(size):
                    mem[addr + k] = (value >> (8 * k)) & 0xFF
            elif ins.mnemonic == "call":
                dest, src = regs.get("rcx"), regs.get("r8")
                if dest is not None and src is not None:
                    name = image.cstr(src, 80).decode(errors="replace")
                    if base is None and name == first:
                        base = dest
                    if base is not None and 0 <= dest - base < stride * count and (dest - base) % stride == 0:
                        names[(dest - base) // stride] = name
                for reg in ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11"):
                    regs.pop(reg, None)
        if base is not None and len(names) == count:
            return [names[i] for i in range(count)], (lambda index, offset: mem.get(base + index * stride + offset))
    return None, None


def check_combat(image, hero, monster, expect):
    """失败由 expect 计数，这里返回 0。"""
    ok = expect

    tables, flags = logic_tables()
    for key, first, stride in (("stat", "status", 0x58), ("source", "bsrc_notspecified", 0x4C), ("duration", "combat_end", 0x48),
                               ("rule", "always", 0x2C)):
        want = tables[key]
        names, _ = init_tables(image, first, stride, len(want))
        got = [n[5:] if key == "source" and n.startswith("bsrc_") else n for n in names] if names else None
        diff = [i for i, (a, b) in enumerate(zip(got or [], want)) if a != b]
        ok(f"枚举表 {key}（{len(want)} 项）和 logic.cpp 一致", got == want, "没找到初始化函数" if got is None else f"不同的下标 {diff[:8]}")
    # A-S2：怪物受伤的免疫表（MonsterBlockReturn 命中位置 +52 的 lea），system.cpp 的 MonsterRule 按 20 种伤害类型读它
    block_ret = sig_address(image, "MonsterBlockReturn")
    if block_ret is not None:
        table = block_ret + 56 + image.i32(block_ret + 52)
        got = list(bytes(image.mem[table:table + 20]))
        ok("怪物免疫表（DEB7D0）：类型 0–7、9、10、13–19 为 1，8、11、12 为 0", got == [1] * 8 + [0, 1, 1, 0, 0] + [1] * 7, str(got))
    else:
        ok("MonsterBlockReturn 特征码唯一命中", False)
    _, read = init_tables(image, "bsrc_notspecified", 0x4C, len(tables["source"]))
    if read:
        game = [(1 if read(i, 0x44) else 0) | (2 if read(i, 0x46) else 0) | (4 if read(i, 0x48) else 0) for i in range(len(flags))]
        ok("来源表 +0x44/+0x46/+0x48 的标志和 logic.cpp 的 SourceFlags 一致", game == flags,
            f"不同的来源 {[i for i in range(len(flags)) if game[i] != flags[i]]}")
    else:
        ok("来源表的初始化函数", False, "没找到")

    slot13 = body(image, image.u64(hero + 13 * 8) - image.base, 0x400)
    ok("Hero 槽位 13（AddBuff）读写 Actor+0x478（下一个组 guid）、传 Actor+0x460（Buff::Group）",
        any("+ 0x478]" in line for line in slot13) and any("0x460" in line for line in slot13))
    erase = sig_address(image, "BuffGroupErase")
    ok("Buff::Group::erase 按 0x418 字节的条目前移", erase is not None and any("0x418" in line for line in body(image, erase, 0x400)))
    copy = sig_address(image, "BuffInstanceCopy")
    copy_body = body(image, copy, 0x800) if copy else []
    ok("Instance 复制构造复制 +0x158 的附加效果、+0x3B0、+0x408 的标志",
        all(any(off in line for line in copy_body) for off in ("0x158", "0x3b0]", "0x408]")))
    ctor = hits(image, "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 30 48 8B D9 C6 41 04 00")
    ctor_body = body(image, ctor[0], 0x400) if len(ctor) == 1 else []
    ok("Instance 构造：duration(+0x4C) = -1、duration_type(+0x50) = 9",
        "mov dword ptr [rcx + 0x4c], 0xffffffff" in ctor_body and any("[rbx + 0x50], 9" in line or "[rcx + 0x50], 9" in line for line in ctor_body))
    rule = sig_address(image, "BuffRuleActive")
    rule_body = body(image, rule, 0x1200) if rule else []
    ok("IsRuleActive 读 rule_type（Instance+0x5C）和 is_false_rule（+0x60）",
        any("+ 0x5c]" in line for line in rule_body) and any("+ 0x60]" in line for line in rule_body))
    upkeep = sig_address(image, "BuffDurationUpkeep")
    upkeep_body = body(image, upkeep, 0x800) if upkeep else []
    ok("BuffDurationUpkeep 按条目读写 duration（GroupEntry+0x54 = Instance+0x4C）、比较 duration_type（+0x58 = Instance+0x50）",
        any("+ 0x54]" in line for line in upkeep_body) and any("+ 0x58]" in line for line in upkeep_body))

    # Effect 的字段：4F02D0 里 lea r8,[rsi+偏移] 后调用的读取函数（36E940 float、36ED50 int）
    parse = sig_address(image, "ParseEffectFile")
    parse_body = list(ENGINE.disasm(bytes(image.mem[parse:image.func_ends.get(parse, parse + 0x5000)]), parse)) if parse else []
    readers = {}
    for i, ins in enumerate(parse_body):
        m = re.fullmatch(r"r8, \[rsi \+ (0x[0-9a-f]+)\]", ins.op_str) if ins.mnemonic == "lea" else None
        if m:
            for nxt in parse_body[i + 1:i + 5]:
                if nxt.mnemonic == "call":
                    readers.setdefault(int(m.group(1), 16), nxt.op_str)
                    break
    floats = [0x48, 0x4C, 0x50, 0xC8, 0xCC, 0x3B8]
    ints = [0xEC, 0x134, 0x138, 0x1CC]
    float_fn = {readers.get(o) for o in floats}
    int_fn = {readers.get(o) for o in ints}
    ok("Effect 的 float 字段（+0x48/+0x4C/+0x50/+0xC8/+0xCC/+0x3B8）用同一个读取函数，int 字段（+0xEC/+0x134/+0x138/+0x1CC）用另一个",
        len(float_fn) == 1 and len(int_fn) == 1 and None not in float_fn and None not in int_fn and float_fn != int_fn,
        f"float {float_fn} int {int_fn}")
    ok("Effect 的 .target 写在 +0x44、.swap_source_and_target 在 +0x3E0、.buff_duration_type 在 +0x430",
        any(f"{ins.mnemonic} {ins.op_str}" == "mov dword ptr [rsi + 0x44], ebx" for ins in parse_body) and
        any(ins.op_str.startswith("byte ptr [rsi + 0x3e0]") for ins in parse_body) and
        any(ins.op_str.startswith("dword ptr [rsi + 0x430]") for ins in parse_body))
    # 技能：英雄技能表 HeroClass+0xBD0、怪物 MonsterClass+0xBA8，效果列表 +0x3C8（技能加载 487D80 里 EffectLookup 之后追加）
    lookup = sig_address(image, "EffectLookup")
    callers = [h for h in hits(image, "E8 ?? ?? ?? ??") if h + 5 + image.i32(h + 1) == lookup] if lookup else []
    skill_load = {image.enclosing_function(c) for c in callers}
    has_3c8 = any(any("0x3c8]" in line for line in body(image, f, 0x6000)) for f in skill_load if f)
    ok("技能的 effect 列表在 +0x3C8（调用 EffectLookup 的技能加载函数里有 +0x3C8）", has_3c8, f"{len(skill_load)} 个调用 EffectLookup 的函数")
    # 技能表 HeroClass+0xBD0/+0xBE8、MonsterClass+0xBA8 和技能对象的大小（0x510/0x500）只有运行证据（P4-0C 探针 0B-11：
    # 55 次执行的技能指针都在这几张表里），这里没有离线核对
    return 0


def main():
    exes = [(Path(p).parent.parent.name, p) for p in sys.argv[1:]] or EXES
    failures = sum(check_exe(tag, path) for tag, path in exes)
    print(f"结果: {'OK' if failures == 0 else 'FAIL'} （{failures} 个失败）")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())

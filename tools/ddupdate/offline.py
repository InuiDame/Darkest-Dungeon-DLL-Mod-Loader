"""只能离线求值的依据（DD_OFFLINE）。layout.inc 里按函数名引用，一个检查可以支撑多个清单项，同一个 exe 上只求值一次。

这些检查来自原 tools/check_actor_layout.py，逻辑不变，只是常量改从清单取（不再解析 C++ 源码或在这里写死）。
写成离线检查的原因：要还原静态初始化里的表、按读取函数给字段分组、比较多处调用的目标、看指令的先后顺序，
超出了运行时依据「唯一命中 + 读值比较」的形状。新检查的写法：

    @check("名字", "一句话说明")
    def _(ctx):
        return ok(布尔值, "细节")

ctx 提供映像、清单常量（ctx.value）、特征码的解析结果（ctx.sig）和反汇编工具。

位置约束（U1 修复 FB，审核 A 备注 5）：只查「某个偏移出现过」的检查在结构中间插入字段时会误判通过（旧偏移正好是别的字段的
新偏移）。能绑到字段身份上的改成绑身份：
  effect_field_types  4F02D0 里 lea r8,[rsi+偏移] 后面那条 lea rdx 指向的键名必须是这个字段的键名（.chance、.dotPoison……），
                      读取函数按 float、int 分组（原来只看这个偏移有没有被同一类读取函数解析）
  effect_misc_fields  写 .target、.swap_source_and_target、.buff_duration_type 的指令，前面最近的键名必须是对应的键名
  skill_effects_offset  调用 EffectLookup 的函数里，偏移处要有一个 vector 的 push_back（cmp [R+偏移+0x10]、add [R+偏移+8], 8、
                      lea rcx,[R+偏移]，同一个基址寄存器，挨在一起），原来是这个函数里任何「偏移]」
  raid_battle_lea     lea [reg+偏移] 要在同一个函数里也读 Raid::Current 的函数里，原来是代码里任何一处
仍然只是「存在」的（写明弱点，没降成 NONE）：instance_copy_fields（复制构造里出现这几个偏移）、hero_stress_slot（槽 37
里出现压力偏移）。它们挂的清单项（InstAdditional、InstClearOnRule、BuffInstanceSize、ActorStress）都另有运行时依据，
这两个离线检查只是补充，不单独决定这些项成立。
"""
import re
import struct
from dataclasses import dataclass

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

from .image import compile_pattern

CHECKS = {}
_ENGINE = Cs(CS_ARCH_X86, CS_MODE_64)


@dataclass
class OfflineResult:
    ok: bool
    detail: str = ""


def check(name, summary):
    def register(fn):
        fn.summary = summary
        CHECKS[name] = fn
        return fn
    return register


def ok(condition, detail=""):
    return OfflineResult(bool(condition), detail)


def le(value, width):
    """常量写成特征码里的小端字节。"""
    return " ".join(f"{b:02X}" for b in (value % (1 << (8 * width))).to_bytes(width, "little"))


class Context:
    def __init__(self, image, manifest, sig_results):
        self.image = image
        self.manifest = manifest
        self.sig_results = sig_results
        self._cache = {}

    def value(self, name):
        return self.manifest.items[name].value

    def names(self, name):
        return self.manifest.items[name].names

    def values(self, name):
        return self.manifest.items[name].values

    def sig(self, name):
        res = self.sig_results.get(name)
        return res.resolved if res is not None and res.status == "ok" else None

    def sig_hit(self, name):
        res = self.sig_results.get(name)
        return res.hit if res is not None and res.status == "ok" else None

    def hits(self, pattern):
        regex, _ = compile_pattern(pattern)
        return self.image.scan_text(regex)

    def code(self, rva, count):
        out = []
        for ins in _ENGINE.disasm(self.image.bytes_at(rva, 0x400), rva):
            out.append(f"{ins.mnemonic} {ins.op_str}")
            if len(out) >= count or ins.mnemonic == "ret":
                break
        return out

    def insns(self, rva, limit=0x3000):
        """从入口到异常目录记录的结尾（没有记录时 limit 字节）的指令。"""
        end = self.image.func_ends.get(rva, rva + limit)
        return list(_ENGINE.disasm(self.image.bytes_at(rva, end - rva), rva))

    def body(self, rva, limit=0x3000):
        return [f"{i.mnemonic} {i.op_str}" for i in self.insns(rva, limit)]

    def vslot(self, vtable_sig, slot):
        table = self.sig(vtable_sig)
        return None if table is None else self.image.u64(table + slot * 8) - self.image.base

    def find_string(self, text):
        data = bytes(self.image.mem)
        needle = text.encode() + b"\0"
        out, at = [], data.find(needle)
        while at != -1:
            if at == 0 or data[at - 1] == 0:
                out.append(at)
            at = data.find(needle, at + 1)
        return out

    def init_tables(self, first, stride, count):
        """静态初始化函数里 strcpy_s(表 + i*stride, 名字) 和对表项的立即数写入，还原成名字列表和字节表。
        函数用「引用第一个名字的 lea」找，表的基址是第一个 strcpy 的目标。结果按参数缓存。"""
        key = ("init", first, stride, count)
        if key not in self._cache:
            self._cache[key] = _init_tables(self, first, stride, count)
        return self._cache[key]


def _init_tables(ctx, first, stride, count):
    image = ctx.image
    start, end = image.text_start, image.text_end
    data = image.bytes_at(start, end - start)
    targets = set(ctx.find_string(first))
    funcs = set()
    for i in range(0, len(data) - 7):
        if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8D and (data[i + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", data, i + 3)[0]
            if start + i + 7 + disp in targets:
                funcs.add(image.enclosing_function(start + i) or start + i)
    for func in sorted(funcs):
        regs, names, mem, base = {}, {}, {}, None
        for ins in _ENGINE.disasm(image.bytes_at(func, image.func_ends.get(func, func + 0x8000) - func), func):
            ops = ins.op_str.split(", ")
            rip = re.search(r"\[rip \+ (0x[0-9a-f]+)\]", ins.op_str)
            addr = ins.address + ins.size + int(rip.group(1), 16) if rip else None
            if ins.mnemonic == "lea" and addr is not None:
                regs[ops[0]] = addr
            elif ins.mnemonic == "mov" and len(ops) == 2 and ops[1] in regs and not ops[0].startswith(("byte", "word", "dword", "qword")):
                regs[ops[0]] = regs[ops[1]]
            elif ins.mnemonic == "mov" and addr is not None and ops[0].startswith(("byte", "word", "dword")) and (
                    ops[1].startswith("0x") or ops[1].isdigit()):
                size = {"byte": 1, "word": 2, "dword": 4}[ops[0].split()[0]]
                value = int(ops[1], 0)
                for k in range(size):
                    mem[addr + k] = (value >> (8 * k)) & 0xFF
            elif ins.mnemonic == "call":
                dest, src = regs.get("rcx"), regs.get("r8")
                if dest is not None and src is not None:
                    name = (image.cstr(src, 80) or b"").decode(errors="replace")
                    if base is None and name == first:
                        base = dest
                    if base is not None and 0 <= dest - base < stride * count and (dest - base) % stride == 0:
                        names[(dest - base) // stride] = name
                for reg in ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11"):
                    regs.pop(reg, None)
        if base is not None and len(names) == count:
            return [names[i] for i in range(count)], (lambda index, offset, b=base, m=mem: m.get(b + index * stride + offset))
    return None, None


# ---- actor 句柄 ----

@check("maxstat_calls", "(Actor+最大生命属性, [Actor+掩码], true) 调用属性合计函数的地方至少 3 处，目标都是 ActorMaxStat")
def _(ctx):
    target = ctx.sig("ActorMaxStat")
    calls = ctx.hits(f"48 8D 8B {le(ctx.value('ActorMaxHpStat'), 4)} 41 B0 01 8B 93 {le(ctx.value('ActorMaxHpMask'), 4)} E8")
    targets = {c + 16 + 5 + ctx.image.i32(c + 17) for c in calls}
    return ok(target is not None and len(calls) >= 3 and targets == {target}, f"{len(calls)} 处，目标 {[hex(t) for t in sorted(targets)]}")


@check("battle_create_order", "Battle::Create 里先把 guid 计数清零（mov [r14+计数], ecx）再置 bs_create（mov [r14+状态], 1），同一个函数")
def _(ctx):
    zero = ctx.hits(f"41 89 8E {le(ctx.value('BattleGuidCounter'), 4)}")
    create = ctx.hits(f"41 C7 86 {le(ctx.value('BattleState'), 4)} 01 00 00 00")
    image = ctx.image
    same = len(zero) == 1 and len(create) == 1 and image.enclosing_function(zero[0]) == image.enclosing_function(create[0])
    return ok(same and 0 < create[0] - zero[0] < 0x1000, f"清零 {[hex(h) for h in zero]}，置状态 {[hex(h) for h in create]}")


def _refs_global(ctx, func, target):
    """func 的指令里有没有 rip 相对地引用 target。"""
    image = ctx.image
    for b, e in image.function_ranges(func) or []:
        for ins in _ENGINE.disasm(image.bytes_at(b, e - b), b):
            m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
            if m:
                disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                if ins.address + ins.size + disp == target:
                    return True
    return False


@check("raid_battle_lea", "lea [reg+Raid 内嵌 Battle 的偏移]，所在函数也读 Raid::Current（U1 修复 FB 加了函数约束）")
def _(ctx):
    raid = ctx.sig("RaidCurrent")
    if raid is None:
        return ok(False, "特征码 RaidCurrent 失效")
    found = ctx.hits(f"48 8D ?? {le(ctx.value('RaidBattle'), 4)}")
    image = ctx.image
    funcs = sorted({f for f in (image.enclosing_function(h) for h in found) if f})
    good = [f for f in funcs if _refs_global(ctx, f, raid)]
    return ok(good, f"{len(found)} 处，其中在读 Raid::Current 的函数里的 {len(good)} 个函数")


@check("party_monster_targets", "按 .target 收集目标的函数（4C41F0）里，队伍读 Raid::Current+队伍偏移、怪物表读 Raid::Current+Battle+怪物表偏移，"
       "两处各唯一、在同一个函数、基址都是 Raid::Current")
def _(ctx):
    heroes = ctx.value("RaidHeroes")
    monsters = ctx.value("RaidBattle") + ctx.value("BattleMonsters")
    party = ctx.hits(f"4C 8B 2D ?? ?? ?? ?? 4D 8B 7D {le(heroes + 8, 1)} 4D 2B 7D {le(heroes, 1)} 49 C1 FF 03")
    enemies = ctx.hits(f"4C 8B 2D ?? ?? ?? ?? 4D 8B BD {le(monsters + 8, 4)} 4D 2B BD {le(monsters, 4)} 49 C1 FF 03")
    raid = ctx.sig("RaidCurrent")
    image = ctx.image
    bases = {at + 7 + image.i32(at + 3) for at in party + enemies}
    funcs = {image.enclosing_function(at) for at in party + enemies}
    good = len(party) == 1 and len(enemies) == 1 and raid is not None and bases == {raid} and len(funcs) == 1
    return ok(good, f"队伍 {[hex(h) for h in party]}，怪物 {[hex(h) for h in enemies]}，函数 {[hex(f or 0) for f in funcs]}")


@check("class_id_strings", "职业 id 在 ActorClass+偏移：hero_class_name_%s、str_monstername_%s 的格式化参数")
def _(ctx):
    offset = ctx.value("ClassId")
    image = ctx.image
    hero = ctx.hits(f"4D 8D 46 {le(offset, 1)} C6 44 24 30 00 48 8D 15")
    hero_names = [image.cstr(h + 9 + 7 + image.i32(h + 12), 40) for h in hero]
    monster = ctx.hits(f"48 8D 15 ?? ?? ?? ?? B8 {le(offset, 4)} 4C 8D 04 38")
    monster_names = [image.cstr(h + 7 + image.i32(h + 3), 40) for h in monster]
    return ok(hero_names == [b"hero_class_name_%s"] and monster_names == [b"str_monstername_%s"], f"{hero_names} {monster_names}")


# ---- 枚举表和来源标志（静态初始化里还原） ----
# 表在静态初始化函数里按 strcpy_s(表 + i*项大小, 名字) 填写。项大小和第一个名字是找表的参数，不是 DDCore 用的常量。
ENUM_TABLES = {
    "EnumStat": ("status", 0x58),
    "EnumSource": ("bsrc_notspecified", 0x4C),
    "EnumDuration": ("combat_end", 0x48),
    "EnumRule": ("always", 0x2C),
}


def _enum_check(item):
    def run(ctx):
        first, stride = ENUM_TABLES[item]
        want = ctx.names(item)
        names, _ = ctx.init_tables(first, stride, len(want))
        if names is None:
            return ok(False, "没找到初始化函数")
        got = [n[5:] if item == "EnumSource" and n.startswith("bsrc_") else n for n in names]
        diff = [i for i, (a, b) in enumerate(zip(got, want)) if a != b]
        return ok(got == want, f"{len(want)} 项，不同的下标 {diff[:8]}")
    return run


for _item, _name in (("EnumStat", "enum_stat"), ("EnumSource", "enum_source"), ("EnumDuration", "enum_duration"), ("EnumRule", "enum_rule")):
    check(_name, f"{_item} 的名字和顺序和游戏静态初始化里的表一致")(_enum_check(_item))

# 来源表的三个标志字节（项内偏移），DD_SOURCE_F_* 的依据
SOURCE_FLAG_OFFSETS = ((1, 0x44), (2, 0x46), (4, 0x48))


@check("source_flags", "来源表每项 +0x44/+0x46/+0x48 的标志和 SourceFlags 表一致")
def _(ctx):
    first, stride = ENUM_TABLES["EnumSource"]
    want = ctx.values("SourceFlags")
    _, read = ctx.init_tables(first, stride, len(ctx.names("EnumSource")))
    if read is None:
        return ok(False, "没找到来源表的初始化函数")
    game = [sum(bit for bit, offset in SOURCE_FLAG_OFFSETS if read(i, offset)) for i in range(len(want))]
    return ok(game == want, f"不同的来源 {[i for i in range(len(want)) if game[i] != want[i]]}")


# ---- 怪物免疫表（damage.final 的部分吸收，审核 A-S2） ----

@check("monster_immune_table", "MonsterBlockReturn 命中位置的 lea 指向的免疫表：类型 0–7、9、10、13–19 为 1，8、11、12 为 0")
def _(ctx):
    hit = ctx.sig_hit("MonsterBlockReturn")
    if hit is None:
        return ok(False, "特征码 MonsterBlockReturn 失效")
    image = ctx.image
    table = hit + ctx.value("MonsterBlockImmuneLeaNext") + image.i32(hit + ctx.value("MonsterBlockImmuneLeaDisp"))
    got = list(image.bytes_at(table, ctx.value("MonsterImmuneTypeCount")))
    return ok(got == [1] * 8 + [0, 1, 1, 0, 0] + [1] * 7, f"表 {table:X}：{got}")


# ---- Effect ----

# 字段 -> (键名, 读取函数的类别)。键名是 4F02D0 里 lea rdx,[rip+字符串] 的字符串（.effects.darkest 里的键）
EFFECT_KEYS = {
    "EffectChance": (".chance", "float"), "EffectStress": (".stress", "float"), "EffectStun": (".stun", "float"),
    "EffectHeal": (".heal", "float"), "EffectHealPercent": (".heal_percent", "float"),
    "EffectBuffAmount": (".buff_amount", "float"),
    "EffectCombatStatBuff": (".combat_stat_buff", "int"), "EffectDotPoison": (".dotPoison", "int"),
    "EffectDotBleed": (".dotBleed", "int"), "EffectDuration": (".duration", "int"),
}
# 直接写字段（不经读取函数）的：字段 -> (键名, 写入的宽度)
EFFECT_WRITES = {"EffectTarget": (".target", "dword"), "EffectSwap": (".swap_source_and_target", "byte"),
                 "EffectBuffDurationType": (".buff_duration_type", "dword")}
KEY_LOOKBACK = 80


def _rip_target(ins):
    m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
    if not m:
        return None
    return ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)


def _effect_parse(ctx):
    """4F02D0 的指令和每个 lea r8,[rsi+偏移] 解析：[(偏移, 键名, 读取函数)]。"""
    if "effect_parse" not in ctx._cache:
        parse = ctx.sig("ParseEffectFile")
        insns = ctx.insns(parse, 0x5000) if parse is not None else []
        fields = []
        for i, ins in enumerate(insns):
            m = re.fullmatch(r"r8, \[rsi \+ (0x[0-9a-f]+)\]", ins.op_str) if ins.mnemonic == "lea" else None
            if not m:
                continue
            key, reader = None, None
            for nxt in insns[i + 1:i + 6]:
                if key is None and nxt.mnemonic == "lea" and nxt.op_str.startswith("rdx, [rip"):
                    raw = ctx.image.cstr(_rip_target(nxt), 80)
                    key = raw.decode(errors="replace") if raw else None
                if nxt.mnemonic == "call":
                    reader = nxt.op_str
                    break
            fields.append((int(m.group(1), 16), key, reader))
        ctx._cache["effect_parse"] = (fields, insns)
    return ctx._cache["effect_parse"]


@check("effect_field_types", "4F02D0 里解析 Effect 字段的 lea r8,[rsi+偏移] 后面的键名是这个字段的键名（.chance、.dotPoison……），"
       "float 字段用同一个读取函数（Steam 版 36E940），int 字段用另一个（36ED50）")
def _(ctx):
    fields, _ = _effect_parse(ctx)
    bad, readers = [], {"float": set(), "int": set()}
    for item, (key, kind) in EFFECT_KEYS.items():
        offset = ctx.value(item)
        here = [(k, r) for o, k, r in fields if o == offset]
        if len(here) != 1 or here[0][0] != key:
            bad.append(f"{item}（{offset:#x}）键名 {[k for k, _r in here]}，应为 {key}")
            continue
        readers[kind].add(here[0][1])
    good = not bad and len(readers["float"]) == 1 and len(readers["int"]) == 1 and readers["float"] != readers["int"] \
        and None not in readers["float"] | readers["int"]
    return ok(good, "；".join(bad) or f"float {sorted(readers['float'])} int {sorted(readers['int'])}")


@check("effect_misc_fields", "4F02D0 里写 .target（dword）、.swap_source_and_target（byte）、.buff_duration_type（dword）的指令，"
       "前面最近的键名是对应的键名")
def _(ctx):
    _, insns = _effect_parse(ctx)
    bad = []
    for item, (key, width) in EFFECT_WRITES.items():
        needle = f"{width} ptr [rsi + {ctx.value(item):#x}],"
        writes = [i for i, ins in enumerate(insns) if ins.mnemonic == "mov" and ins.op_str.startswith(needle)]
        good = 0
        for i in writes:
            nearest = None
            for prev in reversed(insns[max(0, i - KEY_LOOKBACK):i]):
                if prev.mnemonic == "lea" and "rip" in prev.op_str:
                    raw = ctx.image.cstr(_rip_target(prev), 80)
                    if raw and raw.startswith(b"."):
                        nearest = raw.decode(errors="replace")
                        break
            good += nearest == key
        if not writes or not good:
            bad.append(f"{item}（{ctx.value(item):#x}）写入 {len(writes)} 处，前面最近的键名是 {key} 的 {good} 处")
    return ok(not bad, "；".join(bad) or "三个字段的写入都在对应的键名之后")


@check("skill_effects_offset", "调用 EffectLookup 的技能加载函数（487D80）里，技能的 effect 列表偏移处是一个 vector 的 push_back："
       "cmp [R+偏移+0x10]、add qword [R+偏移+8], 8、lea rcx,[R+偏移] 用同一个基址寄存器、在 12 条指令以内")
def _(ctx):
    lookup = ctx.sig("EffectLookup")
    if lookup is None:
        return ok(False, "特征码 EffectLookup 失效")
    image = ctx.image
    callers = [h for h in ctx.hits("E8 ?? ?? ?? ??") if h + 5 + image.i32(h + 1) == lookup]
    funcs = sorted({f for f in (image.enclosing_function(c) for c in callers) if f})
    offset = ctx.value("SkillEffects")
    found = []
    for f in funcs:
        insns = ctx.insns(f, 0x6000)
        for i, ins in enumerate(insns):
            m = re.fullmatch(rf"rcx, \[(\w+) \+ {offset:#x}\]", ins.op_str) if ins.mnemonic == "lea" else None
            if not m:
                continue
            reg = m.group(1)
            window = [f"{x.mnemonic} {x.op_str}" for x in insns[max(0, i - 12):i]]
            has_end = any(t.startswith("cmp ") and t.endswith(f"qword ptr [{reg} + {offset + 0x10:#x}]") for t in window)
            has_last = f"add qword ptr [{reg} + {offset + 8:#x}], 8" in window
            if has_end and has_last:
                found.append(f"{ins.address:X}")
    return ok(found, f"{len(funcs)} 个调用 EffectLookup 的函数，push_back 在 {found}")


# ---- Buff ----

@check("instance_copy_fields", "Buff::Instance 复制构造复制 +附加效果、+规则清除列表和 +0x408 的标志（Instance 到 0x410 为止）")
def _(ctx):
    copy = ctx.sig("BuffInstanceCopy")
    body = ctx.body(copy, 0x800) if copy is not None else []
    wanted = (f"{ctx.value('InstAdditional'):#x}", f"{ctx.value('InstClearOnRule'):#x}]", f"{ctx.value('BuffInstanceSize') - 8:#x}]")
    return ok(all(any(w in line for line in body) for w in wanted), f"{wanted}")


@check("instance_ctor_defaults", "Buff::Instance 构造：duration = -1、duration_type = 9")
def _(ctx):
    ctor = ctx.sig("BuffInstanceCtor")
    body = ctx.body(ctor, 0x400) if ctor is not None else []
    duration = f"mov dword ptr [rcx + {ctx.value('InstDuration'):#x}], 0xffffffff" in body
    dtype = ctx.value("InstDurationType")
    kind = any(f"[rbx + {dtype:#x}], 9" in line or f"[rcx + {dtype:#x}], 9" in line for line in body)
    return ok(duration and kind, f"duration {duration} duration_type {kind}")


@check("hero_stress_slot", "Hero 虚表槽 37（压力恢复）读写 Actor+压力")
def _(ctx):
    fn = ctx.vslot("HeroVtable", 37)
    lines = ctx.code(fn, 60) if fn is not None else []
    return ok(any(f"[rbx + {ctx.value('ActorStress'):#x}]" in line for line in lines))

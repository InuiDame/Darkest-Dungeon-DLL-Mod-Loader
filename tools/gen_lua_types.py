"""从 DDCore 的主题表生成 LuaLS 的主题注解 sdk/lua/dd_topics.lua。

主题表来自 topic_dump 插件（tests/plugins/topic_dump.cpp）在游戏里写出的 topics.json：

    python -B DD.ModLoader/tools/rig.py deploy topics
    python -B DD.ModLoader/tools/rig.py run --seconds 35
    python -B DD.ModLoader/tools/gen_lua_types.py --from-rig      # 复制到 sdk/lua/topics.json 再生成

不带 --from-rig 时直接用仓库里的 sdk/lua/topics.json。--check 只比较，生成结果和现有文件不同时退出码为 1。
生成的内容：主题名的别名 dd.Topic；每个主题的字段类 dd.fields.<主题>，负载 dd.ev.<主题>，wait_event 的快照
dd.snap.<主题>，filter 表 dd.filter.<主题>，选项 dd.opts.<主题>；dd.on、dd.once、dd.wait_event 的按主题重载。
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / "sdk" / "lua" / "topics.json"
OUTPUT = ROOT / "sdk" / "lua" / "dd_topics.lua"
RIG = Path(r"D:\Program Files (x86)\Steam\steamapps\common") / os.environ.get("DD_RIG_NAME", "DarkestDungeon_Dev")
RIG_DUMP = RIG / "DDModLoader" / "log" / "topics.json"

# 指向 Hero / Monster 的指针字段：DDLua 把它们转成 actor 句柄（不是 actor 的指针读出 nil）。
# 其余指针字段（skill、effect、raid、inventory……）在 Lua 里恒为 nil
ACTOR_FIELDS = {"target", "attacker", "performer", "healer", "source", "actor", "hero"}

# 语义主题和应用主题的说明（native.* 和 query.* 按类名统一说明）。依据 sdk/include/dd/dd_api.h 和
# docs/07-event-coverage.md；没写的主题只有字段注解
DESCRIPTIONS = {
    "damage.pre": "伤害结算前（Hero/Monster 受到伤害）。可写 damage，可取消：取消后伤害按 0 结算，不扣血、不进死门",
    "damage.post": "伤害结算后。damage 是实际传给游戏的伤害（被取消时为 0），hp_after 是结算后的生命",
    "effect.pre": "Effect 应用到一个目标之前。可取消：跳过这一个目标。performer、target 是实际生效的双方",
    "effect.post": "Effect 应用到一个目标之后",
    "skill.pre": "技能执行前。skill、target 可写但都是指针，Lua 不能写；Lua 只能读",
    "skill.post": "技能执行后",
    "hit.pre": "命中结算前。可写 hit（false 走未命中路径）、crit、damage",
    "hit.post": "命中结算后，hit、crit、damage 是最终值",
    "battle.round.start": "回合开始（含第一回合）",
    "battle.round.end": "回合结束前；战斗最后一回合没有，用 battle.end",
    "battle.turn.start": "英雄回合开始前（只有英雄回合）",
    "battle.turn.end": "英雄回合的 Combat_StartTurn 返回后（不是行动结束）",
    "stress.pre": "压力变化前（正数加压力，负数减压力）。可写 amount",
    "stress.post": "压力变化后。amount 是传给游戏的值，applied 是游戏返回的实际变化",
    "heal.pre": "治疗前。可写 amount",
    "heal.post": "治疗后。amount 是传给游戏的值，applied 是实际治疗量",
    "app.tick": "每帧一次（帧边界）",
    "app.state": "游戏状态机切换（from/to 是状态号，*_name 是状态名）",
    "campaign.load": "进入存档后。Lua Mod 在 main.lua 里订阅能收到这一次",
    "campaign.unload": "退出存档。按存档加载的 Lua Mod 收不到：DDLua 在这之前已经拆除了它们",
    "battle.start": "战斗创建（BattleCreatedEvent）。从存档恢复的战斗（撤退后重进同一格等）游戏不派发它 [静态]，见 03 的「战斗序号和 battle.start」",
    "battle.end": "战斗销毁（BattleDestroyedEvent）。这时怪物已移出战斗，怪物句柄已失效",
    "actor.deathsdoor": "英雄进入死门",
    "actor.heartattack": "心脏病发作",
    "actor.death": "英雄或怪物死亡（is_monster 区分；英雄看 hero_id，怪物看 monster_guid）",
    "actor.affliction": "英雄崩溃或美德",
    "raid.start": "开始探索。在加载线程上发生，一般是延迟事件（deferred 为 true，raid 为 nil）",
    "raid.finish": "探索结算",
    "raid.step": "队伍在地牢里走一步",
    "raid.torch": "火把亮度变化",
    "town.enter": "开始一次城镇访问（从地牢回城等；读城镇存档不经过这里）",
    "town.event": "城镇事件",
    "town.building": "建筑状态变化",
    "town.activity": "打开城镇活动",
    "town.activity.finish": "城镇活动结束",
    "hero.recruit": "招募英雄",
    "hero.dismiss": "解雇英雄",
    "hero.quirk": "英雄获得怪癖",
    "hero.skill": "英雄技能升级",
    "inventory.add": "物品加入库存。读档时有一部分在加载线程上派发，是延迟事件（07 覆盖表 172 次里 10 次），其余在主线程上",
    "inventory.modified": "库存变化（静态上没找到派发点，可能从不派发）",
    "inventory.premerge": "库存合并前",
    "inventory.postmerge": "库存合并后",
    "estate.currency": "庄园货币变化",
}

LUA_TYPES = {"int": "integer", "float": "number", "bool": "boolean", "string": "string"}


def field_type(topic, field):
    if field["type"] == "pointer":
        return "dd.Actor?" if field["name"] in ACTOR_FIELDS else "nil"
    return LUA_TYPES.get(field["type"], "any")


def field_note(topic, field):
    notes = []
    if field["type"] == "pointer":
        if field["name"] in ACTOR_FIELDS:
            notes.append("Hero/Monster 转成 actor 句柄，其他指针和延迟事件为 nil")
        else:
            notes.append("游戏指针，Lua 里恒为 nil")
    if field["write"] and topic["writable"]:
        if field["type"] in ("pointer", "string"):
            notes.append("SDK 可写，Lua 不能写")
        else:
            notes.append("可写")
    return "，".join(notes)


def describe(topic):
    name = topic["name"]
    if name in DESCRIPTIONS:
        text = DESCRIPTIONS[name]
    elif topic["kind"] == "native":
        text = f"游戏事件 {name[len('native.'):]}（只读）"
    elif topic["kind"] == "query":
        text = f"游戏询问 {name[len('query.'):]}：可写结果字段，不能取消"
    else:
        text = f"{topic['kind']} 主题"
    flags = []
    if topic["writable"] and name not in DESCRIPTIONS and topic["kind"] != "query":
        flags.append("可写")
    if topic["cancellable"] and name not in DESCRIPTIONS:
        flags.append("可取消")
    if not topic["available"]:
        flags.append("不可用：" + (topic.get("reason") or "原因未知"))
    return text + ("（" + "，".join(flags) + "）" if flags else "")


def one_line(text):
    return " ".join(text.split())


def generate(dump):
    topics = dump["topics"]
    out = []
    w = out.append
    w("---@meta dd_topics")
    w("-- 本文件由 tools/gen_lua_types.py 从 sdk/lua/topics.json 生成，不要手改。")
    w(f"-- 主题表：DDCore {dump.get('core_version', '?')}，Darkest.exe SHA1 {dump.get('exe_sha1', '?')}（Steam 版），"
      f"主题 {len(topics)} 个。")
    w("-- 离线版缺特征码时部分主题不可用，dd.on 返回 nil 和原因；运行时以 dd.on 的返回值为准。")
    w("")
    w("---主题名。native 插件自定义的主题也可以订阅，所以参数类型是 dd.Topic|string。")
    w("---@alias dd.Topic")
    for topic in topics:
        w(f"---| \"{topic['name']}\" # {one_line(describe(topic))}")
    w("")
    for topic in topics:
        name = topic["name"]
        w(f"-- ---- {name}")
        w(f"---{one_line(describe(topic))}")
        w(f"---@class dd.fields.{name}")
        for field in topic["fields"]:
            note = field_note(topic, field)
            w(f"---@field {field['name']} {field_type(topic, field)}" + (f" {note}" if note else ""))
        w(f"---{name} 的事件负载，只在回调执行期间有效")
        w(f"---@class dd.ev.{name} : dd.Event, dd.fields.{name}")
        w(f"---{name} 的 dd.wait_event 快照")
        w(f"---@class dd.snap.{name} : dd.Snapshot, dd.fields.{name}")
        w(f"---{name} 的 filter：字段等值比较，指针字段用 actor 句柄")
        w(f"---@class dd.filter.{name}")
        for field in topic["fields"]:
            value = "dd.Actor" if field["type"] == "pointer" else LUA_TYPES.get(field["type"], "any")
            w(f"---@field {field['name']}? {value}")
        w(f"---@class dd.opts.{name} : dd.OnOptions")
        w(f"---@field filter? dd.filter.{name}")
        w("")

    def overloads(make):
        for topic in topics:
            w("---@overload " + make(topic["name"]))

    w("---订阅主题，回调在主线程执行，每次调用有执行预算。主题不存在或不可用、actor 作用域的句柄已失效时")
    w("---返回 nil 和原因（并写日志）；参数或选项写错直接报错。订阅从下一次派发开始生效。")
    w("---回调出错写日志（带 traceback），连续出错 3 次（DDLua.cfg 的 max_failures）或超出执行预算时停用。")
    w("---@param topic dd.Topic|string")
    w("---@param fn fun(ev: dd.Event)")
    w("---@param opts? dd.OnOptions")
    w("---@return dd.Subscription? sub")
    w("---@return string? reason")
    overloads(lambda n: f"fun(topic: \"{n}\", fn: fun(ev: dd.ev.{n}), opts?: dd.opts.{n}): dd.Subscription?, string?")
    w("function dd.on(topic, fn, opts) end")
    w("")
    w("---和 dd.on 相同，回调调用一次后自动撤销（在调用回调之前撤销）。")
    w("---@param topic dd.Topic|string")
    w("---@param fn fun(ev: dd.Event)")
    w("---@param opts? dd.OnOptions")
    w("---@return dd.Subscription? sub")
    w("---@return string? reason")
    overloads(lambda n: f"fun(topic: \"{n}\", fn: fun(ev: dd.ev.{n}), opts?: dd.opts.{n}): dd.Subscription?, string?")
    w("function dd.once(topic, fn, opts) end")
    w("")
    w("---挂起当前协程，等下一次匹配 filter 的事件，下一帧把字段快照交给协程（不是活的负载，改 .pre 要用 dd.on）。")
    w("---超时（秒）返回 nil；主题不存在或不可用时立即返回 nil 和原因。只能在 dd.spawn 的协程里直接调用。")
    w("---@async")
    w("---@param topic dd.Topic|string")
    w("---@param filter? table<string, integer|number|boolean|string|dd.Actor>")
    w("---@param timeout? number")
    w("---@return dd.Snapshot? snapshot")
    w("---@return string? reason")
    overloads(lambda n: f"async fun(topic: \"{n}\", filter?: dd.filter.{n}, timeout?: number): dd.snap.{n}?, string?")
    w("function dd.wait_event(topic, filter, timeout) end")
    w("")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from-rig", action="store_true", help=f"先把 {RIG_DUMP} 复制到 sdk/lua/topics.json")
    parser.add_argument("--dump", type=Path, help="改用指定的 topics.json（不复制）")
    parser.add_argument("--check", action="store_true", help="只检查 dd_topics.lua 是否最新")
    args = parser.parse_args()
    source = SNAPSHOT
    if args.from_rig:
        if not RIG_DUMP.is_file():
            sys.exit(f"没有 {RIG_DUMP}：先 rig.py deploy topics 再 run")
        shutil.copyfile(RIG_DUMP, SNAPSHOT)
        print(f"已复制 {RIG_DUMP} -> {SNAPSHOT}")
    elif args.dump:
        source = args.dump
    dump = json.loads(source.read_text(encoding="utf-8"))
    text = generate(dump)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT} 不是最新的，重新运行 gen_lua_types.py")
            sys.exit(1)
        print(f"{OUTPUT} 是最新的")
        return
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    fields = sum(len(t["fields"]) for t in dump["topics"])
    print(f"已生成 {OUTPUT}：主题 {len(dump['topics'])} 个，字段 {fields} 个，{len(text.splitlines())} 行")


if __name__ == "__main__":
    main()

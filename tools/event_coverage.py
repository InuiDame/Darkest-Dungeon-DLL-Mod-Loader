"""从 test_events 插件的日志生成 docs/07-event-coverage.md 的覆盖表。

用法（guest，工作目录 D:\\workplace）：
    python -B DD.ModLoader/tools/event_coverage.py [--log 路径] [--out 路径]

默认读取测试目录的 DDModLoader\\log\\test_events.log，输出到 DD.ModLoader/docs/07-event-coverage.md。
日志里的“主题 ...”行给出全部主题和可用性；“首次收到 ...”行给出收到与否、线程和字段值；
“计数汇总”行给出次数和是否出现延迟派发。未覆盖的原因写在 NOTES 里，脚本重新生成时保留。
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG = Path(r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon_Dev\DDModLoader\log\test_events.log")
DEFAULT_OUT = ROOT / "docs" / "07-event-coverage.md"

# 主题不在日志的情况和备注。key 是主题名，value 是表格备注
NOTES = {
    "native.AwardAchievementEvent": "没有 Listener<T const> 类型描述符（游戏里没有派发点），无法用 __RTDynamicCast 伪造，不可用",
    "query.TownGoldInterestBuffQuery": "游戏没有 Listener 类型描述符（没有派发点），不可用",
    "native.OptionsSavedEvent": "没有游戏监听者，伪造 COL 和虚表注册（见 knowledge/dd1/events.md）",
    "native.PositiveQuirkTreatedEvent": "没有游戏监听者，伪造虚表注册",
    "native.PostOnStartTownVisitEvent": "没有游戏监听者，伪造虚表注册",
    "native.PrestigeUpdatedEvent": "没有游戏监听者，伪造虚表注册",
    "native.PlayFabConnectionLostEvent": "没有游戏监听者，伪造虚表注册",
    "native.PlayFabLocalizedDataAvailableEvent": "没有游戏监听者，伪造虚表注册",
    "native.PlayFabAchievementsUpdatedEvent": "没有游戏监听者，伪造虚表注册",
    "inventory.modified": "静态上未找到 InventoryModifiedEvent 的派发点，可能从不派发",
    "battle.turn.start": "只在英雄回合（Combat_StartTurn 调用点）触发，只读。跳过回合暂不提供：要走 TurnResult=2 的路径，没有实测",
    "town.enter": "开始一次城镇访问时触发（从地牢回城等），读城镇存档不经过这个流程",
    "battle.turn.end": "Combat_StartTurn 返回后触发，不是英雄行动结束",
    "battle.round.start": "hook 在 UpdateRoundBuffs 的调用者 5A64B0，覆盖第一回合",
    "battle.round.end": "hook 在 5A64B0 入口，回合号大于 0 时触发；战斗最后一回合没有 end，用 battle.end 判定",
}

# 超出 P2 范围的主题，表格里单列
OTHER_PHASES = [
    ("buff.add", "P4"),
    ("buff.remove", "P4"),
    ("buff.tick", "P4"),
    ("campaign.save", "P4"),
    ("raid.curio", "P5"),
    ("raid.trap", "P5"),
    ("raid.camp", "P5"),
    ("spine.event", "P6"),
    ("spine.start", "P6"),
    ("spine.complete", "P6"),
]


def parse_log(path):
    topics = {}  # name -> info
    order = []
    if not path.exists():
        return topics, order
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    topic_line = re.compile(
        r"主题 (\S+) kind=(\w+) 字段数=(\d+) writable=(\w+) cancellable=(\w+) available=(\w+)(?: 原因: (.*))?"
    )
    first_line = re.compile(
        r"首次收到 \S+ topic=(\S+) 线程=(\d+) 派发线程=(\d+) 主线程=(\d+) 延迟=(\S+) 标志=(\S+) 字段:(.*)"
    )
    summary_line = re.compile(r"\] (\S+) count=(\d+) deferred=(\d+) first_thread=(\d+)")
    for line in text.splitlines():
        match = topic_line.search(line)
        if match:
            name = match.group(1)
            if name not in topics:
                order.append(name)
            topics[name] = {
                "kind": match.group(2),
                "fields": int(match.group(3)),
                "writable": match.group(4) == "true",
                "cancellable": match.group(5) == "true",
                "available": match.group(6) == "true",
                "reason": match.group(7) or "",
                "seen": False,
                "thread": "",
                "count": 0,
                "deferred": 0,
                "fields_sample": "",
            }
            continue
        match = first_line.search(line)
        if match:
            name = match.group(1)
            if name not in topics:
                continue
            info = topics[name]
            info["seen"] = True
            main = match.group(4)
            tid = match.group(3)
            info["thread"] = "主线程" if tid == main else f"线程 {tid}（非主线程）"
            info["fields_sample"] = match.group(7).strip()
            continue
        match = summary_line.search(line)
        if match:
            name = match.group(1)
            if name in topics:
                topics[name]["count"] = max(topics[name]["count"], int(match.group(2)))
                topics[name]["deferred"] = max(topics[name]["deferred"], int(match.group(3)))
    return topics, order


def cell(text):
    return text.replace("|", "\\|")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", default=str(DEFAULT_LOG))
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    args = parser.parse_args()
    log = Path(args.log)
    topics, order = parse_log(log)
    out = []
    out.append("# 事件覆盖表\n")
    out.append("由 `tools/event_coverage.py` 从 `test_events` 的日志生成，字段含义见 `knowledge/dd1/events.md`。")
    out.append("“运行中收到”表示本次日志里该主题的回调至少被调用过一次；没收到不代表主题不可用，只是这次流程没有触发。")
    out.append("日志来源：" + (log.name + "（" + str(log) + "）" if log.exists() else "（未找到日志）") + "\n")

    def section(title, predicate):
        names = [n for n in order if predicate(n)]
        out.append(f"## {title}\n")
        out.append("| 主题 | 可用 | 字段数 | 可写/可取消 | 运行中收到 | 派发线程 | 次数/延迟 | 备注 |")
        out.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for name in names:
            info = topics[name]
            writable = "可写" if info["writable"] else "只读"
            if info["cancellable"]:
                writable += "、可取消"
            seen = "是" if info["seen"] or info["count"] else "否"
            thread = info["thread"] if info["seen"] else ""
            count = f"{info['count']}/{info['deferred']}" if info["count"] else ""
            note = NOTES.get(name, "")
            if not info["available"]:
                note = (note + "；" if note else "") + "不可用：" + info["reason"]
            out.append(
                f"| `{name}` | {'是' if info['available'] else '否'} | {info['fields']} | {writable} | {seen} | {thread} | {count} | {cell(note)} |"
            )
        out.append("")

    section("原生事件（native.*）", lambda n: n.startswith("native."))
    section("Query（query.*）", lambda n: n.startswith("query."))
    section("语义主题", lambda n: not n.startswith("native.") and not n.startswith("query.") and not n.startswith("custom."))

    out.append("## 后续阶段的主题（P2 未实现）\n")
    out.append("| 主题 | 所属阶段 |")
    out.append("| --- | --- |")
    for name, phase in OTHER_PHASES:
        out.append(f"| `{name}` | {phase} |")
    out.append("")

    Path(args.out).write_text("\n".join(out), encoding="utf-8")
    print(f"已写入 {args.out}，主题 {len(order)} 个，收到 {sum(1 for n in order if topics[n]['seen'] or topics[n]['count'])} 个")


if __name__ == "__main__":
    main()

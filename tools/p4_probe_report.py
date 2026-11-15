"""P4-0C 探针日志分析：读 probe_p4.log（和同目录的 DDCore.log、.rig-log.json），按 P4-0A、P4-0B 的待探针核实清单
逐项输出「观察到什么、和预期是否一致、没有触发」。在 guest 上用 python -B 运行。

  python -B DD.ModLoader/tools/p4_probe_report.py --log-dir DD.ModLoader/build/rig_logs/<名字>
  python -B DD.ModLoader/tools/p4_probe_report.py --log-dir ... --game-log <游戏自己的日志>   另查游戏日志里的错误文字（0A-7）
  （游戏日志是测试目录根下的 app.log，下次启动时改名 app.log.bak；复制到 --log-dir 里时自动读取）
  python -B DD.ModLoader/tools/p4_probe_report.py --selftest     用自造的假日志（写到 build/p4_probe_fake）自测

判定：一致 / 不一致 / 部分（有一部分没覆盖或没触发）/ 没有触发 / 不做。
退出码：0 没有「不一致」，1 有「不一致」或主动测试出过异常，2 日志缺失或不是 probe4 方案的日志。
探针的日志格式见 tests/plugins/probe_p4.cpp：每行 [t<线程> 主|副 f<帧> <秒>s] <标签> k=v ...
"""
import argparse
import json
import re
import shutil
import sys
from collections import Counter, defaultdict
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
FAKE_DIR = PROJECT / "build" / "p4_probe_fake"

OK, BAD, PART, NONE, SKIP = "一致", "不一致", "部分", "没有触发", "不做"

LINE = re.compile(r"^\ufeff?\[(?P<clock>\d\d:\d\d:\d\d\.\d{3})\] \[(?P<level>\w+)\] (?P<msg>.*)$")
MSG = re.compile(r"^\[t(?P<tid>\d+) (?P<main>主|副) f(?P<frame>\d+) (?P<secs>[\d.]+)s\] (?P<tag>\S+)(?: (?P<body>.*))?$")
KV = re.compile(r'(\w+)=("([^"]*)"|\S+)')
RUN_ID = re.compile(r"运行标识 (\S+?)[，,\s]")

# 已知函数（RVA → 名字），用来读调用者
KNOWN = {
    0x480540: "UpdateBuffsInternal", 0x479BA0: "479BA0 收集生效 buff", 0x486CC0: "486CC0 remove_if_not_active 谓词",
    0x479470: "479470 技能附加效果收集", 0x4C5050: "EffectApplyInternal", 0x47CBA0: "47CBA0", 0x4CA390: "4CA390 effect 加 buff",
    0x5BC890: "5BC890", 0x587F90: "Roster::OnStartTownVisit", 0x47AAD0: "BuffDurationUpkeep", 0x5CE600: "Hero 槽位14",
    0x5FD280: "Monster 槽位14", 0x5C4C10: "灼烧衰减", 0x4849F0: "ResetStats", 0x5978E0: "城镇事件 5978E0",
    0x765D40: "UpdateBattleState", 0x775460: "UpdateRoundBuffDurations", 0x5A6670: "Battle::UpdateRoundBuffs",
    0x5CCD10: "Hero::OnBattleComplete", 0x5B66F0: "撤退 5B66F0", 0x586730: "Roster::OnRaidFinish", 0x535610: "建筑活动结束 535610",
    0x60A350: "60A350 DOT 和回合前后递减", 0x482360: "482360 按库 id 加", 0x53C450: "建筑活动 53C450", 0x562EF0: "DistrictHeroBuff::ApplyBuffsToHero",
    0x602CD0: "Party::OnRaidStart", 0x5CB2D0: "flashback 5CB2D0", 0x614B30: "任务修饰 614B30", 0x615FA0: "任务修饰 615FA0",
    0x76C390: "战斗修饰 76C390", 0x47F050: "47F050", 0x430D30: "430D30", 0x5D03F0: "5D03F0", 0x5C7E50: "5C7E50",
    0x5CAD30: "5CAD30", 0x5A95E0: "5A95E0", 0x5B3B30: "5B3B30", 0x588D30: "588D30", 0x481A40: "UpdateTrinketBuffs",
    0x47A7B0: "47A7B0 技能即时 buff", 0x60D610: "60D610 探索存档", 0x585A60: "585A60 名册存档", 0x294310: "294310 SaveData close",
    0x3D3110: "3D3110 头信息", 0x5B9B50: "5B9B50 技能执行", 0x5C46D0: "RemoveIf", 0x5D8880: "5D8880", 0x5D8F30: "5D8F30",
    0x5FD930: "5FD930", 0x5D03F0: "5D03F0 quirk buff（Hero::AddQuirk）", 0x604460: "604460 按来源删（队伍）",
    0x774CC0: "774CC0 走廊格子",
}
# 0A-3：各持续类型的预期调用者（函数起始）
UPKEEP_EXPECT = {
    0: {0x5CCD10, 0x5B66F0}, 1: {0x586730}, 2: {0x586730}, 3: {0x586730}, 4: {0x535610}, 5: {0x587F90},
    8: {0x775460, 0x60A350}, 9: {0x775460, 0x60A350}, 10: {0x5A6670},
}
# 60A350 先 call 47AAD0（类型 8），再 jmp 47AAD0（类型 9，尾调用），所以类型 9 的返回地址落在 60A350 的调用者里：
# 765D40 的 767BB7、774CC0 的 775050、486530 的 48656B（P4-0D 反汇编确认）
UPKEEP_TAILCALL = {0x767BBC: 0x60A350, 0x775055: 0x60A350, 0x486570: 0x60A350}
# 战斗属性：Actor+0x490 + 类型*0x68，类型就是 ActorCombatStatType（69F90 初始化的名字表 2AAE800 的顺序），
# 面板 ShowBaseStats（7D8C30）按 str_ui_DMG/DEF/PROT/SPD/ATT/CRIT/MAXHP 读同样的偏移（P4-0D）。
# 探针的日志按这个下标顺序写 8 个值；探针 ACT 行里的「spd a->b」取的是下标 6，实际是暴击，不能用。
STAT_NAMES = ["damage_low", "damage_high", "defense", "protection", "speed", "attack", "crit", "max_hp"]
SPEED = 4
# 来源表 +0x46：Roster::System::OnStartTownVisit 对回城的队伍英雄（名册状态 1）经槽位 14 删除的来源（谓词 5902C0），
# 由 78500 的初始化代码读出（P4-0D）
TOWN_CLEAN_SOURCES = {0, 1, 2, 4, 8, 9, 10, 11, 12, 13, 14, 15, 18, 19, 20, 21, 24, 25, 27, 28, 29, 30, 31, 32, 33}
LOADING_STATES = {2, 6, 18}
DURATION_NAMES = ["combat_end", "quest_complete", "quest_not_complete", "quest_end", "activity_end", "idle_start_town_visit",
                  "till_removed", "none", "before_turn", "after_turn", "after_round"]
SAVE_SOURCES = {1, 2, 8, 10, 11, 12, 13, 14, 15, 20, 22, 23, 25, 26, 28, 29, 30, 31}  # 来源表 +0x44 为真（combat.md）
RULE_TRAVERSAL = {0x480540, 0x479BA0, 0x486CC0}  # 调用 479CD0 时手上拿着表内指针的调用者
TEST_IDS = {"ddprobe_p4_main", "ddprobe_p4_load", "ddprobe_p4_rule42", "ddprobe_p4_rule42n", "ddprobe_p4_badstat",
            "ddprobe_p4_badrule", "ddprobe_p4_tray1", "ddprobe_p4_tray6"}


def fname(rva):
    return KNOWN.get(rva, f"{rva:X}")


def hexint(text, default=0):
    try:
        return int(text, 16)
    except (TypeError, ValueError):
        return default


def num(text, default=0):
    try:
        return int(text)
    except (TypeError, ValueError):
        try:
            return float(text)
        except (TypeError, ValueError):
            return default


class Rec:
    __slots__ = ("idx", "clock", "level", "tid", "main", "frame", "secs", "tag", "body", "kv", "session")

    def __init__(self, idx, clock, level, tid, main, frame, secs, tag, body, session):
        self.idx, self.clock, self.level = idx, clock, level
        self.tid, self.main, self.frame, self.secs = tid, main, frame, secs
        self.tag, self.body, self.session = tag, body, session
        self.kv = {m.group(1): (m.group(3) if m.group(3) is not None else m.group(2)) for m in KV.finditer(body)}

    def get(self, key, default=None):
        return self.kv.get(key, default)

    def st(self):
        value = self.kv.get("st")
        if value and "/" in value:
            last, nxt = value.split("/", 1)
            return num(last, -1), num(nxt, -1)
        return -1, -1

    def batt(self):
        return num(self.kv.get("batt", "0"))

    def raid(self):
        return self.kv.get("raid", "0") == "1"


class Log:
    def __init__(self, path):
        self.path = path
        self.recs = []
        self.run_id = None
        self.unparsed = 0
        session = 0
        for idx, raw in enumerate(path.read_text(encoding="utf-8-sig", errors="replace").splitlines()):
            m = LINE.match(raw)
            if not m:
                continue
            msg = m.group("msg")
            if self.run_id is None:
                rid = RUN_ID.search(msg + "，")
                if rid:
                    self.run_id = rid.group(1)
            mm = MSG.match(msg)
            if not mm:
                if not msg.startswith(("日志开始", "hook ")):
                    self.unparsed += 1
                continue
            rec = Rec(idx, m.group("clock"), m.group("level"), int(mm.group("tid")), mm.group("main") == "主", int(mm.group("frame")),
                      float(mm.group("secs")), mm.group("tag"), mm.group("body") or "", session)
            if rec.tag == "CAMPAIGN" and rec.get("phase") == "load":
                session = num(rec.get("serial"), session + 1)
                rec.session = session
            self.recs.append(rec)
        self.by_tag = defaultdict(list)
        for rec in self.recs:
            self.by_tag[rec.tag].append(rec)

    def tag(self, *names):
        out = []
        for name in names:
            out.extend(self.by_tag.get(name, []))
        return sorted(out, key=lambda r: r.idx)

    def last_sum(self, tag):
        """探针每次汇总先写一行 SUM，再写这一批 SUM_*；统计在每次进存档时清零。
        取每个存档（session）最后一批（退出存档或退出游戏时的那一批覆盖整个存档），各存档相加。"""
        sums = self.tag("SUM")
        last = {}
        for rec in sums:
            last[rec.session] = rec
        if tag == "SUM":
            return sorted(last.values(), key=lambda r: r.idx)
        starts = sorted(r.idx for r in sums)
        out = []
        for head in last.values():
            end = min([i for i in starts if i > head.idx], default=10 ** 12)
            out.extend(r for r in self.tag(tag) if head.idx < r.idx < end)
        return out


def sessions_of(log):
    return sorted({r.session for r in log.tag("CAMPAIGN") if r.get("phase") == "load"})


# ------------------------------------------------------------------ 0A

def a1(log):
    lines = log.tag("SLOT13")
    if not lines:
        return NONE, "没有 SLOT13 行（这次没有任何 buff 经槽位 13 添加）", "城镇装卸饰品、进建筑活动，走廊扎营、开奇物，战斗里放带 buff 的技能"
    sums = log.tag("SUM")
    total = sum(num(r.get("slot13")) for r in log.last_sum("SUM")) if sums else len(lines)
    ret_bad = [r for r in lines if r.get("ret") != r.get("next0")]
    gone = [r for r in lines if r.get("ret") == r.get("next0") and r.get("ok") == "0"]
    town = [r for r in lines if r.st() == (19, 19) and not r.raid()]
    town_tgt = [r for r in town if r.get("tgt") != "0"]
    dup = [r for r in lines if num(r.get("sameid")) >= 2 and r.batt() != 0 and r.get("ours") != "1"]
    callers = Counter()
    for r in log.last_sum("SUM_SLOT13"):
        callers[fname(hexint(r.get("func")))] += num(r.get("count"))
    offmain = [r for r in lines if not r.main]
    off_where = Counter((r.st()[0], r.get("caller"), r.get("src")) for r in offmain)
    off_bad = [r for r in offmain if r.st()[0] not in LOADING_STATES]
    obs = (f"详细行 {len(lines)} 条（各存档合计 {total} 次）；返回值不等于调用前的 +0x478：{len(ret_bad)} 条；返回值对但加完不在表里"
           f"（刷新时被删）{len(gone)} 条；城镇里 {len(town)} 条，target 非 0 {len(town_tgt)} 条；战斗中同 id 叠加（加完同 id ≥2 条）"
           f"{len(dup)} 条{('，例如 ' + ', '.join(sorted({r.get('id') or '(空 id)' for r in dup})[:5])) if dup else ''}；非主线程 {len(offmain)} 条"
           f"{('（状态/返回地址/来源：' + ', '.join(f'{s}/{c}/src{src}×{n}' for (s, c, src), n in off_where.most_common(4)) + '）') if offmain else ''}；"
           f"调用者：{', '.join(f'{k}×{v}' for k, v in callers.most_common(10)) or '-'}")
    if gone:
        obs += "；被删的例子：" + "；".join(f"{r.get('id')} src={r.get('src')}" for r in gone[:3])
    exp = ("返回值 = 调用前 Actor+0x478、条目数加一；城镇里 target 都是 0；同一技能连放出现同 id 两条；"
           "不在主线程的添加只出现在加载状态（2/6/18）")
    if off_bad:
        return BAD, obs + "；不在主线程、也不在加载状态的添加：" + "；".join(r.body[:120] for r in off_bad[:2]), exp
    if ret_bad or town_tgt:
        return BAD, obs, exp
    if not dup:
        return PART, obs + "；没有看到战斗中同 id 叠加（同一技能要连放两次）", exp
    return OK, obs, exp


def a2(log):
    lines = log.tag("ERASE")
    sums = log.last_sum("SUM_ERASE")
    if not lines and not sums:
        return NONE, "没有 ERASE 行", "回合推进让 buff 到期、战斗结束、撤退、回城、取下饰品"
    by_via = Counter()
    town_slot14 = Counter()  # 回城的队伍英雄（名册状态 1）：587F90+ADB 经槽位 14，谓词 5902C0（来源表 +0x46）
    town_direct = Counter()  # 名册状态 7 的分支：587F90 直接 5C46D0，谓词 5DAF90（来源表 +0x44）
    off_main = 0
    for r in sums:
        chain = [hexint(x) for x in r.get("via", "0").split("<")]
        by_via[fname(chain[0])] += num(r.get("count"))
        if chain[0] in (0x5CE600, 0x5FD280) and 0x587F90 in chain[1:]:
            town_slot14[num(r.get("src"))] += num(r.get("count"))
        elif chain[0] == 0x587F90:
            town_direct[num(r.get("src"))] += num(r.get("count"))
    off_main = sum(1 for r in lines if not r.main)
    ours = [r for r in lines if r.get("ours") == "1"]
    ours_slot14 = [r for r in ours if hexint(r.get("via")) in (0x5CE600, 0x5FD280)]
    bad_town = sorted(s for s in town_slot14 if s not in TOWN_CLEAN_SOURCES)
    bad_direct = sorted(s for s in town_direct if s not in SAVE_SOURCES)
    obs = (f"详细行 {len(lines)} 条（不在主线程 {off_main} 条）；按经手函数：{', '.join(f'{k}×{v}' for k, v in by_via.most_common(10)) or '-'}；"
           f"回城清理：队伍英雄经槽位 14 删掉的来源 {dict(sorted(town_slot14.items())) or '没有'}，状态 7 分支直接删的来源 "
           f"{dict(sorted(town_direct.items())) or '没有'}；探针按 guid 删除 {len(ours)} 条，其中经槽位 14 {len(ours_slot14)} 条")
    exp = ("每条 buff 消失对应一次 erase；回城的队伍英雄经槽位 14（587F90+ADB）删掉来源表 +0x46 为真的 buff，"
           "城镇里空闲的英雄只做 idle_start_town_visit 递减；槽位 14 → 5C46D0 → 5C4FF0")
    if bad_town or bad_direct:
        return BAD, obs + f"；回城清理删了 +0x46 为假的来源 {bad_town}、直接分支删了 +0x44 为假的来源 {bad_direct}", exp
    if not town_slot14 and not town_direct:
        return PART, obs + "；没有看到回城清理（要从地牢回城）", exp
    return OK, obs, exp


def a3(log):
    sums = log.last_sum("SUM_UPKEEP")
    lines = log.tag("UPKEEP")
    if not sums and not lines:
        return NONE, "没有 BuffDurationUpkeep 调用", "战斗每回合、战斗结束、探索结束、回城"
    per_type = defaultdict(lambda: Counter())
    callers = defaultdict(set)
    for r in sums:
        t = num(r.get("type"))
        for key in ("calls", "changed", "dec", "filt", "rm0", "rmx", "bad"):
            per_type[t][key] += num(r.get(key))
        callers[t].add(UPKEEP_TAILCALL.get(hexint(r.get("caller")), hexint(r.get("func"))))
    wrong_caller = {t: sorted(fname(c) for c in cs - UPKEEP_EXPECT.get(t, set())) for t, cs in callers.items()
                    if cs - UPKEEP_EXPECT.get(t, set())}
    bad = sum(c["bad"] for c in per_type.values())
    parts = []
    for t in sorted(per_type):
        c = per_type[t]
        name = DURATION_NAMES[t] if 0 <= t < len(DURATION_NAMES) else str(t)
        parts.append(f"{t} {name}：调用 {c['calls']}，减一 {c['dec']}，过滤器排除 {c['filt']}，删 duration=0 {c['rm0']}，"
                     f"其他删除 {c['rmx']}，不符 {c['bad']}，调用者 {', '.join(fname(x) for x in sorted(callers[t]))}")
    obs = "；".join(parts)
    if wrong_caller:
        obs += f"；调用者不在预期表里：{wrong_caller}"
    exp = ("类型和调用时机与 combat.md 的表一致（8/9 也由走廊格子 774CC0 → 60A350 递减，60A350 的类型 9 是尾调用，"
           "按返回地址归到 60A350）；只减同类型 duration>0 的；duration==0 的被删")
    if bad:
        return BAD, obs, exp
    seen = set(per_type)
    missing = [t for t in (8, 9, 10, 0) if t not in seen]
    if missing or wrong_caller:
        return PART, obs + (f"；没看到类型 {missing}" if missing else ""), exp
    return OK, obs, exp


def act_summary(log, label):
    acts = [r for r in log.tag("ACT") if r.get("label") == label]
    obs = [r for r in log.tag("ACTOBS") if r.get("label") == label]
    dels = [r for r in log.tag("ACTDEL") if r.get("label") == label]
    done = [r for r in log.tag("ACTDONE") if r.get("label") == label]
    return acts, obs, dels, done


def stats_field(rec, key):
    value = rec.get(key)
    if not value or value == "-":
        return None
    try:
        return [float(x) for x in value.split("/")]
    except ValueError:
        return None


def speed_delta(obs_rec):
    """加上测试 buff 前后的速度变化：ACTOBS 的 stats_before/stats_added 下标 4（+0x630）。
    探针 ACT 行的「spd a->b」读的是下标 6（暴击），不用。"""
    if obs_rec is None:
        return None
    before = stats_field(obs_rec, "stats_before")
    added = stats_field(obs_rec, "stats_added")
    if not before or not added or len(before) <= SPEED or len(added) <= SPEED:
        return None
    return added[SPEED] - before[SPEED]


def check_add(log, label):
    acts, obs, dels, done = act_summary(log, label)
    if not acts:
        return None
    a = acts[0]
    problems = []
    if a.get("guid") != a.get("next0"):
        problems.append(f"guid {a.get('guid')} ≠ 调用前 next {a.get('next0')}")
    if num(a.get("n1")) != num(a.get("n0")) + 1:
        problems.append(f"条目数 {a.get('n0')}→{a.get('n1')}")
    d = speed_delta(obs[0] if obs else None)
    if d is None or abs(d - 5) > 0.01:
        problems.append(f"速度（+0x630）变化 {d}（预期 +5）")
    text = (f"{label}：guid={a.get('guid')}（next {a.get('next0')}→{a.get('next1')}），条目 {a.get('n0')}→{a.get('n1')}，"
            f"速度 {('%+g' % d) if d is not None else '?'}")
    if obs:
        present = "不在了" not in obs[0].body
        text += f"，{('观察时还在' if present else '观察时已不在')}"
        if not present:
            problems.append("观察时条目已不在")
    if dels:
        ok = all(r.get("slot14_ret") == "1" and r.get("pred_hits") == "1" and num(r.get("n1")) == num(r.get("n0")) - 1 for r in dels)
        text += f"，按 guid 移除{'成功' if ok else '有问题'}"
        if not ok:
            problems.append("移除结果不对：" + "；".join(r.body for r in dels))
    else:
        problems.append("没有移除记录")
    if done and obs:
        before = stats_field(obs[0], "stats_before")
        after = stats_field(done[0], "stats_after_remove")
        if before and after and any(abs(x - y) > 0.01 for x, y in zip(before, after)):
            problems.append(f"移除后属性没有恢复：{obs[0].get('stats_before')} → {done[0].get('stats_after_remove')}")
    return text, problems


def a4(log):
    results = [check_add(log, label) for label in ("town_add", "corridor_add")]
    if not any(results):
        return NONE, "没有 town_add / corridor_add（主动测试关闭，或者没有在城镇、走廊停够 1 秒）", "进城镇、走廊各停一下"
    texts, problems = [], []
    for label, res in zip(("城镇", "走廊"), results):
        if res is None:
            texts.append(f"{label}：没有触发")
        else:
            texts.append(res[0])
            problems += res[1]
    obs = "；".join(texts) + "。存档读档部分改由 0A-11 观察"
    exp = "属性变化（速度 +0x630 加 5，移除后恢复）、不崩溃；guid 等于 buff_group_next_guid"
    if problems:
        return BAD, obs + "；问题：" + "；".join(problems), exp
    if None in results:
        return PART, obs, exp
    return OK, obs, exp


def a5(_log):
    return SKIP, "按简报不做（去掉 Mod 再读档），改用 0A-11：CampaignLoad 时原生插入定义、加给英雄，自动存档后下次读档观察", "-"


def rule_counts(rec, ident):
    m = re.search(r"rule\[" + re.escape(ident) + r"\]=(\d+)/(\d+)", rec.body)
    return (int(m.group(1)), int(m.group(2))) if m else None


def a6(log):
    sums = log.last_sum("SUM_RULE")
    if not sums:
        return NONE, "没有 479CD0 的汇总（没进存档）", "城镇、走廊、战斗"
    by_func = Counter()
    total = 0
    traversal = 0
    other_thread = 0
    for r in sums:
        calls = num(r.get("calls"))
        func = hexint(r.get("func"))
        by_func[fname(func)] += calls
        total += calls
        traversal += calls if func in RULE_TRAVERSAL else 0
        other_thread += num(r.get("other"))
    texts = [f"判定 {total} 次，非主线程 {other_thread} 次；调用者：{', '.join(f'{k}×{v}' for k, v in by_func.most_common(8))}"]
    problems = []
    seen = False
    for label, where in (("town_rule42", "城镇"), ("corridor_rule42", "走廊")):
        obs = [r for r in log.tag("ACTOBS") if r.get("label") == label]
        if not obs:
            texts.append(f"{where}：没有触发")
            continue
        seen = True
        r42 = rule_counts(obs[0], "ddprobe_p4_rule42")
        r42n = rule_counts(obs[0], "ddprobe_p4_rule42n")
        d = speed_delta(obs[0])
        texts.append(f"{where}：is_false_rule=0 的为真 {r42[0] if r42 else '?'}/{r42[1] if r42 else '?'}，is_false_rule=1 的为真 "
                     f"{r42n[0] if r42n else '?'}/{r42n[1] if r42n else '?'}，加上两条（+3、+4）后速度 "
                     f"{('%+g' % d) if d is not None else '?'}")
        if r42 and r42[0] != 0:
            problems.append(f"{where} 不接管时 lua_test 规则判成了真")
        # 城镇里规则 42 在前置检查（要 Raid 和区域）就返回假，不经过 is_false_rule 取反，两条都不生效；
        # 走廊里只有取反的那条（+4）生效
        want = 0 if where == "城镇" else 4
        if d is not None and abs(d - want) > 0.01:
            problems.append(f"{where} 速度变化 {d:+g}，预期 {want:+d}")
    exp = ("不接管时 is_actor_status + lua_test 恒为假且无日志：城镇里两条都不生效（前置检查直接返回假，不取反），"
           "走廊里只有 is_false_rule=1 的那条生效（速度 +4）；调用者主要是 480540、479BA0、486CC0、479470、4C5050")
    obs = "；".join(texts)
    if problems:
        return BAD, obs + "；" + "；".join(problems), exp
    return (OK if seen else PART), obs, exp


def a7(log, game_log_text):
    parses = log.tag("BUFF7")
    if not parses:
        return NONE, "没有 BUFF7 行（主动测试关闭或没进存档）", "CampaignLoad 时解析带 lua_stat、lua_rule 的 buff 文件"
    good = [r for r in parses if r.get("stat") == "102" and r.get("rule") == "55"]
    texts = [f"解析 {len(parses)} 次，库里 stat_type=102、rule_type=55 的 {len(good)} 次（路径 {', '.join(sorted({r.get('path') for r in good})) or '-'}）"]
    problems = []
    if not good:
        problems.append("库里没有读到 102/55：" + "；".join(r.body for r in parses[:2]))
    acts, obs, dels, _ = act_summary(log, "town_badenum")
    if obs:
        before = stats_field(obs[0], "stats_before")
        now = stats_field(obs[0], "stats_now")
        rule = rule_counts(obs[0], "ddprobe_p4_badrule")
        same = before and now and all(abs(x - y) < 0.01 for x, y in zip(before, now))
        texts.append(f"城镇里加上两条后属性{'不变' if same else '变了'}，lua_rule 判定为真 {rule[0] if rule else '?'}/{rule[1] if rule else '?'}")
        if not same:
            problems.append(f"未知枚举的 buff 改变了属性：{obs[0].get('stats_before')} → {obs[0].get('stats_now')}")
        if rule and rule[0] != 0:
            problems.append("未知规则判成了真")
    else:
        skipped = [r for r in log.tag("ACT") if r.get("label") == "town_badenum" and "不加" in r.body]
        texts.append("城镇里没有加上未知枚举的 buff" + (f"（{skipped[0].body}）" if skipped else ""))
    if game_log_text is not None:
        marks = {key: game_log_text.count(key) for key in ("BuffStat: Invalid type", "BuffRule: Invalid type", "No Hero check if rule is true")}
        texts.append("游戏日志：" + "，".join(f"{k} {v} 次" for k, v in marks.items()))
    else:
        texts.append("游戏日志里的 BuffStat/BuffRule: Invalid type、No Hero check 要用 --game-log 另查")
    exp = "加载时只写日志不弹框；属性不变；未知规则恒为假；不崩溃"
    obs_text = "；".join(texts)
    if problems:
        return BAD, obs_text + "；" + "；".join(problems), exp
    return (OK if obs else PART), obs_text, exp


def a8(log):
    lines = log.tag("FX8")
    if not lines:
        return NONE, "没有 FX8 行", "CampaignLoad 时查一个原版名字和一个不存在的名字"
    real = [r for r in lines if r.get("name") == "rakebuff_5"]
    fake = [r for r in lines if r.get("name") == "ddprobe_p4_no_such_effect"]
    ok_real = all(hexint(r.get("ptr")) and r.get("name_at0") == "rakebuff_5" for r in real)
    ok_fake = all(hexint(r.get("ptr")) == 0 for r in fake)
    obs = f"rakebuff_5：{len(real)} 次，指针非空且 +0 名字一致 {'是' if ok_real else '否'}；不存在的名字：{len(fake)} 次，返回空 {'是' if ok_fake else '否'}"
    exp = "原版名字非空且名字一致；不存在的为空（游戏日志写 Could not find effect）"
    return (OK if ok_real and ok_fake and real and fake else BAD), obs, exp


def a9(log):
    acts, obs, dels, _ = act_summary(log, "corridor_effect")
    if not acts:
        return NONE, "没有 corridor_effect（主动测试关闭，或走廊里没停够）", "走廊里停一下"
    a = acts[0]
    res = set() if a.get("res_guids") in (None, "-") else set(a.get("res_guids").split(","))
    new = re.findall(r" (\d+):([^:\s]+):s", a.body.split("new:", 1)[1] if "new:" in a.body else "")
    new_guids = {g for g, _ in new}
    obs_text = (f"res+8 的 guid {sorted(res)}，表里新增 {[f'{g}:{i}' for g, i in new]}，roll={a.get('roll')}；"
                f"移除 {sum(1 for r in dels if r.get('slot14_ret') == '1')}/{len(dels)}")
    exp = "加上 buff、不崩溃、res+8 的 guid 与新条目一致（rakebuff_5 加 rake_buff_5l、rake_buff_5h 两条）"
    if not res or res != new_guids:
        return BAD, obs_text, exp
    return OK, obs_text, exp


def a10(log):
    lines = log.tag("FX10")
    if not lines:
        return NONE, "没有 FX10 行", "CampaignLoad 时运行时解析 effect"
    parses = [r for r in lines if r.get("phase") == "parse"]
    worked = [r.get("name") for r in parses if hexint(r.get("ptr"))]
    reparse = [r for r in lines if r.get("phase") == "reparse"]
    starts = [r for r in lines if r.get("phase") == "start"]
    persisted = [r for r in starts if r.session >= 2 and re.search(r"ddprobe_p4_fx_abs=([0-9A-F]+)", r.body)
                 and re.search(r"ddprobe_p4_fx_abs=([0-9A-F]+)", r.body).group(1) != "0"]
    paths = [r.get("path") for r in log.tag("FXFILE") if r.get("ours") != "1"][:2]
    later = [r for r in starts if r.session >= 2]
    sizes = sorted({r.get("map_size") for r in starts})
    obs = (f"成功的路径形式：{', '.join(sorted(set(worked))) or '没有'}（共试 {len(parses)} 次）；同名重解析指针不变："
           f"{', '.join(r.get('same') for r in reparse) or '-'}；第二次及以后进存档时上一次的 effect 还在："
           f"{len(persisted)}/{len(later)}；每次进存档时 effect map 的大小 {', '.join(sizes) or '-'}；"
           f"游戏自己传的路径样例：{', '.join(paths) or '-'}")
    exp = ("至少一种路径成功；同名再解析指针不变；effect map 在卸载存档时清空（3D3A50 → 3EEF20 → 4F3A30(1)），"
           "所以上一次进存档时运行时解析的 effect 下一次不在")
    if not worked or (reparse and any(r.get("same") != "1" for r in reparse)):
        return BAD, obs, exp
    if persisted:
        return BAD, obs + "；上一次的 effect 留到了下一次进存档，和「卸载时清空」不符", exp
    if not later:
        return PART, obs + "；只进了一次存档，清空没有观察到", exp
    return OK, obs, exp


def a11(log):
    adds = [r for r in log.tag("T11ADD") if r.get("skip") != "1"]
    obs_lines = log.tag("T11OBS")
    inst = [r for r in log.tag("INSTLOAD") if r.get("id") in ("ddprobe_p4_main", "ddprobe_p4_load")]
    roster_writes = [r for r in log.tag("WRITE") if r.get("has_main") == "1"]
    texts = []
    problems = []
    game_erased = []
    if adds:
        a = adds[0]
        hero = a.get("hero")
        # T11ADD 的「spd a->b」是下标 6（暴击），不能用。改看同一英雄的城镇测试 town_add 读到的速度（下标 4）：
        # 同一存档里测试 buff 还在时，和之后的存档里比较（只作参考，不参与判定）
        town_obs = [r for r in log.tag("ACTOBS") if r.get("label") == "town_add" and r.get("hero") == hero]
        game_erased = [r for r in log.tag("ERASE") if r.idx > a.idx and r.get("ours") != "1"
                       and any(t in r.get("entry", "") for t in (":ddprobe_p4_main:", ":ddprobe_p4_load:"))]
        first_erase = game_erased[0].idx if game_erased else 10 ** 12
        with_buffs = [r for r in town_obs if r.session == a.session and r.idx < first_erase]
        without = [r for r in town_obs if r.session != a.session]
        spd_with = stats_field(with_buffs[0], "stats_before")[SPEED] if with_buffs and stats_field(with_buffs[0], "stats_before") else None
        spd_without = stats_field(without[0], "stats_before")[SPEED] if without and stats_field(without[0], "stats_before") else None
        texts.append(f"存档 {a.session} 加给 {hero}：main guid {a.get('guid_main')}、load guid {a.get('guid_load')}")
        if spd_with is not None and spd_without is not None:
            texts.append(f"参考：测试 buff 还在时这个英雄的速度 {spd_with:g}，之后的存档里 {spd_without:g}（差 {spd_with - spd_without:+g}，两条合计 +8）")
        later = [r for r in roster_writes if r.idx > a.idx]
        texts.append(f"之后写名册时缓冲里有测试 id {len(later)} 次")
        if game_erased:
            g = game_erased[0]
            texts.append(f"游戏在存档 {g.session} 自己删掉了测试 buff（{len(game_erased)} 条，via {g.get('via')}，"
                         f"{g.get('stack', '').split('<')[2] if g.get('stack', '').count('<') >= 2 else g.get('stack')}，状态 {g.get('st')}）")
    else:
        texts.append("这次运行没有加 0A-11 的测试 buff")
    if obs_lines:
        for r in obs_lines:
            ident = r.get("entry", "").split(":")[1] if ":" in r.get("entry", "") else "?"
            texts.append(f"存档 {r.session} 读档后 {ident} 还在，force_positive={r.get('fp')}，库里有定义={r.get('def_now')}")
            if ident == "ddprobe_p4_main" and r.get("fp") != "0":
                problems.append("main 的 force_positive 读档后仍为 1（预期丢失）")
        dels = log.tag("T11DEL")
        texts.append(f"按 guid 移除 {sum(1 for r in dels if r.get('slot14_ret') == '1')}/{len(dels)}")
    if inst:
        texts.append("读档时 Instance::Load：" + "，".join(f"{r.get('id')} found={r.get('found')} fp={r.get('fp')}" for r in inst[:4]))
        for r in inst:
            if r.get("id") == "ddprobe_p4_load" and r.get("found") == "1" and r.get("fp") != "1":
                problems.append("load 线程注入的定义在读档时找到了，但 force_positive 没有恢复")
    exp = "读档后实例还在，CampaignLoad 才插入定义的 main 丢 force_positive；load 线程注入的 load 恢复 force_positive"
    if problems:
        return BAD, "；".join(texts) + "；" + "；".join(problems), exp
    if not obs_lines:
        why = ("测试 buff 在下一次读档之前就被游戏删了（duration 1 的 idle_start_town_visit，城镇访问开始时递减到 0），读档观察没有发生"
               if game_erased else "还没有读档观察（要加上之后再进一次存档）")
        return (PART if adds else NONE), "；".join(texts) + "；" + why, exp
    return OK, "；".join(texts), exp


def a12(log):
    sums = log.last_sum("SUM_TRAY")
    acts = [r for r in log.tag("ACT") if r.get("label") == "corridor_tray"]
    if not acts:
        return NONE, "没有 corridor_tray", "走廊里停一下"
    res = defaultdict(lambda: [0, 0])
    for r in sums:
        key = (r.get("id"), r.get("polarity"))
        res[key][0] += num(r.get("true"))
        res[key][1] += num(r.get("calls"))
    t1 = {k[1]: v for k, v in res.items() if k[0] == "ddprobe_p4_tray1"}
    t6 = {k[1]: v for k, v in res.items() if k[0] == "ddprobe_p4_tray6"}
    a = acts[0]
    obs = (f"来源表 +0x48：来源1={a.get('trayflag1')} 来源6={a.get('trayflag6')}；700A00 判定（极性: 为真/调用）来源1 "
           f"{ {k: f'{v[0]}/{v[1]}' for k, v in t1.items()} } 来源6 { {k: f'{v[0]}/{v[1]}' for k, v in t6.items()} }；"
           "脉冲（6FC9C0）归 buff_icon 管，没有观察")
    exp = "来源 1 进 buff/debuff 组（并脉冲一次），来源 6 不进这两组"
    if not t1 and not t6:
        return PART, obs + "；700A00 没收到这两条（buff_icon 可能接管了 tray 判定）", exp
    if any(v[0] for v in t6.values()) or not any(v[0] for v in t1.values()):
        return BAD, obs, exp
    return PART, obs, exp


def a13(log):
    sums = log.last_sum("SUM")
    rules = log.last_sum("SUM_RULE")
    if not sums:
        return NONE, "没有汇总", "一场普通战斗"
    adds = sum(num(r.get("slot13")) for r in sums)
    realloc = sum(num(r.get("realloc")) for r in sums)
    total = sum(num(r.get("calls")) for r in rules)
    trav = sum(num(r.get("calls")) for r in rules if hexint(r.get("func")) in RULE_TRAVERSAL)
    obs = f"槽位 13 添加 {adds} 次，其中 buff 表扩容（first 指针变化）{realloc} 次；规则判定 {total} 次，在 buff 遍历中（480540/479BA0/486CC0）{trav} 次"
    exp = "统计扩容次数；规则判定大多发生在 buff 遍历中"
    if not total and not adds:
        return NONE, obs, exp
    if not total:
        return PART, obs, exp
    return (OK if trav * 2 >= total else PART), obs, exp


# ------------------------------------------------------------------ 0B

def write_groups(log):
    groups = []
    current = []
    for r in log.tag("WRITE"):
        if current and (r.tid != current[-1].tid or r.secs - current[-1].secs > 0.5):
            groups.append(current)
            current = []
        current.append(r)
    if current:
        groups.append(current)
    return groups


def b1(log):
    writes = log.tag("WRITE")
    if not writes:
        return NONE, "没有写存档", "城镇里招募、解雇、升级、治疗；出征；走廊；开战；扎营；回城"
    devices = Counter(r.get("dev") for r in writes)
    groups = write_groups(log)
    kinds = Counter()
    raid_order_bad = 0
    load_thread = 0
    extra_after = Counter()
    for g in groups:
        files = tuple(Path(r.get("file")).name for r in g)
        st = g[0].st()
        where = "加载线程" if not g[0].main else ("城镇" if st[0] == 19 else ("地牢" if st[0] == 8 else f"状态{st[0]}"))
        load_thread += 0 if g[0].main else 1
        kinds[(where, files)] += 1
        # 「raid 最后」只对 60D610 自己写的那几个文件成立；同一批里之后可能还有调用者另写的 persist.game.json 等
        # （UpdateBattleState 765D40、load_raid 线程的 3F7DB0 在 60D610 返回后再调 3D3110）
        own = [Path(r.get("file")).name for r in g if "60D610" in r.get("stack", "")]
        if "persist.raid.json" in own and own[-1] != "persist.raid.json":
            raid_order_bad += 1
        if "persist.raid.json" in files and files[-1] != "persist.raid.json":
            last_raid = len(files) - 1 - files[::-1].index("persist.raid.json")
            for r in g[last_raid + 1:]:
                frames = r.get("stack", "").split("<")
                extra_after[f"{Path(r.get('file')).name}<{frames[2] if len(frames) > 2 else '?'}"] += 1
    step_frames = {r.frame for r in log.tag("STEP")}
    raid_writes = [r for r in writes if Path(r.get("file")).name == "persist.raid.json"]
    step_saves = len({r.frame for r in raid_writes if r.frame in step_frames})
    top = "；".join(f"{w} ×{n}：{'、'.join(f)}" for (w, f), n in kinds.most_common(8))
    profile_ok = all(d.startswith("steam_profile_") or d.startswith("profile_") for d in devices if d and "profile" in d)
    steps = sum(num(r.get("steps")) for r in log.last_sum("SUM")) or len(log.tag("STEP"))
    obs = (f"写入 {len(writes)} 次，设备 {dict(devices)}；按批次（同线程、间隔 <0.5 秒）：{top}；加载线程上的批次 {load_thread}；"
           f"走廊步数 {steps}，和走一步同一帧写 persist.raid.json 的 {step_saves} 步；60D610 写的文件里 raid 不是最后一个的批次 {raid_order_bad}；"
           f"批次里 raid 之后另写的文件 {dict(extra_after.most_common(4)) or '-'}")
    exp = ("设备名 steam_profile_N；城镇操作写名册、城镇、庄园、头信息中的几个；探索存档 60D610 写 map、roster、game、raid（raid 最后），"
           "调用者之后可能再写一次 game；走廊不是每步都写；进地牢时有加载线程上的写入")
    if raid_order_bad or not profile_ok:
        return BAD, obs, exp
    has_town = any(w == "城镇" for (w, _f) in kinds)
    has_raid = any("persist.raid.json" in f for (_w, f) in kinds)
    every_step = steps >= 3 and step_saves >= steps
    if every_step:
        return BAD, obs + "；走廊里每步都写了探索存档", exp
    return (OK if has_town and has_raid and load_thread and steps >= 3 else PART), obs, exp


def b2(log):
    done = log.tag("WDONE")
    writes = log.tag("WRITE")
    if not done:
        return NONE, "没有设备写盘记录", "同 0B-1"
    # 选槽复制 backup（2923A0）时，游戏在主线程上直接调设备写盘，不经过 2487E0，这些不参与配对
    in_backup = set()
    inside = False
    for r in sorted(log.tag("BACKUP", "WDONE"), key=lambda x: x.idx):
        if r.tag == "BACKUP":
            inside = r.get("phase") == "enter"
        elif inside and r.main:
            in_backup.add(r.idx)
    backup_done = [r for r in done if r.idx in in_backup]
    queued_done = [r for r in done if r.idx not in in_backup]
    on_main = [r for r in queued_done if r.main]
    failed = [r for r in done if r.get("ok") != "1"]
    # 按（设备，文件名）先进先出配对，算延迟、看顺序
    pending = defaultdict(list)
    for r in writes:
        pending[(r.get("dev"), Path(r.get("file")).name)].append(r)
    lat = []
    matched_order = []
    for r in queued_done:
        key = (r.get("dev"), r.get("file"))
        if pending[key]:
            w = pending[key].pop(0)
            lat.append((r.secs - w.secs) * 1000)
            matched_order.append(w.idx)
    unmatched = sum(len(v) for v in pending.values())
    inversions = sum(1 for a, b in zip(matched_order, matched_order[1:]) if b < a)
    kinds = Counter(r.get("kind") for r in done)
    lat.sort()
    lat_text = f"中位 {lat[len(lat) // 2]:.1f} ms，90% {lat[int(len(lat) * 0.9)]:.1f} ms，最大 {lat[-1]:.1f} ms" if lat else "-"
    obs = (f"写盘 {len(done)} 次（{dict(kinds)}），其中选槽复制 backup 时在主线程同步写 {len(backup_done)} 次（不经 2487E0）；"
           f"其余 {len(queued_done)} 次里在主线程 {len(on_main)} 次，失败 {len(failed)} 次；配对 {len(lat)} 次，没配上的排队 {unmatched} 次；"
           f"排队到写完的延迟 {lat_text}；写完顺序和排队顺序不同 {inversions} 处")
    exp = "排队的写入在 IO 线程上写盘，返回 1，顺序和 0B-1 一致，延迟几毫秒；复制 backup 是主线程同步写"
    if on_main or failed or inversions:
        return BAD, obs, exp
    return OK, obs, exp


def b3(log):
    lines = log.tag("FLUSH")
    if not lines:
        return NONE, "没有调用 FlushSaveDataAsync", "加载界面、退回主菜单、扎营"
    off = [r for r in lines if not r.main]
    slot7 = [r for r in lines if r.get("slot7") not in ("0", None)]
    not_done = [r for r in lines if num(r.get("pending1")) > 0]
    callers = Counter(r.get("caller") for r in lines)
    obs = (f"{len(lines)} 次，非主线程 {len(off)} 次，槽 7 非空 {len(slot7)} 次，返回时还有未写完的存档 {len(not_done)} 次"
           f"（例如 {not_done[0].body if not_done else '-'}）；调用位置 {dict(callers)}")
    exp = "只在主线程；槽 7 为空；返回时 0B-1 记录过的文件未必都写完（不能当作写完的信号）"
    if off or slot7:
        return BAD, obs, exp
    return OK, obs, exp


def b4(log):
    loads = [r for r in log.tag("CAMPAIGN") if r.get("phase") == "load"]
    if not loads:
        return NONE, "没有进存档", "读档"
    texts, problems = [], []
    for r in loads:
        app = r.get("app", "")
        n = re.search(r"(\d+)$", app)
        prefix = r.get("slot5_prefix", "")
        texts.append(f"存档 {r.get('serial')}：app={app} kind={r.get('kind')} slot5={r.get('slot5_name')} prefix={prefix} flush_tid=主线程:{r.get('same')}")
        if not app.startswith("steam_profile_"):
            problems.append(f"设备名 {app} 不是 steam_profile_N")
        if r.get("kind") != "SteamStorageDevice":
            problems.append(f"槽 5 设备是 {r.get('kind')}")
        # Steam 设备的前缀带结尾的「/」（3875B0 用 %s%s 直接拼文件名）
        if n and prefix.rstrip("/") != f"profile_{n.group(1)}":
            problems.append(f"前缀 {prefix} 和设备序号 {n.group(1)} 对不上")
        if r.get("same") != "1":
            problems.append("2A96CD8 不是主线程 id")
    exp = "steam_profile_N、Steam 设备、前缀 profile_N/（带斜杠），N 和所选存档槽一致；2A96CD8 等于主线程 id"
    obs = "；".join(texts)
    return (BAD if problems else OK), obs + ("；" + "；".join(problems) if problems else ""), exp


def b5(log):
    reads = [r for r in log.tag("READ") if "profile" in r.get("dev", "")]
    loads = [r for r in log.tag("CAMPAIGN") if r.get("phase") == "load"]
    if not loads or not reads:
        front = [r for r in reads if r.st()[0] in (21, 22, 23)]
        return NONE, f"没有读档（前端读存档槽信息 {len(front)} 次，线程 {'/'.join(sorted({'主' if r.main else '副' for r in front})) or '-'}）", \
            "分别读城镇存档和地牢存档"
    texts, problems = [], []
    # 探针自己的 vs_run 只按文件名记最后一次写入，同名文件写在两个设备上时（persist.tutorial.json 同时写
    # steam_profile_N 和 steam_profile_9）会比错，这里按（设备，文件名）从 WRITE 行重算
    last_write = {}
    verdict_of = {}
    for r in sorted(log.tag("WRITE", "READ"), key=lambda x: x.idx):
        if r.tag == "WRITE":
            last_write[(r.get("dev"), Path(r.get("file")).name)] = r.get("hash")
        else:
            key = (r.get("dev"), Path(r.get("path")).name)
            verdict_of[r.idx] = ("same" if last_write[key] == r.get("hash") else "diff") if key in last_write else "-"
    for load in loads:
        prev_load = max([l.idx for l in loads if l.idx < load.idx], default=-1)
        before = [r for r in reads if prev_load < r.idx < load.idx and r.st()[0] == 2]
        after = [r for r in reads if r.idx > load.idx and r.st()[0] == 6]
        next_load = min([l.idx for l in loads if l.idx > load.idx], default=10 ** 9)
        after = [r for r in after if r.idx < next_load]
        names_before = sorted({Path(r.get("path")).name for r in before})
        names_after = sorted({Path(r.get("path")).name for r in after})
        same_run = sum(1 for r in before + after if verdict_of.get(r.idx) == "same")
        diff_run = [f"{r.get('dev')}/{Path(r.get('path')).name}" for r in before + after if verdict_of.get(r.idx) == "diff"]
        same_prev = sum(1 for r in before + after if r.get("vs_prev") == "same")
        diff_prev = [Path(r.get("path")).name for r in before + after if r.get("vs_prev") == "diff" and verdict_of.get(r.idx) == "-"]
        threads = {("主" if r.main else "副") for r in before + after}
        texts.append(f"存档 {load.get('serial')}：CampaignLoad 前（状态 2）读 {', '.join(names_before) or '-'}；之后（状态 6）读 "
                     f"{', '.join(names_after) or '-'}；线程 {'/'.join(sorted(threads)) or '-'}；和本次运行同设备同文件最后一次写入的哈希相同 "
                     f"{same_run}，不同 {len(diff_run)}{(' ' + ','.join(diff_run[:4])) if diff_run else ''}；本次没写过、和上次运行相同 {same_prev}"
                     f"{('、不同 ' + ','.join(diff_prev[:3]) + '（上次运行的记录只按文件名，不分设备）') if diff_prev else ''}")
        if "persist.roster.json" in names_after:
            problems.append("名册在 CampaignLoad 之后才读")
        if any(n in names_before for n in ("persist.raid.json", "persist.map.json")):
            problems.append("raid/map 在 CampaignLoad 之前就读了")
        if diff_run:
            problems.append("读到的内容和本次运行最后一次写入不同：" + ",".join(diff_run[:4]))
    exp = ("名册等在 load_campaign 线程上读、早于 CampaignLoad；raid、map 在 load_raid 线程上读、晚于 CampaignLoad；"
           "哈希等于同一设备上同名文件的最后一次写入")
    obs = "；".join(texts)
    return (BAD if problems else OK), obs + ("；" + "；".join(problems) if problems else ""), exp


def b6(log):
    lines = [r for r in log.tag("BACKUP") if r.get("phase") == "enter"]
    loads = [r for r in log.tag("CAMPAIGN") if r.get("phase") == "load"]
    if not lines:
        return NONE, "没有复制 backup（只在前端选存档槽时发生）", "前端选存档槽"
    followed = sum(1 for b in lines if any(l.idx > b.idx for l in loads))
    obs = f"{len(lines)} 次，设备 {', '.join(sorted({r.get('slot5') for r in lines}))}；之后有 CampaignLoad 的 {followed} 次；CampaignLoad 共 {len(loads)} 次"
    return OK, obs, "每次选槽一次，在 CampaignLoad 之前"


def b7(log):
    enters = [r for r in log.tag("RAIDLOAD") if r.get("phase") == "enter"]
    exits = [r for r in log.tag("RAIDLOAD") if r.get("phase") == "exit"]
    battle_loads = [r for r in log.tag("BATTLELOAD") if r.get("phase") == "enter"]
    loads = [r for r in log.tag("CAMPAIGN") if r.get("phase") == "load"]
    if not enters:
        return NONE, "没有读探索存档", "读一个在地牢里（最好在战斗中）存的档"
    texts, problems = [], []
    for e in enters:
        prior = [l for l in loads if l.idx < e.idx]
        if not prior:
            problems.append("60EEE0 早于 CampaignLoad")
        if e.main:
            problems.append("60EEE0 在主线程上")
    starts = [r for r in log.tag("BATTLE") if r.get("phase") == "start"]
    for b in battle_loads:
        nxt_end = min([r.idx for r in log.tag("BATTLE") if r.get("phase") == "end" and r.idx > b.idx], default=10 ** 9)
        if any(b.idx < s.idx < nxt_end for s in starts):
            problems.append("读档恢复的战斗派发了 battle.start")
    texts.append(f"60EEE0 {len(enters)} 次（线程 {'/'.join(sorted({'主' if r.main else '副' for r in enters}))}），返回 {', '.join(r.get('ret') for r in exits) or '-'}；"
                 f"5AD160 {len(battle_loads)} 次（调用者 {', '.join(sorted({r.get('caller') for r in battle_loads})) or '-'}）")
    exp = "load_raid 线程，晚于 CampaignLoad；战斗中的存档会调用 5AD160，不派发 battle.start"
    obs = "；".join(texts)
    if problems:
        return BAD, obs + "；" + "；".join(problems), exp
    return (OK if battle_loads else PART), obs + ("" if battle_loads else "；没有读到战斗中的存档"), exp


def b8(log):
    lines = log.tag("BSAVE")
    if not lines:
        return NONE, "没有战斗存档", "打一场战斗"
    per_battle = defaultdict(list)
    battle = 0
    events = sorted(log.tag("BATTLE") + lines, key=lambda r: r.idx)
    for r in events:
        if r.tag == "BATTLE" and r.get("phase") == "start":
            battle += 1
        elif r.tag == "BSAVE":
            per_battle[battle].append(f"{r.get('state')}/{r.get('round')}")
    obs = "；".join(f"第 {b} 场：{' '.join(v)}" for b, v in per_battle.items()) + "（格式 战斗状态/回合）"
    return OK, obs, "列出在哪些战斗状态存档，确认是否每回合都存"


def b9(log):
    unloads = [r for r in log.tag("CAMPAIGN") if r.get("phase") == "unload"]
    saves = log.tag("BSAVE")
    mons = log.tag("MONLOAD")
    if not saves or not mons:
        return NONE, "没有「战斗中存档 → 退出 → 读档」的组合", "给怪物上流血，等到下一个存档点后退回主菜单，再读档"
    texts, problems = [], []
    for unload in unloads:
        last_save = max([r for r in saves if r.idx < unload.idx], key=lambda r: r.idx, default=None)
        if not last_save:
            continue
        saved = {r.get("guid"): r for r in log.tag("BSAVE_MON") if last_save.idx < r.idx < last_save.idx + 40}
        loaded = {r.get("guid"): r for r in mons if r.idx > unload.idx}
        if not loaded:
            continue
        same = [g for g in saved if g in loaded and saved[g].get("buffs") == loaded[g].get("buffs") and saved[g].get("n") == loaded[g].get("n")]
        diff = [g for g in saved if g in loaded and g not in same]
        missing = [g for g in saved if g not in loaded]
        bleed = sum(1 for g in saved if saved[g].get("buffs") not in ("-", None))
        texts.append(f"退出前最后一次战斗存档有怪物 {len(saved)} 个（带 buff 的 {bleed} 个），读档后一致 {len(same)}，不同 {len(diff)}，没读到 {len(missing)}")
        for g in diff[:3]:
            problems.append(f"怪物 {g}：存 {saved[g].get('buffs')}，读 {loaded[g].get('buffs')}")
    if not texts:
        return NONE, "战斗存档之后没有退出再读档", "同上"
    exp = "条目数、guid、id 和存档时一致"
    obs = "；".join(texts)
    return (BAD if problems else OK), obs + ("；" + "；".join(problems) if problems else ""), exp


def b10(log):
    sums = log.last_sum("SUM")
    lines = log.tag("INSTLOAD")
    if not lines:
        return NONE, "没有读档", "读档"
    found = sum(num(r.get("inst_found")) for r in sums)
    missing = sum(num(r.get("inst_missing")) for r in sums)
    # id 为空的是 effect 直接生成的 buff（combat_stat_buff 等，4C48D0 不填 id），库里本来就没有，照样恢复
    empty = [r for r in lines if r.get("found") == "0" and r.get("id") == ""]
    missing_ids = sorted({r.get("id") for r in lines if r.get("found") == "0"} - TEST_IDS - {""})
    obs =(f"Instance::Load（各存档 CampaignLoad 之后的汇总）：库里找到 {found}，没找到 {missing}；INSTLOAD 行 {len(lines)} 条，"
           f"其中 id 为空、没找到 {len(empty)} 条；没找到的非空、非测试 id：{', '.join(missing_ids[:10]) or '没有'}")
    exp = "有 id 的原版 buff 都能查到；id 为空的（effect 直接生成）查不到，照样恢复"
    return (BAD if missing_ids else OK), obs, exp


def b11(log):
    lines = log.tag("SKILL")
    sums = log.last_sum("SUM")
    if not lines:
        return NONE, "没有技能执行", "英雄和怪物使用攻击、治疗或增益、移动技能"
    tables = Counter(r.get("table") for r in lines)
    e0 = sum(1 for r in lines if r.get("p8E0") == "1")
    paths = Counter((r.get("friendly"), r.get("b100")) for r in lines)
    levels = defaultdict(set)
    for r in lines:
        levels[(r.get("who", "").split("/")[0], r.get("skill"))].add(r.get("lvl"))
    multi = {k: v for k, v in levels.items() if len(v) > 1}
    obs = (f"{len(lines)} 条（各存档合计 {sum(num(r.get('skills')) for r in sums)}），技能指针所在的表 {dict(tables)}；执行后 performer+0x8E0 等于这个技能 "
           f"{e0}/{len(lines)}；分派组合（友方,0x100 位）{dict(paths)}；同一英雄同一技能出现多个等级 {len(multi)} 个。"
           "5BE500/5BF440 收到的指针没有比较（不能 hook DDCore 的语义 hook 入口，也不能订阅 skill.*）")
    exp = "指针都在职业表里；英雄的等级和升级一致；分派路径和 +0x90、0x100 位一致；5BE500/5BF440 收到的是同一个指针"
    if tables.get("none"):
        return BAD, obs, exp
    return PART, obs, exp


def b12(log):
    lines = log.tag("STEAM")
    if not lines:
        return NONE, "没有 STEAM 行", "启动后任意时刻"
    r = lines[-1]
    obs = f"accessor={r.get('accessor')} id32={r.get('id32')} userdata 下的目录 {r.get('userdata')}，匹配={r.get('match')}，remote/262060 存在={r.get('remote_262060')}"
    return (OK if r.get("match") == "1" else BAD), obs, "账号 id 和 userdata 下的目录名相同"


def b13(log):
    gaps = log.tag("SAVEGAP")
    if not gaps:
        return NONE, "主线程上没有写存档", "战斗中的各个存档点"
    in_battle = [r for r in gaps if r.batt() != 0]
    nonzero = [r for r in in_battle if any(num(r.get(k)) for k in ("d_add", "d_erase", "d_upkeep", "d_skill"))]
    obs = (f"主线程写存档的帧 {len(gaps)} 个，其中战斗中 {len(in_battle)} 个；写入到下一次帧边界之间还有 buff 增删、持续时间结算或技能执行的 "
           f"{len(nonzero)} 个{('：' + '；'.join(r.body for r in nonzero[:3])) if nonzero else ''}。没有 DDLua，用游戏活动代替「向 Lua 派发的事件」")
    exp = "差值决定 campaign.save 能不能统一放到帧末"
    if not in_battle:
        return PART, obs, exp
    return OK, obs + ("；可以放到帧末" if not nonzero else "；帧末之前还有活动，同步派发更稳"), exp


def b14(log):
    exits = log.tag("EXIT")
    if not exits:
        return NONE, "没有从菜单退到桌面（或 exit_hook=0）", "城镇里做一次操作后马上从菜单退到桌面（可选）"
    enter = [r for r in exits if r.get("phase") == "enter"][-1]
    after = [r for r in log.tag("WDONE") if r.idx > enter.idx]
    writes_before = [r for r in log.tag("WRITE") if r.idx < enter.idx]
    last_write = writes_before[-1] if writes_before else None
    obs = (f"退出时还没写完 {enter.get('pending')} 个（{enter.body.split('[', 1)[1].split(']', 1)[0] if '[' in enter.body else '-'}）；"
           f"退出之后又写完 {len(after)} 个；最后一次排队写入在退出前 "
           f"{f'{enter.secs - last_write.secs:.2f} 秒' if last_write else '（这次运行没有写过存档）'}")
    exp = "未知：看退出前 IO 队列有没有写完（要在写入后 1 秒内退出才算测到；写盘延迟最大约 0.1 秒）"
    if not last_write or enter.secs - last_write.secs > 1.0:
        return PART, obs + f"；退出前 1 秒内没有写入（退出时状态 {enter.get('st')}），没测到「刚写完就退出」", exp
    return OK, obs, exp


# ------------------------------------------------------------------ 总体

ITEMS = [
    ("0A-1", "槽位 13 添加", a1), ("0A-2", "5C4FF0 删除", a2), ("0A-3", "47AAD0 持续时间", a3), ("0A-4", "主动添加（城镇、走廊）", a4),
    ("0A-5", "读档时库里没有该 id", a5), ("0A-6", "479CD0 规则", a6), ("0A-7", "未知枚举", None), ("0A-8", "EffectLookup", a8),
    ("0A-9", "4CB4D0 施加 effect", a9), ("0A-10", "4F02D0 运行时解析", a10), ("0A-11", "原生插入 buff 定义", a11),
    ("0A-12", "tray", a12), ("0A-13", "扩容与遍历", a13),
    ("0B-1", "2487E0 排队写文件", b1), ("0B-2", "设备写盘", b2), ("0B-3", "FlushSaveDataAsync", b3), ("0B-4", "profile 和设备", b4),
    ("0B-5", "248D60 读文件", b5), ("0B-6", "2923A0 backup", b6), ("0B-7", "读探索存档、读战斗", b7), ("0B-8", "战斗存档时机", b8),
    ("0B-9", "怪物 buff 存读", b9), ("0B-10", "Instance::Load 查库", b10), ("0B-11", "技能执行", b11), ("0B-12", "SteamID", b12),
    ("0B-13", "存档写入到帧末", b13), ("0B-14", "退到桌面", b14),
]


def general(log, log_dir):
    lines = []
    problems = 0
    meta_path = log_dir / ".rig-log.json"
    if meta_path.exists():
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        lines.append(f"方案 {meta.get('profile')}，cfg {meta.get('cfg_files')}")
        if meta.get("profile") != "probe4":
            lines.append("！这不是 probe4 方案的日志")
            problems += 2
    core = log_dir / "DDCore.log"
    if core.exists():
        text = core.read_text(encoding="utf-8-sig", errors="replace")
        rid = RUN_ID.search(text)
        same = rid and rid.group(1) == log.run_id
        lines.append(f"运行标识 {log.run_id}，DDCore.log {'一致' if same else '不一致！'}")
        crash = [l for l in text.splitlines() if "probe_p4" in l and ("崩溃" in l or "已取消" in l)]
        if crash:
            lines.append("DDCore.log 里探针的回调崩溃：" + "；".join(crash[:3]))
            problems += 1
        if not same:
            problems += 2
    hooks = log.tag("HOOKS")
    skips = log.tag("HOOKSKIP")
    if hooks:
        lines.append(f"hook 安装 {hooks[-1].get('installed')}/{hooks[-1].get('total')}，跳过 {len(skips)}" +
                     (("：" + "；".join(r.get("name") for r in skips)) if skips else ""))
    start = log.tag("START")
    if start:
        lines.append(f"active={start[0].get('active')} load_inject={start[0].get('load_inject')} exit_hook={start[0].get('exit_hook')}")
    fails = log.tag("ACTFAIL")
    if fails:
        lines.append("主动测试出过异常，之后的主动测试都停了：" + "；".join(r.body for r in fails))
        problems += 1
    left = log.tag("ACTLEFT")
    if left:
        lines.append("有测试 buff 没来得及移除：" + "；".join(r.body for r in left))
    sessions = sessions_of(log)
    lines.append(f"进存档 {len(sessions)} 次，最后一帧 {log.recs[-1].frame if log.recs else 0}，日志行 {len(log.recs)}，不认识的行 {log.unparsed}")
    return lines, problems


def analyze(log_dir, game_log=None, out=print):
    log_path = log_dir / "probe_p4.log"
    if not log_path.exists():
        out(f"[没有日志] {log_path} 不存在")
        return 2, {}
    log = Log(log_path)
    head, problems = general(log, log_dir)
    out("==== 日志 ====")
    for line in head:
        out("  " + line)
    game_text = None
    if game_log is None and (log_dir / "app.log").exists():
        game_log = log_dir / "app.log"  # 用户按场景步骤把测试目录的 app.log 复制进来
    if game_log:
        game_text = Path(game_log).read_text(encoding="utf-8", errors="replace")
        marks = ("BuffStat: Invalid type", "BuffRule: Invalid type", "No Hero check if rule is true", "Could not find effect",
                 "Could not open EON file", "JSON: Member not found", "doesn't exist")
        out("  游戏日志 " + str(game_log) + "：" + "，".join(f"{m} {game_text.count(m)} 次" for m in marks))
    verdicts = {}
    out("==== 逐项 ====")
    for code, title, fn in ITEMS:
        try:
            verdict, obs, exp = a7(log, game_text) if code == "0A-7" else fn(log)
        except Exception as error:  # 单项分析出错不影响其他项
            verdict, obs, exp = BAD, f"分析脚本出错：{error!r}", "-"
        verdicts[code] = verdict
        out(f"[{code}] {verdict}  {title}")
        out(f"    观察：{obs}")
        out(f"    预期：{exp}")
    counts = Counter(verdicts.values())
    out("==== 汇总 ====")
    out("  " + "，".join(f"{k} {counts.get(k, 0)}" for k in (OK, PART, BAD, NONE, SKIP)))
    if problems >= 2:
        return 2, verdicts
    return (1 if counts.get(BAD) or problems else 0), verdicts


# ------------------------------------------------------------------ 自测：自造一份假日志

class Fake:
    def __init__(self):
        self.lines = ["\ufeff[10:00:00.000] [INFO] 日志开始，DDCore 1.0.0，运行标识 20261112-100000-1234，插件路径 X:\\probe_p4.dll"]
        self.t = 0.0
        self.f = 0

    def add(self, tag, body, main=True, dt=0.01, df=1, level="INFO"):
        self.t += dt
        self.f += df
        clock = f"10:{int(self.t // 60) % 60:02d}:{self.t % 60:06.3f}"
        tid = 100 if main else 200
        self.lines.append(f"[{clock}] [{level}] [t{tid} {'主' if main else '副'} f{self.f} {self.t:.3f}s] {tag} {body}".rstrip())


def write_sample(target):
    """一条完整流程：主菜单 → 进城镇存档（加 0A-11 测试 buff、城镇测试、城镇存档）→ 出征（加载线程存档）→ 走廊测试 → 战斗
    （同技能两次、流血、战斗存档）→ 退回主菜单 → 读档（战斗中）→ 观察 0A-11 → 打完、回城（回城清理）→ 退到桌面。"""
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    k = Fake()
    town = "st=19/19 raid=0 batt=0"
    corr = "st=8/8 raid=1 batt=0"
    batt = "st=8/8 raid=1 batt=12"
    k.add("START", "exe=基准版 base=7FF600000000 main_tid=100 cfg=probe_p4.cfg active=1 load_inject=1 exit_hook=1 prev_writes=0", df=0)
    for name in ("p4_slot13_hero", "p4_slot13_monster", "p4_erase_5C4FF0"):
        k.add("HOOK", f"name={name} rva=0 kind=inline ok=1", df=0)
    k.add("HOOKS", "installed=22 total=22", df=0)
    k.add("STEAM", "exports_user=SteamAPI_SteamUser_v021 exports_id=SteamAPI_ISteamUser_GetSteamID accessor=SteamAPI_SteamUser_v021 called=1 "
                   "user=1 steamid=76561198109909518 id32=149643790 userdata=149643790 match=1 remote_262060=1")
    # 选槽、读档
    k.add("BACKUP", 'phase=enter slot5="steam_profile_5" app="steam_profile_5" st=23/23 raid=0 batt=0')
    k.add("BACKUP", "phase=exit ms=12.0")
    k.add("READ", 'dev="steam_profile_5" path="persist.roster.json" cap=900 got=900 decrypt=0 hash=AA vs_run=- vs_prev=same ms=1 st=2/2 raid=0 batt=0 stack=-', main=False)
    k.add("INSTLOAD", 'id="districts_granary_1" hash=1 found=1 src=26 dur=-1 dtype=9 fp=0 def_fp=0 hash_ok=1', main=False)
    k.add("CAMPAIGN", 'phase=load serial=1 late=0 st=2/19 raid=0 batt=0 app="steam_profile_5" slot5_vtbl=E670A0 kind=SteamStorageDevice '
                      'slot5_name="steam_profile_5" slot5_prefix="profile_5" flush_tid=100 main_tid=100 same=1 backup_flag=0 lib=1 roster=12 campaign=1')
    k.add("T11DEF", "phase=before_insert def_main=0 def_load=7F0 def_load_fp=1 def_load_amt=3")
    k.add("T11SCAN", "found=0 added_this_run=0 roster=12")
    k.add("T11DEF", 'phase=inserted def_main=7F1 found_again=7F1 stat=2 sub="speed_rating" amt=5 fp=1')
    k.add("SLOT13", f'who=hero#3/crusader st=2/19 raid=0 batt=0 id="ddprobe_p4_main" stat=2 amt=5 dur=1 dtype=5 src=1 tgt=0 refresh=1 srcActor=null '
                    f"next0=40 ret=40 next1=41 n0=2 n1=3 present=1 realloc=0 sameid=1 ok=1 ours=1 caller=4A0000")
    k.add("T11ADD", "hero=hero#3/crusader guid_main=40 guid_load=41 def_load=7F0 spd 4->12 next=42（等游戏自动存档，下次进存档时观察）")
    k.add("FX8", 'name="rakebuff_5" ptr=1234 name_at0="rakebuff_5"')
    k.add("FX8", 'name="ddprobe_p4_no_such_effect" ptr=0 name_at0=-')
    k.add("FX10", "phase=start map_size=1189 上个存档留下的 ddprobe_p4_fx_abs=0")
    k.add("FX10", 'phase=parse name=ddprobe_p4_fx_abs path="X:\\a.effects.darkest" written=1 map_size 1189->1190 ptr=5000 target=0 floats=0/0/0/0/0/0/1/0/0/0')
    k.add("FX10", 'phase=parse name=ddprobe_p4_fx_rel path="DDModLoader/probe_p4_tmp/r.effects.darkest" written=1 map_size 1190->1190 ptr=0 target=-1 floats=-')
    k.add("FX10", "phase=reparse name=ddprobe_p4_fx_abs ptr 5000->5000 same=1 floats=0/0/0/0/0/0/2/0/0/0")
    k.add("BUFF7", 'path="X:\\ddprobe_p4.buffs.json" written=1 badstat=6000 stat=102 badrule=6001 rule=55 rule_stat=2')
    k.add("STATE", "last=19 next=19 raid=0 batt=0 campaign=1 loading=0")
    # 城镇主动测试
    k.add("SLOT13", f'who=hero#3/crusader {town} id="SPD5" stat=2 amt=5 dur=3 dtype=9 src=1 tgt=0 refresh=1 srcActor=null next0=42 ret=42 next1=43 '
                    "n0=4 n1=5 present=1 realloc=0 sameid=1 ok=1 ours=1 caller=4A0000", df=60)
    k.add("ACT", f'label=town_add hero=hero#3/crusader {town} def=SPD5 def_stat=2 def_sub="speed_rating" def_amt=5 guid=42 next0=42 next1=43 n0=4 n1=5 spd 12->17')
    k.add("ACTOBS", f"label=town_add when=tick hero=hero#3/crusader {town} stats_before=1/2/3/4/12/6/7/30 stats_added=1/2/3/4/17/6/7/30 "
                    "stats_now=1/2/3/4/17/6/7/30 entries: 42:SPD5:s2:a5:d3:t9:src1:r0", df=90)
    k.add("ERASE", f"who=hero#3/crusader {town} entry=42:SPD5:s2:a5:d3:t9:src1:r0 srcSave=1 ok=1 ours=1 via=5CE600 stack=5C4FF0+0<5C46D0+5A<5CE600+6D")
    k.add("ACTDEL", "label=town_add hero=hero#3/crusader guid=42 slot14_ret=1 pred_hits=1 n0=5 n1=4")
    k.add("ACTDONE", "label=town_add hero=hero#3/crusader stats_after_remove=1/2/3/4/12/6/7/30")
    k.add("ACT", f"label=town_rule42 hero=hero#3/crusader {town} guid42=43 guid42n=44 rstr_hash=1 spd 12->12", df=30)
    k.add("RULETEST", f'who=hero#3/crusader {town} id="ddprobe_p4_rule42" rule=42 false=0 rstr=1 tgt=null skill=0 res=0 caller=4805F9')
    k.add("ACTOBS", f"label=town_rule42 when=tick hero=hero#3/crusader {town} stats_before=1/2/3/4/12/6/7/30 stats_added=1/2/3/4/12/6/7/30 "
                    "stats_now=1/2/3/4/12/6/7/30 entries: 43:ddprobe_p4_rule42:s2:a3:d3:t9:src0:r42 44:ddprobe_p4_rule42n:s2:a4:d3:t9:src0:r42! "
                    "rule[ddprobe_p4_rule42]=0/4 rule[ddprobe_p4_rule42n]=0/4", df=90)
    k.add("ACTDEL", "label=town_rule42 hero=hero#3/crusader guid=43 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    k.add("ACTDEL", "label=town_rule42 hero=hero#3/crusader guid=44 slot14_ret=1 pred_hits=1 n0=5 n1=4")
    k.add("ACT", f"label=town_badenum hero=hero#3/crusader {town} guids=2 stat表[102]+0x52=0 +0x53=0 +0x44=0 spd 12->12", df=30)
    k.add("ACTOBS", f"label=town_badenum when=tick hero=hero#3/crusader {town} stats_before=1/2/3/4/12/6/7/30 stats_added=1/2/3/4/12/6/7/30 "
                    "stats_now=1/2/3/4/12/6/7/30 entries: 45:ddprobe_p4_badstat:s102:a7:d3:t9:src0:r0 46:ddprobe_p4_badrule:s2:a6:d3:t9:src0:r55 "
                    "rule[ddprobe_p4_badstat]=3/3 rule[ddprobe_p4_badrule]=0/3", df=90)
    k.add("ACTDEL", "label=town_badenum hero=hero#3/crusader guid=45 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    k.add("ACTDEL", "label=town_badenum hero=hero#3/crusader guid=46 slot14_ret=1 pred_hits=1 n0=5 n1=4")
    # 城镇操作存档
    for name in ("persist.roster.json", "persist.town.json", "persist.estate.json", "persist.game.json"):
        extra = " has_main=1 has_load=1" if "roster" in name else ""
        k.add("WRITE", f'dev="steam_profile_5" file="{name}" size=100 hash=B{name[8:12]} prof=1 free=1 {town}{extra} stack=294310+1', dt=0.002)
    for name in ("persist.roster.json", "persist.town.json", "persist.estate.json", "persist.game.json"):
        k.add("WDONE", f'kind=steam dev="steam_profile_5" file="{name}" size=100 ok=1 ms=3.0 pending=0', main=False, dt=0.003, df=0)
    k.add("SAVEGAP", f"wframe={k.f} files=persist.roster.json d_add=0 d_erase=0 d_upkeep=0 d_skill=0 {town}")
    # 出征：加载线程
    k.add("STATE", "last=6 next=6 raid=1 batt=0 campaign=1 loading=1")
    k.add("SLOT13", 'who=hero#3/crusader st=6/6 raid=1 batt=0 id="quest_start" stat=2 amt=1 dur=-1 dtype=3 src=32 tgt=0 refresh=1 srcActor=null '
                    "next0=47 ret=47 next1=48 n0=4 n1=5 present=1 realloc=0 sameid=1 ok=1 ours=0 caller=6040A0", main=False)
    for name in ("persist.map.json", "persist.roster.json", "persist.game.json", "persist.raid.json"):
        k.add("WRITE", f'dev="steam_profile_5" file="{name}" size=100 hash=C{name[8:12]} prof=1 free=1 st=6/6 raid=1 batt=0 stack=60D610+1', main=False, dt=0.002)
    for name in ("persist.map.json", "persist.roster.json", "persist.game.json", "persist.raid.json"):
        k.add("WDONE", f'kind=steam dev="steam_profile_5" file="{name}" size=100 ok=1 ms=2.0 pending=0', main=False, dt=0.003, df=0)
    k.add("FLUSH", f"caller=3E1F00 slot7=0 pending0=1 [persist.raid.json] pending1=1 ms=0.5 {corr}")
    k.add("STATE", "last=8 next=8 raid=1 batt=0 campaign=1 loading=0")
    for n in range(1, 6):
        k.add("STEP", f"n={n} {corr}", df=20)
    # 走廊主动测试
    k.add("ACT", f'label=corridor_add hero=hero#3/crusader {corr} def=SPD5 def_stat=2 def_sub="speed_rating" def_amt=5 guid=48 next0=48 next1=49 n0=5 n1=6 spd 12->17', df=60)
    k.add("ACTOBS", f"label=corridor_add when=tick hero=hero#3/crusader {corr} stats_before=1/2/3/4/12/6/7/30 stats_added=1/2/3/4/17/6/7/30 "
                    "stats_now=1/2/3/4/17/6/7/30 entries: 48:SPD5:s2:a5:d3:t9:src1:r0", df=90)
    k.add("ACTDEL", "label=corridor_add hero=hero#3/crusader guid=48 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    k.add("ACTDONE", "label=corridor_add hero=hero#3/crusader stats_after_remove=1/2/3/4/12/6/7/30")
    k.add("ACT", f"label=corridor_rule42 hero=hero#3/crusader {corr} guid42=49 guid42n=50 rstr_hash=1 spd 12->16", df=30)
    k.add("ACTOBS", f"label=corridor_rule42 when=tick hero=hero#3/crusader {corr} stats_before=1/2/3/4/12/6/7/30 stats_added=1/2/3/4/16/6/7/30 "
                    "stats_now=1/2/3/4/16/6/7/30 entries: 49:ddprobe_p4_rule42:s2:a3:d3:t9:src0:r42 50:ddprobe_p4_rule42n:s2:a4:d3:t9:src0:r42! "
                    "rule[ddprobe_p4_rule42]=0/5 rule[ddprobe_p4_rule42n]=5/5", df=90)
    k.add("ACTDEL", "label=corridor_rule42 hero=hero#3/crusader guid=49 slot14_ret=1 pred_hits=1 n0=7 n1=6")
    k.add("ACTDEL", "label=corridor_rule42 hero=hero#3/crusader guid=50 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    k.add("ACT", f"label=corridor_tray hero=hero#3/crusader {corr} guid1=51 guid6=52 trayflag1=1 trayflag6=0 spd 12->14", df=30)
    k.add("TRAY", "id=ddprobe_p4_tray1 polarity=1 res=1 src=1 (首次)")
    k.add("TRAY", "id=ddprobe_p4_tray6 polarity=1 res=0 src=6 (首次)")
    k.add("ACTOBS", f"label=corridor_tray when=tick hero=hero#3/crusader {corr} stats_before=- stats_added=- stats_now=- entries: 51:ddprobe_p4_tray1:s2:a1:d3:t9:src1:r0", df=150)
    k.add("ACTDEL", "label=corridor_tray hero=hero#3/crusader guid=51 slot14_ret=1 pred_hits=1 n0=7 n1=6")
    k.add("ACTDEL", "label=corridor_tray hero=hero#3/crusader guid=52 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    k.add("ACT", f'label=corridor_effect hero=hero#3/crusader {corr} effect=rakebuff_5 ptr=1234 name="rakebuff_5" roll=0.5000 res_interrupt=0 '
                 "res_resisted=0 res_cleared=0 res_guids=53,54 new: 53:rake_buff_5l:s3:a0.25:d4:t9:src1:r26 54:rake_buff_5h:s3:a0.25:d4:t9:src1:r26", df=30)
    k.add("ACTDEL", "label=corridor_effect hero=hero#3/crusader guid=53 slot14_ret=1 pred_hits=1 n0=7 n1=6", df=60)
    k.add("ACTDEL", "label=corridor_effect hero=hero#3/crusader guid=54 slot14_ret=1 pred_hits=1 n0=6 n1=5")
    # 战斗
    k.add("BATTLE", f"phase=start n=1 monsters=2 {batt}")
    k.add("SKILL", 'who=hero#3/crusader skill="battlefield_bandage" lvl=0 type="heal" friendly=1 mask=F b100=0 nfx=1 table=BD0 targets=1 t0=hero#4/vestal flag=0 p8E0=1 p8E8=1')
    k.add("SLOT13", f'who=hero#3/crusader {batt} id="SPD2" stat=2 amt=2 dur=3 dtype=9 src=1 tgt=0 refresh=1 srcActor=hero#3/crusader next0=55 ret=55 next1=56 '
                    "n0=5 n1=6 present=1 realloc=0 sameid=1 ok=1 ours=0 caller=4CA4E0")
    k.add("SLOT13", f'who=hero#3/crusader {batt} id="SPD2" stat=2 amt=2 dur=3 dtype=9 src=1 tgt=0 refresh=1 srcActor=hero#3/crusader next0=56 ret=56 next1=57 '
                    "n0=6 n1=7 present=1 realloc=1 sameid=2 ok=1 ours=0 caller=4CA4E0")
    k.add("SKILL", 'who=monster#1/bandit skill="slice" lvl=0 type="melee" friendly=0 mask=3 b100=0 nfx=2 table=BA8 targets=1 t0=hero#3/crusader flag=0 p8E0=1 p8E8=1')
    k.add("UPKEEP", f"who=hero#3/crusader {batt} type=9 caller=7754C0 n0=7 n1=7 dec=2 filt=0 rm0=0 rmx=0 same=5 bad=0 | 55:SPD2:t9:d3->2 56:SPD2:t9:d3->2")
    k.add("BSAVE", f"state=12 round=1 monsters=2 caller=60D610+1 {batt}")
    k.add("BSAVE_MON", "guid=1 cls=bandit n=1 next=2 buffs=1:bleed_debuff")
    k.add("BSAVE_MON", "guid=2 cls=bandit n=0 next=1 buffs=-")
    k.add("WRITE", f'dev="steam_profile_5" file="persist.raid.json" size=200 hash=D1 prof=1 free=1 {batt} stack=60D610+1')
    k.add("WDONE", 'kind=steam dev="steam_profile_5" file="persist.raid.json" size=200 ok=1 ms=2.0 pending=0', main=False, df=0)
    k.add("SAVEGAP", f"wframe={k.f} files=persist.raid.json d_add=0 d_erase=0 d_upkeep=0 d_skill=0 {batt}")
    # 退回主菜单
    k.add("SUM", "when=campaign_unload session=1 slot13=9 realloc=1 unexpected=0 offmain=1 erases=7 writes=9 done=9 failed=0 reads=1 save_reads=1 "
                 "flush=1 inst_found=1 inst_missing=0 skills=2 in_class=2 not_in_class=0 p8E0=2 p8E8=2 buff_paths=1 fx_paths=2 pending=0 steps=5")
    k.add("SUM_SLOT13", "when=campaign_unload caller=4CA4E0 func=4CA390 count=2")
    k.add("SUM_SLOT13", "when=campaign_unload caller=6040A0 func=602CD0 count=1")
    k.add("SUM_ERASE", "when=campaign_unload src=1 via=5CE600<4A0000 count=7")
    k.add("SUM_UPKEEP", "when=campaign_unload type=9 caller=7754C0 func=775460 calls=6 changed=1 dec=2 filt=0 rm0=0 rmx=0 bad=0")
    k.add("SUM_UPKEEP", "when=campaign_unload type=8 caller=7754C0 func=775460 calls=6 changed=0 dec=0 filt=0 rm0=0 rmx=0 bad=0")
    k.add("SUM_UPKEEP", "when=campaign_unload type=10 caller=5A66C0 func=5A6670 calls=2 changed=0 dec=0 filt=0 rm0=0 rmx=0 bad=0")
    k.add("SUM_RULE", "when=campaign_unload caller=4805F9 func=480540 rule=0 calls=500 true=500 main=499 other=1")
    k.add("SUM_RULE", "when=campaign_unload caller=479BC0 func=479BA0 rule=42 calls=40 true=0 main=40 other=0")
    k.add("SUM_RULE", "when=campaign_unload caller=4C5500 func=4C5050 rule=26 calls=30 true=2 main=30 other=0")
    k.add("SUM_TRAY", "when=campaign_unload id=ddprobe_p4_tray1 polarity=1 calls=150 true=150")
    k.add("SUM_TRAY", "when=campaign_unload id=ddprobe_p4_tray1 polarity=0 calls=150 true=0")
    k.add("SUM_TRAY", "when=campaign_unload id=ddprobe_p4_tray6 polarity=1 calls=150 true=0")
    k.add("CAMPAIGN", "phase=unload serial=1")
    # 读战斗中的存档
    k.add("READ", 'dev="steam_profile_5" path="persist.roster.json" cap=900 got=900 decrypt=0 hash=Crost vs_run=same@f10 vs_prev=diff ms=1 st=2/2 raid=0 batt=0 stack=-', main=False)
    k.add("INSTLOAD", 'id="ddprobe_p4_main" hash=1 found=0 src=1 dur=1 dtype=5 fp=0 def_fp=- hash_ok=1', main=False)
    k.add("INSTLOAD", 'id="ddprobe_p4_load" hash=2 found=1 src=1 dur=1 dtype=5 fp=1 def_fp=1 hash_ok=1', main=False)
    k.add("CAMPAIGN", 'phase=load serial=2 late=0 st=2/6 raid=0 batt=0 app="steam_profile_5" slot5_vtbl=E670A0 kind=SteamStorageDevice '
                      'slot5_name="steam_profile_5" slot5_prefix="profile_5" flush_tid=100 main_tid=100 same=1 backup_flag=0 lib=1 roster=12 campaign=1')
    k.add("T11DEF", "phase=before_insert def_main=0 def_load=7F0 def_load_fp=1 def_load_amt=3")
    k.add("T11OBS", "hero=hero#3/crusader entry=40:ddprobe_p4_main:s2:a5:d1:t5:src1:r0 fp=0 fn=0 amt=5 stat=2 guid_lt_next=1 def_now=0")
    k.add("T11DEL", "hero=hero#3/crusader guid=40 id=ddprobe_p4_main slot14_ret=1 pred_hits=1")
    k.add("T11OBS", "hero=hero#3/crusader entry=41:ddprobe_p4_load:s2:a3:d1:t5:src1:r0 fp=1 fn=0 amt=3 stat=2 guid_lt_next=1 def_now=1")
    k.add("T11DEL", "hero=hero#3/crusader guid=41 id=ddprobe_p4_load slot14_ret=1 pred_hits=1")
    k.add("T11SCAN", "found=2 added_this_run=1 roster=12")
    k.add("FX10", "phase=start map_size=1189 上个存档留下的 ddprobe_p4_fx_abs=0")
    k.add("STATE", "last=6 next=6 raid=1 batt=0 campaign=1 loading=1")
    k.add("RAIDLOAD", "phase=enter session=2 st=6/6 raid=1 batt=0", main=False)
    k.add("READ", 'dev="steam_profile_5" path="persist.raid.json" cap=200 got=200 decrypt=0 hash=D1 vs_run=same@f20 vs_prev=- ms=1 st=6/6 raid=1 batt=0 stack=-', main=False)
    k.add("BATTLELOAD", "phase=enter battle=1 caller=60EEE0+364 st=6/6 raid=1 batt=0", main=False)
    k.add("MONLOAD", "guid=1 cls=bandit n=1 next=2 buffs=1:bleed_debuff", main=False)
    k.add("MONLOAD", "guid=2 cls=bandit n=0 next=1 buffs=-", main=False)
    k.add("BATTLELOAD", "phase=exit monsters=2", main=False)
    k.add("RAIDLOAD", "phase=exit ret=1 st=6/6 raid=1 batt=0", main=False)
    k.add("STATE", "last=8 next=8 raid=1 batt=12 campaign=1 loading=0")
    k.add("BATTLE", f"phase=end n=1 {corr}")
    # 回城
    k.add("UPKEEP", "who=hero#3/crusader st=9/9 raid=1 batt=0 type=5 caller=588B98 n0=6 n1=5 dec=0 filt=0 rm0=1 rmx=0 same=5 bad=0 | 60:x:t5:d1->x")
    k.add("ERASE", "who=hero#3/crusader st=9/9 raid=1 batt=0 entry=57:SPD2:s2:a2:d2:t9:src1:r0 srcSave=1 ok=1 ours=0 via=5CE600 stack=5C4FF0+0<5C46D0+5A<5CE600+72<587F90+ADB")
    k.add("SUM", "when=campaign_unload session=2 slot13=1 realloc=0 unexpected=0 offmain=0 erases=3 writes=0 done=0 failed=0 reads=2 save_reads=2 "
                 "flush=0 inst_found=1 inst_missing=1 skills=0 in_class=0 not_in_class=0 p8E0=0 p8E8=0 buff_paths=0 fx_paths=0 pending=0 steps=0")
    k.add("SUM_ERASE", "when=campaign_unload src=1 via=5CE600<587F90 count=2")
    k.add("SUM_ERASE", "when=campaign_unload src=9 via=5CE600<587F90 count=1")
    k.add("SUM_UPKEEP", "when=campaign_unload type=5 caller=588B98 func=587F90 calls=4 changed=1 dec=0 filt=0 rm0=1 rmx=0 bad=0")
    k.add("SUM_UPKEEP", "when=campaign_unload type=0 caller=5CCE00 func=5CCD10 calls=4 changed=0 dec=0 filt=0 rm0=0 rmx=0 bad=0")
    k.add("SUM_RULE", "when=campaign_unload caller=4805F9 func=480540 rule=0 calls=100 true=100 main=100 other=0")
    # 城镇里存一次档后马上退到桌面
    k.add("WRITE", 'dev="steam_profile_5" file="persist.game.json" size=100 hash=E1 prof=1 free=1 st=19/19 raid=0 batt=0 stack=294310+1')
    k.add("EXIT", "phase=enter pending=1 [persist.game.json] st=19/19 raid=0 batt=0")
    k.add("WDONE", 'kind=steam dev="steam_profile_5" file="persist.game.json" size=100 ok=1 ms=2.0 pending=0', main=False, df=0)
    (target / "probe_p4.log").write_text("\n".join(k.lines) + "\n", encoding="utf-8")
    (target / "DDCore.log").write_text("\ufeff[10:00:00.000] [INFO] DDCore 1.0.0 启动，运行标识 20261112-100000-1234，主线程 100\n", encoding="utf-8")
    (target / ".rig-log.json").write_text(json.dumps({"profile": "probe4", "cfg_files": ["probe_p4.cfg"]}), encoding="utf-8")


SELFTEST_EXPECT = {
    "0A-1": OK, "0A-2": OK, "0A-3": OK, "0A-4": OK, "0A-5": SKIP, "0A-6": OK, "0A-7": OK, "0A-8": OK, "0A-9": OK, "0A-10": OK,
    "0A-11": OK, "0A-12": PART, "0A-13": OK, "0B-1": OK, "0B-2": OK, "0B-3": OK, "0B-4": OK, "0B-5": OK, "0B-6": OK, "0B-7": OK,
    "0B-8": OK, "0B-9": OK, "0B-10": OK, "0B-11": PART, "0B-12": OK, "0B-13": OK, "0B-14": OK,
}


def selftest():
    write_sample(FAKE_DIR)
    code, verdicts = analyze(FAKE_DIR)
    wrong = {k: (v, SELFTEST_EXPECT.get(k)) for k, v in verdicts.items() if v != SELFTEST_EXPECT.get(k)}
    # 再造一个坏样本：0A-1 的返回值不等于调用前的 next guid，必须判成不一致、退出码 1
    text = (FAKE_DIR / "probe_p4.log").read_text(encoding="utf-8")
    text = text.replace("next0=42 ret=42", "next0=42 ret=99", 1)
    bad_dir = FAKE_DIR.parent / "p4_probe_fake_bad"
    if bad_dir.exists():
        shutil.rmtree(bad_dir)
    shutil.copytree(FAKE_DIR, bad_dir)
    (bad_dir / "probe_p4.log").write_text(text, encoding="utf-8")
    sink = []
    bad_code, bad_verdicts = analyze(bad_dir, out=sink.append)
    if bad_verdicts.get("0A-1") != BAD:
        wrong["坏样本 0A-1"] = (bad_verdicts.get("0A-1"), BAD)
    if bad_code != 1:
        wrong["坏样本退出码"] = (bad_code, 1)
    missing_code, _ = analyze(FAKE_DIR / "没有这个目录", out=sink.append)
    if missing_code != 2:
        wrong["缺日志退出码"] = (missing_code, 2)
    print("==== 自测 ====")
    if wrong or code != 0:
        print(f"失败：{wrong}，退出码 {code}")
        return 1
    print(f"通过：假日志 {len(verdicts)} 项判定和预期一致，坏样本判出不一致，缺日志返回 2（假日志在 {FAKE_DIR}）")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log-dir", type=Path, help="rig.py save-log 保存的目录（含 probe_p4.log）")
    parser.add_argument("--game-log", type=Path, help="游戏自己的日志文件，另查 BuffStat/BuffRule: Invalid type 等文字")
    parser.add_argument("--selftest", action="store_true", help="用自造的假日志自测")
    args = parser.parse_args()
    if args.selftest:
        sys.exit(selftest())
    if not args.log_dir:
        parser.error("需要 --log-dir 或 --selftest")
    code, _ = analyze(args.log_dir, args.game_log)
    sys.exit(code)


if __name__ == "__main__":
    main()

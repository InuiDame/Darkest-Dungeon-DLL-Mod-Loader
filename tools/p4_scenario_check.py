"""检查 P4 用户场景的日志，逐项判定 docs/10-p4-scenario.md「P4 的整体验收」的 6 条（每条拆成可判定的子项）：通过、失败、没有触发，
另外列出要用户目测的几项（需要目测）。

用法（guest，工作目录 D:\\workplace；步骤见 DD.ModLoader/docs/10-p4-scenario.md）：

    python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario4 --combat-log-dir DD.ModLoader/build/rig_logs/combat
    python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir ...   # 只看第一段：combat.stack 没有触发，退出码 2
    python -B DD.ModLoader/tools/p4_scenario_check.py --log-dir ... --combat-log-dir ... --lua-log-dir DD.ModLoader/build/rig_logs/lua-p4   # 可选的 #6
    python -B DD.ModLoader/tools/p4_scenario_check.py --selftest     # 自测，用 dd_lua_tests 生成的 build/p4c_rehearsal

场景是一次 scenario4 方案的游戏运行（DDCore、DDLua 和 4 个场景 Lua Mod：p4b_combat、shield_lite、p4c_shield、p4c_persist），
加上第二段：另一次 combat 方案的运行（test_combat，--combat-log-dir，也是必需的：审核 A-S4 的倍率核对要在游戏里跑过）。
先核对日志本身（同一次运行、晚于部署、方案、DDLua 默认配置、4 个 Mod 都加载了），再逐项判定。复刻 DDShield 的部分按
shield_lite 的 SHIELD 行逐次重放护盾的状态，用 DDShield 的规则独立地重算每一次获得（effect 名字也独立解析）、吸收、回合变化、
生命 ≤ 0 时的清零和生命损失。场景 Mod 自己写的「失败 [标签]」一律判失败。rig_tamper.log 只用本次运行的记录（运行标识对得上，
或者游戏没运行时改的、早于本次启动），用 --restore 恢复过的改动从恢复的时刻起不再算预期的不一致。
退出码：0 全部必需项通过；1 有失败或格式不认识；2 没有失败，但有必需项没有触发。「需要目测」不影响退出码。
"""
import argparse
import io
import json
import os
import re
import shutil
import sys
import tempfile
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import p3_scenario_check as p3  # noqa: E402  日志读取、同一次运行的核对、hook 报告、汇总输出都沿用 P3 的

PASS, FAIL, FORMAT, MISS, NOLOG = p3.PASS, p3.FAIL, p3.FORMAT, p3.MISS, p3.NOLOG
LOOK = "需要目测"  # 检查脚本判定不了、要用户看画面的项；不影响退出码
PROJECT = Path(__file__).resolve().parent.parent
SHIELD_CONFIG = PROJECT / "examples" / "lua" / "shield_lite" / "lua" / "config.lua"
REHEARSAL = PROJECT / "build" / "p4c_rehearsal"
TAMPER_LOG = "rig_tamper.log"
p3.PROFILE_PLUGINS["scenario4"] = {"DDLua"}
p3.PROFILE_PLUGINS["combat"] = {"test_combat"}

# 场景 Lua Mod：id -> project.xml 的标题
SCENARIO_MODS = {
    "p4b_combat": "DDLua Test P4b Combat",
    "shield_lite": "DDLua Example Shield Lite",
    "p4c_shield": "DDLua Test P4c Shield",
    "p4c_persist": "DDLua Test P4c Persist",
}
# 场景 Mod 自报的失败归到哪条验收
MOD_ACCEPTANCE = {"p4b_combat": "#4、#5", "shield_lite": "#1", "p4c_shield": "#1", "p4c_persist": "#2、#3"}
EPS = 1e-3
STATE_KEYS = ("cur", "max", "base", "emax", "regen", "regen_turns", "decay", "decay_turns", "pierce", "pierce_by")
REQUIRED_OPS = ("add", "max", "full", "break", "regen", "decay")
WHERES = ("town", "corridor", "battle")
# p4b_combat 每个场合都要有的写路径（审核 A 的运行证据第 1 条）；battle 另有怪物上的增删
WRITE_TAGS = ("add", "script.apply", "stack", "queue", "queue.done", "remove.tag", "remove.guid", "script.remove", "effect", "rule.live")
BATTLE_WRITE_TAGS = ("monster", "monster.remove")
# buff.remove 的原因（DDCore 的 RemoveReason）；报告里列出哪些在这次运行里出现过
REMOVE_REASONS = ("expired", "refresh", "town_visit", "burn_decay", "by_guid", "reset", "effect", "item", "skill_instant", "trinket", "plugin",
                  "predicate", "other")
DEFAULT_FRAME_MS = 10  # DDLua 的 combat_frame_ms 默认值


# ---------------------------------------------------------------- DDShield 的规则（独立实现，和 shield_lite 的 Lua 互相核对）

def read_config(path=SHIELD_CONFIG):
    """shield_lite/lua/config.lua 里检查要用的数：基础上限、初始值、每回合增减、职业表、名字看不出规则的 effect。读不到的用默认值。"""
    cfg = {"default_max": 0.0, "start_flat": 0.0, "start_hp_ratio": 0.0, "max_hp_ratio": 0.0, "regen_per_round": 0.0, "decay_per_round": 0.0,
           "classes": {}, "specs": {}}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return cfg
    text = re.sub(r"--[^\n]*", "", text)
    for key in list(cfg):
        if key in ("classes", "specs"):
            continue
        m = re.search(rf"(?<![\w.]){key}\s*=\s*(-?[\d.]+)", text)
        if m:
            cfg[key] = float(m.group(1))
    block = re.search(r"classes\s*=\s*\{(.*?)\n\s*\}", text, re.S)
    if block:
        for name, body in re.findall(r"(\w+)\s*=\s*\{([^}]*)\}", block.group(1)):
            entry = {}
            for k, v in re.findall(r"(max|start)\s*=\s*(-?[\d.]+)", body):
                entry[k] = float(v)
            cfg["classes"][name.lower()] = entry
    block = re.search(r"specs\s*=\s*\{(.*?)\n\s*\}", text, re.S)
    if block:
        for name, body in re.findall(r"(\w+)\s*=\s*\{([^}]*)\}", block.group(1)):
            op = re.search(r"op\s*=\s*\"(\w+)\"", body)
            value = re.search(r"value\s*=\s*(-?[\d.]+)", body)
            turns = re.search(r"(?:turns|duration)\s*=\s*(-?\d+)", body)
            if op:
                cfg["specs"][name] = {"op": op.group(1), "value": float(value.group(1)) if value else 0.0,
                                      "turns": int(turns.group(1)) if turns else 1}
    return cfg


def c_atoi(s):
    m = re.match(r"\s*([+-]?\d+)", s)
    return int(m.group(1)) if m else 0


def parse_name(name):
    """DDShield 的 ParseNameSpec：shield_[self_]<规则>。认不出时 None。turns 只有 regen、decay 有。"""
    if not name.startswith("shield_"):
        return None
    p = name[7:]
    if p.startswith("self_"):
        p = p[5:]
    for pre, op in (("gain_pct_", "addpct"), ("heal_pct_", "addpct"), ("max_pct_", "maxpct"), ("damage_pct_", "damagepct"),
                    ("gain_", "add"), ("heal_", "add"), ("max_", "max"), ("damage_", "damage")):
        if p.startswith(pre):
            return {"op": op, "value": float(c_atoi(p[len(pre):])), "turns": None}
    for op in ("regen", "decay"):
        if p.startswith(op + "_"):
            rest = p[len(op) + 1:]
            us = rest.find("_")
            return {"op": op, "value": float(c_atoi(rest)), "turns": c_atoi(rest[us + 1:]) if us >= 0 else 1}
    if p == "pierce":
        return {"op": "pierce", "value": 100.0, "turns": None}
    if p.startswith("pierce_"):
        return {"op": "pierce", "value": float(c_atoi(p[7:])), "turns": None}
    if p in ("full", "break"):
        return {"op": p, "value": 0.0, "turns": None}
    return None


def spec_of(name, cfg):
    """配置表优先（DDShield 先查 DDShield.effects.json），再按名字解析。"""
    s = cfg["specs"].get(name)
    if s:
        return dict(s)
    return parse_name(name)


def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def expected_init(line, cfg):
    """EnsureInit：配置的上限和初始值，英雄按职业表覆盖。"""
    max_hp = float(line.get("max_hp") or 0)
    cap = cfg["default_max"]
    if cfg["max_hp_ratio"] > 0 and max_hp > 0:
        cap = cfg["max_hp_ratio"] * max_hp
    start = cfg["start_flat"]
    if cfg["start_hp_ratio"] > 0 and max_hp > 0:
        start += cfg["start_hp_ratio"] * max_hp
    entry = cfg["classes"].get((line.get("class") or "").lower()) if line.get("actor", "").startswith("hero:") else None
    if entry:
        cap = entry.get("max", cap)
        start = entry.get("start", start)
    s = dict(cur=0.0, max=cap, base=cap, emax=0.0, regen=0.0, regen_turns=0, decay=0.0, decay_turns=0, pierce=0.0, pierce_by="")
    s["cur"] = clamp(start, 0, cap if cap > 0 else start)
    if s["max"] <= 0:
        s["max"] = s["cur"]
    return s


def apply_op(before, op, value, turns, performer):
    """ApplySpec（含 RecalcMax，没有 buff 带来的上限）。heal、clear 是配置表里 add、break 的别名（DDShield 同）。"""
    s = dict(before)

    def recalc():
        s["max"] = max(s["base"] + s["emax"], 0.0)
        s["cur"] = clamp(s["cur"], 0.0, s["max"])

    if op in ("add", "heal"):
        s["cur"] += value
    elif op == "addpct":
        s["cur"] += s["max"] * value / 100
    elif op == "max":
        s["emax"] = max(s["emax"] + value, 0.0)
        recalc()
    elif op == "maxpct":
        s["emax"] = max(s["emax"] + (s["base"] + s["emax"]) * value / 100, 0.0)
        recalc()
    elif op == "set":
        s["cur"] = value
    elif op == "full":
        s["cur"] = s["max"]
    elif op in ("break", "clear"):
        s["cur"] = 0.0
    elif op == "damage":
        s["cur"] -= value
    elif op == "damagepct":
        s["cur"] -= s["max"] * value / 100
    elif op == "pierce":
        s["pierce"] = clamp(value, 0, 100)
        s["pierce_by"] = performer
    elif op == "regen":
        s["regen"] += value
        s["regen_turns"] = turns if turns and turns > 0 else 1
    elif op == "decay":
        s["decay"] += value
        s["decay_turns"] = turns if turns and turns > 0 else 1
    else:
        return None
    if s["cur"] > s["max"] and s["max"] > 0:
        s["cur"] = s["max"]
    if s["cur"] < 0:
        s["cur"] = 0.0
    if s["max"] <= 0 and s["cur"] > 0:
        s["max"] = s["cur"]
    return s


def round_step(before, cfg):
    """ShieldOnRound：穿透清掉，固定增减，按回合的再生和衰减，夹到 [0, 上限]。"""
    s = dict(before)
    s["pierce"], s["pierce_by"] = 0.0, ""
    s["cur"] += cfg["regen_per_round"] - cfg["decay_per_round"]
    if s["regen_turns"] > 0:
        s["cur"] += s["regen"]
        s["regen_turns"] -= 1
        if s["regen_turns"] == 0:
            s["regen"] = 0.0
    if s["decay_turns"] > 0:
        s["cur"] -= s["decay"]
        s["decay_turns"] -= 1
        if s["decay_turns"] == 0:
            s["decay"] = 0.0
    s["cur"] = clamp(s["cur"], 0.0, s["max"] if s["max"] > 0 else s["cur"])
    return s


def when(l):
    """日志行的时间（时:分:秒）。"""
    s = int(l.t)
    return f"{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}"


def same_state(a, b):
    if a is None or b is None:
        return False
    for k in STATE_KEYS:
        x, y = a.get(k), b.get(k)
        if isinstance(x, str) or isinstance(y, str):
            if (x or "") != (y or ""):
                return False
        elif abs(float(x or 0) - float(y or 0)) > EPS:
            return False
    return True


def fmt(s):
    return "cur=%.2f/max=%.2f（base %.2f emax %.2f regen %g×%d decay %g×%d pierce %g%s）" % (
        s["cur"], s["max"], s["base"], s["emax"], s["regen"], s["regen_turns"], s["decay"], s["decay_turns"], s["pierce"],
        f" by {s['pierce_by']}" if s.get("pierce_by") else "")


# ---------------------------------------------------------------- 日志

def shield_lines(lines):
    """shield_lite 的 SHIELD 行：(Line, dict)；JSON 解析不了的单独返回。"""
    out, bad = [], []
    for l in lines:
        if l.who == "shield_lite" and l.body.startswith("SHIELD "):
            try:
                out.append((l, json.loads(l.body[7:])))
            except ValueError:
                bad.append(l)
    return out, bad


# 自然触发：圣骑士的 Smite 带 shield_self_gain_5。p4b_combat 只记每次进存档的前 8 个技能（tests/lua/scenario/p4b_combat/lua/main.lua
# 的 skill.pre），之后的技能没有日志
NATURAL_EFFECT, NATURAL_SKILL = "shield_self_gain_5", "smite"
P4B_SKILL_LOG_MAX = 8


def skill_line(l):
    """p4b_combat 的「信息 [skill]」行：(执行者, 技能 id, effect 名字列表)，执行者写成 SHIELD 行的样子（hero:1、monster:34）；
    技能句柄为 nil、格式认不出时执行者和 id 是 None。不是这种行时返回 None。"""
    if l.who != "p4b_combat" or not l.body.startswith("信息 [skill] "):
        return None
    m = re.match(r"信息 \[skill\] dd\.actor\((hero|monster)#(\d+)[^)]*\)：(\S+) 等级 -?\d+ .*effects=\[(.*)\]$", l.body)
    if not m:
        return None, None, []
    return f"{m.group(1)}:{m.group(2)}", m.group(3), [x for x in m.group(4).split(",") if x]


def shield_ev(l, ev):
    return l.who == "shield_lite" and l.body.startswith("SHIELD ") and f'"ev":"{ev}"' in l.body


def natural_gain(lines, pos, d):
    """lines 是一个会话的日志，lines[pos] 是 SHIELD apply 行（d）。返回 (证据行, 原因, 是不是驱动的)：技能自然触发的
    shield_self_gain_5（Smite，执行者给自己加）时有证据行；是这个 effect、但条件不全时有原因；别的 effect 两者都是 None。"""
    l = lines[pos]
    key, name, skill = d.get("actor"), d.get("effect") or "", d.get("skill") or ""
    if name != NATURAL_EFFECT or d.get("performer") != key:
        return None, None, False
    who = f"{when(l)} {key} 的 {name}"
    # p4c_shield 驱动的：它的 e:apply 同步派发 effect.pre，SHIELD 行紧挨着「[drive] <effect>：」行（中间没有别的 apply、drive 行）。
    # 前后都找，驱动的不算自然获得，skill 是 smite 也不算（审核 C-Y8）
    for step in (1, -1):
        i = pos + step
        while 0 <= i < len(lines):
            x = lines[i]
            m = x.who == "p4c_shield" and re.match(r"信息 \[drive\] (shield_\w+)：", x.body)
            if m and m.group(1) == name:
                return None, f"{who}：是 p4c_shield 驱动的（{when(x)} 的 [drive] 行）", True
            if m or shield_ev(x, "apply"):
                break
            i += step
    if skill == NATURAL_SKILL:
        return [l], None, False
    if skill:
        return None, f"{who}：shield_lite 归给了 {skill}", False

    def why(text):
        return None, f"{who}：skill 为空，{text}", False

    # skill 为空（修复 F6 之前的 shield_lite 只认 skill.pre 到 skill.post 之间，游戏在技能演出时才施加 effect）：看 p4b_combat 的
    # [skill] 行，这个执行者最近一次技能是 Smite、effect 列表含这个 effect，两者之间这个执行者没有别的技能，并且在同一场战斗里
    prior = [(i, sk) for i, sk in ((i, skill_line(lines[i])) for i in range(pos)) if sk]
    if len(prior) >= P4B_SKILL_LOG_MAX:
        return why(f"之前已有 {len(prior)} 行 [skill]，p4b_combat 之后的技能不记日志，判断不了中间有没有别的技能")
    last = next(((i, sk) for i, sk in reversed(prior) if sk[0] in (key, None)), None)
    if last is None:
        return why("之前没有这个执行者的 p4b_combat [skill] 行")
    i, (performer, sid, effects) = last
    if performer is None:
        return why(f"之前最近的 [skill] 行认不出执行者（{when(lines[i])}）")
    if sid != NATURAL_SKILL:
        return why(f"这个执行者最近一次技能是 {sid}（{when(lines[i])}）")
    if name not in effects:
        return why(f"这个执行者最近一次的 {sid} 的 effect 列表里没有它（[{','.join(effects)}]）")
    if any(shield_ev(x, "battle") for x in lines[i + 1:pos]):
        return why(f"这个执行者最近一次的 {sid}（{when(lines[i])}）在上一场战斗里")
    return [lines[i], l], None, False


def tag_lines(lines, who, tag, exact=True):
    """「通过 [tag]」「失败 [tag]」行。exact 为假时 tag 是前缀（persist.tag. 之类）。"""
    ok, bad = [], []
    for l in lines:
        if l.who != who:
            continue
        m = re.match(r"(通过|失败) \[([^\]]+)\]", l.body)
        if m and (m.group(2) == tag if exact else m.group(2).startswith(tag)):
            (ok if m.group(1) == "通过" else bad).append(l)
    return ok, bad


class Checker:
    def __init__(self, log_dir, lua_log_dir=None, combat_log_dir=None):
        self.log_dir = Path(log_dir)
        self.logs = p3.LogSet(log_dir, with_probe=False)
        self.lua, self.core = self.logs.lua or [], self.logs.core or []
        self.lua_log_dir = lua_log_dir
        self.combat_log_dir = combat_log_dir
        self.sessions = p3.split_sessions(self.logs.lua)
        self.results = []
        self.cfg = read_config()
        self.tamper = self.read_tamper()
        self.windows, self.tamper_other = self.tamper_windows()

    def add(self, key, title, status, detail, required=True, acceptance=""):
        self.results.append(p3.Result(key, title, status, detail if isinstance(detail, list) else [detail], required, acceptance))

    # ---------------------------------------------------------------- 时间和 rig_tamper.log

    def run_start(self):
        start = self.logs.start_of("DDCore.log", self.core) if self.core else None
        return start[0] if start else None

    def line_dt(self, l):
        """DDCore.log 一行的日期时间（日志只有时分秒，日期取运行开始那天，跨午夜加一天）。"""
        start = self.run_start()
        if start is None:
            return None
        t = datetime.combine(start.date(), datetime.min.time()) + timedelta(seconds=l.t)
        if t < start - timedelta(seconds=5):
            t += timedelta(days=1)
        return t

    def read_tamper(self):
        records = []
        path = self.log_dir / TAMPER_LOG
        if path.is_file():
            for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
                try:
                    records.append(json.loads(raw))
                except ValueError:
                    continue
        return records

    def tamper_windows(self):
        """本次运行的 tamper 改动 → [(新哈希, 起, 止, 记录)]，以及不属于本次运行的记录条数（审核 C-N1、A-N7）。
        属于本次运行：run 等于 DDCore.log 的运行标识；run 为 null（或旧格式没有 run）的是游戏没运行时改的，属于之后启动的那次，
        这里按「早于本次启动、不早于部署」算。同一个 key 之后有 restore 记录时，改动从恢复的时刻起不再算。"""
        if not self.tamper or not self.core:
            return [], len(self.tamper)
        run = p3.LogSet.run_id(self.core)
        start = self.run_start()
        deployed = None
        if self.logs.deploy and self.logs.deploy.get("deployed_at"):
            deployed = datetime.fromtimestamp(self.logs.deploy["deployed_at"])
        windows, other = [], 0
        active = {}
        for r in self.tamper:
            t = datetime.fromtimestamp(float(r.get("time") or 0))
            rid = r.get("run")
            if rid is not None:
                mine = rid == run
            else:
                mine = start is not None and t <= start + timedelta(seconds=5) and (deployed is None or t >= deployed - timedelta(seconds=5))
            if not mine:
                other += 1
                continue
            key = r.get("key")
            if r.get("restore"):
                for w in active.pop(key, []):
                    w[2] = t
                continue
            for item in r.get("changed", []):
                m = re.search(r"->([0-9A-Fa-f]{16})", item)
                if m:
                    w = [m.group(1).upper(), t, datetime.max, r]
                    windows.append(w)
                    active.setdefault(key, []).append(w)
        return windows, other

    def expected_hash(self, text, l):
        """text 里有没有本次运行改过、在这一行的时刻仍然有效的哈希。"""
        at = self.line_dt(l)
        upper = text.upper()
        for h, start, end, _ in self.windows:
            if h in upper and (at is None or start - timedelta(seconds=2) <= at < end):
                return True
        return False

    def mod_sessions(self, mod="p4c_persist"):
        return [s for s in self.sessions if any(l.who == mod and l.body.startswith("main.lua 执行完毕") for l in s["lines"])]

    # ---------------------------------------------------------------- 日志本身

    def check_logs(self):
        title = "日志文件：同一次游戏运行、晚于部署、scenario4 方案、DDLua 默认配置"
        if self.logs.lua is None or self.logs.core is None:
            self.add("logs", title, NOLOG, f"{self.log_dir} 下缺 DDLua.log 或 DDCore.log：先 rig.py deploy scenario4，按 docs/10-p4-scenario.md 走流程，再 save-log")
            return False
        config, _, problems = self.logs.config()
        detail, id_problems = self.logs.identity("scenario4", ("DDLua.log", "DDCore.log"))
        problems += id_problems + self.logs.serial_problems()
        if self.tamper:
            detail.append(f"rig_tamper.log：{len(self.tamper)} 条记录，本次运行的改动 {len(self.windows)} 处，"
                          f"不属于本次运行的 {self.tamper_other} 条（不用）")
        self.add("logs", title, FAIL if problems else PASS, problems + [config.text if config else "没有配置行"] + detail)
        return True

    def check_mods(self):
        title = "4 个场景 Mod 都勾选并加载成功（标题、来源对得上）"
        best = None
        for s in self.sessions:
            found = {}
            for l in s["lines"]:
                m = re.match(r"发现 Lua Mod，版本 \S+，来源 (.*)（([^（）]*)），目录 (.*)$", l.body)
                if m and l.who in SCENARIO_MODS:
                    found[l.who] = (m.group(1), m.group(2))
            loaded = {l.who for l in s["lines"] if l.body.startswith("main.lua 执行完毕") and l.who in SCENARIO_MODS}
            if best is None or len(loaded) > len(best[2]):
                best = (s, found, loaded)
        if best is None:
            self.add("mods", title, MISS, "DDLua.log 里没有「[存档#N] 进入存档」：没有进过存档")
            return
        s, found, loaded = best
        wrong = [f"{mid} 的来源是 {src}（{kind}），应为 {SCENARIO_MODS[mid]}（mod_local_source）" for mid, (src, kind) in found.items()
                 if src != SCENARIO_MODS[mid] or kind != "mod_local_source"]
        detail = [s["start"].text, "加载成功：" + "、".join(sorted(loaded))]
        missing = [f"{mid}（标题 {t}）" for mid, t in SCENARIO_MODS.items() if mid not in loaded]
        if wrong:
            self.add("mods", title, FAIL, wrong + detail)
        elif missing:
            self.add("mods", title, MISS, detail + ["没有加载：" + "、".join(missing) + "。在存档的 Mod 列表里补勾，回主菜单再进一次"])
        else:
            self.add("mods", title, PASS, detail)

    def check_selfreport(self):
        """审核 C-B1：场景 Mod 写的每一行「失败 [标签]」都判失败（检查脚本别的项只按名字取了一部分标签）。"""
        bad = [l for l in self.lua if l.who in SCENARIO_MODS and l.body.startswith("失败 [")]
        title = "场景 Mod 自己报的结果没有失败（4 个 Mod 的全部「失败 [标签]」行）"
        if bad:
            mods = sorted({l.who for l in bad})
            self.add("selfreport", title, FAIL, [f"{len(bad)} 行，来自 {'、'.join(mods)}："] + [l.text for l in bad[:8]],
                     acceptance="、".join(MOD_ACCEPTANCE[m] for m in mods))
        else:
            passed = sum(1 for l in self.lua if l.who in SCENARIO_MODS and l.body.startswith("通过 ["))
            self.add("selfreport", title, PASS, f"「通过 [标签]」{passed} 行，「失败 [标签]」0 行", acceptance="#1–#5")

    # ---------------------------------------------------------------- #1 DDShield 的复刻

    def replay_shields(self):
        """逐个会话重放 SHIELD 行。返回统计和问题。"""
        r = dict(lines=0, problems=[], format=[], ops=set(), natural=[], natural_why=[], natural_driven=[], full=[], partial=[], hp_ok=0, hp_skip=[], cap_checked=0,
                 rounds=0, round1_skips=0, round_changes=[], clears=0, rules_true=0, rules_original=0, rule_bad=[], restores=[],
                 monster_absorb=0, monster_full=[], monster_partial=[], pierce_absorb=0, carrier_left=[], carrier_adds=0, carrier_removes=0,
                 zeros=[])
        prev_saves = {}
        for s in self.sessions:
            lines, bad = shield_lines(s["lines"])
            r["format"] += [l.text for l in bad]
            if not lines:
                continue
            serial = s["serial"]
            track, saves, pending, carriers, cleared = {}, {}, {}, {}, {}
            round_expect = None

            def problem(l, text):
                r["problems"].append(f"存档#{serial} {when(l)} {text}")

            def check_before(l, d):
                key = d.get("actor")
                if key not in track:
                    problem(l, f"{key} 没有 init/restore 就有 {d['ev']}")
                    return False
                if not same_state(track[key], d.get("before")):
                    problem(l, f"{key} 的 before 和上一行之后的状态不同：{fmt(d['before'])}，应为 {fmt(track[key])}")
                    return False
                return True

            def cap_ok(l, d, after):
                r["cap_checked"] += 1
                if after["max"] > 0 and after["cur"] > after["max"] + EPS:
                    problem(l, f"{d.get('actor')} 的护盾超过上限：{fmt(after)}")

            def finish_round():
                nonlocal round_expect
                if round_expect is None:
                    return
                for key, (exp, seen) in round_expect.items():
                    if not seen and not same_state(exp, track.get(key)):
                        r["problems"].append(f"存档#{serial} 第 {round_expect_no} 回合：{key} 应变成 {fmt(exp)}，但没有 round 行")
                round_expect = None

            round_expect_no = 0
            for l, d in lines:
                r["lines"] += 1
                ev = d.get("ev")
                key = d.get("actor")
                if ev != "round":
                    finish_round()
                if ev == "battle":
                    for k, guid in list(cleared.items()):
                        if carriers.get(k) == guid:
                            r["carrier_left"].append(f"存档#{serial}：{k} 的载体（guid {guid}）在战斗结束后没删")
                    cleared = {}
                elif ev == "init":
                    exp = expected_init(d, self.cfg)
                    if not same_state(exp, d.get("after")):
                        problem(l, f"{key} 的初始值 {fmt(d['after'])}，按配置应为 {fmt(exp)}")
                    track[key] = d["after"]
                elif ev == "restore":
                    track[key] = d["after"]
                    if d.get("guid") is not None:
                        carriers[key] = d.get("guid")
                    last = prev_saves.get(key)
                    r["restores"].append((serial, key, d["after"], last))
                elif ev == "apply":
                    name = d.get("effect") or ""
                    spec = spec_of(name, self.cfg)
                    if spec is None:
                        problem(l, f"effect {name} 按 DDShield 的规则解析不了，shield_lite 却结算了（{d.get('op')}）")
                    elif d.get("op") != spec["op"] or abs(float(d.get("value") or 0) - spec["value"]) > EPS or \
                            (spec["op"] in ("regen", "decay") and int(d.get("turns") or 0) != spec["turns"]):
                        problem(l, f"effect {name} 按 DDShield 的规则是 {spec['op']} {spec['value']:g}" +
                                (f"×{spec['turns']}" if spec["op"] in ("regen", "decay") else "") +
                                f"，shield_lite 解析成 {d.get('op')} {d.get('value')}" + (f"×{d.get('turns')}" if d.get("turns") else ""))
                    if not check_before(l, d):
                        track[key] = d["after"]
                        continue
                    op = spec["op"] if spec else d.get("op")
                    value = spec["value"] if spec else float(d.get("value") or 0)
                    turns = spec["turns"] if spec else d.get("turns")
                    exp = apply_op(d["before"], op, value, turns, d.get("performer") or "")
                    if exp is None:
                        problem(l, f"不认识的 op {op}")
                    elif not same_state(exp, d["after"]):
                        problem(l, f"{key} {name}（{op} {value:g}）：{fmt(d['before'])} → {fmt(d['after'])}，按 DDShield 应为 {fmt(exp)}")
                    r["ops"].add({"heal": "add", "clear": "break"}.get(op, op))
                    # 自然触发：圣骑士的 Smite 带 shield_self_gain_5，执行者给自己加（审核 C-Y8：只看 skill 非空会把跨帧的驱动当成自然触发）。
                    # skill 是 smite，或者 skill 为空而 p4b_combat 的 [skill] 行能证明来自 Smite；驱动的都不算（natural_gain）
                    evidence, why, driven = natural_gain(s["lines"], l.index - s["start"].index, d)
                    if evidence:
                        r["natural"].append(evidence)
                    elif why:
                        r["natural_driven" if driven else "natural_why"].append(f"存档#{serial} {why}")
                    cap_ok(l, d, d["after"])
                    track[key] = d["after"]
                elif ev == "zero":
                    if check_before(l, d):
                        if float(d.get("hp") if d.get("hp") is not None else 1) > EPS:
                            problem(l, f"{key} 生命 {d.get('hp')} 不是 ≤ 0，护盾却清零了")
                        elif not same_state(dict(d["before"], cur=0.0), d["after"]):
                            problem(l, f"{key} 生命 ≤ 0 时清零护盾：{fmt(d['before'])} → {fmt(d['after'])}，应只把 cur 清零")
                    r["zeros"].append(l)
                    track[key] = d["after"]
                elif ev == "absorb":
                    ok_before = check_before(l, d)
                    b, a = d["before"], d["after"]
                    damage, absorb = float(d["damage"]), float(d["absorb"])
                    pierce = b["pierce"] if b.get("pierce_by") and b.get("pierce_by") == d.get("attacker") else 0.0
                    want = min(b["cur"], damage * (1 - pierce / 100))
                    full = want >= damage - EPS
                    want_after = dict(b, cur=b["cur"] - want)
                    if d.get("hp_before") is not None and float(d["hp_before"]) <= EPS:
                        problem(l, f"{key} 生命 {d['hp_before']}（≤ 0，死门上）还在吸收：DDShield 这时已经把护盾清零")
                    if ok_before and (abs(absorb - want) > EPS or abs(float(d.get("pierce") or 0) - pierce) > EPS):
                        problem(l, f"{key} 最终伤害 {damage:g}、护盾 {b['cur']:g}、穿透 {pierce:g}%：吸收 {absorb:g}，应为 min(护盾, 伤害×(1−穿透)) = {want:g}")
                    elif ok_before and d.get("kind") != ("full" if full else "partial"):
                        problem(l, f"{key} 吸收 {absorb:g}/{damage:g} 判成 {d.get('kind')}，应为 {'full' if full else 'partial'}")
                    elif ok_before and abs(float(d.get("written")) - (damage if full else want)) > EPS:
                        problem(l, f"{key} 写给 damage.final 的 absorb 是 {d.get('written')}，应为 {damage if full else want:g}")
                    elif ok_before and not same_state(want_after, a):
                        problem(l, f"{key} 吸收后的状态 {fmt(a)}，应为 {fmt(want_after)}")
                    pending[d.get("n")] = (l, d)
                    if key.startswith("monster:"):
                        r["monster_absorb"] += 1
                    if pierce > 0:
                        r["pierce_absorb"] += 1
                    track[key] = a
                elif ev == "hit":
                    src = pending.pop(d.get("n"), None)
                    if src is None:
                        problem(l, f"hit n={d.get('n')} 前面没有对应的 absorb")
                        continue
                    damage, absorb = float(d["damage"]), float(d["absorb"])
                    hp_before, hp_after = float(d["hp_before"]), float(d["hp_after"])
                    loss = hp_before - hp_after
                    net = damage - absorb
                    monster = (key or "").startswith("monster:")
                    if d.get("kind") == "full":
                        if abs(loss) > EPS:
                            problem(l, f"{key} 全部吸收（{damage:g}），生命却从 {hp_before:g} 变成 {hp_after:g}")
                        else:
                            r["hp_ok"] += 1
                            r["full"].append(l)
                            if monster:
                                r["monster_full"].append(l)
                    elif d.get("deaths_door"):
                        r["hp_skip"].append(f"{when(l)} {key} 在死门上（游戏不扣血，只判定死亡），吸收 {absorb:g}/{damage:g}")
                    elif hp_before - net <= EPS:
                        if hp_after > EPS:
                            problem(l, f"{key} 净伤害 {net:g} 不少于生命 {hp_before:g}，生命却剩 {hp_after:g}")
                        else:
                            r["hp_skip"].append(f"{when(l)} {key} 净伤害 {net:g} 打空了生命 {hp_before:g}（{hp_after:g}），只核对了生命不为正")
                    elif abs(loss - net) > EPS:
                        problem(l, f"{key} 部分吸收：最终伤害 {damage:g} − 吸收 {absorb:g} = {net:g}，实际生命损失 {loss:g}（{hp_before:g} → {hp_after:g}）")
                    else:
                        r["hp_ok"] += 1
                        r["partial"].append(l)
                        if monster:
                            r["monster_partial"].append(l)
                elif ev == "endhit":
                    if check_before(l, d):
                        exp = dict(d["before"], pierce=0.0, pierce_by="")
                        if d["before"].get("pierce_by") != d.get("attacker") or not same_state(exp, d["after"]):
                            problem(l, f"{key} 命中结束清掉穿透：{fmt(d['before'])} → {fmt(d['after'])}")
                    track[key] = d["after"]
                elif ev == "round_begin":
                    r["rounds"] += 1
                    rnd = int(d.get("round") or 0)
                    round_expect_no = rnd
                    if rnd <= 1:
                        # 游戏只在回合号大于 1 时结算回合的 buff，DDShield 挂在那后面：第 1 回合什么都不变（审核 C-Y6 第 3 条）
                        if not d.get("skip"):
                            problem(l, f"第 {rnd} 回合不该结算（游戏只在回合号大于 1 时调用 UpdateRoundBuffs）")
                        r["round1_skips"] += 1
                        round_expect = {k: (dict(v), False) for k, v in track.items()}
                    else:
                        if d.get("skip"):
                            problem(l, f"第 {rnd} 回合被跳过了，应当结算")
                        round_expect = {k: (round_step(v, self.cfg), False) for k, v in track.items()}
                elif ev == "round":
                    if round_expect is None or key not in round_expect:
                        problem(l, f"{key} 的 round 行前面没有 round_begin（或这个角色当时没有状态）")
                        track[key] = d["after"]
                        continue
                    exp, _ = round_expect[key]
                    if not same_state(track[key], d["before"]):
                        problem(l, f"{key} 第 {d.get('round')} 回合的 before {fmt(d['before'])}，应为 {fmt(track[key])}")
                    elif not same_state(exp, d["after"]):
                        problem(l, f"{key} 第 {d.get('round')} 回合：{fmt(d['before'])} → {fmt(d['after'])}，按 DDShield 应为 {fmt(exp)}")
                    round_expect[key] = (exp, True)
                    cap_ok(l, d, d["after"])
                    b = d["before"]
                    if b["regen_turns"] > 0 or b["decay_turns"] > 0:
                        r["round_changes"].append(l)
                    track[key] = d["after"]
                elif ev == "clear":
                    check_before(l, d)
                    r["clears"] += 1
                    if key in carriers:
                        cleared[key] = carriers[key]
                    track.pop(key, None)
                elif ev == "carrier":
                    if d.get("op") == "add":
                        carriers[key] = d.get("guid")
                        r["carrier_adds"] += 1
                    elif d.get("op") == "removed":
                        r["carrier_removes"] += 1
                        if carriers.get(key) == d.get("guid"):
                            carriers.pop(key, None)
                elif ev == "save":
                    if key in track and not same_state(track[key], d.get("state")):
                        problem(l, f"{key} 存档时写进载体的状态 {fmt(d['state'])} 和当时的护盾 {fmt(track[key])} 不同")
                    saves[key] = d.get("state")
                elif ev == "rule":
                    cur = track[key]["cur"] if key in track else 0.0
                    res = d.get("result")
                    text = f"存档#{serial} {when(l)} {key} shielded 判成 {res}（日志里护盾 {d.get('cur')}，重放的护盾 {cur:g}）"
                    if res is True:
                        if cur <= EPS or abs(float(d.get("cur") or 0) - cur) > EPS:
                            r["rule_bad"].append(text)
                        r["rules_true"] += 1
                    elif res == "original":
                        # 没有护盾、城镇里交还游戏原来的判定（和 DDShield 一致，见 shield_lite 的 README）
                        if d.get("in_raid", True) and cur > EPS:
                            r["rule_bad"].append(text + "：有护盾却交还了原来的判定")
                        r["rules_original"] += 1
                    else:
                        r["rule_bad"].append(text + "：应为 true 或 original（没有护盾时交还原来的判定）")
            finish_round()
            for k, guid in cleared.items():
                if carriers.get(k) == guid:
                    r["carrier_left"].append(f"存档#{serial}：{k} 的载体（guid {guid}）在战斗结束后没删")
            prev_saves = saves
        return r

    def check_shield(self):
        acc = "#1"
        r = self.replay_shields()
        self.shield_replay = r
        if r["format"]:
            self.add("shield.formula", "SHIELD 日志能解析", FORMAT, ["SHIELD 行不是 JSON（shield_lite 改过输出？）："] + r["format"][:3], acceptance=acc)
            return
        if not r["lines"]:
            self.add("shield.formula", "护盾每一次变化都符合 DDShield 的规则（重放 SHIELD 行）", MISS,
                "没有 SHIELD 行：没有勾选「DDLua Example Shield Lite」，或没进过战斗", acceptance=acc)
            return
        self.add("shield.formula", "护盾每一次变化都符合 DDShield 的规则（effect 名字的解析、获得、上限、吸收、回合、生命 ≤ 0 清零、命中结束清穿透，逐行重放）",
            FAIL if r["problems"] else PASS, r["problems"][:8] or [f"SHIELD {r['lines']} 行，全部和独立重算一致；用到的规则：{'、'.join(sorted(r['ops']))}；"
            f"第 1 回合跳过 {r['round1_skips']} 次"], acceptance=acc)
        cap_bad = [p for p in r["problems"] if "超过上限" in p]
        self.add("shield.cap", "护盾不超过上限", FAIL if cap_bad else (PASS if r["cap_checked"] else MISS),
            cap_bad[:3] or [f"核对了 {r['cap_checked']} 次变化后的值"], acceptance=acc)
        nat = r["natural"]
        self.add("shield.natural", "技能自然触发护盾（圣骑士用 Smite 时 shield_self_gain_5 给自己加护盾，effect.pre 按名字路由到 Lua）",
            PASS if nat else MISS, [x.text for group in nat[:2] for x in group] or
            ["没有 Smite 施加的 shield_self_gain_5：队伍里带圣骑士（第 1、2 位），战斗里用 Smite"] + r["natural_why"][:3] +
            ([f"另有 {len(r['natural_driven'])} 次是 p4c_shield 驱动的，不算：{r['natural_driven'][0]}"] if r["natural_driven"] else []),
            acceptance=acc)
        hp_bad = [p for p in r["problems"] if "生命" in p and "吸收" in p]
        self.add("shield.full", "全部吸收：生命不变（游戏按格挡处理）", FAIL if any("全部吸收" in p for p in hp_bad) else
            (PASS if r["full"] else MISS), [l.text for l in r["full"][:2]] if r["full"] else
            ["还没有一次伤害被护盾全部挡下：让有护盾的圣骑士挨一次比护盾小的伤害"], acceptance=acc)
        self.add("shield.partial", "部分吸收：生命损失 = 最终伤害 − 吸收量", FAIL if any("部分吸收" in p for p in hp_bad) else
            (PASS if r["partial"] else MISS), [l.text for l in r["partial"][:2]] + r["hp_skip"][:2]
            if r["partial"] else ["还没有护盾不够挡下整次伤害的命中（打不死目标的那种）"] + r["hp_skip"][:2], acceptance=acc)
        # 怪物目标的部分吸收：DDCore 对怪物按 5FC3E0 的规则补回生命（审核 A-S2 的修正，A 的运行证据第 3 条）
        self.add("shield.monster", "怪物目标的部分吸收：生命损失 = 最终伤害 − 吸收量（p4c_shield 给每个怪物 10 点护盾）",
            PASS if r["monster_partial"] else MISS,
            [l.text for l in r["monster_partial"][:1] + r["monster_full"][:1]] if r["monster_partial"] else
            [f"怪物一侧的吸收 {r['monster_absorb']} 次，核对过的全部吸收 {len(r['monster_full'])} 次、部分吸收 0 次：p4c_shield 驱动之后（日志里「通过 [drive.N]」，开战约 5 秒），"
             "用伤害高的技能打怪物，或者连打同一个怪物两次，直到它的 10 点护盾打不住"], acceptance=acc)
        missing_ops = [op for op in REQUIRED_OPS if op not in r["ops"]]
        self.add("shield.ops", f"规则都用到了：{'、'.join(REQUIRED_OPS)}（p4c_shield 每场战斗驱动一次）", PASS if not missing_ops else MISS,
            [f"用到的：{'、'.join(sorted(r['ops']))}；穿透生效的吸收 {r['pierce_absorb']} 次，怪物一侧的吸收 {r['monster_absorb']} 次"] +
            ([f"没用到：{'、'.join(missing_ops)}（打一场战斗，开战后先别操作，等到「通过 [drive.N]」）"] if missing_ops else []), acceptance=acc)
        self.add("shield.round", "每回合的再生、衰减按 DDShield 的规则结算（第 1 回合不结算）", PASS if r["round_changes"] and not any("回合" in p for p in r["problems"]) else
            (FAIL if any("回合" in p for p in r["problems"]) else MISS),
            [l.text for l in r["round_changes"][:2]] or [f"回合 {r['rounds']} 次，没有带再生或衰减的回合变化（驱动之后要再过一个回合）"], acceptance=acc)
        self.add("shield.clear", "战斗结束清掉护盾和载体", FAIL if r["carrier_left"] else (PASS if r["clears"] else MISS),
            r["carrier_left"][:3] or ([f"清理 {r['clears']} 个角色的护盾，载体都删掉了"] if r["clears"] else ["还没有一场有护盾的战斗打完"]), acceptance=acc)
        # shielded：日志里的结果和重放的护盾一致；游戏真的按结果生效看 p4c_shield 的 rule.live（remove_if_not_active 的条目在不在，审核 C-Y4）
        live_ok, live_bad = tag_lines(self.lua, "p4c_shield", "rule.live")
        rule_status = FAIL if r["rule_bad"] or live_bad else (PASS if r["rules_true"] and r["rules_original"] and live_ok else MISS)
        detail = r["rule_bad"][:3] + [l.text for l in live_bad[:1]]
        if not detail:
            detail = [f"判定成立 {r['rules_true']} 次、交还原来的判定 {r['rules_original']} 次（只记每个角色结果变化的时候）"] + \
                     [l.text for l in live_ok[:1]] + ([] if live_ok else ["还没有 p4c_shield 的「通过 [rule.live]」：队伍里要有两个以上活着的英雄，开战后先别操作，等到「通过 [drive.N]」"])
        self.add("shield.rule", "shielded 规则：有护盾时成立、没有时交还原来的判定（和重放的护盾一致），游戏按结果留下或删掉 remove_if_not_active 的 buff",
            rule_status, detail, acceptance=acc)
        # 读档后从载体恢复的护盾，和上一次进存档最后一次存档时写进载体的一致（DDShield 读档就没有了，这是 Lua 版多出来的）
        restored = [(s, k, a, last) for s, k, a, last in r["restores"] if last is not None]
        bad = [f"存档#{s} {k} 恢复成 {fmt(a)}，上一次存档时是 {fmt(last)}" for s, k, a, last in restored if not same_state(a, last)]
        self.add("shield.persist", "读档后护盾从载体恢复，和存档时一致", FAIL if bad else (PASS if restored else MISS),
            bad[:3] or [f"恢复 {len(restored)} 次，都和上一次进存档最后写进载体的相同"] if restored or bad else
            ["战斗中有护盾时没有存档、读档（可选）"], required=False, acceptance=acc)
        self.add("shield.zero", "生命 ≤ 0 时护盾清零（DDShield 每次画血条时这样做；死门上的英雄没有护盾）",
            PASS if r["zeros"] else MISS, [l.text for l in r["zeros"][:2]] or ["没有有护盾的角色生命降到 0（死门、怪物被打死）"],
            required=False, acceptance=acc)

    # ---------------------------------------------------------------- #2 持久化

    def core_sessions(self):
        """DDCore.log 按进存档切开：序号、设备、观察到的名册/raid 写入、campaign.save（序号，提交旁路文件数，行）。"""
        out, cur = [], None
        for l in self.core:
            m = re.match(r"阶段 CampaignLoad，帧 \d+，存档序号 (\d+)", l.text)
            if m:
                cur = {"serial": int(m.group(1)), "device": None, "writes": [], "saves": [], "loads": []}
                out.append(cur)
                continue
            if cur is None:
                continue
            if l.text.startswith("阶段 CampaignUnload"):
                cur = None
                continue
            m = re.match(r"persist：进存档：设备 ([^，]+)，", l.text)
            if m:
                cur["device"] = m.group(1)
            if re.match(r"persist：观察到写存档：设备 [^，]+，persist\.(roster|raid)\.json，", l.text):
                cur["writes"].append(l)
            m = re.match(r"persist：campaign\.save #(\d+)（[^）]*）：.*；提交旁路文件 (\d+) 个", l.text)
            if m:
                cur["saves"].append((int(m.group(1)), int(m.group(2)), l))
            m = re.match(r"persist：DDLua/([\w.-]+)：(loaded|prev|empty|mismatch|unavailable)(?:（[^）]*）)?(?:：(.*))?$", l.text)
            if m:
                cur["loads"].append((cur["serial"], m.group(1), m.group(2), m.group(3) or "", l))
            m = re.match(r"persist：DDLua/([\w.-]+)：raid 核对不一致：(.*)$", l.text)
            if m:
                cur["loads"].append((cur["serial"], m.group(1), "raid_mismatch", m.group(2), l))
        return out

    def core_persist(self):
        """每次进存档 DDLua/<key> 的结果：[(序号, key, 状态, 原因, 行)]，加上 raid 核对不一致。"""
        return [x for s in self.core_sessions() for x in s["loads"]]

    def lua_saves(self):
        """DDLua 的「campaign.save #k …：写出 N 个 Mod：…」：{(存档序号, k): (N, [Mod id], 行)}。
        campaign.save 的序号 k 每次进存档从 1 数起（DDCore 在 CampaignLoad 时清零），所以按（存档序号，k）配对。"""
        out = {}
        for l in self.lua:
            m = re.match(r"存档#(\d+)$", l.who)
            mm = m and re.match(r"campaign\.save #(\d+)（.*?）：写出 (\d+) 个 Mod(?:：(.*))?$", l.body)
            if mm:
                ids = re.findall(r"(?:^| )([\w.-]+)（\d+ 字节", mm.group(3) or "")
                out[(int(m.group(1)), int(mm.group(1)))] = (int(mm.group(2)), ids, l)
        return out

    def mod_saves(self):
        """p4c_persist 每次 campaign.save 的「信息 [save] #k where=… saves=N expect=M」：[(存档序号, k, where, saves, expect, 行)]，按日志顺序。"""
        out = []
        for s in self.sessions:
            for l in s["lines"]:
                m = l.who == "p4c_persist" and re.match(r"信息 \[save\] #(\d+) where=(\w+) saves=(\d+) expect=(\d+)", l.body)
                if m:
                    out.append((s["serial"], int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), l))
        return out

    def hook_writes(self):
        """hook 报告里「观察到存档读 N 次、写 M 次」的 M（每份报告一个）。"""
        return [int(m.group(1)) for l in self.core for m in [re.search(r"persist：观察 hook \d+ 个（已装）.*观察到存档读 \d+ 次、写 (\d+) 次", l.text)] if m]

    def unexpected_loads(self):
        """F2 给的 persist.unexpected 的含义：本次运行的 tamper 解释不了的 mismatch、raid 核对不一致；unavailable；
        这个设备上写过旁路之后又读到的 empty（新战役除外）。prev 不算。返回 [(序号, key, 状态, 原因, 行, 说明)]。"""
        lua_saves = self.lua_saves()
        written = set()
        out = []
        for s in self.core_sessions():
            for serial, key, status, reason, l in s["loads"]:
                if status in ("mismatch", "raid_mismatch") and not self.expected_hash(reason, l):
                    why = "不是本次运行 tamper-persist 改过的哈希"
                    if (s["device"], key) not in written:
                        # 本次运行还没写过这个存档位的旁路就不一致：多半是这个存档在别的方案下玩过（第二段的 combat 方案、
                        # lua 方案下没勾 Mod），游戏存档往前走了、旁路没跟上（审核 R2B-3）
                        why += "；本次运行第一次读这个存档位就不一致：这个存档可能在别的方案下（combat、lua）进过，旁路落后了。" \
                               "重跑前先 rig.py reset-persist（把测试目录的旁路整个改名留底），再从第 1 步开始"
                    out.append((serial, key, status, reason, l, why))
                elif status == "unavailable":
                    out.append((serial, key, status, reason, l, "旁路不可用"))
                elif status == "empty" and "新战役" not in reason and (s["device"], key) in written:
                    out.append((serial, key, status, reason, l, "这个存档位之前写过它的旁路，读档却是 empty（旁路丢了，审核 B-S9）"))
            for k, n, _ in s["saves"]:
                if n > 0 and (s["serial"], k) in lua_saves:
                    for key in lua_saves[(s["serial"], k)][1]:
                        written.add((s["device"], key))
        return out

    def check_written(self, acc):
        """审核 B-S9 / F2 的说明：旁路在游戏里真的写了。观察到写存档却没有派发、交了数据没写、整次运行一次写入都没观察到，都判失败。"""
        title = "游戏存档时旁路真的写了（观察到写名册就派发 campaign.save，DDLua 交出的数据 DDCore 都提交了）"
        sessions = self.core_sessions()
        lua_saves = self.lua_saves()
        problems, ok = [], []
        for s in sessions:
            if s["writes"] and not s["saves"]:
                problems.append(f"存档序号 {s['serial']}：观察到写存档（{s['writes'][0].text}），但这次进存档一次 campaign.save 都没有派发")
            for k, n, l in s["saves"]:
                ls = lua_saves.get((s["serial"], k))
                if ls and ls[0] > 0 and n == 0:
                    problems.append(f"存档序号 {s['serial']} 的 campaign.save #{k}：DDLua 交了 {ls[0]} 个 Mod 的数据，DDCore 提交旁路文件 0 个：{l.text}")
                if n > 0:
                    ok.append(l)
        # 观察到的写入大小要正常（几万字节）：审核 B-B1 那样把 32 位的大小读成 64 位时是几 GB
        for s in sessions:
            for l in s["writes"]:
                m = re.search(r"，(\d+) 字节，", l.text)
                if m and int(m.group(1)) > 64 * 1024 * 1024:
                    problems.append(f"观察到的写入大小不正常：{l.text}")
        total = sum(len(s["saves"]) for s in sessions)
        writes = self.hook_writes()
        first = [s["writes"][0].text for s in sessions if s["writes"]][:1]
        # 每次 campaign.save 在主线程上花的时间（审核 B 的运行证据第 5 条）：同一次进存档里 p4c_persist 的回调到 DDLua 写出那一行
        callbacks = {(serial, k): l for serial, k, _, _, _, l in self.mod_saves()}
        spans = []
        for (serial, k), (n, ids, l) in lua_saves.items():
            cb = callbacks.get((serial, k))
            if cb is not None:
                spans.append((max(0.0, (l.t - cb.t) * 1000), f"存档#{serial} #{k}"))
        timing = [f"campaign.save 回调到写出：最长 {max(spans)[0]:.0f} ms（{max(spans)[1]}），共 {len(spans)} 次"] if spans else []
        if problems:
            self.add("persist.written", title, FAIL, problems[:5], acceptance=acc)
        elif not total:
            if len(sessions) >= 2 and writes and max(writes) == 0:
                self.add("persist.written", title, FAIL, [f"进过 {len(sessions)} 次存档，hook 报告里的存档写入一直是 0 次，campaign.save 一次都没派发："
                         "写入 hook 收不到游戏的存档写入（审核 B-B1 这一类）"], acceptance=acc)
            else:
                self.add("persist.written", title, MISS, "DDCore.log 里没有 campaign.save：进存档后没有让游戏存过档（城镇里招募、解雇一个英雄）",
                         acceptance=acc)
        else:
            exit_line = [l.text for l in self.core if l.text.startswith("persist：退到桌面")]
            self.add("persist.written", title, PASS, first + [l.text for l in ok[:1]] +
                     [f"campaign.save {total} 次，提交了旁路的 {len(ok)} 次；hook 报告里的存档写入最多 {max(writes) if writes else '?'} 次"] + timing +
                     exit_line[-1:], acceptance=acc)
        # 招募、解雇是不是各自让游戏存档（10 的「没有在游戏里确认的事」）：p4c_persist 的 [roster] 之后 5 秒内有没有 [save]
        found = {}
        for i, l in enumerate(self.lua):
            m = l.who == "p4c_persist" and re.match(r"信息 \[roster\] hero\.(recruit|dismiss)", l.body)
            if m:
                save = next((x for x in self.lua[i + 1:] if x.who == "p4c_persist" and x.body.startswith("信息 [save]")), None)
                gap = (save.t - l.t) if save is not None else None
                found.setdefault(m.group(1), []).append(f"{when(l)} hero.{m.group(1)}：" +
                    (f"{gap:.1f} 秒后存档（{save.body[9:40]}…）" if gap is not None and 0 <= gap <= 5 else "5 秒内没有存档"))
        self.add("persist.roster_ops", "招募、解雇各自让游戏存一次档（写名册，派发 campaign.save）",
                 PASS if "recruit" in found and "dismiss" in found else MISS,
                 [x for v in found.values() for x in v[:2]] or ["这次运行没有招募或解雇（第 2、10 步）"], required=False, acceptance=acc)

    def check_persist(self):
        acc = "#2"
        lines = self.lua
        for where, title, need_monster in (("town", "城镇里存档、读档后，脚本 Buff 的状态、guid、id 一致", False),
                                           ("corridor", "地牢走廊里存档、读档后一致（raid 核对之后）", False),
                                           ("battle", "战斗中存档、读档后一致（英雄的读档时、怪物的 raid 核对之后）", True)):
            ok, bad = tag_lines(lines, "p4c_persist", f"persist.{where}")
            counted = []
            for l in ok:
                m = re.search(r"载体 (\d+) 条一致（英雄 (\d+)，怪物 (\d+)）", l.body)
                if m and int(m.group(1)) > 0 and (not need_monster or int(m.group(3)) > 0):
                    counted.append(l)
            if bad:
                self.add(f"persist.{where}", title, FAIL, [l.text for l in bad[:3]], acceptance=acc)
            elif counted:
                self.add(f"persist.{where}", title, PASS, [l.text for l in counted[:2]], acceptance=acc)
            else:
                hint = {"town": "城镇里等「城镇的载体加好了」之后招募一个英雄（游戏存档），退回主菜单再读这个存档",
                        "corridor": "地牢里走几步（每步存档），退回主菜单再读，进去后等 10 秒",
                        "battle": "战斗里等「战斗的载体加好了」、再等一个角色行动完，退回主菜单再读，进去后等 10 秒"}[where]
                self.add(f"persist.{where}", title, MISS, [l.text for l in ok[:1]] + [hint], acceptance=acc)
        ok, bad = tag_lines(lines, "p4c_persist", "persist.tag.", exact=False)
        self.add("persist.tag", "读档后 DDCore 的标签（mine、tag）也恢复了", FAIL if bad else (PASS if ok else MISS),
            [l.text for l in (bad or ok)[:3]] or ["没有读过带载体的存档"], acceptance=acc)
        self.check_written(acc)
        self.check_store_continuity(acc)
        self.check_mismatch(acc)

    def check_store_continuity(self, acc):
        title = ("dd.store 随存档：读档时的 saves、最后一次存档的序号和场合，loaded 等于上一次进存档最后写的，prev 等于本次运行里更早写的那一次；"
                 "DDCore 和 DDLua 报的状态相同")
        problems, compared = [], []
        core = {(serial, key): (status, reason) for serial, key, status, reason, _ in self.core_persist() if status != "raid_mismatch"}
        last_save = None
        earlier = []  # 之前各次进存档的 [save]：(saves, "#k/where", expect, 存档序号)
        for s in self.mod_sessions():
            store = next((re.match(r"信息 \[store\] status=(\w+) loads=(\d+) saves=(\S+) last=(\S+) expect=(\d+)", l.body)
                          for l in s["lines"] if l.who == "p4c_persist" and l.body.startswith("信息 [store]")), None)
            if store:
                status = store.group(1)
                got = (store.group(3), store.group(4), store.group(5))
                cs = core.get((s["serial"], "p4c_persist"))
                if cs and cs[0] != status and not (cs[0] == "loaded" and status == "mismatch"):
                    problems.append(f"存档#{s['serial']}：DDCore 报 {cs[0]}，DDLua 的 dd.store_status() 是 {status}")
                if last_save and status == "loaded":
                    want = (last_save[0], last_save[1], last_save[2])
                    text = f"存档#{s['serial']} 读到 saves={got[0]} last={got[1]} expect={got[2]}，上一次进存档最后写的是 saves={want[0]} {want[1]} expect={want[2]}"
                    compared.append(text)
                    if got != want:
                        problems.append(text)
                elif status == "prev":
                    # 上一代是当前一代之前写的那一份：不是上一次进存档的最后一次（后台写盘还会合并连续的存档），按 last 在本次运行
                    # 更早的存档里找，saves、场合、expect 都要对上（审核 R2B-1：原来和最后一次比，读到上一代必然判失败）
                    # 每次存档 saves 都加一，上一代不会等于最后一次存档；等于的话 DDLua 给的多半是当前一代
                    match = next((e for e in reversed(earlier) if (e[0], e[1], e[2]) == got), None)
                    text = f"存档#{s['serial']} 读到上一代 saves={got[0]} last={got[1]} expect={got[2]}"
                    if match and last_save and match is last_save:
                        problems.append(text + "，就是上一次进存档最后写的那一次：给的不是上一代")
                    elif match:
                        compared.append(text + f"，和存档#{match[3]} 那次进存档写的一致（上一次进存档最后写的是 saves={last_save[0] if last_save else '?'}）")
                    elif earlier:
                        problems.append(text + "，本次运行之前写过的存档里没有这一次")
                    else:
                        compared.append(text + "：本次运行还没写过，不比较")
                elif last_save and status == "empty" and "新战役" not in ((cs or ("", ""))[1]):
                    problems.append(f"存档#{s['serial']} 的 dd.store 是 empty，上一次进存档写过（saves={last_save[0]}）：旁路丢了？")
            saves = [re.match(r"信息 \[save\] #(\d+) where=(\w+) saves=(\d+) expect=(\d+)", l.body) for l in s["lines"]
                     if l.who == "p4c_persist" and l.body.startswith("信息 [save]")]
            saves = [(m.group(3), f"#{m.group(1)}/{m.group(2)}", m.group(4), s["serial"]) for m in saves if m]
            earlier += saves
            if saves:
                last_save = saves[-1]
        if problems:
            self.add("persist.store", title, FAIL, problems[:5], acceptance=acc)
        elif compared:
            prevs = [c for c in compared if "上一代" in c]
            self.add("persist.store", title, PASS, compared[:4] + [c for c in prevs if c not in compared[:4]][:1], acceptance=acc)
        else:
            self.add("persist.store", title, MISS, "没有「存档 → 退回主菜单 → 读档」的记录", acceptance=acc)

    def check_mismatch(self, acc):
        records = self.core_persist()
        title = "旁路和存档不一致时被发现并报告（tamper-persist 改过的旁路：读档时 mismatch，dd.store 是空表，名册里带状态的标记一条都没恢复）"
        expected = [e for e in records if e[2] in ("mismatch", "raid_mismatch") and self.expected_hash(e[3], e[4])]
        if not self.windows:
            why = f"（rig_tamper.log 里有 {self.tamper_other} 条记录，都不属于本次运行）" if self.tamper_other else ""
            self.add("persist.mismatch", title, MISS, f"没有本次运行的 tamper-persist 记录{why}：按 10-p4-scenario.md 第 11 步，游戏停在主菜单时运行 "
                     "rig.py tamper-persist p4c_persist，再读档", acceptance=acc)
        else:
            mine = [e for e in expected if e[1] == "p4c_persist"]
            serials = {e[0] for e in mine}
            ok, bad = tag_lines(self.lua, "p4c_persist", "persist.mismatch")
            ok_here, empty_marks = [], []
            for l in ok:
                if not any(s["serial"] in serials and l in s["lines"] for s in self.sessions):
                    continue
                m = re.search(r"名册里的 p4c_mark 载体 (\d+) 条，状态是空表的 (\d+) 条", l.body)
                if m and (int(m.group(1)) == 0 or m.group(1) != m.group(2)):
                    empty_marks.append(l)  # 0 条时「状态不恢复」是空判（审核 C-Y5）
                else:
                    ok_here.append(l)
            if bad or empty_marks:
                self.add("persist.mismatch", title, FAIL, [l.text for l in (bad + empty_marks)[:3]] +
                         (["名册里没有 p4c_mark：「标记的状态不恢复」没有核对到（第 10 步要等城镇的载体加好、再招募解雇一次让游戏存档）"] if empty_marks else []),
                         acceptance=acc)
            elif mine and ok_here:
                self.add("persist.mismatch", title, PASS, [e[4].text for e in mine[:1]] + [l.text for l in ok_here[:1]], acceptance=acc)
            elif mine:
                self.add("persist.mismatch", title, FAIL, [e[4].text for e in mine[:1]] + ["DDCore 报了不一致，p4c_persist 没有「通过 [persist.mismatch]」"],
                    acceptance=acc)
            else:
                after = self.loaded_after_tamper()
                if after:
                    self.add("persist.mismatch", title, FAIL, [f"改过旁路之后读档仍是 {after[2]}：{after[4].text}"], acceptance=acc)
                else:
                    self.add("persist.mismatch", title, MISS, [f"本次运行改过旁路（{self.windows[0][3].get('changed')}），之后没有读这个存档"],
                        acceptance=acc)
        unexpected = self.unexpected_loads()
        self.add("persist.unexpected", "没有意料之外的不一致、unavailable、写过之后的 empty（只有本次运行改过的旁路才不一致；prev 不算）",
            FAIL if unexpected else PASS,
            [f"存档#{s}：DDLua/{k} {st}：{r}（{why}）" for s, k, st, r, _, why in unexpected[:4]] or
            [f"DDLua 的旁路读档 {len(records)} 次，不一致 {len(expected)} 次都对得上本次运行 rig_tamper.log 改过的哈希"], acceptance=acc)
        # 可选：--current-only 只改当前一代，上一代一致时读档是 prev（审核 B 的运行证据第 6 条 a）
        prevs = [e for e in records if e[2] == "prev"]
        current_only = [w for w in self.windows if w[3].get("current_only")]
        self.add("persist.prev", "只改当前一代（tamper-persist --current-only）时读到上一代（prev）", PASS if prevs else MISS,
                 [e[4].text for e in prevs[:1]] or ["可选：见 10-p4-scenario.md 的可选部分 D" + ("（本次运行用过 --current-only，但没有读到 prev）" if current_only else "")],
                 required=False, acceptance=acc)

    def loaded_after_tamper(self):
        """改过旁路之后（在改动仍然有效的时段里）p4c_persist 的读档结果是 loaded 的那一次（--current-only 时 prev 是预期的）。"""
        if not self.windows:
            return None
        for serial, key, status, reason, l in self.core_persist():
            at = self.line_dt(l)
            if key != "p4c_persist" or at is None:
                continue
            latest = None
            for _, start, end, rec in self.windows:
                if rec.get("key") == "p4c_persist" and start <= at < end and (latest is None or start >= latest[0]):
                    latest = (start, rec)
            if latest and (status == "loaded" or (status == "prev" and not latest[1].get("current_only"))):
                return (serial, key, status, reason, l)
        return None

    # ---------------------------------------------------------------- #3 边界

    def check_boundaries(self):
        acc = "#3"
        rows = (
            ("boundary.stack", "同一个 id 加两次是两条，各自的状态", "stack", "城镇里进存档 3 秒后 p4c_persist 自己做"),
            ("boundary.battle_end", "战斗结束时游戏删掉的载体（remove_on_battle_complete）调用 on_remove", "bend.remove",
             "打完一场 p4c_persist 加过载体的战斗（日志里「battle.add」，开战 1～2 秒）"),
            ("boundary.monster", "怪物身上的脚本 Buff 状态随战斗丢弃（on_remove 收到 gone）", "monster.gone", "同上，战斗结束后等 2 秒"),
            ("boundary.town_visit", "回城清理：游戏删掉队伍英雄身上的载体，on_remove 收到 town_visit", "town_visit",
             "结束这次探索回到城镇（p4c_persist 的载体在队伍英雄身上）"),
        )
        for key, title, tag, hint in rows:
            ok, bad = tag_lines(self.lua, "p4c_persist", tag)
            self.add(key, title, FAIL if bad else (PASS if ok else MISS), [l.text for l in (bad or ok)[:2]] or [hint], acceptance=acc)
        # 撤退（审核 A 的运行证据第 6 条）：游戏删掉 remove_on_battle_complete 的载体时 on_remove 照样调用；不删的话记下来（载体留到回城）
        ok, bad = tag_lines(self.lua, "p4c_persist", "bend.retreat")
        kept = [l for l in self.lua if l.who == "p4c_persist" and l.body.startswith("信息 [bend.after_end]") and "撤退" in l.body]
        title = "撤退时游戏删掉的载体调用 on_remove（没有怪物死亡就结束的战斗算撤退）"
        if bad:
            self.add("boundary.retreat", title, FAIL, [l.text for l in bad[:2]], acceptance=acc)
        elif ok:
            self.add("boundary.retreat", title, PASS, [l.text for l in ok[:1]], acceptance=acc)
        elif kept:
            self.add("boundary.retreat", title, PASS, [l.text for l in kept[:1]] + ["撤退时游戏没有按 remove_on_battle_complete 删载体，载体留到回城清理（记下这个事实）"],
                     acceptance=acc)
        else:
            self.add("boundary.retreat", title, MISS, "还没有撤退过：第二场战斗开战后不攻击，等到「battle.add」再撤退（撤退失败就下回合再撤）", acceptance=acc)
        # buff.remove 的原因和 on_remove 收到的一致；列出这次出现过的原因（审核 A 的运行证据第 4 条）
        ok, bad = tag_lines(self.lua, "p4c_persist", "remove.match")
        seen = set()
        for l in self.lua:
            if l.who == "p4b_combat":
                m = re.search(r"buff\.remove 的原因 \[([^\]]*)\]", l.body)
                if m:
                    seen |= {x.split("=")[0] for x in m.group(1).split()}
            if l.who == "p4c_persist":
                m = re.search(r"on_remove 收到 (\w+)", l.body)
                if m:
                    seen.add(m.group(1))
        detail = [f"出现过的原因：{'、'.join(sorted(seen)) or '（没有汇总行）'}；没出现过的：{'、'.join(r for r in REMOVE_REASONS if r not in seen)}"]
        self.add("buff.reasons", "buff.remove 的原因和脚本 Buff 的 on_remove 对得上（各种原因出现了哪些）", FAIL if bad else (PASS if ok else MISS),
                 [l.text for l in bad[:2]] + detail + [l.text for l in ok[:3]], acceptance=acc)

    # ---------------------------------------------------------------- #4 排队、写路径、崩溃、guid

    def check_runtime(self):
        acc = "#4"
        done = {w: tag_lines(self.lua, "p4b_combat", f"queue.done.{w}") for w in WHERES}
        queue_bad = [l for w in done for l in done[w][1]] + tag_lines(self.lua, "p4b_combat", "queue.", exact=False)[1]
        queued_adds = [l for l in self.lua if l.who == "p4c_persist" and l.body.startswith("信息 [add.done]")]
        have = [w for w in done if done[w][0]]
        title = "事件回调里的写操作排到下一帧执行，done 回调拿到的 guid 就是游戏里那一条"
        if queue_bad:
            self.add("queue", title, FAIL, [l.text for l in queue_bad[:3]], acceptance=acc)
        elif len(have) == 3:
            self.add("queue", title, PASS, [done[w][0][0].text for w in have] + [l.text for l in queued_adds[:1]], acceptance=acc)
        else:
            self.add("queue", title, MISS, [done[w][0][0].text for w in have] + [f"p4b_combat 还缺 {'、'.join(w for w in done if not done[w][0])} 那一轮"],
                     acceptance=acc)
        # 当前构建的 combat 写路径（审核 A 的运行证据第 1 条）：城镇、走廊、战斗各一遍立即添加、排队、按标签和 guid 删除、施加 effect，
        # 战斗里怪物上的增删
        missing, fails = [], []
        for w in WHERES:
            for tag in WRITE_TAGS + (BATTLE_WRITE_TAGS if w == "battle" else ()):
                ok, bad = tag_lines(self.lua, "p4b_combat", f"{tag}.{w}")
                fails += bad
                if not ok:
                    missing.append(f"{tag}.{w}")
        title = "combat 写路径在三个场合都走过（立即添加、脚本 Buff、叠加、排队和 done、按标签和 guid 删除、施加 effect、规则结果；战斗里怪物上的增删）"
        if fails:
            self.add("combat.writes", title, FAIL, [l.text for l in fails[:3]], acceptance=acc)
        elif missing:
            self.add("combat.writes", title, MISS, ["还缺：" + "、".join(missing[:12])], acceptance=acc)
        else:
            self.add("combat.writes", title, PASS, f"p4b_combat 的 {len(WRITE_TAGS) * 3 + len(BATTLE_WRITE_TAGS)} 项都通过", acceptance=acc)
        drops = [l for l in self.lua if "脚本 Buff 状态丢弃：" in l.body and l.who in SCENARIO_MODS]
        restores = [l for l in self.lua if re.search(r"脚本 Buff 状态（(英雄|怪物)）：从旁路恢复 [1-9]", l.body)]
        noadd_ok, noadd_bad = tag_lines(self.lua, "p4c_persist", "restore.noadd")
        title = "读档后脚本 Buff 的状态按（角色，组 guid）恢复，没有对不上而丢弃的；读档恢复的条目不派发 buff.add"
        if drops or noadd_bad:
            self.add("guid", title, FAIL, [l.text for l in (drops + noadd_bad)[:3]], acceptance=acc)
        elif restores and noadd_ok:
            self.add("guid", title, PASS, [l.text for l in restores[:2] + noadd_ok[:1]], acceptance=acc)
        else:
            self.add("guid", title, MISS, [l.text for l in restores[:1]] + ["没有读过带脚本 Buff 状态的存档，或读档后没等 10 秒（p4c_persist 的 restore.noadd）"],
                     acceptance=acc)
        # 加载线程上的 buff.*（延迟事件，审核 A 的运行证据第 2 条）：静态上可能，这条路径之前从没跑过；出现了记下来
        deferred = [l for l in self.lua if l.who == "p4b_combat" and l.body.startswith("信息 [buff.deferred]")]
        self.add("buff.deferred", "加载线程上的 buff.* 作为延迟事件派发（走廊、战斗中读档时）", PASS if deferred else MISS,
                 [l.text for l in deferred[:2]] or ["这次运行没有出现延迟的 buff.* 事件（游戏可能根本不在加载线程上增删 buff；走廊、战斗中读档是必做步骤，没有出现不算失败）"],
                 required=False, acceptance=acc)
        crash = [l for l in self.lua if re.search(r"损坏|原生异常|崩溃", l.body)]
        crash += [l for l in self.core if re.search(r"规则统计.*崩溃 [1-9]", l.text)]
        exit_line = [l for l in self.core if l.text.startswith("persist：退到桌面")]
        title = "整个场景没有崩溃（Lua 没有 VM 损坏、规则回调没有崩溃），从菜单正常退出"
        if crash:
            self.add("crash", title, FAIL, [l.text for l in crash[:3]], acceptance=acc)
        elif exit_line:
            self.add("crash", title, PASS, [exit_line[-1].text], acceptance=acc)
        else:
            # save-log 要求游戏已经退出；没有这一行就是没从菜单正常退出（崩溃、强杀、关窗口），审核 C-Y8
            self.add("crash", title, FAIL, "DDCore.log 没有「persist：退到桌面」：游戏不是从菜单正常退出的（崩溃、强杀、关窗口）。"
                     "如果是检查时游戏还开着，先从菜单退出再 save-log", acceptance=acc)
        self.check_errors(acc)

    def check_errors(self, acc):
        """DDLua.log 的 WARN 和 ERROR（场景 Mod 的「失败 [」由 selfreport 判）、DDCore.log 的 WARN 和 ERROR，
        本次运行改过的旁路报的不一致除外（审核 C-Y8：WARN 以前不查）。"""
        bad = []
        for l in self.lua:
            if l.text.startswith("\t"):
                continue
            if l.level == "ERROR" and not (l.who in SCENARIO_MODS and l.body.startswith("失败 [")):
                bad.append("DDLua.log：" + l.text)
            elif l.level == "WARN" and not (("mismatch" in l.text or "raid 核对不一致" in l.text) and self.expected_hash(l.text, l)):
                bad.append("DDLua.log：" + l.text)
            elif l.who in SCENARIO_MODS and l.body.startswith("日志限速："):
                bad.append("DDLua.log：" + l.text + "（场景 Mod 的日志被限速丢弃）")
        for l in self.core:
            if l.level in ("WARN", "ERROR"):
                if not (("mismatch" in l.text or "raid 核对不一致" in l.text) and self.expected_hash(l.text, l)):
                    bad.append("DDCore.log：" + l.text)
        self.add("errors", "日志里没有意外的 WARN、ERROR（本次运行改过的旁路报的不一致除外）", FAIL if bad else PASS, bad[:6] or "没有", acceptance=acc)

    # ---------------------------------------------------------------- #5 effect 和规则

    def check_effects_rules(self):
        acc = "#5"
        problems, ok_lines, missing = [], [], []
        for w in WHERES:
            ok, bad = tag_lines(self.lua, "p4b_combat", f"effect.{w}")
            problems += bad
            ok_lines += ok[:1]
            if not ok:
                missing.append(w)
        drive = tag_lines(self.lua, "p4c_shield", "drive.", exact=False)
        problems += drive[1]
        title = "从 Lua 施加 effect：城镇里被拒绝并给出原因，走廊、战斗里生效"
        if problems:
            self.add("effect", title, FAIL, [l.text for l in problems[:3]], acceptance=acc)
        elif not missing:
            self.add("effect", title, PASS, [l.text for l in ok_lines] + [l.text for l in drive[0][:1]], acceptance=acc)
        else:
            self.add("effect", title, MISS, [l.text for l in ok_lines] + [f"p4b_combat 还缺 {'、'.join(missing)} 那一轮"], acceptance=acc)
        # 规则按 Lua 的结果生效（审核 C-Y4）：p4b_live 先返回真再返回假，remove_if_not_active 的条目在游戏刷新属性时留下或被删掉，
        # 三个场合各一遍；is_false_rule 的那条反过来。只数判定次数（旧做法）证明不了结果被用上
        live = {w: tag_lines(self.lua, "p4b_combat", f"rule.live.{w}") for w in WHERES}
        readonly, ro_bad = tag_lines(self.lua, "p4b_combat", "rule.readonly")
        bad = ro_bad + [l for w in live for l in live[w][1]]
        title = "规则扩展在城镇、走廊、战斗里按 Lua 的结果生效（Lua 返回真时 remove_if_not_active 的条目留下、返回假时被游戏删掉，is_false_rule 反过来；规则回调只读）"
        detail = [live[w][0][0].text for w in WHERES if live[w][0]] + [l.text for l in readonly[:1]]
        if bad:
            self.add("rule", title, FAIL, [l.text for l in bad[:3]], acceptance=acc)
        elif all(live[w][0] for w in WHERES) and readonly:
            self.add("rule", title, PASS, detail, acceptance=acc)
        else:
            self.add("rule", title, MISS, detail + ["还缺的场合：" + "、".join(w for w in WHERES if not live[w][0])], acceptance=acc)
        # 规则判定的单帧峰值（审核 A 的运行证据第 5 条、C 的 U4）：退出存档时 DDCore 写一行统计，峰值不超过 combat_frame_ms
        stats = []
        for l in self.core:
            m = re.match(r"规则统计（退出存档时）：命中 (\d+) 次（交还原函数 (\d+) 次），跳过 (\d+) 次，崩溃 (\d+) 次，累计 ([\d.]+) ms，单次最长 ([\d.]+) us，"
                         r"单帧峰值 ([\d.]+) us / (\d+) 次", l.text)
            if m:
                stats.append((float(m.group(7)), int(m.group(8)), int(m.group(4)), float(m.group(5)), l))
        config = next((l for l in self.lua if l.who == "DDLua" and l.body.startswith("配置")), None)
        m = config and re.search(r"combat_frame_ms=(\d+)", config.body)
        limit_ms = int(m.group(1)) if m else DEFAULT_FRAME_MS
        title = f"规则判定的耗时：每次退出存档的「规则统计」里单帧峰值不超过 combat_frame_ms（{limit_ms} 毫秒），没有崩溃"
        if not stats:
            self.add("rule.stats", title, MISS, "DDCore.log 里没有「规则统计（退出存档时）」：进过存档、退回过主菜单才有", acceptance=acc)
        else:
            peak = max(stats, key=lambda x: x[0])
            over = [s for s in stats if s[0] > limit_ms * 1000 or s[2] > 0]
            self.add("rule.stats", title, FAIL if over else PASS,
                     [s[4].text for s in over[:2]] or [f"{len(stats)} 次退出存档，单帧峰值最大 {peak[0]:g} us（{peak[1]} 次判定），累计最多 "
                     f"{max(s[3] for s in stats):g} ms", peak[4].text], acceptance=acc)

    # ---------------------------------------------------------------- #6 没有 Lua Mod 时

    def check_no_lua(self):
        acc = "#6"
        reports = self.logs.hook_reports()
        base = reports.get("AssetsReady")
        title = "没有 Lua Mod 时（主菜单）：hook 报告 1 + 4 个目标（每帧 hook 加持久化观察 hook）、语义 hook 0、订阅 0，DDLua 不建 VM"
        first_enter = next((l.index for l in self.lua if re.match(r"存档#\d+$", l.who) and l.body.startswith("进入存档")), len(self.lua))
        early_vm = [l for l in self.lua[:first_enter] if "加载完成：" in l.body or "订阅 lifecycle TICK" in l.body]
        if not base:
            self.add("hooks.menu", title, FAIL, "DDCore.log 里没有 AssetsReady 的 hook 报告", acceptance=acc)
        else:
            text = f"AssetsReady：共 {base[0]} 个目标（持久化观察 hook {base[5]} 个），listener {base[1]}，语义 hook {base[2]}，订阅 {base[3]}"
            ok = base[5] == p3.PERSIST_OBSERVE_HOOKS and base[0] == 1 + base[5] and base[2] == 0 and base[3] == 0 and not early_vm
            self.add("hooks.menu", title, PASS if ok else FAIL, [text] + [l.text for l in early_vm[:2]], acceptance=acc)
        # 可选：同一次运行里进一个没有 Lua Mod 的存档，CampaignLoad 的报告和主菜单相同
        nolua = [s for s in self.sessions if any(l.body.startswith("没有可加载的 Lua Mod") for l in s["lines"])]
        loads = {k[1]: v for k, v in reports.items() if isinstance(k, tuple)}
        title2 = "没有 Lua Mod 的存档：不建 VM，CampaignLoad 的 hook 报告和主菜单相同（原版行为）"
        if nolua and base:
            bad = [s for s in nolua if s["serial"] in loads and loads[s["serial"]][:4] != base[:4]]
            self.add("hooks.nolua", title2, FAIL if bad else PASS, [f"存档#{s['serial']}：{loads[s['serial']][4] if s['serial'] in loads else '没有 hook 报告'}"
                for s in nolua[:2]] + [f"主菜单：{base[4]}"], required=False, acceptance=acc)
        elif self.lua_log_dir:
            c = p3.Checker(self.lua_log_dir, self.lua_log_dir)
            c.check_lua_profile()
            r = c.results[-1]
            self.add("hooks.nolua", title2 + "（lua 方案的运行）", r.status, r.detail, required=False, acceptance=acc)
        else:
            self.add("hooks.nolua", title2, MISS, "这次运行没有进过不带 Lua Mod 的存档（可选：进存档 B，或给 --lua-log-dir）", required=False, acceptance=acc)

    # ---------------------------------------------------------------- 可选：死门上的吸收、第二段（combat 方案）

    def check_deaths_door(self):
        """DDCore 的 damage.final 在死门上：部分吸收不补生命（游戏这时不扣血），全部吸收按格挡处理（审核 A 的运行证据第 3 条）。"""
        rows = {}
        for l in self.lua:
            m = l.who == "p4c_shield" and re.match(r"信息 \[deathsdoor\] kind=(\w+) .*hp_before=(-?[\d.]+) hp_after=(-?[\d.]+)", l.body)
            if m:
                rows.setdefault(m.group(1), []).append((float(m.group(2)), float(m.group(3)), l))
        bad = [l.text for k in rows for b, a, l in rows[k] if abs(a - b) > EPS]
        title = "死门上的吸收：部分吸收不补生命、全部吸收按格挡处理（生命都不变；全部吸收的那一下应显示 Blocked、不判定死亡）"
        if bad:
            self.add("damage.deaths_door", title, FAIL, bad[:2], required=False, acceptance="#1")
        elif "partial" in rows and "full" in rows:
            self.add("damage.deaths_door", title, PASS, [rows["partial"][0][2].text, rows["full"][0][2].text], required=False, acceptance="#1")
        else:
            self.add("damage.deaths_door", title, MISS, "没有英雄在死门上挨打两次（可选，见 10 的可选部分 E）", required=False, acceptance="#1")

    def check_combat_run(self):
        """第二段：combat 方案重跑 test_combat（审核 A-S4 改成按倍率核对之后，城镇、走廊、战斗里都要核对一遍）。
        第二段是必做的（审核 R2B-2）：没给 --combat-log-dir 时这一项没有触发，退出码 2。"""
        title = "第二段（另一次运行，必做）：combat 方案的 test_combat 在城镇、走廊、战斗里 STACK 按倍率核对通过，DMGCHECK 全部、部分吸收都对，日志没有 WARN"
        if not self.combat_log_dir:
            self.add("combat.stack", title, MISS, "没有给 --combat-log-dir：第二段（combat 方案另跑一次）是必做的，见 10-p4-scenario.md 的「第二段」；"
                     "做完后两个目录一起检查", acceptance="#5")
            return
        d = Path(self.combat_log_dir)
        logs = p3.LogSet(d, with_probe=False)
        tc = p3.read_log(d / "test_combat.log")
        if tc is None or logs.core is None:
            self.add("combat.stack", title, NOLOG, f"{d} 下缺 test_combat.log 或 DDCore.log", acceptance="#5")
            return
        problems = []
        _, id_problems = logs.identity("combat", ("DDCore.log",))
        problems += id_problems
        # test_combat.log 和 DDCore.log 是同一次运行（运行标识、开始时间），不是上一次运行留下的（审核 A-N7）
        core_id, tc_id = p3.LogSet.run_id(logs.core), p3.LogSet.run_id(tc)
        if core_id != tc_id:
            problems.append(f"test_combat.log 的运行标识 {tc_id} 和 DDCore.log 的 {core_id} 不同：是别的运行留下的")
        cs, ts = logs.start_of("DDCore.log", logs.core), logs.start_of("test_combat.log", tc)
        if cs and ts and abs((ts[0] - cs[0]).total_seconds()) > p3.SAME_RUN_SECONDS:
            problems.append(f"test_combat.log 开始于 {p3.text_time(ts[0])}，DDCore.log 开始于 {p3.text_time(cs[0])}，不是同一次运行")
        found = []
        for w in WHERES:
            start = next((l.index for l in tc if l.body.startswith(f"ROUND where={w} ")), None)
            if start is None:
                problems.append(f"没有 where={w} 的一轮")
                continue
            stack = next((l for l in tc[start:] if l.body.startswith("STACK ")), None)
            done = next((l for l in tc[start:] if l.body.startswith(f"ROUNDDONE where={w}")), None)
            for name, l in (("STACK", stack), ("ROUNDDONE", done)):
                if l is None:
                    problems.append(f"where={w} 没有 {name}")
                elif l.level != "INFO":
                    problems.append(l.text)
            if stack is not None:
                found.append(stack.text)
        for kind in ("full", "partial"):
            l = next((l for l in tc if l.body.startswith(f"DMGCHECK kind={kind}")), None)
            if l is None:
                problems.append(f"没有 DMGCHECK kind={kind}")
            elif l.level != "INFO":
                problems.append(l.text)
        warns = [l.text for l in tc if l.level in ("WARN", "ERROR")]
        problems += [w for w in warns if w not in problems][:3]
        missing_only = problems and all(p.startswith("没有") for p in problems)
        self.add("combat.stack", title, MISS if missing_only else (FAIL if problems else PASS), problems[:6] or found, acceptance="#5")

    # ---------------------------------------------------------------- 需要目测

    def check_look(self):
        r = getattr(self, "shield_replay", None) or {}
        adds, removes = r.get("carrier_adds", 0), r.get("carrier_removes", 0)
        self.add("look.tray", "有护盾的英雄头顶 tray 里出现载体 Buff 的图标，护盾打空（或战斗结束）后消失（审核 C-Y9）", LOOK,
                 [f"日志里载体加了 {adds} 次、删了 {removes} 次（SHIELD 的 carrier 行）。第 6 步看到的结果（出现 / 不出现 / 出现在哪一组）写进交给 agent 的说明；"
                  "不出现时 shield_lite 的载体要改数值或换 stat"], required=False, acceptance="#1")
        full = r.get("full", [])
        self.add("look.blocked", "护盾全部挡下时界面弹「Blocked」（生命不变已由 shield.full 核对）", LOOK,
                 [f"全部吸收 {len(full)} 次" + (f"，第一次在 {when(full[0])}" if full else "") + "。第 6 步看到的结果写进交给 agent 的说明"],
                 required=False, acceptance="#1")

    def run(self):
        if not self.check_logs():
            return self.results
        self.check_mods()
        self.check_selfreport()
        self.check_shield()
        self.check_persist()
        self.check_boundaries()
        self.check_runtime()
        self.check_effects_rules()
        self.check_no_lua()
        self.check_deaths_door()
        self.check_combat_run()
        self.check_look()
        return self.results


def report(results, out=sys.stdout):
    code = p3.report(results, out)
    look = [r for r in results if r.status == LOOK]
    if look:
        print("需要目测：" + "、".join(r.key for r in look) + "（看画面，结果写进交给 agent 的说明；不影响退出码）", file=out)
    return code


# ---------------------------------------------------------------- 自测

def statuses(log_dir, lua_dir=None, combat_dir=None):
    return {r.key: r.status for r in Checker(log_dir, lua_dir, combat_dir).run()}


def exit_code(log_dir, combat_dir=None):
    """命令行的退出码（输出丢掉）。"""
    return report(Checker(log_dir, None, combat_dir).run(), io.StringIO())


def detail_of(log_dir, key):
    return "\n".join(next((r.detail for r in Checker(log_dir).run() if r.key == key), []))


COMBAT_CORE = """\ufeff[10:00:00.000] [INFO] DDCore 1.0.0 启动，运行标识 20261114-100000-7000，主线程 1，游戏根目录 {root}
[10:00:00.100] [INFO] 插件 test_combat 加载成功（版本 1.0.0）
"""
COMBAT_LOG = """\ufeff[10:00:01.000] [INFO] 日志开始，DDCore 1.0.0，运行标识 20261114-100000-7000，插件路径 （自测样本）
[10:01:00.000] [INFO] [f100] ROUND where=town hero=hero#1 safe_point=1 buffs=18
[10:01:00.000] [INFO] [f100] STACK who=hero#1 ddtest_combat_spd 的条数=2 速度 6->8->9->11，逐条增量 2/1/2（期望 2u/u/2u，u=1）
[10:01:01.500] [INFO] [f190] ROUNDDONE where=town buffs 22->18 剩下的测试 buff=0 速度 6（开始时 6）
[10:02:00.000] [INFO] [f300] ROUND where=corridor hero=hero#78 safe_point=1 buffs=14
[10:02:00.000] [INFO] [f300] STACK who=hero#78 ddtest_combat_spd 的条数=2 速度 6.25->8.75->10->12.5，逐条增量 2.5/1.25/2.5（期望 2u/u/2u，u=1.25）
[10:02:01.500] [INFO] [f390] ROUNDDONE where=corridor buffs 19->14 剩下的测试 buff=0 速度 6.25（开始时 6.25）
[10:03:00.000] [INFO] [f500] ROUND where=battle hero=hero#78 safe_point=1 buffs=14
[10:03:00.000] [INFO] [f500] STACK who=hero#78 ddtest_combat_spd 的条数=2 速度 6.25->8.75->10->12.5，逐条增量 2.5/1.25/2.5（期望 2u/u/2u，u=1.25）
[10:03:01.500] [INFO] [f590] ROUNDDONE where=battle buffs 19->14 剩下的测试 buff=0 速度 6.25（开始时 6.25）
[10:04:00.000] [INFO] [f700] DMGCHECK kind=full(应显示 Blocked) damage=6 absorb=6 hp_before=46.25 hp_after=46.25 loss=0 expect=0
[10:04:10.000] [INFO] [f800] DMGCHECK kind=partial damage=2 absorb=1 hp_before=46.25 hp_after=45.25 loss=1 expect=1
"""


def selftest(sample_dir):
    sample_dir = Path(sample_dir)
    if not (sample_dir / "DDLua.log").is_file():
        print(f"{sample_dir} 下没有 DDLua.log：先运行 xmake run dd_lua_tests P4c2 生成离线预演的日志")
        return 1
    failures = []

    def expect(name, got, wanted):
        for key, status in wanted.items():
            ok = got.get(key) == status
            print(f"  {'PASS' if ok else 'FAIL'} {name}：{key} = {got.get(key)}（应为 {status}）")
            if not ok:
                failures.append(f"{name}/{key}")

    with tempfile.TemporaryDirectory(dir=PROJECT / "build") as tmp:
        tmp = Path(tmp)
        base = tmp / "base"
        base.mkdir()
        for name in ("DDLua.log", "DDCore.log", TAMPER_LOG):
            text = (sample_dir / name).read_text(encoding="utf-8-sig")
            text = text.replace("游戏根目录 （离线预演）", f"游戏根目录 {p3.RIG}")
            (base / name).write_text(("\ufeff" if name != TAMPER_LOG else "") + text, encoding="utf-8")
        stamp = datetime.now().replace(hour=10, minute=30, second=0, microsecond=0).timestamp()
        for p in base.iterdir():
            os.utime(p, (stamp, stamp))

        print(f"== 完整样本（{sample_dir}）")
        results = Checker(base).run()
        report(results)
        # 离线预演里没有：第二段（另一次运行，必需项，只给第一段时退出码 2）；需要目测的两项判定不了。
        # 可选部分 D（--current-only 之后读到上一代）在预演里有，persist.prev 通过
        wanted = {r.key: (LOOK if r.key.startswith("look.") else MISS if r.key == "combat.stack" else PASS) for r in results}
        expect("完整样本", {r.key: r.status for r in results}, wanted)

        def variant(name, file, fn):
            d = tmp / name
            shutil.copytree(base, d)
            path = d / file
            text = path.read_text(encoding="utf-8-sig")
            new = fn(text)
            if new == text:
                failures.append(f"{name}：{file} 没有改动（样本里找不到要改的内容）")
            path.write_text(("\ufeff" if file != TAMPER_LOG else "") + new, encoding="utf-8")
            for p in d.iterdir():
                os.utime(p, (stamp, stamp))
            return d

        def sub(name, file, pattern, repl, count=1):
            return variant(name, file, lambda t: re.sub(pattern, repl, t, count=count))

        def drop(name, file, needle):
            return variant(name, file, lambda t: "".join(l for l in t.splitlines(True) if needle not in l))

        def to_fail(name, who, tag):
            """把场景 Mod 的「[INFO] … 通过 [tag]」改成「[ERROR] … 失败 [tag]」（审核 C-B1 的篡改样本）。"""
            pattern = rf"\[INFO\] \[{re.escape(who)}\] 通过 \[{re.escape(tag)}\]"
            return sub(name, "DDLua.log", pattern, f"[ERROR] [{who}] 失败 [{tag}]")

        print("== 篡改过的样本")
        # 审核 C-B1：检查脚本别的项不看的标签失败了，也要判失败
        for who, tag in (("p4b_combat", "script.apply.town"), ("p4b_combat", "script.remove.corridor"), ("p4b_combat", "fs"),
                         ("p4b_combat", "stack.battle"), ("p4b_combat", "define"), ("p4b_combat", "monster.battle"), ("p4b_combat", "remove.tag.town"),
                         ("p4b_combat", "define.game"), ("p4c_shield", "define"), ("p4c_persist", "define.p4c_mark"), ("p4c_persist", "remove.match")):
            expect(f"自报失败 {who} {tag}", statuses(to_fail(f"fail-{who}-{tag}", who, tag)), {"selfreport": FAIL})
        expect("吸收量算错", statuses(sub("absorb", "DDLua.log", r'("ev":"absorb".*?"kind":"full".*?"written":)(\d+)', r"\g<1>99")),
            {"shield.formula": FAIL})
        expect("吸收量和护盾都改", statuses(sub("absorb2", "DDLua.log", r'\{"absorb":3,("actor":"hero:12","after":\{"base":12,"cur":)13\.25,',
            r'{"absorb":2,\g<1>14.25,')), {"shield.formula": FAIL})
        expect("超过上限", statuses(sub("cap", "DDLua.log", r'("after":\{"base":12,"cur":)22(,"decay":0,"decay_turns":0,"emax":10,"max":22)',
            r"\g<1>25\g<2>")), {"shield.cap": FAIL, "shield.formula": FAIL})
        expect("effect 名字的解析和 DDShield 不同", statuses(sub("parse", "DDLua.log",
            r'("effect":"shield_self_gain_5","ev":"apply","op":"add",.*?"value":)5', r"\g<1>4")), {"shield.formula": FAIL})
        expect("部分吸收的生命损失不对", statuses(sub("loss", "DDLua.log", r'("ev":"hit","expect":2,"hp_after":)28', r"\g<1>26")),
            {"shield.partial": FAIL})
        expect("全部吸收却掉血", statuses(sub("fullhp", "DDLua.log", r'("ev":"hit","expect":0,"hp_after":)28', r"\g<1>25")),
            {"shield.full": FAIL})
        expect("驱动的 effect 不算自然获得", statuses(sub("natural2", "DDLua.log", r'"effect":"shield_self_gain_5"', '"effect":"shield_gain_5"', count=0)),
            {"shield.natural": MISS})

        # 修复 F6：SHIELD 行的 skill 为空时看 p4b_combat 的 [skill] 行。先把 skill 都改成空（修复之前的 shield_lite 写出的样子），再改别处
        def no_attr(t):
            return t.replace('"skill":"smite"', '"skill":""')

        def expect_natural(name, d, status, why=None):
            r = next(x for x in Checker(d).run() if x.key == "shield.natural")
            detail = "\n".join(r.detail)
            ok = r.status == status and (why is None or why in detail)
            print(f"  {'PASS' if ok else 'FAIL'} {name}：shield.natural = {r.status}（应为 {status}）" + (f"，说明里要有「{why}」" if why else ""))
            if not ok:
                failures.append(f"{name}/shield.natural")
                print("    " + detail.replace("\n", "\n    "))

        skill_re = r"(?m)^[^\n]*\[p4b_combat\] 信息 \[skill\] dd\.actor\(hero#1[23]\)：smite 等级 \d+ [^\n]*\n"
        expect_natural("skill 为空，[skill] 行证明来自 Smite（修复 F6 之前的日志）", variant("nat-ev", "DDLua.log", no_attr), PASS)
        expect_natural("skill 为空，没有 [skill] 行", variant("nat-noskill", "DDLua.log",
            lambda t: "".join(l for l in no_attr(t).splitlines(True) if "[p4b_combat] 信息 [skill]" not in l)), MISS, "没有这个执行者的")
        expect_natural("skill 为空，[skill] 行的执行者不同", variant("nat-who", "DDLua.log",
            lambda t: re.sub(r"(\[p4b_combat\] 信息 \[skill\] dd\.actor\(hero#)1[23]\)", r"\g<1>14)", no_attr(t))), MISS, "没有这个执行者的")
        expect_natural("skill 为空，effect 不在 Smite 的列表里", variant("nat-list", "DDLua.log",
            lambda t: no_attr(t).replace("effects=[Unholy Killer 1,shield_self_gain_5]", "effects=[Unholy Killer 1]")), MISS, "effect 列表里没有它")
        expect_natural("skill 为空，中间夹着这个执行者的别的技能", variant("nat-between", "DDLua.log",
            lambda t: re.sub(skill_re, lambda m: m.group(0) + m.group(0).replace("：smite 等级", "：stunning_blow 等级").replace(
                "Unholy Killer 1,shield_self_gain_5", "Stun 1"), no_attr(t))), MISS, "最近一次技能是 stunning_blow")
        # 驱动的施加即使 skill 是 smite 也不算（真的 Smite 先去掉）
        expect_natural("驱动的施加写着 smite", variant("nat-drive", "DDLua.log", lambda t: re.sub(
            r'("effect":"shield_self_gain_5","ev":"apply"[^\n]*"skill":)""([^\n]*\n[^\n]*\[p4c_shield\] 信息 \[drive\] shield_self_gain_5：)', r'\g<1>"smite"\g<2>',
            "".join(l for l in no_attr(t).splitlines(True) if "[p4b_combat] 信息 [skill]" not in l))), MISS, "驱动的，不算")
        expect("回合结算不对", statuses(sub("round", "DDLua.log", r'("after":\{"base":12,"cur":)15\.25(,"decay":0,"decay_turns":0,"emax":21,"max":33,"pierce":0,"pierce_by":"","regen":2,"regen_turns":2\},"battle":1,"before")',
            r"\g<1>16.25\g<2>")), {"shield.round": FAIL})
        expect("第 1 回合也结算了", statuses(sub("round1", "DDLua.log", r',"skip":true', "")), {"shield.formula": FAIL})
        expect("生命 > 0 却清零", statuses(sub("zero", "DDLua.log", r'("ev":"zero","hp":)0', r"\g<1>5")), {"shield.formula": FAIL})
        expect("规则结果和护盾对不上", statuses(sub("rule", "DDLua.log", r'("actor":"hero:13","battle":1,"buff":"p4c_shield_probe","cur":0,"ev":"rule","in_raid":true,"is_false_rule":false,"result":)"original"',
            r"\g<1>true")), {"shield.rule": FAIL})
        expect("有护盾却交还原来的判定", statuses(sub("rule2", "DDLua.log", r'("ev":"rule","in_raid":true,"is_false_rule":false,"result":)true', r'\g<1>"original"')),
            {"shield.rule": FAIL})
        expect("shielded 没有真的生效", statuses(sub("live", "DDLua.log", r"\[INFO\] \[p4c_shield\] 通过 \[rule\.live\]", "[ERROR] [p4c_shield] 失败 [rule.live]")),
            {"shield.rule": FAIL, "selfreport": FAIL})
        expect("before 接不上", statuses(sub("cont", "DDLua.log", r'("before":\{"base":12,"cur":)13\.25', r"\g<1>12.25")),
            {"shield.formula": FAIL})
        expect("恢复的护盾和存档时不同", statuses(sub("restore", "DDLua.log", r'("after":\{"base":12,"cur":)15\.25([^\n]*?"ev":"restore")', r"\g<1>14.25\g<2>")),
            {"shield.persist": FAIL})
        expect("战斗结束没删载体", statuses(drop("carrier", "DDLua.log", '"op":"removed","reason":"other"')), {"shield.clear": FAIL})
        expect("没有 SHIELD 行", statuses(drop("noshield", "DDLua.log", "SHIELD {")), {"shield.formula": MISS})
        expect("没有怪物的部分吸收", statuses(drop("nomonster", "DDLua.log", '"actor":"monster:4500","battle":1,"damage":7')), {"shield.monster": MISS})
        expect("城镇读档失败", statuses(sub("town", "DDLua.log", r"通过 \[persist\.town\]", "失败 [persist.town]")), {"persist.town": FAIL})
        expect("没有战斗中读档", statuses(drop("nobattle", "DDLua.log", "[persist.battle]")), {"persist.battle": MISS})
        expect("dd.store 接不上", statuses(sub("store", "DDLua.log", r"(信息 \[store\] status=loaded loads=3 saves=)5", r"\g<1>4")),
            {"persist.store": FAIL})
        # 审核 R2B-1：读到上一代时和本次运行里更早的那次存档比（完整样本里可选部分 D 的 prev 通过）；对不上、或者就是最后一次存档时失败
        expect("上一代的 saves 对不上", statuses(sub("prev-bad", "DDLua.log", r"(信息 \[store\] status=prev loads=\d+ saves=)(\d+)", r"\g<1>99")),
            {"persist.store": FAIL, "persist.prev": PASS})
        last_d1 = re.findall(r"\[p4c_persist\] 信息 \[save\] #(\d+) where=(\w+) saves=(\d+) expect=(\d+)",
                             (base / "DDLua.log").read_text(encoding="utf-8-sig").split("status=prev")[0])[-1]
        expect("上一代给的是最后一次存档", statuses(sub("prev-last", "DDLua.log", r"(信息 \[store\] status=prev loads=\d+ )saves=\d+ last=\S+ expect=\d+",
            rf"\g<1>saves={last_d1[2]} last=#{last_d1[0]}/{last_d1[1]} expect={last_d1[3]}")), {"persist.store": FAIL})
        # 审核 B-S9：观察到写存档却没派发、交了数据没写、写过之后读到 empty
        expect("观察到写存档却没派发", statuses(drop("nosave", "DDCore.log", "persist：campaign.save #")), {"persist.written": FAIL})
        expect("交了数据没写", statuses(sub("nocommit", "DDCore.log", r"提交旁路文件 4 个", "提交旁路文件 0 个")), {"persist.written": FAIL})
        expect("写入的大小读成几 GB", statuses(sub("bigsize", "DDCore.log", r"(观察到写存档：[^\n]*?)4096 字节", r"\g<1>4294971392 字节")),
            {"persist.written": FAIL})
        expect("写过之后读到 empty", statuses(sub("s9-empty", "DDCore.log", r"persist：DDLua/p4b_combat：loaded[^\n]*", "persist：DDLua/p4b_combat：empty：没有旁路文件", count=2)),
            {"persist.unexpected": FAIL})
        expect("没有改过旁路", statuses(drop("notamper", TAMPER_LOG, "p4c_persist")), {"persist.mismatch": MISS, "persist.unexpected": FAIL})
        # 审核 R2B-3：存档在别的方案下进过，旁路落后，本次运行第一次读档就不一致：判失败，提示重跑前 rig.py reset-persist
        stale = sub("stale", "DDCore.log", r"\[INFO\] persist：DDLua/p4c_persist：empty：没有旁路文件",
                    "[WARN] persist：DDLua/p4c_persist：mismatch：当前一代：名册的哈希不同（旁路 2222222222222222/4096 字节，存档 0123456789ABCDEF/4096 字节）；上一代：没有文件")
        expect("第一次读档就不一致（别的方案下玩过）", statuses(stale), {"persist.unexpected": FAIL})
        hint = "reset-persist" in detail_of(stale, "persist.unexpected")
        print(f"  {'PASS' if hint else 'FAIL'} 第一次读档就不一致：persist.unexpected 的说明提示 rig.py reset-persist")
        if not hint:
            failures.append("stale/hint")
        # 审核 C-N1：别的运行的 tamper 记录不能解释这次的不一致；改完又恢复了，之后的不一致也不算预期的
        expect("tamper 记录属于别的运行", statuses(sub("otherrun", TAMPER_LOG, r'"run": "[^"]+"', '"run": "20261113-090000-1111"', count=0)),
            {"persist.mismatch": MISS, "persist.unexpected": FAIL, "errors": FAIL})
        expect("改完又恢复了", statuses(variant("restored", TAMPER_LOG, lambda t: t + json.dumps({"time": 1, "run": "20261114-100000-5150", "key": "p4c_persist",
            "restore": True, "restored": ["p4c_persist.ddsave"]}) + "\n")), {"persist.unexpected": FAIL})
        expect("改过旁路却读成 loaded", statuses(variant("tamper-loaded", "DDCore.log", lambda t: re.sub(r"persist：DDLua/p4c_persist：mismatch：[^\n]*",
            "persist：DDLua/p4c_persist：loaded（当前一代，版本 1.0.0，写于 1，数据 1 字节）", t).replace("[WARN] persist：DDLua/p4c_persist：loaded", "[INFO] persist：DDLua/p4c_persist：loaded"))),
            {"persist.mismatch": FAIL})
        # 审核 C-Y5：名册里一条标记都没有时「状态不恢复」是空判
        expect("不一致时名册里没有标记", statuses(sub("nomarks", "DDLua.log", r"名册里的 p4c_mark 载体 \d+ 条，状态是空表的 \d+ 条", "名册里的 p4c_mark 载体 0 条，状态是空表的 0 条")),
            {"persist.mismatch": FAIL})
        expect("没有回城清理", statuses(drop("novisit", "DDLua.log", "[town_visit]")), {"boundary.town_visit": MISS})
        expect("怪物状态没丢弃", statuses(sub("gone", "DDLua.log", r"通过 \[monster\.gone\]", "失败 [monster.gone]")), {"boundary.monster": FAIL})
        expect("没有撤退", statuses(drop("noretreat", "DDLua.log", "[bend.retreat]")), {"boundary.retreat": MISS})
        expect("排队失败", statuses(sub("queue", "DDLua.log", r"通过 \[queue\.done\.battle\]", "失败 [queue.done.battle]")), {"queue": FAIL})
        expect("怪物上没有删除", statuses(drop("nomonrm", "DDLua.log", "[monster.remove.battle]")), {"combat.writes": MISS})
        expect("状态丢弃", statuses(variant("drop", "DDLua.log", lambda t: t.replace("[p4c_persist] 信息 [load]",
            "[p4c_persist] 脚本 Buff 状态丢弃：p4c_mark（hero#12，guid 7）：条目不存在或 id 不一致\n[10:00:30.000] [INFO] [p4c_persist] 信息 [load]", 1))),
            {"guid": FAIL})
        expect("没有正常退出", statuses(drop("noexit", "DDCore.log", "persist：退到桌面")), {"crash": FAIL})
        expect("VM 损坏", statuses(variant("broken", "DDLua.log", lambda t: t.replace("[p4c_persist] 信息 [load]",
            "[p4c_persist] 回调里的原生异常，VM 已损坏\n[10:00:30.000] [INFO] [p4c_persist] 信息 [load]", 1))), {"crash": FAIL})
        expect("城镇里施加 effect 没被拒绝", statuses(sub("effect", "DDLua.log", r"通过 \[effect\.town\]", "失败 [effect.town]")), {"effect": FAIL})
        expect("走廊里规则结果没核对", statuses(drop("rule0", "DDLua.log", "[rule.live.corridor]")), {"rule": MISS})
        expect("规则单帧峰值超过 combat_frame_ms", statuses(sub("peak", "DDCore.log", r"单帧峰值 [\d.]+ us", "单帧峰值 12000.0 us")), {"rule.stats": FAIL})
        expect("主菜单有语义 hook", statuses(sub("menu", "DDCore.log", r"(hook 报告（AssetsReady）[^\n]*\n[^\n]*\n[^\n]*语义 hook )0", r"\g<1>3")),
            {"hooks.menu": FAIL})
        expect("DDCore 意外的 WARN", statuses(variant("warn", "DDCore.log", lambda t: t.replace("[INFO] 阶段 AssetsReady", "[WARN] 阶段 AssetsReady", 1))),
            {"errors": FAIL})
        expect("DDLua 意外的 WARN", statuses(variant("luawarn", "DDLua.log", lambda t: t.replace("[INFO] [DDLua] 只订阅了", "[WARN] [DDLua] 只订阅了", 1))),
            {"errors": FAIL})
        expect("漏勾 p4c_shield", statuses(drop("nodrive", "DDLua.log", "[p4c_shield]")), {"mods": MISS})
        expect("没有不带 Lua Mod 的存档", statuses(drop("nonolua", "DDLua.log", "没有可加载的 Lua Mod")), {"hooks.nolua": MISS})

        # 第二段：combat 方案的 test_combat.log（运行标识、开始时间要和同目录的 DDCore.log 对得上）
        combat = tmp / "combat"
        combat.mkdir()
        (combat / "DDCore.log").write_text(COMBAT_CORE.format(root=p3.RIG), encoding="utf-8")
        (combat / "test_combat.log").write_text(COMBAT_LOG, encoding="utf-8")
        for p in combat.iterdir():
            os.utime(p, (stamp, stamp))
        expect("第二段", statuses(base, combat_dir=combat), {"combat.stack": PASS})
        # 审核 R2B-2：第二段是必做的，只给第一段的日志时退出码 2，两段都给了才是 0
        for name, d, want in (("只有第一段", None, 2), ("两段都有", combat, 0)):
            code = exit_code(base, d)
            print(f"  {'PASS' if code == want else 'FAIL'} 退出码（{name}）= {code}（应为 {want}）")
            if code != want:
                failures.append(f"退出码/{name}")

        def combat_variant(name, fn):
            d = tmp / name
            shutil.copytree(combat, d)
            path = d / "test_combat.log"
            path.write_text(fn(path.read_text(encoding="utf-8")), encoding="utf-8")
            for p in d.iterdir():
                os.utime(p, (stamp, stamp))
            return d

        expect("第二段 STACK 报 WARN", statuses(base, combat_dir=combat_variant("c-warn", lambda t: t.replace("[INFO] [f300] STACK", "[WARN] [f300] STACK"))),
               {"combat.stack": FAIL})
        expect("第二段 test_combat.log 是旧的", statuses(base, combat_dir=combat_variant("c-old", lambda t: t.replace("20261114-100000-7000", "20261113-080000-1"))),
               {"combat.stack": FAIL})
        expect("第二段没有战斗那一轮", statuses(base, combat_dir=combat_variant("c-miss", lambda t: "".join(
            l for l in t.splitlines(True) if "where=battle" not in l))), {"combat.stack": MISS})
        empty = tmp / "empty"
        empty.mkdir()
        expect("空目录", statuses(empty), {"logs": NOLOG})
    print(f"\n自测{'通过' if not failures else '失败：' + '、'.join(failures)}")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log-dir", type=Path, default=p3.DEFAULT_LOG_DIR, help=f"scenario4 那次运行的日志目录，默认 {p3.DEFAULT_LOG_DIR}")
    parser.add_argument("--lua-log-dir", type=Path, help="可选：lua 方案那次运行的日志目录（进过一个没有 Lua Mod 的存档），判定 #6 的可选项")
    parser.add_argument("--combat-log-dir", type=Path, help="可选：第二段 combat 方案那次运行的日志目录（test_combat.log），见 10 的「第二段」")
    parser.add_argument("--selftest", nargs="?", const=REHEARSAL, type=Path, metavar="目录",
        help=f"用离线预演的日志自测（默认 {REHEARSAL}，由 xmake run dd_lua_tests P4c2 生成）")
    args = parser.parse_args()
    if args.selftest:
        sys.exit(selftest(args.selftest))
    print(f"日志目录：{args.log_dir}" + (f"；lua 方案：{args.lua_log_dir}" if args.lua_log_dir else "") +
          (f"；combat 方案：{args.combat_log_dir}" if args.combat_log_dir else "") + "\n")
    sys.exit(report(Checker(args.log_dir, args.lua_log_dir, args.combat_log_dir).run()))


if __name__ == "__main__":
    main()

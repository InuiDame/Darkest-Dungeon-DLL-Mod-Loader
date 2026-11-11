"""检查 P3 用户场景的日志，逐条输出验收项：通过、失败、格式不认识、没有触发。

用法（guest，工作目录 D:\\workplace；步骤见 DD.ModLoader/docs/09-lua-scenario.md）：

    python -B DD.ModLoader/tools/p3_scenario_check.py --log-dir DD.ModLoader/build/rig_logs/scenario --lua-log-dir DD.ModLoader/build/rig_logs/lua
    python -B DD.ModLoader/tools/p3_scenario_check.py --selftest DD.ModLoader/build/p3d_rehearsal    # 自测

场景是两次游戏运行：scenario 方案（场景 Lua Mod 和探针 probe_p3c）和 lua 方案（只有 DDCore + DDLua，进一个没有
Lua Mod 的存档，验收 #7）。游戏每次启动都重写日志，所以每次退出游戏后用 rig.py save-log 把日志复制出来，再把两个
副本目录交给本脚本。--log-dir 不给时读测试目录的 DDModLoader\\log。

先核对日志本身：DDLua.log、DDCore.log、probe_p3c.log 属于同一次游戏运行（开始时间、运行标识），晚于最近一次部署，
方案和加载的插件对得上，DDLua 用的是默认配置；之后才逐项判定。
退出码：0 全部必需项通过；1 有失败或格式不认识；2 没有失败，但有必需项没有触发。
"""
import argparse
import json
import os
import re
import shutil
import sys
import tempfile
from datetime import datetime, timedelta
from pathlib import Path

RIG = Path(r"D:\Program Files (x86)\Steam\steamapps\common") / os.environ.get("DD_RIG_NAME", "DarkestDungeon_Dev")
DEFAULT_LOG_DIR = RIG / "DDModLoader" / "log"
LOG_META = ".rig-log.json"  # rig.py save-log 写进副本目录的说明（方案、部署时间）

PASS, FAIL, FORMAT, MISS, NOLOG = "通过", "失败", "格式不认识", "没有触发", "没有日志"

# 场景 Lua Mod：id -> project.xml 的标题（存档按标题识别本地 Mod）
SCENARIO_MODS = {
    "hello_lua": "DDLua Example Hello",
    "p3c_actor": "DDLua Test P3c Actor",
    "p3d_core": "DDLua Test P3d Core",
    "p3d_faulty": "DDLua Test P3d Faulty",
    "p3d_broken": "DDLua Test P3d Broken",
}
# 每个方案在 DDCore.log 里应当加载成功的新协议插件（rig.py 的 PROFILES）
PROFILE_PLUGINS = {"scenario": {"DDLua", "probe_p3c"}, "lua": {"DDLua"}}
SAME_RUN_SECONDS = 30  # 同一次运行里 DDCore 和各插件日志的开始时间实际相差 1～2 秒
BUDGET_MARGIN_MS = 500  # 死循环从进入到被打断，允许比执行预算多这么多（日志、调度的开销）

LINE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] \[(\w+)\] (.*)$")
PREFIX = re.compile(r"^\[([^\]]+)\] (.*)$")
RUN_ID = re.compile(r"运行标识 ([0-9A-Za-z_.:-]+)")

# probe_p3c.log 的判定行：前缀 -> 完整格式。前缀对上、格式对不上时判「格式不认识」，不放过
PROBE_FORMATS = {
    # 分组：1 第几场 2 探索序号 3 战斗序号 4 怪物表里 5 actor_find 找回 6 上一场的句柄 7 仍能解析 8 其中 guid 和本场相同
    "battle.start 第": re.compile(r"battle\.start 第 (\d+) 场：探索序号 (\d+)，战斗序号 (\d+)，怪物表里 (\d+) 个（guid:[^）]*），"
                                 r"actor_find 找回同一指针 (\d+) 个；上一场战斗的怪物句柄 (\d+) 个，仍能解析 (\d+) 个（应为 0），"
                                 r"其中 guid 和本场怪物相同 (\d+) 个$"),
    # 英雄句柄不计阵亡、解雇的（探针自己排除）：1 第几场 2 本场怪物句柄 3 仍能解析 4 英雄句柄 5 仍能解析 6 阵亡解雇的 7 其中仍能解析
    "battle.end 第": re.compile(r"battle\.end 第 (\d+) 场：本场怪物句柄 (\d+) 个，仍能解析 (\d+) 个（应为 0）；本存档英雄句柄 (\d+) 个，"
                               r"仍能解析 (\d+) 个（不计阵亡、解雇的 (\d+) 个，其中仍能解析 (\d+) 个）$"),
    "CampaignLoad 存档序号": re.compile(r"CampaignLoad 存档序号 (\d+)：actor_kinds 英雄 (\d) 怪物 (\d)，探索序号 (\d+)，战斗序号 (\d+)；"
                                     r"上一个存档的英雄句柄 (\d+) 个，仍能解析 (\d+) 个（应为 0）$"),
    "指针字段": re.compile(r"指针字段 (\d+) 个：转成句柄 (\d+)，不是 actor (\d+)，虚表是 actor 却转换失败 (\d+)；解析回同一指针 (\d+)，读取成功 (\d+)$"),
    "独立读法：": re.compile(r"独立读法：HP 一致 (\d+)，压力一致 (\d+)/(\d+)，最大生命一致 (\d+)/(\d+)，职业 id 一致 (\d+)；HP 大于最大生命 (\d+) 次$"),
    ".post 结算值": re.compile(r"\.post 结算值和句柄读数一致 (\d+)/(\d+)；治疗后不超过最大生命 (\d+)/(\d+)；战斗 (\d+) 场，本存档英雄句柄 (\d+) 个$"),
}
SUMMARY_LINES = ("指针字段", "独立读法", ".post 结算值")  # 「==== 汇总」后面紧跟的三行

# p3c_actor 在下一场战斗开始时对「上一场的怪物里和本场怪物 guid 相同」的逐项检查（第二轮审核 R2B-应修1）。
# 分组：1 通过/失败 2 第几场 3 探索序号 4 战斗序号（新句柄的） 5 同 guid 个数，之后五对「做到/应做」：
# 旧句柄失效、== 不相等、不是同一个对象、表键分开、新句柄驻留
LUA_SAME_PREFIX = re.compile(r"(通过|失败) 第 \d+ 场.*上一场的怪物里和本场怪物 guid 相同")
LUA_SAME = re.compile(r"(通过|失败) 第 (\d+) 场（探索 (\d+)，战斗 (\d+)）：上一场的怪物里和本场怪物 guid 相同的 (\d+) 个；"
                      r"旧句柄失效 (\d+)/(\d+)，== 不相等 (\d+)/(\d+)，不是同一个对象 (\d+)/(\d+)，表键分开 (\d+)/(\d+)，新句柄驻留 (\d+)/(\d+)$")
LUA_SAME_ITEMS = ("旧句柄失效", "== 不相等", "不是同一个对象", "表键分开", "新句柄驻留")


class Line:
    def __init__(self, index, level, text, t):
        self.index = index
        self.level = level
        self.text = text
        self.t = t  # 当天的秒数（日志只有时分秒）
        match = PREFIX.match(text)
        self.who, self.body = (match.group(1), match.group(2)) if match else ("", text)

    def __repr__(self):
        return self.text


def read_log(path):
    if not path.is_file():
        return None
    lines = []
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = LINE.match(raw)
        if match:
            h, m, s, ms = (int(match.group(i)) for i in range(1, 5))
            lines.append(Line(len(lines), match.group(5), match.group(6), h * 3600 + m * 60 + s + ms / 1000))
        elif lines and raw.strip():
            lines.append(Line(len(lines), lines[-1].level, raw, lines[-1].t))  # 不带时间戳的续行
    return lines


def elapsed_ms(a, b):
    """同一天内从 a 到 b 的毫秒数，跨午夜时加一天。"""
    d = (b - a) * 1000
    return d + 86400 * 1000 if d < -1000 else d


def text_time(value):
    return value.strftime("%Y-%m-%d %H:%M:%S") if value else "未知"


def same_path(a, b):
    try:
        return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
    except OSError:
        return False


def deploy_info(log_dir):
    """日志目录对应的部署：rig.py save-log 的说明，或者测试目录自己的 .rig.json。都没有时返回 None。"""
    meta = log_dir / LOG_META
    if meta.is_file():
        data = json.loads(meta.read_text(encoding="utf-8"))
        return {"profile": data.get("profile"), "deployed_at": data.get("deployed_at"), "cfg_files": data.get("cfg_files"),
            "source": f"rig.py save-log 记下的，{data.get('deployed_at_source', '')}"}
    state = RIG / ".rig.json"
    if same_path(log_dir, DEFAULT_LOG_DIR) and state.is_file():
        data = json.loads(state.read_text(encoding="utf-8"))
        when = data.get("deployed_at") or state.stat().st_mtime
        source = "测试目录的 .rig.json" + ("" if "deployed_at" in data else "，没有记录部署时间，用它的修改时间")
        return {"profile": data.get("profile"), "deployed_at": when, "cfg_files": None, "source": source}
    return None


class LogSet:
    """一次游戏运行的日志目录。"""

    def __init__(self, log_dir, with_probe=True):
        self.dir = Path(log_dir)
        self.lua = read_log(self.dir / "DDLua.log")
        self.core = read_log(self.dir / "DDCore.log")
        # lua 方案没有探针。测试目录里上一次 scenario 运行的 probe_p3c.log 会被 save-log 一起复制过来，不能读
        self.probe = read_log(self.dir / "probe_p3c.log") if with_probe else None
        self.deploy = deploy_info(self.dir)

    def files(self, names):
        table = {"DDLua.log": self.lua, "DDCore.log": self.core, "probe_p3c.log": self.probe}
        return [(name, table[name]) for name in names if table[name] is not None]

    def start_of(self, name, lines):
        """日志开始的日期时间：首行的时分秒配上文件修改时间的日期（首行晚于修改时间时是前一天）。"""
        first = next((l for l in lines if l.t is not None), None)
        if first is None:
            return None
        mtime = datetime.fromtimestamp((self.dir / name).stat().st_mtime)
        start = datetime.combine(mtime.date(), datetime.min.time()) + timedelta(seconds=first.t)
        if start > mtime + timedelta(seconds=5):
            start -= timedelta(days=1)
        return start, mtime

    @staticmethod
    def run_id(lines):
        for l in lines[:3]:
            match = RUN_ID.search(l.text)
            if match:
                return match.group(1)
        return None

    def config(self):
        """DDLua 的配置行、执行预算（毫秒）、不是默认配置的问题。"""
        lines = [l for l in (self.lua or []) if l.who == "DDLua" and l.body.startswith("配置")]
        if not lines:
            return None, 50, ["DDLua.log 里没有配置行"]
        line = lines[0]
        problems = []
        if not line.body.startswith("配置（没有 DDLua.cfg，用默认值）"):
            problems.append("DDLua 读了 DDLua.cfg，不是默认配置（rig.py deploy 会把不是它生成的 DDLua.cfg 改名备份，重新部署后再走一次）")
        if not re.search(r"(^|\s)dev_mode=0(\s|$)", line.body):
            problems.append("dev_mode 不是 0")
        match = re.search(r"(^|\s)budget_ms=(\d+)", line.body)
        return line, int(match.group(2)) if match else 50, problems

    def identity(self, profile, names):
        """这几份日志是不是同一次运行、是不是部署之后的、方案对不对。返回 (说明, 问题)。"""
        detail, problems = [], []
        present = self.files(names)
        starts = {}
        for name, lines in present:
            start = self.start_of(name, lines)
            if start is None:
                problems.append(f"{name} 里没有带时间戳的行")
                continue
            starts[name] = start
            detail.append(f"{name}：开始于 {text_time(start[0])}，最后修改 {text_time(start[1])}")
        if len(starts) >= 2:
            first = min(v[0] for v in starts.values())
            last = max(v[0] for v in starts.values())
            if (last - first).total_seconds() > SAME_RUN_SECONDS:
                problems.append(f"日志的开始时间相差 {(last - first).total_seconds():.0f} 秒，不是同一次游戏运行：" +
                    "、".join(f"{name} {text_time(v[0])}" for name, v in starts.items()) + "（有的插件这次没有被加载，留下的是旧日志）")
        ids = {name: self.run_id(lines) for name, lines in present}
        if any(ids.values()):
            if None in ids.values() or len(set(ids.values())) > 1:
                problems.append("运行标识对不上：" + "、".join(f"{name} {value or '没有'}" for name, value in ids.items()))
            else:
                detail.append(f"运行标识 {next(iter(ids.values()))}，{len(ids)} 份日志一致")
        if self.deploy:
            deployed = datetime.fromtimestamp(self.deploy["deployed_at"]) if self.deploy["deployed_at"] else None
            detail.append(f"部署：{self.deploy['profile']} 方案，{text_time(deployed)}（{self.deploy['source']}）")
            if self.deploy["profile"] != profile:
                problems.append(f"这份日志对应的方案是 {self.deploy['profile']}，这里要的是 {profile} 方案的运行")
            old = [name for name, v in starts.items() if deployed and v[0] < deployed - timedelta(seconds=2)]
            if old:
                problems.append(f"{'、'.join(old)} 早于最近一次部署（{text_time(deployed)}），是旧日志：部署后还没运行过游戏，"
                    "或者这次是从 Steam 启动了真实安装；换过方案后测试目录的日志不再是上一次的，用 rig.py save-log 保存的副本")
            if self.deploy.get("cfg_files") and "DDLua.cfg" in self.deploy["cfg_files"]:
                detail.append("保存日志时 DDModLoader\\cfg 里有 DDLua.cfg")
        else:
            detail.append("没有部署记录（不是 rig.py save-log 保存的目录，也不是测试目录的日志目录）：只核对了日志彼此一致")
        if self.core is not None:
            loaded = {m.group(1) for l in self.core for m in [re.match(r"插件 (\S+) 加载成功", l.text)] if m}
            want = PROFILE_PLUGINS[profile]
            if loaded != want:
                problems.append(f"DDCore.log 里加载成功的插件是 {'、'.join(sorted(loaded)) or '（没有）'}，{profile} 方案应为 {'、'.join(sorted(want))}")
            root = next((m.group(1) for l in self.core[:3] for m in [re.search(r"游戏根目录 (.+)$", l.text)] if m), None)
            if root and not same_path(root, RIG):
                problems.append(f"DDCore.log 的游戏根目录是 {root}，不是测试目录 {RIG}")
        return detail, problems

    def serial_problems(self):
        """DDLua 的会话、DDCore 和探针的 CampaignLoad 要一一对上，对不上说明混进了别的运行的日志。"""
        problems = []
        lua = {int(m.group(1)) for l in (self.lua or []) for m in [re.match(r"存档#(\d+)$", l.who)] if m and l.body.startswith("进入存档")}
        no_list = {int(m.group(1)) for l in (self.lua or []) for m in [re.match(r"存档#(\d+)$", l.who)] if m and "启用 Mod 列表不可用" in l.body}
        if self.core is not None:
            core = {int(m.group(1)) for l in self.core for m in [re.match(r"阶段 CampaignLoad，帧 \d+，存档序号 (\d+)", l.text)] if m}
            if self.lua is not None and lua - core:
                problems.append(f"DDLua.log 里有存档#{'、#'.join(map(str, sorted(lua - core)))}，DDCore.log 里没有这些 CampaignLoad：两份日志不是同一次运行")
            if self.lua is not None and core - lua - no_list:
                problems.append(f"DDCore.log 里有存档序号 {'、'.join(map(str, sorted(core - lua - no_list)))} 的 CampaignLoad，DDLua.log 里没有：两份日志不是同一次运行")
            if self.probe is not None:
                probe = {int(m.group(1)) for l in self.probe for m in [re.match(r"CampaignLoad 存档序号 (\d+)", l.text)] if m}
                if probe != core:
                    problems.append(f"probe_p3c.log 的 CampaignLoad（{sorted(probe)}）和 DDCore.log（{sorted(core)}）对不上")
        return problems

    def hook_reports(self):
        """{"AppReady"/"AssetsReady": 报告, ("CampaignLoad", 序号): 报告}，报告是 (目标数, listener, 语义 hook, 订阅, 原文)。"""
        reports = {}
        current = None
        serial = None
        for l in self.core or []:
            m = re.match(r"阶段 CampaignLoad，帧 \d+，存档序号 (\d+)", l.text)
            if m:
                serial = int(m.group(1))
            m = re.match(r"======== hook 报告（(\w+)），共 (\d+) 个目标", l.text)
            if m:
                current = (m.group(1), int(m.group(2)))
            m = re.match(r"事件系统：已注册 listener (\d+) 个，语义 hook (\d+) 个，订阅 (\d+) 个", l.text)
            if m and current:
                key = current[0] if current[0] != "CampaignLoad" else ("CampaignLoad", serial)
                reports[key] = (current[1], int(m.group(1)), int(m.group(2)), int(m.group(3)), l.text)
                current = None
        return reports


def split_sessions(lua):
    """按「[存档#N] 进入存档」切分 DDLua.log。每个会话：serial、start、lines（到下一个进入存档为止）。"""
    sessions = []
    starts = [l for l in (lua or []) if re.match(r"存档#\d+$", l.who) and l.body.startswith("进入存档")]
    for i, start in enumerate(starts):
        end = starts[i + 1].index if i + 1 < len(starts) else len(lua)
        sessions.append({"serial": int(start.who[3:]), "start": start, "lines": lua[start.index:end]})
    return sessions


class Result:
    def __init__(self, key, title, status, detail, required=True, acceptance=""):
        self.key, self.title, self.status, self.detail, self.required, self.acceptance = key, title, status, detail, required, acceptance


class Checker:
    def __init__(self, log_dir, lua_log_dir=None):
        self.log_dir = Path(log_dir)
        self.logs = LogSet(log_dir)
        self.lua, self.core, self.probe = self.logs.lua, self.logs.core, self.logs.probe
        self.lua_run = LogSet(lua_log_dir, with_probe=False) if lua_log_dir else None
        self.results = []
        self.sessions = split_sessions(self.lua)
        self.budget_ms = 50
        self.probe_sessions = self.split_probe()

    # ---------------------------------------------------------------- 工具

    def add(self, key, title, status, detail, required=True, acceptance=""):
        self.results.append(Result(key, title, status, detail if isinstance(detail, list) else [detail], required, acceptance))

    def find(self, lines, needle, who=None):
        return [l for l in (lines or []) if needle in l.text and (who is None or l.who == who)]

    def session_mods(self, session):
        """会话里发现的 Lua Mod：id -> (来源, 来源类型, 目录)"""
        mods = {}
        for l in session["lines"]:
            match = re.match(r"发现 Lua Mod，版本 \S+，来源 (.*)（([^（）]*)），目录 (.*)$", l.body)
            if match:
                mods[l.who] = (match.group(1), match.group(2), match.group(3))
        return mods

    def session_loaded(self, session):
        return {l.who for l in session["lines"] if l.body.startswith("main.lua 执行完毕")}

    @staticmethod
    def expected_loaded(discovered):
        """发现了这些场景 Mod 时应当加载成功的：p3d_broken 故意失败，p3d_faulty 依赖 p3d_core。"""
        ok = {mid for mid in ("hello_lua", "p3c_actor", "p3d_core") if mid in discovered}
        if "p3d_faulty" in discovered and "p3d_core" in discovered:
            ok.add("p3d_faulty")
        return ok

    def scenario_sessions(self):
        return [s for s in self.sessions if "p3d_core" in self.session_mods(s)]

    def split_probe(self):
        """按探针的「CampaignLoad 存档序号 N」切分 probe_p3c.log：序号 -> 行。"""
        out = {}
        current = None
        for l in self.probe or []:
            m = re.match(r"CampaignLoad 存档序号 (\d+)", l.text)
            if m:
                current = int(m.group(1))
                out[current] = []
            if current is not None:
                out[current].append(l)
        return out

    def probe_count(self, prefixes, sessions=None):
        """探针在加载了 p3d_core 的存档里以这些前缀开头的行数（探针的指针字段日志每次进存档最多 60 行、每场战斗至少 20 行，少了不说明问题）。"""
        serials = {s["serial"] for s in (sessions if sessions is not None else self.scenario_sessions())}
        return sum(1 for serial in serials for l in self.probe_sessions.get(serial, []) if l.text.startswith(prefixes))

    def tag_lines(self, tag, who="p3d_core", lines=None):
        source = self.lua if lines is None else lines
        ok = [l for l in (source or []) if l.who == who and l.body.startswith(f"通过 [{tag}]")]
        bad = [l for l in (source or []) if l.who == who and l.body.startswith(f"失败 [{tag}]")]
        return ok, bad

    def tag_result(self, key, title, tag, hint, required=True, acceptance=""):
        ok, bad = self.tag_lines(tag)
        if bad:
            self.add(key, title, FAIL, [l.text for l in bad[:3]], required, acceptance)
        elif ok:
            self.add(key, title, PASS, ok[0].text, required, acceptance)
        else:
            self.add(key, title, MISS, hint, required, acceptance)

    # ---------------------------------------------------------------- 各项

    def check_logs(self):
        title = "日志文件：同一次游戏运行、晚于部署、scenario 方案、DDLua 默认配置"
        if self.lua is None:
            self.add("logs", title, NOLOG, f"{self.log_dir} 下没有 DDLua.log：先 rig.py deploy scenario 再按 docs/09-lua-scenario.md 走流程")
            return False
        loaded = [l for l in self.lua if l.who == "DDLua" and re.match(r"DDLua \S+（Lua .*加载", l.body)]
        config, self.budget_ms, problems = self.logs.config()
        detail = [loaded[0].text if loaded else "没有 DDLua 加载行！", config.text if config else "没有配置行"]
        if not loaded:
            problems.append("没有 DDLua 加载行")
        id_detail, id_problems = self.logs.identity("scenario", ("DDLua.log", "DDCore.log", "probe_p3c.log"))
        problems += id_problems + self.logs.serial_problems()
        missing = [name for name, lines in (("DDCore.log", self.core), ("probe_p3c.log", self.probe)) if lines is None]
        if missing:
            detail.append("缺少：" + "、".join(missing) + "（相关的项单独报「没有日志」）")
        self.add("logs", title, FAIL if problems else PASS, problems + detail + id_detail)
        return True

    def check_enabled_list(self):
        title = "启用 Mod 列表解析（按存档发现 Lua Mod）"
        if not self.sessions:
            self.add("mods", title, MISS, "DDLua.log 里没有「[存档#N] 进入存档」：没有进过存档", acceptance="#6")
            return
        detail = ["DDCore：" + l.text for l in self.find(self.core, "启用 Mod 列表 ")[:3]]
        best = None
        for s in self.sessions:
            mods = self.session_mods(s)
            found = {mid for mid in SCENARIO_MODS if mid in mods}
            if best is None or len(found) > len(best[1]):
                best = (s, found, mods)
        s, found, mods = best
        detail.append(s["start"].text)
        wrong = []
        for mid in sorted(found):
            source, kind, directory = mods[mid]
            if source != SCENARIO_MODS[mid] or kind != "mod_local_source":
                wrong.append(f"{mid} 的来源是 {source}（{kind}），应为 {SCENARIO_MODS[mid]}（mod_local_source）")
            detail.append(f"{mid}：来源 {source}（{kind}），目录 {directory}")
        missing = [f"{mid}（标题 {t}）" for mid, t in SCENARIO_MODS.items() if mid not in found]
        if wrong:
            self.add("mods", title, FAIL, wrong + detail, acceptance="#6")
        elif missing:
            # 漏勾是流程问题，不是实现的问题：判「没有触发」，补勾后再进一次存档
            self.add("mods", title, MISS, detail + ["没有发现：" + "、".join(missing) +
                "。在这个存档的 Mod 列表里补勾，回主菜单再进一次（同一次游戏运行里就不会丢日志）"], acceptance="#6")
        else:
            self.add("mods", title, PASS, detail, acceptance="#6")

    def check_load_order(self):
        title = "加载顺序（依赖排序）、加载失败隔离，每次进存档加载的集合都对"
        sessions = [s for s in self.sessions if self.session_mods(s).keys() & SCENARIO_MODS.keys()]
        if not any("p3d_core" in self.session_mods(s) for s in sessions):
            self.add("order", title, MISS, "没有加载 p3d_core 的存档", acceptance="#5")
            return
        detail, problems = [], []
        for s in sessions:
            name = f"存档#{s['serial']}"
            discovered = set(self.session_mods(s)) & set(SCENARIO_MODS)
            loaded = self.session_loaded(s) & set(SCENARIO_MODS)
            want = self.expected_loaded(discovered)
            order = [l for l in s["lines"] if l.body.startswith("加载顺序：")]
            done = [l for l in s["lines"] if l.body.startswith("加载完成：")]
            broken = [l for l in s["lines"] if l.who == "p3d_broken" and "加载失败：" in l.body]
            torn = [l for l in s["lines"] if l.who == "p3d_broken" and l.body.startswith("已拆除")]
            detail += [l.text for l in order[:1] + done[:1]]
            m = re.match(r"加载完成：成功 (\d+) 个", done[0].body) if done else None
            if m and int(m.group(1)) != len(self.session_loaded(s)):
                problems.append(f"{name}：「加载完成」说成功 {m.group(1)} 个，「main.lua 执行完毕」的 Mod 有 {len(self.session_loaded(s))} 个")
            if order:
                ids = order[0].body[len("加载顺序："):].split()
                if "p3d_core" in ids and "p3d_faulty" in ids and ids.index("p3d_faulty") < ids.index("p3d_core"):
                    problems.append(f"{name}：p3d_faulty 依赖 p3d_core，却排在它前面")
            if "p3d_broken" in discovered:
                if not broken:
                    problems.append(f"{name}：p3d_broken 没有加载失败")
                elif not torn:
                    problems.append(f"{name}：p3d_broken 加载失败后没有「已拆除」")
            for mid in sorted(want - loaded):
                problems.append(f"{name}：{mid} 没有加载成功")
            for mid in sorted(loaded - want):
                problems.append(f"{name}：{mid} 不应加载成功")
        self.add("order", title, FAIL if problems else PASS, problems + detail, acceptance="#5")

    def check_example(self):
        title = "示例 Mod hello_lua 跑通"
        lines = [l for l in (self.lua or []) if l.who == "hello_lua"]
        if not lines:
            self.add("example", title, MISS, "没有 [hello_lua] 的日志：没有勾选「DDLua Example Hello」，或没进存档", acceptance="#1")
            return
        errors = [l for l in lines if l.level == "ERROR"]
        loaded = [l for l in lines if l.body.startswith("已加载（版本")]
        start = [l for l in lines if l.body.startswith("战斗开始：")]
        end = [l for l in lines if l.body.startswith("战斗结束：")]
        after = [l for l in lines if l.body.startswith("进入存档 3 秒了")]
        damage = [l for l in lines if "点伤害" in l.body]
        detail = [l.text for l in (loaded[:1] + after[:1] + start[:1] + damage[:1] + end[:1])]
        if errors:
            self.add("example", title, FAIL, ["出错：" + errors[0].text] + detail, acceptance="#1")
        elif loaded and start and end and after:
            self.add("example", title, PASS, detail, acceptance="#1")
        else:
            self.add("example", title, MISS, detail + ["还缺：" + "、".join(n for n, v in (("已加载", loaded), ("3 秒定时器", after),
                ("战斗开始", start), ("战斗结束", end)) if not v) + "（要打完至少一场战斗）"], acceptance="#1")

    def check_writes(self):
        probe = lambda prefixes, what: (f"（探针在加载了 p3d_core 的存档里记了 {what} {self.probe_count(prefixes)} 行）"
            if self.probe is not None and self.scenario_sessions() else "")
        rows = [
            ("damage.halve", "damage.pre 写入（英雄受到的第一次打不死的伤害减半，算上受到伤害的倍率后掉血符合写入值）", "damage.halve",
                "英雄还没受到过 ≥2 点、打不死他的伤害" + probe(("damage.pre.",), "damage.pre")),
            ("damage.cancel", "damage.pre 取消（之后英雄受到的下一次伤害按 0 结算）", "damage.cancel", "减半之后英雄还没再受伤"),
            ("damage.cancel.seen", "damage.pre 取消后，排在后面的订阅看到 ev.cancelled", "damage.cancel.seen", "同上"),
            ("hit.miss", "hit.pre 写入（第一次命中英雄的攻击改成未命中）", "hit.miss", "还没有命中英雄的攻击" + probe(("hit.pre.",), "hit.pre")),
            ("heal.zero", "heal.pre 写入（第一次治疗没满血的英雄改成 0）", "heal.zero",
                "还没治疗过没满血的英雄（用 Vestal 等的治疗技能；满血时的治疗只写「信息 [heal.zero]」）" + probe(("heal.post ",), "heal.post")),
            ("stress.zero", "stress.pre 写入（第一次让英雄加压力的变化改成 0）", "stress.zero", "英雄还没加过压力" + probe(("stress.pre.",), "stress.pre")),
            ("skill.pre", "skill.pre：读字段；写指针、写只读字段、取消都报错（Lua 不能写指针）", "skill.pre", "还没有技能事件（打一场战斗）"),
            ("cancel.error", "不能取消的 .pre（heal.pre）调用 cancel 报错", "cancel.error", "还没有 heal.pre（有人治疗一次，满血也行）"),
            ("payload.expired", "回调返回后负载失效（访问报错）", "payload.expired", "还没有 damage.post"),
            ("campaign.load", "main.lua 里订阅的 campaign.load 收到", "campaign.load", "没有进存档或 campaign.load 没收到"),
            ("scope.battle", "scope=\"battle\" 的订阅在战斗结束后撤销", "scope.battle", "战斗结束后 2 秒内就退出了，或没有战斗"),
            ("scope.raid", "scope=\"raid\" 的订阅在探索结算后撤销", "scope.raid",
                "没有探索结算：要在同一次进入存档里完成或放弃这次探索（回城的结算界面），结算后等 3 秒"),
        ]
        # 减半判不了（受到伤害的百分比修正、掉血被截断）时 p3d_core 只写「信息」，下一场再减半一次（第二轮审核 R2B-备注4）
        halve_info = [l.text for l in (self.lua or []) if l.who == "p3d_core" and l.body.startswith("信息 [damage.halve]")]
        for key, title, tag, hint in rows:
            if key == "damage.halve" and halve_info:
                hint = [hint + "；p3d_core 写过「信息 [damage.halve]」：英雄有受到伤害的百分比修正，判不了，下一场会再减半一次"
                    "（每次进存档最多 3 次），多打一两场"] + halve_info[:3]
            self.tag_result(key, title, tag, hint)

    def check_deferred(self):
        title = "延迟事件的指针字段在 Lua 里为 nil（raid.start / inventory.add）"
        oks, bads = [], []
        for tag in ("deferred.raid", "deferred.snapshot", "deferred.inventory"):
            ok, bad = self.tag_lines(tag)
            oks += ok[:1]
            bads += bad
        info = [l for l in (self.lua or []) if l.who == "p3d_core" and l.body.startswith("信息 [deferred.")]
        if bads:
            self.add("deferred", title, FAIL, [l.text for l in bads[:3]], acceptance="#9")
        elif oks:
            self.add("deferred", title, PASS, [l.text for l in oks] + [l.text for l in info[:2]], acceptance="#9")
        else:
            self.add("deferred", title, MISS, ["还没有延迟事件：要从城镇出发一次探索（raid.start）"] + [l.text for l in info[:2]], acceptance="#9")

    def check_isolation(self):
        title = "一个 Mod 报错不影响其他 Mod"
        lua = self.lua or []
        post_err = [l for l in lua if l.who == "p3d_faulty" and l.body.startswith("damage.post 的回调出错")]
        post_off = [l for l in post_err if "已停用该订阅" in l.body]
        ok, bad = self.tag_lines("isolation")
        imp_ok, imp_bad = self.tag_lines("broken.import")
        broken_fail = [l for l in lua if l.who == "p3d_broken" and "加载失败：" in l.body]
        broken_leak = [l for l in lua if l.who == "p3d_broken" and l.body.startswith("失败 [")]
        traceback = any("stack traceback" in l.text or l.body.startswith("\t") for l in lua if l.who == "p3d_faulty" and l.level == "ERROR")
        detail = [l.text for l in post_err[:1] + post_off[:1] + ok[:1] + broken_fail[:1] + imp_ok[:1]]
        if not traceback and post_err:
            detail.append("p3d_faulty 的错误没有 traceback！")
        if bad or broken_leak or imp_bad:
            self.add("isolation", title, FAIL, [l.text for l in (bad + broken_leak + imp_bad)[:3]] + detail, acceptance="#5")
        elif post_off and ok and broken_fail and imp_ok and traceback:
            self.add("isolation", title, PASS, detail, acceptance="#5")
        else:
            missing = [n for n, v in (("p3d_faulty 的 damage.post 连续出错 3 次后停用", post_off), ("p3d_core 的 [isolation] 通过", ok),
                ("p3d_broken 加载失败", broken_fail), ("p3d_core 的 [broken.import] 通过", imp_ok)) if not v]
            self.add("isolation", title, MISS, detail + ["还缺：" + "、".join(missing) + "（要打完一场有 ≥3 次伤害的战斗）"], acceptance="#5")

    def check_budget(self):
        title = "死循环被执行预算打断（用时不超过预算加余量），游戏不卡死"
        lua = self.lua or []
        faulty = [l for l in lua if l.who == "p3d_faulty"]
        pairs = (
            ("battle.start 回调", "信息 [budget] battle.start 回调进入死循环", "battle.start 的回调出错（超出执行预算），已停用该订阅"),
            ("dd.after 定时器", "信息 [budget] 定时器回调进入死循环", "dd.after 的回调出错（超出执行预算）"),
            ("协程", "信息 [budget] 协程进入死循环", "协程超出执行预算，已终止"),
        )
        limit = self.budget_ms + BUDGET_MARGIN_MS
        found, slow, detail = {}, [], []
        for name, enter_text, stop_text in pairs:
            for enter in (l for l in faulty if l.body.startswith(enter_text)):
                stop = next((l for l in faulty if l.index > enter.index and l.body.startswith(stop_text)), None)
                if stop is None:
                    continue
                ms = elapsed_ms(enter.t, stop.t)
                found.setdefault(name, []).append((ms, stop))
                if ms > limit:
                    slow.append(f"{name}的死循环从进入到被打断用了 {ms:.0f} 毫秒（预算 {self.budget_ms} 毫秒，上限 {limit} 毫秒）：{stop.text}")
        for name, items in found.items():
            worst = max(items, key=lambda item: item[0])
            detail.append(f"{name}：打断 {len(items)} 次，最长 {worst[0]:.0f} 毫秒；{worst[1].text}")
        swallowed = [l for l in faulty if l.body.startswith("失败 [budget]")]
        ok, bad = self.tag_lines("budget")
        detail += [l.text for l in ok[:1]]
        stops = [item[1] for items in found.values() for item in items]
        later = []
        if stops:
            last = max(l.index for l in stops)
            later = [l for l in lua if l.index > last and l.who != "p3d_faulty" and not l.text.startswith("\t")]
            detail.append(f"最后一次打断之后又写了 {len(later)} 行日志（游戏还在运行）" + (f"，例如：{later[0].text}" if later else ""))
        if bad or swallowed or slow:
            self.add("budget", title, FAIL, slow + [l.text for l in (bad + swallowed)[:3]] + detail, acceptance="#3")
        elif len(found) == len(pairs) and ok and later:
            self.add("budget", title, PASS, detail, acceptance="#3")
        else:
            missing = [f"{name}的死循环" for name, _, _ in pairs if name not in found]
            missing += [n for n, v in (("p3d_core 的 [budget] 通过（战斗结束时）", ok), ("之后的日志", later)) if not v]
            self.add("budget", title, MISS, detail + ["还缺：" + "、".join(missing) + "（第一场战斗开始后再过 2 秒以上）"], acceptance="#3")

    def check_teardown(self):
        title = "切换存档时拆除和重建（重建后同样的 Mod 加载成功并开始工作）"
        with_vm = [s for s in self.sessions if any(l.body.startswith("加载完成：") for l in s["lines"])]
        if not with_vm:
            self.add("teardown", title, MISS, "没有创建过 Lua VM 的存档", acceptance="#6")
            return
        detail, problems = [], []
        complete = []
        for s in with_vm:
            lines = s["lines"]
            leave = [l for l in lines if l.body.startswith("退出存档")]
            check = [l for l in lines if l.body.startswith("订阅核对（拆除后）")]
            freed = [l for l in lines if re.match(r"拆除 \d+ 个 Lua Mod：", l.body)]
            tick_off = [l for l in lines if l.who == "DDLua" and "撤销 lifecycle TICK" in l.body]
            detail.append(s["start"].text)
            for l in leave[:1] + check[:1] + freed[:1] + tick_off[:1]:
                detail.append("  " + l.text)
            for l in check:
                if not re.search(r"DDLua 持有 SDK 订阅共 0 个，DDCore 统计 DDLua 的订阅 0 个", l.body):
                    problems.append("拆除后还有订阅：" + l.text)
            for l in freed:
                if not l.body.endswith("分配器残留 0 字节"):
                    problems.append("拆除后分配器有残留：" + l.text)
            if leave and check and freed and tick_off:
                complete.append(s)
        # 重建：完整拆除之后，再进一次发现了同样的场景 Mod 的存档，加载的集合和预期一致（至少 1 个），并且有 Mod 开始工作的日志
        rebuilt = None
        unsignaled = None
        for s in complete:
            mods = set(self.session_mods(s)) & set(SCENARIO_MODS)
            for later in (x for x in with_vm if x["start"].index > s["start"].index):
                if set(self.session_mods(later)) & set(SCENARIO_MODS) != mods:
                    continue
                loaded = self.session_loaded(later) & set(SCENARIO_MODS)
                done = next((re.match(r"加载完成：成功 (\d+) 个", l.body) for l in later["lines"] if l.body.startswith("加载完成：")), None)
                if done and int(done.group(1)) != len(self.session_loaded(later)):
                    problems.append(f"存档#{later['serial']} 的「加载完成」说成功 {done.group(1)} 个，「main.lua 执行完毕」的 Mod 有 "
                        f"{len(self.session_loaded(later))} 个")
                    continue
                if not loaded or loaded != self.expected_loaded(mods):
                    problems.append(f"存档#{later['serial']} 重建后加载成功的是 {'、'.join(sorted(loaded)) or '（没有）'}，"
                        f"应为 {'、'.join(sorted(self.expected_loaded(mods))) or '（没有）'}")
                    continue
                serial = later["serial"]
                signal = [l for l in later["lines"] if (l.who == "p3d_core" and l.body.startswith("通过 [campaign.load]") and
                    f"serial={serial}，" in l.body) or (l.who == "hello_lua" and l.body.startswith("进入存档 3 秒了"))]
                if signal:
                    rebuilt = (s, later, loaded, signal[0])
                    break
                unsignaled = later
            if rebuilt:
                break
        if problems:
            self.add("teardown", title, FAIL, problems + detail, acceptance="#6")
        elif rebuilt:
            s, later, loaded, signal = rebuilt
            self.add("teardown", title, PASS, [f"创建过 VM 的存档 {len(with_vm)} 次，完整拆除 {len(complete)} 次；存档#{s['serial']} 拆除后，"
                f"存档#{later['serial']} 重建，加载成功 {len(loaded)} 个（{'、'.join(sorted(loaded))}）", "  " + signal.text] + detail, acceptance="#6")
        elif unsignaled:
            self.add("teardown", title, MISS, detail + [f"存档#{unsignaled['serial']} 重建后没有看到 Mod 开始工作的日志（p3d_core 的 campaign.load、"
                "hello_lua 的 3 秒定时器）：再进一次存档，进去后至少等 5 秒"], acceptance="#6")
        else:
            self.add("teardown", title, MISS, detail + ["要回到主菜单再进一次同一个存档：完整拆除之后，还要看到同样的 Mod 再加载一次"], acceptance="#6")

    def check_subscription_counts(self):
        title = "订阅核对：DDLua 记录的订阅数和 DDCore 统计的一致"
        lines = self.find(self.lua, "订阅核对（")
        bad = [l for l in lines if "不一致" in l.body]
        if not lines:
            self.add("subs", title, MISS, "没有订阅核对行（没有创建过 Lua VM）", acceptance="#2")
        elif bad:
            self.add("subs", title, FAIL, [l.text for l in bad[:3]], acceptance="#2")
        else:
            self.add("subs", title, PASS, [f"{len(lines)} 行订阅核对，全部一致"] + [l.text for l in lines[:2]], acceptance="#2")

    def check_handles_lua(self):
        title = "actor 句柄（Lua 侧，p3c_actor）：跨回调有效、战斗结束后失效、actor 作用域撤销"
        lines = [l for l in (self.lua or []) if l.who == "p3c_actor"]
        if not lines:
            self.add("handles.lua", title, MISS, "没有 [p3c_actor] 的日志：没有勾选「DDLua Test P3c Actor」", acceptance="#8")
            return
        # 阵亡、解雇的英雄由 p3c_actor 自己排除（「不计阵亡、解雇的 N 个」），这里不再放宽：任何「失败」行都判失败
        fails = [l for l in lines if l.body.startswith("失败 ")]
        gone = [l for l in lines if re.match(r"信息 英雄 hero_id=\d+ (阵亡|被解雇)", l.body)]
        need = {
            "第 1 场战斗开始": "打一场战斗",
            "本场怪物句柄": "战斗结束",
            # p3c_actor 等的是正在 damage.pre 里挨打的那个英雄，任何英雄都行（第二轮审核 R2B-应修2）
            "wait_event 快照的 target 就是保存的英雄句柄": "任何一个英雄受到一次伤害（战斗里或走廊陷阱）",
            "怪物作用域的订阅在战斗结束后已撤销": "战斗结束后再等 2 秒",
            "英雄作用域的订阅在战斗结束后仍然有效": "战斗结束",
            "按句柄过滤的 damage.pre 进入 Lua": "战斗结束",
        }
        got = {k: [l for l in lines if k in l.body] for k in need}
        second = [l for l in lines if "第 2 场战斗开始" in l.body]
        detail = [v[0].text for v in got.values() if v] + [l.text for l in second[:1]]
        passed = len([l for l in lines if l.body.startswith("通过 ")])
        detail.insert(0, f"[p3c_actor] 通过 {passed} 行，失败 {len(fails)} 行" + (f"；阵亡、解雇的英雄 {len(gone)} 个（不计入英雄句柄的检查）" if gone else ""))
        if fails:
            self.add("handles.lua", title, FAIL, [l.text for l in fails[:5]] + detail, acceptance="#8")
        elif all(got.values()):
            if not second:
                detail.append("（只打了一场战斗：「上一场的怪物句柄仍有效 0 个」要第二场战斗才检查）")
            self.add("handles.lua", title, PASS, detail, acceptance="#8")
        else:
            self.add("handles.lua", title, MISS, detail + ["还缺：" + "、".join(f"{k}（{v}）" for k, v in need.items() if not got[k])], acceptance="#8")

    def probe_parse(self, line):
        """探针判定行：(前缀, match)；前缀对上、格式对不上时 match 为 None；不是判定行返回 None。"""
        for prefix, pattern in PROBE_FORMATS.items():
            if line.text.startswith(prefix):
                return prefix, pattern.match(line.text)
        return None

    def check_handles_probe(self):
        title = "actor 句柄（native 侧，probe_p3c）：转换、反查、字段读数和独立读法一致，失效后解析不到"
        if self.probe is None:
            self.add("handles.probe", title, NOLOG, "没有 probe_p3c.log：部署的不是 scenario 方案？", acceptance="#8")
            return
        lines = self.probe
        problems = [l.text for l in lines if "！" in l.text][:5]
        unknown = []
        notes = []
        for l in lines:
            parsed = self.probe_parse(l)
            if parsed is None:
                continue
            prefix, m = parsed
            if m is None:
                unknown.append(l.text)
                continue
            if prefix == "CampaignLoad 存档序号":
                if m.group(7) != "0":
                    problems.append(l.text)
            elif prefix == "battle.start 第":
                if m.group(7) != "0" or m.group(4) != m.group(5):
                    problems.append(l.text)
            elif prefix == "battle.end 第":
                # 英雄句柄已经不计阵亡、解雇的：剩下的必须全部能解析
                if m.group(3) != "0" or m.group(4) != m.group(5):
                    problems.append(l.text)
                elif m.group(6) != "0":
                    notes.append(f"{l.text}（阵亡、解雇的英雄单独计数：阵亡的条目留在名册里，解雇的被删除）")
            elif prefix == "指针字段":
                if m.group(4) != "0" or m.group(2) != m.group(5) or m.group(2) != m.group(6):
                    problems.append(l.text)
            elif prefix == "独立读法：":
                if m.group(2) != m.group(3) or m.group(4) != m.group(5) or m.group(7) != "0":
                    problems.append(l.text)
            elif prefix == ".post 结算值":
                if m.group(1) != m.group(2) or m.group(3) != m.group(4):
                    problems.append(l.text)
        summaries = [l for l in lines if l.text.startswith("==== 汇总")]
        for s in summaries:
            following = [x.text for x in lines[s.index + 1:s.index + 4]]
            if len(following) < 3 or any(not text.startswith(prefix) for text, prefix in zip(following, SUMMARY_LINES)):
                unknown.append(f"{s.text} 后面不是「{'」「'.join(SUMMARY_LINES)}」三行")
        ends = [l for l in lines if l.text.startswith("battle.end 第")]
        starts = [l for l in lines if l.text.startswith("battle.start 第")]
        loads = [l for l in lines if l.text.startswith("CampaignLoad 存档序号")]
        cross_save = [l for l in loads if re.search(r"上一个存档的英雄句柄 [1-9]\d* 个，仍能解析 0 个", l.text)]
        detail = [l.text for l in (starts[:1] + ends[:1] + loads[1:2] + summaries[:1])]
        for l in summaries[:1]:
            detail += [x.text for x in lines[l.index + 1:l.index + 4]]
        detail += notes
        if unknown:
            self.add("handles.probe", title, FORMAT, ["探针日志的格式和检查脚本不一致，判不了（探针改过输出？）："] + unknown[:5] + problems[:3], acceptance="#8")
        elif problems:
            self.add("handles.probe", title, FAIL, problems[:6] + detail, acceptance="#8")
        elif ends and summaries:
            if not cross_save:
                detail.append("（没有第二次进存档时的「上一个存档的英雄句柄 N 个，仍能解析 0 个」：跨存档失效没有检查）")
            self.add("handles.probe", title, PASS, detail, acceptance="#8")
        else:
            self.add("handles.probe", title, MISS, detail + ["要打完一场战斗并退出存档（汇总在退出存档时写）"], acceptance="#8")

    def check_battles(self):
        title = "同一次探索里打了至少两场战斗，上一场的怪物句柄解析不到本场同 guid 的怪物（审核 B-B1）"
        if self.probe is None:
            self.add("battles", title, NOLOG, "没有 probe_p3c.log", acceptance="#8")
            return
        # 探针按（存档、探索序号）分组的 battle.start
        groups = []
        for serial, lines in self.probe_sessions.items():
            by_raid = {}
            for l in lines:
                parsed = self.probe_parse(l)
                if parsed and parsed[0] == "battle.start 第" and parsed[1]:
                    by_raid.setdefault(parsed[1].group(2), []).append((l, parsed[1]))
            groups += [(serial, raid, items) for raid, items in by_raid.items() if len(items) >= 2]
        if not groups:
            self.add("battles", title, MISS, "探针没有记到同一个探索序号下的两次 battle.start：在同一次探索里打完第二场战斗，"
                "中间不要回城或退回主菜单", acceptance="#8")
            return
        # 战斗序号不递增是 DDCore 的问题，不管哪次进存档都判失败
        for serial, raid, items in groups:
            serials = [int(m.group(3)) for _, m in items]
            if any(b <= a for a, b in zip(serials, serials[1:])):
                self.add("battles", title, FAIL, [f"存档#{serial}、探索序号 {raid}：战斗序号 {'、'.join(map(str, serials))}",
                    "同一次探索里的几场战斗，战斗序号没有递增：DDCore 没有发现新战斗"] + [l.text for l, _ in items[:3]], acceptance="#8")
                return
        # 第二轮审核 R2B-应修1：只在加载了 p3c_actor 的那次进存档里选，并且要有 p3c_actor 对同一场（探索序号、战斗序号）的
        # 逐项检查行，DDLua 一侧的 B-B1 检查（旧句柄失效、==、rawequal、表键、驻留）都跑到了才判通过
        p3c = {s["serial"]: s for s in self.sessions if "p3c_actor" in self.session_loaded(s)}
        candidates = [g for g in groups if g[0] in p3c]
        if not candidates:
            seen = "、".join(f"存档#{serial}（探索序号 {raid}，{len(items)} 场）" for serial, raid, items in groups[:3])
            self.add("battles", title, MISS, [f"探针在 {seen} 记到了同一次探索里的几场战斗，但那几次进存档都没有加载 p3c_actor，"
                "DDLua 一侧的检查（旧句柄失效、==、表键、驻留）没有跑：在加载了测试 Mod 的那次进入里（第 3 步回主菜单再进存档 A 之后）"
                "同一次探索里打两场"], acceptance="#8")
            return
        unknown, failed, passed, notes = [], [], None, []
        for serial, raid, items in candidates:
            later = [(l, m) for l, m in items[1:] if int(m.group(6)) > 0]
            same = [(l, m) for l, m in later if int(m.group(8)) > 0]
            head = f"存档#{serial}、探索序号 {raid}：战斗 {len(items)} 场，战斗序号 {'、'.join(m.group(3) for _, m in items)}"
            if not later:
                notes.append((1, head, "后面的战斗开始时探针手里没有上一场的怪物句柄（上一场没有怪物参与的事件？）：再打一场"))
                continue
            if not same:
                notes.append((2, head, "上一场的怪物 guid 在后面的战斗里都没有出现，跨战斗同 guid 的情况没有碰上"
                    "（游戏每场战斗从同一个基数重新编号，正常会碰上；再打一场）"))
                continue
            battles = {m.group(3) for _, m in same}
            lua = []
            for l in p3c[serial]["lines"]:
                if l.who != "p3c_actor" or not LUA_SAME_PREFIX.match(l.body):
                    continue
                m = LUA_SAME.match(l.body)
                if m is None:
                    unknown.append(l.text)
                elif m.group(3) == raid and m.group(4) in battles:
                    lua.append((l, m))
            if not lua:
                notes.append((3, head, f"探针在战斗序号 {'、'.join(sorted(battles))} 碰上了同 guid 的怪物，p3c_actor 没有对应的"
                    f"「第 N 场（探索 {raid}，战斗 B）：上一场的怪物里和本场怪物 guid 相同」行，DDLua 一侧的检查没有跑到"))
                continue
            for l, m in lua:
                n = int(m.group(5))
                pairs = [(int(m.group(6 + 2 * i)), int(m.group(7 + 2 * i))) for i in range(len(LUA_SAME_ITEMS))]
                short = [f"{name} {a}/{b}" for name, (a, b) in zip(LUA_SAME_ITEMS, pairs) if not (a == b == n)]
                if m.group(1) == "失败" or short or n == 0:
                    failed.append(f"{l.text}（没做到：{'、'.join(short) or '见原文'}）")
                elif passed is None:
                    passed = [head] + [x.text for x, _ in items[:3]] + [l.text]
        if unknown:
            self.add("battles", title, FORMAT, ["p3c_actor 的同 guid 检查行和检查脚本的格式不一致（p3c_actor 改过输出？）："] + unknown[:3], acceptance="#8")
        elif failed:
            self.add("battles", title, FAIL, failed[:3], acceptance="#8")
        elif passed:
            # 探针的「仍能解析 0 个」在 handles.probe 判
            self.add("battles", title, PASS, passed, acceptance="#8")
        else:
            rank, head, hint = max(notes)
            self.add("battles", title, MISS, [head, hint], acceptance="#8")

    def check_cross(self):
        title = "探针收到的主题 Lua Mod 也收到了（区分「游戏没派发」和「hook 或派发坏了」）"
        if self.probe is None:
            self.add("cross", title, NOLOG, "没有 probe_p3c.log")
            return
        compared, problems = [], []
        for s in self.scenario_sessions():
            serial = s["serial"]
            plines = self.probe_sessions.get(serial)
            if not plines:
                continue
            lines = s["lines"]
            loaded = self.session_loaded(s)
            battles = sum(1 for l in plines if l.text.startswith("battle.start 第"))
            if battles:
                for mid, test in (("p3d_core", lambda l: l.body.startswith("信息 [battle] 第") and "战斗开始" in l.body),
                                  ("hello_lua", lambda l: l.body.startswith("战斗开始：")),
                                  ("p3c_actor", lambda l: "场战斗开始：" in l.body)):
                    if mid not in loaded:
                        continue
                    # 只在 Lua 一侧一次也没有时判失败：次数不同也可能是日志限速或最后一场没打完
                    n = sum(1 for l in lines if l.who == mid and test(l))
                    compared.append(f"存档#{serial}：battle.start 探针 {battles} 次，{mid} {n} 次")
                    if n == 0:
                        problems.append(f"存档#{serial}：探针收到 battle.start {battles} 次，{mid} 一次也没收到：hook 或 DDLua 的派发有问题")
            if "p3d_core" not in loaded:
                continue
            # p3d_core 每次进存档对第一个 heal.pre、skill.pre 都会写一行判定，和改写的条件无关
            for topic, prefixes, tag in (("heal.pre", ("heal.post ", "heal.pre."), "cancel.error"), ("skill.pre", ("skill.pre.",), "skill.pre")):
                n = sum(1 for l in plines if l.text.startswith(prefixes))
                if not n:
                    continue
                ok, bad = self.tag_lines(tag, lines=lines)
                compared.append(f"存档#{serial}：{topic} 探针记了 {n} 行，p3d_core 的 [{tag}] {len(ok) + len(bad)} 行")
                if not ok and not bad:
                    problems.append(f"存档#{serial}：探针收到了 {topic}（{n} 行），p3d_core 一次也没收到（没有 [{tag}]）：hook 或 DDLua 的派发有问题")
        if problems:
            self.add("cross", title, FAIL, problems + compared)
        elif compared:
            self.add("cross", title, PASS, compared)
        else:
            self.add("cross", title, MISS, "探针和 p3d_core 没有同一次进存档的记录可比（要进存档打一场战斗）")

    def check_hook_report(self):
        title = "CampaignLoad 的订阅数 = 主菜单的订阅数 + DDLua 的订阅数（#2/#7 的辅助核对，scenario 方案）"
        if self.core is None:
            self.add("hooks", title, NOLOG, "没有 DDCore.log", acceptance="#2")
            return
        reports = self.logs.hook_reports()
        base = reports.get("AssetsReady")
        loads = {k[1]: v for k, v in reports.items() if isinstance(k, tuple)}
        if not base:
            self.add("hooks", title, FAIL, "DDCore.log 里没有 AssetsReady 的 hook 报告", acceptance="#2")
            return
        detail = [f"AssetsReady：共 {base[0]} 个目标，listener {base[1]}，语义 hook {base[2]}，订阅 {base[3]}"]
        if not loads:
            self.add("hooks", title, MISS, detail + ["没有 CampaignLoad 的 hook 报告：没有进过存档"], acceptance="#2")
            return
        problems = []
        nolua = []
        for serial, rep in sorted(loads.items()):
            session = next((s for s in self.sessions if s["serial"] == serial), None)
            text = f"CampaignLoad（存档#{serial}）：共 {rep[0]} 个目标，listener {rep[1]}，语义 hook {rep[2]}，订阅 {rep[3]}"
            if session is None:
                text += "；DDLua.log 里没有这个存档"
            else:
                m = next((re.search(r"DDCore 统计 DDLua 的订阅 (\d+) 个", l.body) for l in session["lines"] if l.body.startswith("订阅核对（加载完成）")), None)
                lua_count = int(m.group(1)) if m else 0
                if not any(l.body.startswith("加载完成：") for l in session["lines"]):
                    nolua.append(serial)
                text += f"；DDLua 的订阅 {lua_count}，应为 {base[3]} + {lua_count} = {base[3] + lua_count}"
                if rep[3] != base[3] + lua_count:
                    problems.append(text)
            detail.append(text)
        self.add("hooks", title, FAIL if problems else PASS, problems + detail if problems else detail, acceptance="#2")
        # 没有 Lua Mod 的存档：CampaignLoad 的报告和主菜单完全相同（DDLua 不加任何东西）。scenario 方案带着探针，只是相对比较
        nolua_title = "scenario 方案里没有 Lua Mod 的存档：CampaignLoad 的 hook 报告和主菜单相同（相对比较）"
        if nolua:
            bad = [s for s in nolua if loads[s][:4] != base[:4]]
            self.add("hooks.nolua", nolua_title, FAIL if bad else PASS,
                [f"存档#{s}：{loads[s][4]}（主菜单：{base[4]}）" for s in nolua], required=False, acceptance="#7 辅助")
        else:
            self.add("hooks.nolua", nolua_title, MISS, "这次运行里没有进过不带 Lua Mod 的存档", required=False, acceptance="#7 辅助")

    def check_lua_profile(self):
        """验收 #7：lua 方案（DDCore + DDLua），进一个没有 Lua Mod 的存档，CampaignLoad 的 hook 报告是 1 个目标、语义 hook 0、订阅 0。"""
        title = "lua 方案、没有 Lua Mod 的存档：CampaignLoad 的 hook 报告 1 个目标、语义 hook 0、订阅 0"
        run = self.lua_run
        if run is None:
            self.add("hooks.lua", title, MISS, "没有给 --lua-log-dir：场景最后一步（rig.py deploy lua，进一个没有勾选测试 Mod 的存档，"
                "退出后 rig.py save-log lua）的日志", acceptance="#7")
            return
        if run.lua is None or run.core is None:
            self.add("hooks.lua", title, NOLOG, f"{run.dir} 下缺 DDLua.log 或 DDCore.log", acceptance="#7")
            return
        config, _, problems = run.config()
        id_detail, id_problems = run.identity("lua", ("DDLua.log", "DDCore.log"))
        problems += id_problems + run.serial_problems()
        detail = [config.text if config else "没有配置行"] + id_detail
        reports = run.hook_reports()
        want = lambda rep: rep[0] == 1 and rep[2] == 0 and rep[3] == 0
        base = reports.get("AssetsReady")
        if not base:
            problems.append("DDCore.log 里没有 AssetsReady 的 hook 报告")
        else:
            detail.append(f"AssetsReady：{base[4]}，共 {base[0]} 个目标")
            if not want(base):
                problems.append(f"主菜单（AssetsReady）的报告不是 1 个目标、语义 hook 0、订阅 0：共 {base[0]} 个目标，{base[4]}")
        loads = {k[1]: v for k, v in reports.items() if isinstance(k, tuple)}
        sessions = {s["serial"]: s for s in split_sessions(run.lua)}
        for serial, rep in sorted(loads.items()):
            detail.append(f"CampaignLoad（存档#{serial}）：共 {rep[0]} 个目标，{rep[4]}")
            if not want(rep) or (base and rep[1] != base[1]):
                problems.append(f"存档#{serial} 的 CampaignLoad 报告不是 1 个目标、语义 hook 0、订阅 0（listener 和主菜单相同）：共 {rep[0]} 个目标，{rep[4]}")
            s = sessions.get(serial)
            if s:
                detail.append("  " + s["start"].text)
                if any(l.body.startswith("加载完成：") for l in s["lines"]):
                    problems.append(f"存档#{serial} 加载了 Lua Mod，这一步要用没有 Lua Mod 的存档（测试 Mod 的目录在 lua 方案下已删除，加载的是别的 Lua Mod？）")
                elif not any(l.body.startswith("没有可加载的 Lua Mod") for l in s["lines"]):
                    problems.append(f"存档#{serial} 没有「没有可加载的 Lua Mod，不创建 VM」")
                m = re.search(r"(\d+) 个没有找到目录", s["start"].body)
                if m and m.group(1) != "0":
                    detail.append(f"  启用列表里有 {m.group(1)} 个 Mod 没有找到目录（用的是场景存档？游戏对缺失 Mod 的处理没有确认）")
        if any(l.who == "DDLua" and "订阅 lifecycle TICK" in l.body and "撤销" not in l.body for l in run.lua):
            problems.append("DDLua 订阅了 lifecycle TICK（创建过 Lua VM）")
        if problems:
            self.add("hooks.lua", title, FAIL, problems + detail, acceptance="#7")
        elif loads:
            self.add("hooks.lua", title, PASS, detail, acceptance="#7")
        else:
            self.add("hooks.lua", title, MISS, detail + ["这次 lua 方案的运行没有进过存档"], acceptance="#7")

    def check_other_errors(self):
        title = "DDLua.log 里没有意外的错误，场景 Mod 的日志没有被限速丢弃"
        expected_who = {"p3d_faulty", "p3d_broken"}
        bad = []
        # 每个 Mod 每秒最多 log_lines_per_sec 行（默认 200），超出的丢弃后补一行汇总。场景 Mod 的判定行都按事件计，
        # 远低于这个数；出现汇总行说明判定行可能丢了，结论不可信
        # DDLua 替 Mod 写的错误、traceback 也计入同一个限速（第二轮 G1）；一个会话里和 Mod 有关的日志超过 log_max_mb
        # 之后不再写入（同样会丢判定行）；拆除时关闭协程超过 teardown_budget_ms（场景 Mod 没有 <close>，不该出现）
        for l in self.lua or []:
            if l.who in SCENARIO_MODS and l.body.startswith("日志限速："):
                bad.append(l.text + "（场景 Mod 的日志被限速丢弃，判定行可能缺失）")
            elif "到了 log_max_mb" in l.body or "因为 log_max_mb 有" in l.body:
                bad.append(l.text + "（和 Mod 有关的日志到了总量上限，之后的判定行没有写入）")
            elif "的总时间到了 teardown_budget_ms" in l.body:
                bad.append(l.text + "（场景 Mod 没有 <close>，拆除时不该用完这个时间）")
        for l in self.lua or []:
            if l.level != "ERROR" or l.who in expected_who or l.text.startswith("\t") or l.body.startswith("\t") or l.body.startswith("stack traceback"):
                continue
            if l.body.startswith("失败 ["):
                continue  # 各项已经单独判定
            bad.append(l.text)
        if bad:
            self.add("errors", title, FAIL, bad[:6])
        else:
            self.add("errors", title, PASS, "除了 p3d_faulty、p3d_broken 故意的错误，没有 ERROR；场景 Mod 没有被日志限速，没有到 log_max_mb")

    def run(self):
        if not self.check_logs():
            self.check_lua_profile()
            return self.results
        self.check_enabled_list()
        self.check_load_order()
        self.check_example()
        self.check_writes()
        self.check_deferred()
        self.check_isolation()
        self.check_budget()
        self.check_teardown()
        self.check_subscription_counts()
        self.check_handles_lua()
        self.check_handles_probe()
        self.check_battles()
        self.check_cross()
        self.check_hook_report()
        self.check_lua_profile()
        self.check_other_errors()
        return self.results


def report(results, out=sys.stdout):
    for r in results:
        tag = f"（验收 {r.acceptance}）" if r.acceptance else ""
        opt = "" if r.required else "（可选）"
        print(f"[{r.status}] {r.title}{tag}{opt}", file=out)
        for line in r.detail:
            print(f"    {line}", file=out)
    required = [r for r in results if r.required]
    failed = [r for r in results if r.status == FAIL]
    unknown = [r for r in results if r.status == FORMAT]
    missing = [r for r in required if r.status in (MISS, NOLOG)]
    passed = [r for r in results if r.status == PASS]
    print(f"\n汇总：{len(results)} 项，通过 {len(passed)}，失败 {len(failed)}，格式不认识 {len(unknown)}，必需项没有触发 {len(missing)}", file=out)
    if failed:
        print("失败：" + "、".join(r.key for r in failed), file=out)
    if unknown:
        print("格式不认识：" + "、".join(r.key for r in unknown) + "（把输出交给主线）", file=out)
    if missing:
        print("没有触发：" + "、".join(r.key for r in missing), file=out)
    return 1 if failed or unknown else (2 if missing else 0)


# ---------------------------------------------------------------- 自测

PROBE_SAMPLE = """\ufeff[10:00:00.000] [INFO] 日志开始，DDCore 1.0.0，运行标识 {run_id}，插件路径 （自测样本）
[10:00:00.001] [INFO] exe 9db2d73b89aa034e2055df65a139403e18df641f，独立读法启用（基准版本）
[10:00:00.002] [INFO] CampaignLoad 存档序号 1：actor_kinds 英雄 1 怪物 1，探索序号 1，战斗序号 1；上一个存档的英雄句柄 0 个，仍能解析 0 个（应为 0）
[10:00:00.412] [INFO] battle.start 第 1 场：探索序号 2，战斗序号 2，怪物表里 1 个（guid: 4510），actor_find 找回同一指针 1 个；上一场战斗的怪物句柄 0 个，仍能解析 0 个（应为 0），其中 guid 和本场怪物相同 0 个
[10:00:00.500] [INFO] skill.pre.performer=0x20010 -> monster#4510（存档 1，探索 2，战斗 2） 解析回同一指针=是 HP 12.0/无 压力 - 职业 'skeleton'
[10:00:00.600] [INFO] damage.pre.target=0x10000 -> hero#12（存档 1） 解析回同一指针=是 HP 10.0/30.000000 压力 42.500000 职业 'crusader'
[10:00:00.847] [INFO] heal.post hero#12（存档 1） 治疗后 8.0，最大生命 30.0
[10:00:01.353] [INFO] battle.end 第 1 场：本场怪物句柄 1 个，仍能解析 0 个（应为 0）；本存档英雄句柄 2 个，仍能解析 2 个（不计阵亡、解雇的 0 个，其中仍能解析 0 个）
[10:00:03.659] [INFO] battle.start 第 2 场：探索序号 2，战斗序号 3，怪物表里 1 个（guid: 4510），actor_find 找回同一指针 1 个；上一场战斗的怪物句柄 1 个，仍能解析 0 个（应为 0），其中 guid 和本场怪物相同 1 个
[10:00:04.511] [INFO] battle.end 第 2 场：本场怪物句柄 1 个，仍能解析 0 个（应为 0）；本存档英雄句柄 2 个，仍能解析 2 个（不计阵亡、解雇的 0 个，其中仍能解析 0 个）
[10:00:09.142] [INFO] ==== 汇总（退出存档）====
[10:00:09.142] [INFO] 指针字段 20 个：转成句柄 18，不是 actor 2，虚表是 actor 却转换失败 0；解析回同一指针 18，读取成功 18
[10:00:09.142] [INFO] 独立读法：HP 一致 18，压力一致 9/9，最大生命一致 18/18，职业 id 一致 18；HP 大于最大生命 0 次
[10:00:09.142] [INFO] .post 结算值和句柄读数一致 6/6；治疗后不超过最大生命 2/2；战斗 2 场，本存档英雄句柄 2 个
[10:00:09.243] [INFO] CampaignLoad 存档序号 2：actor_kinds 英雄 1 怪物 1，探索序号 2，战斗序号 4；上一个存档的英雄句柄 2 个，仍能解析 0 个（应为 0）
[10:00:15.000] [INFO] ==== 汇总（退出存档）====
[10:00:15.000] [INFO] 指针字段 30 个：转成句柄 27，不是 actor 3，虚表是 actor 却转换失败 0；解析回同一指针 27，读取成功 27
[10:00:15.000] [INFO] 独立读法：HP 一致 27，压力一致 12/12，最大生命一致 27/27，职业 id 一致 27；HP 大于最大生命 0 次
[10:00:15.000] [INFO] .post 结算值和句柄读数一致 9/9；治疗后不超过最大生命 3/3；战斗 3 场，本存档英雄句柄 2 个
[10:00:15.319] [INFO] CampaignLoad 存档序号 3：actor_kinds 英雄 1 怪物 1，探索序号 3，战斗序号 5；上一个存档的英雄句柄 2 个，仍能解析 0 个（应为 0）
[10:00:15.600] [INFO] ==== 汇总（退出存档）====
[10:00:15.600] [INFO] 指针字段 30 个：转成句柄 27，不是 actor 3，虚表是 actor 却转换失败 0；解析回同一指针 27，读取成功 27
[10:00:15.600] [INFO] 独立读法：HP 一致 27，压力一致 12/12，最大生命一致 27/27，职业 id 一致 27；HP 大于最大生命 0 次
[10:00:15.600] [INFO] .post 结算值和句柄读数一致 9/9；治疗后不超过最大生命 3/3；战斗 3 场，本存档英雄句柄 0 个
"""

# 旧的离线预演样本没有插件加载行，自测时补上（格式照 core.cpp）；F2 之后的预演样本自己带
CORE_PLUGIN_LINES = "[10:00:00.000] [INFO] 插件 DDLua 加载成功（版本 1.0.0）\n[10:00:00.000] [INFO] 插件 probe_p3c 加载成功（版本 1.0.0）\n"

# lua 方案的运行（验收 #7）：格式照 core.cpp、plugin.cpp、session.cpp 和 P3c 主菜单自检的真实日志
LUA_RUN_CORE = """\ufeff[11:00:00.000] [INFO] DDCore 1.0.0 启动，运行标识 20261111-110000-9680，主线程 9680，游戏根目录 {root}
[11:00:01.000] [INFO] 发现新协议插件 -> {root}\\mods\\DD Mod Lib\\DDLua.dll
[11:00:01.500] [INFO] 插件 DDLua 加载成功（版本 1.0.0）
[11:00:01.500] [INFO] 阶段 Early，1 个插件已加载
[11:00:01.510] [INFO] ======== hook 报告（AppReady），共 1 个目标 ========
[11:00:01.510] [INFO] #1 内联 Darkest.exe+0x3DCE10 偷取 5 字节，状态 正常
[11:00:01.510] [INFO]     DDCore: per_frame(UpdateStateInternal)（优先级 -1000）
[11:00:01.510] [INFO] 事件系统：已注册 listener 0 个，语义 hook 0 个，订阅 0 个
[11:00:01.510] [INFO] ======== 报告结束 ========
[11:00:02.000] [INFO] 阶段 AssetsReady，帧 2，线程 9680
[11:00:02.000] [INFO] ======== hook 报告（AssetsReady），共 1 个目标 ========
[11:00:02.000] [INFO] #1 内联 Darkest.exe+0x3DCE10 偷取 5 字节，状态 正常
[11:00:02.000] [INFO]     DDCore: per_frame(UpdateStateInternal)（优先级 -1000）
[11:00:02.000] [INFO] 事件系统：已注册 listener 0 个，语义 hook 0 个，订阅 0 个
[11:00:02.000] [INFO] ======== 报告结束 ========
[11:01:00.000] [INFO] 阶段 CampaignLoad，帧 3000，存档序号 1，线程 9680，Campaign=0000000000070000，Buff::Library=0000000000071000，状态 last=2 next=19
[11:01:00.000] [INFO] 启用 Mod 列表 0 项（已安装 300 个），找到目录 0 项
[11:01:00.000] [INFO] actor 句柄：英雄可用，怪物可用，最大生命可用；探索序号 0，战斗序号 0
[11:01:00.001] [INFO] ======== hook 报告（CampaignLoad），共 1 个目标 ========
[11:01:00.001] [INFO] #1 内联 Darkest.exe+0x3DCE10 偷取 5 字节，状态 正常
[11:01:00.001] [INFO]     DDCore: per_frame(UpdateStateInternal)（优先级 -1000）
[11:01:00.001] [INFO] 事件系统：已注册 listener 0 个，语义 hook 0 个，订阅 0 个
[11:01:00.001] [INFO] ======== 报告结束 ========
[11:02:00.000] [INFO] 阶段 CampaignUnload，帧 6000，存档序号 1，线程 9680
"""
LUA_RUN_LUA = """\ufeff[11:00:01.500] [INFO] 日志开始，DDCore 1.0.0，运行标识 20261111-110000-9680，插件路径 {root}\\mods\\DD Mod Lib\\DDLua.dll
[11:00:01.500] [INFO] [DDLua] DDLua 1.0.0（Lua 5.4.9，按 C++ 编译；Lua API 1）加载，DDCore 1.0.0
[11:00:01.500] [INFO] [DDLua] 配置（没有 DDLua.cfg，用默认值）：memory_limit_mb=128 budget_ms=50 load_budget_ms=500 max_failures=3 max_per_mod=1000 frame_budget_ms=20 log_lines_per_sec=200 log_max_mb=32 teardown_budget_ms=200 dev_mode=0 dev_global_dirs=（关闭）
[11:00:01.500] [INFO] [DDLua] 只订阅了 lifecycle（CampaignLoad、CampaignUnload），没有订阅事件主题，没有安装 hook
[11:01:00.000] [INFO] [存档#1] 进入存档：启用 0 个 Mod，0 个没有找到目录（见 DDCore.log）
[11:01:00.000] [INFO] [存档#1] 检查了 0 个 Mod 目录，发现 0 个 Lua Mod
[11:01:00.000] [INFO] [存档#1] 没有可加载的 Lua Mod，不创建 VM
"""


def run_dir(log_dir, lua_dir=None):
    return {r.key: r.status for r in Checker(Path(log_dir), lua_dir).run()}


def selftest(sample_dir):
    sample_dir = Path(sample_dir)
    failures = []

    def expect(name, statuses, wanted):
        for key, status in wanted.items():
            got = statuses.get(key)
            ok = got == status
            print(f"  {'PASS' if ok else 'FAIL'} {name}：{key} = {got}（应为 {status}）")
            if not ok:
                failures.append(f"{name}/{key}")

    def touch_all(d, when):
        for p in d.iterdir():
            os.utime(p, (when, when))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        base = tmp / "base"
        base.mkdir()
        shutil.copy(sample_dir / "DDLua.log", base / "DDLua.log")
        core = (sample_dir / "DDCore.log").read_text(encoding="utf-8-sig").splitlines(True)
        plugins = "" if any("插件 DDLua 加载成功" in line for line in core) else CORE_PLUGIN_LINES
        (base / "DDCore.log").write_text("\ufeff" + core[0] + plugins + "".join(core[1:]), encoding="utf-8")
        # 探针样本用预演 DDCore.log 的运行标识（预演没有标识时去掉这一段）
        rid = RUN_ID.search(core[0])
        probe = PROBE_SAMPLE.replace("{run_id}", rid.group(1)) if rid else PROBE_SAMPLE.replace("，运行标识 {run_id}", "")
        (base / "probe_p3c.log").write_text(probe, encoding="utf-8")
        # 三份样本的首行都是 10:00:00；修改时间统一成同一天的 10:30，让「同一次运行」的核对有确定的日期
        stamp = datetime.now().replace(hour=10, minute=30, second=0, microsecond=0).timestamp()
        touch_all(base, stamp)
        lua_run = tmp / "lua_run"
        lua_run.mkdir()
        (lua_run / "DDCore.log").write_text(LUA_RUN_CORE.format(root=RIG), encoding="utf-8")
        (lua_run / "DDLua.log").write_text(LUA_RUN_LUA.format(root=RIG), encoding="utf-8")
        touch_all(lua_run, stamp + 3600)

        print(f"== 完整样本（{sample_dir} 的 DDLua.log、DDCore.log 加插件加载行，内置的 probe_p3c.log 和 lua 方案的运行）")
        results = Checker(base, lua_run).run()
        report(results)
        expect("完整样本", {r.key: r.status for r in results}, {r.key: PASS for r in results})

        def variant(name, src=base):
            d = tmp / name
            shutil.copytree(src, d)
            return d

        def edit(d, file, fn):
            path = d / file
            mtime = path.stat().st_mtime
            text = path.read_text(encoding="utf-8-sig")
            new = fn(text)
            if new == text:
                failures.append(f"{d.name}：{file} 没有改动（样本里找不到要改的内容）")
            path.write_text(new, encoding="utf-8")
            os.utime(path, (mtime, mtime))
            return d

        def mutated(name, file, old, new, count=1, src=base):
            return edit(variant(name, src), file, lambda t: t.replace(old, new, count))

        def removed(name, file, needle, src=base):
            return edit(variant(name, src), file, lambda t: "".join(l for l in t.splitlines(True) if needle not in l))

        def cut(text, start, end=None):
            """去掉从 start 所在行到 end 所在行之前（没有 end 时到末尾）的内容。"""
            i = text.rfind("\n", 0, text.index(start)) + 1
            j = len(text) if end is None else text.rfind("\n", 0, text.index(end)) + 1
            return text[:i] + text[j:]

        print("== 篡改过的样本（P3d 自测的 13 种）")
        expect("残留", run_dir(mutated("residual", "DDLua.log", "分配器残留 0 字节", "分配器残留 64 字节"), lua_run), {"teardown": FAIL})
        expect("hit 失败", run_dir(mutated("hit", "DDLua.log", "通过 [hit.miss]", "失败 [hit.miss]"), lua_run), {"hit.miss": FAIL})
        expect("没有取消", run_dir(removed("cancel", "DDLua.log", "[damage.cancel"), lua_run), {"damage.cancel": MISS, "damage.cancel.seen": MISS})
        expect("协程没打断", run_dir(removed("budget", "DDLua.log", "协程超出执行预算"), lua_run), {"budget": MISS})
        expect("吞掉预算", run_dir(mutated("swallow", "DDLua.log", "信息 [budget] 协程进入死循环", "失败 [budget] 协程进入死循环"), lua_run),
            {"budget": FAIL})
        expect("句柄没失效", run_dir(mutated("probe", "probe_p3c.log", "本场怪物句柄 1 个，仍能解析 0 个", "本场怪物句柄 1 个，仍能解析 1 个"), lua_run),
            {"handles.probe": FAIL})
        expect("hook 报告", run_dir(edit(variant("hooks"), "DDCore.log", lambda t: re.sub(r"语义 hook 10 个，订阅 (\d+) 个",
            lambda m: f"语义 hook 10 个，订阅 {int(m.group(1)) + 1} 个", t, count=1)), lua_run), {"hooks": FAIL})
        expect("订阅不一致", run_dir(mutated("subs", "DDLua.log", "DDCore 统计 DDLua 的订阅 0 个", "DDCore 统计 DDLua 的订阅 0 个，不一致！"), lua_run),
            {"subs": FAIL})
        expect("p3d_broken 泄漏", run_dir(mutated("leak", "DDLua.log", "[p3d_broken] 已拆除", "[p3d_broken] 失败 [broken] 加载失败的 Mod 的订阅还在被调用"), lua_run),
            {"isolation": FAIL})
        expect("意外错误", run_dir(mutated("error", "DDLua.log", "[INFO] [hello_lua] 战斗开始", "[ERROR] [hello_lua] 战斗开始"), lua_run),
            {"errors": FAIL, "example": FAIL})
        expect("没勾选 p3c", run_dir(removed("nop3c", "DDLua.log", "p3c_actor"), lua_run), {"mods": MISS, "handles.lua": MISS})
        one = edit(variant("one"), "DDLua.log", lambda t: cut(t, "[存档#2] 进入存档"))
        edit(one, "DDCore.log", lambda t: cut(t, "存档序号 2，"))
        edit(one, "probe_p3c.log", lambda t: cut(t, "CampaignLoad 存档序号 2"))
        expect("只进一次存档", run_dir(one, lua_run), {"teardown": MISS, "example": PASS, "logs": PASS})
        empty = tmp / "empty"
        empty.mkdir()
        expect("空目录", run_dir(empty), {"logs": NOLOG, "hooks.lua": MISS})
        # lua 方案的副本里混着上一次 scenario 运行的 probe_p3c.log（save-log 会复制测试目录里的全部日志）
        stale = variant("lua_stale_probe", lua_run)
        shutil.copy2(base / "probe_p3c.log", stale / "probe_p3c.log")
        expect("lua 方案带着旧的探针日志", run_dir(base, stale), {"hooks.lua": PASS})

        print("== 第一轮审核者 C 的反例")
        # A（S1）：没有 lua 方案的运行时 #7 不能整体通过
        expect("A 没有 lua 方案的运行", run_dir(base), {"hooks.lua": MISS, "hooks": PASS})
        # B（S5）：打断用了 40 秒
        expect("B 打断太慢", run_dir(edit(variant("slow"), "DDLua.log", lambda t: re.sub(
            r"\[(\d\d:\d\d:\d\d\.\d{3})\] \[ERROR\] \[p3d_faulty\] battle\.start 的回调出错",
            lambda m: "[10:00:40.417] [ERROR] [p3d_faulty] battle.start 的回调出错", t, count=1)), lua_run), {"budget": FAIL})
        # C（S4）：非默认配置
        expect("C 非默认配置", run_dir(mutated("cfg", "DDLua.log", "配置（没有 DDLua.cfg，用默认值）：memory_limit_mb=128 budget_ms=50",
            "配置（DDLua.cfg）：memory_limit_mb=128 budget_ms=60000"), lua_run), {"logs": FAIL})
        expect("C dev_mode=1", run_dir(mutated("dev", "DDLua.log", "dev_mode=0", "dev_mode=1"), lua_run), {"logs": FAIL})

        # D（S2）：第二次进存档只剩 p3d_broken（0 个加载成功）不能判重建成功
        def only_broken(t):
            s, e = t.index("[存档#2] 进入存档"), t.index("[存档#2] 退出存档")
            keep = [l for l in t[s:e].split("\n") if not re.search(r"\] \[(hello_lua|p3c_actor|p3d_core|p3d_faulty)\] ", l)]
            seg = "\n".join(keep).replace("加载顺序：p3d_broken hello_lua p3c_actor p3d_core p3d_faulty", "加载顺序：p3d_broken")
            seg = seg.replace("加载完成：成功 4 个，共 5 个 Lua Mod", "加载完成：成功 0 个，共 1 个 Lua Mod")
            return (t[:s] + seg + t[e:]).replace("[存档#2] 拆除 4 个 Lua Mod", "[存档#2] 拆除 0 个 Lua Mod")
        expect("D 重建后 0 个", run_dir(edit(variant("rebuild0"), "DDLua.log", only_broken), lua_run), {"teardown": MISS})
        # 审核者的样本 D 只改了「加载顺序」「加载完成」两行（Mod 的行还在）：汇总和逐个 Mod 的行对不上
        def summary_only(t):
            s = t.index("[存档#2] 进入存档")
            return t[:s] + t[s:].replace("加载完成：成功 4 个，共 5 个 Lua Mod", "加载完成：成功 0 个，共 1 个 Lua Mod", 1)
        expect("D 只改汇总行", run_dir(edit(variant("rebuild0-summary"), "DDLua.log", summary_only), lua_run), {"order": FAIL, "teardown": FAIL})
        # 同样的 Mod 再进一次，但 hello_lua 没加载成功：order、teardown 都判失败
        def drop_hello(t):
            s = t.index("[存档#2] 进入存档")
            return t[:s] + t[s:].replace("[hello_lua] main.lua 执行完毕", "[hello_lua] 已加载完毕（改掉）", 1)
        expect("重建后少一个", run_dir(edit(variant("rebuild-1"), "DDLua.log", drop_hello), lua_run), {"order": FAIL, "teardown": FAIL})
        # 重建后没有 Mod 开始工作的日志
        def no_signal(t):
            s = t.index("[存档#2] 进入存档")
            return t[:s] + re.sub(r"[^\n]*(通过 \[campaign\.load\]|进入存档 3 秒了)[^\n]*\n", "", t[s:])
        expect("重建后没有信号", run_dir(edit(variant("nosignal"), "DDLua.log", no_signal), lua_run), {"teardown": MISS})
        # E（备注 1）：探针汇总的措辞变了
        expect("E 探针格式", run_dir(mutated("drift", "probe_p3c.log", "解析回同一指针 18，读取成功 18", "解析回同一指针 17 个，读取成功 16 个"), lua_run),
            {"handles.probe": FORMAT})
        expect("E 汇总少一行", run_dir(removed("drift2", "probe_p3c.log", "独立读法：HP 一致 18"), lua_run), {"handles.probe": FORMAT})
        # F（S6）：漏勾一个测试 Mod 是「没有触发」
        expect("F 漏勾", run_dir(removed("forgot", "DDLua.log", "[p3d_broken]"), lua_run), {"mods": MISS, "isolation": MISS, "order": PASS})
        expect("F 标题不对", run_dir(mutated("title", "DDLua.log", "来源 DDLua Test P3d Broken（", "来源 Some Other Title（", 2), lua_run), {"mods": FAIL})
        # G（S3）：DDLua.log 是旧的完整运行，DDCore.log 只到主菜单
        expect("G 旧的 DDLua.log", run_dir(edit(variant("stale"), "DDCore.log", lambda t: cut(t, "阶段 CampaignLoad")), lua_run), {"logs": FAIL})
        stale = variant("stale-time")
        os.utime(stale / "DDLua.log", (stamp - 86400, stamp - 86400))
        expect("G DDLua.log 是前一天的", run_dir(stale, lua_run), {"logs": FAIL})
        later = mutated("later", "probe_p3c.log", "[10:00:00.000] [INFO] 日志开始", "[10:05:00.000] [INFO] 日志开始")
        os.utime(later / "probe_p3c.log", (stamp + 600, stamp + 600))
        expect("G 探针晚 5 分钟开始", run_dir(later, lua_run), {"logs": FAIL})
        def with_run_id(d, ids):
            """首行的运行标识：有就替换，没有就加在末尾；值为 None 时去掉（样本本身可能已经带标识）。"""
            def change(t, rid):
                first, rest = (t.split("\n", 1) + [""])[:2]
                first = re.sub(r"，运行标识 [0-9A-Za-z_.:-]+", "", first)
                return (first + (f"，运行标识 {rid}" if rid else "")) + "\n" + rest
            for name, rid in ids.items():
                edit(d, name, lambda t, rid=rid: change(t, rid))
            return d
        names = ("DDLua.log", "DDCore.log", "probe_p3c.log")
        same = with_run_id(variant("runid-same"), {name: "20261111-100000-4242" for name in names})
        expect("运行标识相同", run_dir(same, lua_run), {"logs": PASS})
        diff = with_run_id(variant("runid-diff"), {"DDLua.log": "20261111-100000-4242", "DDCore.log": "20261111-100000-4242",
            "probe_p3c.log": "20261111-095959-77"})
        expect("运行标识不同", run_dir(diff, lua_run), {"logs": FAIL})
        expect("只有一份有运行标识", run_dir(with_run_id(variant("runid-one"), {"DDLua.log": None, "probe_p3c.log": None,
            "DDCore.log": "20261111-100000-4243"}), lua_run), {"logs": FAIL})
        expect("都没有运行标识（旧版 DDCore）", run_dir(with_run_id(variant("runid-none"), {name: None for name in names}), lua_run), {"logs": PASS})
        meta = variant("meta-old")
        (meta / LOG_META).write_text(json.dumps({"profile": "scenario", "deployed_at": stamp + 7200}), encoding="utf-8")
        expect("部署晚于日志", run_dir(meta, lua_run), {"logs": FAIL})
        meta = variant("meta-ok")
        (meta / LOG_META).write_text(json.dumps({"profile": "scenario", "deployed_at": stamp - 7200}), encoding="utf-8")
        expect("部署早于日志", run_dir(meta, lua_run), {"logs": PASS})
        meta = variant("meta-profile")
        (meta / LOG_META).write_text(json.dumps({"profile": "lua", "deployed_at": stamp - 7200}), encoding="utf-8")
        expect("方案不对", run_dir(meta, lua_run), {"logs": FAIL})
        expect("插件不对", run_dir(removed("plugins", "DDCore.log", "插件 probe_p3c 加载成功"), lua_run), {"logs": FAIL})

        print("== #7（lua 方案的运行）")
        bad = mutated("lua-hook", "DDCore.log", "语义 hook 0 个，订阅 0 个", "语义 hook 10 个，订阅 11 个", 3, src=lua_run)
        expect("CampaignLoad 有订阅", run_dir(base, bad), {"hooks.lua": FAIL})
        bad = edit(variant("lua-vm", lua_run), "DDLua.log", lambda t: t.replace("[存档#1] 没有可加载的 Lua Mod，不创建 VM",
            "[存档#1] 加载完成：成功 1 个，共 1 个 Lua Mod\n[11:01:00.000] [INFO] [DDLua] 订阅 lifecycle TICK（有 Lua VM，驱动定时器、协程和重载）"))
        expect("加载了 Lua Mod", run_dir(base, bad), {"hooks.lua": FAIL})
        bad = mutated("lua-probe", "DDCore.log", "[11:00:01.500] [INFO] 插件 DDLua 加载成功（版本 1.0.0）",
            "[11:00:01.500] [INFO] 插件 DDLua 加载成功（版本 1.0.0）\n[11:00:01.500] [INFO] 插件 probe_p3c 加载成功（版本 1.0.0）", src=lua_run)
        expect("lua 方案带着探针", run_dir(base, bad), {"hooks.lua": FAIL})
        bad = edit(variant("lua-noload", lua_run), "DDCore.log", lambda t: cut(t, "阶段 CampaignLoad"))
        edit(bad, "DDLua.log", lambda t: cut(t, "[存档#1] 进入存档"))
        expect("lua 方案没进存档", run_dir(base, bad), {"hooks.lua": MISS})
        bad = mutated("lua-cfg", "DDLua.log", "dev_mode=0", "dev_mode=1", src=lua_run)
        expect("lua 方案不是默认配置", run_dir(base, bad), {"hooks.lua": FAIL})

        print("== 新增的判定")
        expect("同一探索只有一场", run_dir(mutated("one-battle", "probe_p3c.log", "battle.start 第 2 场：探索序号 2", "battle.start 第 2 场：探索序号 3"), lua_run),
            {"battles": MISS})
        expect("探针有战斗、p3d_core 没收到", run_dir(edit(variant("cross-battle"), "DDLua.log", lambda t: re.sub(
            r"[^\n]*\[p3d_core\] 信息 \[battle\] 第 \d+ 场战斗开始[^\n]*\n", "", t)), lua_run), {"cross": FAIL})
        expect("探针有治疗、p3d_core 没收到", run_dir(removed("cross-heal", "DDLua.log", "[cancel.error]"), lua_run), {"cross": FAIL, "cancel.error": MISS})
        # 阵亡、解雇的英雄由探针自己排除、单独计数，剩下的英雄句柄必须全部能解析
        dead = mutated("dead-hero", "probe_p3c.log", "battle.end 第 2 场：本场怪物句柄 1 个，仍能解析 0 个（应为 0）；本存档英雄句柄 2 个，仍能解析 2 个（不计阵亡、解雇的 0 个，其中仍能解析 0 个）",
            "actor.death 英雄 hero_id=13：actor_find 成功\n[10:00:04.511] [INFO] battle.end 第 2 场：本场怪物句柄 1 个，仍能解析 0 个（应为 0）；本存档英雄句柄 1 个，仍能解析 1 个（不计阵亡、解雇的 1 个，其中仍能解析 1 个）")
        expect("阵亡的英雄单独计数", run_dir(dead, lua_run), {"handles.probe": PASS})
        expect("没有阵亡却失效", run_dir(mutated("lost-hero", "probe_p3c.log", "本存档英雄句柄 2 个，仍能解析 2 个", "本存档英雄句柄 2 个，仍能解析 1 个"), lua_run),
            {"handles.probe": FAIL})
        expect("p3c_actor 的英雄句柄失效", run_dir(mutated("lost-hero-lua", "DDLua.log", "[p3c_actor] 通过 第 1 场战斗结束：英雄句柄 2 个，仍有效 2 个",
            "[p3c_actor] 失败 第 1 场战斗结束：英雄句柄 2 个，仍有效 1 个"), lua_run), {"handles.lua": FAIL})
        # B-B1：同一次探索的下一场战斗没有碰上同 guid 的怪物、战斗序号没递增、上一场的同 guid 句柄还能解析
        expect("没有同 guid", run_dir(mutated("no-same-guid", "probe_p3c.log", "其中 guid 和本场怪物相同 1 个", "其中 guid 和本场怪物相同 0 个"), lua_run),
            {"battles": MISS})
        expect("战斗序号没递增", run_dir(mutated("battle-serial", "probe_p3c.log", "探索序号 2，战斗序号 3", "探索序号 2，战斗序号 2"), lua_run),
            {"battles": FAIL})
        expect("同 guid 的旧句柄还能解析", run_dir(mutated("same-guid-resolves", "probe_p3c.log", "上一场战斗的怪物句柄 1 个，仍能解析 0 个（应为 0），其中 guid 和本场怪物相同 1 个",
            "上一场战斗的怪物句柄 1 个，仍能解析 1 个（应为 0），其中 guid 和本场怪物相同 1 个"), lua_run), {"handles.probe": FAIL})
        expect("p3c_actor 没有同 guid 的行", run_dir(removed("no-same-guid-lua", "DDLua.log", "上一场的怪物里和本场怪物 guid 相同"), lua_run),
            {"battles": MISS})
        expect("p3c_actor 的同 guid 旧句柄仍有效", run_dir(mutated("same-guid-lua", "DDLua.log",
            "[p3c_actor] 通过 第 2 场（探索 2，战斗 3）：上一场的怪物里和本场怪物 guid 相同的 1 个；旧句柄失效 1/1",
            "[p3c_actor] 失败 第 2 场（探索 2，战斗 3）：上一场的怪物里和本场怪物 guid 相同的 1 个；旧句柄失效 0/1"), lua_run),
            {"handles.lua": FAIL, "battles": FAIL})

        print("== 第二轮审核者 B 的反例")
        # 应修1（审核者的 ce/A_battles_no_lua）：探针的两场同探索战斗所在的存档#1 没有加载 p3c_actor，Lua 一侧的检查没跑
        def drop_p3c_in_session1(t):
            s, e = t.index("[存档#1] 进入存档"), t.index("[存档#2] 进入存档")
            s, e = t.rfind("\n", 0, s) + 1, t.rfind("\n", 0, e) + 1
            return t[:s] + "".join(l for l in t[s:e].splitlines(True) if "p3c_actor" not in l) + t[e:]
        expect("A 存档#1 没有加载 p3c_actor", run_dir(edit(variant("r2b-a"), "DDLua.log", drop_p3c_in_session1), lua_run),
            {"battles": MISS, "handles.lua": PASS, "mods": PASS})
        # 应修1：p3c_actor 的同 guid 行属于另一次探索（探索序号对不上探针的那一组）
        expect("A p3c_actor 的行是别的探索", run_dir(mutated("r2b-a-raid", "DDLua.log", "第 2 场（探索 2，战斗 3）", "第 2 场（探索 9，战斗 3）"), lua_run),
            {"battles": MISS})
        # 应修1：逐项计数不全（旧格式的行）：格式不认识，不放过
        expect("A p3c_actor 的行是旧格式", run_dir(edit(variant("r2b-a-old"), "DDLua.log", lambda t: re.sub(
            r"通过 第 2 场（探索 2，战斗 3）：上一场的怪物里和本场怪物 guid 相同的 1 个；[^\n]*",
            "通过 第 2 场：上一场的怪物里和本场怪物 guid 相同的 1 个，旧句柄全部失效、和新句柄不相等", t)), lua_run), {"battles": FORMAT})
        # 应修2（审核者的 ce/B_wait_timeout）：日志里没有快照的判定行时仍是「没有触发」；修好的 p3c_actor 等的是正在挨打的英雄，
        # 离线用例「p3c_actor 的 wait_event 快照不依赖第一个英雄」覆盖
        def wait_timeout(t):
            t = re.sub(r"[^\n]*\[p3c_actor\] 通过 wait_event 快照[^\n]*\n", "", t)
            return re.sub(r"[^\n]*\[p3c_actor\] 通过 快照 hp_after[^\n]*\n", "", t)
        expect("B 快照超时", run_dir(edit(variant("r2b-b"), "DDLua.log", wait_timeout), lua_run), {"handles.lua": MISS})
        # 备注4：减半判不了只写信息，是「没有触发」
        expect("备注4 减半判不了", run_dir(edit(variant("r2b-halve"), "DDLua.log", lambda t: re.sub(
            r"\[p3d_core\] 通过 \[damage\.halve\] ([^\n]*)", r"[p3d_core] 信息 [damage.halve] \1，判不了，下一场再试", t)), lua_run),
            {"damage.halve": MISS})
        # G1 的 log_max_mb：和 Mod 有关的日志到了总量上限，判定行可能没写
        expect("log_max_mb 到了", run_dir(mutated("logcap", "DDLua.log", "[INFO] [hello_lua] 战斗开始",
            "[WARN] [存档#1] 和 Mod 有关的日志本会话已写 32768 KB，到了 log_max_mb（32 MB），之后不再写入，拆除时写出丢弃的行数\n"
            "[10:00:00.000] [INFO] [hello_lua] 战斗开始"), lua_run), {"errors": FAIL})
        # 日志限速丢了场景 Mod 的行
        expect("场景 Mod 被日志限速", run_dir(mutated("ratelimit", "DDLua.log", "[INFO] [hello_lua] 战斗开始",
            "[WARN] [p3d_core] 日志限速：有 7 行日志没有写入（每个 Mod 每秒最多 200 行，超出的丢弃；DDLua.cfg 的 log_lines_per_sec）\n[10:00:00.000] [INFO] [hello_lua] 战斗开始"), lua_run),
            {"errors": FAIL})
        expect("scope.raid 是必需项", run_dir(removed("noraid", "DDLua.log", "[scope.raid]"), lua_run), {"scope.raid": MISS})
    print(f"\n自测{'通过' if not failures else '失败：' + '、'.join(failures)}")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log-dir", type=Path, default=DEFAULT_LOG_DIR, help=f"scenario 方案那次运行的日志目录，默认 {DEFAULT_LOG_DIR}")
    parser.add_argument("--lua-log-dir", type=Path, help="lua 方案那次运行的日志目录（验收 #7），rig.py save-log 保存的副本")
    parser.add_argument("--selftest", type=Path, metavar="目录", help="用离线预演的日志样本（build/p3d_rehearsal）自测")
    args = parser.parse_args()
    if args.selftest:
        sys.exit(selftest(args.selftest))
    print(f"日志目录：{args.log_dir}" + (f"；lua 方案：{args.lua_log_dir}" if args.lua_log_dir else "") + "\n")
    sys.exit(report(Checker(args.log_dir, args.lua_log_dir).run()))


if __name__ == "__main__":
    main()

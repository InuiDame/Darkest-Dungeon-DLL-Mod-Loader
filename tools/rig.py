"""管理并列的测试目录，让测试不碰真实安装。在 guest 上用 python -B 运行。

测试目录复制可执行文件，数据目录用联接指向真实安装，日志和加载器配置都留在测试目录。
放在 steamapps\\common 下，是为了让加载器的 ..\\..\\workshop 相对路径依然成立。

  create              新建测试目录，并记录真实安装的基线
  refresh             补上新增的 Mod 和数据目录，重写 start.bat
  deploy <方案>       部署构建产物，方案见 PROFILES；换方案时先删掉上一个方案部署的文件和 Lua Mod 目录，
                      方案外的 DDModLoader\\cfg\\DDLua.cfg 改名备份
  run [--seconds N]   启动游戏，N 秒后强杀；默认带 -skipfeflowintro 跳过开场动画，--intro 不跳过。
                      强杀可能碰上写存档，进存档的长流程改用测试目录的 start.bat，在游戏里正常退出
  save-log <名字>     游戏退出后把测试目录的 DDModLoader\\log 复制到 build/rig_logs/<名字>（保留修改时间，
                      附上当时的方案和部署时间）；目标已存在时拒绝，不覆盖、不套层
  tamper-persist <key> [--what roster|raid] [--current-only] [--restore]
                      P4 场景构造「旁路和存档不一致」：游戏停在主菜单（或没运行）时，把测试目录里 DDLua/<key>.ddsave
                      和上一代 .prev 头里记的名册（或 raid）哈希各改一位（--current-only 只改当前一代，上一代一致时
                      读档是 prev），原文件另存 .before-tamper，并记一行到 DDModLoader\\log\\rig_tamper.log（检查脚本
                      按它判断哪次不一致是预期的；游戏在运行时记下 DDCore.log 的运行标识 run，没运行时 run 为 null）。
                      --restore 把还没被游戏重写过的文件恢复成 .before-tamper，同样记一行（"restore": true）。
                      游戏把改过的当前一代轮换成 .prev 时，备份跟着换名（.ddsave.prev.before-tamper），照样能恢复；
                      头里已经是 rig_tamper.log 记过的改后哈希的文件不再改（再改一位可能正好改回原值）
  reset-persist       游戏没运行时，把测试目录的 DDModLoader\\save（全部存档位的旁路）改名为 save.old-<时间> 留底，
                      之后第一次读档是 empty。存档在别的方案下进过（旁路落后）、要重跑 P4 场景时用
  status              当前方案、部署时间、DDModLoader\\cfg 里的文件、游戏是否在运行、真实安装是否被改动
  verify-real         只检查真实安装（基线文件和 rig.py 会写的全部位置），改动过时退出码为 1
  compare-log         对比测试目录和真实安装的加载器日志
  destroy             删除测试目录，只移除联接，不进入真实安装

环境变量 DD_RIG_NAME 换测试目录（默认 DarkestDungeon_Dev），只接受 steamapps\\common 下的一层目录名，
形如 DarkestDungeon_<字母、数字、下划线、减号>。会写、会删或会启动游戏的命令（create、refresh、deploy、run、
save-log、destroy）开头都先检查测试目录本身：不是联接或符号链接，实际位置就是 steamapps\\common\\<名字>，
和真实安装不是同一个目录、互不包含。refresh、deploy、run 还检查要写的子目录不是联接。

文件这一层：deploy 只删 .rig.json 里记录的、某个方案确实会部署的相对路径，记录不对时整个拒绝；覆盖写入的目标是
符号链接、联接或硬链接（链接数大于 1）时先移除测试目录里的这个名字再写新文件，不会写穿到链接的另一端。

检查和写入之间仍有一个很短的窗口：tasklist（几百毫秒）放在检查之前，检查之后紧接着写，但这段时间里如果有并发的
外部进程把测试目录或其中的目录、文件换成联接或链接，rig.py 察觉不到。rig.py 防的是误操作和遗留的状态，不防
并发的恶意进程。
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path, PureWindowsPath

COMMON = Path(r"D:\Program Files (x86)\Steam\steamapps\common")
REAL = COMMON / "DarkestDungeon"
RIG_NAME = os.environ.get("DD_RIG_NAME", "DarkestDungeon_Dev")
# 一层目录名：没有分隔符、盘符、..、结尾的点或空格（Windows 会去掉它们，DarkestDungeon. 就是真实安装），也没有 ~（短文件名）
RIG_NAME_PATTERN = re.compile(r"DarkestDungeon_[A-Za-z0-9_-]+")
RIG = COMMON / RIG_NAME
PROJECT = Path(__file__).resolve().parent.parent
BUILD = PROJECT / "build" / "windows" / "x64" / "releasedbg"
STATE = RIG / ".rig.json"

# 根目录下不做联接的目录
OWN_DIRS = {"_windows", "_windowsnosteam", "mods", "DDModLoader"}
ROOT_FILES = ("pin", "steam_appid.txt", "svn_revision.txt")
# 这些目录要实体复制：DD Mod Lib 是 DDCore 的部署位置
COPIED_MODS = {"DD Mod Lib"}
# 和框架冲突的 Mod，只从测试目录卸载（ADR 0017），真实安装保持原样
UNINSTALLED_MODS = {"DDShield"}
BASELINE_FILES = ("_windows/win64/Darkest.exe", "_windows/win64/dbghelp.dll")
# rig.py 会往测试目录里写的目录。它们本身（以及路径上的任何一级）不能是联接或符号链接，
# 否则复制、删除会落到别处，例如真实安装（旧版本的测试目录、手工操作都可能留下这样的联接）
OWN_WRITE_DIRS = ("_windows", "_windows/win64", "mods", "DDModLoader", "DDModLoader/cfg", "DDModLoader/log")
LUA_CFG = "DDModLoader/cfg/DDLua.cfg"
SAVED_LOGS = PROJECT / "build" / "rig_logs"
LOG_META = ".rig-log.json"  # save-log 写进副本目录的说明：方案、部署时间、保存时间

# loader 改名为 dbghelp.dll 放进 win64，mods 是 {Mod 目录: [目标名]}，
# files 是 {测试目录内的相对路径: 生成内容的函数}，和 DLL 一样记进 managed，换方案时删除。
# lua_mods 是 {mods 下的目录名: 工程内的源目录}，整个目录复制进测试目录的 mods（真实安装里没有），
# 目录里放一个标记文件，记进 managed_dirs，换方案时整个删除
_NEGATIVES = [
    "neg_abi_mismatch",
    "neg_missing_dep",
    "neg_load_false",
    "neg_load_crash",
    "neg_query_crash",
    "neg_dep_on_failed",
    "neg_cycle_a",
    "neg_cycle_b",
    "neg_hook_conflict",
    "neg_abi_unset",
    "neg_hook_bad_args",
]
PROFILES = {
    "formal": {"loader": "DDModLoader", "mods": {}},
    "core": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore"]}},
    "hello": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore"], "DDHello": ["hello_native", "hello_chain"]}},
    "events": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore"], "DDEventsTest": ["test_events"]}},
    "probe": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore"], "DDProbe": ["probe_p3"]}},
    # P3c：actor 句柄的运行探针（进存档、打一场战斗后看 probe_p3c.log），DDLua 一起部署
    "probe-actor": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore", "DDLua"], "DDProbe": ["probe_p3c"]}},
    # P3d：只把主题表写成 JSON（DDModLoader/log/topics.json），给 tools/gen_lua_types.py 生成 LuaLS 注解。
    # P4c1 起带 DDLua：它声明持久化，campaign.save 才可用，主题表和 Lua Mod 看到的一致
    "topics": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore", "DDLua"], "DDTopicDump": ["topic_dump"]}},
    # P3d 用户场景：DDCore + DDLua（默认配置）+ 只读探针 probe_p3c + 场景 Lua Mod（只复制进测试目录的 mods，
    # 要在存档的 Mod 列表里勾选）。步骤见 docs/09-lua-scenario.md，跑完用 tools/p3_scenario_check.py 检查日志
    "scenario": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore", "DDLua"], "DDProbe": ["probe_p3c"]},
        "lua_mods": {
            "DDLuaTest_hello_lua": "examples/lua/hello_lua",
            "DDLuaTest_p3c_actor": "tests/lua/scenario/p3c_actor",
            "DDLuaTest_p3d_core": "tests/lua/scenario/p3d_core",
            "DDLuaTest_p3d_faulty": "tests/lua/scenario/p3d_faulty",
            "DDLuaTest_p3d_broken": "tests/lua/scenario/p3d_broken",
        },
    },
    # P3：DDCore + DDLua，默认配置（没有 DDLua.cfg），主菜单上不创建 VM、不订阅事件
    "lua": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore", "DDLua"]}},
    # P3 开发自检：另外写一份 DDLua.cfg（dev_mode = 1），在 AssetsReady 全局加载 tests/lua/dev 下的 Mod（不是按存档加载的路径）
    "lua-dev": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore", "DDLua"]},
        "files": {"DDModLoader/cfg/DDLua.cfg": lambda: lua_dev_cfg()},
    },
    "negative": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore"], "DDHello": ["hello_native", "hello_chain"], "DDNegative": _NEGATIVES},
    },
    # P4a：combat 服务的验证插件 test_combat（只带 DDCore，不带 DDLua）。主菜单上自检 hook 的按需装卸；进存档后在城镇、
    # 走廊、战斗中各做一轮主动测试（临时给英雄加、删 buff，90 帧内删干净）。DDModLoader\cfg\test_combat.cfg 的
    # active = 0 只做被动记录。用法见 docs/10-p4-scenario.md 的「第二段」
    "combat": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore"], "DDCombatTest": ["test_combat"]},
        "files": {"DDModLoader/cfg/test_combat.cfg": lambda: test_combat_cfg()},
    },
    # P4b 主菜单自检：DDCore + DDLua，另写一份 DDLua.cfg（dev_mode = 1），在 AssetsReady 全局加载 tests/lua/dev/dev_combat：
    # 调用 Buff 与 Effect 的只读 API，写操作在主菜单被拒绝（不在存档里），数据目录读写一次。不注册规则、不碰 actor
    "lua-p4b": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore", "DDLua"]},
        "files": {"DDModLoader/cfg/DDLua.cfg": lambda: lua_p4b_cfg()},
    },
    # P4b 用户场景：DDCore + DDLua（默认配置）+ 场景 Lua Mod p4b_combat（只复制进测试目录的 mods，要在存档的 Mod 列表里
    # 勾选「DDLua Test P4b Combat」）。城镇、走廊、战斗各一轮，用法见 docs/10-p4-scenario.md 的「单独跑的方案」；
    # scenario4 方案会一起部署它
    "p4b": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore", "DDLua"]},
        "lua_mods": {"DDLuaTest_p4b_combat": "tests/lua/scenario/p4b_combat"},
    },
    # P4c2 用户场景：DDCore + DDLua（默认配置）+ 4 个场景 Lua Mod（只复制进测试目录的 mods，要在存档的 Mod 列表里勾选）。
    # shield_lite 的副本里另生成圣骑士的职业文件（见 crusader_smite_shield），Smite 给自己加护盾。
    # 不带 test_combat：它的 damage.final 订阅也写 absorb，和护盾的吸收冲突。步骤见 docs/10-p4-scenario.md，
    # 跑完用 tools/p4_scenario_check.py 检查日志
    "scenario4": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore", "DDLua"]},
        "lua_mods": {
            "DDLuaTest_p4b_combat": "tests/lua/scenario/p4b_combat",
            "DDLuaTest_shield_lite": "examples/lua/shield_lite",
            "DDLuaTest_p4c_shield": "tests/lua/scenario/p4c_shield",
            "DDLuaTest_p4c_persist": "tests/lua/scenario/p4c_persist",
        },
        # {Lua Mod 目录名: {目录内的相对路径: 生成内容（bytes）的函数}}，复制完 Lua Mod 之后写进去，随目录一起删除
        "lua_mod_files": {
            "DDLuaTest_shield_lite": {"heroes/crusader/crusader.info.darkest": lambda: crusader_smite_shield()},
        },
    },
    # P4-0C：buff、effect、规则、tray、存档读写的运行探针 probe_p4，只带 DDCore，不带 DDLua 和 Lua Mod。
    # 另写一份 DDModLoader\cfg\probe_p4.cfg：active = 1 打开主动测试（加、删测试 buff，施加 effect，运行时解析
    # effect/buff 文件），改成 0 只做被动观察。用户场景见 tests/plugins/probe_p4.cpp 开头的注释，
    # 日志用 tools/p4_probe_report.py 分析
    "probe4": {
        "loader": "DDModLoader",
        "mods": {"DD Mod Lib": ["DDCore"], "DDProbe": ["probe_p4"]},
        "files": {"DDModLoader/cfg/probe_p4.cfg": lambda: probe_p4_cfg()},
    },
}

LUA_MOD_MARKER = ".rig-managed"  # rig 复制的 Lua Mod 目录里的标记文件，没有它的目录不删
# Lua Mod 目录名（PROFILES 的 lua_mods 和 .rig.json 的 managed_dirs）：没有点、空格、分隔符，Windows 不会改写它
LUA_MOD_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*")

DEV_MODS = ("dev_selftest", "dev_dependent", "dev_loop", "dev_pattern", "dev_memory", "dev_events", "dev_watch")


def lua_dev_cfg():
    dirs = ";".join(str(PROJECT / "tests" / "lua" / "dev" / name) for name in DEV_MODS)
    return (
        "# rig.py deploy lua-dev 生成，换方案时删除\n"
        "memory_limit_mb = 32\n"
        "budget_ms = 50\n"
        "load_budget_ms = 500\n"
        "dev_mode = 1\n"
        f"dev_global_dirs = {dirs}\n"
    )


def lua_p4b_cfg():
    return (
        "# rig.py deploy lua-p4b 生成，换方案时删除\n"
        "dev_mode = 1\n"
        f"dev_global_dirs = {PROJECT / 'tests' / 'lua' / 'dev' / 'dev_combat'}\n"
    )


SMITE_SHIELD_EFFECT = "shield_self_gain_5"


def crusader_smite_shield():
    """P4 场景用的圣骑士职业文件：只读真实安装的 heroes/crusader/crusader.info.darkest，给 Smite 每个等级的 .effect
    后面加上 shield_self_gain_5（Smite 是生成英雄时必带的技能）。游戏数据不放进仓库，每次部署时生成。"""
    source = REAL / "heroes" / "crusader" / "crusader.info.darkest"
    if not source.is_file():
        sys.exit(f"真实安装里没有 {source}，生成不了圣骑士的职业文件")
    lines = source.read_bytes().split(b"\n")
    add = b' "' + SMITE_SHIELD_EFFECT.encode() + b'"'
    changed = 0
    for i, line in enumerate(lines):
        if not re.match(rb'\s*combat_skill:.*\.id\s+"smite"(\s|$)', line):
            continue
        effect = re.search(rb'\.effect(\s+"[^"]*")+', line)
        if effect:
            lines[i] = line[:effect.end()] + add + line[effect.end():]
        else:
            body = line.rstrip(b"\r")
            lines[i] = body + b" .effect" + add + line[len(body):]
        changed += 1
    if changed != 5:
        sys.exit(f"{source} 里 Smite 的 combat_skill 行有 {changed} 行（应为 5 个等级），没有生成职业文件")
    return b"\n".join(lines)


def test_combat_cfg():
    return (
        "# rig.py deploy combat 生成，换方案时删除\n"
        "# active = 1：进存档后做主动测试（城镇、走廊、战斗中各一轮，临时加、删 buff，施加 effect）；0：只做被动记录\n"
        "active = 1\n"
    )


def probe_p4_cfg():
    return (
        "# rig.py deploy probe4 生成，换方案时删除\n"
        "# active = 1：主动测试（在主线程帧边界加、删测试 buff，施加 effect，运行时解析 effect/buff 文件）；0：只做被动观察\n"
        "active = 1\n"
        "# load_inject = 1：在 load_campaign 线程上（4AA580 出口）向 Buff 库插入 ddprobe_p4_load 的定义（active = 0 时不生效）\n"
        "load_inject = 1\n"
        "# exit_hook = 1：hook 退到桌面（3D1CC0），记下退出时还没写完的存档\n"
        "exit_hook = 1\n"
    )


START_BAT ='@echo off\r\nrem The game root must be the working directory.\r\ncd /d "%~dp0"\r\nstart "" "%~dp0_windows\\win64\\Darkest.exe" -skipfeflowintro\r\n'


def sha1(path):
    return hashlib.sha1(path.read_bytes()).hexdigest()


def game_running():
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Darkest.exe"], capture_output=True).stdout
    return b"Darkest.exe" in output


def require_stopped():
    if game_running():
        sys.exit("Darkest.exe 正在运行，先关闭游戏")


def is_link(path):
    """联接、符号链接或其他重分析点（不跟随）。不存在时返回 False。"""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def real_norm(path):
    return os.path.normcase(os.path.normpath(os.path.realpath(path)))


def inside(child, parent):
    """child 和 parent（都已经 real_norm）是同一个目录，或者 child 在 parent 里面。"""
    return child == parent or child.startswith(parent.rstrip("\\/") + os.sep)


def rig_root_problems():
    """测试目录本身的问题（审核 R2B-阻断1）。各级目录的检查（redirected）以「测试目录的实际位置就是它自己」为前提，
    测试目录本身被重定向时那些检查两边一起变，全部失效，所以先查这里。返回说明列表，没有问题时为空。"""
    problems = []
    if not RIG_NAME_PATTERN.fullmatch(RIG_NAME):
        problems.append(f"DD_RIG_NAME 只能是 {COMMON} 下的一层目录名，形如 DarkestDungeon_Dev"
                        f"（DarkestDungeon_ 后面接字母、数字、下划线、减号）：{RIG_NAME!r}")
    if RIG.parent != COMMON or RIG.name != RIG_NAME:  # 不规范化：DarkestDungeon_Dev\..\DarkestDungeon 在这里就对不上
        problems.append(f"测试目录 {RIG} 不是 {COMMON} 下的一层目录")
    if is_link(COMMON) or real_norm(COMMON) != os.path.normcase(os.path.normpath(COMMON)):
        problems.append(f"{COMMON} 本身被联接或符号链接带到了 {os.path.realpath(COMMON)}")
    if is_link(RIG):
        problems.append(f"测试目录本身是联接或符号链接，指向 {os.path.realpath(RIG)}")
    actual, real = real_norm(RIG), real_norm(REAL)
    if actual != os.path.normcase(os.path.normpath(COMMON / RIG.name)):
        problems.append(f"测试目录的实际位置是 {os.path.realpath(RIG)}，不是 {COMMON / RIG.name}")
    if inside(actual, real) or inside(real, actual):
        problems.append(f"测试目录的实际位置 {os.path.realpath(RIG)} 和真实安装 {os.path.realpath(REAL)} 是同一个目录或互相包含")
    return problems


def require_safe_root():
    """会写、会删或会启动游戏的命令开头调用：测试目录本身有问题时拒绝，什么都不动。"""
    problems = rig_root_problems()
    if problems:
        sys.exit("拒绝操作测试目录（测试目录本身有问题，什么都没有改）：\n  " + "\n  ".join(problems))


def require_rig():
    require_safe_root()
    if not RIG.is_dir():
        sys.exit(f"测试目录不存在：{RIG}")


def detach(path):
    """覆盖写入 path 之前调用（审核 R3B-阻断1）：path 是符号链接、联接或别的重分析点，或者是硬链接（链接数大于 1）时，
    先移除测试目录里的这个名字，链接的另一端不动，之后写的是新文件。是实体目录时拒绝。不存在或是普通文件时什么都不做。
    检查和写入之间的窗口见模块说明。"""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return
    if is_link(path):
        target = os.path.realpath(path)
        try:
            os.unlink(path)  # Windows 上对目录联接、目录符号链接也只移除链接本身
        except OSError:
            os.rmdir(path)
        print(f"{path} 是联接或符号链接（指向 {target}），已先移除链接本身再写新文件")
    elif stat.S_ISDIR(info.st_mode):
        sys.exit(f"拒绝覆盖：{path} 是目录")
    elif info.st_nlink > 1:
        os.unlink(path)
        print(f"{path} 是硬链接（链接数 {info.st_nlink}），已先移除测试目录里的这个名字再写新文件，其余链接的内容不变")


def write_file(path, data):
    detach(path)
    Path(path).write_bytes(data)


def write_text_file(path, text):
    """和 Path.write_text 写出的字节相同（Windows 上换行是 CRLF），只是先经过 detach。"""
    write_file(path, text.replace("\n", os.linesep).encode("utf-8"))


def copy_file(source, path):
    detach(path)
    shutil.copy2(source, path)


def load_state():
    return json.loads(STATE.read_text(encoding="utf-8")) if STATE.exists() else {}


def save_state(state):
    write_text_file(STATE, json.dumps(state, indent=1, ensure_ascii=False))


def real_fingerprint():
    """记录基线用（create、refresh）。verify-real 不用它，见 baseline_problems。"""
    missing = [name for name in BASELINE_FILES if not (REAL / name).is_file()]
    if missing:
        sys.exit(f"真实安装里没有 {'、'.join(missing)}，不能记录基线")
    return {name: sha1(REAL / name) for name in BASELINE_FILES}


def baseline_problems(baseline):
    """真实安装的基线文件和基线比较（审核 R3B-阻断1、备注1）：文件不存在、读不了时写成一条问题而不是抛异常；
    文件是链接或有多个硬链接时也报出来，测试目录里的同名文件可能就是它的另一个名字。"""
    problems = []
    for name in BASELINE_FILES:
        path = REAL / name
        try:
            info = os.lstat(path)
        except FileNotFoundError:
            problems.append(f"真实安装的 {name} 不存在（基线里有它）")
            continue
        if is_link(path):
            problems.append(f"真实安装的 {name} 是联接或符号链接，指向 {os.path.realpath(path)}")
        elif info.st_nlink > 1:
            problems.append(f"真实安装的 {name} 有 {info.st_nlink} 个硬链接：别处（例如测试目录）写这些链接，真实安装的内容就跟着变")
        try:
            value = sha1(path)
        except OSError as error:
            problems.append(f"读不了真实安装的 {name}：{error}")
            continue
        if baseline.get(name) != value:
            problems.append(f"真实安装的 {name} 和基线不同")
    return problems


def same_path(a, b):
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


def redirected(relative):
    """测试目录里的这个位置被联接或符号链接带到了别处时返回说明；正常或不存在时返回 None。"""
    path = RIG / relative
    if not os.path.lexists(path):
        return None
    if path.is_junction() or path.is_symlink():
        return f"{relative} 是联接或符号链接，指向 {os.path.realpath(path)}"
    actual = os.path.realpath(path)  # 路径上任何一级是联接都会解析到别处
    if not same_path(actual, Path(os.path.realpath(RIG)) / relative):
        return f"{relative} 的实际位置是 {actual}，不在测试目录里"
    return None


def own_dir_problems(extra=()):
    return [problem for problem in (redirected(relative) for relative in (*OWN_WRITE_DIRS, *extra)) if problem]


def managed_paths():
    """deploy 记进 .rig.json 的 managed 只可能是这些：各方案的插件 DLL 和 files（相对测试目录，/ 分隔）。"""
    paths = set()
    for profile in PROFILES.values():
        for mod, targets in profile["mods"].items():
            paths.update(f"mods/{mod}/{target}.dll" for target in targets)
        paths.update(profile.get("files", {}))
    return paths


def relative_problem(relative):
    """测试目录内相对路径的形状（审核 R3B-阻断1）：不能是绝对路径、带盘符或根，各级不能是 . 或 ..、不能带 :、
    结尾不能是点或空格（Windows 会去掉），规范化之后仍在测试目录下。没有问题时返回 None。"""
    if not isinstance(relative, str) or not relative:
        return f"{relative!r} 不是非空的字符串"
    pure = PureWindowsPath(relative)
    if pure.drive or pure.root:
        return f"{relative!r} 是绝对路径或带盘符、根"
    for part in re.split(r"[\\/]", relative):
        if part in ("", ".", "..") or ":" in part or part != part.rstrip(". ") or any(ord(ch) < 32 for ch in part):
            return f"{relative!r} 里有不允许的一级 {part!r}"
    root = os.path.normcase(os.path.normpath(RIG))
    if not os.path.normcase(os.path.normpath(os.path.join(RIG, relative))).startswith(root + os.sep):
        return f"{relative!r} 规范化之后不在测试目录里"
    return None


def state_problems(state):
    """.rig.json 里 deploy 要删的记录（审核 R3B-阻断1）。managed 只接受某个方案确实会部署的相对路径，
    managed_dirs 只接受 Lua Mod 目录名。返回说明列表，没有问题时为空。"""
    problems = []
    managed = state.get("managed", [])
    if not isinstance(managed, list):
        problems.append(f"managed 不是列表：{managed!r}")
    else:
        allowed = managed_paths()
        for relative in managed:
            problem = relative_problem(relative)
            if problem is None and relative not in allowed:
                problem = f"{relative!r} 不是任何方案会部署的文件"
            if problem:
                problems.append(f"managed：{problem}")
    dirs = state.get("managed_dirs", [])
    if not isinstance(dirs, list):
        problems.append(f"managed_dirs 不是列表：{dirs!r}")
    else:
        problems += [f"managed_dirs：{name!r} 不是 Lua Mod 目录名" for name in dirs
                     if not isinstance(name, str) or not LUA_MOD_NAME.fullmatch(name)]
    return problems


def overwrite_targets():
    """rig.py 会覆盖写入的测试目录文件（相对路径）。"""
    return ["_windows/win64/dbghelp.dll", "_windows/win64/dbghelp.pdb", "start.bat", ".rig.json", *sorted(managed_paths())]


def linked_files():
    """overwrite_targets 里是链接或硬链接的文件（只读检查，status 用）。deploy、refresh 写它们之前会先移除这个名字。"""
    found = []
    for relative in overwrite_targets():
        path = RIG / relative
        try:
            info = os.lstat(path)
        except (FileNotFoundError, NotADirectoryError):
            continue
        if is_link(path):
            found.append(f"{relative} 是联接或符号链接，指向 {os.path.realpath(path)}")
        elif info.st_nlink > 1:
            found.append(f"{relative} 是硬链接（链接数 {info.st_nlink}）")
    return found


def require_own_dirs(extra=()):
    problems = own_dir_problems(extra)
    if problems:
        sys.exit("拒绝写入测试目录，下面的位置不是测试目录里的实体目录：\n  " + "\n  ".join(problems))


def deploy_time(state):
    """最近一次 deploy 的时间（epoch 秒）和来源。旧版本 rig.py 部署的没有记录，用 .rig.json 的修改时间代替（不早于部署）。"""
    if "deployed_at" in state:
        return state["deployed_at"], "deploy 记录"
    if STATE.exists():
        return STATE.stat().st_mtime, ".rig.json 的修改时间，部署时没有记录时间"
    return None, "没有 .rig.json"


def text_time(epoch):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(epoch)) if epoch else "未知"


def link(target, path):
    import _winapi

    if not path.exists():
        _winapi.CreateJunction(str(target), str(path))
        return True
    return False


def sync_links():
    """补齐联接，已有的不动，返回新增数量。"""
    added = 0
    for entry in os.scandir(REAL):
        if entry.is_dir() and entry.name not in OWN_DIRS:
            added += link(Path(entry.path), RIG / entry.name)
    (RIG / "mods").mkdir(exist_ok=True)
    for entry in os.scandir(REAL / "mods"):
        if not entry.is_dir():
            continue
        path = RIG / "mods" / entry.name
        if entry.name in UNINSTALLED_MODS:
            if path.is_junction():
                os.rmdir(path)
            elif path.exists():
                remove_tree(path)
            continue
        if entry.name in COPIED_MODS:
            if path.is_junction():
                os.rmdir(path)  # 只移除链接本身，之后换成实体复制
            if not path.exists():
                shutil.copytree(entry.path, path)
                added += 1
        else:
            added += link(Path(entry.path), path)
    for entry in os.scandir(REAL / "_windows" / "win64"):
        if entry.is_dir():
            added += link(Path(entry.path), RIG / "_windows" / "win64" / entry.name)
    return added


def write_start():
    write_file(RIG / "start.bat", START_BAT.encode("ascii"))


def cmd_create(_):
    require_safe_root()
    if os.path.lexists(RIG):  # 不跟随：悬空的联接也算已存在
        sys.exit(f"目标已存在：{RIG}")
    win64 = RIG / "_windows" / "win64"
    win64.mkdir(parents=True)
    for entry in os.scandir(REAL / "_windows" / "win64"):
        name = entry.name.lower()
        if entry.is_file() and not name.startswith("dbghelp") and not name.endswith((".backup", ".log", ".bak")):
            shutil.copy2(entry.path, win64 / entry.name)
    for name in ROOT_FILES:
        if (REAL / name).exists():
            shutil.copy2(REAL / name, RIG / name)
    (RIG / "DDModLoader" / "log").mkdir(parents=True)
    cfg = REAL / "DDModLoader" / "cfg"
    if cfg.is_dir():
        shutil.copytree(cfg, RIG / "DDModLoader" / "cfg")
    print(f"联接 {sync_links()} 项")
    write_start()
    save_state({"baseline": real_fingerprint()})
    print(f"已创建 {RIG}，接下来用 deploy 部署")


def cmd_refresh(_):
    require_rig()  # 测试目录本身（联接、指向或包含真实安装）
    require_own_dirs()  # mods 若是指向真实安装的联接，下面卸载 DDShield 会删到真实安装
    print(f"新增 {sync_links()} 项")
    write_start()
    state = load_state()
    if "baseline" not in state:
        state["baseline"] = real_fingerprint()
        save_state(state)
        print("已记录真实安装的基线")


def lua_mod_dir(name):
    """rig 管理的 Lua Mod 目录：只能是测试目录 mods 下的一层目录，不能和真实安装的 Mod 同名。"""
    if not isinstance(name, str) or not LUA_MOD_NAME.fullmatch(name):
        sys.exit(f"Lua Mod 目录名不合法（只能用字母、数字、下划线、减号）：{name!r}")
    if (REAL / "mods" / name).exists():
        sys.exit(f"真实安装里有同名的 Mod：{name}，拒绝部署")
    require_own_dirs()  # mods 本身是联接时，这个目录实际在别处
    return RIG / "mods" / name


def remove_lua_mod(name):
    path = lua_mod_dir(name)
    if not path.exists():
        return False
    if path.is_junction() or path.is_symlink():
        sys.exit(f"mods/{name} 是联接，拒绝删除")
    if not (path / LUA_MOD_MARKER).is_file():
        sys.exit(f"mods/{name} 没有 {LUA_MOD_MARKER} 标记，不是 rig 复制的，拒绝删除")
    remove_tree(path)
    return True


def cmd_deploy(args):
    require_stopped()  # tasklist 要几百毫秒，放在检查之前，让检查紧挨着写入（审核 R3B-备注1）
    require_rig()
    profile = PROFILES[args.profile]
    state = load_state()
    # 先确认 .rig.json 的记录、要写和要删的目录都没有问题，再动任何文件
    problems = state_problems(state)
    if problems:
        sys.exit(f"拒绝部署（{STATE} 的记录有问题，什么都没有改；确认之后手工改正或删掉这些记录）：\n  " + "\n  ".join(problems))
    plugin_dirs = {f"mods/{mod}" for mod in profile["mods"]}
    plugin_dirs |= {Path(path).parent.as_posix() for path in state.get("managed", [])}
    require_own_dirs(sorted(plugin_dirs))
    for path in state.get("managed", []):
        (RIG / path).unlink(missing_ok=True)  # 父目录已确认是测试目录里的实体目录，unlink 只去掉这个名字
    removed = [name for name in state.get("managed_dirs", []) if remove_lua_mod(name)]
    state["managed_dirs"] = []
    save_state(state)  # 先记下已删除，后面复制失败也不会留下记录之外的目录

    # 不是 rig 生成的 DDLua.cfg（手写的、别的工具留下的）会让场景和自检不在默认配置下运行：改名备份
    stray_cfg = RIG / LUA_CFG
    stray_backup = None
    if os.path.lexists(stray_cfg):  # 是链接时改名的是链接本身
        stray_backup = stray_cfg.with_name(f"DDLua.cfg.{time.strftime('%Y%m%d-%H%M%S')}.bak")
        stray_cfg.rename(stray_backup)

    # 以下覆盖写入都经过 detach：目标是链接或硬链接时先移除这个名字再写（审核 R3B-阻断1）
    managed = []
    win64 = RIG / "_windows" / "win64"
    copy_file(BUILD / f"{profile['loader']}.dll", win64 / "dbghelp.dll")
    pdb = BUILD / f"{profile['loader']}.pdb"
    if pdb.exists():
        copy_file(pdb, win64 / "dbghelp.pdb")
    for mod, targets in profile["mods"].items():
        (RIG / "mods" / mod).mkdir(exist_ok=True)  # 开头已确认不是联接
        for target in targets:
            copy_file(BUILD / f"{target}.dll", RIG / "mods" / mod / f"{target}.dll")
            managed.append(f"mods/{mod}/{target}.dll")
    for relative, content in profile.get("files", {}).items():
        path = RIG / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        write_text_file(path, content())
        managed.append(relative)
    managed_dirs = []
    for name, source in profile.get("lua_mods", {}).items():
        path = lua_mod_dir(name)
        if path.exists():
            remove_lua_mod(name)  # 上次的记录丢了但有标记：照样删掉重来
        src = PROJECT / source
        if not (src / "project.xml").is_file() or not (src / "lua" / "manifest.json").is_file():
            sys.exit(f"{source} 不是 Lua Mod（缺 project.xml 或 lua/manifest.json）")
        shutil.copytree(src, path)
        (path / LUA_MOD_MARKER).write_text(f"rig.py deploy {args.profile} 从 {source} 复制，换方案时整个删除\n", encoding="utf-8")
        for relative, content in profile.get("lua_mod_files", {}).get(name, {}).items():
            target = path / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            write_file(target, content())
            print(f"生成：mods/{name}/{relative}")
        managed_dirs.append(name)
        state["managed_dirs"] = managed_dirs
        save_state(state)

    state.update(profile=args.profile, managed=managed, managed_dirs=managed_dirs, deployed_at=time.time())
    save_state(state)
    if removed:
        print(f"已删除上一个方案的 Lua Mod 目录：{'、'.join(removed)}")
    if stray_backup:
        print(f"测试目录里有不是 rig 生成的 DDLua.cfg，已改名为 {stray_backup.name}")
    for name in managed_dirs:
        print(f"Lua Mod：mods/{name}（标题 {project_title(RIG / 'mods' / name)}）")
    print(f"已部署 {args.profile}，dbghelp.dll SHA1 {sha1(win64 / 'dbghelp.dll')}")


def project_title(path):
    match = re.search(r"<Title>(.*?)</Title>", (path / "project.xml").read_text(encoding="utf-8", errors="replace"))
    return match.group(1) if match else "（没有标题）"


def cmd_run(args):
    require_stopped()
    require_rig()
    require_own_dirs()  # 游戏往 DDModLoader\log 写日志，它是联接时日志会写到别处（审核 R3B-备注1）
    command = [str(RIG / "_windows" / "win64" / "Darkest.exe")]
    if not args.intro:
        command.append("-skipfeflowintro")
    process = subprocess.Popen(command, cwd=RIG)
    started = time.time()
    try:
        code = process.wait(timeout=args.seconds)
        print(f"游戏退出，退出码 {code:#x}，运行 {time.time() - started:.0f} 秒")
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        print(f"{args.seconds} 秒到，已强杀")


def rig_written_paths():
    """rig.py 会往测试目录写、而真实安装里本不该有的文件和目录（相对游戏根目录）。
    deploy 为插件建的目录（DD Mod Lib 除外，真实安装本来就有）也在内（审核 R2B-备注6）。"""
    paths = {"_windows/win64/dbghelp.pdb", "start.bat", ".rig.json", *managed_paths()}
    for profile in PROFILES.values():
        paths.update(f"mods/{mod}" for mod in profile["mods"] if mod not in COPIED_MODS)
    return sorted(paths)


def real_install_problems():
    """真实安装里出现了 rig.py 才会写的东西（基线文件之外的部分）。"""
    problems = [f"真实安装里有 {relative}（rig.py 只往测试目录写它）" for relative in rig_written_paths() if os.path.lexists(REAL / relative)]
    # deploy 把不是它生成的 DDLua.cfg 改名备份成 DDLua.cfg.<时间>.bak（审核 R2B-备注6）
    cfg = REAL / "DDModLoader" / "cfg"
    if cfg.is_dir():
        for entry in os.scandir(cfg):
            if re.fullmatch(r"DDLua\.cfg\..*\.bak", entry.name, re.IGNORECASE):
                problems.append(f"真实安装里有 DDModLoader/cfg/{entry.name}（rig.py deploy 备份 DDLua.cfg 时的名字）")
    lua_names = {name for profile in PROFILES.values() for name in profile.get("lua_mods", {})}
    for entry in os.scandir(REAL / "mods"):
        if entry.name in lua_names or entry.name.startswith("DDLuaTest_"):
            problems.append(f"真实安装的 mods 里有 {entry.name}（rig 的 Lua Mod 目录名）")
        elif entry.is_dir() and os.path.lexists(Path(entry.path) / LUA_MOD_MARKER):
            problems.append(f"真实安装的 mods/{entry.name} 里有 {LUA_MOD_MARKER}（rig 复制的目录）")
    return problems


def verify_real():
    root = rig_root_problems()
    for problem in root:
        print(f"测试目录本身有问题（基线读自 {STATE}，不可信）：{problem}")
    if root:
        return False
    baseline = load_state().get("baseline")
    if not baseline:
        print("没有基线，先运行 refresh")
        return False
    changed = baseline_problems(baseline)  # 不存在、读不了、链接、和基线不同（审核 R3B-阻断1、备注1）
    for problem in changed:
        print(problem)
    problems = real_install_problems()
    for problem in problems:
        print(problem)
    if not changed and not problems:
        print(f"真实安装未被改动（基线 {len(BASELINE_FILES)} 个文件，另查了 rig 会写的 {len(rig_written_paths())} 个位置、"
              "DDLua.cfg 的备份和 mods 下的 Lua Mod 目录）")
    return not changed and not problems


def cmd_status(_):
    # 只读：测试目录本身有问题时照样列出能看到的东西，问题写在最前面
    for problem in rig_root_problems():
        print(f"测试目录本身有问题（create、refresh、deploy、run、save-log、destroy 都会拒绝）：{problem}")
    if not RIG.is_dir():
        sys.exit(f"测试目录不存在：{RIG}")
    state = load_state()
    print(f"测试目录：{RIG}")
    print(f"当前方案：{state.get('profile', '未知')}")
    when, source = deploy_time(state)
    print(f"部署时间：{text_time(when)}（{source}）")
    for name in state.get("managed_dirs", []):
        path = RIG / "mods" / name
        print(f"Lua Mod：mods/{name}" + (f"（标题 {project_title(path)}）" if path.is_dir() else "（目录不存在！）"))
    cfg = RIG / "DDModLoader" / "cfg"
    names = sorted(os.listdir(cfg)) if cfg.is_dir() else []
    print("DDModLoader\\cfg：" + ("、".join(names) if names else "（空）"))
    if (RIG / LUA_CFG).exists():
        print("  DDLua.cfg " + ("由当前方案生成" if LUA_CFG in state.get("managed", []) else "不是 rig 生成的：DDLua 不在默认配置下运行！"))
    for problem in own_dir_problems():
        print(f"测试目录有问题：{problem}")
    for problem in state_problems(state):
        print(f".rig.json 的记录有问题（deploy 会拒绝）：{problem}")
    for problem in linked_files():
        print(f"测试目录里的 {problem}（deploy、refresh 会先移除这个名字再写新文件，不会写穿）")
    loader = RIG / "_windows" / "win64" / "dbghelp.dll"
    print(f"dbghelp.dll：{sha1(loader) if loader.is_file() else '（没有）'}")
    print(f"游戏运行中：{'是' if game_running() else '否'}")
    verify_real()


def cmd_verify_real(_):
    sys.exit(0 if verify_real() else 1)


def cmd_save_log(args):
    require_stopped()  # 游戏运行时日志还在写
    require_rig()
    name = args.name
    if not re.fullmatch(r"[\w.-]+", name) or name.startswith("."):
        sys.exit(f"名字只能用字母、数字、下划线、点和减号，不能以点开头：{name}")
    source = RIG / "DDModLoader" / "log"
    dest = SAVED_LOGS / name
    if os.path.lexists(dest):
        sys.exit(f"{dest} 已存在，换一个名字（不覆盖，也不往里面再套一层）")
    SAVED_LOGS.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, dest)  # copy2 保留修改时间，检查脚本靠它判断日志属于哪一次运行
    state = load_state()
    when, when_source = deploy_time(state)
    cfg = RIG / "DDModLoader" / "cfg"
    meta = {
        "profile": state.get("profile"),
        "deployed_at": when,
        "deployed_at_source": when_source,
        "saved_at": time.time(),
        "rig": str(RIG),
        "lua_mods": state.get("managed_dirs", []),
        "cfg_files": sorted(os.listdir(cfg)) if cfg.is_dir() else [],
        "dbghelp_sha1": sha1(RIG / "_windows" / "win64" / "dbghelp.dll"),
    }
    (dest / LOG_META).write_text(json.dumps(meta, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"已复制到 {dest}（方案 {meta['profile']}，部署时间 {text_time(when)}）")
    for log in ("DDCore.log", "DDLua.log", "probe_p3c.log"):
        path = dest / log
        if path.exists():
            print(f"  {log}：修改时间 {text_time(path.stat().st_mtime)}")


TAMPER_LOG = "rig_tamper.log"


def in_campaign_now():
    """DDCore.log 里最后一个进出存档的阶段是 CampaignLoad（游戏正在存档里）时返回那一行，否则 None。"""
    log = RIG / "DDModLoader" / "log" / "DDCore.log"
    if not log.is_file():
        return None
    last = None
    for line in log.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if "阶段 CampaignLoad" in line or "阶段 CampaignUnload" in line:
            last = line
    return last if last and "阶段 CampaignLoad" in last else None


def tamper_header(data, name):
    """把旁路文件头里 file <name> <哈希> <大小> 那一行的哈希最后一位改掉（数据的校验和不管头）。返回 (新内容, 旧哈希, 新哈希)。"""
    end = data.find(b"\n\n")
    if not data.startswith(b"DDSAVE ") or end < 0:
        return None
    head = data[:end].split(b"\n")
    for i, line in enumerate(head):
        parts = line.split(b" ")
        if len(parts) == 4 and parts[0] == b"file" and parts[1] == name.encode():
            old = parts[2]
            new = old[:-1] + (b"0" if old[-1:] != b"0" else b"1")
            parts[2] = new
            head[i] = b" ".join(parts)
            return b"\n".join(head) + data[end:], old.decode(), new.decode()
    return None


TAMPER_NAMES = ("persist.roster.json", "persist.raid.json")


def current_run_id():
    """游戏在运行时，测试目录 DDCore.log 第一行的运行标识（DDCore 每次启动重写这个日志）；没运行或读不到时 None。"""
    if not game_running():
        return None
    log = RIG / "DDModLoader" / "log" / "DDCore.log"
    try:
        with open(log, encoding="utf-8-sig", errors="replace") as handle:
            head = handle.readline()
    except OSError:
        return None
    m = re.search(r"运行标识 ([0-9-]+)", head)
    return m.group(1) if m else None


def tampered_by_us(path, backup):
    """path 现在的内容是不是由 backup 改出来的（tamper-persist 写的、之后游戏没有重写过）。"""
    if not path.is_file() or not backup.is_file():
        return False
    data, before = path.read_bytes(), backup.read_bytes()
    return any((r := tamper_header(before, name)) is not None and r[0] == data for name in TAMPER_NAMES)


def header_hash(data, name):
    """旁路文件头里 file <name> <哈希> <大小> 那一行的哈希（大写），没有时 None。"""
    end = data.find(b"\n\n")
    if not data.startswith(b"DDSAVE ") or end < 0:
        return None
    for line in data[:end].split(b"\n"):
        parts = line.split(b" ")
        if len(parts) == 4 and parts[0] == b"file" and parts[1] == name.encode():
            return parts[2].decode(errors="replace").upper()
    return None


def recorded_hashes():
    """rig_tamper.log 里记过的改后哈希（所有运行的）。"""
    log = RIG / "DDModLoader" / "log" / TAMPER_LOG
    out = set()
    if log.is_file():
        for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                record = json.loads(raw)
            except ValueError:
                continue
            for item in record.get("changed", []):
                m = re.search(r"->([0-9A-Fa-f]{16})", item)
                if m:
                    out.add(m.group(1).upper())
    return out


def follow_rotation(current, prev):
    """游戏写旁路时把当前一代轮换成 .prev（审核 R2B-1）：改过的当前一代变成了 .prev 时，把它的备份也改名成 .prev 的备份，
    之后按同名的备份就能认出、恢复它。返回是否换过名。"""
    backup = current.with_name(current.name + ".before-tamper")
    prev_backup = prev.with_name(prev.name + ".before-tamper")
    if backup.is_file() and not tampered_by_us(current, backup) and tampered_by_us(prev, backup):
        # .prev 原来的备份（如果还在）对应的文件已经被轮换掉了，过时的
        if prev_backup.is_file():
            prev_backup.unlink()
        os.replace(backup, prev_backup)
        print(f"{current.name} 改过之后游戏又写了一次，改过的那一代轮换成了 {prev.name}，备份跟着改名为 {prev_backup.name}")
        return True
    return False


def tamper_log(record):
    log = RIG / "DDModLoader" / "log" / TAMPER_LOG
    record = {"time": time.time(), "run": current_run_id(), "game_running": game_running(), **record}
    with open(log, "a", encoding="utf-8") as out:
        out.write(json.dumps(record, ensure_ascii=False) + "\n")
    return log, record


def cmd_tamper_persist(args):
    require_rig()
    if not LUA_MOD_NAME.fullmatch(args.key):
        sys.exit(f"key 只能用字母、数字、下划线、减号：{args.key}")
    inside = in_campaign_now() if game_running() else None
    if inside:
        sys.exit(f"游戏正在存档里（DDCore.log 最后一个进出存档的阶段：{inside.strip()}），先退回主菜单再改旁路")
    root = RIG / "DDModLoader" / "save"
    if is_link(RIG / "DDModLoader") or is_link(root):
        sys.exit(f"{root} 或它的上一级是联接，拒绝修改")
    if args.restore:
        names = (f"{args.key}.ddsave.before-tamper", f"{args.key}.ddsave.prev.before-tamper")
        pattern = f"{args.key}.ddsave*.before-tamper"
        files = [p for p in root.glob(f"*/*/DDLua/{pattern}") if p.name in names and p.is_file() and not is_link(p)]
    else:
        pattern = f"{args.key}.ddsave"
        files = [p for p in root.glob(f"*/*/DDLua/{pattern}") if p.is_file() and not is_link(p)]
    if not files:
        if args.restore:
            sys.exit(f"{root} 下没有 */*/DDLua/{pattern}：没有可恢复的（没改过，或者已经恢复了）")
        sys.exit(f"{root} 下没有 */*/DDLua/{pattern}：这个存档还没有写过旁路（先在存档里让游戏存一次档）")
    chosen = max(files, key=lambda p: p.stat().st_mtime)
    # <账号>\<设备>\DDLua 这三级也不能是联接（审核 C-N2），文件本身经 write_file 的 detach
    for level in (chosen.parent, chosen.parent.parent, chosen.parent.parent.parent):
        if is_link(level):
            sys.exit(f"{level} 是联接，拒绝修改")
    slots = {p.parent for p in files}  # 恢复时同一个存档位可能有两个备份
    if len(slots) > 1:
        print(f"有 {len(slots)} 个存档位{'改过' if args.restore else '写过'} {args.key}，处理最近的那个：{chosen.parent.parent.name}")
    current = chosen.with_name(f"{args.key}.ddsave")
    pairs = [(current, current.with_name(current.name + ".before-tamper")),
             (current.with_name(current.name + ".prev"), current.with_name(current.name + ".prev.before-tamper"))]
    follow_rotation(pairs[0][0], pairs[1][0])
    if args.restore:
        restored = []
        for path, backup in pairs:
            if not backup.is_file():
                continue
            if tampered_by_us(path, backup):
                write_file(path, backup.read_bytes())
                backup.unlink()
                restored.append(path.name)
                print(f"{path}：恢复成改之前的内容，删掉 {backup.name}")
            else:
                print(f"{path.name}：改过的那一代已经不在了（游戏读档后存档时归档成 .mismatch-*，或者又轮换掉了），不用恢复，{backup.name} 留着")
        if not restored:
            # 第 11 步读到不一致之后游戏马上存一次档（读城镇存档时写 persist.narration.json），改过的两代在那时就归档了：
            # 通常是这样，不算出错（审核 R2B 备注 1）
            print("没有要恢复的文件：改过的旁路已经被游戏归档或轮换掉，测试目录里现在的旁路是游戏之后写的")
            return
        log, _ = tamper_log({"key": args.key, "restore": True, "device": current.parent.parent.name, "restored": restored})
        print(f"已记到 {log}")
        return
    name = "persist.roster.json" if args.what == "roster" else "persist.raid.json"
    changed = []
    known = recorded_hashes()
    targets = [(path, backup) for path, backup in pairs[: 1 if args.current_only else 2] if path.is_file()]
    for path, backup in targets:
        # 先全部检查再动手：检查到一半退出时不能留下改了一半、没有记录的文件
        if tampered_by_us(path, backup):
            # 再改一次会用改过的内容覆盖 .before-tamper，原来的就找不回了（审核 B-N5）
            sys.exit(f"{path.name} 已经改过、游戏还没重写（{backup.name} 还在），先 tamper-persist {args.key} --restore")
    for path, backup in targets:
        data = path.read_bytes()
        if header_hash(data, name) in known:
            # 备份对不上（比如轮换了两次）但头里是记过的改后哈希：已经和存档不一致了，再改一位可能正好改回原值（审核 R2B-1）
            print(f"{path.name}：头里的 {name} 哈希 {header_hash(data, name)} 是 rig_tamper.log 记过的改后的值，已经和存档不一致，不再改")
            continue
        result = tamper_header(data, name)
        if result is None:
            print(f"{path.name}：头里没有 {name} 那一行（不是地牢里写的？），没改")
            continue
        new_data, old, new = result
        write_file(backup, data)
        write_file(path, new_data)
        changed.append(f"{path.name} {old}->{new}")
        print(f"{path}：{name} 的哈希 {old} 改成 {new}（原文件另存 {backup.name}）")
    if not changed:
        sys.exit("一个文件都没改")
    log, record = tamper_log({"key": args.key, "what": args.what, "current_only": args.current_only, "device": current.parent.parent.name,
        "changed": changed})
    if record["run"] is None:
        print("游戏没在运行：这条记录（run 为 null）属于下一次启动的游戏")
    if args.current_only:
        print(f"已记到 {log}。现在从主菜单读这个存档：上一代一致时 DDCore 报 {args.key} prev，否则是 mismatch；"
              f"读到 prev 之后游戏存档时，改过的当前一代轮换成 {args.key}.ddsave.prev（--restore 照样能恢复）")
    else:
        # 地牢里的存档在读档前就读到了 raid，所以 raid 改了也是读档时就判不一致（F2，审核 S1）
        print(f"已记到 {log}。现在从主菜单读这个存档：读档时 DDCore 报 {args.key} mismatch（两代的原因都写），dd.store_status() 是 mismatch；"
              f"之后第一次存档前 DDCore 把这两代改名为 {args.key}.ddsave.mismatch-<时间>（--restore 就不用恢复了，要找回时手动改名）")


def cmd_reset_persist(_):
    """把测试目录的旁路整个改名留底（审核 R2B-3）：存档在别的方案下进过、旁路落后时，重跑场景前用。只动测试目录。"""
    require_stopped()
    require_rig()
    base = RIG / "DDModLoader"
    root = base / "save"
    if is_link(base) or is_link(root):
        sys.exit(f"{root} 或它的上一级是联接，拒绝改名")
    if not root.exists():
        print(f"{root} 不存在：还没有写过旁路，不用重置")
        return
    files = sum(1 for p in root.rglob("*") if p.is_file())
    dest = base / ("save.old-" + time.strftime("%Y%m%d-%H%M%S"))
    if os.path.lexists(dest):
        sys.exit(f"{dest} 已存在，过一秒再试")
    os.rename(root, dest)
    print(f"已把 {root} 改名为 {dest.name}（{files} 个文件，留底，不删）。下次进存档时旁路是 empty（没有旁路文件），"
          "之后照常写；要找回时把名字改回去")


def normalized_log(path):
    lines = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if "DDModLoader 退出" in line or any(f"\\mods\\{name}\\" in line for name in UNINSTALLED_MODS):
            continue
        line = re.sub(r"成功: \d+", "成功: N", line)  # 测试目录卸载了部分 Mod，数量和真实安装不同
        lines.append(line.replace(RIG.name, REAL.name).replace("_windowsnosteam", "_windows"))
    return lines


def cmd_compare_log(_):
    require_rig()
    relative = Path("DDModLoader") / "log" / "DDModLoader.log"
    real_lines = normalized_log(REAL / relative)
    rig_lines = normalized_log(RIG / relative)
    # P1 新增的行都带 [DDCore] 前缀，分开统计；其余行必须和真实安装一致
    core_lines = [line for line in rig_lines if "[DDCore]" in line]
    rig_old = [line for line in rig_lines if "[DDCore]" not in line]
    diff = list(difflib.unified_diff(real_lines, rig_old, "real", "rig（去掉 [DDCore] 行）", lineterm="", n=0))
    print(f"[DDCore] 新增日志 {len(core_lines)} 行：")
    for line in core_lines:
        print("  " + line)
    print()
    print("\n".join(diff) if diff else "旧式路径日志一致（路径已归一化）")


def remove_tree(path):
    """逐项删除：联接只移除链接本身，绝不进入。要删的根目录本身是联接或符号链接时拒绝（审核 R2B-阻断1）。"""
    if is_link(path):
        sys.exit(f"拒绝删除：{path} 本身是联接或符号链接（指向 {os.path.realpath(path)}），删除会落到链接目标里")
    for entry in os.scandir(path):
        if entry.is_junction():
            os.rmdir(entry.path)
        elif entry.is_symlink() or entry.is_file(follow_symlinks=False):
            os.unlink(entry.path)
        else:
            remove_tree(entry.path)
    os.rmdir(path)


def cmd_destroy(_):
    require_stopped()
    require_rig()  # 名字、联接、实际位置、和真实安装的关系
    remove_tree(RIG)
    print(f"已删除 {RIG}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, handler in (
        ("create", cmd_create),
        ("refresh", cmd_refresh),
        ("status", cmd_status),
        ("verify-real", cmd_verify_real),
        ("compare-log", cmd_compare_log),
        ("destroy", cmd_destroy),
        ("reset-persist", cmd_reset_persist),
    ):
        commands.add_parser(name).set_defaults(handler=handler)
    deploy = commands.add_parser("deploy")
    deploy.add_argument("profile", choices=PROFILES)
    deploy.set_defaults(handler=cmd_deploy)
    run = commands.add_parser("run")
    run.add_argument("--seconds", type=int, default=45)
    run.add_argument("--intro", action="store_true", help="不跳过开场动画")
    run.set_defaults(handler=cmd_run)
    tamper = commands.add_parser("tamper-persist")
    tamper.add_argument("key", help="旁路文件的 key（Lua Mod 的 id，例如 p4c_persist）")
    tamper.add_argument("--what", choices=("roster", "raid"), default="roster",
        help="改名册的哈希，或 raid 的哈希（地牢里的存档；游戏读档前已经读过 raid，两种都是读档时就不一致）")
    tamper.add_argument("--current-only", action="store_true", help="只改当前一代，上一代不动（上一代和存档一致时读档是 prev）")
    tamper.add_argument("--restore", action="store_true", help="把还没被游戏重写过的文件恢复成 .before-tamper")
    tamper.set_defaults(handler=cmd_tamper_persist)
    save_log = commands.add_parser("save-log")
    save_log.add_argument("name", help="副本目录名，放在 build/rig_logs 下")
    save_log.set_defaults(handler=cmd_save_log)
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()

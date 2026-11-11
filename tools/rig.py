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
    # P3d：只把主题表写成 JSON（DDModLoader/log/topics.json），给 tools/gen_lua_types.py 生成 LuaLS 注解
    "topics": {"loader": "DDModLoader", "mods": {"DD Mod Lib": ["DDCore"], "DDTopicDump": ["topic_dump"]}},
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


START_BAT = '@echo off\r\nrem The game root must be the working directory.\r\ncd /d "%~dp0"\r\nstart "" "%~dp0_windows\\win64\\Darkest.exe" -skipfeflowintro\r\n'


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
    ):
        commands.add_parser(name).set_defaults(handler=handler)
    deploy = commands.add_parser("deploy")
    deploy.add_argument("profile", choices=PROFILES)
    deploy.set_defaults(handler=cmd_deploy)
    run = commands.add_parser("run")
    run.add_argument("--seconds", type=int, default=45)
    run.add_argument("--intro", action="store_true", help="不跳过开场动画")
    run.set_defaults(handler=cmd_run)
    save_log = commands.add_parser("save-log")
    save_log.add_argument("name", help="副本目录名，放在 build/rig_logs 下")
    save_log.set_defaults(handler=cmd_save_log)
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()

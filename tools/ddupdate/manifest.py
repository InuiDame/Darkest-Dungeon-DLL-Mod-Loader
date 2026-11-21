"""解析 signatures.inc 和 layout.inc（X-macro），并检查清单本身。格式说明在两个文件的开头。

解析器只认识清单用到的宏：行首的预处理指令整行跳过（含续行），// 和 /* */ 注释跳过，宏参数里可以有
字符串（相邻的字符串字面量拼接）、整数（十六进制、十进制、负数）、标识符和 `A | B` 形式的表达式。
"""
import re
from dataclasses import dataclass, field
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
PROJECT = TOOLS.parent
ROOT = PROJECT.parent
SIGS_FILE = PROJECT / "core" / "src" / "game" / "signatures.inc"
LAYOUT_FILE = PROJECT / "core" / "src" / "game" / "layout.inc"

SIG_MACROS = ("DD_SIG", "DD_ENTRY", "DD_CHAIN_TARGET", "DD_RUNTIME_SELECT", "DD_KNOWN_BUILD", "DD_DEPS")
LAYOUT_MACROS = ("DD_LAYOUT", "DD_EVIDENCE", "DD_OFFLINE", "DD_UNVERIFIED", "DD_NAMES", "DD_VALUES", "DD_GROUP")

SIG_KINDS = ("HOOK", "GLOBAL", "GLOBAL_ANY", "CODE")
SIG_REQUIRED = ("REQUIRED", "LOADER", "OPT", "ANCHOR")
ITEM_TYPES = ("OFFSET", "SIZE", "SLOT", "ENUM", "COUNT", "CONST", "TABLE")
ITEM_NEEDS = ("REQUIRED", "OPT")
ITEM_CHECKS = ("RUNTIME", "OFFLINE", "NONE")
ORIGINS = ("HIT", "TARGET", "HIT_VALUE", "VSLOT", "VSLOT_VALUE", "NOSIG")
WHERES = ("ENTRY", "WINDOW", "FUNC", "TEXT")
READS = ("NONE", "U8", "U16", "U32", "U64", "I8", "I32", "DEREF_U32", "REL32")

# DD_VALUES 里可以用的符号，C++ 一侧在包含 layout.inc 之前定义同名的 constexpr（见 layout.h）
VALUE_SYMBOLS = {"SRC_S": 1, "SRC_T": 2, "SRC_Y": 4}

# 依赖表的词汇（layout.inc 开头有说明）。topic:<名字> 不在这里，任意主题名都接受，可以用 * 结尾表示前缀
DEPENDENTS = {
    "core": "DDCore 整体（必需项，失效时 DDCore 停用，ADR 0006）",
    "loader": "加载器的早期 hook（CreateAppSystems），失效时 DDCore 不启动",
    "events.native": "原生事件桥（native.* 和映射的语义主题）",
    "events.query": "Query 桥（query.*）",
    "pvp": "PvP 判定：所有 .pre 主题、Query 写回、combat 写操作在 PvP 时停用的前提",
    "game.actor": "actor 句柄（actor_from_pointer、actor_find、actor_resolve，以及所有按句柄解析 actor 的能力）",
    "game.actor_info": "actor_info 的字段（生命、压力、最大生命、职业）",
    "game.actor_list": "party_heroes、battle_monsters",
    "game.battle": "战斗序号和战斗状态（battle_serial、battle_state，怪物句柄的有效性）",
    "game.mods": "启用 Mod 列表（mods_available、mod_count、mod_at）",
    "combat.buff_read": "buff_list、buff_get、buff_def_get",
    "combat.buff_add": "buff_add",
    "combat.buff_remove": "buff_remove",
    "combat.buff_define": "buff_define",
    "combat.effect_get": "effect_get",
    "combat.effect_apply": "effect_apply",
    "combat.effect_define": "effect_define",
    "combat.rule": "rule_register 注册的规则判定",
    "combat.skill": "skill_from_pointer、skill_info",
    "combat.enum": "enum_count、enum_name、enum_value、source_flags 和 combat 的参数校验",
    "persist": "持久化服务（persist.*、campaign.save 等主题）",
    "check": "只给校验和修复用（离线依据、锚点），DDCore 不用它的结果",
}


class ManifestError(Exception):
    pass


@dataclass
class Sig:
    id: str
    kind: str
    required: str
    steal: int
    capture: int
    pattern: str
    file: str
    line: int
    entry: str | None = None
    chain: bool = False
    runtime_select: bool = False
    deps: list = field(default_factory=list)
    has_deps: bool = False

    @property
    def need(self):
        """REQUIRED 和 LOADER 缺失时是必需项失效；OPT、ANCHOR 只影响依赖它的项。注意 LOADER 在这里也是 REQUIRED，
        要区分加载器的特征码时看 required。"""
        return "REQUIRED" if self.required in ("REQUIRED", "LOADER") else "OPT"


@dataclass
class Evidence:
    item: str
    sig: str
    origin: str
    arg: int
    where: str
    span: int
    pattern: str
    at: int
    read: str
    mul: int
    rel: str
    add: int
    line: int
    index: int = 0  # 同一项里的序号（从 0 起），报告和基线用

    def describe(self):
        origin = {"HIT": f"{self.sig} 的命中位置", "TARGET": f"{self.sig} 的解析结果",
                  "HIT_VALUE": f"{self.sig} 的命中位置 + 常量{self.arg:+d}",
                  "VSLOT": f"{self.sig} 虚表槽 {self.arg}", "VSLOT_VALUE": f"{self.sig} 虚表槽（常量）",
                  "NOSIG": ".text"}[self.origin]
        where = {"ENTRY": "起点处", "WINDOW": f"起点后 {self.span:#x} 字节内", "FUNC": "所在函数内", "TEXT": "整个 .text"}[self.where]
        return f"{origin}，{where}匹配 {self.pattern}"


@dataclass
class Item:
    id: str
    type: str
    value: int
    need: str
    check: str
    deps: list
    line: int
    evidence: list = field(default_factory=list)
    offline: list = field(default_factory=list)
    unverified: str | None = None
    names: list | None = None
    values: list | None = None


@dataclass
class Group:
    """DD_GROUP（U1 修复 FB，只给 Python 用，C++ 忽略）：同一张虚表、同一个结构、同一个函数的原型这类「一起变」的项。
    组里有运行时能核对的成员（RUNTIME 项、特征码）变了或失效时，组里没有运行时依据的成员（NONE、OFFLINE 项）
    升级成「需要人工」，accept 要求逐项确认（审核 A 的 S4）。"""
    id: str
    text: str
    members: list
    line: int


@dataclass
class Manifest:
    sigs: dict = field(default_factory=dict)
    known_builds: dict = field(default_factory=dict)
    items: dict = field(default_factory=dict)
    evidence: list = field(default_factory=list)
    groups: dict = field(default_factory=dict)
    sig_file: str = ""
    layout_file: str | None = None

    def value(self, name):
        """依据的对象的常量：清单项是它的值，特征码名字按 0 算（同一性依据），NONE 是 0。
        rel 是特征码名字时它的值是解析结果的 RVA，要映像才知道，见 evidence.rel_value。"""
        if name == "NONE":
            return 0
        if name in self.items:
            return self.items[name].value
        if name in self.sigs:
            return 0
        raise KeyError(name)

    def dependents_of(self, name):
        if name in self.items:
            return self.items[name].deps
        if name in self.sigs:
            return self.sigs[name].deps
        return []


# ---- 词法 ----

_IDENT = re.compile(r"[A-Za-z_]\w*")
_INT = re.compile(r"-?(?:0[xX][0-9A-Fa-f]+|\d+)[uUlL]*")
_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _skip_comment(text, i):
    if text.startswith("//", i):
        end = text.find("\n", i)
        return len(text) if end < 0 else end
    if text.startswith("/*", i):
        end = text.find("*/", i + 2)
        return len(text) if end < 0 else end + 2
    return i


def iter_macros(text, names):
    """返回 [(宏名, [原始参数文本...], 行号)]。"""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            line_start = True
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        j = _skip_comment(text, i)
        if j != i:
            i = j
            continue
        if c == "#" and line_start:
            # 预处理指令整行跳过（含反斜杠续行）
            while i < n:
                end = text.find("\n", i)
                end = n if end < 0 else end
                if text[i:end].rstrip().endswith("\\"):
                    i = end + 1
                    continue
                i = end
                break
            continue
        line_start = False
        if c == '"':
            m = _STRING.match(text, i)
            i = m.end() if m else i + 1
            continue
        m = _IDENT.match(text, i)
        if not m:
            i += 1
            continue
        name = m.group()
        i = m.end()
        if name not in names:
            continue
        k = i
        while k < n and text[k] in " \t":
            k += 1
        if k >= n or text[k] != "(":
            continue
        line = text.count("\n", 0, m.start()) + 1
        args, i = _parse_args(text, k + 1)
        out.append((name, args, line))
    return out


def _parse_args(text, i):
    args, current, depth = [], [], 0
    n = len(text)
    while i < n:
        c = text[i]
        j = _skip_comment(text, i)
        if j != i:
            current.append(" ")
            i = j
            continue
        if c == '"':
            m = _STRING.match(text, i)
            if not m:
                raise ManifestError(f"字符串没有结束（偏移 {i}）")
            current.append(m.group())
            i = m.end()
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            if depth == 0:
                args.append("".join(current).strip())
                return args, i + 1
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    raise ManifestError("宏参数没有结束")


def _unescape(body):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "0": "\0"}.get(m.group(1), m.group(1)), body)


def literal(raw):
    """参数文本转成 Python 值：字符串、整数或标识符（str）。其他形式原样返回（表达式）。"""
    raw = raw.strip()
    if raw.startswith('"'):
        parts = _STRING.findall(raw)
        rest = _STRING.sub("", raw).strip()
        if rest:
            raise ManifestError(f"字符串参数里有多余的内容：{raw!r}")
        return "".join(_unescape(p) for p in parts)
    if _INT.fullmatch(raw):
        return int(raw.rstrip("uUlL"), 0)
    return raw


def _int(raw, what):
    value = literal(raw)
    if not isinstance(value, int):
        raise ManifestError(f"{what} 应该是整数：{raw!r}")
    return value


def _ident(raw, what, choices=None):
    value = literal(raw)
    if not isinstance(value, str) or not _IDENT.fullmatch(value) or raw.strip().startswith('"'):
        raise ManifestError(f"{what} 应该是标识符：{raw!r}")
    if choices and value not in choices:
        raise ManifestError(f"{what} 只能是 {'/'.join(choices)}：{value}")
    return value


def _string(raw, what):
    if not raw.strip().startswith('"'):
        raise ManifestError(f"{what} 应该是字符串：{raw!r}")
    return literal(raw)


def eval_value(raw):
    """DD_VALUES 的一项：整数或用 | 连接的整数和 VALUE_SYMBOLS。"""
    total = 0
    for part in raw.split("|"):
        part = part.strip()
        if part in VALUE_SYMBOLS:
            total |= VALUE_SYMBOLS[part]
        elif _INT.fullmatch(part):
            total |= int(part.rstrip("uUlL"), 0)
        else:
            raise ManifestError(f"DD_VALUES 里不认识的值：{raw!r}")
    return total


def split_deps(text):
    return [t for t in text.split() if t]


# ---- 加载 ----

def load(sig_file=None, layout_file=None, use_layout=True):
    """读清单。sig_file 默认 signatures.inc；layout_file 为 None 且 use_layout 时读默认的 layout.inc。"""
    m = Manifest()
    sig_path = Path(sig_file) if sig_file else SIGS_FILE
    m.sig_file = str(sig_path)
    _load_sigs(m, sig_path)
    if not m.known_builds and sig_path != SIGS_FILE and SIGS_FILE.exists():
        # 插件自己的特征码文件通常不写 DD_KNOWN_BUILD，版本名沿用主清单的
        for name, args, _line in iter_macros(SIGS_FILE.read_text(encoding="utf-8"), ("DD_KNOWN_BUILD",)):
            m.known_builds[literal(args[0]).lower()] = literal(args[1])
    if use_layout:
        layout_path = Path(layout_file) if layout_file else LAYOUT_FILE
        if layout_path.exists():
            m.layout_file = str(layout_path)
            _load_layout(m, layout_path)
        elif layout_file:
            raise ManifestError(f"布局清单不存在：{layout_path}")
    return m


def _where(path, line):
    return f"{Path(path).name}:{line}"


def _load_sigs(m, path):
    text = path.read_text(encoding="utf-8")
    pending = []
    for name, args, line in iter_macros(text, SIG_MACROS):
        at = _where(path, line)
        try:
            if name == "DD_SIG":
                if len(args) != 6:
                    raise ManifestError("DD_SIG 要 6 个参数")
                sig = Sig(_ident(args[0], "id"), _ident(args[1], "kind", SIG_KINDS), _ident(args[2], "required", SIG_REQUIRED),
                          _int(args[3], "steal"), _int(args[4], "capture"), _string(args[5], "pattern"), str(path), line)
                if sig.id in m.sigs:
                    raise ManifestError(f"特征码 {sig.id} 重复")
                m.sigs[sig.id] = sig
            elif name == "DD_KNOWN_BUILD":
                sha, label = _string(args[0], "sha1"), _string(args[1], "label")
                m.known_builds[sha.lower()] = label
            else:
                pending.append((name, args, line))
        except ManifestError as e:
            raise ManifestError(f"{at}: {e}") from None
    for name, args, line in pending:
        at = _where(path, line)
        try:
            sid = _ident(args[0], "id")
            if sid not in m.sigs:
                raise ManifestError(f"{name} 引用了不存在的特征码 {sid}")
            sig = m.sigs[sid]
            if name == "DD_ENTRY":
                sig.entry = _string(args[1], "bytes")
            elif name == "DD_CHAIN_TARGET":
                sig.chain = True
            elif name == "DD_RUNTIME_SELECT":
                sig.runtime_select = True
            elif name == "DD_DEPS":
                sig.deps = split_deps(_string(args[1], "deps"))
                sig.has_deps = True
        except ManifestError as e:
            raise ManifestError(f"{at}: {e}") from None
    if not m.sigs:
        raise ManifestError(f"{path} 里没有 DD_SIG")


def _load_layout(m, path):
    text = path.read_text(encoding="utf-8")
    pending = []
    for name, args, line in iter_macros(text, LAYOUT_MACROS):
        at = _where(path, line)
        try:
            if name == "DD_LAYOUT":
                if len(args) != 6:
                    raise ManifestError("DD_LAYOUT 要 6 个参数")
                item = Item(_ident(args[0], "id"), _ident(args[1], "type", ITEM_TYPES), _int(args[2], "value"),
                            _ident(args[3], "need", ITEM_NEEDS), _ident(args[4], "check", ITEM_CHECKS),
                            split_deps(_string(args[5], "deps")), line)
                if item.id in m.items:
                    raise ManifestError(f"清单项 {item.id} 重复")
                if item.id in m.sigs:
                    raise ManifestError(f"清单项 {item.id} 和特征码同名")
                m.items[item.id] = item
            else:
                pending.append((name, args, line))
        except ManifestError as e:
            raise ManifestError(f"{at}: {e}") from None
    for name, args, line in pending:
        at = _where(path, line)
        try:
            iid = _ident(args[0], "id")
            if name == "DD_EVIDENCE":
                if len(args) != 12:
                    raise ManifestError("DD_EVIDENCE 要 12 个参数")
                ev = Evidence(iid, _ident(args[1], "sig"), _ident(args[2], "origin", ORIGINS), _int(args[3], "arg"),
                              _ident(args[4], "where", WHERES), _int(args[5], "span"), _string(args[6], "pattern"),
                              _int(args[7], "at"), _ident(args[8], "read", READS), _int(args[9], "mul"), _ident(args[10], "rel"),
                              _int(args[11], "add"), line)
                if iid not in m.items and iid not in m.sigs:
                    raise ManifestError(f"DD_EVIDENCE 的对象 {iid} 既不是清单项也不是特征码")
                owner = m.items[iid].evidence if iid in m.items else [e for e in m.evidence if e.item == iid]
                ev.index = len(owner)
                if iid in m.items:
                    m.items[iid].evidence.append(ev)
                m.evidence.append(ev)
                continue
            if name == "DD_GROUP":
                if len(args) < 3:
                    raise ManifestError("DD_GROUP 要组名、说明和至少一个成员")
                if iid in m.groups:
                    raise ManifestError(f"组 {iid} 重复")
                m.groups[iid] = Group(iid, _string(args[1], "说明"), [_ident(a, "成员") for a in args[2:]], line)
                continue
            if iid not in m.items:
                raise ManifestError(f"{name} 引用了不存在的清单项 {iid}")
            item = m.items[iid]
            if name == "DD_OFFLINE":
                item.offline.append(_ident(args[1], "check"))
            elif name == "DD_UNVERIFIED":
                item.unverified = _string(args[1], "说明")
            elif name == "DD_NAMES":
                item.names = [_string(a, "名字") for a in args[1:]]
            elif name == "DD_VALUES":
                item.values = [eval_value(a) for a in args[1:]]
        except ManifestError as e:
            raise ManifestError(f"{at}: {e}") from None


# ---- 检查清单本身 ----

def lint(m, require_deps=True):
    """返回问题列表（字符串）。require_deps：每条特征码都要有 DD_DEPS（主清单要求，插件的 --sigs 文件不要求）。"""
    problems = []

    def dep_ok(token):
        return token in DEPENDENTS or (token.startswith("topic:") and len(token) > 6)

    for sig in m.sigs.values():
        at = _where(sig.file, sig.line)
        try:
            compile_check(sig.pattern)
        except ValueError as e:
            problems.append(f"{at} {sig.id}：{e}")
        if require_deps and not sig.has_deps:
            problems.append(f"{at} {sig.id}：没有 DD_DEPS")
        for token in sig.deps:
            if not dep_ok(token):
                problems.append(f"{at} {sig.id}：依赖表里不认识的 {token}")
        if sig.need == "REQUIRED" and sig.has_deps and not ({"core", "loader"} & set(sig.deps)):
            problems.append(f"{at} {sig.id}：必需的特征码，依赖表里要有 core 或 loader")
        if sig.kind == "HOOK" and sig.steal < 5:
            # jmp rel32 要 5 字节：加载器编译期断言、DDCore 的 hook 引擎装的时候拒绝（审核 A 备注 4，verify 也查）
            problems.append(f"{at} {sig.id}：HOOK 的 steal 至少 5（放得下 jmp rel32）")
        if sig.kind in ("GLOBAL", "GLOBAL_ANY") and sig.capture + 4 > len(sig.pattern.split()):
            problems.append(f"{at} {sig.id}：capture 超出特征码长度")
        if sig.runtime_select and sig.kind != "GLOBAL_ANY":
            problems.append(f"{at} {sig.id}：DD_RUNTIME_SELECT 只用于 GLOBAL_ANY")
    for item in m.items.values():
        at = f"{Path(m.layout_file).name}:{item.line}" if m.layout_file else str(item.line)
        if not item.deps:
            problems.append(f"{at} {item.id}：依赖表为空")
        for token in item.deps:
            if not dep_ok(token):
                problems.append(f"{at} {item.id}：依赖表里不认识的 {token}")
        if item.need == "REQUIRED" and "core" not in item.deps:
            problems.append(f"{at} {item.id}：必需项的依赖表里要有 core")
        if item.check == "RUNTIME" and not item.evidence:
            problems.append(f"{at} {item.id}：标成 RUNTIME 但没有 DD_EVIDENCE")
        if item.check == "OFFLINE" and (item.evidence or not item.offline):
            problems.append(f"{at} {item.id}：标成 OFFLINE 时要有 DD_OFFLINE、不能有 DD_EVIDENCE")
        if item.check == "NONE" and (item.evidence or item.offline or not item.unverified):
            problems.append(f"{at} {item.id}：标成 NONE 时只能有 DD_UNVERIFIED（写明怎么核实）")
        if item.check != "NONE" and item.unverified:
            problems.append(f"{at} {item.id}：有依据的项不要写 DD_UNVERIFIED")
        if item.type == "TABLE":
            content = item.names if item.names is not None else item.values
            if content is None:
                problems.append(f"{at} {item.id}：TABLE 要有 DD_NAMES 或 DD_VALUES")
            elif len(content) != item.value:
                problems.append(f"{at} {item.id}：TABLE 的值 {item.value} 和表的项数 {len(content)} 不一致")
        elif item.names is not None or item.values is not None:
            problems.append(f"{at} {item.id}：只有 TABLE 能带 DD_NAMES / DD_VALUES")
    for group in m.groups.values():
        at = f"{Path(m.layout_file).name}:{group.line}" if m.layout_file else str(group.line)
        for member in group.members:
            if member not in m.items and member not in m.sigs:
                problems.append(f"{at} 组 {group.id}：成员 {member} 既不是清单项也不是特征码")
        if len(set(group.members)) != len(group.members):
            problems.append(f"{at} 组 {group.id}：成员重复")
        triggers = [x for x in group.members if x in m.sigs or (x in m.items and m.items[x].check == "RUNTIME")]
        flagged = [x for x in group.members if x in m.items and m.items[x].check != "RUNTIME"]
        if not triggers or not flagged:
            problems.append(f"{at} 组 {group.id}：要同时有运行时能核对的成员（RUNTIME 项或特征码）和没有运行时依据的成员"
                            "（NONE、OFFLINE 项），否则这个组不起作用")
    for ev in m.evidence:
        at = f"{Path(m.layout_file).name}:{ev.line}" if m.layout_file else str(ev.line)
        what = f"{at} {ev.item} 的依据 {ev.index}"
        try:
            length = compile_check(ev.pattern)
        except ValueError as e:
            problems.append(f"{what}：{e}")
            continue
        if ev.origin == "NOSIG":
            if ev.sig != "NONE" or ev.where != "TEXT":
                problems.append(f"{what}：NOSIG 只能配 sig=NONE、where=TEXT")
        else:
            if ev.sig not in m.sigs:
                problems.append(f"{what}：特征码 {ev.sig} 不存在")
            elif ev.origin in ("VSLOT", "VSLOT_VALUE") and m.sigs[ev.sig].kind != "GLOBAL":
                problems.append(f"{what}：虚表起点要求 GLOBAL 特征码")
            elif ev.origin in ("TARGET", "VSLOT", "VSLOT_VALUE") and m.sigs[ev.sig].runtime_select:
                # 按内容选的候选只有运行时才选得出来（文件里的表是空的），离线和运行时的起点会不同
                problems.append(f"{what}：DD_RUNTIME_SELECT 的特征码只能用 HIT 当起点")
            if ev.where == "TEXT":
                problems.append(f"{what}：where=TEXT 时 origin 要写 NOSIG")
        if ev.origin in ("VSLOT_VALUE", "HIT_VALUE") and ev.item not in m.items:
            problems.append(f"{what}：{ev.origin} 只能用于清单项")
        if ev.arg and ev.origin not in ("VSLOT", "HIT_VALUE"):
            problems.append(f"{what}：arg 只用于 VSLOT 和 HIT_VALUE，其余写 0")
        if ev.where == "WINDOW" and ev.span <= 0:
            problems.append(f"{what}：WINDOW 要给 span")
        if ev.rel != "NONE" and ev.rel not in m.items and ev.rel not in m.sigs:
            problems.append(f"{what}：rel {ev.rel} 不存在")
        width = {"NONE": 0, "U8": 1, "I8": 1, "U16": 2, "U32": 4, "I32": 4, "U64": 8, "DEREF_U32": 4, "REL32": 4}[ev.read]
        if ev.read != "NONE" and not 0 <= ev.at <= length - width:
            problems.append(f"{what}：读取位置 {ev.at} 超出特征码")
        if ev.read == "NONE" and (ev.mul or ev.add or ev.rel != "NONE"):
            problems.append(f"{what}：read=NONE 时 mul、rel、add 要写 0 / NONE / 0")
        if ev.read == "REL32" and ev.rel not in m.sigs:
            problems.append(f"{what}：REL32 读出的是 RVA，rel 要写特征码名字")
        if ev.read not in ("NONE", "REL32") and (ev.rel == "NONE" or ev.rel in m.items):
            expected = m.value(ev.item) * ev.mul + m.value(ev.rel) + ev.add
            bits = 8 * width
            low, high = (-(1 << (bits - 1)), 1 << (bits - 1)) if ev.read in ("I8", "I32") else (0, 1 << bits)
            if not low <= expected < high:
                problems.append(f"{what}：期望值 {expected:#x} 超出 {ev.read} 的范围（读取不截断，换更宽的读取或有符号读取）")
    return problems


def compile_check(pattern):
    tokens = pattern.split()
    if not tokens:
        raise ValueError("空特征码")
    for t in tokens:
        if t != "??" and not re.fullmatch(r"[0-9A-Fa-f]{2}", t):
            raise ValueError(f"特征码里有无效的字节 {t!r}")
    return len(tokens)

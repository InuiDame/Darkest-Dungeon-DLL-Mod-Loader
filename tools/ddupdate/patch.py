"""清单文件（signatures.inc、layout.inc）的文本改写和 unified diff。只改宏的参数，注释和排版不动。

宏按 manifest 解析器记下的行号定位（Sig.line、Item.line、Evidence.line），从那一行里找宏名，用解析器的参数切分得到
宏的结束位置，整段换成新的宏调用（参数之间用「, 」）。DD_ENTRY、DD_KNOWN_BUILD 没有记行号，按 id 查找。
"""
import difflib
import re
from pathlib import Path

from .manifest import ManifestError, _parse_args


def _line_offset(text, line):
    pos = 0
    for _ in range(line - 1):
        pos = text.index("\n", pos) + 1
    return pos


def _find_macro(text, name, line=None, first_arg=None):
    """返回 (宏名起点, 参数列表, 结束位置)。"""
    if line is not None:
        start = _line_offset(text, line)
        m = re.compile(rf"\b{name}\s*\(").search(text, start)
        if not m or text.count("\n", start, m.start()):
            raise ManifestError(f"第 {line} 行没有 {name}")
    else:
        pattern = re.compile(rf"^\s*{name}\s*\(\s*{re.escape(first_arg)}\s*,", re.M)
        m = pattern.search(text)
        if not m:
            return None
        m = re.compile(rf"\b{name}\s*\(").search(text, m.start())
    args, end = _parse_args(text, m.end())
    return m.start(), args, end


def _replace(text, name, start, end, args):
    return text[:start] + f"{name}(" + ", ".join(args) + ")" + text[end:]


def fmt_int(value, like):
    """按原参数的写法（十六进制 / 十进制）写整数。"""
    like = like.strip()
    if re.fullmatch(r"-?0[xX][0-9A-Fa-f]+", like):
        digits = len(like.lstrip("-")) - 2
        body = f"{abs(value):0{digits}X}" if digits >= 2 and len(f"{abs(value):X}") <= digits else f"{abs(value):X}"
        return ("-" if value < 0 else "") + "0x" + body
    return str(value)


def set_sig(text, sig, pattern, steal, capture, entry):
    start, args, end = _find_macro(text, "DD_SIG", sig.line)
    args = list(args)
    args[3], args[4], args[5] = str(steal), str(capture), f'"{pattern}"'
    text = _replace(text, "DD_SIG", start, end, args)
    if entry is not None:
        found = _find_macro(text, "DD_ENTRY", first_arg=sig.id)
        if found:
            s, a, e = found
            text = _replace(text, "DD_ENTRY", s, e, [a[0], f'"{entry}"'])
    return text


def set_item_value(text, item, value):
    start, args, end = _find_macro(text, "DD_LAYOUT", item.line)
    args = list(args)
    args[2] = fmt_int(value, args[2])
    return _replace(text, "DD_LAYOUT", start, end, args)


def set_evidence(text, ev, pattern, at, read):
    start, args, end = _find_macro(text, "DD_EVIDENCE", ev.line)
    args = list(args)
    if args[0].strip() != ev.item:
        raise ManifestError(f"第 {ev.line} 行的 DD_EVIDENCE 不是 {ev.item} 的")
    args[6], args[7], args[8] = f'"{pattern}"', str(at), read
    return _replace(text, "DD_EVIDENCE", start, end, args)


def add_known_build(text, sha1, label):
    line = f'DD_KNOWN_BUILD("{sha1}", "{label}")'
    matches = list(re.finditer(r"^DD_KNOWN_BUILD\(.*\)[ \t]*$", text, re.M))
    if matches:
        pos = matches[-1].end()
        return text[:pos] + "\n" + line + text[pos:]
    return text.rstrip("\n") + "\n" + line + "\n"


def diff(old, new, path):
    return "".join(difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                                        fromfile=f"a/{path}", tofile=f"b/{path}"))


def write_keep_newlines(path, text):
    """把改过的清单写回文件，换行保持原文件的写法（CRLF 或 LF；Windows 上 write_text 会把 LF 全改成 CRLF）。
    text 是 read_text 读进来再改的，里面只有 LF。"""
    path = Path(path)
    newline = "\r\n" if path.is_file() and b"\r\n" in path.read_bytes() else "\n"
    with open(path, "w", encoding="utf-8", newline=newline) as f:
        f.write(text.replace("\r\n", "\n"))

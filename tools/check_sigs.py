"""离线校验 core/src/game/signatures.inc：不启动游戏，对 exe 扫描全部特征码。

要求：每条特征码唯一命中（GLOBAL_ANY 见 signatures.inc 注释，多处命中必须解析到同一地址，
DD_RUNTIME_SELECT 标记的允许多个候选，运行时按内容选）；HOOK 的偷取字节落在指令边界，不含
rip 相对和分支指令，函数内没有分支跳进偷取范围；DD_ENTRY 给出的入口字节要和干净 exe 的
实际字节一致；DDCore 的内联 hook 不和 knowledge/dd1/hooks.md 清单重叠（DD_CHAIN_TARGET
标记的目标是有意串联，单独列出，不算失败）。

用法（guest，工作目录 D:\\workplace）：
    python -B DD.ModLoader/tools/check_sigs.py [exe ...]
不带参数时检查 Steam 版和离线版。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "knowledge" / "tools"))

from capstone import CS_ARCH_X86, CS_MODE_64, Cs  # noqa: E402
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP  # noqa: E402
from dd_pe import Image  # noqa: E402

SIGS_FILE = ROOT / "DD.ModLoader" / "core" / "src" / "game" / "signatures.inc"
HOOKS_MD = ROOT / "knowledge" / "dd1" / "hooks.md"

DEFAULT_PATCH_LEN = 16  # hooks.md 里没写字节数的内联 hook，按保守长度算

EXES = [
    ("steam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe"),
    ("nosteam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windowsnosteam\win64\Darkest.exe"),
]


def load_sigs():
    text = SIGS_FILE.read_text(encoding="utf-8")
    sigs = []
    for match in re.finditer(r'^DD_SIG\(\s*(\w+),\s*(\w+),\s*(\w+),\s*(\d+),\s*(-?\d+),\s*"([^"]+)"\s*\)', text, re.M):
        sigs.append({
            "id": match.group(1),
            "kind": match.group(2),
            "required": match.group(3) != "OPT",  # REQUIRED 和 LOADER 都必须唯一命中
            "steal": int(match.group(4)),
            "capture": int(match.group(5)),
            "pattern": match.group(6),
        })
    assert sigs, "no DD_SIG entries found"
    return sigs


def load_markers():
    text = SIGS_FILE.read_text(encoding="utf-8")
    entries = dict(re.findall(r'^DD_ENTRY\(\s*(\w+),\s*"([^"]+)"\s*\)', text, re.M))
    chain = set(re.findall(r'^DD_CHAIN_TARGET\(\s*(\w+)\s*\)', text, re.M))
    runtime_select = set(re.findall(r'^DD_RUNTIME_SELECT\(\s*(\w+)\s*\)', text, re.M))
    return entries, chain, runtime_select


def load_known_builds():
    text = SIGS_FILE.read_text(encoding="utf-8")
    return dict(re.findall(r'DD_KNOWN_BUILD\("([0-9a-f]{40})",\s*"(\w+)"\)', text))


def pattern_regex(pattern):
    parts = []
    for token in pattern.split():
        parts.append(b"." if token == "??" else re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.S)


def branch_targets(image, engine, begin):
    """包含 begin 的主函数所有分块里的分支目标。"""
    primary = image.enclosing_function(begin) or begin
    chunks = [b for b, p in image.func_primary.items() if p == primary]
    targets = set()
    for chunk in chunks:
        code = bytes(image.mem[chunk:image.func_ends[chunk]])
        for ins in engine.disasm(code, chunk):
            if ins.mnemonic == "jmp" or ins.mnemonic.startswith("j") or ins.mnemonic == "call":
                for op in ins.operands:
                    if op.type == X86_OP_IMM:
                        targets.add(op.imm)
    return targets, len(chunks)


def foreign_patches():
    """hooks.md 里 DDCore/加载器之外的区间（rva, 长度, 所属），只认带反引号的 RVA 表格行。
    DDCore 和验证用 hook 段落排除，这两段里是我们自己的目标。"""
    text = HOOKS_MD.read_text(encoding="utf-8")
    # 去掉「DDCore 与加载器」和「验证用 hook」两节
    sections = re.split(r"^## ", text, flags=re.M)
    kept = [sections[0]]
    for section in sections[1:]:
        head = section.split("\n", 1)[0]
        if head.startswith("DDCore") or head.startswith("验证用"):
            continue
        kept.append("## " + section)
    body = "".join(kept)

    spans = []
    for row in re.findall(r"^\|.*$", body, re.M):
        cells = [c.strip() for c in row.strip("|").split("|")]
        rvas = re.findall(r"`([0-9A-F]{5,7})`", cells[0]) if cells else []
        if not rvas:
            continue
        length = DEFAULT_PATCH_LEN
        owner = cells[-1] if cells else ""
        for cell in cells[1:]:
            if re.fullmatch(r"\d+", cell):
                length = int(cell)
                break
        spans += [(int(rva, 16), length, owner) for rva in rvas]
    return spans


def check_exe(tag, path, sigs, known, foreign, engine, entries, chain, runtime_select):
    image = Image(path)
    print(f"== {tag} {path}")
    label = known.get(image.sha1)
    print(f"   sha1={image.sha1} {'已知版本 ' + label if label else '未知版本（仅警告）'}")
    start, end = image.sections[".text"]
    data = bytes(image.mem[start:end])
    failures = 0

    for sig in sigs:
        name = sig["id"]
        hits = [m.start() + start for m in pattern_regex(sig["pattern"]).finditer(data)]
        severity = "FAIL" if sig["required"] else "WARN"

        if sig["kind"] == "GLOBAL_ANY":
            if not hits:
                print(f"  {severity} {name:<22} 0 次命中")
                failures += 1 if sig["required"] else 0
                continue
            resolved = sorted({h + sig["capture"] + 4 + image.i32(h + sig["capture"]) for h in hits})
            bad = [r for r in resolved if not 0 <= r < image.size]
            if bad:
                print(f"  {severity} {name:<22} 解析出的地址超出映像 {[hex(b) for b in bad[:4]]}")
                failures += 1 if sig["required"] else 0
                continue
            if len(resolved) > 1 and name not in runtime_select:
                print(f"  FAIL {name:<22} {len(hits)} 次命中解析出 {len(resolved)} 个不同地址（需要 DD_RUNTIME_SELECT 或收紧特征码）")
                failures += 1
                continue
            tag_text = "（候选多个，运行时按内容选）" if len(resolved) > 1 else ""
            print(f"  OK   {name:<22} {len(hits)} 次命中，解析出 {len(resolved)} 个地址 {[hex(r) for r in resolved[:6]]}{tag_text}")
            continue

        if len(hits) != 1:
            print(f"  {severity} {name:<22} {len(hits)} 次命中 {[hex(h) for h in hits[:4]]}")
            failures += 1 if sig["required"] else 0
            continue
        rva = hits[0]
        notes = [f"rva={rva:X}"]

        if sig["kind"] == "GLOBAL":
            disp_off = rva + sig["capture"]
            global_rva = disp_off + 4 + image.i32(disp_off)
            if not 0 <= global_rva < image.size:
                print(f"  {severity} {name:<22} 全局地址 {global_rva:X} 超出映像")
                failures += 1 if sig["required"] else 0
                continue
            section = image.section_of(global_rva)
            notes.append(f"全局 {global_rva:X}（{section}）")
            if sig["required"] and section not in (".data", ".rdata"):
                print(f"  FAIL {name:<22} 解析出的全局不在 .data/.rdata：{' '.join(notes)}")
                failures += 1
                continue
        elif sig["kind"] == "HOOK":
            steal = sig["steal"]
            pos = 0
            bad = False
            for ins in engine.disasm(bytes(image.mem[rva:rva + 64]), rva):
                if pos >= steal:
                    break
                uses_rip = any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands)
                if uses_rip or ins.mnemonic in ("call", "jmp", "ret") or ins.mnemonic.startswith("j"):
                    bad = True
                    notes.append(f"不安全指令 {ins.mnemonic} {ins.op_str}")
                pos += ins.size
            if pos != steal:
                bad = True
                notes.append(f"偷取 {steal} 字节没有落在指令边界（下一边界 {pos}）")
            targets, chunks = branch_targets(image, engine, rva)
            inside = sorted(t for t in targets if rva < t < rva + steal)
            if inside:
                bad = True
                notes.append(f"函数内有分支跳进偷取范围 {[hex(t) for t in inside]}")
            notes.append(f"steal={steal} chunks={chunks}")

            if label == "steam":
                # hooks.md 的 RVA 是 Steam 版的，离线版不检查。
                # 偷取范围不能和其他插件的内联 hook 重叠（DDCore 比它们先运行，会让它们的特征码匹配失败）
                overlap = [
                    f"{start_:X}+{length}（{owner}）"
                    for start_, length, owner in foreign
                    if start_ < rva + steal and rva < start_ + length
                ]
                if overlap and name in chain:
                    notes.append(f"有意串联（DD_CHAIN_TARGET），重叠 {overlap}")
                elif overlap:
                    bad = True
                    notes.append(f"和 hooks.md 清单重叠 {overlap}")
            expected_entry = entries.get(name)
            if expected_entry is not None:
                want = bytes(int(token, 16) for token in expected_entry.split())
                got = bytes(image.mem[rva:rva + len(want)])
                if want != got:
                    bad = True
                    notes.append(f"入口字节 {got.hex()} 和 DD_ENTRY {want.hex()} 不符")
                if len(want) != steal:
                    bad = True
                    notes.append(f"DD_ENTRY 长度 {len(want)} 不等于 steal {steal}")
            if bad:
                print(f"  FAIL {name:<22} {' '.join(notes)}")
                failures += 1
                continue

        print(f"  OK   {name:<22} {' '.join(notes)}")

    return failures


def main():
    sigs = load_sigs()
    known = load_known_builds()
    foreign = foreign_patches()
    entries, chain, runtime_select = load_markers()
    print(f"signatures.inc：{len(sigs)} 条特征码；hooks.md 中其他插件的 hook 区间：{len(foreign)}；"
        f"有意串联 {sorted(chain)}；运行时选择 {sorted(runtime_select)}")

    engine = Cs(CS_ARCH_X86, CS_MODE_64)
    engine.detail = True

    paths = [("arg", p) for p in sys.argv[1:]] or EXES
    failures = 0
    for tag, path in paths:
        failures += check_exe(tag, path, sigs, known, foreign, engine, entries, chain, runtime_select)

    print("结果:", "FAIL" if failures else "OK", f"（{failures} 个失败）")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())

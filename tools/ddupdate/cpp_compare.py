"""C++/Python 对照测试（U1b）：同一批 exe 上，DDCore 的运行时核对（core/src/game/layout_check.cpp，经 tests/layout_compare 的
dd_layout_compare.exe 按节映射、不执行）和 dd_update.py verify --json 逐项比较，结论必须一致。在 guest 上运行：

    python -B DD.ModLoader/tools/ddupdate/cpp_compare.py [exe ...] [--bin dd_layout_compare.exe] [--out 目录] [--repeat N]
        [--tree 源码副本]

不带 exe 时比较 Steam 版、离线版、U1a 的手工破坏样本（build/u1a/mut/）、build/u1b/mut/ 下的全部样本（由
tests/layout_compare/make_mutants.py 生成）和 build/u1d/mut/ 下 selftest 的变异样本（ddupdate/mutants.py 生成）。
先 xmake build dd_layout_compare。dd_update.py selftest 把这一步并了进去。

比较的内容：
  特征码  命中次数、命中位置、解析结果、候选、状态（ok / fail）
  依据    每条 DD_EVIDENCE（按 清单项 + 同一项里的序号 对齐，含特征码的同一性依据）的 status、hit、captured，
          另外比较 origin、expected、命中次数
「Python 独有的检查」单列，不算不一致：
  - 特征码的静态检查（sigs.py 的类别 static：HOOK 偷取范围的指令、偷取不在指令边界、函数内跳进偷取范围的分支、hooks.md
    的重叠、偷取不到 5 字节，以及 DD_ENTRY 和入口字节的比较，运行时入口可能已被旧式 DLL 改过）。某条特征码失效的原因
    **全部**属于这一类时（两边的命中和解析结果一致），才再用 --sig-fail 让 C++ 把它当作失效重新求值，保证两边依据求值的
    输入相同；它的解析结果仍然逐项比较。失效原因里只要有一条是运行时也做的检查（类别 resolve、section、entry：命中
    次数、必需 GLOBAL 指向 .data/.rdata、HOOK 命中位置是函数入口），C++ 却判成立，就算不一致（审核 A 的 S5：原来一律
    归为 Python 独有，C++ 把这些检查弄丢了也查不出来）。HOOK 的函数入口检查 U1 修复 FA 起两边都做，不再单列。
  - 依据位置核对（verify 的 positions，规则见 position.py）：依据求值的结论两边照常逐项比较、必须一致；Python 另外
    发现「唯一命中、值也对，命中的却不是基线里那条指令」时判为依据误绑定、位置没法对照时判为无法确认，这一层运行时不做。

self_check()（selftest 的 compare 步骤先跑它）：用构造的两份结论检查上面的分类本身，C++ 少了运行时也做的检查时必须报
不一致。真正的「故意改坏 C++」实验（去掉 .data/.rdata 检查、配 statelast_text.exe）见 docs/u1/fb-report.md。

退出码：0 全部一致；1 有不一致；3 参数错、工具或 exe 不存在。每个 exe 的两份 JSON 和 compare.txt 写在 --out（默认
build/u1b/compare/）。
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
PROJECT = TOOLS.parent
# 只有 verify 做的特征码检查的类别（和 sigs.STATIC_KINDS 相同；这里写死，脚本单独运行时不依赖包导入）
STATIC_KINDS = frozenset(("static",))
DEFAULT_BIN = PROJECT / "build" / "windows" / "x64" / "releasedbg" / "dd_layout_compare.exe"
DEFAULT_OUT = PROJECT / "build" / "u1b" / "compare"
REAL_EXES = [
    ("steam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe"),
    ("nosteam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windowsnosteam\win64\Darkest.exe"),
]
SAMPLE_DIRS = [PROJECT / "build" / "u1a" / "mut", PROJECT / "build" / "u1b" / "mut", PROJECT / "build" / "u1d" / "mut"]


def default_exes():
    exes = list(REAL_EXES)
    for folder in SAMPLE_DIRS:
        for path in sorted(folder.glob("*.exe")):
            exes.append((f"{folder.parent.name}/{path.stem}", str(path)))
    return exes


def run_verify(exe, json_path, tree=None):
    cmd = [sys.executable, "-B", str(TOOLS / "dd_update.py"), "verify", exe, "--json", str(json_path)]
    if tree:
        cmd += ["--tree", str(tree)]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode not in (0, 1, 2) or not json_path.is_file():
        raise RuntimeError(f"verify 失败（退出码 {proc.returncode}）：{proc.stdout[-2000:]}{proc.stderr[-2000:]}")
    data = json.loads(json_path.read_text(encoding="utf-8"))
    return data["exes"][0], proc.returncode


def run_cpp(binary, exe, json_path, sig_fail, repeat):
    cmd = [str(binary), exe, "--json", str(json_path), "--repeat", str(repeat)]
    if sig_fail:
        cmd += ["--sig-fail", ",".join(sig_fail)]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not json_path.is_file():
        raise RuntimeError(f"dd_layout_compare 失败（退出码 {proc.returncode}）：{proc.stderr[-2000:]}")
    return json.loads(json_path.read_text(encoding="utf-8")), proc.stderr.strip()


def hexs(v):
    if v is None:
        return "-"
    if isinstance(v, list):
        return "[" + ",".join(hexs(x) for x in v) + "]"
    return f"{v:X}" if isinstance(v, int) and v >= 0 else str(v)


def py_evidence(report):
    """(清单项或特征码, 序号) -> verify JSON 里的依据记录。"""
    out = {}
    for iid, rec in report.get("layout", {}).items():
        for ev in rec["evidence"]:
            out[(iid, ev["index"])] = ev
    for sid, rec in report.get("identity", {}).items():
        for ev in rec["evidence"]:
            out[(sid, ev["index"])] = ev
    return out


def classify_sigs(py_sigs, cpp_sigs):
    """特征码逐项比较。返回 (只因 Python 独有的静态检查失效的特征码, 不一致的说明)。规则见文件开头。"""
    static_only, mismatches = [], []
    for sid, p in py_sigs.items():
        c = cpp_sigs.get(sid)
        if c is None:
            mismatches.append(f"特征码 {sid}：C++ 里没有")
            continue
        fields = (("hits", p["hits"], c["hits"]), ("hit", p["hit"], c["hit"]), ("resolved", p["resolved"], c["resolved"]),
                  ("candidates", p["candidates"], c["candidates"]))
        diff = [f"{k} Python {hexs(a)} / C++ {hexs(b)}" for k, a, b in fields if a != b]
        if diff:
            mismatches.append(f"特征码 {sid}：" + "；".join(diff))
            continue
        if p["status"] == c["status"]:
            continue
        kinds = p.get("kinds")
        if kinds is None:
            mismatches.append(f"特征码 {sid}：verify 的 JSON 没有问题类别（kinds），分不清是不是 Python 独有的检查")
            continue
        if p["status"] == "fail" and c["status"] == "ok":
            runtime = [f"{text}（{kind}）" for text, kind in zip(p["problems"], kinds) if kind not in STATIC_KINDS]
            if runtime:
                mismatches.append(f"特征码 {sid}：Python 失效、C++ 成立，但失效原因里有运行时也做的检查：{'；'.join(runtime)}")
            else:
                static_only.append(sid)
        else:
            mismatches.append(f"特征码 {sid}：状态 Python {p['status']} / C++ {c['status']}（{'；'.join(p['problems'])} | {c['problem']}）")
    return static_only, mismatches


def self_check():
    """分类规则的自检（不需要 dd_layout_compare）：返回问题列表，空表示通过。"""
    base = {"hits": 1, "hit": 0x100, "resolved": 0x200, "candidates": []}
    cases = [
        # (说明, Python 的记录, C++ 的记录, 预期：static / mismatch / same)
        ("必需 GLOBAL 指向 .text，C++ 少了这项检查",
         {**base, "status": "fail", "problems": ["解析出的全局 200 不在 .data/.rdata（.text）"], "kinds": ["section"]},
         {**base, "status": "ok", "problem": ""}, "mismatch"),
        ("HOOK 不在函数入口，C++ 少了这项检查",
         {**base, "status": "fail", "problems": ["命中位置不是函数入口"], "kinds": ["entry"]},
         {**base, "status": "ok", "problem": ""}, "mismatch"),
        ("偷取范围有分支，只有 Python 查",
         {**base, "status": "fail", "problems": ["偷取范围里有不安全指令 jmp"], "kinds": ["static"]},
         {**base, "status": "ok", "problem": ""}, "static"),
        ("静态检查和运行时检查一起不过，C++ 成立",
         {**base, "status": "fail", "problems": ["DD_ENTRY 不符", "命中位置不是函数入口"], "kinds": ["static", "entry"]},
         {**base, "status": "ok", "problem": ""}, "mismatch"),
        ("两边都判函数入口失效",
         {**base, "status": "fail", "problems": ["命中位置不是函数入口"], "kinds": ["entry"]},
         {**base, "status": "fail", "problem": "命中位置不是函数入口"}, "same"),
        ("verify 的 JSON 没有类别",
         {**base, "status": "fail", "problems": ["x"]},
         {**base, "status": "ok", "problem": ""}, "mismatch"),
    ]
    problems = []
    for text, p, c, want in cases:
        static_only, mismatches = classify_sigs({"S": p}, {"S": c})
        got = "static" if static_only else ("mismatch" if mismatches else "same")
        if got != want:
            problems.append(f"{text}：分类成 {got}，预期 {want}")
    return problems


def compare_one(tag, exe, binary, out_dir, repeat, w, tree=None):
    name = tag.replace("/", "_")
    py, code = run_verify(exe, out_dir / f"{name}.verify.json", tree)
    cpp, summary = run_cpp(binary, exe, out_dir / f"{name}.cpp.json", [], repeat)
    w(f"== {tag}  {exe}\n   sha1={py['sha1']} {'已知版本 ' + py['known'] if py['known'] else '未知版本'}；verify 退出码 {code}\n")
    mismatches = []

    # 特征码：解析结果逐项比较；Python 只因静态检查失效的单列
    static_only, sig_mismatches = classify_sigs(py["sigs"], cpp["sigs"])
    mismatches += sig_mismatches
    position_misbind = [(k, r) for k, r in py.get("positions", {}).items() if r["status"] == "disagree"]
    position_unconfirmed = [(k, r) for k, r in py.get("positions", {}).items() if r.get("counted")]
    if static_only or position_misbind or position_unconfirmed:
        w("   Python 独有的检查（运行时不做，不算不一致）：\n")
    if static_only:
        w(f"     特征码静态检查判失效（两边的命中和解析结果一致）：{', '.join(static_only)}\n")
        for sid in static_only:
            w(f"       {sid}：{'；'.join(py['sigs'][sid]['problems'])}\n")
        cpp, summary = run_cpp(binary, exe, out_dir / f"{name}.cpp.json", static_only, repeat)
    if position_misbind:
        w(f"     依据位置核对判误绑定（依据求值的结论两边照常比较）：{', '.join(k for k, _r in position_misbind)}\n")
        for key, r in position_misbind:
            w(f"       {key}：{r['detail']}\n")
    if position_unconfirmed:
        w(f"     依据位置无法确认（依据求值的结论两边照常比较）：{', '.join(k for k, _r in position_unconfirmed)}\n")
    w(f"   C++：{summary}\n")

    # 依据
    pe = py_evidence(py)
    ce = {(e["item"], e["index"]): e for e in cpp["evidence"]}
    if set(pe) != set(ce):
        only_py = sorted(set(pe) - set(ce))
        only_cpp = sorted(set(ce) - set(pe))
        mismatches.append(f"依据集合不同：只在 Python {only_py[:6]}，只在 C++ {only_cpp[:6]}")
    counts = {}
    for key in sorted(set(pe) & set(ce)):
        p, c = pe[key], ce[key]
        counts[p["status"]] = counts.get(p["status"], 0) + 1
        fields = (("status", p["status"], c["status"]), ("hit", p["hit"], c["hit"]), ("captured", p["captured"], c["captured"]),
                  ("origin", p["origin"], c["origin"]), ("expected", p["expected"], c["expected"]), ("hits", p["hits"], c["hits"]))
        diff = [f"{k} Python {hexs(a)} / C++ {hexs(b)}" for k, a, b in fields if a != b]
        if diff:
            mismatches.append(f"依据 {key[0]}[{key[1]}]（{p['describe']}）：" + "；".join(diff))
    w(f"   依据 {len(set(pe) & set(ce))} 条逐项比较：" + "、".join(f"{k} {v}" for k, v in sorted(counts.items())) + "\n")
    not_ok = [(k, pe[k]) for k in sorted(pe) if pe[k]["status"] != "ok" and k in ce]
    for (item, index), p in not_ok[:40]:
        c = ce[(item, index)]
        w(f"     {p['status']:<7} {item}[{index}] Python「{p['detail']}」 C++「{c['detail']}」\n")
    if len(not_ok) > 40:
        w(f"     ……另有 {len(not_ok) - 40} 条\n")

    # C++ 的清单项结论和停用开关（只报告，不比较：Python 没有运行时策略）
    states = {}
    for state in cpp["items"].values():
        states[state] = states.get(state, 0) + 1
    w("   C++ 清单项：" + "、".join(f"{k} {v}" for k, v in sorted(states.items())) + "\n")
    for mode in ("known", "unknown"):
        gate = cpp["gate"][mode]
        topics = [t["pattern"] + ("（可写的）" if t["write_only"] else "") for t in gate["topics"]]
        w(f"   停用开关（按{'已知' if mode == 'known' else '未知'}版本）：能力 {list(gate['off']) or '无'}；主题 {len(topics)} 条"
          + (f" {topics[:12]}{'…' if len(topics) > 12 else ''}" if topics else "") + "\n")

    if mismatches:
        w(f"   不一致 {len(mismatches)} 处：\n" + "".join(f"     {m}\n" for m in mismatches))
    else:
        w("   结论：一致\n")
    return not mismatches, cpp


def main(argv=None):
    parser = argparse.ArgumentParser(prog="cpp_compare.py", description="DDCore 运行时核对和 dd_update.py verify 的逐项对照")
    parser.add_argument("exe", nargs="*", help="要比较的 exe，默认 Steam 版、离线版和 build/u1a、build/u1b 下的样本")
    parser.add_argument("--bin", default=str(DEFAULT_BIN), help="dd_layout_compare.exe 的位置")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="两份 JSON 和 compare.txt 的目录")
    parser.add_argument("--repeat", type=int, default=1, help="C++ 每条依据重复求值的次数（耗时取平均）")
    parser.add_argument("--tree", help="源码副本：verify 用副本里的清单和基线（--bin 要用同一个副本编出来的 dd_layout_compare）")
    parser.add_argument("--self-check", action="store_true", help="只跑分类规则的自检（不需要 dd_layout_compare）")
    args = parser.parse_args(argv)
    problems = self_check()
    if args.self_check:
        print("分类规则自检：" + ("通过" if not problems else "不通过：" + "；".join(problems)))
        return 0 if not problems else 1
    if problems:
        print("分类规则自检不通过：" + "；".join(problems))
        return 1
    binary = Path(args.bin)
    if not binary.is_file():
        print(f"没有 {binary}，先 xmake build dd_layout_compare")
        return 3
    exes = [(Path(p).stem, p) for p in args.exe] or default_exes()
    for _tag, path in exes:
        if not Path(path).is_file():
            print(f"exe 不存在：{path}")
            return 3
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    lines = []

    def w(text):
        sys.stdout.write(text)
        sys.stdout.flush()
        lines.append(text)

    results = []
    for tag, path in exes:
        try:
            ok, _cpp = compare_one(tag, path, binary, out_dir, args.repeat, w, args.tree)
        except RuntimeError as e:
            w(f"== {tag}：{e}\n")
            return 3
        results.append((tag, ok))
    w("\n汇总：" + "，".join(f"{tag} {'一致' if ok else '不一致'}" for tag, ok in results) + "\n")
    code = 0 if all(ok for _t, ok in results) else 1
    w(f"结果：{'全部一致' if code == 0 else '有不一致'}，退出码 {code}\n")
    (out_dir / "compare.txt").write_text("".join(lines), encoding="utf-8")
    return code


if __name__ == "__main__":
    sys.exit(main())

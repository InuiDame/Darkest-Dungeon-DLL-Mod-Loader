"""游戏更新的校验与修复（U1）。在 guest 上运行，工作目录 D:\\workplace：

    python -B DD.ModLoader/tools/dd_update.py verify [exe ...] [--json 报告.json] [--only sigs,identity,layout]
        [--sigs 特征码文件 [--layout 布局清单]] [--baseline 基线.json ...] [--tree 源码目录]
    python -B DD.ModLoader/tools/dd_update.py baseline [exe ...] [--out 目录] [--sigs 特征码文件 [--layout 布局清单]]
    python -B DD.ModLoader/tools/dd_update.py simstats [--a exe] [--b exe] [--pairs N] [--seed S] [--json 输出.json]
    python -B DD.ModLoader/tools/dd_update.py repair --exe 新exe [--baseline 基线.json] [--apply] [--tree 源码目录]
        [--sigs 特征码文件 [--layout 布局清单]] [--out 目录] [--min-confidence high|medium] [--old-exe 旧exe]
    python -B DD.ModLoader/tools/dd_update.py accept --exe 新exe [--label 版本名] [--replace] [--tree 源码目录]
    python -B DD.ModLoader/tools/dd_update.py blind [--baseline 基线.json --exe exe] [--json 输出.json]
    python -B DD.ModLoader/tools/dd_update.py selftest [--only blind,mutants,mcheck,roundtrip,compare] [--samples 名字,...]

verify     逐项判定特征码（原 check_sigs.py 的全部检查）、同一性（和基线的指纹、调用关系比较，防误绑定）、
           布局（layout.inc 每条依据求值，原 check_actor_layout.py 的全部检查）；报出失效项影响的服务和主题。
           不带 exe 时检查 Steam 版和离线版。退出码：0 全部通过，1 只有可选项失效，2 必需项失效或有误绑定，3 清单或参数有错。
           --sigs 换成别的特征码文件（同格式，插件作者校验自己的特征码），这时默认不读布局清单和基线。
baseline   生成基线快照（默认写 tools/baseline/<版本名>.json，Steam 版和离线版），同一个 exe 两次生成字节一致。
simstats   相似度度量的数据：同一函数跨版本、最近冒充者、随机函数对的分布。
suggest    给清单项生成运行时依据的候选（写 layout.inc 时用），见 ddupdate/suggest.py。
repair     对 verify 失败的项在新 exe 里定位，重新生成特征码、偏移和依据模式，输出逐项报告、清单补丁、旧→新 RVA
           对照表；--apply 写进清单（--tree 时写进副本）再 verify。不改 DD_KNOWN_BUILD。见 ddupdate/repair.py。
accept     verify 通过后把 exe 加进 DD_KNOWN_BUILD，生成这个版本的基线。
blind      跨版本盲定位自测：以一个版本的基线、不用现有特征码，在另一个版本里定位全部特征码项并和现有结果比较，
           错误必须为 0。
selftest   U1 的全部自测（盲定位、变异样本、清单变异、往返、C++/Python 对照），全部通过时退出码 0，见 ddupdate/selftest.py。

清单格式见 core/src/game/signatures.inc 和 layout.inc 的开头。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ddupdate import baseline as baseline_mod  # noqa: E402
from ddupdate import manifest as manifest_mod  # noqa: E402
from ddupdate import sigs as sigs_mod  # noqa: E402
from ddupdate import repair, selftest, simstats, suggest, verify  # noqa: E402
from ddupdate.image import Image  # noqa: E402


def cmd_baseline(args, out):
    try:
        manifest = manifest_mod.load(args.sigs, args.layout, use_layout=not args.sigs or bool(args.layout))
    except manifest_mod.ManifestError as e:
        out.write(f"清单解析失败：{e}\n")
        return verify.EXIT_ERROR
    problems = manifest_mod.lint(manifest, require_deps=not args.sigs)
    if problems:
        out.write(f"清单有 {len(problems)} 个问题：\n" + "".join(f"  {p}\n" for p in problems))
        return verify.EXIT_ERROR
    if args.sigs and not args.out:
        out.write("--sigs 时要用 --out 指定基线目录（tools/baseline 是主清单的）\n")
        return verify.EXIT_ERROR
    out_dir = Path(args.out) if args.out else baseline_mod.BASELINE_DIR
    exes = [(None, p) for p in args.exe] or verify.DEFAULT_EXES
    foreign = sigs_mod.foreign_patches()
    code = 0
    for _tag, path in exes:
        image = Image(path)
        label = manifest.known_builds.get(image.sha1)
        sig_results = sigs_mod.resolve_all(image, manifest, label, foreign)
        failed = [sid for sid, r in sig_results.items() if r.status != "ok"]
        data = baseline_mod.generate(image, manifest, sig_results, label)
        target = out_dir / f"{label or image.sha1[:12]}.json"
        baseline_mod.write(data, target)
        size = target.stat().st_size
        out.write(f"{path}\n  sha1={image.sha1} {'已知版本 ' + label if label else '未知版本'} -> {target}（{size} 字节）\n")
        if failed:
            out.write(f"  注意：{len(failed)} 条特征码没有解析成功，基线里记为失败：{', '.join(failed)}\n")
            code = 1
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(prog="dd_update.py", description="游戏更新的校验与修复（U1）")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("verify", help="逐项校验特征码、同一性和布局")
    p.add_argument("exe", nargs="*", help="要校验的 Darkest.exe，默认 Steam 版和离线版")
    p.add_argument("--json", help="机器可读的报告写到这里")
    p.add_argument("--only", help="只做这些部分：sigs、identity、layout，逗号分隔")
    p.add_argument("--sigs", help="特征码文件（同 signatures.inc 的格式），默认 core/src/game/signatures.inc")
    p.add_argument("--layout", help="布局清单（同 layout.inc 的格式），--sigs 时默认不读")
    p.add_argument("--baseline", action="append", help="基线文件，可重复；默认 tools/baseline/*.json（--sigs 时默认没有）")
    p.add_argument("--tree", help="源码目录（或副本）：清单和基线从 <tree>/core/src/game、<tree>/tools/baseline 读")

    p = sub.add_parser("baseline", help="生成基线快照")
    p.add_argument("exe", nargs="*", help="已知版本的 Darkest.exe，默认 Steam 版和离线版")
    p.add_argument("--out", help="输出目录，默认 tools/baseline")
    p.add_argument("--sigs", help="特征码文件")
    p.add_argument("--layout", help="布局清单")

    p = sub.add_parser("simstats", help="相似度度量的数据验证")
    p.add_argument("--a", help="版本 A（默认 Steam 版）")
    p.add_argument("--b", help="版本 B（默认离线版）")
    p.add_argument("--pairs", type=int, default=5000, help="随机函数对的数量")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--json", help="数据写到这里")

    suggest.add_parser(sub)
    repair.add_parsers(sub)
    selftest.add_parser(sub)

    args = parser.parse_args(argv)
    out = sys.stdout
    if args.command == "verify":
        return verify.run(args, out)
    if args.command == "baseline":
        return cmd_baseline(args, out)
    if args.command == "suggest":
        return suggest.run(args, out)
    if args.command == "repair":
        return repair.cmd_repair(args, out)
    if args.command == "accept":
        return repair.cmd_accept(args, out)
    if args.command == "blind":
        return repair.cmd_blind(args, out)
    if args.command == "selftest":
        return selftest.run(args, out)
    return simstats.run(args, out)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())

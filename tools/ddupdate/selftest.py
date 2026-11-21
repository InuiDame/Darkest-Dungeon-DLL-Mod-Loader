"""dd_update.py selftest（U1d）：一条命令跑完 U1 的自测，全部通过时退出码 0，否则 1。在 guest 上运行：

    python -B DD.ModLoader/tools/dd_update.py selftest [--only blind,mutants,mcheck,roundtrip,gate,compare] [--samples 名字,...]

  blind      跨版本盲定位（repair.blind_both）：Steam 版基线 → 离线版、离线版基线 → Steam 版，不用现有特征码，错误定位必须为 0
  mutants    变异样本（mutants.py）：生成到 build/u1d/mut/（只读 Steam 版，不运行生成的 exe），每个样本断言
               verify：失效项（名字, 类别）的集合和退出码都等于样本表里写的
               repair：逐项状态（ok / repaired / manual）和退出码都等于样本表里写的（没写的项必须是 ok 或 unverified）
             再查两条「不能给错结论」的不变式：
               修复项的新位置、新值等于样本的真值；verify 判通过、repair 判 ok 的项，现有的位置和值也要等于真值（漏报也算错）
               repair 预演 verify 里剩下的失效项都要是报了需要人工的项
  mcheck     清单变异冒烟（mcheck.py）：Steam 版基线 + 离线版 exe + 故意改坏的清单副本
  roundtrip  往返：源码复制到 build/u1d/roundtrip/tree，对一个变异样本 repair --apply --tree，再 verify 通过；用修复后的清单
             在副本里编译 DDCore（只编译不运行）；accept 往返（U1 修复 FB）：不带 --confirm 时拒绝、副本不变；
             --confirm-template 生成的清单要列出全部 NONE 项和同组升级的项（Instance 组的 InstStat 等）；逐项填好以后
             accept 通过，DD_KNOWN_BUILD、基线、确认记录（tools/baseline/confirm/）按预期更新，仓库里的清单和基线不变
  gate       （U1 修复 FB）规则自检：cpp_compare 的分类规则（self_check）；LOADER 的 GLOBAL 指向 .text 时失效（审核 A 备注 2）、
             HOOK 偷取不到 5 字节时清单检查和 verify 都报（备注 4）；GLOBAL_ANY 的命中函数换成别的模板族、候选数和基线不同时
             判误绑定（S6）；accept 的拦截（B2、B1）：enum.exe（离线依据失效）、
             inst44.exe（结构插入）带不带 --allow-optional 都拒绝，hook_entry.exe（只有运行时也会发现的失效）不带
             --allow-optional 拒绝、带了以后还要 --confirm；审核 A 的 B1 原始反例：清单副本里换回 U1 修复 FA 收紧之前的
             宽松依据（InstSubHash 是 "8B 42 ?? 89 41 44" 这一类），inst44.exe 上 verify 必须报无法确认（退出码 2，原来判 OK），
             repair 对这四项只能报需要人工或修对，accept 拒绝
  compare    C++/Python 对照（cpp_compare.py）：Steam、离线、build/u1a、u1b、u1d 的样本，退出码 0 才算过。需要已构建的
             dd_layout_compare（xmake build dd_layout_compare），没有时这一步失败，不当作通过

输出在 build/u1d/selftest/：summary.txt（就是屏幕上的汇总）、summary.json、每一步的详细输出。全部跑完约半小时
（repair 每个样本 1–2 分钟，对照测试每个 exe 约 20 秒）。
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path

from . import baseline as baseline_mod
from . import cpp_compare
from . import manifest as manifest_mod
from . import mcheck
from . import mutants as mutants_mod
from . import repair as repair_mod
from . import sigs as sigs_mod
from . import verify as verify_mod
from .image import Image
from .manifest import PROJECT

OUT_DIR = PROJECT / "build" / "u1d" / "selftest"
STEPS = ("blind", "mutants", "mcheck", "roundtrip", "gate", "compare")
ROUNDTRIP_SAMPLE = "struct_shift"
TREE_PARTS = ("common", "core", "examples", "loader", "lua", "sdk", "spikes", "templates", "tests", "tools", "xmake",
              "xmake.lua", ".gitattributes")


class Step:
    def __init__(self, name):
        self.name = name
        self.ok = True
        self.lines = []
        self.data = {}
        self.seconds = 0.0

    def fail(self, text):
        self.ok = False
        self.lines.append(f"不通过：{text}")

    def log(self, text):
        self.lines.append(text)
        print(f"  {text}", flush=True)


# ---- 1. 盲定位 ----

def step_blind(step, out_dir, args):
    results = repair_mod.blind_both(workers=args.workers)
    text = "".join(repair_mod.render_blind(r) for r in results)
    (out_dir / "blind.txt").write_text(text, encoding="utf-8")
    for r in results:
        t, f = r["total"], r["functions"]
        manual = [sid for sid, row in r["rows"].items() if row["verdict"] == "manual"]
        step.log(f"{Path(r['baseline']).stem} 基线 → {Path(r['exe']).parent.parent.name}：特征码 正确 {t['correct']}、需要人工 "
                 f"{t['manual']}、错误 {t['wrong']}；函数级 正确 {f['correct']}、错误 {f['wrong']}、需要人工 {f['manual']}"
                 + (f"；需要人工的项 {manual}" if manual else ""))
        step.data[Path(r["baseline"]).stem] = {"total": t, "functions": f, "manual": manual, "exit_code": r["exit_code"]}
        if t["wrong"] or f["wrong"] or r["exit_code"]:
            step.fail(f"{Path(r['baseline']).stem} 基线方向有错误定位")


# ---- 2. 变异样本 ----

def _status_class(status):
    return "repaired" if status.startswith("repaired") else status


def check_sample(step, sample, manifest, baselines, base_res, out_dir, args):
    rec = {"category": sample.category, "simulates": sample.simulates, "unreal": sample.unreal, "notes": sample.notes,
           "problems": []}
    problems = rec["problems"]
    foreign = sigs_mod.foreign_patches()
    report = verify_mod.verify_exe(sample.path, manifest, baselines, verify_mod.SECTIONS, foreign)
    failures = {(f["name"], f["severity"]) for f in report["failures"]}
    rec["verify"] = {"exit": report["exit_code"], "failures": sorted(failures),
                     "positions_disagree": sorted(k for k, r in report.get("positions", {}).items() if r["status"] == "disagree")}
    want = sample.verify
    if report["exit_code"] != want["exit"]:
        problems.append(f"verify 退出码 {report['exit_code']}，预期 {want['exit']}")
    if failures != want["failures"]:
        extra, missing = sorted(failures - want["failures"]), sorted(want["failures"] - failures)
        problems.append(f"verify 的失效项和预期不同：多出 {extra}，缺少 {missing}")

    result, _ = repair_mod.repair_exe(sample.path, out=str(out_dir / "repair" / sample.name), workers=args.workers)
    statuses = {sid: r["status"] for sid, r in result["sigs"].items()}
    statuses.update({iid: r["status"] for iid, r in result["layout"].items()})
    rec["repair"] = {"exit": result["exit_code"], "after_exit": result["after"]["exit_code"] if result.get("after") else None,
                     "status": {k: v for k, v in sorted(statuses.items()) if v not in ("ok", "unverified")}}
    if result["exit_code"] != sample.repair["exit"]:
        problems.append(f"repair 退出码 {result['exit_code']}，预期 {sample.repair['exit']}")
    expect = sample.repair["status"]
    for name, status in statuses.items():
        cls = _status_class(status)
        wanted = expect.get(name)
        if wanted is None and cls not in ("ok", "unverified"):
            problems.append(f"repair：{name} 是 {status}，样本表里没写（预期 ok）")
        elif wanted is not None and cls != wanted:
            problems.append(f"repair：{name} 是 {status}，预期 {wanted}")

    # 不变式 1：不能修错、不能漏报
    truth = sample.truth
    wrong = []
    sig_fail = {name for name, sev in failures if name in manifest.sigs}
    for sid, r in result["sigs"].items():
        want_loc = truth["sigs"].get(sid, {"hit": base_res[sid].hit, "resolved": base_res[sid].resolved})
        kind = manifest.sigs[sid].kind
        if r["status"].startswith("repaired"):
            got = r["new"]
            if got["resolved"] != want_loc["resolved"] or (kind in ("HOOK", "CODE") and got["hit"] != want_loc["hit"]):
                wrong.append(f"特征码 {sid} 修到 {got['hit']:#x}/{got['resolved']:#x}，真值 {want_loc['hit']:#x}/{want_loc['resolved']:#x}")
        elif r["status"] == "ok" and sid not in sig_fail:
            now = report["sigs"][sid]
            if now["resolved"] != want_loc["resolved"] or (kind in ("HOOK", "CODE") and now["hit"] != want_loc["hit"]):
                wrong.append(f"特征码 {sid} verify 判通过，解析到 {now['hit']:#x}/{now['resolved']:#x}，真值 "
                             f"{want_loc['hit']:#x}/{want_loc['resolved']:#x}")
    for iid, r in result["layout"].items():
        if r["status"] in ("manual", "unverified"):
            continue
        item = manifest.items[iid]
        want_value = truth["layout"].get(iid, item.value if item.type != "TABLE" else list(item.names or item.values or []))
        if want_value == mutants_mod.ABSENT:
            wrong.append(f"{iid} 在新版本里已经不存在，却判 {r['status']}")
            continue
        if isinstance(want_value, list):
            current = list(item.names or item.values or [])
            if current != want_value:
                wrong.append(f"{iid} 判 {r['status']}，表的内容和真值不同")
            continue
        final = r["new"] if r["new"] is not None else item.value
        if final != want_value:
            wrong.append(f"{iid} 判 {r['status']}，值 {final:#x}，真值 {want_value:#x}")
    rec["wrong"] = wrong
    problems += [f"给错结论：{w}" for w in wrong]

    # 不变式 2：预演 verify 剩下的失效项都要报了需要人工
    manual = {k for k, v in statuses.items() if v == "manual"}
    leftover = sorted({f["name"] for f in (result.get("after") or {}).get("failures", [])} - manual)
    if leftover:
        problems.append(f"repair 的预演 verify 还有失效项，但没报需要人工：{leftover}")
    return rec


def step_mutants(step, out_dir, args):
    selected = [s for s in mutants_mod.SAMPLES if not args.samples or s.name in args.samples]
    samples = mutants_mod.generate(selected, log=step.log)
    manifest = manifest_mod.load()
    baselines = [baseline_mod.load(p) for p in baseline_mod.default_files()]
    base = Image(mutants_mod.STEAM)
    base_res = sigs_mod.resolve_all(base, manifest, "steam", [])
    categories = {}
    for sample in samples:
        started = time.time()
        try:
            rec = check_sample(step, sample, manifest, baselines, base_res, out_dir, args)
        except Exception as e:  # noqa: BLE001 - 一个样本出错不影响其他样本，记为不通过
            rec = {"category": sample.category, "problems": [f"出错：{type(e).__name__}: {e}"],
                   "traceback": traceback.format_exc()}
        rec["seconds"] = round(time.time() - started, 1)
        step.data[sample.name] = rec
        categories.setdefault(sample.category, []).append(sample.name)
        v, r = rec.get("verify", {}), rec.get("repair", {})
        step.log(f"{sample.name:<18} [{sample.category}] verify 退出码 {v.get('exit')}（失效 {len(v.get('failures', []))} 项）；"
                 f"repair 退出码 {r.get('exit')}，非 ok 的项 {r.get('status')}；{rec['seconds']} 秒"
                 + ("；通过" if not rec["problems"] else ""))
        for p in rec["problems"]:
            step.fail(f"{sample.name}：{p}")
    required = ("结构偏移整体变化", "特征码被破坏", "多处命中", "静默误绑定", "函数搬迁", "枚举表变化", "hook 入口不安全",
                "虚表槽位变化", "依据误绑定")
    missing = [c for c in required if c not in categories]
    step.data["_categories"] = categories
    if missing and not args.samples:
        step.fail(f"缺少这些类别的样本：{missing}")


# ---- 3. 清单变异冒烟 ----

def step_mcheck(step, out_dir, args):
    ok, summary = mcheck.run(out_dir / "mcheck", log=step.log)
    step.data = {k: v for k, v in summary.items() if k != "rows"}
    if not ok:
        step.fail("；".join(summary["problems"]))


# ---- 4. 往返 ----

def _hash_files(paths):
    out = {}
    for p in paths:
        p = Path(p)
        out[str(p)] = hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
    return out


def _repo_files():
    return [manifest_mod.SIGS_FILE, manifest_mod.LAYOUT_FILE] + list(baseline_mod.default_files()) + \
        sorted((baseline_mod.BASELINE_DIR / repair_mod.CONFIRM_DIR).glob("*.json"))


def copy_tree(dest):
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", "build", ".xmake")
    for part in TREE_PARTS:
        src = PROJECT / part
        if src.is_dir():
            shutil.copytree(src, dest / part, ignore=ignore)
        elif src.is_file():
            shutil.copy2(src, dest / part)


def step_roundtrip(step, out_dir, args):
    name = args.roundtrip_sample
    sample = next(s for s in mutants_mod.SAMPLES if s.name == name)
    exe = mutants_mod.OUT_DIR / f"{name}.exe"
    if not exe.is_file():
        mutants_mod.generate([sample], log=step.log)
    before = _hash_files(_repo_files())
    work = out_dir / "roundtrip"
    tree = work / "tree"
    copy_tree(tree)
    step.log(f"源码复制到 {tree}（{', '.join(TREE_PARTS)}）")

    # repair --apply --tree，再 verify
    result, report = repair_mod.repair_exe(str(exe), tree=str(tree), apply=True, out=str(work / "repair"), workers=args.workers)
    diff_sig = result["patch"].get("signatures", "")
    diff_layout = result["patch"].get("layout") or ""
    step.log(f"repair --apply --tree：退出码 {result['exit_code']}，补丁 signatures.inc {diff_sig.count(chr(10) + '+') } 行、"
             f"layout.inc {diff_layout.count(chr(10) + '+')} 行；apply 后 verify 退出码 {report['exit_code']}")
    step.data["repair"] = {"exit": result["exit_code"], "verify_after_apply": report["exit_code"],
                           "failures_after_apply": report["failures"]}
    if report["exit_code"] != 0:
        step.fail(f"apply 之后 verify 退出码 {report['exit_code']}：{report['failures']}")
    if not diff_sig and not diff_layout:
        step.fail("补丁是空的，往返没有测到东西")

    # 用修复后的清单编译 DDCore（只编译）
    env_sh = (PROJECT / "tools" / "env.sh").as_posix()
    tree_posix = tree.as_posix()
    cmd = (f"source '{env_sh}' >/dev/null 2>&1; cd '{tree_posix}' && xmake f -P . -p windows -a x64 -m releasedbg -y && "
           f"xmake build -P . DDCore")
    started = time.time()
    proc = subprocess.run(["bash", "-lc", cmd], capture_output=True, text=True, encoding="utf-8", errors="replace")
    (work / "build_ddcore.txt").write_text(proc.stdout + proc.stderr, encoding="utf-8")
    dll = tree / "build" / "windows" / "x64" / "releasedbg" / "DDCore.dll"
    built = dll.is_file() and dll.stat().st_mtime >= started - 1
    step.log(f"副本里 xmake build DDCore：退出码 {proc.returncode}，{round(time.time() - started)} 秒，"
             f"{dll.name} {'已生成' if built else '没有生成'}（输出在 {work / 'build_ddcore.txt'}）")
    step.data["build"] = {"exit": proc.returncode, "dll": str(dll), "built": built}
    if proc.returncode != 0 or not built:
        step.fail("副本里编译 DDCore 失败")

    # accept 往返（U1 修复 FB：先不带 --confirm，要拒绝、副本不变；再生成确认清单、逐项填好后接受）
    label = f"u1d_{name}"
    import io
    sha1 = Image(str(exe)).sha1
    tree_sigs = tree / "core" / "src" / "game" / "signatures.inc"
    known_line = f'DD_KNOWN_BUILD("{sha1}", "{label}")'
    new_baseline = tree / "tools" / "baseline" / f"{label}.json"
    tree_manifest = manifest_mod.load(str(tree_sigs), str(tree / "core" / "src" / "game" / "layout.inc"))

    def accept(**extra):
        ns = argparse.Namespace(exe=str(exe), label=label, replace=False, allow_optional=False, tree=str(tree), baseline=None,
                                out=None, sigs=None, layout=None, confirm=None, confirm_template=None)
        for k, v in extra.items():
            setattr(ns, k, v)
        buf = io.StringIO()
        return repair_mod.cmd_accept(ns, buf), buf.getvalue()

    code0, text0 = accept()
    (work / "accept_noconfirm.txt").write_text(text0, encoding="utf-8")
    refused = code0 != 0 and known_line not in tree_sigs.read_text(encoding="utf-8") and not new_baseline.exists()
    step.log(f"accept 不带 --confirm：退出码 {code0}，{'拒绝、副本没变' if refused else '没有按预期拒绝'}")
    if not refused:
        step.fail("accept 不带 --confirm 时应当拒绝、不改副本")
    template = work / "confirm_template.json"
    code_t, text_t = accept(confirm_template=str(template))
    data = json.loads(template.read_text(encoding="utf-8")) if template.is_file() else {"items": {}}
    nones = sorted(i for i, it in tree_manifest.items.items() if it.check == "NONE")
    missing_none = [i for i in nones if i not in data["items"]]
    grouped = sorted(i for i, e in data["items"].items() if any(w.startswith("组 ") for w in e["why"]))
    # struct_shift apply 之后和基线比：Instance 组、GroupEntry 组的值变了，BuffGroupAdd 的特征码重新生成过；别的组不该触发
    want_grouped = {"InstStat", "InstScalarSize", "InstFlagSize", "GameTextCap", "BuffEntryGuid", "BuffGuidSize",
                    "ProtoBuffGroupAdd"}
    step.log(f"确认清单模板：{len(data['items'])} 项（NONE {len(nones)} 个，缺 {len(missing_none)}；带同组原因的 {len(grouped)} 个："
             f"{', '.join(grouped[:10])}{' …' if len(grouped) > 10 else ''}）")
    step.data["confirm_template"] = {"items": len(data["items"]), "missing_none": missing_none, "grouped": grouped}
    if code_t != 0 or missing_none or want_grouped != set(grouped):
        step.fail(f"确认清单模板不对：退出码 {code_t}，缺 NONE 项 {missing_none[:5]}，同组升级的项缺 "
                  f"{sorted(want_grouped - set(grouped))}、多出 {sorted(set(grouped) - want_grouped)}")
    for iid, entry in data["items"].items():
        entry["method"] = f"selftest 往返：变异样本 {name} 只改了 Buff::Instance 的字段和按 Entry 做除法的函数，这一项不受影响（样本说明）"
        entry["evidence"] = "DD.ModLoader/tools/ddupdate/mutants.py"
    filled = work / "confirm.json"
    filled.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    code, text_a = accept(confirm=str(filled))
    (work / "accept.txt").write_text(text0 + "\n----\n" + text_t + "\n----\n" + text_a, encoding="utf-8")
    text = tree_sigs.read_text(encoding="utf-8")
    ok_line = known_line in text
    ok_baseline = new_baseline.is_file() and baseline_mod.load(new_baseline)["exe"]["sha1"] == sha1
    record = tree / "tools" / "baseline" / repair_mod.CONFIRM_DIR / f"{label}.json"
    ok_record = record.is_file() and json.loads(record.read_text(encoding="utf-8")).get("sha1") == sha1
    step.log(f"确认记录 {record.name}：{'已写' if ok_record else '没写'}")
    if not ok_record:
        step.fail("accept 通过后没有写确认记录")
    tree_manifest = manifest_mod.load(str(tree_sigs), str(tree / "core" / "src" / "game" / "layout.inc"))
    tree_baselines = [baseline_mod.load(p) for p in sorted((tree / "tools" / "baseline").glob("*.json"))]
    del tree_manifest  # 重新读（accept 改了 signatures.inc）
    tree_manifest = manifest_mod.load(str(tree_sigs), str(tree / "core" / "src" / "game" / "layout.inc"))
    again = verify_mod.verify_exe(str(exe), tree_manifest, tree_baselines, verify_mod.SECTIONS, sigs_mod.foreign_patches())
    step.log(f"accept --label {label}：退出码 {code}；DD_KNOWN_BUILD {'已加' if ok_line else '没加'}，基线 {new_baseline.name} "
             f"{'已生成' if ok_baseline else '没有生成'}；再 verify：{again['known'] or '未知版本'}，退出码 {again['exit_code']}")
    step.data["accept"] = {"exit": code, "known_build_line": ok_line, "baseline": ok_baseline, "verify_known": again["known"],
                           "verify_exit": again["exit_code"]}
    if code != 0 or not ok_line or not ok_baseline or again["known"] != label or again["exit_code"] != 0:
        step.fail("accept 往返没有按预期更新")
    after = _hash_files(_repo_files())
    changed = [p for p in set(before) | set(after) if before.get(p) != after.get(p)]
    step.log("仓库里的清单和基线" + ("没有变化" if not changed else f"变了：{changed}"))
    if changed:
        step.fail(f"仓库里的文件被改了：{changed}")


# ---- 5. 规则自检（U1 修复 FB） ----

def _mini_tree(dest):
    """只有清单和基线的源码副本（accept 在里面改，不碰仓库）。"""
    shutil.rmtree(dest, ignore_errors=True)
    (dest / "core" / "src" / "game").mkdir(parents=True)
    shutil.copy(manifest_mod.SIGS_FILE, dest / "core" / "src" / "game" / "signatures.inc")
    shutil.copy(manifest_mod.LAYOUT_FILE, dest / "core" / "src" / "game" / "layout.inc")
    shutil.copytree(baseline_mod.BASELINE_DIR, dest / "tools" / "baseline")
    return dest


def step_gate(step, out_dir, args):
    import io
    import dataclasses
    work = out_dir / "gate"
    work.mkdir(parents=True, exist_ok=True)
    # 1. cpp_compare 的分类规则
    problems = cpp_compare.self_check()
    step.log(f"cpp_compare 分类规则自检：{'通过' if not problems else '；'.join(problems)}")
    if problems:
        step.fail("cpp_compare 的分类规则：" + "；".join(problems))
    # 2. LOADER 的 GLOBAL 指向 .text；HOOK 偷取不到 5 字节
    manifest = manifest_mod.load()
    steam = Image(mutants_mod.STEAM)
    base = manifest.sigs["StateLast"]
    loader_global = dataclasses.replace(base, id="ProbeLoaderGlobal", required="LOADER")
    res = sigs_mod.resolve(steam, loader_global)
    # StateLast 解析到 .data；把 capture 指到同一条模式里一条 rel32（jz 的位移），解析出的地址落在 .text
    text_capture = dataclasses.replace(loader_global, capture=16)
    res_text = sigs_mod.resolve(steam, text_capture)
    ok_loader = res.status == "ok" and res_text.status == "fail" and "section" in res_text.kinds
    step.log(f"LOADER 的 GLOBAL：指向 .data 时 {res.status}；指向 {res_text.section} 时 {res_text.status} {res_text.kinds}")
    if not ok_loader:
        step.fail("LOADER 的 GLOBAL 指向 .text 时应当失效（类别 section）")
    hook = manifest.sigs["UpdateStateInternal"]
    short = dataclasses.replace(hook, steal=3)
    fake = manifest_mod.Manifest(sigs={short.id: short}, sig_file=hook.file)
    lint = [x for x in manifest_mod.lint(fake, require_deps=False) if "steal" in x]
    r = sigs_mod.resolve(steam, short)
    sigs_mod.check_hook(steam, sigs_mod.engine(), short, r, [], None)
    ok_steal = bool(lint) and any("小于 5" in x for x in r.problems)
    step.log(f"HOOK 偷取 3 字节：清单检查 {lint[:1]}；verify {r.problems}")
    if not ok_steal:
        step.fail("HOOK 偷取不到 5 字节时清单检查和 verify 都要报")
    # 2b. GLOBAL_ANY 的同一性（审核 A 的 S6）：换成别的模板族的命中函数代表、改掉候选数，都要判误绑定
    steam_base = baseline_mod.load(baseline_mod.BASELINE_DIR / "steam.json")
    steam_res = sigs_mod.resolve_all(steam, manifest, "steam", [])
    cs = sigs_mod.engine()
    imports = verify_mod.Imports(steam)

    def global_any(base):
        rec = {"problems": [], "notes": []}
        verify_mod._global_any(steam, "EventTypeCount", steam_res["EventTypeCount"], [base], cs, imports, rec)
        return rec

    same = global_any(steam_base)
    alien = {"sigs": {"EventTypeCount": dict(steam_base["sigs"]["EventTypeCount"],
                                             hit_reps=steam_base["sigs"]["EventClassNames"]["hit_reps"])}}
    other_family = global_any(alien)
    fewer = {"sigs": {"EventTypeCount": dict(steam_base["sigs"]["EventTypeCount"], candidates=[0, 1])}}
    count = global_any(fewer)
    ok_s6 = not same["problems"] and any("都不像" in x for x in other_family["problems"]) \
        and any("候选数" in x for x in count["problems"])
    step.log(f"GLOBAL_ANY（EventTypeCount）：和自己的基线 {same['problems'] or '通过'}；代表换成 EventClassNames 的 "
             f"{other_family['problems'][:1]}；基线候选数改成 2 {count['problems'][:1]}")
    if not ok_s6:
        step.fail("GLOBAL_ANY 的同一性检查没有按预期判误绑定")
    # 3. accept 的拦截
    need = [s for s in mutants_mod.SAMPLES if s.name in ("enum", "inst44", "hook_entry")
            and not (mutants_mod.OUT_DIR / f"{s.name}.exe").is_file()]
    if need:
        mutants_mod.generate(need, log=step.log)

    def accept(exe, **extra):
        tree = _mini_tree(work / "tree")
        before = (tree / "core" / "src" / "game" / "signatures.inc").read_bytes()
        ns = argparse.Namespace(exe=str(exe), label="gate_probe", replace=False, allow_optional=False, tree=str(tree),
                                baseline=None, out=None, sigs=None, layout=None, confirm=None, confirm_template=None)
        for k, v in extra.items():
            setattr(ns, k, v)
        buf = io.StringIO()
        code = repair_mod.cmd_accept(ns, buf)
        after = (tree / "core" / "src" / "game" / "signatures.inc").read_bytes()
        return code, buf.getvalue(), before != after

    cases = []
    for name in ("enum", "inst44"):
        exe = mutants_mod.OUT_DIR / f"{name}.exe"
        for allow in (False, True):
            code, text, changed = accept(exe, allow_optional=allow)
            tail = text[text.rfind(" 结论："):]
            (work / f"{name}{'_allow' if allow else ''}.txt").write_text(text, encoding="utf-8")
            good = code != 0 and not changed and "一律" not in tail and "不接受" in tail \
                and "--allow-optional 接受" not in tail and "运行时发现不了" in tail
            cases.append((f"{name}{' --allow-optional' if allow else ''}", code, good, tail.splitlines()[1] if "\n" in tail else tail))
    exe = mutants_mod.OUT_DIR / "hook_entry.exe"
    code, text, changed = accept(exe)
    (work / "hook_entry.txt").write_text(text, encoding="utf-8")
    good = code != 0 and not changed and "--allow-optional" in text and "运行时自己会发现" in text
    cases.append(("hook_entry", code, good, "要 --allow-optional"))
    code, text, changed = accept(exe, allow_optional=True)
    (work / "hook_entry_allow.txt").write_text(text, encoding="utf-8")
    good = code != 0 and not changed and "--confirm" in text
    cases.append(("hook_entry --allow-optional", code, good, "还要 --confirm"))
    for label, code, good, why in cases:
        step.log(f"accept {label}：退出码 {code}，{'按预期拒绝' if good else '不符合预期'}（{why.strip()[:120]}）")
        if not good:
            step.fail(f"accept {label} 没有按预期拒绝")
    step.data = {"cases": [(c[0], c[1], c[2]) for c in cases]}
    # 4. B1 的原始反例：审核时的宽松依据（U1 修复 FA 收紧之前），位置核对没法对照时不能判通过
    loose = {"InstSubHash": "8B 42 ?? 89 41 44", "InstAmount": "8B 42 ?? 89 41 48", "InstSource": "8B 42 ?? 89 41 54",
             "InstSourceData": "8B 42 ?? 89 41 58"}
    tree = _mini_tree(work / "loose")
    layout = tree / "core" / "src" / "game" / "layout.inc"
    text = layout.read_text(encoding="utf-8")
    for iid, pattern in loose.items():
        text, n = re.subn(rf'^(DD_EVIDENCE\({iid}, BuffInstanceCopy, TARGET, 0, FUNC, 0, )"[^"]*"', rf'\1"{pattern}"', text,
                          count=1, flags=re.M)
        if n != 1:
            step.fail(f"清单副本里没找到 {iid} 的复制构造依据")
    layout.write_text(text, encoding="utf-8", newline="")
    exe = mutants_mod.OUT_DIR / "inst44.exe"
    loose_manifest = manifest_mod.load(str(tree / "core" / "src" / "game" / "signatures.inc"), str(layout))
    loose_baselines = [baseline_mod.load(p) for p in sorted((tree / "tools" / "baseline").glob("*.json"))]
    report = verify_mod.verify_exe(str(exe), loose_manifest, loose_baselines, verify_mod.SECTIONS, sigs_mod.foreign_patches())
    unconfirmed = sorted(f["name"] for f in report["failures"] if f["severity"] == "unconfirmed")
    ok_items = sorted(i for i in loose if report["layout"][i]["status"] == "ok")
    step.log(f"B1 原始反例（宽松依据 + inst44.exe）：verify 退出码 {report['exit_code']}，无法确认 {unconfirmed}；"
             f"依据本身成立的 {ok_items}")
    step.data["b1_loose"] = {"verify_exit": report["exit_code"], "unconfirmed": unconfirmed, "evidence_ok": ok_items}
    if report["exit_code"] != 2 or not {"InstSubHash", "InstAmount"} <= set(unconfirmed):
        step.fail("宽松依据在 inst44.exe 上应当报无法确认（InstSubHash、InstAmount 的依据成立、位置没法对照），退出码 2")
    result, _ = repair_mod.repair_exe(str(exe), tree=str(tree), out=str(work / "loose_repair"), workers=args.workers)
    wrong = []
    for iid in loose:
        r = result["layout"][iid]
        final = r["new"] if r["new"] is not None else r["old"]
        if r["status"] != "manual" and final != r["old"] + 4:
            wrong.append(f"{iid} {r['status']} {final:#x}（真值 {r['old'] + 4:#x}）")
    step.log("B1 原始反例的 repair：" + "，".join(f"{i} {result['layout'][i]['status']}" for i in loose)
             + (f"；修错 {wrong}" if wrong else "；没有修错"))
    if wrong:
        step.fail(f"B1 原始反例 repair 修错：{wrong}")
    code, text_a, changed = accept(exe)
    (work / "loose_accept.txt").write_text(text_a, encoding="utf-8")
    good = code != 0 and not changed
    step.log(f"B1 原始反例的 accept（宽松依据的副本另算，这里用正式清单的副本）：退出码 {code}，{'拒绝' if good else '没有拒绝'}")
    if not good:
        step.fail("inst44.exe 不应被 accept")


# ---- 6. C++/Python 对照 ----

def step_compare(step, out_dir, args):
    binary = cpp_compare.DEFAULT_BIN
    if not binary.is_file():
        step.fail(f"没有 {binary}：先 source DD.ModLoader/tools/env.sh; dd_config; xmake build dd_layout_compare（没有构建不能当作通过）")
        return
    u1b = PROJECT / "build" / "u1b" / "mut"
    if not any(u1b.glob("*.exe")):
        step.log("build/u1b/mut 没有样本，先跑 tests/layout_compare/make_mutants.py")
        subprocess.run([sys.executable, "-B", str(PROJECT / "tests" / "layout_compare" / "make_mutants.py")], check=False)
    exes = cpp_compare.default_exes()
    step.log(f"比较 {len(exes)} 个 exe：{', '.join(tag for tag, _p in exes)}")
    code = cpp_compare.main(["--out", str(out_dir / "compare"), "--bin", str(binary)])
    text = (out_dir / "compare" / "compare.txt").read_text(encoding="utf-8") if (out_dir / "compare" / "compare.txt").is_file() else ""
    summary = [ln for ln in text.splitlines() if ln.startswith(("汇总", "结果"))]
    for ln in summary:
        step.log(ln)
    step.data = {"exit": code, "exes": [t for t, _p in exes]}
    if code != 0:
        step.fail(f"cpp_compare 退出码 {code}")


RUNNERS = {"blind": step_blind, "mutants": step_mutants, "mcheck": step_mcheck, "roundtrip": step_roundtrip,
           "gate": step_gate, "compare": step_compare}
TITLES = {"blind": "跨版本盲定位", "mutants": "变异样本", "mcheck": "清单变异冒烟", "roundtrip": "往返（repair --apply、编译、accept）",
          "gate": "规则自检（对照分类、LOADER GLOBAL、偷取字节、accept 的拦截）", "compare": "C++/Python 对照"}


def run(args, out):
    steps = [s for s in STEPS if not args.only or s in args.only]
    out_dir = Path(args.out) if args.out else OUT_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    started = time.time()
    for name in steps:
        step = Step(name)
        print(f"== {TITLES[name]}", flush=True)
        t = time.time()
        try:
            RUNNERS[name](step, out_dir, args)
        except Exception as e:  # noqa: BLE001 - 一步出错记为不通过，接着跑下一步
            step.fail(f"出错：{type(e).__name__}: {e}")
            step.data["traceback"] = traceback.format_exc()
            print(step.data["traceback"], flush=True)
        step.seconds = round(time.time() - t, 1)
        print(f"   {'通过' if step.ok else '不通过'}（{step.seconds} 秒）", flush=True)
        results.append(step)
    lines = [f"dd_update.py selftest：{len(steps)} 步，{round(time.time() - started)} 秒"]
    for step in results:
        lines.append(f"== {TITLES[step.name]}：{'通过' if step.ok else '不通过'}（{step.seconds} 秒）")
        lines += [f"   {ln}" for ln in step.lines]
    code = 0 if all(s.ok for s in results) else 1
    lines.append(f"结果：{'全部通过' if code == 0 else '有不通过的步骤'}，退出码 {code}")
    text = "\n".join(lines) + "\n"
    (out_dir / "summary.txt").write_text(text, encoding="utf-8")
    (out_dir / "summary.json").write_text(json.dumps({"exit_code": code, "steps": {s.name: {"ok": s.ok, "seconds": s.seconds,
                                                                                          "lines": s.lines, "data": s.data}
                                                                                 for s in results}},
                                                     ensure_ascii=False, indent=1, default=str), encoding="utf-8")
    out.write("\n" + text)
    out.write(f"汇总：{out_dir / 'summary.txt'}、summary.json\n")
    return code


def add_parser(sub):
    p = sub.add_parser("selftest", help="U1 的自测：盲定位、变异样本、清单变异、往返、C++/Python 对照")
    p.add_argument("--only", type=lambda s: s.split(","), help=f"只跑这几步：{','.join(STEPS)}")
    p.add_argument("--samples", type=lambda s: s.split(","), help="只跑这几个变异样本（mutants.py 的 SAMPLES）")
    p.add_argument("--roundtrip-sample", default=ROUNDTRIP_SAMPLE, help=f"往返用的样本，默认 {ROUNDTRIP_SAMPLE}")
    p.add_argument("--out", help=f"输出目录，默认 {OUT_DIR}")
    p.add_argument("--workers", type=int, help="函数索引的进程数")

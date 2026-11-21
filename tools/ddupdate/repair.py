"""repair / accept / blind 命令：失效项的定位和修复、确认新版本、跨版本盲定位。

repair（只读，--apply 才写清单）：
  1. 对新 exe 跑 verify 的全部检查（特征码、同一性、布局），失败的特征码（没命中、多处命中、hook 安全检查不过、
     DD_ENTRY 不符、误绑定）进入定位。
  2. 已经通过（特征码和同一性都过）的特征码、成立的依据当作已确认的新旧对应关系（种子）。
  3. locate.Matcher 做函数级匹配（放宽特征码、调用图、锚点、指纹搜索），locate_sig 推出新的命中位置和解析结果，
     regen.gen_sig 按原条目的种类和语义重新生成特征码并自检。
  4. layoutfix 用修好的特征码重新求值布局依据，给出新值和新的依据模式。
  5. 把补丁写进临时目录的清单副本，在新 exe 上跑一遍完整的 verify（预演）：修复过的项要通过，不通过的改成
     「需要人工」并从补丁里拿掉，再预演，直到稳定。
  6. 输出逐项报告（人读 report.txt、机器读 report.json）、清单的 unified diff、旧 -> 新 RVA 对照表（rva_map.tsv）。
     --apply 把补丁写进清单文件（--tree 时写进那个目录下的副本），再跑一遍 verify。不改 DD_KNOWN_BUILD。
  7. 同组升级（U1 修复 FB，审核 A 的 S4）：DD_GROUP 的组里有 RUNTIME 项的值变了（或需要人工）、有特征码被重新定位
     （或需要人工）时，组里没有运行时依据的项（NONE、OFFLINE）改成 manual，原因写明是哪个成员变了。
状态：ok（不用修）、repaired-高、repaired-中、manual（需要人工，列出候选和原因）。
退出码：0 没有 manual；1 只有可选项 manual；2 有必需项 manual；3 参数或清单有错。

accept（U1 修复 FB 重写，审核 A 的 B2、S4、B1）：把 exe 的 SHA1 加进 DD_KNOWN_BUILD、生成这个版本的基线。已知版本在运行时
不再停用只能离线核对、没有静态依据的项（ADR 0036），所以 accept 只接受运行时自己发现不了的问题都已经排除或人工确认过的
版本：
  - 一律拒绝：必需项失效、误绑定、只有 verify 做的特征码静态检查不过（偷取范围、DD_ENTRY 等，运行时照样装 hook）、
    离线依据不过（OFFLINE 项，或 RUNTIME 项上挂的离线检查）。这些要先修（repair 或人工改清单），--allow-optional 不管用。
  - --allow-optional 才接受：运行时自己会发现并停用的失效，即可选特征码失效（原因全是命中、解析、函数入口这类运行时也查
    的）和可选 RUNTIME 项的运行时依据不成立。
  - --confirm <清单> 才接受：全部 NONE 项、同组升级的项（verify 的「同组有变化」）、依据位置无法确认的项。清单逐项写
    method（怎么核实的）和 evidence（证据路径，相对工作区根目录或 DD.ModLoader，或绝对路径，要存在），sha1 要等于这个
    exe。--confirm-template <文件> 生成要填的清单（每项附上原因和 DD_UNVERIFIED 的说明），不接受。接受时清单复制到
    基线目录的 confirm/<版本名>.json，和基线、DD_KNOWN_BUILD 一起进仓库，可以追溯。

blind：盲定位自测。以一个版本的基线为准、不用现有特征码扫描（放宽特征码这一手段也关掉，模式只用来在基线的
旧函数里认出锚点指令——format 2 的基线直接存了下标，用不到模式），只靠调用图、锚点、指纹搜索在另一个 exe 里找
全部特征码项，和现有特征码在那个 exe 上的解析结果逐项比较：正确 / 需要人工 / 错误。错误必须为 0（退出码 2）。
selftest（U1d）直接调用 blind_compare。
"""
import datetime
import json
import shutil
import tempfile
import time
from pathlib import Path

from . import align
from . import baseline as baseline_mod
from . import evidence as evidence_mod
from . import locate as locate_mod
from . import manifest as manifest_mod
from . import patch as patch_mod
from . import regen
from . import sigs as sigs_mod
from . import verify as verify_mod
from .fingerprint import Imports, engine, fingerprint
from .image import Image, compile_pattern
from .index import FunctionIndex, dice, shingle_ids
from .layoutfix import LayoutRepair
from .locate import new_function_of, seed_from_evidence, seed_from_ok
from .manifest import PROJECT, ROOT

EXIT_OK, EXIT_OPTIONAL, EXIT_REQUIRED, EXIT_ERROR = 0, 1, 2, 3
REPAIR_DIR = PROJECT / "build" / "repair"
TMP_DIR = PROJECT / "build" / "ddupdate-tmp"
CONF_RANK = {"高": 2, "中": 1}
RELAXED_MIN_INSNS = 4
EXPAND_LIMIT = 5000


# ---- 清单的位置 ----

class Paths:
    """清单文件和基线目录。--tree 指向源码目录（或它的副本），清单和基线都从那里读写。"""

    def __init__(self, args):
        tree = Path(args.tree) if getattr(args, "tree", None) else None
        self.tree = tree
        game = (tree / "core" / "src" / "game") if tree else manifest_mod.SIGS_FILE.parent
        if getattr(args, "sigs", None):
            self.sigs = Path(args.sigs)
            self.layout = Path(args.layout) if getattr(args, "layout", None) else None
            self.plugin = True
        else:
            self.sigs = game / "signatures.inc"
            self.layout = Path(args.layout) if getattr(args, "layout", None) else game / "layout.inc"
            self.plugin = False
        if tree and (tree / "tools" / "baseline").is_dir():
            self.baseline_dir = tree / "tools" / "baseline"
        else:
            self.baseline_dir = baseline_mod.BASELINE_DIR
        self.rel_sigs = _rel(self.sigs, tree)
        self.rel_layout = _rel(self.layout, tree) if self.layout else None

    def load(self):
        return manifest_mod.load(str(self.sigs), str(self.layout) if self.layout else None, use_layout=bool(self.layout))

    def baselines(self, explicit):
        files = explicit or ([] if self.plugin else sorted(self.baseline_dir.glob("*.json")))
        return [baseline_mod.load(p) for p in files]


def _rel(path, tree):
    path = Path(path).resolve()
    for base in ([tree.resolve()] if tree else []) + [PROJECT.resolve()]:
        try:
            return path.relative_to(base).as_posix()
        except ValueError:
            continue
    return path.name


def hit_bound(manifest, sid):
    """有依据用这条特征码的命中位置（HIT、HIT_VALUE）时，重新生成的模式不能挪起点。"""
    return any(ev.sig == sid and ev.origin in ("HIT", "HIT_VALUE") for ev in manifest.evidence)


def make_hit_check(image, manifest, sig_results, sid, resolved):
    """以某个位置为 sid 的命中位置时，用这条特征码命中位置（HIT、HIT_VALUE）的依据里成立的那些（按行号）。"""
    evs = [ev for ev in manifest.evidence if ev.sig == sid and ev.origin in ("HIT", "HIT_VALUE")]
    sig = manifest.sigs[sid]
    cache = {}

    def check(hit):
        if hit not in cache:
            res = sigs_mod.SigResult(sid, sig.kind, sig.need, hits=[hit], hit=hit, resolved=resolved)
            results = dict(sig_results)
            results[sid] = res
            cache[hit] = frozenset(ev.line for ev in evs
                                   if evidence_mod.evaluate(image, manifest, results, ev).status == "ok")
        return cache[hit]
    return check


def need_class(sig):
    return {"REQUIRED": "REQUIRED", "LOADER": "REQUIRED", "OPT": "OPT", "ANCHOR": "ANCHOR"}[sig.required]


# ---- 种子和放宽特征码 ----
# new_function_of、seed_from_ok、seed_from_evidence 在 locate.py（verify 的依据位置核对也用它们播种）


def relaxed_hints(m, image, manifest, baseline, failing, cs):
    """放宽特征码：函数候选交给 Matcher，命中位置（GLOBAL、函数中间的 CODE 的锚点）按特征码记下。"""
    direct, info = {}, {}
    for sid in failing:
        sig, entry = manifest.sigs[sid], baseline["sigs"].get(sid)
        if not entry or not entry["function"]:
            continue
        relaxed, count = align.relax_pattern(sig.pattern, cs, sig.entry)
        tokens = relaxed.split()
        hits = []
        while True:
            regex, _ = compile_pattern(relaxed)
            hits = image.scan_text(regex)
            if hits or count <= RELAXED_MIN_INSNS:
                break
            # 不命中时去掉最后一条指令再试（函数后半段变了）
            _lead, insns = align.decode_pattern(sig.pattern, cs, sig.entry)
            count -= 1
            end = insns[count].address if count < len(insns) else len(tokens)
            relaxed = " ".join(tokens[:end])
        info[sid] = {"pattern": relaxed, "hits": len(hits)}
        if not hits or len(hits) > locate_mod.RELAXED_MAX_HITS:
            continue
        fn = entry["function"]
        entry_kind = sig.kind in ("HOOK", "CODE") and entry["hit"] == fn["rva"]
        for h in hits:
            if entry_kind:
                if h in m.index.funcs or image.is_function_start(h):
                    m.hint(fn["rva"], h, "放宽特征码")
            else:
                f = new_function_of(image, fn, h)
                if f is not None:
                    m.hint(fn["rva"], f, "放宽特征码")
                    direct.setdefault(sid, {}).setdefault(f, []).append(h)
    return direct, info


def sig_nodes(entry):
    out = []
    for fp in (entry.get("function"), entry.get("target_function")):
        while fp:
            out.append(fp["rva"])
            fp = fp.get("thunk")
    return out


# ---- repair ----

def _sig_old(sig, entry):
    return {"pattern": sig.pattern, "steal": sig.steal, "capture": sig.capture, "entry": sig.entry,
            "baseline_hit": entry["hit"] if entry else None, "baseline_resolved": entry["resolved"] if entry else None}


pick_baseline = verify_mod.pick_baseline   # 选「更新前」的基线，和 verify 的依据位置核对用同一个


def run_repair(exe, paths, baselines, args, log):
    manifest = paths.load()
    problems = manifest_mod.lint(manifest, require_deps=not paths.plugin)
    if problems:
        raise manifest_mod.ManifestError("清单有问题：" + "；".join(problems[:5]))
    image = Image(exe)
    cs, imports = engine(), Imports(image)
    label = manifest.known_builds.get(image.sha1)
    foreign = sigs_mod.foreign_patches()
    started = time.time()
    before = verify_mod.verify_exe(exe, manifest, baselines, verify_mod.SECTIONS, foreign)
    log(f"预检 verify：{before['result']}（退出码 {before['exit_code']}），{time.time() - started:.0f} 秒")
    sig_results = sigs_mod.resolve_all(image, manifest, label, foreign, cs)
    identity = before.get("identity", {})
    baseline = None
    if args.baseline:
        baseline = baselines[0] if baselines else None
    else:
        baseline = pick_baseline(baselines, identity, image, label)
    failing = [sid for sid in manifest.sigs
               if sig_results[sid].status != "ok" or identity.get(sid, {}).get("status") == "misbind"]
    ok_sigs = [sid for sid in manifest.sigs if sid not in failing]
    result = {"tool": "dd_update repair", "exe": {"path": str(exe), "sha1": image.sha1, "known": label},
              "baseline": {"path": baseline["path"] if baseline else None,
                           "label": baseline["exe"]["label"] if baseline else None,
                           "format": None},
              "manifest": {"signatures": str(paths.sigs), "layout": str(paths.layout) if paths.layout else None},
              "thresholds": thresholds(), "before": {"exit_code": before["exit_code"], "failures": before["failures"]},
              "sigs": {}, "layout": {}, "unverified": before.get("unverified", []), "rva_map": [], "patch": {}}
    if baseline is None:
        raise manifest_mod.ManifestError("没有可用的基线")
    with open(baseline["path"], encoding="utf-8") as f:
        result["baseline"]["format"] = json.load(f).get("format")

    m = locate_mod.Matcher(baseline, image, None, cs, imports, relaxed=True, log=log)
    seed_from_ok(m, image, manifest, baseline, sig_results, ok_sigs, cs, imports)
    failing_ev_nodes = seed_from_evidence(m, image, manifest, baseline, sig_results)
    direct, relaxed_info = ({}, {})
    locations = {}
    index = None
    if failing or failing_ev_nodes:
        index = m.index = FunctionIndex(image, workers=args.workers, log=log)
    if failing:
        direct, relaxed_info = relaxed_hints(m, image, manifest, baseline, failing, cs)
    if index is not None:
        targets = set(failing_ev_nodes)
        for sid in failing:
            entry = baseline["sigs"].get(sid)
            if entry:
                targets.update(sig_nodes(entry))
        m.run(sorted(targets))
        for sid in failing:
            entry = baseline["sigs"].get(sid)
            if not entry:
                loc = locate_mod.Location(sid)
                loc.reasons.append(f"基线 {Path(baseline['path']).name} 里没有这条特征码")
            else:
                loc = locate_mod.locate_sig(m, manifest.sigs[sid], entry, direct.get(sid))
            locations[sid] = loc

    # 重新生成特征码
    patches, new_results, sig_records = {}, dict(sig_results), {}
    min_rank = CONF_RANK["高" if args.min_confidence == "high" else "中"]
    for sid, sig in manifest.sigs.items():
        entry = baseline["sigs"].get(sid)
        record = {"kind": sig.kind, "required": sig.required, "need": need_class(sig), "status": "ok",
                  "confidence": None, "methods": [], "old": _sig_old(sig, entry), "new": None, "reasons": [], "notes": [],
                  "before": {"status": sig_results[sid].status, "problems": sig_results[sid].problems,
                             "identity": identity.get(sid, {}).get("status"),
                             "identity_problems": identity.get(sid, {}).get("problems", [])},
                  "relaxed": relaxed_info.get(sid), "location": None}
        sig_records[sid] = record
        if sid not in failing:
            continue
        loc = locations[sid]
        record["location"] = loc.to_json()
        record["methods"] = loc.methods
        record["notes"] += loc.notes
        if loc.status != "located":
            record["status"] = "manual"
            record["reasons"] += loc.reasons
            continue
        p, why = regen.gen_sig(image, cs, sig, loc, entry, hit_bound(manifest, sid), label, foreign,
                               make_hit_check(image, manifest, sig_results, sid, loc.resolved))
        if p is None:
            record["status"] = "manual"
            record["reasons"].append(why)
            continue
        record["notes"] += p.notes
        record["new"] = {"pattern": p.pattern, "steal": p.steal, "capture": p.capture, "entry": p.entry,
                         "hit": loc.hit, "resolved": loc.resolved, "function": loc.function}
        if CONF_RANK.get(loc.confidence, 0) < min_rank:
            record["status"] = "manual"
            record["confidence"] = loc.confidence
            record["reasons"].append(f"可信度 {loc.confidence} 低于 --min-confidence")
            continue
        record["status"] = f"repaired-{loc.confidence}"
        record["confidence"] = loc.confidence
        patches[sid] = p

    # 预演：补丁写进临时副本，跑 verify；不通过的项改成 manual 再来
    layout_fixes, values = {}, {}
    after = None
    rejected = {}   # 预演 verify 不过、已改成 manual 的布局项 -> 原因（每轮 LayoutRepair 重新算，要把结论带到下一轮）
    for attempt in range(4):
        new_results = dict(sig_results)
        for sid in failing:
            if sid in patches:
                _new_sig, res = regen.new_sig_result(image, cs, manifest.sigs[sid], patches[sid], label, foreign)
                new_results[sid] = res
            else:
                res = sigs_mod.SigResult(sid, manifest.sigs[sid].kind, manifest.sigs[sid].need)
                res.fail("没有修好")
                new_results[sid] = res
        sig_conf = {sid: ("高" if sid not in failing else sig_records[sid]["confidence"]) for sid in manifest.sigs}
        if manifest.items:
            layout_fixes, values = LayoutRepair(image, manifest, new_results, baseline, m, cs, sig_conf).run()
            for iid, fix in layout_fixes.items():
                if fix.status.startswith("repaired") and CONF_RANK.get(fix.confidence, 0) < min_rank:
                    fix.reasons.append(f"可信度 {fix.confidence} 低于 --min-confidence")
                    fix.status = "manual"
                if iid in rejected and fix.status != "manual":
                    fix.status = "manual"
                    fix.reasons += rejected[iid]
        sig_text, layout_text = build_patch(paths, manifest, patches, layout_fixes)
        after = dry_run(exe, paths, sig_text, layout_text, baselines, foreign)
        status_before = {iid: (fix.status, len(fix.reasons)) for iid, fix in layout_fixes.items()}
        changed = reconcile(after, before, manifest, patches, sig_records, layout_fixes)
        for iid, fix in layout_fixes.items():
            if fix.status == "manual" and status_before[iid][0] != "manual":
                rejected[iid] = fix.reasons[status_before[iid][1]:]
        if not changed:
            break
    else:
        # 最后一轮的预演还有修复项被拿掉：按最终结论重建补丁、再预演一次，补丁里不留 manual 的项
        sig_text, layout_text = build_patch(paths, manifest, patches, layout_fixes)
        after = dry_run(exe, paths, sig_text, layout_text, baselines, foreign)
    group_upgrade(manifest, layout_fixes, sig_records)
    result["after"] = {"exit_code": after["exit_code"], "failures": after["failures"], "result": after["result"],
                       "group_flags": after.get("group_flags", {})} if after else None
    result["sigs"] = sig_records
    result["layout"] = {iid: fix.to_json() for iid, fix in layout_fixes.items()}
    old_sig_text = paths.sigs.read_text(encoding="utf-8")
    result["patch"]["signatures"] = patch_mod.diff(old_sig_text, sig_text, paths.rel_sigs)
    if paths.layout:
        old_layout_text = paths.layout.read_text(encoding="utf-8")
        result["patch"]["layout"] = patch_mod.diff(old_layout_text, layout_text, paths.rel_layout)
    result["rva_map"] = rva_map(m, manifest, baseline, sig_records, image, cs, args, log)
    result["summary"] = summarize(result, manifest)
    result["exit_code"] = result["summary"]["exit_code"]
    result["_texts"] = (sig_text, layout_text)
    result["_index_built"] = index is not None
    return result


def group_upgrade(manifest, layout_fixes, sig_records):
    """S4：组里有 RUNTIME 项的值变了（或需要人工）、特征码被重新定位（或需要人工）时，组里 NONE、OFFLINE 项改成 manual。"""
    for group in manifest.groups.values():
        triggers = []
        for member in group.members:
            if member in manifest.items and manifest.items[member].check == "RUNTIME":
                fix = layout_fixes.get(member)
                if fix is None:
                    continue
                if fix.status.startswith("repaired") and fix.new is not None:
                    triggers.append(f"{member} {fix.old:#x} → {fix.new:#x}")
                elif fix.status == "manual":
                    triggers.append(f"{member} 需要人工")
            elif member in sig_records:
                status = sig_records[member]["status"]
                if status.startswith("repaired"):
                    triggers.append(f"特征码 {member} 重新定位")
                elif status == "manual":
                    triggers.append(f"特征码 {member} 需要人工")
        if not triggers:
            continue
        for member in group.members:
            if member not in manifest.items or manifest.items[member].check == "RUNTIME":
                continue
            fix = layout_fixes.get(member)
            if fix is None:
                continue
            why = (f"同组 {group.id}（{group.text}）变了：{verify_mod.trigger_text(triggers)}。这一项没有运行时依据，"
                   "要人工核实（审核 A 的 S4）")
            if fix.status != "manual":
                fix.status = "manual"
                fix.confidence = None
                fix.new = None
            fix.reasons.append(why)


def build_patch(paths, manifest, patches, layout_fixes):
    sig_text = paths.sigs.read_text(encoding="utf-8")
    for sid, p in patches.items():
        sig_text = patch_mod.set_sig(sig_text, manifest.sigs[sid], p.pattern, p.steal, p.capture, p.entry)
    layout_text = paths.layout.read_text(encoding="utf-8") if paths.layout else None
    if layout_text is not None:
        for iid, fix in layout_fixes.items():
            if not fix.status.startswith("repaired"):
                continue
            item = manifest.items[iid]
            if fix.new is not None:
                layout_text = patch_mod.set_item_value(layout_text, item, fix.new)
            for ev, ef in zip(item.evidence, fix.evidence):
                if ef.new is not None and not isinstance(ef.new, tuple):
                    layout_text = patch_mod.set_evidence(layout_text, ev, ef.new.pattern, ef.new.at, ef.new.read)
    return sig_text, layout_text


def dry_run(exe, paths, sig_text, layout_text, baselines, foreign):
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix="repair-", dir=TMP_DIR))
    try:
        sig_file = tmp / paths.sigs.name
        sig_file.write_text(sig_text, encoding="utf-8")
        layout_file = None
        if layout_text is not None:
            layout_file = tmp / paths.layout.name
            layout_file.write_text(layout_text, encoding="utf-8")
        manifest = manifest_mod.load(str(sig_file), str(layout_file) if layout_file else None, use_layout=layout_file is not None)
        return verify_mod.verify_exe(exe, manifest, baselines, verify_mod.SECTIONS, foreign)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def reconcile(after, before, manifest, patches, sig_records, layout_fixes):
    """预演 verify 不通过的修复项改成 manual。返回是否有变化。"""
    changed = False
    for sid in list(patches):
        problems = []
        s = after.get("sigs", {}).get(sid)
        if s and s["status"] != "ok":
            problems += s["problems"]
        ident = after.get("identity", {}).get(sid)
        if ident and ident["status"] == "misbind":
            problems += ident["problems"]
        if problems:
            patches.pop(sid)
            rec = sig_records[sid]
            rec["status"] = "manual"
            rec["reasons"].append("修复后的预演 verify 不过：" + "；".join(problems))
            changed = True
    # 依据位置核对（依据误绑定）不过：修过的项、没修的项都改成 manual；特征码的同一性依据对不上时拿掉那条特征码的补丁
    for key, r in after.get("positions", {}).items():
        if r["status"] != "disagree":
            continue
        why = f"修复后的预演 verify 依据 {r['index']} 位置核对不过（疑似依据误绑定）：{r['detail']}"
        fix = layout_fixes.get(r["item"])
        if fix is not None and fix.status not in ("manual", "unverified"):
            fix.status = "manual"
            fix.reasons.append(why)
            changed = True
        elif r["item"] in patches:
            patches.pop(r["item"])
            rec = sig_records[r["item"]]
            rec["status"] = "manual"
            rec["reasons"].append(why)
            changed = True
    # 依据位置无法确认（B1，verify 的 unconfirmed）：修过的项、没修的项都改成 manual
    for f in after.get("failures", []):
        if f["severity"] != "unconfirmed":
            continue
        why = f"修复后的预演 verify 无法确认：{f['why']}"
        fix = layout_fixes.get(f["name"])
        if fix is not None and fix.status not in ("manual", "unverified"):
            fix.status = "manual"
            fix.reasons.append(why)
            changed = True
        elif f["name"] in patches:
            patches.pop(f["name"])
            rec = sig_records[f["name"]]
            rec["status"] = "manual"
            rec["reasons"].append(why)
            changed = True
    layout_after = after.get("layout", {})
    for iid, fix in layout_fixes.items():
        r = layout_after.get(iid)
        if r is None or r["status"] != "fail":
            continue
        if fix.status.startswith("repaired"):
            fix.status = "manual"
            fix.reasons.append("修复后的预演 verify 不过：" + "；".join(r["problems"]))
            changed = True
            continue
        # 原来成立、修了特征码之后不成立：怪到依据用的那条修过的特征码上
        if before.get("layout", {}).get(iid, {}).get("status") != "ok":
            continue
        for ev in manifest.items[iid].evidence:
            if ev.sig in patches:
                patches.pop(ev.sig)
                rec = sig_records[ev.sig]
                rec["status"] = "manual"
                rec["reasons"].append(f"修复后 {iid} 的依据不成立：" + "；".join(r["problems"]))
                changed = True
    return changed


def thresholds():
    return {"sim_gate": locate_mod.SIM_GATE, "margin": locate_mod.MARGIN, "rel_gate": locate_mod.REL_GATE,
            "discriminate_gap": locate_mod.DISCRIMINATE_GAP, "anchor_max": locate_mod.ANCHOR_MAX,
            "imm_max": locate_mod.IMM_MAX, "relaxed_max_hits": locate_mod.RELAXED_MAX_HITS,
            "min_insns": regen.MIN_INSNS, "min_fixed": regen.MIN_FIXED, "max_bytes": regen.MAX_BYTES,
            "ev_min_insns": regen.EV_MIN_INSNS, "ev_min_fixed": regen.EV_MIN_FIXED,
            "align_window": align.MAX_WINDOW}


def summarize(result, manifest):
    counts = {}
    manual_required = manual_optional = 0
    for sid, r in result["sigs"].items():
        counts.setdefault("sigs", {}).setdefault(r["status"], 0)
        counts["sigs"][r["status"]] += 1
        if r["status"] == "manual":
            if r["need"] == "REQUIRED":
                manual_required += 1
            else:
                manual_optional += 1
    for iid, r in result["layout"].items():
        counts.setdefault("layout", {}).setdefault(r["status"], 0)
        counts["layout"][r["status"]] += 1
        if r["status"] == "manual":
            if r["need"] == "REQUIRED":
                manual_required += 1
            else:
                manual_optional += 1
    code = EXIT_REQUIRED if manual_required else (EXIT_OPTIONAL if manual_optional else EXIT_OK)
    return {"counts": counts, "manual_required": manual_required, "manual_optional": manual_optional, "exit_code": code}


# ---- RVA 对照表 ----

def rva_map(m, manifest, baseline, sig_records, image, cs, args, log):
    rows = {}

    def add(kind, old, new, confidence, method, name):
        if old is None or new is None:
            return
        key = (kind, old)
        if key in rows:
            if name not in rows[key]["names"]:
                rows[key]["names"].append(name)
            return
        rows[key] = {"kind": kind, "old": old, "new": new, "confidence": confidence, "method": method, "names": [name]}

    for old, d in sorted(m.matches.items()):
        names = [f"{sid}:{role}" for sid, role in m.nodes[old].roles]
        add("function", old, d.new, d.confidence, "；".join(d.methods), names[0] if names else "")
        for n in names[1:]:
            add("function", old, d.new, d.confidence, "", n)
    for sid, r in sig_records.items():
        entry = baseline["sigs"].get(sid)
        if not entry or entry["section"] not in (".data", ".rdata") or manifest.sigs[sid].runtime_select:
            continue
        if r["status"] == "ok":
            add("global", entry["resolved"], m.globals.get(entry["resolved"]), "高", "特征码校验通过", sid)
        elif r["status"].startswith("repaired") and r["new"]:
            add("global", entry["resolved"], r["new"]["resolved"], r["confidence"], "修复", sid)
    # 只有一个调用者、新旧都只有一个时，调用者也对应
    if m.index is not None:
        for old, d in sorted(m.matches.items()):
            node = m.nodes[old]
            if node.base_callers and len(node.base_callers) == 1:
                now = m.index.callers_of.get(d.new, [])
                if len(now) == 1:
                    add("function", node.base_callers[0], now[0], "中", "唯一调用者", f"{node.roles[0][0]} 的调用者")
    if getattr(args, "old_exe", None):
        expand_with_old(rows, args.old_exe, image, m, args, log)
    return sorted(rows.values(), key=lambda r: (r["kind"], r["old"]))


def expand_with_old(rows, old_exe, image, m, args, log):
    """有旧 exe 时沿调用图把对应关系往外扩：已对应的函数对，按指令对齐配对直接调用的目标、唯一的调用者，
    两边的指纹相似度要过门槛，冲突的丢弃。"""
    old_image = Image(old_exe)
    old_index = FunctionIndex(old_image, workers=getattr(args, "workers", None), log=log)
    new_index = m.index or FunctionIndex(image, workers=getattr(args, "workers", None), log=log)
    cs = engine()
    mapped = {r["old"]: r["new"] for r in rows.values() if r["kind"] == "function"}
    reverse = {v: k for k, v in mapped.items()}
    bad = set()
    queue = sorted(mapped.items())
    added = 0
    limit = getattr(args, "expand_limit", None) or EXPAND_LIMIT

    def sim(o, n):
        a, b = old_index.funcs.get(o), new_index.funcs.get(n)
        if a is None or b is None:
            return 0.0
        return dice(shingle_ids(new_index.ids([old_index.vocab[k] for k in a.tokens])), shingle_ids(b.tokens))

    def calls(img, rva):
        _, insns, _ = locate_mod.decode_function(img, rva, cs)
        toks = [locate_mod.token(i) for i in insns]
        targets = {}
        for i, ins in enumerate(insns):
            if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == locate_mod.X86_OP_IMM:
                targets[i] = ins.operands[0].imm
        return toks, targets

    while queue and added < limit:
        o, n = queue.pop(0)
        if o not in old_index.funcs or n not in new_index.funcs:
            continue
        otoks, ocalls = calls(old_image, o)
        ntoks, ncalls = calls(image, n)
        aligner = align.Aligner(otoks, ntoks)
        pairs = []
        for i, t in ocalls.items():
            j, _size = aligner.by_blocks(i)
            if j is not None and j in ncalls:
                pairs.append((t, ncalls[j]))
        oc, nc = old_index.callers_of.get(o, []), new_index.callers_of.get(n, [])
        if len(oc) == 1 and len(nc) == 1:
            pairs.append((oc[0], nc[0]))
        for t, t2 in pairs:
            if t in bad or t2 in bad:
                continue
            if t in mapped:
                if mapped[t] != t2:
                    bad.update((t, t2))
                continue
            if t2 in reverse:
                if reverse[t2] != t:
                    bad.update((t, t2))
                continue
            if sim(t, t2) < locate_mod.SIM_GATE:
                continue
            mapped[t], reverse[t2] = t2, t
            rows[("function", t)] = {"kind": "function", "old": t, "new": t2, "confidence": "中", "method": "调用图扩展",
                                     "names": [f"由 {o:X} 扩展"]}
            queue.append((t, t2))
            added += 1
    for key in [k for k, r in rows.items() if r["method"] == "调用图扩展" and (r["old"] in bad or r["new"] in bad)]:
        rows.pop(key)
    log(f"RVA 对照表：调用图扩展 {added} 个函数，冲突丢弃 {len(bad) // 2}")


# ---- 输出 ----

def _h(v):
    return "-" if v is None else f"{v:X}"


def write_outputs(result, out_dir, out):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    data = {k: v for k, v in result.items() if not k.startswith("_")}
    (out_dir / "report.json").write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    (out_dir / "signatures.diff").write_text(result["patch"].get("signatures", ""), encoding="utf-8")
    (out_dir / "layout.diff").write_text(result["patch"].get("layout") or "", encoding="utf-8")
    with open(out_dir / "rva_map.tsv", "w", encoding="utf-8") as f:
        f.write("kind\told\tnew\tconfidence\tmethod\tnames\n")
        for r in result["rva_map"]:
            f.write(f"{r['kind']}\t{r['old']:X}\t{r['new']:X}\t{r['confidence']}\t{r['method']}\t{', '.join(r['names'])}\n")
    text = render(result)
    (out_dir / "report.txt").write_text(text, encoding="utf-8")
    out.write(text)
    out.write(f"输出：{out_dir}（report.txt、report.json、signatures.diff、layout.diff、rva_map.tsv）\n")


def render(result):
    lines = []
    w = lines.append
    exe = result["exe"]
    w(f"== repair {exe['path']}")
    w(f"   sha1={exe['sha1']} {'已知版本 ' + exe['known'] if exe['known'] else '未知版本'}；"
      f"基线 {Path(result['baseline']['path']).name}（format {result['baseline']['format']}）")
    w(f"   修复前 verify：退出码 {result['before']['exit_code']}，失效 {len(result['before']['failures'])} 项")
    w(" 特征码")
    for sid, r in result["sigs"].items():
        if r["status"] == "ok":
            continue
        tag = {"repaired-高": "FIX高", "repaired-中": "FIX中", "manual": "MANU"}.get(r["status"], r["status"])
        before = "；".join(r["before"]["problems"] + r["before"]["identity_problems"]) or r["before"]["status"]
        w(f"  {tag} {sid:<24} [{r['need']}] 修复前：{before}")
        if r["methods"]:
            w(f"        手段：{'；'.join(r['methods'])}")
        if r["new"]:
            n = r["new"]
            w(f"        新位置：命中 {_h(n['hit'])} 解析 {_h(n['resolved'])}；DD_SIG steal={n['steal']} capture={n['capture']} \"{n['pattern']}\""
              + (f"；DD_ENTRY \"{n['entry']}\"" if n["entry"] else ""))
        for note in r["notes"]:
            w(f"        {note}")
        for reason in r["reasons"]:
            w(f"        原因：{reason}")
        loc = r.get("location") or {}
        dec = loc.get("decision") or {}
        if r["status"] == "manual" and dec.get("candidates"):
            w("        候选：" + "；".join(f"{c['rva']:X} 相似度 {c['sim']} 约束 {c['rel_n']} 字符串 {c['strings']} "
                                    f"[{','.join(c['methods'])}]{'' if c['gated'] else '（' + c['gate_fail'] + '）'}"
                                    for c in dec["candidates"][:5]))
    ok_sigs = sum(r["status"] == "ok" for r in result["sigs"].values())
    w(f"  其余 {ok_sigs} 条特征码校验通过，不用修")
    if result["layout"]:
        w(" 布局")
        for iid, r in result["layout"].items():
            if r["status"] in ("ok", "unverified"):
                continue
            tag = {"repaired-高": "FIX高", "repaired-中": "FIX中", "manual": "MANU"}.get(r["status"], r["status"])
            new = f" → {r['new']:#x}" if r["new"] is not None else ""
            w(f"  {tag} {iid:<28} [{r['need']}] {r['old']:#x}{new}")
            for e in r["evidence"]:
                if e["method"] == "依据成立" and "pattern" not in e:
                    continue
                extra = f"；新模式 \"{e['pattern']}\" at={e['at']} {e['read']}" if "pattern" in e else ""
                w(f"        依据 {e['index']}：{e['method']}" + (f"（{e['detail']}）" if e["detail"] else "") + extra)
            for o in r["offline"]:
                w(f"        离线 {o['name']}：{'通过' if o['ok'] else '不通过'} {o['detail']}")
            for note in r["notes"]:
                w(f"        {note}")
            for reason in r["reasons"]:
                w(f"        原因：{reason}")
        ok_items = sum(r["status"] == "ok" for r in result["layout"].values())
        w(f"  其余 {ok_items} 项依据成立，不用修")
    if result["unverified"]:
        w(f" 更新后要人工核实（没有静态依据，修复工具不修）：{len(result['unverified'])} 项，见 verify 报告的分组")
    if result.get("after"):
        w(f" 预演 verify（补丁应用到临时副本）：{result['after']['result']}（退出码 {result['after']['exit_code']}）")
        for f in result["after"]["failures"]:
            w(f"   - {f['name']}（{f['severity']}）：{f['why']}")
    s = result["summary"]
    counts = s["counts"]
    w(" 统计：特征码 " + "、".join(f"{k} {v}" for k, v in sorted(counts.get("sigs", {}).items()))
      + "；布局 " + "、".join(f"{k} {v}" for k, v in sorted(counts.get("layout", {}).items())))
    w(f" RVA 对照表 {len(result['rva_map'])} 行（函数 {sum(r['kind'] == 'function' for r in result['rva_map'])}、"
      f"全局 {sum(r['kind'] == 'global' for r in result['rva_map'])}）")
    w(f" 结论：需要人工 必需 {s['manual_required']}、可选 {s['manual_optional']}（退出码 {s['exit_code']}）")
    return "\n".join(lines) + "\n"


def cmd_repair(args, out):
    paths = Paths(args)
    try:
        baselines = paths.baselines(args.baseline)
    except (OSError, ValueError) as e:
        out.write(f"基线读不了：{e}\n")
        return EXIT_ERROR
    if not Path(args.exe).is_file():
        out.write(f"exe 不存在：{args.exe}\n")
        return EXIT_ERROR
    log = (lambda text: out.write(f"[repair] {text}\n"))
    try:
        result = run_repair(args.exe, paths, baselines, args, log)
    except manifest_mod.ManifestError as e:
        out.write(f"清单解析失败：{e}\n")
        return EXIT_ERROR
    out_dir = Path(args.out) if args.out else REPAIR_DIR / result["exe"]["sha1"][:12]
    write_outputs(result, out_dir, out)
    code = result["exit_code"]
    if args.apply:
        sig_text, layout_text = result["_texts"]
        patch_mod.write_keep_newlines(paths.sigs, sig_text)
        if layout_text is not None and paths.layout:
            patch_mod.write_keep_newlines(paths.layout, layout_text)
        out.write(f"已写入 {paths.sigs}" + (f"、{paths.layout}" if paths.layout else "") + "，重新 verify：\n")
        manifest = paths.load()
        report = verify_mod.verify_exe(args.exe, manifest, baselines, verify_mod.SECTIONS, sigs_mod.foreign_patches())
        verify_mod.print_report(report, manifest, out)
        out.write("（--apply 不把版本记为已知；人工确认后运行 accept）\n")
    return code


# ---- accept ----

CONFIRM_DIR = "confirm"


def accept_gate(report, manifest):
    """按 accept 的规则（见文件开头）给 verify 的结论分类。返回 (一律拒绝的, --allow-optional 才接受的, 要人工确认的)：
    前两个是 [(名字, 原因)]，最后一个是 {名字: [原因, ...]}。"""
    blocking, runtime_only, confirm = [], [], {}
    for f in report["failures"]:
        name, sev, why = f["name"], f["severity"], f["why"]
        if sev == "unconfirmed":
            confirm.setdefault(name, []).append(f"依据位置无法确认：{why}")
        elif sev == "required":
            blocking.append((name, f"必需项失效：{why}"))
        elif sev == "misbind":
            blocking.append((name, f"误绑定：{why}"))
        elif name in manifest.sigs:
            r = report["sigs"].get(name, {})
            if r.get("runtime_detectable"):
                runtime_only.append((name, f"可选特征码失效（运行时同样判失效、停用依赖它的能力）：{why}"))
            else:
                blocking.append((name, f"特征码只有 verify 做的静态检查不过（运行时照样会用它）：{why}"))
        else:
            item = manifest.items[name]
            rec = report.get("layout", {}).get(name, {})
            offline_bad = [o for o in rec.get("offline", []) if not o["ok"]]
            if item.check == "RUNTIME" and not offline_bad:
                runtime_only.append((name, f"可选 RUNTIME 项的运行时依据不成立（运行时同样判不成立、停用依赖它的能力）：{why}"))
            else:
                blocking.append((name, f"离线依据不过（运行时核对不了，已知版本上不会停用）：{why}"))
    for iid, item in manifest.items.items():
        if item.check == "NONE":
            confirm.setdefault(iid, []).append(f"没有静态依据：{item.unverified}")
    for iid, why in report.get("group_flags", {}).items():
        confirm.setdefault(iid, []).extend(why)
    return blocking, runtime_only, confirm


def _evidence_paths(text):
    """证据路径：分号分隔，相对工作区根目录（guest 的 D:/workplace）、DD.ModLoader，或绝对路径。返回 [(路径, 是否存在)]。"""
    out = []
    for part in [x.strip() for x in str(text).split(";") if x.strip()]:
        found = any(p.exists() for p in (Path(part), ROOT / part, PROJECT / part))
        out.append((part, found))
    return out


def confirm_template(report, manifest, image, label, confirm):
    return {"tool": "dd_update accept 的确认清单（U1 修复 FB，审核 A 的 S4、B1）", "sha1": image.sha1, "label": label,
            "说明": "每项写 method（怎么核实的：探针、test_events、对照反汇编……）和 evidence（证据的路径，分号分隔，相对 "
                    "D:\\workplace 或 DD.ModLoader，或绝对路径，accept 时要存在）。why 是这一项为什么要确认，不用改。"
                    "全部填好后 accept --confirm 这个文件；接受时它复制到基线目录的 confirm/<版本名>.json 进仓库",
            "items": {iid: {"why": why, "method": "", "evidence": ""} for iid, why in sorted(confirm.items())}}


def check_confirm(path, image, confirm):
    """读确认清单并核对。返回 (清单 dict, 问题列表)。"""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return None, [f"确认清单读不了：{e}"]
    problems = []
    if str(data.get("sha1", "")).lower() != image.sha1:
        problems.append(f"确认清单的 sha1 {data.get('sha1')} 不是这个 exe 的 {image.sha1}")
    items = data.get("items") or {}
    for iid in sorted(confirm):
        entry = items.get(iid)
        if not isinstance(entry, dict):
            problems.append(f"{iid}：清单里没有这一项")
            continue
        if not str(entry.get("method", "")).strip():
            problems.append(f"{iid}：没写 method（怎么核实的）")
        paths = _evidence_paths(entry.get("evidence", ""))
        if not paths:
            problems.append(f"{iid}：没写 evidence（证据路径）")
        for part, found in paths:
            if not found:
                problems.append(f"{iid}：证据路径不存在：{part}")
    return data, problems


def cmd_accept(args, out):
    paths = Paths(args)
    manifest = paths.load()
    baselines = paths.baselines(args.baseline)
    image = Image(args.exe)
    report = verify_mod.verify_exe(args.exe, manifest, baselines, verify_mod.SECTIONS, sigs_mod.foreign_patches())
    verify_mod.print_report(report, manifest, out)
    label = args.label or manifest.known_builds.get(image.sha1)
    blocking, runtime_only, confirm = accept_gate(report, manifest)
    template = getattr(args, "confirm_template", None)
    if template:
        data = confirm_template(report, manifest, image, label, confirm)
        Path(template).parent.mkdir(parents=True, exist_ok=True)
        Path(template).write_text(json.dumps(data, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        out.write(f"确认清单模板：{template}（{len(confirm)} 项要填 method 和 evidence）；没有接受\n")
        if blocking or runtime_only:
            out.write(f"注意：另有 {len(blocking)} 项一律拒绝、{len(runtime_only)} 项要 --allow-optional，见下面 accept 的结论\n")
        return EXIT_OK
    if blocking:
        out.write(f"不接受：{len(blocking)} 项失效运行时发现不了，或者是必需项、误绑定，要先修（repair，或人工改清单），"
                  "--allow-optional 和 --confirm 都不管用：\n")
        out.write("".join(f"  {name}：{why}\n" for name, why in blocking))
        return EXIT_REQUIRED
    if runtime_only and not args.allow_optional:
        out.write(f"不接受：{len(runtime_only)} 项失效，都是运行时自己会发现、停用依赖它们的能力的（可选特征码、可选 RUNTIME 项的"
                  "运行时依据）。修好再 accept；确认要带着它们记为已知版本时加 --allow-optional：\n")
        out.write("".join(f"  {name}：{why}\n" for name, why in runtime_only))
        return EXIT_REQUIRED
    record = None
    if confirm:
        if not args.confirm:
            unconfirmed = sorted(n for n, why in confirm.items() if any(w.startswith("依据位置无法确认") for w in why))
            grouped = sorted(n for n in report.get("group_flags", {}))
            out.write(f"不接受：{len(confirm)} 项运行时核对不了，要逐项人工确认（--confirm <清单>）：NONE 项 "
                      f"{sum(1 for i in manifest.items.values() if i.check == 'NONE')} 个，同组升级 {len(grouped)} 个"
                      + (f"（{', '.join(grouped[:12])}{' …' if len(grouped) > 12 else ''}）" if grouped else "")
                      + f"，依据位置无法确认 {len(unconfirmed)} 个"
                      + (f"（{', '.join(unconfirmed)}）" if unconfirmed else "")
                      + "。先用 --confirm-template <文件> 生成清单，逐项核实、填好 method 和 evidence\n")
            return EXIT_REQUIRED
        record, problems = check_confirm(args.confirm, image, confirm)
        if problems:
            out.write(f"不接受：确认清单 {args.confirm} 有 {len(problems)} 个问题：\n" + "".join(f"  {p}\n" for p in problems[:40])
                      + (f"  ……另有 {len(problems) - 40} 个\n" if len(problems) > 40 else ""))
            return EXIT_REQUIRED
    if not label:
        out.write("未知版本要用 --label 给出版本名\n")
        return EXIT_ERROR
    text = paths.sigs.read_text(encoding="utf-8")
    new_text = text
    if args.replace:
        lines = [ln for ln in new_text.splitlines(keepends=True)
                 if not (ln.startswith("DD_KNOWN_BUILD(") and f'"{label}"' in ln and image.sha1 not in ln)]
        new_text = "".join(lines)
    if image.sha1 not in manifest.known_builds:
        new_text = patch_mod.add_known_build(new_text, image.sha1, label)
    if new_text != text:
        patch_mod.write_keep_newlines(paths.sigs, new_text)
        out.write(patch_mod.diff(text, new_text, paths.rel_sigs))
    else:
        out.write(f"{image.sha1} 已经是已知版本 {label}\n")
    manifest = paths.load()
    label = manifest.known_builds.get(image.sha1)
    sig_results = sigs_mod.resolve_all(image, manifest, label, sigs_mod.foreign_patches())
    data = baseline_mod.generate(image, manifest, sig_results, label)
    out_dir = Path(args.out) if args.out else paths.baseline_dir
    target = out_dir / f"{label}.json"
    baseline_mod.write(data, target)
    out.write(f"基线：{target}（{target.stat().st_size} 字节）\n")
    if record is not None:
        record = dict(record)
        record["label"] = label
        record["accepted"] = {"date": datetime.date.today().isoformat(), "allow_optional": bool(args.allow_optional),
                              "runtime_only_failures": [name for name, _why in runtime_only],
                              "items": len(confirm)}
        confirm_path = out_dir / CONFIRM_DIR / f"{label}.json"
        confirm_path.parent.mkdir(parents=True, exist_ok=True)
        confirm_path.write_text(json.dumps(record, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        out.write(f"确认记录：{confirm_path}（{len(confirm)} 项，和基线、DD_KNOWN_BUILD 一起进仓库）\n")
    return EXIT_OK


# ---- 盲定位 ----

def blind_compare(baseline, exe, manifest, workers=None, log=None, regenerate=True):
    """以 baseline 为准，不用现有特征码，在 exe 里定位全部特征码项，和现有特征码的解析结果比较。返回结果 dict。"""
    log = log or (lambda text: None)
    image = exe if isinstance(exe, Image) else Image(exe)
    cs, imports = engine(), Imports(image)
    index = FunctionIndex(image, workers=workers, log=log)
    started = time.time()
    m = locate_mod.Matcher(baseline, image, index, cs, imports, relaxed=False, log=log)
    m.run(m.node_keys)
    label = manifest.known_builds.get(image.sha1)
    existing = sigs_mod.resolve_all(image, manifest, label, [], cs)
    rows = {}
    stats = {}
    for sid, sig in manifest.sigs.items():
        entry = baseline["sigs"].get(sid)
        row = {"need": need_class(sig), "kind": sig.kind, "verdict": None, "located": None, "expected": None,
               "confidence": None, "methods": [], "reasons": [], "notes": [], "regen": None, "sim": None, "second": None}
        rows[sid] = row
        ex = existing[sid]
        row["expected"] = ex.resolved if ex.status == "ok" else None
        if not entry or entry["status"] != "ok":
            row["verdict"] = "skip"
            row["reasons"].append("基线里这条特征码失效或不存在")
            continue
        loc = locate_mod.locate_sig(m, sig, entry)
        d = loc.decision
        if d is not None:
            row["sim"], row["second"], row["runner"] = d.sim, d.second, d.runner
            row["candidates"] = [c.to_json() for c in d.candidates[:5]]
        row.update(confidence=loc.confidence, methods=loc.methods, reasons=list(loc.reasons), notes=list(loc.notes),
                   located=loc.resolved if loc.status == "located" else None)
        if loc.status != "located":
            row["verdict"] = "manual"
        elif ex.status != "ok":
            row["verdict"] = "manual"
            row["reasons"].append("现有特征码在这个 exe 上本身失效，没有参照")
        elif sig.runtime_select:
            # 按内容选的特征码：定位结果要等于现有特征码离线解析出的那个（离线选不出时是第一个命中的候选，Steam 版和
            # 离线版都正好是 Query 表），重新生成的模式的候选要是现有候选的子集
            if loc.resolved not in ex.candidates:
                row["verdict"] = "wrong"
            elif loc.resolved != ex.resolved:
                row["verdict"] = "manual"
                row["reasons"].append("定位结果在现有候选里，但不是现有特征码离线解析出的那个")
            else:
                row["verdict"] = "pending"
        else:
            row["verdict"] = "correct" if loc.resolved == ex.resolved else "wrong"
        if loc.status == "located" and regenerate:
            p, why = regen.gen_sig(image, cs, sig, loc, entry, hit_bound(manifest, sid),
                                   hit_check=make_hit_check(image, manifest, existing, sid, loc.resolved))
            row["regen"] = {"ok": p is not None, "pattern": p.pattern if p else None, "steal": p.steal if p else None,
                            "capture": p.capture if p else None, "entry": p.entry if p else None, "why": why}
            if sig.runtime_select and row["verdict"] == "pending":
                if p is not None:
                    _s, res = regen.new_sig_result(image, cs, sig, p)
                    subset = set(res.candidates) <= set(ex.candidates)
                    row["verdict"] = "correct" if subset else "manual"
                    row["notes"].append(f"重新生成的模式候选 {len(res.candidates)} 个，现有 {len(ex.candidates)} 个")
                    if not subset:
                        row["reasons"].append("重新生成的模式的候选不是现有候选的子集")
                else:
                    row["verdict"] = "manual"
        if row["verdict"] == "pending":
            row["verdict"] = "manual"
        cls = stats.setdefault(row["need"], {"correct": 0, "manual": 0, "wrong": 0, "skip": 0})
        cls[row["verdict"]] += 1
    total = {"correct": 0, "manual": 0, "wrong": 0, "skip": 0}
    for cls in stats.values():
        for k, v in cls.items():
            total[k] += v
    truth = function_truth(image, manifest, baseline, existing, cs, imports)
    decisions = []
    for old in sorted(m.final):
        d = m.final[old]
        expect = truth.get(old)
        verdict = "manual" if d.status != "matched" else ("unknown" if expect is None else
                                                          ("correct" if d.new == expect else "wrong"))
        decisions.append({"old": old, "new": d.new, "expected": expect, "verdict": verdict, "sim": d.sim,
                          "second": d.second, "runner": d.runner, "confidence": d.confidence, "methods": d.methods,
                          "notes": d.notes, "reasons": d.reasons, "roles": [f"{a}:{b}" for a, b in m.nodes[old].roles]})
    functions = {"nodes": len(m.nodes), "matched": sum(d.status == "matched" for d in m.final.values()),
                 "manual": sum(d.status == "manual" for d in m.final.values()),
                 "correct": sum(x["verdict"] == "correct" for x in decisions),
                 "wrong": sum(x["verdict"] == "wrong" for x in decisions),
                 "unknown": sum(x["verdict"] == "unknown" for x in decisions)}
    if functions["wrong"]:
        total["wrong_functions"] = functions["wrong"]
    return {"baseline": baseline["path"], "baseline_label": baseline["exe"]["label"], "exe": image.path, "sha1": image.sha1,
            "rows": rows, "stats": stats, "total": total, "functions": functions, "decisions": decisions,
            "regen_ok": sum(1 for r in rows.values() if r["regen"] and r["regen"]["ok"]),
            "regen_fail": sum(1 for r in rows.values() if r["regen"] and not r["regen"]["ok"]),
            "seconds": round(time.time() - started, 1), "thresholds": thresholds(),
            "exit_code": EXIT_REQUIRED if total["wrong"] or functions["wrong"] else EXIT_OK}


def function_truth(image, manifest, baseline, existing, cs, imports):
    """盲定位的函数级参照：现有特征码在这个 exe 上解析出的函数、目标函数、thunk 目标，和依据命中所在的函数。"""
    truth = {}
    for sid, entry in baseline["sigs"].items():
        sig, res = manifest.sigs.get(sid), existing.get(sid)
        if not sig or not res or res.status != "ok" or not entry.get("function") or sig.kind == "GLOBAL_ANY":
            continue
        fn = entry["function"]
        anchor = res.resolved if sig.kind in ("HOOK", "CODE") else res.hit
        new_fn = new_function_of(image, fn, anchor)
        truth.setdefault(fn["rva"], new_fn)
        if fn.get("thunk") and new_fn is not None:
            now = fingerprint(image, new_fn, cs, imports)
            if now.get("thunk"):
                truth.setdefault(fn["thunk"]["rva"], now["thunk"]["rva"])
        if entry.get("target_function"):
            truth.setdefault(entry["target_function"]["rva"], res.resolved)
    extra = {fp["rva"] for fp in baseline.get("functions", [])}
    records = {(r["item"], r["index"]): r for r in baseline["evidence"]}
    for ev in manifest.evidence:
        rec = records.get((ev.item, ev.index))
        if not rec or rec.get("function") not in extra:
            continue
        er = evidence_mod.evaluate(image, manifest, existing, ev)
        if er.status == "ok" and er.hit is not None:
            truth.setdefault(rec["function"], image.enclosing_function(er.hit))
    return truth


def render_blind(res):
    lines = []
    w = lines.append
    w(f"== 盲定位：基线 {Path(res['baseline']).name} → {res['exe']}")
    f = res["functions"]
    w(f"   函数节点 {f['nodes']}：匹配 {f['matched']}（和现有特征码、依据给出的函数比较：正确 {f['correct']}、错误 {f['wrong']}、"
      f"没有参照 {f['unknown']}），需要人工 {f['manual']}；{res['seconds']} 秒")
    for x in res["decisions"]:
        if x["verdict"] in ("wrong", "manual"):
            w(f"   函数 {x['old']:X}（{', '.join(x['roles'])}）：{'错误，选了 ' + _h(x['new']) + ' 参照 ' + _h(x['expected']) if x['verdict'] == 'wrong' else '需要人工'}"
              + (f"；{'；'.join(x['reasons'])}" if x["reasons"] else ""))
    for sid, r in res["rows"].items():
        tag = {"correct": "OK  ", "manual": "MANU", "wrong": "ERR!", "skip": "SKIP"}[r["verdict"]]
        sim = f"{r['sim']:.3f}" if r["sim"] is not None else "-"
        second = f"{r['second']:.3f}" if r["second"] is not None else "-"
        runner = f"{r['runner']:.3f}" if r.get("runner") is not None else "-"
        text = f"  {tag} {sid:<24} [{r['need']:<8}] 定位 {_h(r['located'])} 现有 {_h(r['expected'])} 可信度 {r['confidence'] or '-'} " \
               f"相似度 {sim} 过门槛的第二名 {second} 最近冒充者 {runner}"
        if r["methods"]:
            text += f" 手段 {'；'.join(r['methods'])}"
        w(text)
        for reason in r["reasons"]:
            w(f"         原因：{reason}")
        if r["regen"] and not r["regen"]["ok"]:
            w(f"         重新生成特征码失败：{r['regen']['why']}")
    w("   统计（正确 / 需要人工 / 错误）：")
    for cls in ("REQUIRED", "OPT", "ANCHOR"):
        s = res["stats"].get(cls)
        if s:
            w(f"     {cls:<8} {s['correct']} / {s['manual']} / {s['wrong']}" + (f"（跳过 {s['skip']}）" if s["skip"] else ""))
    t = res["total"]
    w(f"     合计     {t['correct']} / {t['manual']} / {t['wrong']}；重新生成特征码成功 {res['regen_ok']}、失败 {res['regen_fail']}")
    w(f"   结论：{'错误为 0' if not t['wrong'] else '有错误定位'}（退出码 {res['exit_code']}）")
    return "\n".join(lines) + "\n"


def cmd_blind(args, out):
    paths = Paths(args)
    manifest = paths.load()
    pairs = []
    if args.baseline and args.exe:
        pairs.append((args.baseline, args.exe))
    else:
        exes = dict(verify_mod.DEFAULT_EXES)
        files = {p.stem: p for p in paths.baseline_dir.glob("*.json")}
        if "steam" in files and "nosteam" in files:
            pairs = [(files["steam"], exes["nosteam"]), (files["nosteam"], exes["steam"])]
        else:
            out.write("没有 steam.json 和 nosteam.json，要用 --baseline 和 --exe 指定\n")
            return EXIT_ERROR
    log = (lambda text: out.write(f"[blind] {text}\n"))
    results = []
    for bpath, exe in pairs:
        baseline = baseline_mod.load(bpath)
        res = blind_compare(baseline, exe, manifest, workers=args.workers, log=log)
        out.write(render_blind(res))
        results.append(res)
    code = max(r["exit_code"] for r in results)
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({"tool": "dd_update blind", "results": results, "exit_code": code},
                                              ensure_ascii=False, indent=1), encoding="utf-8")
        out.write(f"JSON：{args.json}\n")
    return code


# ---- 给 selftest（U1d）的函数接口 ----

def repair_exe(exe, tree=None, sigs=None, layout=None, baselines=None, apply=False, out=None, min_confidence="medium",
               old_exe=None, workers=None, log=None):
    """和 repair 命令相同，返回 (结果 dict, apply 之后的 verify 报告或 None)。tree 是源码目录的副本（清单在
    <tree>/core/src/game，基线默认 <tree>/tools/baseline）；baselines 是基线文件路径列表。结果的 exit_code、
    summary、sigs[id].status、layout[id].status、patch、rva_map 见 report.json。"""
    import argparse
    ns = argparse.Namespace(exe=str(exe), tree=tree, sigs=sigs, layout=layout, baseline=baselines, apply=apply, out=out,
                            min_confidence=min_confidence, old_exe=old_exe, expand_limit=EXPAND_LIMIT, workers=workers)
    paths = Paths(ns)
    log = log or (lambda text: None)
    result = run_repair(str(exe), paths, paths.baselines(baselines), ns, log)
    if out:
        import io
        write_outputs(result, out, io.StringIO())
    report = None
    if apply:
        sig_text, layout_text = result["_texts"]
        patch_mod.write_keep_newlines(paths.sigs, sig_text)
        if layout_text is not None and paths.layout:
            patch_mod.write_keep_newlines(paths.layout, layout_text)
        report = verify_mod.verify_exe(str(exe), paths.load(), paths.baselines(baselines), verify_mod.SECTIONS,
                                       sigs_mod.foreign_patches())
    return result, report


def blind_both(baseline_dir=None, manifest=None, workers=None, log=None):
    """盲定位的两个方向（steam.json → 离线版、nosteam.json → Steam 版），返回结果列表；任一方向 exit_code 非 0 即失败。"""
    baseline_dir = Path(baseline_dir) if baseline_dir else baseline_mod.BASELINE_DIR
    manifest = manifest or manifest_mod.load()
    exes = dict(verify_mod.DEFAULT_EXES)
    out = []
    for label, other in (("steam", "nosteam"), ("nosteam", "steam")):
        baseline = baseline_mod.load(baseline_dir / f"{label}.json")
        out.append(blind_compare(baseline, exes[other], manifest, workers=workers, log=log))
    return out


# ---- 命令行 ----

def add_parsers(sub):
    p = sub.add_parser("repair", help="定位失效项、重新生成特征码和偏移，输出补丁和 RVA 对照表")
    p.add_argument("--exe", required=True, help="新的 Darkest.exe")
    p.add_argument("--baseline", action="append", help="基线文件（默认按同一性检查挑最像的那个）")
    p.add_argument("--sigs", help="特征码文件（插件作者的，同 signatures.inc 的格式），这时默认不读布局清单")
    p.add_argument("--layout", help="布局清单")
    p.add_argument("--tree", help="源码目录（或它的副本）：清单从 <tree>/core/src/game 读写，基线从 <tree>/tools/baseline 读")
    p.add_argument("--apply", action="store_true", help="把补丁写进清单文件（--tree 时写进副本），再跑一遍 verify")
    p.add_argument("--out", help="报告目录，默认 DD.ModLoader/build/repair/<sha1 前 12 位>")
    p.add_argument("--min-confidence", choices=("high", "medium"), default="medium",
                   help="低于这个可信度的修复按需要人工处理（默认 medium：高和中都进补丁）")
    p.add_argument("--old-exe", help="旧版本的 exe（还在的话），RVA 对照表沿调用图往外扩")
    p.add_argument("--expand-limit", type=int, default=EXPAND_LIMIT)
    p.add_argument("--workers", type=int, help="生成函数索引的进程数（默认 CPU 数，最多 16）")

    p = sub.add_parser("accept", help="verify 通过、人工确认后把版本加进 DD_KNOWN_BUILD，生成这个版本的基线")
    p.add_argument("--exe", required=True)
    p.add_argument("--label", help="版本名（未知版本必填），也是基线文件名")
    p.add_argument("--replace", action="store_true", help="删掉同名的旧版本 DD_KNOWN_BUILD")
    p.add_argument("--allow-optional", action="store_true",
                   help="只放行运行时自己会发现并停用的失效（可选特征码失效、可选 RUNTIME 项的运行时依据不成立），别的失效照样拒绝")
    p.add_argument("--confirm", help="确认清单（JSON）：NONE 项、同组升级的项、依据位置无法确认的项逐项写核实方式和证据路径")
    p.add_argument("--confirm-template", help="生成要填的确认清单到这个文件，不接受")
    p.add_argument("--baseline", action="append")
    p.add_argument("--sigs")
    p.add_argument("--layout")
    p.add_argument("--tree")
    p.add_argument("--out", help="基线目录，默认 tools/baseline（--tree 时是 <tree>/tools/baseline）")

    p = sub.add_parser("blind", help="跨版本盲定位自测（不用现有特征码）")
    p.add_argument("--baseline", help="基线文件，默认两个方向都做：steam.json → 离线版、nosteam.json → Steam 版")
    p.add_argument("--exe", help="要定位的 exe")
    p.add_argument("--sigs")
    p.add_argument("--layout")
    p.add_argument("--tree")
    p.add_argument("--json")
    p.add_argument("--workers", type=int)

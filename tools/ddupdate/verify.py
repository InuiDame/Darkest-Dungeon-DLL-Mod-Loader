"""verify 命令：对 exe 逐项判定特征码、同一性和布局，报出受影响的服务和主题。

退出码：0 全部通过；1 只有可选项失效；2 必需项失效、有误绑定或无法确认；3 清单或参数有错、exe 读不了（没有开始校验）。
多个 exe 时取最大的那个。没有静态依据的项单列，不算失效。

误绑定有两种：特征码误绑定（同一性检查，见下面 _identity）和依据误绑定（依据位置核对，见 position.py：依据唯一命中、
值也对，命中的却不是基线里那条指令）。依据位置核对只在 Python 侧做，不改依据本身的求值结论（evidence.py 和 DDCore 同义），
C++/Python 对照测试把它和 HOOK 的静态检查一样列为「Python 独有的检查」。

无法确认（U1 修复 FB，审核 A 的 B1）：依据成立、唯一命中，位置核对却给不出结论（没法对照，position.py 的 unknown）时，
这条依据不算通过：同一项里没有别的「成立、位置对上」的依据，这一项就记为「无法确认」，和误绑定一样是退出码 2，accept
不接受（除非逐项人工确认，见 repair.py 的 accept）。只有一种例外：exe 是已知版本、用的是它自己的基线（这时位置核对是
拿自己和自己比，没法对照只可能是基线比清单旧，重新生成基线即可）。未知版本即使放了自己 SHA1 的基线也不当作自己的基线
（否则位置核对和同一性检查都变成自己和自己比，等于没查）。

同组升级（U1 修复 FB，审核 A 的 S4）：layout.inc 的 DD_GROUP 把一起变的项编成组。组里有运行时能核对的成员变了（RUNTIME 项
失效、值和基线生成时不同；特征码失效、误绑定，或模式和基线生成时不同）时，组里没有运行时依据的成员（NONE、OFFLINE 项）
列进「同组有变化，要人工核实」。这一节不改退出码（这些项本来就核对不了），repair 把它们报成需要人工，accept 要求逐项确认。
"""
import json
from pathlib import Path

from . import baseline as baseline_mod
from . import evidence as evidence_mod
from . import locate as locate_mod
from . import manifest as manifest_mod
from . import offline as offline_mod
from . import sigs as sigs_mod
from .fingerprint import SIMILARITY_THRESHOLD, CallIndex, Imports, engine, fingerprint, fp_similarity, jaccard
from .image import Image
from .position import AGREE, DISAGREE, SKIP, UNKNOWN, PositionCheck

EXIT_OK, EXIT_OPTIONAL, EXIT_REQUIRED, EXIT_ERROR = 0, 1, 2, 3
SECTIONS = ("sigs", "identity", "layout")
RESULT_TEXT = {EXIT_OK: "OK（全部通过）", EXIT_OPTIONAL: "只有可选项失效", EXIT_REQUIRED: "必需项失效、有误绑定或无法确认"}
# 失效的类别：required 必需项失效；optional 可选项失效；misbind 误绑定；unconfirmed 依据位置无法确认（B1）
SEVERE = ("required", "misbind", "unconfirmed")

DEFAULT_EXES = [
    ("steam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windows\win64\Darkest.exe"),
    ("nosteam", r"D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\_windowsnosteam\win64\Darkest.exe"),
]


# ---- 同一性 ----
# 命中的函数和基线里的是不是同一个函数。判为误绑定（退出码 2）的条件，任意一条：
#   1. 指纹相似度低于 SIMILARITY_THRESHOLD（GLOBAL 指向 .text 时目标函数也要过）
#   2. 基线里有直接调用者（E8），现在一个都没有（函数的复制品、诱饵没有人调用）
#   3. 和其他特征码的关系（调用了哪些特征码的函数、引用了哪些特征码的全局，只算两边都解析成功的）的 Jaccard 低于
#      RELATION_THRESHOLD；高于阈值但不完全一致时只记一笔（Steam 版和离线版之间就有这样的合理差异）
#   4. 这条特征码的同一性依据（layout.inc 里 id 是特征码名字的 DD_EVIDENCE）不成立
# 有自己 SHA1 的基线（而且 exe 是已知版本，见 own_baselines）时只和它比，否则和所有基线比、取最像的。
# GLOBAL_ANY 是模板实例，规则不同（U1 修复 FB，审核 A 的 S6）：
#   1. 候选数（解析出的不同地址）和基线不同就是误绑定（原来只记一笔）；
#   2. 每个命中所在的函数都要像基线里这条特征码的某个命中函数代表（基线的 hit_reps，没有时用 function），指纹相似度
#      不低于 SIMILARITY_THRESHOLD，否则是误绑定（别的模板族恰好匹配这条模式时，它的函数形状不同）；
#   3. 命中次数可以变（模板实例随事件类型增减，Steam 版 69 个、离线版 68 个），只记一笔。
RELATION_THRESHOLD = 0.5


def own_baselines(baselines, image, known):
    """exe 自己的基线：只在 exe 是已知版本时算数（未知版本放一份自己 SHA1 的基线进来，位置核对和同一性检查都会变成和自己比）。"""
    if not known:
        return []
    return [b for b in baselines if b["exe"]["sha1"] == image.sha1]


def _global_any(image, sid, res, usable, cs, imports, record):
    counts = sorted({len(b["sigs"][sid]["candidates"]) for b in usable if sid in b["sigs"] and b["sigs"][sid]["candidates"]})
    hit_counts = sorted({b["sigs"][sid]["hit_count"] for b in usable if sid in b["sigs"] and b["sigs"][sid].get("hit_count")})
    record["candidates"] = len(res.candidates)
    record["hits"] = len(res.hits)
    if counts and len(res.candidates) not in counts:
        record["problems"].append(f"候选数 {len(res.candidates)} 和基线 {counts} 不同")
    if hit_counts and len(res.hits) not in hit_counts:
        record["notes"].append(f"命中 {len(res.hits)} 次，基线 {hit_counts} 次（模板实例可以增减）")
    reps = []
    for b in usable:
        entry = b["sigs"].get(sid)
        if entry and entry.get("status") == "ok":
            reps += entry.get("hit_reps") or []
    if not reps:
        # 旧格式的基线只存了最低命中的函数，一个模板族里的实例彼此也可能不像（EventTypeCount 有 3 种形状），不能拿它判
        record["notes"].append("基线里没有命中函数的代表（旧格式的基线），没有比较命中所在的函数")
        return
    worst = None
    funcs = sorted({image.enclosing_function(h) or h for h in res.hits})
    for func in funcs:
        fp = fingerprint(image, func, cs, imports)
        best = max(fp_similarity(fp, rep) for rep in reps)
        if worst is None or best < worst[0]:
            worst = (best, func)
    record["rep_score"] = round(worst[0], 4)
    if worst[0] < SIMILARITY_THRESHOLD:
        record["problems"].append(f"命中所在的函数 {worst[1]:X} 和基线里这条特征码的任何命中函数都不像（最高 {worst[0]:.3f}，"
                                  f"阈值 {SIMILARITY_THRESHOLD}；{len(funcs)} 个命中函数里最不像的那个）")


def _relation_set(rel, alive):
    if not rel:
        return set()
    return {("call", x) for x in rel.get("calls", []) if x in alive} | {("ref", x) for x in rel.get("refs", []) if x in alive}


def _relation_text(now, base):
    lost = sorted(f"{'调用' if k == 'call' else '引用'} {x}" for k, x in base - now)
    gained = sorted(f"{'调用' if k == 'call' else '引用'} {x}" for k, x in now - base)
    parts = []
    if lost:
        parts.append("基线里有、现在没有：" + "、".join(lost))
    if gained:
        parts.append("现在有、基线里没有：" + "、".join(gained))
    return "；".join(parts)


def _identity(image, manifest, sig_results, baselines, cs, sig_evidence, known=None):
    imports = Imports(image)
    functions = {sid: baseline_mod.sig_functions(image, sig, sig_results[sid], cs, imports) for sid, sig in manifest.sigs.items()}
    relations = baseline_mod.compute_relations(manifest, sig_results, functions)
    alive = {sid for sid, r in sig_results.items() if r.status == "ok"}
    own = own_baselines(baselines, image, known)
    usable = own or [b for b in baselines if b["exe"]["sha1"] != image.sha1] or baselines
    call_index = None
    out = {}
    for sid, sig in manifest.sigs.items():
        res = sig_results[sid]
        record = {"status": "n/a", "baseline": None, "score": None, "target_score": None, "strings": None, "callers": None,
                  "base_callers": None, "relation_score": None, "relations": None, "evidence": [], "problems": [], "notes": [],
                  "scores": {}}
        out[sid] = record
        for ev, er in sig_evidence.get(sid, []):
            record["evidence"].append(_evidence_json(ev, er))
            if er.status != "ok":
                record["problems"].append(f"同一性依据 {ev.index}（{ev.describe()}）：{er.detail or er.status}")
        if res.status != "ok":
            continue
        fn, target = functions[sid]
        if sig.kind == "GLOBAL_ANY":
            _global_any(image, sid, res, usable, cs, imports, record)
            record["status"] = "misbind" if record["problems"] else "ok"
            continue
        now_rel = _relation_set(relations.get(sid), alive)
        best = None
        for b in usable:
            entry = b["sigs"].get(sid)
            if not entry or entry["status"] != "ok" or not entry["function"]:
                continue
            name = Path(b["path"]).stem
            score = fp_similarity(fn, entry["function"])
            target_score = None
            if target is not None and entry["target_function"]:
                target_score = fp_similarity(target, entry["target_function"])
            overall = score if target_score is None else min(score, target_score)
            base_rel = _relation_set(entry.get("relations"), alive)
            rel_score = jaccard(now_rel, base_rel)
            record["scores"][name] = round(overall, 4)
            candidate = (overall >= SIMILARITY_THRESHOLD and rel_score >= RELATION_THRESHOLD, overall, rel_score, name,
                         score, target_score, base_rel, entry)
            if best is None or candidate[:3] > best[:3]:
                best = candidate
        if best is None:
            record["status"] = "misbind" if record["problems"] else "no_baseline"
            continue
        _ok, overall, rel_score, name, score, target_score, base_rel, entry = best
        record.update(baseline=name, score=round(score, 4), target_score=None if target_score is None else round(target_score, 4),
                      strings=round(jaccard(fn["strings"], entry["function"]["strings"]), 4), relations=relations.get(sid),
                      relation_score=round(rel_score, 4))
        if overall < SIMILARITY_THRESHOLD:
            record["problems"].append(f"指纹相似度 {overall:.3f} 低于阈值 {SIMILARITY_THRESHOLD}（基线 {name}）")
        if rel_score < RELATION_THRESHOLD:
            record["problems"].append(f"和其他特征码的关系一致度 {rel_score:.2f} 低于 {RELATION_THRESHOLD}：{_relation_text(now_rel, base_rel)}")
        elif now_rel != base_rel:
            record["notes"].append(f"关系一致度 {rel_score:.2f}：{_relation_text(now_rel, base_rel)}")
        base_callers = entry.get("callers")
        if base_callers is not None:
            if call_index is None:
                call_index = CallIndex(image)
            callers = call_index.callers(fn["rva"], cs)
            record.update(callers=len(callers), base_callers=len(base_callers))
            if base_callers and not callers:
                record["problems"].append(f"基线里有 {len(base_callers)} 个直接调用者，现在一个都没有（可能绑到了没人调用的复制品）")
        record["status"] = "misbind" if record["problems"] else "ok"
    return out


def pick_baseline(baselines, identity, image, known=None):
    """依据位置核对和修复用的「更新前」基线：已知版本有本 SHA1 的基线时用它，否则按同一性检查里每条特征码和各基线的
    指纹相似度加起来，取最像的那个（未知版本自己 SHA1 的基线不参加，见 own_baselines）。"""
    own = own_baselines(baselines, image, known)
    if own:
        return own[0]
    baselines = [b for b in baselines if b["exe"]["sha1"] != image.sha1] or baselines
    totals = {}
    for record in identity.values():
        for name, score in (record.get("scores") or {}).items():
            totals[name] = totals.get(name, 0.0) + score
    by_name = {Path(b["path"]).stem: b for b in baselines}
    if totals:
        name = max(sorted(totals), key=lambda n: totals[n])
        return by_name[name]
    return baselines[0] if baselines else None


# ---- 依据位置核对（依据误绑定，规则见 position.py） ----

def _positions(image, manifest, sig_results, baselines, cs, identity, results, known=None):
    """results：(清单项或特征码, 序号) -> (Evidence, EvidenceResult)。返回 (结论表, 用的基线)。"""
    baseline = pick_baseline(baselines, identity, image, known)
    if baseline is None:
        return {}, None
    imports = Imports(image)
    matcher = locate_mod.Matcher(baseline, image, None, cs, imports, relaxed=False)
    ok_sigs = [sid for sid, r in sig_results.items() if r.status == "ok" and identity.get(sid, {}).get("status") != "misbind"]
    locate_mod.seed_from_ok(matcher, image, manifest, baseline, sig_results, ok_sigs, cs, imports)
    locate_mod.seed_from_evidence(matcher, image, manifest, baseline, sig_results, {k: er for k, (_ev, er) in results.items()})
    check = PositionCheck(image, baseline, matcher)
    out = {}
    for (item, index), (ev, er) in results.items():
        if er.status == "blocked" or er.hit is None or len(er.hits) != 1:
            continue
        status, why = check.check(manifest, sig_results, ev, er.hit)
        if status == SKIP:
            continue
        out[f"{item}[{index}]"] = {"item": item, "index": index, "describe": ev.describe(), "status": status, "detail": why,
                                   "hit": er.hit, "evidence_status": er.status}
    return out, baseline


def mark_unconfirmed(positions, own):
    """B1：成立的依据位置没法对照时，这一项要有别的「成立、位置对上」的依据，否则记为无法确认。在 positions 的记录上
    写 counted（算不算失效）和 why；返回无法确认的 (对象, 序号) 列表。own 是「已知版本、用的是自己的基线」（例外，见开头）。"""
    agree_by_owner = {}
    for r in positions.values():
        if r["status"] == AGREE and r["evidence_status"] == "ok":
            agree_by_owner.setdefault(r["item"], []).append(r["index"])
    out = []
    for r in positions.values():
        r["counted"] = False
        if r["status"] != UNKNOWN or r["evidence_status"] != "ok":
            continue
        if own:
            r["why"] = "已知版本、基线是它自己的：不计入（重新生成基线后就有参照）"
            continue
        others = agree_by_owner.get(r["item"], [])
        if others:
            r["why"] = f"同一项的依据 {others} 成立、位置对上，值由它们确认"
            continue
        r["counted"] = True
        r["why"] = "同一项没有别的成立、位置对上的依据，值无法确认"
        out.append((r["item"], r["index"]))
    return out


# ---- 布局 ----

def _evidence_json(ev, er):
    return {"index": ev.index, "line": ev.line, "describe": ev.describe(), "status": er.status, "origin": er.origin,
            "hit": er.hit, "hits": len(er.hits), "captured": er.captured, "expected": er.expected, "implied": er.implied,
            "detail": er.detail}


# ---- 同组升级（S4） ----

def group_flags(manifest, report, baseline):
    """返回 {清单项: [原因, ...]}：同组有运行时能核对的成员变了或失效时，组里没有运行时依据的成员。规则见开头。"""
    snapshot = (baseline or {}).get("items")
    base_sigs = (baseline or {}).get("sigs", {})
    layout = report.get("layout", {})
    sigs = report.get("sigs", {})
    identity = report.get("identity", {})
    bad = {f["name"] for f in report.get("failures", [])}
    flags = {}
    for group in manifest.groups.values():
        triggers = []
        for member in group.members:
            if member in manifest.items:
                item = manifest.items[member]
                if item.check != "RUNTIME":
                    continue
                if member in bad or layout.get(member, {}).get("status") == "fail":
                    triggers.append(f"{member} 失效")
                elif snapshot is None:
                    continue
                elif member not in snapshot:
                    triggers.append(f"{member} 是基线之后新加的项")
                elif snapshot[member] != item.value:
                    old = snapshot[member]
                    triggers.append(f"{member} 的值变了（基线 {old:#x} → 现在 {item.value:#x}）" if isinstance(old, int)
                                    else f"{member} 的值变了")
            elif member in manifest.sigs:
                if member in bad or sigs.get(member, {}).get("status", "ok") != "ok":
                    triggers.append(f"特征码 {member} 失效")
                elif identity.get(member, {}).get("status") == "misbind":
                    triggers.append(f"特征码 {member} 误绑定")
                elif member in base_sigs and base_sigs[member].get("pattern") not in (None, manifest.sigs[member].pattern):
                    triggers.append(f"特征码 {member} 的模式和基线生成时不同（修复工具重新生成过）")
        if not triggers:
            continue
        for member in group.members:
            if member in manifest.items and manifest.items[member].check != "RUNTIME":
                flags.setdefault(member, []).append(f"组 {group.id}（{group.text}）：{trigger_text(triggers)}")
    return flags


def trigger_text(triggers, limit=4):
    """同组升级的原因：最多列 limit 条，其余写个数。"""
    head = "；".join(triggers[:limit])
    return head + (f"……共 {len(triggers)} 项" if len(triggers) > limit else "")


def _layout(image, manifest, sig_results, item_evidence):
    ctx = offline_mod.Context(image, manifest, sig_results)
    offline_cache = {}
    out = {}
    for item in manifest.items.values():
        record = {"type": item.type, "value": item.value, "need": item.need, "check": item.check, "deps": item.deps,
                  "status": "ok", "evidence": [], "offline": [], "unverified": item.unverified, "problems": []}
        out[item.id] = record
        if item.check == "NONE":
            record["status"] = "unverified"
            continue
        for ev, er in item_evidence.get(item.id, []):
            record["evidence"].append(_evidence_json(ev, er))
            if er.status != "ok":
                record["problems"].append(f"依据 {ev.index}（{ev.describe()}）：{er.detail or er.status}")
        for name in item.offline:
            if name not in offline_cache:
                fn = offline_mod.CHECKS.get(name)
                if fn is None:
                    offline_cache[name] = offline_mod.OfflineResult(False, f"没有名为 {name} 的离线检查")
                else:
                    try:
                        offline_cache[name] = fn(ctx)
                    except Exception as e:  # noqa: BLE001 - 检查本身出错按失败处理，写明原因
                        offline_cache[name] = offline_mod.OfflineResult(False, f"检查出错：{type(e).__name__}: {e}")
            result = offline_cache[name]
            summary = getattr(offline_mod.CHECKS.get(name), "summary", "")
            record["offline"].append({"name": name, "ok": result.ok, "detail": result.detail, "summary": summary})
            if not result.ok:
                record["problems"].append(f"离线检查 {name}（{summary}）：{result.detail}")
        if record["problems"]:
            record["status"] = "fail"
    return out


# ---- 一个 exe ----

def verify_exe(path, manifest, baselines, sections, foreign, label_hint=None):
    image = Image(path)
    label = manifest.known_builds.get(image.sha1)
    cs = engine()
    sig_results = sigs_mod.resolve_all(image, manifest, label, foreign, cs)
    item_evidence, sig_evidence = {}, {}
    for ev in manifest.evidence:
        er = evidence_mod.evaluate(image, manifest, sig_results, ev)
        (item_evidence if ev.item in manifest.items else sig_evidence).setdefault(ev.item, []).append((ev, er))
    report = {"path": str(path), "tag": label_hint, "sha1": image.sha1, "file_size": image.file_size, "known": label,
              "baselines": [Path(b["path"]).name for b in baselines], "sections": list(sections)}
    if "sigs" in sections:
        report["sigs"] = {sid: {"kind": manifest.sigs[sid].kind, "required": manifest.sigs[sid].required, "need": r.need,
                                "status": r.status, "hits": len(r.hits), "hit": r.hit, "resolved": r.resolved, "section": r.section,
                                "candidates": r.candidates, "problems": r.problems, "kinds": r.kinds, "notes": r.notes,
                                "runtime_detectable": r.runtime_detectable, "deps": manifest.sigs[sid].deps}
                          for sid, r in sig_results.items()}
    baseline = None
    if "identity" in sections or "layout" in sections:
        # 依据位置核对要用同一性检查的结论（挑基线、播种）。只要布局时也做（check_actor_layout.py 的 --only layout 也要有
        # 位置核对，审核 A 备注 6），同一性的结论只在要求时写进报告
        identity = _identity(image, manifest, sig_results, baselines, cs, sig_evidence, label)
        if "identity" in sections:
            report["identity"] = identity
        results = {(ev.item, ev.index): (ev, er) for group in (item_evidence, sig_evidence)
                   for pairs in group.values() for ev, er in pairs}
        report["positions"], baseline = _positions(image, manifest, sig_results, baselines, cs, identity, results, label)
        report["position_baseline"] = Path(baseline["path"]).name if baseline else None
        own = bool(baseline) and baseline["exe"]["sha1"] == image.sha1 and bool(label)
        report["position_own"] = own
        mark_unconfirmed(report["positions"], own)
    if "layout" in sections and manifest.items:
        report["layout"] = _layout(image, manifest, sig_results, item_evidence)
    _summarize(report, manifest, sig_results)
    report["group_flags"] = group_flags(manifest, report, baseline) if manifest.groups else {}
    report["baseline_items"] = bool(baseline and baseline.get("items") is not None)
    return report


def _summarize(report, manifest, sig_results):
    affected = {}
    failures = []

    def hit(name, severity, why, deps):
        failures.append({"name": name, "severity": severity, "why": why})
        for token in deps:
            affected.setdefault(token, []).append(name)

    for sid, r in report.get("sigs", {}).items():
        if r["status"] != "ok":
            hit(sid, "required" if r["need"] == "REQUIRED" else "optional", "; ".join(r["problems"]), r["deps"])
    for sid, r in report.get("identity", {}).items():
        if r["status"] == "misbind":
            hit(sid, "misbind", "; ".join(r["problems"]), manifest.sigs[sid].deps)
    for key, r in report.get("positions", {}).items():
        name = r["item"]
        deps = manifest.items[name].deps if name in manifest.items else manifest.sigs[name].deps
        if r["status"] == DISAGREE:
            hit(name, "misbind", f"依据 {r['index']} 位置核对：{r['detail']}（疑似依据误绑定）", deps)
        elif r.get("counted"):
            hit(name, "unconfirmed", f"依据 {r['index']} 成立，但命中位置没法和基线对照：{r['detail']}；{r['why']}", deps)
    for iid, r in report.get("layout", {}).items():
        if r["status"] == "fail":
            hit(iid, "required" if r["need"] == "REQUIRED" else "optional", "; ".join(r["problems"]), r["deps"])
    if any(f["severity"] in SEVERE for f in failures):
        code = EXIT_REQUIRED
    elif failures:
        code = EXIT_OPTIONAL
    else:
        code = EXIT_OK
    report["failures"] = failures
    report["affected"] = {k: sorted(set(v)) for k, v in sorted(affected.items())}
    report["unverified"] = sorted(iid for iid, r in report.get("layout", {}).items() if r["status"] == "unverified")
    report["exit_code"] = code
    report["result"] = RESULT_TEXT[code]


# ---- 输出 ----

def _hex(value):
    return "-" if value is None else f"{value:X}"


def print_report(report, manifest, out):
    w = out.write
    w(f"== {report['tag'] or 'exe'} {report['path']}\n")
    known = f"已知版本 {report['known']}" if report["known"] else "未知版本"
    w(f"   sha1={report['sha1']} {known}；基线 {', '.join(report['baselines']) or '无'}\n")
    if "sigs" in report:
        w(" 特征码\n")
        for sid, r in report["sigs"].items():
            if r["status"] == "ok":
                where = f"rva={_hex(r['hit'])}"
                if r["kind"] in ("GLOBAL", "GLOBAL_ANY"):
                    where += f" 全局 {_hex(r['resolved'])}（{r['section']}）"
                tag = "OK  "
            else:
                where = "；".join(r["problems"])
                tag = "FAIL" if r["need"] == "REQUIRED" else "WARN"
            notes = f" {'；'.join(r['notes'])}" if r["notes"] else ""
            w(f"  {tag} {sid:<24} {where}{notes}\n")
    if "identity" in report:
        w(f" 同一性（指纹相似度阈值 {SIMILARITY_THRESHOLD}，关系一致度阈值 {RELATION_THRESHOLD}）\n")
        for sid, r in report["identity"].items():
            if r["status"] == "n/a":
                continue
            if r["status"] == "ok" and r.get("score") is not None:
                extra = f"{r['score']:.3f}" + (f"/目标 {r['target_score']:.3f}" if r["target_score"] is not None else "")
                callers = f"，调用者 {r['callers']}（基线 {r['base_callers']}）" if r["callers"] is not None else ""
                text = f"{extra}（基线 {r['baseline']}，关系 {r['relation_score']:.2f}，字符串 {r['strings']:.2f}{callers}）"
                if r["notes"]:
                    text += "；" + "；".join(r["notes"])
            elif r["status"] == "ok":
                text = f"GLOBAL_ANY，候选 {r.get('candidates')} 个、命中 {r.get('hits')} 次" \
                    + (f"，命中函数和基线代表最低相似度 {r['rep_score']:.3f}" if r.get("rep_score") is not None else "") \
                    + (f"；{'；'.join(r['notes'])}" if r.get("notes") else "")
            elif r["status"] == "no_baseline":
                text = "没有基线"
            else:
                text = "；".join(r["problems"])
            if r["evidence"]:
                text += f"；同一性依据 {sum(e['status'] == 'ok' for e in r['evidence'])}/{len(r['evidence'])}"
            tag = {"ok": "OK  ", "no_baseline": "--  ", "misbind": "BIND"}[r["status"]]
            w(f"  {tag} {sid:<24} {text}\n")
    if "positions" in report:
        positions = report["positions"]
        counts = {k: sum(1 for r in positions.values() if r["status"] == k) for k in (AGREE, DISAGREE, UNKNOWN)}
        unconfirmed = sum(1 for r in positions.values() if r.get("counted"))
        w(f" 依据位置（唯一命中要落在基线里那条指令对齐到的位置，基线 {report['position_baseline'] or '无'}）："
          f"对上 {counts[AGREE]}、对不上 {counts[DISAGREE]}、没法对照 {counts[UNKNOWN]}（其中无法确认 {unconfirmed}）\n")
        for key, r in positions.items():
            if r["status"] == DISAGREE:
                state = "成立" if r["evidence_status"] == "ok" else "不成立"
                w(f"  BIND {key:<28} {r['detail']}（依据本身{state}，疑似依据误绑定）\n")
        for key, r in positions.items():
            if r["status"] == UNKNOWN:
                tag = "UNKN" if r.get("counted") else "--  "
                extra = f"；{r['why']}" if r.get("why") else ("；依据本身不成立，按失效处理" if r["evidence_status"] != "ok" else "")
                w(f"  {tag} {key:<28} {r['detail']}{extra}\n")
    if "layout" in report:
        w(" 布局\n")
        for iid, r in report["layout"].items():
            if r["status"] == "unverified":
                continue
            kinds = []
            if r["evidence"]:
                kinds.append(f"运行时 {sum(e['status'] == 'ok' for e in r['evidence'])}/{len(r['evidence'])}")
            if r["offline"]:
                kinds.append(f"离线 {sum(o['ok'] for o in r['offline'])}/{len(r['offline'])}")
            tag = "OK  " if r["status"] == "ok" else ("FAIL" if r["need"] == "REQUIRED" else "WARN")
            w(f"  {tag} {iid:<28} = {r['value']:#x}  {'，'.join(kinds)}\n")
            for problem in r["problems"]:
                w(f"         {problem}\n")
        groups = {}
        for iid in report["unverified"]:
            groups.setdefault(report["layout"][iid]["unverified"], []).append(iid)
        if groups:
            w(f" 没有静态依据（更新后要人工或靠自检核实）：{len(report['unverified'])} 项\n")
            for how, ids in groups.items():
                w(f"  - {how}\n    {', '.join(ids)}\n")
    if report.get("group_flags"):
        w(f" 同组有变化，要人工核实（DD_GROUP，accept 要求逐项确认）：{len(report['group_flags'])} 项\n")
        for iid, why in report["group_flags"].items():
            w(f"  GRP  {iid:<28} {'；'.join(why)}\n")
    elif "layout" in report and not report.get("baseline_items"):
        w(" 同组升级：用的基线没有清单项的快照（旧格式），看不出哪些项的值变了，只按失效判断\n")
    if report["affected"]:
        w(" 失效影响\n")
        for token, names in report["affected"].items():
            w(f"  {token:<22} ← {', '.join(names)}\n")
    w(f" 结论：{report['result']}（退出码 {report['exit_code']}）\n")


def manifest_summary(manifest):
    sigs = list(manifest.sigs.values())
    by_req = {}
    for s in sigs:
        by_req[s.required] = by_req.get(s.required, 0) + 1
    items = list(manifest.items.values())
    by_check = {}
    for i in items:
        by_check[i.check] = by_check.get(i.check, 0) + 1
    req = "、".join(f"{k} {v}" for k, v in by_req.items())
    text = f"清单：{Path(manifest.sig_file).name} {len(sigs)} 条特征码（{req}）"
    if manifest.layout_file:
        chk = "、".join(f"{k} {v}" for k, v in by_check.items())
        text += f"；{Path(manifest.layout_file).name} {len(items)} 项（{chk}），依据 {len(manifest.evidence)} 条"
    return text


def run(args, out):
    tree = Path(args.tree) if getattr(args, "tree", None) else None
    if tree and not args.sigs:
        # --tree：用源码副本里的清单和基线（游戏更新的手册先在副本里走一遍时用，repair、accept 也有同样的参数）
        args.sigs = str(tree / "core" / "src" / "game" / "signatures.inc")
        args.layout = args.layout or str(tree / "core" / "src" / "game" / "layout.inc")
        if not args.baseline and (tree / "tools" / "baseline").is_dir():
            args.baseline = [str(p) for p in sorted((tree / "tools" / "baseline").glob("*.json"))]
    try:
        manifest = manifest_mod.load(args.sigs, args.layout, use_layout=not args.sigs or bool(args.layout))
    except manifest_mod.ManifestError as e:
        out.write(f"清单解析失败：{e}\n")
        return EXIT_ERROR
    problems = manifest_mod.lint(manifest, require_deps=not args.sigs or bool(tree))
    out.write(manifest_summary(manifest) + "\n")
    if problems:
        out.write(f"清单有 {len(problems)} 个问题：\n" + "".join(f"  {p}\n" for p in problems))
        return EXIT_ERROR
    sections = tuple(args.only.split(",")) if args.only else SECTIONS
    if any(s not in SECTIONS for s in sections):
        out.write(f"--only 只能是 {','.join(SECTIONS)} 的组合\n")
        return EXIT_ERROR
    baseline_files = args.baseline if args.baseline else ([] if args.sigs else baseline_mod.default_files())
    baselines = []
    for path in baseline_files:
        try:
            baselines.append(baseline_mod.load(path))
        except (OSError, ValueError) as e:
            out.write(f"基线读不了：{path}：{e}\n")
            return EXIT_ERROR
    exes = [(None, p) for p in args.exe] or DEFAULT_EXES
    foreign = sigs_mod.foreign_patches()
    reports = []
    for tag, path in exes:
        if not Path(path).is_file():
            out.write(f"exe 不存在：{path}\n")
            return EXIT_ERROR
        report = verify_exe(path, manifest, baselines, sections, foreign, tag)
        print_report(report, manifest, out)
        reports.append(report)
    code = max(r["exit_code"] for r in reports)
    out.write(f"结果: {RESULT_TEXT[code]}，退出码 {code}\n")
    if args.json:
        data = {"tool": "dd_update verify", "manifest": {"signatures": manifest.sig_file, "layout": manifest.layout_file,
                                                           "sigs": len(manifest.sigs), "items": len(manifest.items),
                                                           "evidence": len(manifest.evidence)},
                "similarity_threshold": SIMILARITY_THRESHOLD, "relation_threshold": RELATION_THRESHOLD, "exes": reports,
                "exit_code": code}
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
        out.write(f"JSON 报告：{args.json}\n")
    return code

"""基线快照：已知版本的 exe 上每条特征码命中的函数的指纹、调用关系，以及每条依据的命中位置。

文件是 tools/baseline/<版本名>.json，同一个 exe 两次生成字节一致（键排序、没有时间戳、集合都排过序）。
游戏更新后旧 exe 通常已经被覆盖，修复工具（U1c）只能靠基线文件，所以这里存的是完整的指纹而不是摘要。

结构（format 2，format 1 的字段全部保留，load 两种都认）：
  exe        label、sha1、文件大小、映像基址、.text 范围
  vocab      全部指纹用到的规范化指令，按字典序；指纹里的 tokens 是空格分隔的下标
  sigs       每条特征码：kind、required、pattern、status、hit、resolved、section、hit_count、candidates（GLOBAL_ANY）、
             function（HOOK/CODE 是解析出的函数，GLOBAL/GLOBAL_ANY 是命中位置所在的函数，GLOBAL_ANY 取最低的命中）、
             target_function（GLOBAL 指向 .text 时，例如 call rel32 的目标）、callers（调用 function 的函数）、
             relations（calls：function 调用了哪些特征码的函数；refs：引用了哪些 GLOBAL 特征码的全局）、
             anchor（format 2）：命中位置和 capture 在 function 的指令序列里的下标 {insn, capture_insn, capture_field}，
             capture_field 是 disp（rip 相对位移）或 imm（call/jmp 的 rel32）；修复工具靠它在新函数里找对应的指令
  evidence   每条运行时依据：对象、序号、状态、起点、命中位置、命中所在函数、函数内偏移、命中的指令下标、读到的值
  functions  （format 2）依据命中所在、但不是特征码函数的那些函数（虚表槽位、TEXT 依据所在的函数）的指纹，按 rva 排序，
             修复工具对它们做指令对齐
  items      （U1 修复 FB）生成基线时清单项的值（TABLE 是名字或整数表），和特征码的 pattern 一起是「这个版本 accept 时清单
             的样子」：verify 和 accept 拿它和现在的清单比，找出值变了的 RUNTIME 项、模式变了的特征码，把同组（DD_GROUP）
             没有运行时依据的项升级成需要人工（审核 A 的 S4）。没有这个键的旧基线按「不知道变没变」处理（见 verify.py）
  sigs[id].hit_reps  （U1 修复 FB，GLOBAL_ANY）命中所在函数的代表：按命中位置排序，和已有代表的指纹相似度都低于阈值的
             函数才加进来（Steam 版 EventTypeCount 69 个命中 3 个代表，EventClassNames、QueryClassNames 各 1 个）。verify 要求
             每个命中所在的函数都像其中一个代表（审核 A 的 S6）
  指纹里的 refs（format 2）：每条指令引用的字符串、导入函数、直接调用的目标、全局，见 fingerprint.insn_refs。
             存成一个空格分隔的字符串，每项「指令下标 + 种类 + 值」：s / i 的值是在同一指纹 strings / imports 里的下标
             （十进制），c / g / f 的值是 RVA（十六进制），例如 "12s3 15c4A1200 20g2A6D4BB 31i0"；load 时还原成列表。
  只存规范化 token 和引用，不存机器码。
"""
import json
import re
from pathlib import Path

from . import evidence as evidence_mod
from .fingerprint import SIMILARITY_THRESHOLD, CallIndex, Imports, decode_function, engine, fingerprint, fp_similarity, insn_refs
from .manifest import TOOLS

BASELINE_DIR = TOOLS / "baseline"
FORMAT = 2
READABLE_FORMATS = (1, 2)


def sig_functions(image, sig, res, cs, imports):
    """特征码对应的函数指纹：(function, target_function)。"""
    if res.status != "ok":
        return None, None
    anchor = res.resolved if sig.kind in ("HOOK", "CODE") else res.hit
    function = fingerprint(image, anchor, cs, imports)
    target = None
    if sig.kind == "GLOBAL" and image.in_text(res.resolved):
        target = fingerprint(image, res.resolved, cs, imports)
    return function, target


def hit_representatives(image, res, cs, imports):
    """GLOBAL_ANY：命中所在函数的代表（贪心聚类，按命中位置的顺序，确定性）。"""
    reps = []
    seen = set()
    for h in sorted(res.hits):
        func = image.enclosing_function(h) or h
        if func in seen:
            continue
        seen.add(func)
        fp = fingerprint(image, func, cs, imports)
        if not any(fp_similarity(fp, rep) >= SIMILARITY_THRESHOLD for rep in reps):
            reps.append(fp)
    return reps


def item_snapshot(manifest):
    """清单项的值（TABLE 是表的内容），存进基线。"""
    out = {}
    for iid, item in manifest.items.items():
        if item.type == "TABLE":
            out[iid] = list(item.names if item.names is not None else (item.values or []))
        else:
            out[iid] = item.value
    return out


def function_starts(fp):
    starts = {fp["rva"]}
    if fp.get("thunk"):
        starts.add(fp["thunk"]["rva"])
    return starts


def compute_relations(manifest, sig_results, functions):
    """functions: id -> (function, target)。返回 id -> {"calls": [...], "refs": [...]}。"""
    starts = {}
    for sid, (fn, target) in functions.items():
        if fn is None:
            continue
        sig = manifest.sigs[sid]
        if sig.kind in ("HOOK", "CODE"):
            starts[sid] = function_starts(fn)
        elif target is not None:
            starts[sid] = function_starts(target)
    globals_ = {sid: res.resolved for sid, res in sig_results.items()
                if res.status == "ok" and manifest.sigs[sid].kind in ("GLOBAL", "GLOBAL_ANY") and res.section in (".data", ".rdata")}
    out = {}
    for sid, (fn, _target) in functions.items():
        if fn is None:
            continue
        callees = set(fn["callees"])
        calls = sorted(other for other, s in starts.items() if other != sid and callees & s)
        refs = sorted(other for other, g in globals_.items() if g in set(fn["globals"]))
        out[sid] = {"calls": calls, "refs": refs}
    return out


def insn_index(fp_insns, rva):
    for i, (address, size) in enumerate(fp_insns):
        if address <= rva < address + size:
            return i
    return -1


def sig_anchor(image, sig, res, cs):
    """命中位置和 capture 在 function 的指令序列（decode_function 的顺序，和 tokens 一一对应）里的下标。"""
    if res.status != "ok":
        return None
    start = res.resolved if sig.kind in ("HOOK", "CODE") else res.hit
    _, insns, _ = decode_function(image, start, cs)
    anchor = {"insn": None, "capture_insn": None, "capture_field": None}
    capture = res.hit + sig.capture if sig.kind in ("GLOBAL", "GLOBAL_ANY") else None
    for i, ins in enumerate(insns):
        if ins.address <= res.hit < ins.address + ins.size and anchor["insn"] is None:
            anchor["insn"] = i
        if capture is not None and ins.address <= capture < ins.address + ins.size:
            anchor["capture_insn"] = i
            offset = capture - ins.address
            if ins.disp_size == 4 and ins.disp_offset == offset:
                anchor["capture_field"] = "disp"
            elif ins.imm_size == 4 and ins.imm_offset == offset:
                anchor["capture_field"] = "imm"
    return anchor


def attach_refs(image, fp, cs, imports):
    """给指纹（含 thunk 目标）加上 refs，解码方式和 fingerprint 一致。"""
    while fp:
        _, insns, _ = decode_function(image, fp["rva"], cs)
        fp["refs"] = insn_refs(image, insns, imports)
        fp = fp.get("thunk")


def generate(image, manifest, sig_results, label):
    cs = engine()
    imports = Imports(image)
    calls = CallIndex(image)
    functions = {}
    for sid, sig in manifest.sigs.items():
        functions[sid] = sig_functions(image, sig, sig_results[sid], cs, imports)
        for fp in functions[sid]:
            attach_refs(image, fp, cs, imports)
    relations = compute_relations(manifest, sig_results, functions)
    known = set()
    for fn, target in functions.values():
        for fp in (fn, target):
            if fp:
                known.add(fp["rva"])
    extra = {}
    for ev in manifest.evidence:
        result = evidence_mod.evaluate(image, manifest, sig_results, ev)
        func = image.enclosing_function(result.hit) if result.hit is not None else None
        if func is not None and func not in known and func not in extra:
            extra[func] = fingerprint(image, func, cs, imports)
            attach_refs(image, extra[func], cs, imports)
    reps_by_sig = {}
    for sid, sig in manifest.sigs.items():
        if sig.kind == "GLOBAL_ANY" and sig_results[sid].status == "ok":
            reps_by_sig[sid] = hit_representatives(image, sig_results[sid], cs, imports)
    vocab = set()
    every = [f for pair in functions.values() for f in pair] + list(extra.values())
    every += [f for reps in reps_by_sig.values() for f in reps]
    for fp in every:
        while fp:
            vocab.update(fp["tokens"])
            fp = fp.get("thunk")
    vocab = sorted(vocab)
    index = {t: i for i, t in enumerate(vocab)}

    def pack(fp):
        if fp is None:
            return None
        out = dict(fp)
        out["tokens"] = " ".join(str(index[t]) for t in fp["tokens"])
        out["thunk"] = pack(fp["thunk"]) if fp.get("thunk") else None
        if "refs" in fp:
            out["refs"] = encode_refs(fp)
        return out

    sigs = {}
    for sid, sig in manifest.sigs.items():
        res = sig_results[sid]
        fn, target = functions[sid]
        entry = {
            "kind": sig.kind,
            "required": sig.required,
            "pattern": sig.pattern,
            "status": res.status,
            "hit": res.hit,
            "resolved": res.resolved,
            "section": res.section,
            "hit_count": len(res.hits),
            "candidates": res.candidates if sig.kind == "GLOBAL_ANY" else None,
            "function": pack(fn),
            "target_function": pack(target),
            "callers": calls.callers(fn["rva"], cs) if fn is not None and sig.kind in ("HOOK", "CODE") else None,
            "relations": relations.get(sid),
            "anchor": sig_anchor(image, sig, res, cs),
        }
        if sid in reps_by_sig:
            entry["hit_reps"] = [pack(fp) for fp in reps_by_sig[sid]]
        sigs[sid] = entry

    evidence = []
    decoded = {}
    for ev in manifest.evidence:
        result = evidence_mod.evaluate(image, manifest, sig_results, ev)
        record = {"item": ev.item, "index": ev.index, "status": result.status, "origin": result.origin, "hit": result.hit,
                  "captured": result.captured, "function": None, "offset": None, "insn_index": None}
        if result.hit is not None:
            func = image.enclosing_function(result.hit)
            record["function"] = func
            if func is not None:
                record["offset"] = result.hit - func
                if func not in decoded:
                    _, insns, _ = decode_function(image, func, cs)
                    decoded[func] = [(i.address, i.size) for i in insns]
                record["insn_index"] = insn_index(decoded[func], result.hit)
        evidence.append(record)

    return {
        "format": FORMAT,
        "exe": {"label": label, "sha1": image.sha1, "file_size": image.file_size, "image_base": image.base,
                "text": [image.text_start, image.text_end]},
        "vocab": vocab,
        "sigs": sigs,
        "evidence": evidence,
        "functions": [pack(extra[rva]) for rva in sorted(extra)],
        "items": item_snapshot(manifest),
    }


def dumps(data):
    return json.dumps(data, ensure_ascii=False, sort_keys=True, indent=1) + "\n"


def write(data, path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(dumps(data).encode("utf-8"))


def unpack(fp, vocab):
    if fp is None:
        return None
    out = dict(fp)
    out["tokens"] = [vocab[int(i)] for i in fp["tokens"].split()] if fp["tokens"] else []
    out["thunk"] = unpack(fp["thunk"], vocab) if fp.get("thunk") else None
    if isinstance(fp.get("refs"), str):
        out["refs"] = decode_refs(fp["refs"], fp)
    return out


def load(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if data.get("format") not in READABLE_FORMATS:
        raise ValueError(f"{path}：不认识的基线格式 {data.get('format')}")
    vocab = data["vocab"]
    for entry in data["sigs"].values():
        entry["function"] = unpack(entry["function"], vocab)
        entry["target_function"] = unpack(entry["target_function"], vocab)
        entry.setdefault("anchor", None)
        if entry.get("hit_reps"):
            entry["hit_reps"] = [unpack(fp, vocab) for fp in entry["hit_reps"]]
    data["functions"] = [unpack(fp, vocab) for fp in data.get("functions", [])]
    data["path"] = str(path)
    return data


_REF = re.compile(r"(\d+)([sicgf])([0-9A-Fa-f]+)")


def encode_refs(fp):
    strings = {t: i for i, t in enumerate(fp["strings"])}
    imports = {t: i for i, t in enumerate(fp["imports"])}
    out = []
    for i, kind, value in fp["refs"]:
        if kind == "s":
            out.append(f"{i}s{strings[value]}")
        elif kind == "i":
            out.append(f"{i}i{imports[value]}")
        else:
            out.append(f"{i}{kind}{value:X}")
    return " ".join(out)


def decode_refs(text, fp):
    out = []
    for i, kind, value in _REF.findall(text or ""):
        if kind == "s":
            out.append([int(i), "s", fp["strings"][int(value)]])
        elif kind == "i":
            out.append([int(i), "i", fp["imports"][int(value)]])
        else:
            out.append([int(i), kind, int(value, 16)])
    return out


def default_files():
    return sorted(BASELINE_DIR.glob("*.json"))

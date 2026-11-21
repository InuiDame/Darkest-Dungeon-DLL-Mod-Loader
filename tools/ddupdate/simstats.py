"""相似度度量的数据验证（simstats 命令）。

三组数据，都用 fingerprint.fp_similarity：
  same      每条特征码的函数在 A、B 两个版本之间（同一个函数，RVA 不同）
  impostor  B 版本里每条特征码的函数和 A 版本里所有其他函数（大小在 1/2 到 2 倍之间）比，最像的那个（「最近冒充者」，
            误绑定最可能落到的地方）
  random    A 版本里随机抽的不同函数对（大小在 1/2 到 2 倍之间），固定种子；random_big 是其中两边都不少于 BIG 个 3-gram 的
阈值要落在 same 的最小值和 impostor 的主体之间；impostor 里高于阈值的单列出来，说明靠什么区分。
"""
import json
import random
import statistics
from pathlib import Path

from . import manifest as manifest_mod
from . import sigs as sigs_mod
from .baseline import sig_functions
from .fingerprint import SIMILARITY_THRESHOLD, Imports, decode_function, engine, fp_similarity, shingles, similarity, token
from .image import Image
from .verify import DEFAULT_EXES

BIG = 30  # 「大函数」：3-gram 不少于这么多（约 32 条指令），小函数里模板实例和 thunk 大量雷同


def _percentiles(values):
    if not values:
        return {}
    values = sorted(values)

    def q(p):
        return round(values[min(len(values) - 1, int(p * (len(values) - 1) + 0.5))], 4)
    return {"n": len(values), "min": round(values[0], 4), "p01": q(0.01), "p05": q(0.05), "p25": q(0.25), "median": q(0.5),
            "p75": q(0.75), "p95": q(0.95), "p99": q(0.99), "max": round(values[-1], 4), "mean": round(statistics.fmean(values), 4)}


def _histogram(values, step=0.05):
    bins = [0] * int(round(1 / step) + 1)
    for v in values:
        bins[min(len(bins) - 1, int(v / step))] += 1
    return {f"{i * step:.2f}": n for i, n in enumerate(bins) if n}


class ShingleCache:
    def __init__(self, image, cs):
        self.image, self.cs, self.cache = image, cs, {}

    def get(self, rva):
        if rva not in self.cache:
            _, insns, _ = decode_function(self.image, rva, self.cs)
            self.cache[rva] = shingles([token(i) for i in insns])
        return self.cache[rva]


def run(args, out):
    manifest = manifest_mod.load()
    paths = dict(DEFAULT_EXES)
    path_a, path_b = args.a or paths["steam"], args.b or paths["nosteam"]
    image_a, image_b = Image(path_a), Image(path_b)
    cs = engine()
    foreign = sigs_mod.foreign_patches()
    res_a = sigs_mod.resolve_all(image_a, manifest, None, foreign, cs)
    res_b = sigs_mod.resolve_all(image_b, manifest, None, foreign, cs)
    imports_a, imports_b = Imports(image_a), Imports(image_b)

    same, rows = [], []
    sig_fns = {}
    for sid, sig in manifest.sigs.items():
        if sig.kind == "GLOBAL_ANY" or res_a[sid].status != "ok" or res_b[sid].status != "ok":
            continue
        fa, ta = sig_functions(image_a, sig, res_a[sid], cs, imports_a)
        fb, tb = sig_functions(image_b, sig, res_b[sid], cs, imports_b)
        score = fp_similarity(fa, fb)
        if ta and tb:
            score = min(score, fp_similarity(ta, tb))
        same.append(score)
        sig_fns[sid] = (fa, fb)
        rows.append({"sig": sid, "same": round(score, 4), "a": fa["rva"], "b": fb["rva"], "size_a": fa["size"], "size_b": fb["size"],
                     "insns": len(fa["tokens"])})

    cache_a = ShingleCache(image_a, cs)
    starts = sorted(p for p in set(image_a.func_primary.values()))
    sizes = {p: sum(e - b for b, e in image_a.func_chunks[p]) for p in starts}
    impostor = []
    for row in rows:
        sid = row["sig"]
        fa, fb = sig_fns[sid]
        if not fb["pdata"]:
            row["impostor"] = None
            continue
        target = shingles(fb["tokens"])
        size = fb["size"]
        best, best_rva = 0.0, None
        for rva in starts:
            if rva == fa["rva"] or not size / 2 <= sizes[rva] <= size * 2:
                continue
            score = similarity(target, cache_a.get(rva))
            if score > best:
                best, best_rva = score, rva
        row["impostor"] = round(best, 4)
        row["impostor_rva"] = best_rva
        impostor.append(best)

    rng = random.Random(args.seed)
    randoms, randoms_big = [], []
    tries = 0
    while len(randoms) < args.pairs and tries < args.pairs * 20:
        tries += 1
        a, b = rng.choice(starts), rng.choice(starts)
        if a == b or not sizes[a] / 2 <= sizes[b] <= sizes[a] * 2 or sizes[a] < 16:
            continue
        sa, sb = cache_a.get(a), cache_a.get(b)
        score = similarity(sa, sb)
        randoms.append(score)
        if min(sum(sa.values()), sum(sb.values())) >= BIG:
            randoms_big.append(score)

    data = {
        "a": {"path": path_a, "sha1": image_a.sha1}, "b": {"path": path_b, "sha1": image_b.sha1},
        "threshold": SIMILARITY_THRESHOLD, "seed": args.seed,
        "same": _percentiles(same), "impostor": _percentiles(impostor), "random": _percentiles(randoms),
        "random_big": _percentiles(randoms_big),
        "histogram": {"same": _histogram(same), "impostor": _histogram(impostor), "random": _histogram(randoms),
                      "random_big": _histogram(randoms_big)},
        "above_threshold": {"impostor": sorted(r["sig"] for r in rows if r.get("impostor") is not None and r["impostor"] >= SIMILARITY_THRESHOLD),
                            "random": sum(v >= SIMILARITY_THRESHOLD for v in randoms),
                            "random_big": sum(v >= SIMILARITY_THRESHOLD for v in randoms_big)},
        "below_threshold_same": sorted(r["sig"] for r in rows if r["same"] < SIMILARITY_THRESHOLD),
        "rows": rows,
    }
    w = out.write
    w(f"A={path_a}\nB={path_b}\n阈值 {SIMILARITY_THRESHOLD}\n")
    for key, text in (("same", "同一函数跨版本"), ("impostor", "最近冒充者"), ("random", "随机不同函数对"),
                      ("random_big", f"随机不同函数对（两边都不少于 {BIG} 个 3-gram）")):
        w(f"{text}：{data[key]}\n")
    w(f"同一函数低于阈值：{data['below_threshold_same']}\n")
    w(f"冒充者高于阈值：{data['above_threshold']['impostor']}；随机对高于阈值 {data['above_threshold']['random']} 个，"
      f"其中大函数 {data['above_threshold']['random_big']} 个\n")
    w("逐条（同一函数 / 最近冒充者）：\n")
    for r in rows:
        imp = "-" if r.get("impostor") is None else f"{r['impostor']:.3f}@{r['impostor_rva']:X}"
        w(f"  {r['sig']:<26} {r['same']:.3f}  {imp:<18} {r['insns']} 条指令\n")
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
        w(f"JSON：{args.json}\n")
    return 0

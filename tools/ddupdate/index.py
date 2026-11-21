"""整个 exe 的函数指纹索引（修复工具的「全函数指纹搜索」、调用图传播、字符串和常量锚点都靠它）。

范围：异常目录里的全部主函数，加上落在异常目录空隙里、被 E8 调用的叶函数（同 CallIndex 的规则）。每个函数存
fingerprint.fingerprint 的结果（token、callees、strings、globals、imports、imms、thunk 目标），token 存成词表下标。
另外存 rip 相对引用的 .text 地址（函数指针），建反查表：被谁调用、哪些函数引用某个字符串 / 全局 / 导入函数 /
立即数 / 函数指针、哪些 thunk 跳到某个函数。

全量解码一个 exe 约 3.4M 条指令，单进程约 45 秒，所以按 exe 的 SHA1 缓存到 DD.ModLoader/build/ddupdate-cache/，
缓存键里带上 fingerprint.py、image.py、本文件的内容哈希（指纹语义一改缓存就作废），并且多进程生成。

相似度和 fingerprint.fp_similarity 同义：token 3-gram 多重集的 Dice 系数，不足 3 条指令时整个序列当一个元素；
thunk 再和跳转目标比，取较小值；一边是 thunk 一边不是时为 0。
"""
import bisect
import hashlib
import heapq
import os
import pickle
from array import array
from collections import Counter
from pathlib import Path

from .fingerprint import CallIndex, Imports, engine, fingerprint
from .manifest import PROJECT

CACHE_DIR = PROJECT / "build" / "ddupdate-cache"
INDEX_VERSION = 2
_HERE = Path(__file__).resolve().parent


def _code_hash():
    h = hashlib.sha1()
    for name in ("fingerprint.py", "image.py", "index.py"):
        h.update((_HERE / name).read_bytes())
    return h.hexdigest()[:16]


class Func:
    __slots__ = ("rva", "pdata", "size", "chunks", "tokens", "callees", "strings", "globals", "imports", "imms", "thunk",
                 "coderefs")

    def __init__(self, rva, pdata, size, chunks, tokens, callees, strings, globals_, imports, imms, thunk, coderefs):
        self.rva, self.pdata, self.size, self.chunks = rva, pdata, size, chunks
        self.tokens, self.callees, self.strings, self.globals = tokens, callees, strings, globals_
        self.imports, self.imms, self.thunk, self.coderefs = imports, imms, thunk, coderefs

    def state(self):
        return (self.rva, self.pdata, self.size, self.chunks, self.tokens.tobytes(), self.callees, self.strings,
                self.globals, self.imports, self.imms, self.thunk, self.coderefs)

    @classmethod
    def from_state(cls, st):
        tokens = array("H")
        tokens.frombytes(st[4])
        return cls(st[0], st[1], st[2], st[3], tokens, *st[5:])


# ---- 生成（可以多进程）----

_worker_image = None


def _worker_init(path):
    global _worker_image
    from .image import Image
    image = Image(path)
    _worker_image = (image, engine(), Imports(image))


def _fingerprint_many(image, cs, imports, rvas):
    vocab, index, out = [], {}, []
    for rva in rvas:
        coderefs = []
        fp = fingerprint(image, rva, cs, imports, coderefs=coderefs)
        ids = []
        for t in fp["tokens"]:
            k = index.get(t)
            if k is None:
                k = index[t] = len(vocab)
                vocab.append(t)
            ids.append(k)
        thunk = fp["thunk"]["rva"] if fp.get("thunk") else None
        out.append((rva, fp["pdata"], fp["size"], tuple(tuple(c) for c in fp["chunks"]), ids, tuple(fp["callees"]),
                    tuple(fp["strings"]), tuple(fp["globals"]), tuple(fp["imports"]), tuple(fp["imms"]), thunk,
                    tuple(sorted(set(coderefs)))))
    return vocab, out


def _worker_run(rvas):
    image, cs, imports = _worker_image
    return _fingerprint_many(image, cs, imports, rvas)


def function_starts(image):
    """要索引的函数：异常目录的主函数，加上空隙里被 E8 调用的叶函数。"""
    starts = set(image.func_chunks)
    for target in CallIndex(image).calls:
        if image.enclosing_function(target) is None:
            starts.add(target)
    return sorted(starts)


class FunctionIndex:
    def __init__(self, image, cache=True, workers=None, log=None):
        self.image = image
        self.vocab = []
        self.vocab_index = {}
        self.funcs = {}
        key = f"index-{image.sha1}-v{INDEX_VERSION}-{_code_hash()}.pkl"
        path = CACHE_DIR / key
        if cache and path.is_file():
            with open(path, "rb") as f:
                data = pickle.load(f)
            self.vocab = data["vocab"]
            self.funcs = {st[0]: Func.from_state(st) for st in data["funcs"]}
            if log:
                log(f"函数索引：读缓存 {path.name}（{len(self.funcs)} 个函数）")
        else:
            self._build(workers, log)
            if cache:
                CACHE_DIR.mkdir(parents=True, exist_ok=True)
                tmp = path.with_suffix(".tmp")
                with open(tmp, "wb") as f:
                    pickle.dump({"vocab": self.vocab, "funcs": [fn.state() for fn in self.funcs.values()]}, f, protocol=5)
                os.replace(tmp, path)
        self.vocab_index = {t: i for i, t in enumerate(self.vocab)}
        self._build_maps()
        self._shingle_cache = {}

    def _build(self, workers, log):
        import time
        started = time.time()
        starts = function_starts(self.image)
        workers = workers if workers is not None else min(16, os.cpu_count() or 1)
        results = None
        if workers > 1:
            try:
                from concurrent.futures import ProcessPoolExecutor
                step = max(1, len(starts) // (workers * 4))
                parts = [starts[i:i + step] for i in range(0, len(starts), step)]
                with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init, initargs=(self.image.path,)) as pool:
                    results = list(pool.map(_worker_run, parts))
            except Exception as e:  # noqa: BLE001 - 多进程不可用（例如从标准输入运行脚本）时退回单进程
                if log:
                    log(f"函数索引：多进程失败（{type(e).__name__}: {e}），改用单进程")
                results = None
        if results is None:
            results = [_fingerprint_many(self.image, engine(), Imports(self.image), starts)]
        for local_vocab, records in results:
            remap = []
            for t in local_vocab:
                k = self.vocab_index.get(t)
                if k is None:
                    k = self.vocab_index[t] = len(self.vocab)
                    self.vocab.append(t)
                remap.append(k)
            for rec in records:
                tokens = array("H", (remap[k] for k in rec[4]))
                self.funcs[rec[0]] = Func(rec[0], rec[1], rec[2], rec[3], tokens, *rec[5:])
        if log:
            log(f"函数索引：解码 {len(self.funcs)} 个函数，{time.time() - started:.1f} 秒")

    def _build_maps(self):
        self.callers_of, self.by_string, self.by_global, self.by_import, self.by_imm = {}, {}, {}, {}, {}
        self.by_coderef, self.thunks_to = {}, {}
        for fn in self.funcs.values():
            for t in fn.coderefs:
                self.by_coderef.setdefault(t, []).append(fn.rva)
            if fn.thunk is not None:
                self.thunks_to.setdefault(fn.thunk, []).append(fn.rva)
            for c in fn.callees:
                self.callers_of.setdefault(c, []).append(fn.rva)
            for s in fn.strings:
                self.by_string.setdefault(s, []).append(fn.rva)
            for g in fn.globals:
                self.by_global.setdefault(g, []).append(fn.rva)
            for name in fn.imports:
                self.by_import.setdefault(name, []).append(fn.rva)
            for v in fn.imms:
                self.by_imm.setdefault(v, []).append(fn.rva)
        self.thunks = [fn.rva for fn in self.funcs.values() if fn.thunk is not None]
        self.by_count = sorted((shingle_count(len(fn.tokens)), fn.rva) for fn in self.funcs.values())

    # ---- 相似度 ----

    def ids(self, tokens):
        """token 字符串转成词表下标；词表里没有的 token 给一个不会和任何函数重合的下标。"""
        out = []
        for t in tokens:
            k = self.vocab_index.get(t)
            if k is None:
                k = self.vocab_index[t] = len(self.vocab)
                self.vocab.append(t)
            out.append(k)
        return out

    def shingles(self, rva):
        sh = self._shingle_cache.get(rva)
        if sh is None:
            fn = self.funcs.get(rva)
            sh = shingle_ids(fn.tokens) if fn is not None else None
            if len(self._shingle_cache) > 4096:
                self._shingle_cache.clear()
            self._shingle_cache[rva] = sh
        return sh

    def func(self, rva):
        """索引里没有的函数（例如没被 E8 调用的叶函数）现算一个。"""
        fn = self.funcs.get(rva)
        if fn is None:
            vocab, recs = _fingerprint_many(self.image, engine(), Imports(self.image), [rva])
            rec = recs[0]
            tokens = array("H", self.ids([vocab[k] for k in rec[4]]))
            fn = self.funcs[rva] = Func(rec[0], rec[1], rec[2], rec[3], tokens, *rec[5:])
        return fn

    def similarity(self, old_fp, rva, old_shingles=None):
        """基线里的指纹（tokens 是字符串列表）和本 exe 里 rva 处函数的相似度，同 fp_similarity。"""
        fn = self.func(rva)
        a = old_shingles if old_shingles is not None else shingle_ids(self.ids(old_fp["tokens"]))
        score = dice(a, self.shingles(rva) or shingle_ids(fn.tokens))
        old_thunk = old_fp.get("thunk")
        if old_thunk and fn.thunk is not None:
            target = self.func(fn.thunk)
            score = min(score, dice(shingle_ids(self.ids(old_thunk["tokens"])), shingle_ids(target.tokens)))
        elif bool(old_thunk) != (fn.thunk is not None):
            score = 0.0
        return score

    def search(self, old_fps, min_sim=0.6, ratio=0.6, top=12):
        """全函数指纹搜索：old_fps 是 {键: 基线指纹}，返回 {键: [(相似度, rva), ...]}（降序，最多 top 个，不低于 min_sim）。
        只比较 3-gram 个数之比不小于 ratio 的函数（Dice 的上界 2r/(1+r)，ratio 0.6 对应 0.75）。"""
        olds = []
        for key, fp in old_fps.items():
            sh = shingle_ids(self.ids(fp["tokens"]))
            n = sum(sh.values())
            olds.append((n * ratio, n / ratio, key, fp, sh))
        heaps = {key: [] for key in old_fps}
        thunk_olds = [o for o in olds if o[3].get("thunk")]
        plain = [o for o in olds if not o[3].get("thunk")]
        if plain:
            lo = min(o[0] for o in plain)
            hi = max(o[1] for o in plain)
            start = bisect.bisect_left(self.by_count, (lo, -1))
            for count, rva in self.by_count[start:]:
                if count > hi:
                    break
                matching = [o for o in plain if o[0] <= count <= o[1]]
                if not matching:
                    continue
                fn = self.funcs[rva]
                if fn.thunk is not None:
                    continue
                sh = shingle_ids(fn.tokens)
                for o in matching:
                    s = dice(o[4], sh)
                    if s >= min_sim:
                        _push(heaps[o[2]], (s, rva), top)
        for o in thunk_olds:
            target_sh = shingle_ids(self.ids(o[3]["thunk"]["tokens"]))
            for rva in self.thunks:
                target = self.funcs.get(self.funcs[rva].thunk)
                if target is None:
                    continue
                s = min(dice(o[4], shingle_ids(self.funcs[rva].tokens)), dice(target_sh, shingle_ids(target.tokens)))
                if s >= min_sim:
                    _push(heaps[o[2]], (s, rva), top)
        return {key: sorted(h, reverse=True) for key, h in heaps.items()}


def _push(heap, item, top):
    if len(heap) < top:
        heapq.heappush(heap, item)
    elif item > heap[0]:
        heapq.heapreplace(heap, item)


def shingle_count(n_tokens):
    return max(n_tokens - 2, 1)


def shingle_ids(ids):
    ids = list(ids)
    if len(ids) < 3:
        return Counter([("short",) + tuple(ids)])
    return Counter((a << 32) | (b << 16) | c for a, b, c in zip(ids, ids[1:], ids[2:]))


def dice(a, b):
    total = sum(a.values()) + sum(b.values())
    if total == 0:
        return 1.0
    return 2 * sum((a & b).values()) / total

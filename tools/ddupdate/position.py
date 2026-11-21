"""依据位置核对（U1d）：依据唯一命中时，命中位置要和基线里这条依据的指令位置对上。verify 的同一性检查和修复共用。

为什么要有：依据的模式常靠字面量里的邻居偏移区分（例如「下一个字段是 0x140」）。结构中间插入字段、偏移整体后移时，
模式可能唯一命中邻居字段的代码，读到的值又恰好等于旧常量：依据求值（evidence.py，和 DDCore 同义）报成立，值却是错的。
这种「依据误绑定」只在 Python 侧核对，evidence.py 的求值语义不变（C++ 照它实现，运行时不做这一层）：

  1. 只核对唯一命中的依据（成立的，和唯一命中但值变了的）。ENTRY 范围和起点依赖这一项自己的值（VSLOT_VALUE、
     HIT_VALUE）的依据，命中位置由定义决定，不核对（skip；起点是虚表槽的另外按 5 核对槽里的函数）。
  2. 要有参照：基线里这条依据当时成立，记了命中所在的函数和指令下标（insn_index），基线也存了那个函数的 token 序列
     （format 2 的特征码函数和 functions）。没有参照时结论是 unknown，只记一笔。
  3. 新函数：FUNC、WINDOW 是起点所在的函数；TEXT 是命中所在的函数，它必须和基线里那个函数指纹相似度不低于
     SIMILARITY_THRESHOLD，而且和已确认的对应关系（特征码通过同一性检查得到的新旧对应）不冲突，否则就是「命中落在别的
     函数里」（disagree）。
  4. 指令对齐（align.Aligner，和修复工具同一套：唯一窗口和全局匹配块两种方法一致才算）把基线里的指令下标映射到新函数，
     命中必须落在映射到的那条指令里（agree）；落在别处是 disagree，对齐不可用是 unknown。
  5. 起点是虚表槽（VSLOT、VSLOT_VALUE）的依据，不论范围是 ENTRY 还是 FUNC，先核对槽里的函数和基线里这条依据所在的
     函数是同一个函数（指纹相似度不低于阈值）。虚表插入槽位、后面整体后移时，槽 n 里换成了别的函数，依据的模式可能照样
     成立（例如两个槽的函数都调用同一个取属性的函数），这一条把它报成 disagree。槽里是没有异常目录表项的叶函数时基线
     没存它的指纹，ENTRY 范围的依据照旧不核对；两个形状完全相同的小函数（取字段的 getter）互换时指纹也区分不了，是已知局限。

disagree 在 verify 里归为误绑定（退出码 2）。修复工具把它当作「模式命中的不是原来那条指令」，按放宽模式和指令对齐重新找；
槽里的函数对不上时不在那个函数里重新找（报需要人工），按依据搜索槽号时也要求槽里的函数对得上。
"""
from . import evidence as evidence_mod
from .fingerprint import SIMILARITY_THRESHOLD, fingerprint, fp_similarity

ORIGIN_BY_VALUE = ("VSLOT_VALUE", "HIT_VALUE")
AGREE, DISAGREE, UNKNOWN, SKIP = "agree", "disagree", "unknown", "skip"


def old_functions(baseline):
    """基线里存了 token 序列的函数：rva -> 指纹（特征码函数、目标函数、format 2 的 functions）。"""
    out = {}
    if not baseline:
        return out
    for entry in baseline["sigs"].values():
        for fp in (entry.get("function"), entry.get("target_function")):
            if fp:
                out.setdefault(fp["rva"], fp)
    for fp in baseline.get("functions", []):
        out.setdefault(fp["rva"], fp)
    return out


class PositionCheck:
    """matcher 是 locate.Matcher（提供 aligner、new_of、cs、imports），已用确认的新旧对应关系播种。"""

    def __init__(self, image, baseline, matcher):
        self.image = image
        self.baseline = baseline
        self.matcher = matcher
        self.base_ev = {(r["item"], r["index"]): r for r in baseline["evidence"]} if baseline else {}
        self.old_fps = old_functions(baseline)
        self._aligners = {}
        self._sims = {}

    def record(self, ev):
        return self.base_ev.get((ev.item, ev.index))

    def aligner(self, old_rva, new_rva):
        key = (old_rva, new_rva)
        got = self._aligners.get(key)
        if got is None:
            got = self._aligners[key] = self.matcher.aligner(self.old_fps[old_rva], new_rva)
        return got

    def similarity(self, old_rva, new_rva):
        key = (old_rva, new_rva)
        if key not in self._sims:
            fp = fingerprint(self.image, new_rva, self.matcher.cs, self.matcher.imports)
            self._sims[key] = fp_similarity(fp, self.old_fps[old_rva])
        return self._sims[key]

    def text_function(self, ev, hit):
        """TEXT 依据的新函数：命中所在的函数。返回 (函数, 不成立时的原因)。"""
        rec = self.record(ev)
        here = self.image.enclosing_function(hit)
        if here is None:
            return None, f"命中 {hit:X} 不在异常目录记录的任何函数里（基线里在 {rec['function']:X} 里）"
        mapped = self.matcher.new_of(rec["function"]) if self.matcher is not None else None
        if mapped is not None and mapped != here:
            return None, f"命中落在 {here:X}，基线里这条依据所在的函数 {rec['function']:X} 在新 exe 里对应 {mapped:X}"
        sim = self.similarity(rec["function"], here)
        if sim < SIMILARITY_THRESHOLD:
            return None, (f"命中落在 {here:X}，和基线里这条依据所在的函数 {rec['function']:X} 指纹相似度 {sim:.3f}"
                          f" 低于阈值 {SIMILARITY_THRESHOLD}")
        return here, None

    def new_function(self, view, sig_results, ev, hit=None):
        """修复工具找要读的指令时用：这条依据在新 exe 里所在的函数，找不到时 None。"""
        if ev.where == "TEXT":
            rec = self.record(ev)
            if rec and rec.get("function") is not None and self.matcher is not None:
                mapped = self.matcher.new_of(rec["function"])
                if mapped is not None:
                    return mapped
            if hit is not None and rec and rec.get("function") in self.old_fps:
                here, _why = self.text_function(ev, hit)
                return here
            return None
        origin, error = evidence_mod.origin_of(self.image, view, sig_results, ev)
        if error or origin is None:
            return None
        return self.image.enclosing_function(origin)

    def slot_identity(self, view, sig_results, ev):
        """起点是虚表槽的依据：槽里的函数和基线里这条依据所在的函数是不是同一个函数。返回 (agree / disagree / unknown, 说明)。"""
        rec = self.record(ev)
        if not rec or rec.get("status") != "ok" or rec.get("function") not in self.old_fps or self.matcher is None:
            return UNKNOWN, "基线里没有这个槽位函数的指纹"
        origin, error = evidence_mod.origin_of(self.image, view, sig_results, ev)
        if error or origin is None:
            return UNKNOWN, error or "没有起点"
        func = self.image.enclosing_function(origin) or origin
        sim = self.similarity(rec["function"], func)
        if sim < SIMILARITY_THRESHOLD:
            slot = ev.arg if ev.origin == "VSLOT" else view.value(ev.item)
            return DISAGREE, (f"虚表槽 {slot} 的函数 {func:X} 和基线里这条依据所在的函数 {rec['function']:X} 指纹相似度"
                              f" {sim:.3f} 低于阈值 {SIMILARITY_THRESHOLD}（槽位可能变了）")
        return AGREE, ""

    def check(self, view, sig_results, ev, hit):
        """唯一命中 hit 和基线对齐的位置比较。返回 (agree / disagree / unknown / skip, 说明)。"""
        if hit is None:
            return SKIP, ""
        if ev.origin in ("VSLOT", "VSLOT_VALUE"):
            status, why = self.slot_identity(view, sig_results, ev)
            if status == UNKNOWN and ev.where == "ENTRY":
                # 槽里是没有异常目录表项的叶函数（取字段的 getter 之类），基线没存它的指纹；模式在入口处匹配已经约束了
                # 整个小函数，指纹对这种函数也区分不了，和以前一样不核对
                return SKIP, ""
            if status == DISAGREE or ev.where == "ENTRY" or ev.origin == "VSLOT_VALUE":
                return status, why
        if ev.where == "ENTRY" or ev.origin in ORIGIN_BY_VALUE:
            return SKIP, ""
        rec = self.record(ev)
        if not rec or rec.get("status") != "ok" or rec.get("insn_index") is None or rec.get("function") not in self.old_fps \
                or self.matcher is None:
            return UNKNOWN, "基线里没有这条依据所在函数的指令序列"
        if ev.where == "TEXT":
            new_func, why = self.text_function(ev, hit)
            if new_func is None:
                return DISAGREE, why
        else:
            new_func = self.new_function(view, sig_results, ev)
            if new_func is None:
                return UNKNOWN, "新 exe 里这条依据的起点不在异常目录记录的函数里"
        aligner, insns = self.aligner(rec["function"], new_func)
        j, why = aligner.map(rec["insn_index"])
        if j is None:
            return UNKNOWN, f"指令对齐不可用：{why}"
        ins = insns[j]
        if ins.address <= hit < ins.address + ins.size:
            return AGREE, ""
        return DISAGREE, f"命中 {hit:X}，基线里这条依据的指令（函数内第 {rec['insn_index']} 条）对齐到 {ins.address:X}"

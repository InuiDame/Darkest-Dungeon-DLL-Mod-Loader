"""定位：把基线里的函数在新 exe 里找出来（函数级匹配），再推出每条特征码在新 exe 里的命中位置和解析结果。

函数级匹配（Matcher）。基线里的「节点」是特征码的函数、GLOBAL 指向 .text 的目标函数、thunk 的目标，以及依据所在的
函数（format 2 的 functions）。每个待定位的节点从下面几种手段收集候选（可信度从高到低）：
  放宽特征码  repair 模式才用：原特征码里每条指令的位移、立即数、rel 都写成 ??，在 .text 里扫描（盲定位不用）
  调用图      已确认的节点在基线里调用它 / 被它调用 / 引用它的地址（函数指针）/ 被它引用 / 是它的 thunk 或 thunk
              目标，或者它引用了已确认的全局：候选是新函数的 callees / callers / 引用的函数指针 / 引用新全局的函数等
  锚点        它引用的字符串、导入函数在新 exe 里只被不超过 ANCHOR_MAX 个函数引用；不小于 0x100 的立即数和位移只出现在
              不超过 IMM_MAX 个函数里
  指纹搜索    全部函数里 3-gram 个数相近的，按相似度取前几名（总是做，用来看「第二名」离第一名多远）
每个候选算：相似度（同 verify 的指纹相似度）、调用图约束的满足比例、字符串集合的 Jaccard、调用者个数。
门槛（和 verify 的同一性检查一致，修复结果才能通过 verify）：相似度不低于 SIM_GATE；有可用的调用图约束时满足比例
不低于 REL_GATE；基线里有直接调用者时候选也要有。过门槛的按相似度排序，第一名 T：
  - 没有别的过门槛候选的相似度在 [T - MARGIN, T] 内：选 T。T 另有结构上的支持（放宽特征码命中、调用图约束全部满足、
    新 exe 里唯一引用某个字符串 / 导入 / 常量）时可信度「高」，只靠指纹「中」；有相似度相近、只因调用者 / 调用图门槛
    落选的候选时（模板孪生靠调用者区分），可信度「中」。
  - 有相近的候选（模板孪生、相似函数）：依次看调用图约束满足比例、字符串 Jaccard，某一项上恰好一个候选领先其余的
    DISCRIMINATE_GAP 以上就选它，可信度「中」；都区分不了报「需要人工」，列出候选。
同一轮里所有待定位节点都按上一轮结束时的已确认集合判定，判定完一起提交（结果和处理顺序无关）；两个旧函数选中
同一个新函数时两个都改成「需要人工」。有新确认的就再来一轮，直到没有进展。
"""
from dataclasses import dataclass, field

from capstone.x86 import X86_OP_IMM

from . import align
from . import evidence as evidence_mod
from .fingerprint import SIMILARITY_THRESHOLD, decode_function, fingerprint, fp_similarity, jaccard, token

SIM_GATE = SIMILARITY_THRESHOLD   # 0.85，同一函数跨版本最低 0.929（U1a simstats）
MARGIN = 0.10                     # 第一名要领先其余过门槛候选的相似度差（数据和取舍见 U1c 报告）
REL_GATE = 0.5                    # 调用图约束的满足比例，同 verify 的关系一致度阈值
DISCRIMINATE_GAP = 0.5            # 相近候选之间靠调用图 / 字符串区分时要求的领先量
ANCHOR_MAX = 8                    # 字符串、导入锚点：新 exe 里引用它的函数不超过这么多个才当候选来源
IMM_MAX = 4                       # 立即数锚点
RELAXED_MAX_HITS = 200            # 放宽特征码命中太多时不当候选来源（太泛）
CONSTRAINT_MAX = 2000             # 调用图约束的候选集太大时只当检查用


@dataclass
class Node:
    key: int
    fp: dict
    roles: list = field(default_factory=list)       # [(特征码或依据, 角色)]
    base_callers: list | None = None


@dataclass
class Candidate:
    rva: int
    sim: float = 0.0
    rel_sat: int = 0
    rel_app: int = 0
    strj: float | None = None
    callers: int = 0
    methods: set = field(default_factory=set)
    gated: bool = False
    gate_fail: str = ""

    @property
    def rel(self):
        return self.rel_sat / self.rel_app if self.rel_app else None

    def to_json(self):
        return {"rva": self.rva, "sim": round(self.sim, 4), "rel": None if self.rel is None else round(self.rel, 3),
                "rel_n": f"{self.rel_sat}/{self.rel_app}", "strings": None if self.strj is None else round(self.strj, 3),
                "callers": self.callers, "methods": sorted(self.methods), "gated": self.gated, "gate_fail": self.gate_fail}


@dataclass
class Decision:
    old: int
    new: int | None = None
    status: str = "manual"          # seed / matched / manual
    confidence: str | None = None   # 高 / 中
    methods: list = field(default_factory=list)
    sim: float | None = None
    second: float | None = None     # 过门槛的第二名的相似度（阈值数据）
    runner: float | None = None     # 不论门槛，选中者之外最像的候选的相似度（最近冒充者，阈值数据）
    reasons: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    candidates: list = field(default_factory=list)
    group: list = field(default_factory=list)   # 相近候选（模板孪生）的 rva，GLOBAL_ANY 用

    def to_json(self):
        return {"old": self.old, "new": self.new, "status": self.status, "confidence": self.confidence,
                "methods": self.methods, "sim": None if self.sim is None else round(self.sim, 4),
                "second": None if self.second is None else round(self.second, 4),
                "runner": None if self.runner is None else round(self.runner, 4), "reasons": self.reasons,
                "notes": self.notes, "candidates": [c.to_json() for c in self.candidates], "group": self.group}


class Matcher:
    def __init__(self, baseline, image, index, cs, imports, relaxed=True, log=None):
        self.baseline = baseline
        self.image = image
        self.index = index
        self.cs = cs
        self.imports = imports
        self.relaxed = relaxed
        self.log = log or (lambda text: None)
        self.nodes = {}
        self.matches = {}        # 旧 rva -> Decision（seed / matched）
        self.final = {}          # 旧 rva -> Decision（最后一轮的结论，含 manual）
        self.new_owner = {}      # 新 rva -> 旧 rva
        self.globals = {}        # 旧全局 -> 新全局
        self.hints = {}          # 旧 rva -> {新 rva: {手段}}
        self.search_results = {}
        self._decoded = {}
        for sid, entry in baseline["sigs"].items():
            for role, fp in (("function", entry.get("function")), ("target", entry.get("target_function"))):
                while fp:
                    node = self.nodes.setdefault(fp["rva"], Node(fp["rva"], fp))
                    node.roles.append((sid, role))
                    if role == "function" and entry.get("callers") is not None and fp is entry.get("function"):
                        node.base_callers = entry["callers"]
                    fp = fp.get("thunk")
                    role = "thunk_target"
        for fp in baseline.get("functions", []):
            node = self.nodes.setdefault(fp["rva"], Node(fp["rva"], fp))
            node.roles.append(("evidence", "function"))
        self.node_keys = sorted(self.nodes)

    # ---- 已确认的对应关系 ----

    def seed(self, old, new, method):
        if old not in self.nodes or new is None:
            return
        d = Decision(old, new, "seed", "高", [method])
        self.matches[old] = d
        self.final[old] = d
        self.new_owner[new] = old

    def seed_global(self, old, new):
        if old is not None and new is not None:
            self.globals[old] = new

    def hint(self, old, new, method):
        self.hints.setdefault(old, {}).setdefault(new, set()).add(method)

    def new_of(self, old):
        d = self.matches.get(old)
        return d.new if d else None

    # ---- 候选 ----

    def constraints(self, key):
        node = self.nodes[key]
        callees = set(node.fp["callees"])
        coderefs = _coderefs(node.fp)
        thunk = node.fp["thunk"]["rva"] if node.fp.get("thunk") else None
        out = []
        for other, d in self.matches.items():
            if other == key:
                continue
            ofp = self.nodes[other].fp
            if key in ofp["callees"]:
                out.append(("called_by", d.new, other))
            if other in callees:
                out.append(("calls", d.new, other))
            if key in _coderefs(ofp):
                out.append(("referenced_by", d.new, other))
            if other in coderefs:
                out.append(("references", d.new, other))
            if other == thunk:
                out.append(("thunk_to", d.new, other))
            if ofp.get("thunk") and ofp["thunk"]["rva"] == key:
                out.append(("thunk_of", d.new, other))
        for g in node.fp["globals"]:
            if g in self.globals:
                out.append(("refs", self.globals[g], g))
        return out

    def _check(self, cons, rva):
        fn = self.index.func(rva)
        sat = 0
        for kind, target, _ in cons:
            if kind == "called_by":
                ok = rva in self.index.func(target).callees
            elif kind == "calls":
                ok = target in fn.callees
            elif kind == "referenced_by":
                ok = rva in self.index.func(target).coderefs
            elif kind == "references":
                ok = target in fn.coderefs
            elif kind == "thunk_to":
                ok = fn.thunk == target
            elif kind == "thunk_of":
                ok = self.index.func(target).thunk == rva
            else:
                ok = target in fn.globals
            sat += ok
        return sat

    def _from_constraint(self, kind, target):
        if kind == "called_by":
            return list(self.index.func(target).callees)
        if kind == "calls":
            return self.index.callers_of.get(target, [])
        if kind == "referenced_by":
            return list(self.index.func(target).coderefs)
        if kind == "references":
            return self.index.by_coderef.get(target, [])
        if kind == "thunk_to":
            return self.index.thunks_to.get(target, [])
        if kind == "thunk_of":
            t = self.index.func(target).thunk
            return [t] if t is not None else []
        return self.index.by_global.get(target, [])

    def candidates(self, key):
        node = self.nodes[key]
        fp = node.fp
        pool = {}

        def add(rvas, method):
            for rva in rvas:
                if rva in self.index.funcs or self.image.in_text(rva):
                    pool.setdefault(rva, set()).add(method)

        for rva, methods in self.hints.get(key, {}).items():
            for m in methods:
                add([rva], m)
        cons = self.constraints(key)
        for kind, target, _ in cons:
            rvas = self._from_constraint(kind, target)
            if len(rvas) <= CONSTRAINT_MAX:
                add(rvas, "调用图")
        unique = set()
        for s in fp["strings"]:
            rvas = self.index.by_string.get(s, [])
            if 0 < len(rvas) <= ANCHOR_MAX:
                add(rvas, "字符串")
                if len(rvas) == 1:
                    unique.add(rvas[0])
        for name in fp["imports"]:
            rvas = self.index.by_import.get(name, [])
            if 0 < len(rvas) <= ANCHOR_MAX:
                add(rvas, "导入")
                if len(rvas) == 1:
                    unique.add(rvas[0])
        for v in fp["imms"]:
            rvas = self.index.by_imm.get(v, [])
            if 0 < len(rvas) <= IMM_MAX:
                add(rvas, "常量")
                if len(rvas) == 1:
                    unique.add(rvas[0])
        for _sim, rva in self.search_results.get(key, []):
            add([rva], "指纹搜索")
        return pool, cons, unique

    # ---- 判定 ----

    def decide(self, key):
        node = self.nodes[key]
        fp = node.fp
        pool, cons, unique = self.candidates(key)
        d = Decision(key)
        if not pool:
            d.reasons.append("没有任何候选")
            return d
        from .index import shingle_ids
        old_sh = shingle_ids(self.index.ids(fp["tokens"]))
        cands = []
        for rva, methods in pool.items():
            if rva in self.new_owner and self.new_owner[rva] != key:
                continue
            c = Candidate(rva, methods=set(methods))
            c.sim = self.index.similarity(fp, rva, old_sh)
            if cons:
                c.rel_app = len(cons)
                c.rel_sat = self._check(cons, rva)
            if fp["strings"]:
                c.strj = jaccard(fp["strings"], self.index.func(rva).strings)
            c.callers = len(self.index.callers_of.get(rva, []))
            fails = []
            if c.sim < SIM_GATE:
                fails.append(f"相似度 {c.sim:.3f} < {SIM_GATE}")
            if c.rel is not None and c.rel < REL_GATE:
                fails.append(f"调用图约束 {c.rel_sat}/{c.rel_app}")
            if node.base_callers and not c.callers:
                fails.append("没有直接调用者")
            c.gated = not fails
            c.gate_fail = "，".join(fails)
            cands.append(c)
        cands.sort(key=lambda c: (-c.sim, c.rva))
        d.candidates = cands[:6]
        gated = [c for c in cands if c.gated]
        if not gated:
            d.reasons.append("没有候选过门槛（" + "；".join(f"{c.rva:X} {c.gate_fail}" for c in cands[:3]) + "）")
            return d
        top = gated[0]
        near = [c for c in gated[1:] if c.sim >= top.sim - MARGIN]
        others = [c for c in gated[1:]]
        d.second = others[0].sim if others else None
        chosen, how = None, None
        if not near:
            chosen = top
        else:
            group = [top] + near
            for feature, label in (("rel", "调用图约束"), ("strj", "字符串")):
                values = [getattr(c, feature) for c in group]
                if any(v is None for v in values):
                    continue
                best = max(values)
                leaders = [c for c, v in zip(group, values) if v == best]
                rest = sorted((v for c, v in zip(group, values) if c not in leaders), reverse=True)
                if len(leaders) == 1 and (not rest or best - rest[0] >= DISCRIMINATE_GAP):
                    chosen, how = leaders[0], label
                    break
            if chosen is None:
                d.group = [c.rva for c in group]
                d.reasons.append("相近的候选区分不了（模板孪生或相似函数）："
                                 + "、".join(f"{c.rva:X}（{c.sim:.3f}）" for c in group))
                return d
        d.new = chosen.rva
        d.sim = chosen.sim
        d.runner = max((c.sim for c in cands if c is not chosen), default=None)
        d.status = "matched"
        d.methods = sorted(chosen.methods)
        shadow = [c for c in cands if not c.gated and c.sim >= SIM_GATE and c.sim >= chosen.sim - MARGIN]
        support = []
        if "放宽特征码" in chosen.methods:
            support.append("放宽特征码命中")
        if chosen.rel_app and chosen.rel_sat == chosen.rel_app:
            support.append(f"调用图约束 {chosen.rel_sat}/{chosen.rel_app}")
        if chosen.rva in unique:
            support.append("唯一锚点")
        if how:
            d.confidence = "中"
            d.notes.append(f"相近候选靠{how}区分：" + "、".join(f"{c.rva:X}（{c.sim:.3f}，{c.rel_sat}/{c.rel_app}，"
                                                         f"字符串 {c.strj}）" for c in [top] + near))
        elif shadow:
            d.confidence = "中"
            d.notes.append("相近候选因门槛落选（靠调用者 / 调用图区分）：" + "、".join(f"{c.rva:X}（{c.sim:.3f}，{c.gate_fail}）"
                                                                    for c in shadow))
        elif support:
            d.confidence = "高"
        else:
            d.confidence = "中"
            d.notes.append("只靠指纹相似度")
        if support:
            d.notes.append("支持：" + "，".join(support))
        return d

    def run(self, targets):
        targets = [k for k in targets if k in self.nodes]
        pending = {k for k in targets if k not in self.matches}
        if not pending:
            return
        self.search_results.update(self.index.search({k: self.nodes[k].fp for k in pending if k not in self.search_results}))
        rounds = 0
        while pending:
            rounds += 1
            decisions = {k: self.decide(k) for k in sorted(pending)}
            accepted = {k: d for k, d in decisions.items() if d.status == "matched"}
            by_new = {}
            for k, d in accepted.items():
                by_new.setdefault(d.new, []).append(k)
            for new, ks in by_new.items():
                if len(ks) > 1:
                    for k in ks:
                        d = decisions[k]
                        d.status, d.reasons = "manual", d.reasons + [f"和 {', '.join(f'{x:X}' for x in ks if x != k)} 选中了同一个函数 {new:X}"]
                        accepted.pop(k)
                        pending.discard(k)
            for k, d in decisions.items():
                self.final[k] = d
            if not accepted:
                break
            for k, d in accepted.items():
                self.matches[k] = d
                self.new_owner[d.new] = k
                pending.discard(k)
        self.log(f"函数匹配：{len(targets)} 个节点，{rounds} 轮")

    # ---- 指令对齐 ----

    def decoded(self, rva):
        got = self._decoded.get(rva)
        if got is None:
            _, insns, _ = decode_function(self.image, rva, self.cs)
            refs = align.new_refs(self.image, insns, self.imports)
            got = self._decoded[rva] = (insns, [token(i) for i in insns], refs)
        return got

    def aligner(self, old_fp, new_rva):
        insns, tokens, refs = self.decoded(new_rva)
        matched_new = set(self.new_owner)
        new_globals = set(self.globals.values())

        def tr_old(kind, value):
            if kind == "c":
                return self.new_of(value)
            return self.globals.get(value)

        def tr_new(kind, value):
            if kind == "c":
                return value if value in matched_new else None
            return value if value in new_globals else None

        old_seq = align.enriched(old_fp["tokens"], old_fp.get("refs"), tr_old)
        new_seq = align.enriched(tokens, refs, tr_new)
        return align.Aligner(old_seq, new_seq), insns


# ---- 特征码级的定位 ----

@dataclass
class Location:
    sid: str
    status: str = "manual"            # located / manual
    confidence: str | None = None
    methods: list = field(default_factory=list)
    function: int | None = None       # 新函数
    hit: int | None = None            # 新的命中位置（模式的起点）
    resolved: int | None = None       # 新的解析结果
    insns: list | None = None         # 新函数的指令（重新生成特征码用）
    anchor: int | None = None         # 命中位置在 insns 里的下标
    capture: int | None = None        # capture 所在指令的下标
    capture_field: str | None = None
    reasons: list = field(default_factory=list)
    notes: list = field(default_factory=list)
    decision: Decision | None = None
    group_targets: dict = field(default_factory=dict)

    def to_json(self):
        return {"status": self.status, "confidence": self.confidence, "methods": self.methods, "function": self.function,
                "hit": self.hit, "resolved": self.resolved, "reasons": self.reasons, "notes": self.notes,
                "decision": self.decision.to_json() if self.decision else None,
                "group_targets": {f"{k:X}": v for k, v in self.group_targets.items()}}


CONF_ORDER = {"高": 2, "中": 1, None: 0}


def _coderefs(fp):
    return {v for _i, k, v in fp.get("refs") or [] if k == "f"}


def lower(a, b):
    return a if CONF_ORDER[a] <= CONF_ORDER[b] else b


def old_anchor(entry, sig, cs):
    """基线里命中位置、capture 在旧函数指令序列里的下标：(anchor, capture, field, 说明)。format 2 直接取；format 1 用模式的
    token 在旧序列里找，必须唯一。"""
    fn = entry["function"]
    anchor = entry.get("anchor")
    if anchor and anchor.get("insn") is not None:
        return anchor["insn"], anchor.get("capture_insn"), anchor.get("capture_field"), "基线 anchor"
    if sig.kind in ("HOOK", "CODE") and entry["hit"] == fn["rva"]:
        return 0, None, None, "函数入口"
    tokens = align.pattern_tokens(sig.pattern, cs, sig.entry)
    positions = align.unique_positions(fn["tokens"], tokens) if tokens else []
    if len(positions) != 1:
        return None, None, None, f"基线是 format 1，模式的指令序列在旧函数里出现 {len(positions)} 次，定不了锚点指令"
    capture, kind = None, None
    if sig.kind in ("GLOBAL", "GLOBAL_ANY"):
        found = align.locate_in_pattern(sig.pattern, cs, sig.capture, sig.entry)
        if found is None or found[2] is None:
            return None, None, None, "capture 不在模式里完整的指令上"
        capture, kind = positions[0] + found[0], found[2]
    return positions[0], capture, kind, "模式在旧函数里唯一"


def capture_target(image, ins, kind):
    if kind == "disp" and align.is_rip(ins) and ins.disp_size == 4:
        return ins.address + ins.size + ins.disp
    if kind == "imm" and ins.imm_size == 4 and ins.operands and ins.operands[0].type == X86_OP_IMM \
            and ins.mnemonic in ("call", "jmp"):
        return ins.operands[0].imm
    return None


def locate_sig(m, sig, entry, direct=None):
    """在 Matcher 跑完之后推出一条特征码的新位置。direct：放宽特征码的命中 {新函数: [命中位置]}（repair 模式）。"""
    loc = Location(sig.id)
    fn = entry["function"]
    if not fn:
        loc.reasons.append("基线里这条特征码没有函数（基线生成时就失效了）")
        return loc
    d = m.final.get(fn["rva"]) or m.matches.get(fn["rva"])
    loc.decision = d
    if d is None:
        loc.reasons.append("函数没有参与匹配")
        return loc
    i_old, k_old, kind, how = old_anchor(entry, sig, m.cs)
    if i_old is None:
        loc.reasons.append(how)
        return loc
    entry_kind = sig.kind in ("HOOK", "CODE") and entry["hit"] == fn["rva"]
    if d.status not in ("seed", "matched"):
        if sig.kind == "GLOBAL_ANY" and d.group:
            return _locate_group(m, sig, entry, loc, d, i_old, k_old, kind)
        loc.reasons += d.reasons
        return loc
    loc.function = d.new
    loc.confidence = d.confidence
    loc.methods = list(d.methods)
    loc.notes += d.notes
    if entry_kind:
        loc.hit = loc.resolved = d.new
        insns, _tokens, _refs = m.decoded(d.new)
        loc.insns, loc.anchor = insns, 0
        if not insns or insns[0].address != d.new:
            loc.reasons.append(f"新函数 {d.new:X} 解码失败")
            return loc
        loc.status = "located"
        return loc
    return _locate_inside(m, sig, entry, loc, d.new, i_old, k_old, kind, (direct or {}).get(d.new))


def _map(m, fp, new_rva, i_old, direct_hits):
    """旧指令下标 -> 新指令下标：放宽特征码在这个函数里恰好命中一次时直接用（再和对齐比较），否则用指令对齐。
    返回 (下标, 方法, 可信度, 原因)。"""
    aligner, insns = m.aligner(fp, new_rva)
    j, detail = aligner.map(i_old)
    if direct_hits is not None and len(direct_hits) == 1:
        jd = next((n for n, ins in enumerate(insns) if ins.address == direct_hits[0]), None)
        if jd is None:
            return None, None, None, f"放宽特征码的命中 {direct_hits[0]:X} 不在指令边界上"
        if j is not None and j != jd:
            return None, None, None, f"放宽特征码的命中（指令 {jd}）和指令对齐（指令 {j}）不一致"
        return jd, "放宽特征码直接命中" + ("，指令对齐一致" if j is not None else ""), "高", None
    if j is None:
        return None, None, None, f"指令对齐失败：{detail}"
    return j, f"指令对齐（{detail}）", "中", None


def _locate_inside(m, sig, entry, loc, new_rva, i_old, k_old, kind, hits):
    fn = entry["function"]
    hits = sorted(set(hits)) if hits else None
    j, method, conf, why = _map(m, fn, new_rva, i_old, hits)
    if j is None:
        loc.reasons.append(f"锚点指令：{why}")
        return loc
    insns, tokens, _refs = m.decoded(new_rva)
    loc.insns, loc.anchor = insns, j
    loc.hit = insns[j].address
    loc.methods.append(method)
    if sig.kind in ("HOOK", "CODE"):
        loc.resolved = loc.hit
        loc.confidence = lower(loc.confidence, conf)
        loc.status = "located"
        return loc
    if k_old is None:
        loc.reasons.append("基线里没有 capture 所在的指令")
        return loc
    if hits is not None and len(hits) == 1:
        k = j + (k_old - i_old)
    else:
        k, detail = m.aligner(fn, new_rva)[0].map(k_old)
        if k is None:
            loc.reasons.append(f"capture 指令对齐失败：{detail}")
            return loc
    if k < j or k >= len(insns) or tokens[k] != fn["tokens"][k_old]:
        loc.reasons.append(f"capture 指令对不上（新下标 {k}）")
        return loc
    target = capture_target(m.image, insns[k], kind)
    if target is None:
        loc.reasons.append(f"新函数里 capture 指令 {insns[k].mnemonic} {insns[k].op_str} 没有 {kind} 字段")
        return loc
    loc.capture, loc.capture_field = k, kind
    loc.resolved = target
    conf_target, problems, notes = check_target(m, sig, entry, target)
    loc.notes += notes
    if problems:
        loc.reasons += problems
        return loc
    if conf == "中" and conf_target == "高":
        conf = "高"
        loc.notes.append("对齐得到的全局由交叉检查确认")
    loc.confidence = lower(loc.confidence, conf)
    loc.status = "located"
    return loc


def check_target(m, sig, entry, target):
    """GLOBAL 解析出的新地址的交叉检查：节和基线一致；指向 .text 时目标函数的指纹要过门槛，独立匹配过的要一致；
    指向数据时，基线里引用旧全局、已经确认的其他函数，现在要引用新全局。返回 (能否把可信度提到高, 问题, 说明)。"""
    image = m.image
    problems, notes = [], []
    section = image.section_of(target)
    if section != entry["section"]:
        problems.append(f"新地址 {target:X} 在 {section}，基线在 {entry['section']}")
        return None, problems, notes
    tf = entry.get("target_function")
    if tf:
        if target not in m.index.funcs and not image.is_function_start(target):
            problems.append(f"目标 {target:X} 不是函数入口")
            return None, problems, notes
        sim = m.index.similarity(tf, target)
        if sim < SIM_GATE:
            problems.append(f"目标函数相似度 {sim:.3f} 低于 {SIM_GATE}")
            return None, problems, notes
        independent = m.new_of(tf["rva"])
        if independent is not None and independent != target:
            problems.append(f"目标 {target:X} 和独立匹配到的 {independent:X} 不一致")
            return None, problems, notes
        notes.append(f"目标函数相似度 {sim:.3f}" + ("，和独立匹配一致" if independent == target else ""))
        return ("高" if independent == target else None), problems, notes
    old_global = entry["resolved"]
    sat = app = 0
    for other, d in m.matches.items():
        if other == entry["function"]["rva"]:
            continue
        if old_global in m.nodes[other].fp["globals"]:
            app += 1
            sat += target in m.index.func(d.new).globals
    if app:
        notes.append(f"引用旧全局的已确认函数 {sat}/{app} 现在引用新地址")
        # 只有一个旁证时它不引用新地址不算反证（那个函数本身可能改了，例如 Steam 版和离线版的 StateNames 所在函数），
        # 但可信度不能提到高；两个以上旁证时过半数不引用就报需要人工
        if app >= 2 and sat * 2 < app:
            problems.append(f"引用旧全局的已确认函数只有 {sat}/{app} 引用新地址 {target:X}")
            return None, problems, notes
    return ("高" if app and sat == app else None), problems, notes


def _locate_group(m, sig, entry, loc, d, i_old, k_old, kind):
    """GLOBAL_ANY 的模板实例：相近候选全部对齐、都解析到同一个地址时可以定位（可信度中）。"""
    fn = entry["function"]
    targets = {}
    for rva in d.group:
        aligner, insns = m.aligner(fn, rva)
        j, detail = aligner.map(i_old)
        k, detail2 = aligner.map(k_old) if k_old is not None else (None, "没有 capture 指令")
        if j is None or k is None:
            loc.reasons.append(f"候选 {rva:X} 对齐失败：{detail if j is None else detail2}")
            return loc
        target = capture_target(m.image, insns[k], kind)
        if target is None:
            loc.reasons.append(f"候选 {rva:X} 的 capture 指令没有 {kind} 字段")
            return loc
        targets[rva] = (j, k, target)
    loc.group_targets = {rva: t[2] for rva, t in targets.items()}
    distinct = sorted({t[2] for t in targets.values()})
    if len(distinct) != 1:
        loc.reasons.append(f"{len(d.group)} 个相近候选解析出 {len(distinct)} 个不同地址 "
                           + "、".join(f"{t:X}" for t in distinct)
                           + ("（DD_RUNTIME_SELECT：只有运行时能按内容选）" if sig.runtime_select else ""))
        return loc
    rva = min(targets)
    j, k, target = targets[rva]
    conf_target, problems, notes = check_target(m, sig, entry, target)
    loc.notes += notes
    if problems:
        loc.reasons += problems
        return loc
    insns, _t, _r = m.decoded(rva)
    loc.function, loc.insns, loc.anchor, loc.capture, loc.capture_field = rva, insns, j, k, kind
    loc.hit, loc.resolved = insns[j].address, target
    loc.methods = ["模板实例"]
    loc.notes.append(f"{len(d.group)} 个相近候选（模板实例）都解析到 {target:X}")
    loc.confidence = "中"
    loc.status = "located"
    return loc


# ---- 种子：已确认的新旧对应关系（repair 和 verify 的依据位置核对共用） ----

def new_function_of(image, fp_old, anchor):
    """基线指纹的 rva 规则：在异常目录里时是包含锚点的主函数，否则是锚点本身。"""
    if fp_old.get("pdata"):
        return image.enclosing_function(anchor)
    return anchor


def seed_from_ok(m, image, manifest, baseline, sig_results, ok_sigs, cs, imports):
    """ok_sigs：特征码检查和同一性检查都通过的特征码。它们的函数、目标函数、thunk 目标、全局当作已确认。"""
    for sid in ok_sigs:
        sig, entry, res = manifest.sigs[sid], baseline["sigs"].get(sid), sig_results[sid]
        if not entry or entry["status"] != "ok" or not entry["function"]:
            continue
        fn = entry["function"]
        anchor = res.resolved if sig.kind in ("HOOK", "CODE") else res.hit
        new_fn = new_function_of(image, fn, anchor)
        m.seed(fn["rva"], new_fn, f"{sid} 校验通过")
        if fn.get("thunk") and new_fn is not None:
            now = fingerprint(image, new_fn, cs, imports)
            if now.get("thunk"):
                m.seed(fn["thunk"]["rva"], now["thunk"]["rva"], f"{sid} 的 thunk 目标")
        if entry.get("target_function"):
            m.seed(entry["target_function"]["rva"], res.resolved, f"{sid} 的目标函数")
        if sig.kind in ("GLOBAL", "GLOBAL_ANY") and entry["section"] in (".data", ".rdata") and not sig.runtime_select:
            m.seed_global(entry["resolved"], res.resolved)


def seed_from_evidence(m, image, manifest, baseline, sig_results, results=None):
    """成立的依据：基线里依据所在的函数（format 2 的 functions）对应新 exe 里命中所在的函数。命中所在的函数和基线里
    那个函数指纹相似度低于阈值时不播种（依据可能命中了别的函数，U1d 的依据位置核对会报出来）。
    results：(清单项, 序号) -> EvidenceResult，已经求值过时传入，省得重算。返回没能确认的依据函数。"""
    extra = {fp["rva"]: fp for fp in baseline.get("functions", [])}
    records = {(r["item"], r["index"]): r for r in baseline["evidence"]}
    failing_nodes = set()
    for ev in manifest.evidence:
        rec = records.get((ev.item, ev.index))
        if not rec or rec.get("function") not in extra:
            continue
        er = (results or {}).get((ev.item, ev.index)) or evidence_mod.evaluate(image, manifest, sig_results, ev)
        if er.status == "ok" and er.hit is not None:
            new_fn = image.enclosing_function(er.hit)
            if new_fn is not None and rec["function"] not in m.matches:
                now = fingerprint(image, new_fn, m.cs, m.imports)
                if fp_similarity(now, extra[rec["function"]]) >= SIMILARITY_THRESHOLD:
                    m.seed(rec["function"], new_fn, f"{ev.item} 的依据 {ev.index} 成立")
                else:
                    failing_nodes.add(rec["function"])
        else:
            failing_nodes.add(rec["function"])
    return failing_nodes - set(m.matches)

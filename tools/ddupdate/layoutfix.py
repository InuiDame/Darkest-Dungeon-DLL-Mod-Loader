"""布局项的修复：在（修好特征码之后的）新 exe 上重新求值依据，给出新值和新的依据模式。

每条运行时依据按下面的顺序处理，结论是「这一项应该等于多少」的提议（值、可信度、手段）：
  1. 依据成立：提议当前值（高）。read=NONE 或 mul=0 的依据不提议值，只要求成立。
  2. 模式唯一命中、读到的值变了：按 EvidenceResult.implied 提议新值（高）。
     1、2 之前先做依据位置核对（position.py，和 verify 同一套）：唯一命中要落在基线里这条依据的指令对齐到的位置，
     对不上（疑似依据误绑定）按 3 重新找。没法对照（unknown）时不当作成立（U1 修复 FB，审核 A 的 B1）：也按 3 重新找，
     3 要靠指令对齐确认位置；找不到或确认不了时这条依据记为「位置无法确认」（见下面的佐证规则）。
  3. 模式不唯一命中（或位置对不上、没法对照）：
     a. 放宽依据模式（每条指令的位移、立即数都写成 ??）在范围里唯一命中；
     b. 放宽后多处命中时，用基线里这条依据命中的指令下标（insn_index）和旧函数的 token 序列（基线的特征码函数或
        format 2 的 functions），对齐到新函数，选恰好落在对齐位置上的那个命中；
     c. 放宽后不命中时，只用指令对齐。
     找到要读的指令后从同一种字段读值（字段宽度可以变），提议新值（中，比 2 低一级），并在那条指令处生成新的依据模式。
     a 的唯一命中没有指令对齐的确认时（对齐有歧义、给不出结果），这个位置同样是「位置无法确认」：放宽的模式在一串同形状
     的字段里可能正好错开一格（审核 A 的 B1：在要读的字段之后插入字段时，放宽的模式会命中后一个字段）。
     新模式尽量保住原模式的范围（原来在要读的指令前后各有几条指令，新模式也带上几条），只在保不住时退回最短的唯一模式，
     并在报告里写明（FA 收紧过的依据带着同一串复制和界标，退回最短模式会丢掉这层保护）。
  佐证规则（B1）：「位置无法确认」的依据最多让这一项需要人工，除非同一项里另有位置确认过的依据（位置对上，或者重新定位时
  指令对齐一致）给出同样的值；没有佐证的项报需要人工，原因写进 report.txt。预演 verify 再按同样的规则（verify.py 的
  「无法确认」）复核一遍，不通过的改成需要人工。
  4. 起点依赖这一项的值（VSLOT_VALUE、HIT_VALUE）的依据不成立时，在一定范围里搜索新值（虚表槽号 0..原值+32，
     命中偏移 0..max(256, 2*原值)），所有这类依据都成立的值恰好一个时提议它（中）。
一项的全部提议必须一致，不一致报「需要人工」。rel 是别的清单项时用它的新值，所以整个过程迭代到不再变化（最多 6 轮）。
最后用新值（和新模式）把这一项的全部依据再求值一遍，全部成立；有离线检查的，用新值重跑，全部通过。
只有离线依据的项：离线检查通过就是 ok，不通过报「需要人工」（离线检查不给出新值）。没有静态依据的项不修，列进
「更新后要人工核实」。
"""
import copy
import dataclasses

from . import align
from . import evidence as evidence_mod
from . import offline as offline_mod
from . import regen
from .fingerprint import token
from .image import compile_pattern
from .position import ORIGIN_BY_VALUE, PositionCheck

ROUNDS = 6
SLOT_EXTRA = 32
CONF_RANK = {"高": 2, "中": 1}


@dataclasses.dataclass
class EvidenceFix:
    index: int
    line: int
    before: str                      # 修复前的求值状态（用修好的特征码）
    method: str = ""
    proposal: int | None = None
    confidence: str | None = None
    new: object = None               # 新的 Evidence（模式变了时）
    detail: str = ""
    confirmed: bool = False          # 位置确认过：位置核对对上（或按定义不用核对），或者重新定位时指令对齐一致
    unconfirmed: str = ""            # 位置无法确认的原因（B1）；非空时这条依据的结论要别的依据佐证

    def to_json(self):
        out = {"index": self.index, "line": self.line, "before": self.before, "method": self.method,
               "proposal": self.proposal, "confidence": self.confidence, "detail": self.detail,
               "confirmed": self.confirmed, "unconfirmed": self.unconfirmed}
        if isinstance(self.new, tuple):
            # 找到了要读的指令、还没生成新模式（这一项后来判成需要人工，finish 没有走到生成那一步）
            ins, _kind = self.new
            out.update(located=f"{ins.address:X} {ins.mnemonic} {ins.op_str}")
        elif self.new is not None:
            out.update(pattern=self.new.pattern, at=self.new.at, read=self.new.read)
        return out


@dataclasses.dataclass
class ItemFix:
    id: str
    need: str
    check: str
    old: int
    new: int | None = None
    status: str = "ok"               # ok / repaired-高 / repaired-中 / manual / unverified
    confidence: str | None = None
    evidence: list = dataclasses.field(default_factory=list)
    offline: list = dataclasses.field(default_factory=list)
    reasons: list = dataclasses.field(default_factory=list)
    notes: list = dataclasses.field(default_factory=list)

    def to_json(self):
        return {"need": self.need, "check": self.check, "old": self.old, "new": self.new, "status": self.status,
                "confidence": self.confidence, "evidence": [e.to_json() for e in self.evidence], "offline": self.offline,
                "reasons": self.reasons, "notes": self.notes}


def manifest_view(manifest, values):
    view = copy.copy(manifest)
    view.items = dict(manifest.items)
    for iid, value in values.items():
        view.items[iid] = dataclasses.replace(manifest.items[iid], value=value)
    return view


class LayoutRepair:
    def __init__(self, image, manifest, sig_results, baseline, matcher, cs, sig_conf):
        self.image = image
        self.manifest = manifest
        self.sig_results = sig_results
        self.baseline = baseline
        self.matcher = matcher
        self.cs = cs
        self.sig_conf = sig_conf          # 特征码 -> 可信度（ok 的是高）
        self.pos = PositionCheck(image, baseline, matcher)
        self.base_ev = self.pos.base_ev
        self.old_fps = self.pos.old_fps

    # ---- 单条依据 ----

    def _origin_ranges(self, view, ev):
        origin, error = evidence_mod.origin_of(self.image, view, self.sig_results, ev)
        if error:
            return None, None, error
        _regex, length = compile_pattern(ev.pattern)
        ranges, error = evidence_mod.ranges_of(self.image, ev, origin, length)
        return origin, ranges, error

    def _new_function(self, view, ev):
        return self.pos.new_function(view, self.sig_results, ev)

    def realign(self, view, ev):
        """模式不唯一命中时找要读的指令。返回 (指令, 字段, 手段, 原因, 指令对齐是否确认了这个位置)。"""
        ins, kind, method, why, aligned = self._realign(view, ev)
        return ins, kind, method, why, aligned

    def _realign(self, view, ev):
        origin, ranges, error = self._origin_ranges(view, ev)
        if error:
            return None, None, None, error, False
        if ev.origin in ("VSLOT", "VSLOT_VALUE"):
            status, why = self.pos.slot_identity(view, self.sig_results, ev)
            if status != "agree":
                return None, None, None, f"不在槽里的函数里重新找：{why}", False
        if ev.read != "NONE":
            found = align.locate_in_pattern(ev.pattern, self.cs, ev.at)
            if found is None or found[2] is None:
                return None, None, None, "读取位置不在模式里某条指令的位移或立即数字段上", False
            k, _old_ins, kind, _off = found
        else:
            k, kind = 0, None
        _lead, pins = align.decode_pattern(ev.pattern, self.cs)
        if not pins:
            return None, None, None, "依据模式解码不了", False
        read_off = pins[k].address
        relaxed, _n = align.relax_pattern(ev.pattern, self.cs)
        regex, _ = compile_pattern(relaxed)
        hits = []
        for b, e in ranges:
            hits += self.image.scan(regex, b, e)
        # 对齐
        aligned, align_why = None, "基线里没有这条依据所在函数的指令序列"
        rec = self.base_ev.get((ev.item, ev.index))
        new_func = self._new_function(view, ev)
        if rec and rec.get("function") in self.old_fps and rec.get("insn_index") is not None and new_func is not None \
                and self.matcher is not None:
            aligner, insns = self._aligner(rec["function"], new_func)
            j, align_why = aligner.map(rec["insn_index"] + k)
            if j is not None:
                aligned = insns[j]
        elif new_func is None:
            align_why = "新 exe 里这条依据所在的函数没有确认"
        chosen, method = None, None
        if len(hits) == 1:
            addr = hits[0] + read_off
            if aligned is not None and aligned.address != addr:
                return None, None, None, f"放宽模式的命中和指令对齐不一致（{addr:X} / {aligned.address:X}）", False
            chosen = addr
            method = "放宽依据模式唯一命中" + ("，指令对齐一致" if aligned is not None else f"（指令对齐给不出结果：{align_why}）")
        elif len(hits) > 1:
            if aligned is None:
                return None, None, None, f"放宽模式命中 {len(hits)} 次，指令对齐不可用：{align_why}", False
            if aligned.address - read_off not in hits:
                return None, None, None, f"放宽模式命中 {len(hits)} 次，都不在对齐位置 {aligned.address:X}", False
            chosen, method = aligned.address, f"放宽依据模式（{len(hits)} 处）里选对齐位置"
        else:
            if aligned is None:
                return None, None, None, f"放宽模式不命中，指令对齐不可用：{align_why}", False
            chosen, method = aligned.address, "指令对齐"
        ins = next(iter(self.cs.disasm(self.image.bytes_at(chosen, 16), chosen)), None)
        if ins is None:
            return None, None, None, f"{chosen:X} 解码失败", False
        if token(ins) != token(pins[k]):
            return None, None, None, f"新指令 {ins.mnemonic} {ins.op_str} 和原模式里的指令种类不同", False
        if kind and align.field_span(ins, kind) is None:
            return None, None, None, f"新指令 {ins.mnemonic} {ins.op_str} 没有 {kind} 字段", False
        return ins, kind, method, None, aligned is not None

    def read_field(self, view, ev, ins, kind):
        span = align.field_span(ins, kind)
        read = regen.read_for(ev.read, span[1])
        if read is None:
            return None, f"字段宽度 {span[1]} 不适合 {ev.read}"
        value = evidence_mod.read_value(self.image, ins.address + span[0], read)
        return value, (None if value is not None else "读取位置或 rip 相对的目标超出映像")

    def implied(self, view, ev, captured):
        rel = evidence_mod.rel_value(view, self.sig_results, ev.rel)
        if rel is None or not ev.mul:
            return None
        rest = captured - rel - ev.add
        return rest // ev.mul if rest % ev.mul == 0 else None

    def _aligner(self, old_rva, new_rva):
        return self.pos.aligner(old_rva, new_rva)

    def position(self, view, ev, hit):
        """依据的唯一命中和基线对齐的位置比较（防依据误绑定：模式还唯一命中、命中的却是别处），规则见 position.py，
        和 verify 的依据位置核对同一套。返回 (agree / disagree / unknown / skip, 说明)。"""
        return self.pos.check(view, self.sig_results, ev, hit)

    def _unconfirmed(self, fix, ev, er, current, why, anchor_conf):
        """B1：命中位置没法和基线对照、重新定位也确认不了。结论（当前值或按读到的值反推的新值）照样给出，标成「位置无法
        确认」，由 assess_item 按佐证规则决定这一项是不是需要人工。"""
        fix.method = "位置无法确认"
        fix.unconfirmed = why
        fix.detail = why
        if er.status == "ok":
            fix.proposal = current if ev.read != "NONE" and ev.mul else None
        else:
            fix.proposal = er.implied
        fix.confidence = regen_lower(anchor_conf)
        return fix

    def assess_evidence(self, view, item, ev, current):
        fix = EvidenceFix(ev.index, ev.line, "")
        er = evidence_mod.evaluate(self.image, view, self.sig_results, ev)
        fix.before = er.status
        anchor_conf = self.sig_conf.get(ev.sig, "高") if ev.sig != "NONE" else "高"
        if er.status == "blocked":
            fix.method = "blocked"
            fix.detail = er.detail
            return fix
        unique = len(er.hits) == 1
        changed = unique and er.status != "ok" and er.captured is not None and ev.mul and er.implied is not None
        suspect = ""
        unknown = ""
        if er.status == "ok" or changed:
            pos, why = self.position(view, ev, er.hit)
            if pos == "disagree":
                suspect = f"命中位置和基线不一致（疑似依据误绑定）：{why}"
            elif pos == "unknown":
                unknown = f"命中位置没法和基线对照（{why}）"
            elif er.status == "ok":
                fix.method = "依据成立"
                if ev.read != "NONE" and ev.mul:
                    fix.proposal = current
                fix.confidence = anchor_conf
                fix.confirmed = True
                return fix
            else:
                fix.method, fix.proposal = "模式命中，值变了", er.implied
                fix.detail = er.detail
                fix.confidence = anchor_conf
                fix.confirmed = True
                return fix
        elif unique and er.captured is not None:
            fix.method = "值不符"
            fix.detail = er.detail
            return fix
        if ev.origin in ORIGIN_BY_VALUE:
            # 起点就是这一项的值，不能在别处重新找，只能按依据搜索值
            if unknown:
                return self._unconfirmed(fix, ev, er, current, unknown + "；起点依赖这一项的值，不能在别处重新找", anchor_conf)
            fix.method = "定位失败"
            fix.detail = suspect or er.detail
            return fix
        if ev.read == "NONE":
            if unknown:
                return self._unconfirmed(fix, ev, er, current, unknown + "；read=NONE 的依据不能重新生成", anchor_conf)
            fix.method = "定位失败"
            fix.detail = (suspect or er.detail) + "；read=NONE 的依据把值写在模式的字面量里，模式失效时不能自动重新生成"
            return fix
        ins, kind, method, why, aligned = self.realign(view, ev)
        if ins is None:
            if unknown:
                return self._unconfirmed(fix, ev, er, current, f"{unknown}；重新定位也确认不了：{why}", anchor_conf)
            fix.method = "定位失败"
            fix.detail = "；".join(x for x in (suspect, er.detail, why) if x)
            return fix
        fix.method = method
        fix.confidence = regen_lower(anchor_conf)
        captured, why = self.read_field(view, ev, ins, kind)
        if captured is None:
            fix.method, fix.detail = "定位失败", why
            fix.confidence = None
            return fix
        fix.detail = "；".join(x for x in (suspect, unknown) if x)
        fix.detail += ("；" if fix.detail else "") + f"{ins.address:X} {ins.mnemonic} {ins.op_str}，读到 {captured:#x}"
        if ev.mul:
            fix.proposal = self.implied(view, ev, captured)
            if fix.proposal is None:
                fix.method, fix.detail = "定位失败", fix.detail + "，反推不出整数值"
                fix.confidence = None
                return fix
        if aligned:
            fix.confirmed = True
        else:
            fix.unconfirmed = "放宽的模式唯一命中，但指令对齐给不出结果，这个位置没有对照确认"
        fix.new = (ins, kind)
        return fix

    # ---- 搜索起点依赖值的项 ----

    def search_value(self, view_values, item):
        evs = [ev for ev in item.evidence if ev.origin in ("VSLOT_VALUE", "HIT_VALUE")]
        if not evs:
            return None, "没有起点依赖这一项的依据"
        if any(ev.origin == "VSLOT_VALUE" for ev in evs):
            hi = item.value + SLOT_EXTRA
            limit = range(0, hi + 1)
        else:
            limit = range(0, max(256, 2 * item.value) + 1)
        found = []
        for v in limit:
            values = dict(view_values)
            values[item.id] = v
            view = manifest_view(self.manifest, values)
            results = [(ev, evidence_mod.evaluate(self.image, view, self.sig_results, ev)) for ev in evs]
            # 槽号：依据成立之外，槽里的函数还要和基线里的是同一个函数（position.py 的第 5 条）
            if all(er.status == "ok" and self.pos.check(view, self.sig_results, ev, er.hit)[0] != "disagree"
                   for ev, er in results):
                found.append(v)
                if len(found) > 1:
                    break
        if len(found) == 1:
            return found[0], None
        return None, ("搜索不到满足依据的值" if not found else f"多个值满足依据（{found[0]}、{found[1]} …）")

    # ---- 整体 ----

    def run(self):
        items = [i for i in self.manifest.items.values()]
        values = {}
        fixes = {}
        first = {}   # 第一次不是「依据成立」的结论（后面几轮用新值求值时会变成成立，报告里保留原来的手段）
        floor = {}   # 每项历轮里最低的可信度（新值是低可信度的手段给出的，后面几轮用新值求值成立也不能升级）
        notes = {}
        for _round in range(ROUNDS):
            view = manifest_view(self.manifest, values)
            changed = False
            for item in items:
                if item.check != "RUNTIME":
                    continue
                current = values.get(item.id, item.value)
                fix = self.assess_item(view, values, item, current)
                fixes[item.id] = fix
                if fix.status != "manual" and fix.confidence:
                    old = floor.get(item.id)
                    if old is None or CONF_RANK[fix.confidence] < CONF_RANK[old]:
                        floor[item.id] = fix.confidence
                for n in fix.notes:
                    if n not in notes.setdefault(item.id, []):
                        notes[item.id].append(n)
                for ef in fix.evidence:
                    if ef.method != "依据成立":
                        first.setdefault((item.id, ef.index), (ef.method, ef.detail))
                if fix.new is not None and fix.new != current and fix.status != "manual":
                    values[item.id] = fix.new
                    changed = True
            if not changed:
                break
        view = manifest_view(self.manifest, values)
        out = {}
        for item in items:
            if item.check == "NONE":
                out[item.id] = ItemFix(item.id, item.need, item.check, item.value, status="unverified")
                continue
            if item.check == "OFFLINE":
                fix = ItemFix(item.id, item.need, item.check, item.value)
            else:
                fix = fixes[item.id]
                for ef in fix.evidence:
                    if ef.method == "依据成立" and (item.id, ef.index) in first:
                        ef.method, ef.detail = first[(item.id, ef.index)]
                if item.id in floor and fix.confidence and CONF_RANK[floor[item.id]] < CONF_RANK[fix.confidence]:
                    fix.confidence = floor[item.id]
                fix.notes = notes.get(item.id, fix.notes)
            self.finish(view, item, fix)
            out[item.id] = fix
        return out, values

    def assess_item(self, view, values, item, current):
        fix = ItemFix(item.id, item.need, item.check, item.value)
        props = []
        need_search = False
        for ev in item.evidence:
            ef = self.assess_evidence(view, item, ev, current)
            fix.evidence.append(ef)
            if ef.method == "blocked":
                fix.reasons.append(f"依据 {ev.index}：{ef.detail}")
            elif ef.method in ("定位失败", "值不符"):
                if ev.origin in ORIGIN_BY_VALUE:
                    need_search = True
                else:
                    fix.reasons.append(f"依据 {ev.index}：{ef.detail}")
            elif ef.proposal is not None:
                props.append((ef.proposal, ef.confidence, ev.index, ef.method))
        # 佐证规则（B1，见文件开头）：位置无法确认的依据要有同一项里位置确认过的依据给出同样的值（或者都不给值时，
        # 有位置确认过的依据），否则这一项需要人工
        for ef in fix.evidence:
            if not ef.unconfirmed:
                continue
            backers = [o for o in fix.evidence if o is not ef and o.confirmed
                       and (o.proposal is None or ef.proposal is None or o.proposal == ef.proposal)]
            if backers:
                fix.notes.append(f"依据 {ef.index} 位置无法确认（{ef.unconfirmed}），由依据 {[o.index for o in backers]} 佐证")
            else:
                fix.reasons.append(f"依据 {ef.index} 位置无法确认：{ef.unconfirmed}；同一项没有位置确认过的依据给出同样的值，"
                                   "要人工核对（审核 A 的 B1：没法对照不算通过）")
        if need_search:
            v, why = self.search_value(values, item)
            if v is None:
                fix.reasons.append(f"按依据搜索新值：{why}")
            else:
                props.append((v, "中", "search", "按依据搜索"))
                fix.notes.append(f"按起点依赖值的依据搜索到 {v:#x}")
        if fix.reasons:
            fix.status = "manual"
            return fix
        distinct = sorted({p[0] for p in props})
        if len(distinct) > 1:
            fix.status = "manual"
            fix.reasons.append("依据的结论不一致：" + "，".join(f"依据 {p[2]}（{p[3]}）→ {p[0]:#x}" for p in props))
            return fix
        if distinct:
            fix.new = distinct[0]
        else:
            fix.new = current
        fix.confidence = min((p[1] for p in props), key=lambda c: {"高": 2, "中": 1}.get(c, 0), default="高")
        return fix

    def finish(self, view, item, fix):
        """用最终的值和新模式把依据再求值一遍、重跑离线检查，定出状态。"""
        if fix.status == "manual":
            return
        new_value = fix.new if fix.new is not None else item.value
        confidence = fix.confidence or "高"
        all_ok = True
        for ev, ef in zip(item.evidence, fix.evidence):
            target = ev
            if isinstance(ef.new, tuple):
                ins, kind = ef.new
                origin, ranges, error = self._origin_ranges(view, ev)
                if error:
                    fix.reasons.append(f"依据 {ev.index}：{error}")
                    all_ok = False
                    continue
                new_ev, why = regen.gen_evidence(self.image, self.cs, ev, origin, ranges, ins, kind, view, self.sig_results)
                if new_ev is None:
                    fix.reasons.append(f"依据 {ev.index} 重新生成模式：{why}")
                    ef.new = None
                    all_ok = False
                    continue
                if why:
                    fix.notes.append(f"依据 {ev.index} 重新生成模式：{why}")
                ef.new = new_ev
                target = new_ev
            er = evidence_mod.evaluate(self.image, view, self.sig_results, target)
            if er.status != "ok":
                fix.reasons.append(f"依据 {ev.index} 用新值 {new_value:#x} 求值不成立：{er.detail or er.status}")
                all_ok = False
        ctx = offline_mod.Context(self.image, view, self.sig_results)
        for name in item.offline:
            check = offline_mod.CHECKS.get(name)
            try:
                result = check(ctx) if check else offline_mod.OfflineResult(False, f"没有名为 {name} 的离线检查")
            except Exception as e:  # noqa: BLE001
                result = offline_mod.OfflineResult(False, f"检查出错：{type(e).__name__}: {e}")
            fix.offline.append({"name": name, "ok": result.ok, "detail": result.detail})
            if not result.ok:
                all_ok = False
                fix.reasons.append(f"离线检查 {name}：{result.detail}")
        if not all_ok:
            fix.status = "manual"
            return
        changed = new_value != item.value or any(ef.new is not None for ef in fix.evidence)
        if not changed:
            fix.status = "ok"
            fix.new = None
            return
        fix.new = new_value if new_value != item.value else None
        fix.status = f"repaired-{confidence}"
        fix.confidence = confidence


def regen_lower(conf):
    """指令对齐 / 放宽模式找到的值比「原模式命中」低一级。"""
    return "中" if conf in ("高", "中") else None

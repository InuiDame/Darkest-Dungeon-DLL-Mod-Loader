"""selftest 的变异样本（U1d）：在 Steam 版 Darkest.exe 的副本上模拟真实的编译器输出变化，生成到 build/u1d/mut/。

不碰真实安装和测试目录里的 exe（只读 Steam 版，写 build/ 下的副本），生成的 exe 不运行。每个样本写明：
  simulates  模拟的是哪种真实变化
  unreal     做不到真实的地方（如实写，不构造只为让实现通过的形状）
  truth      变化之后的真值：哪些特征码的命中位置变了、哪些清单项的值变了（没列出的就是不变）。selftest 用它判断 repair
             有没有修错：修复项的新值必须等于真值。值是 ABSENT 的项在新版本里已经不存在（字段被删掉），任何值都是错的，
             verify 判通过、repair 给出值都算给错结论
  verify     verify 应当给出的结论：失效项（名字, 类别）的集合和退出码
  repair     repair 应当给出的结论：逐项状态（ok / repaired / manual）和退出码
样本的改法用 pepatch.py（搬迁、复制、原地改位移和立即数、改 unwind 信息、改异常目录），地址都按清单在 Steam 版上现场解析，
不写死 RVA；但改哪几条指令（寄存器、位移范围、指令文本）是照 Steam 版 9db2d73b 读代码定的，generate 先核对 SHA1。
游戏更新、tools/baseline/steam.json 被 accept --replace 换成新版本之后，要留一份旧 exe（环境变量 DD_SELFTEST_STEAM 指向它）
和旧基线（用 verify 的 --baseline / repair 的 --tree 指过去）才能接着跑这些样本，或者照新版本重写改法。

U1 修复 FB（审核 A 的 S7、B1）：结构里加字段的样本都带上新字段完整的对应代码（复制构造里的复制、构造函数里的初始化、
解析函数里的解析），函数后面的填充放不下时用 pepatch.grow_in_place 原地变长、后面的函数依次后移；吸收了审核 A 的两个
样本（inst44：Buff::Instance 在 0x44 插入 int；inst_remove44：删掉 0x44 的 int），加了模板孪生两份都有调用者的
twin_callers。
"""
import dataclasses
import os
import struct
from pathlib import Path

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from . import evidence as evidence_mod
from . import manifest as manifest_mod
from . import pepatch
from . import sigs as sigs_mod
from . import verify as verify_mod
from .fingerprint import engine
from .image import Image
from .index import FunctionIndex
from .manifest import PROJECT

STEAM = os.environ.get("DD_SELFTEST_STEAM") or dict(verify_mod.DEFAULT_EXES)["steam"]
STEAM_SHA1 = "9db2d73b89aa034e2055df65a139403e18df641f"
OUT_DIR = PROJECT / "build" / "u1d" / "mut"
ABSENT = "absent"   # truth 里的值：这个字段在新版本里不存在了


@dataclasses.dataclass
class Sample:
    name: str
    category: str
    simulates: str
    unreal: str
    build: object
    verify: dict
    repair: dict
    truth: dict = dataclasses.field(default_factory=dict)
    notes: list = dataclasses.field(default_factory=list)
    path: str = ""


class Ctx:
    def __init__(self, image, manifest, sig_results, refs, cs):
        self.image = image
        self.manifest = manifest
        self.sigs = sig_results
        self.refs = refs
        self.cs = cs
        self.pe = pepatch.PeFile(image.path)
        self.notes = []
        self.truth = {"sigs": {}, "layout": {}}
        # 清单里特征码对应的函数：grow_in_place 不挪它们（挪了要在 truth 里记新位置，样本就不只模拟一种变化了）
        self.protect = set()
        for r in sig_results.values():
            if r.status != "ok":
                continue
            for x in (r.hit, r.resolved):
                if x is not None and image.in_text(x):
                    f = image.enclosing_function(x)
                    if f is not None:
                        self.protect.add(f)

    def reg(self, reg_id):
        return self.cs.reg_name(reg_id)

    def func_of(self, sid):
        """特征码对应的函数（HOOK/CODE 是解析结果，GLOBAL 指向 .text 时是目标函数，否则是命中所在的函数）：(起点, 结尾)。"""
        sig, res = self.manifest.sigs[sid], self.sigs[sid]
        if sig.kind in ("HOOK", "CODE") or self.image.in_text(res.resolved):
            anchor = res.resolved
        else:
            anchor = res.hit
        chunks = self.image.function_ranges(anchor)
        if not chunks or len(chunks) != 1:
            raise pepatch.PatchError(f"{sid} 的函数不是一个分块")
        return chunks[0]

    def insns(self, begin, end):
        return pepatch.function_insns(self.pe, self.cs, begin, end)

    def edit(self, insns, mnemonic, op_str, field, value, count=1):
        """把文本完全等于「mnemonic op_str」的指令的 field 改成 value，要求恰好 count 条。"""
        hits = [i for i in insns if i.mnemonic == mnemonic and i.op_str == op_str]
        if len(hits) != count:
            raise pepatch.PatchError(f"「{mnemonic} {op_str}」有 {len(hits)} 条，预期 {count} 条")
        for ins in hits:
            pepatch.set_field(self.pe, self.cs, ins.address, field, value)
        return hits

    def note(self, text):
        self.notes.append(text)

    def moved(self, sid, hit, resolved=None):
        self.truth["sigs"][sid] = {"hit": hit, "resolved": hit if resolved is None else resolved}

    def grow(self, begin, insert, what):
        """函数原地插入指令（pepatch.grow_in_place），后面的函数放不下时依次后移。"""
        first = min(insert)
        end = self.image.func_ends.get(begin, begin)
        inside = [sid for sid, r in self.sigs.items() if r.status == "ok" and r.hit is not None and first <= r.hit < end]
        if inside:
            raise pepatch.PatchError(f"插入点后面有特征码的命中 {inside}，真值要跟着改，这里没做")
        size, moves, counts = pepatch.grow_in_place(self.pe, self.image, self.refs, begin, insert=insert, cs=self.cs)
        # 后移的函数里有清单特征码命中的（例如 ParseEffectFile 后面的 EffectLookup）：真值里记新位置
        for sid, r in self.sigs.items():
            if r.status != "ok" or r.hit is None:
                continue

            def shifted(x):
                for a, b, n in moves:
                    if x is not None and a <= x < a + n:
                        return x + (b - a)
                return x
            if shifted(r.hit) != r.hit or shifted(r.resolved) != r.resolved:
                self.moved(sid, shifted(r.hit), shifted(r.resolved))
                self.note(f"特征码 {sid} 所在的函数后移，命中 {r.hit:X} → {shifted(r.hit):X}")
        moved = "；".join(f"{a:X}→{b:X}（{n:#x} 字节）" for a, b, n in moves)
        self.note(f"{what}（{begin:X}）原地重排，长度 {size:#x}" + (f"；后面 {len(moves)} 个函数依次后移：{moved}；改写引用 {counts}"
                                                                   if moves else "（后面的填充够用）"))


def _shift(ctx, begin, end, regs, lo, hi, delta, what):
    changed = pepatch.shift_fields(ctx.pe, ctx.cs, begin, end, regs, lo, hi, delta, ctx.reg)
    ctx.note(f"{what}（{begin:X}）：基址 {'/'.join(sorted(regs))}、位移 [{lo:#x}, {hi:#x}) 的 {len(changed)} 条访问后移 {delta:#x}")
    return changed


# ---- 1. 函数搬迁（调用方和异常目录跟着改，特征码照样命中） ----

def build_relocate(ctx):
    begin, _end = ctx.func_of("ListenerAdd")
    insns = ctx.insns(begin, _end)
    call = next(i for i in insns if i.mnemonic == "call")
    after = insns[insns.index(call) + 1]
    new, size, _amap, _labels, counts = pepatch.relocate(ctx.pe, ctx.image, ctx.refs, begin, insert={after.address: [b"\x90"]},
                                                         cs=ctx.cs)
    ctx.note(f"ListenerAdd {begin:X} → {new:X}（{size:#x} 字节，在 {call.address:X} 的 call 后面多一条 nop）；改写引用：{counts}")
    ctx.moved("ListenerAdd", new)
    ctx.moved("ListenerEntryVtable", new, ctx.sigs["ListenerEntryVtable"].resolved)


def build_relocate_sched(ctx):
    begin, end = ctx.func_of("QueryListenerAdd")
    insns = ctx.insns(begin, end)
    mov_rbx = next(i for i in insns if i.mnemonic == "mov" and i.op_str == "rbx, rcx")
    store = next(i for i in insns if i.mnemonic == "mov" and i.op_str == "qword ptr [rsp + 0x28], rdx")
    after = insns[insns.index(store) + 1]
    new, size, _amap, _labels, counts = pepatch.relocate(ctx.pe, ctx.image, ctx.refs, begin,
                                                         insert={after.address: [bytes(mov_rbx.bytes)]},
                                                         drop={mov_rbx.address}, cs=ctx.cs)
    ctx.note(f"QueryListenerAdd {begin:X} → {new:X}（{size:#x} 字节，mov rbx,rcx 挪到 mov [rsp+28h],rdx 后面）；改写引用：{counts}")
    ctx.moved("QueryListenerAdd", new)


# ---- 2. 栈帧变化（特征码被破坏） ----

def build_frame(ctx):
    begin, end = ctx.func_of("CampaignCurrent")
    insns = ctx.insns(begin, end)
    ctx.edit(insns, "sub", "rsp, 0x38", "imm", 0x48)
    ctx.edit(insns, "add", "rsp, 0x38", "imm", 0x48, count=2)
    moved = _shift(ctx, begin, end, {"rsp"}, 0x38, 0x1000, 0x10, "CampaignCurrent 的函数，帧以上的访问")
    pepatch.edit_unwind(ctx.pe, begin, lambda info: pepatch.unwind_set_alloc(info, 0x38, 0x48))
    ctx.note(f"CampaignCurrent 的函数 {begin:X}：sub/add rsp 0x38 → 0x48，UWOP_ALLOC_SMALL 同步（帧以上的访问 {len(moved)} 条）")


# ---- 3. 多处命中（模板孪生） ----

def build_twin(ctx):
    begin, _end = ctx.func_of("EffectLookup")
    new, size = pepatch.copy_function(ctx.pe, ctx.image, begin, cs=ctx.cs)
    ctx.note(f"EffectLookup {begin:X} 复制到 {new:X}（{size:#x} 字节，内部 call 照样指向原来的目标，异常目录加了表项，没有调用者）")


# ---- 4. 静默误绑定（真函数改了、别处有唯一命中的诱饵） ----

def build_decoy(ctx):
    begin, end = ctx.func_of("BuffRemoveByGuid")
    decoy, size = pepatch.copy_function(ctx.pe, ctx.image, begin, cs=ctx.cs)
    insns = ctx.insns(begin, end)
    ctx.edit(insns, "sub", "rsp, 0x30", "imm", 0x40)
    ctx.edit(insns, "add", "rsp, 0x30", "imm", 0x40, count=2)
    pepatch.edit_unwind(ctx.pe, begin, lambda info: pepatch.unwind_set_alloc(info, 0x30, 0x40))
    ctx.note(f"BuffRemoveByGuid {begin:X}：栈帧 0x30 → 0x40（原特征码不再命中）；改之前的样子留了一份在 {decoy:X}（{size:#x} 字节，"
             f"有异常目录表项、没有调用者），特征码唯一命中它")


# ---- 5. 枚举表变化（静态初始化里的顺序） ----

def init_entries(ctx, first, stride):
    """枚举表的静态初始化函数里 strcpy_s(表 + i*stride, 名字)：{下标: 给 r8 赋名字地址的 lea 指令}。照 offline._init_tables 找。"""
    image = ctx.image
    targets = {s for s in _find_strings(image, first)}
    funcs = set()
    data = image.bytes_at(image.text_start, image.text_end - image.text_start)
    for i in range(len(data) - 7):
        if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8D and (data[i + 2] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", data, i + 3)[0]
            if image.text_start + i + 7 + disp in targets:
                funcs.add(image.enclosing_function(image.text_start + i))
    for func in sorted(f for f in funcs if f):
        regs, base, out = {}, None, {}
        for b, e in image.function_ranges(func):
            for ins in ctx.cs.disasm(image.bytes_at(b, e - b), b):
                ops = ins.operands
                if ins.mnemonic == "lea" and len(ops) == 2 and ops[0].type == X86_OP_REG and ins.disp_size == 4 \
                        and "rip" in ins.op_str:
                    regs[ctx.reg(ops[0].reg)] = (ins.address + ins.size + ins.disp, ins)
                elif ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG \
                        and ctx.reg(ops[1].reg) in regs:
                    regs[ctx.reg(ops[0].reg)] = regs[ctx.reg(ops[1].reg)]
                elif ins.mnemonic == "call":
                    dest, src = regs.get("rcx"), regs.get("r8")
                    if dest and src:
                        name = (image.cstr(src[0], 80) or b"").decode(errors="replace")
                        if base is None and name == first:
                            base = dest[0]
                        if base is not None and 0 <= dest[0] - base and (dest[0] - base) % stride == 0:
                            out[(dest[0] - base) // stride] = (src[1], name)
                    for reg in ("rax", "rcx", "rdx", "r8", "r9", "r10", "r11"):
                        regs.pop(reg, None)
        if base is not None and out:
            return func, out
    raise pepatch.PatchError(f"没找到 {first} 那张表的初始化函数")


def _find_strings(image, text):
    data = bytes(image.mem)
    needle = text.encode() + b"\0"
    out, at = [], data.find(needle)
    while at != -1:
        if at == 0 or data[at - 1] == 0:
            out.append(at)
        at = data.find(needle, at + 1)
    return out


def build_enum(ctx):
    func, entries = init_entries(ctx, "status", 0x58)
    a, b = entries[2], entries[3]
    ins_a, ins_b = a[0], b[0]
    target_a = ins_a.address + ins_a.size + ins_a.disp
    target_b = ins_b.address + ins_b.size + ins_b.disp
    pepatch.set_field(ctx.pe, ctx.cs, ins_a.address, "disp", target_b - (ins_a.address + ins_a.size))
    pepatch.set_field(ctx.pe, ctx.cs, ins_b.address, "disp", target_a - (ins_b.address + ins_b.size))
    names = list(ctx.manifest.items["EnumStat"].names)
    names[2], names[3] = names[3], names[2]
    ctx.truth["layout"]["EnumStat"] = names
    ctx.note(f"EnumStat 的初始化函数 {func:X}：第 2、3 项（{a[1]}、{b[1]}）的名字对调（{ins_a.address:X}、{ins_b.address:X} 的 lea r8）")


# ---- 6. hook 入口不安全 ----

def build_hook_entry(ctx):
    begin, _end = ctx.func_of("ConsumeHealthDamageBlock")
    early = pepatch.Label("early")
    insert = {begin: [b"\x48\x85\xc9", pepatch.Unit(b"", kind="rel", target=early, short=True, opcode=4)]}   # test rcx,rcx; je
    append = [pepatch.Unit(b"\x32\xc0", label="early"), b"\xc3"]                                           # xor al,al; ret
    new, size, amap, _labels, counts = pepatch.relocate(ctx.pe, ctx.image, ctx.refs, begin, insert=insert, append=append,
                                                        unwind_edit=pepatch.unwind_shift, cs=ctx.cs)
    shift = amap[begin] - new     # je 到末尾超出短跳转范围时被排成近跳转，入口一共 3 + 6 字节
    ctx.note(f"ConsumeHealthDamageBlock {begin:X} → {new:X}（{size:#x} 字节）：入口先 test rcx,rcx / je 到末尾的 xor al,al / ret，"
             f"原来的序言整体后移 {shift} 字节，UNWIND_INFO 的序言长度和代码偏移同步加 {shift}；改写引用：{counts}")
    ctx.moved("ConsumeHealthDamageBlock", new)


# ---- 7. 虚表插入槽位 ----

def _vtable_slots(ctx, table):
    n = 0
    while ctx.image.in_text(ctx.image.u64(table + n * 8) - ctx.image.base):
        n += 1
    return n


def _return_zero_stub(ctx):
    """一个已有的「return 0」叶函数（33 C0 C3，前面是填充、16 字节对齐）。"""
    image = ctx.image
    data = image.bytes_at(image.text_start, image.text_end - image.text_start)
    at = data.find(b"\xcc\x33\xc0\xc3")
    while at != -1:
        rva = image.text_start + at + 1
        if rva & 0xF == 0 and image.enclosing_function(rva) is None:
            return rva
        at = data.find(b"\xcc\x33\xc0\xc3", at + 1)
    raise pepatch.PatchError("没找到 return 0 的叶函数")


VSLOT_INSERT = 38


def build_vslot(ctx):
    stub = _return_zero_stub(ctx)
    for sid in ("HeroVtable", "MonsterVtable"):
        table = ctx.sigs[sid].resolved
        n = _vtable_slots(ctx, table)
        slots = [ctx.pe.u64(table + i * 8) for i in range(n)]
        new = slots[:VSLOT_INSERT] + [ctx.image.base + stub] + slots[VSLOT_INSERT:n - 1]
        ctx.pe.write(table, b"".join(struct.pack("<Q", v) for v in new))
        ctx.note(f"{sid} {table:X}：{n} 个槽，在第 {VSLOT_INSERT} 槽插入 {stub:X}（return 0），后面整体后移一槽，原来的最后一槽去掉")
    for iid in ("VtSlotDeathsDoor", "VtSlotDead", "VtSlotDamage", "VtSlotHeal", "VtSlotStress", "SlotAddBuff", "SlotRemoveBuffs"):
        value = ctx.manifest.items[iid].value
        if value >= VSLOT_INSERT:
            ctx.truth["layout"][iid] = value + 1


# ---- 8. 依据误绑定（Effect 中间插入字段，模式靠邻居偏移区分） ----

EFFECT_INSERT, EFFECT_DELTA, EFFECT_END = 0x134, 8, 0x440


EFFECT_NEW_KEYS = (b".dotChill\0", b".dotChillDuration\0")


def build_evidence_misbind(ctx):
    begin, end = ctx.func_of("ParseEffectFile")
    _shift(ctx, begin, end, {"rsi"}, EFFECT_INSERT, EFFECT_END, EFFECT_DELTA, "ParseEffectFile（rsi 是 Effect）")
    # 新字段（两个 int，+0x134、+0x138）的解析代码，照相邻字段的形状：lea r8,[rsi+字段]; lea rdx,[键名]; lea rcx,[rbp+210h];
    # call 读 int 的函数，插在（已后移的）.dotPoison 的解析前面；键名字符串放进 .rdata 末尾的空隙（S7）
    insns = ctx.insns(begin, end)
    k = next(n for n, i in enumerate(insns) if i.mnemonic == "lea" and i.op_str == f"r8, [rsi + {EFFECT_INSERT + EFFECT_DELTA:#x}]")
    block = insns[k:k + 4]
    if [i.mnemonic for i in block] != ["lea", "lea", "lea", "call"] or block[2].op_str != "rcx, [rbp + 0x210]":
        raise pepatch.PatchError(f".dotPoison 的解析不是预期的形状：{[(i.mnemonic, i.op_str) for i in block]}")
    reader = block[3].operands[0].imm
    units = []
    for n, key in enumerate(EFFECT_NEW_KEYS):
        at = ctx.pe.alloc_rdata(len(key))
        ctx.pe.write(at, key)
        field = EFFECT_INSERT + 4 * n
        units += [b"\x4c\x8d\x86" + field.to_bytes(4, "little"),
                  pepatch.Unit(b"\x48\x8d\x15\x00\x00\x00\x00", kind="rip", target=at, disp_offset=3),
                  bytes(block[2].bytes),
                  pepatch.Unit(b"", kind="rel", target=reader, short=False, opcode="call")]
        ctx.note(f"新字段 +{field:#x} 的键名 {key[:-1].decode()} 放在 .rdata {at:X}")
    ctx.grow(begin, {block[0].address: units}, f"ParseEffectFile：在 .dotPoison 的解析前面加两个新字段的解析（读 int 的函数 {reader:X}）")
    ev = next(e for e in ctx.manifest.evidence if e.item == "EffectSwap" and e.sig == "EffectApplyInternal")
    er = evidence_mod.evaluate(ctx.image, ctx.manifest, ctx.sigs, ev)
    old = pepatch.set_field(ctx.pe, ctx.cs, er.hit, "disp", 0x3E0 + EFFECT_DELTA)
    ctx.note(f"EffectApplyInternal：{er.hit:X} 读 .swap 的位移 {old:#x} → {0x3E0 + EFFECT_DELTA:#x}")
    for iid, item in ctx.manifest.items.items():
        if iid.startswith("Effect") and item.type == "OFFSET" and EFFECT_INSERT <= item.value < EFFECT_END \
                and not iid.startswith(("EffectList", "EffectApplyResult")):
            ctx.truth["layout"][iid] = item.value + EFFECT_DELTA


# ---- 9. 结构偏移整体变化（Buff::Instance 中间插入字段） ----

INST_INSERT, INST_DELTA, INST_SIZE, ENTRY_SIZE = 0xB0, 8, 0x410, 0x418
DIV_MAGIC_OLD = 0x3E88CB3C9484E2B           # 除以 0x418（= 8 * 0x83）：sar 3 再乘 0x83 的逆元
DIV_MAGIC_NEW = pow(33, -1, 1 << 64)        # 除以 0x420（= 32 * 33）：sar 5 再乘 33 的逆元


def _entry_vector_functions(ctx):
    """按元素个数除以 0x418 的函数（vector<Buff::Entry> 的遍历和查找）。"""
    image = ctx.image
    data = image.bytes_at(image.text_start, image.text_end - image.text_start)
    needle = struct.pack("<Q", DIV_MAGIC_OLD)
    funcs, at = set(), data.find(needle)
    while at != -1:
        funcs.add(image.enclosing_function(image.text_start + at))
        at = data.find(needle, at + 1)
    return sorted(f for f in funcs if f)


def _stride_edits(ctx, func, what):
    """一个 vector<Entry> 函数里：除法的魔数和移位、元素大小 0x418 的立即数和位移（不含栈上的）改成新的。"""
    image = ctx.image
    count = {"magic": 0, "sar": 0, "stride": 0}
    for b, e in image.function_ranges(func):
        insns = ctx.insns(b, e)
        magic_regs = set()
        for k, ins in enumerate(insns):
            if ins.mnemonic == "movabs" and ins.operands[1].type == X86_OP_IMM and ins.operands[1].imm == DIV_MAGIC_OLD:
                pepatch.set_field(ctx.pe, ctx.cs, ins.address, "imm", DIV_MAGIC_NEW)
                magic_regs.add(ctx.reg(ins.operands[0].reg))
                count["magic"] += 1
        for k, ins in enumerate(insns):
            if ins.mnemonic == "sar" and ins.op_str.endswith(", 3") and ins.operands[0].type == X86_OP_REG:
                reg = ctx.reg(ins.operands[0].reg)
                for nxt in insns[k + 1:k + 7]:
                    if nxt.mnemonic == "imul" and len(nxt.operands) == 2 and nxt.operands[0].type == X86_OP_REG \
                            and ctx.reg(nxt.operands[0].reg) == reg and nxt.operands[1].type == X86_OP_REG \
                            and ctx.reg(nxt.operands[1].reg) in magic_regs:
                        pepatch.set_field(ctx.pe, ctx.cs, ins.address, "imm", 5)
                        count["sar"] += 1
                        break
            for op in ins.operands:
                if op.type == X86_OP_IMM and op.imm in (ENTRY_SIZE, -ENTRY_SIZE) and ins.mnemonic in ("imul", "add", "sub", "mov"):
                    pepatch.set_field(ctx.pe, ctx.cs, ins.address, "imm", op.imm + (INST_DELTA if op.imm > 0 else -INST_DELTA))
                    count["stride"] += 1
                elif op.type == X86_OP_MEM and op.mem.disp in (ENTRY_SIZE, -ENTRY_SIZE) and ins.mnemonic == "lea" \
                        and ctx.reg(op.mem.base) not in ("rsp", "rbp"):
                    pepatch.set_field(ctx.pe, ctx.cs, ins.address, "disp",
                                      op.mem.disp + (INST_DELTA if op.mem.disp > 0 else -INST_DELTA))
                    count["stride"] += 1
    ctx.note(f"{what}（{func:X}）：除以元素大小的魔数 {count['magic']} 处、移位 {count['sar']} 处、元素大小 {count['stride']} 处")
    return count


def build_struct_shift(ctx):
    lo, hi, d = INST_INSERT, INST_SIZE, INST_DELTA
    copy_b, copy_e = ctx.func_of("BuffInstanceCopy")
    _shift(ctx, copy_b, copy_e, {"rcx", "rdx", "rbx", "rdi"}, lo, hi, d, "Instance 复制构造")
    ctx.edit(ctx.insns(copy_b, copy_e), "add", "rdx, 0x158", "imm", 0x158 + d)
    ctx.edit(ctx.insns(copy_b, copy_e), "add", "rcx, 0x158", "imm", 0x158 + d)
    ctor_b, ctor_e = ctx.func_of("BuffInstanceCtor")
    _shift(ctx, ctor_b, ctor_e, {"rcx", "rbx"}, lo, hi, d, "Instance 构造")
    rule_b, rule_e = ctx.func_of("BuffRuleActive")
    _shift(ctx, rule_b, rule_e, {"r15"}, lo, hi, d, "BuffRuleActive（r15 是 Instance）")
    # Entry = { uint32 guid; pad; Instance }：Instance 在 Entry+8，元素大小 0x418
    erase_b, erase_e = ctx.func_of("BuffGroupErase")
    _shift(ctx, erase_b, erase_e, {"rsi", "r14"}, lo + 8, hi + 8, d, "BuffGroupErase 逐个前移 Entry（rsi/r14 是 Entry）")
    insns = ctx.insns(erase_b, erase_e)
    ctx.edit(insns, "lea", "rsi, [r8 + 0x418]", "disp", ENTRY_SIZE + d)
    ctx.edit(insns, "add", "r14, 0x418", "imm", ENTRY_SIZE + d)
    ctx.edit(insns, "add", "rsi, 0x418", "imm", ENTRY_SIZE + d)
    ctx.edit(insns, "lea", "rdx, [rax + 0x2c]", "disp", 0x2C + d // 8)     # Entry+0x160（附加效果数组）= (i*3 + 0x2C) * 8
    ctx.edit(insns, "lea", "rax, [rax + 0x2c]", "disp", 0x2C + d // 8)
    ctx.edit(insns, "lea", "rax, [rdi - 0x418]", "disp", -(ENTRY_SIZE + d))   # 最后一个元素
    ctx.edit(insns, "lea", "rax, [rdi - 0x410]", "disp", -(INST_SIZE + d))    # 最后一个元素的 Instance
    ctx.edit(insns, "add", "qword ptr [rbp + 8], -0x418", "imm", -(ENTRY_SIZE + d))
    ctx.note("BuffGroupErase：元素大小、末元素的 Entry/Instance、附加效果数组的比例寻址同步改")
    upkeep_b, upkeep_e = ctx.func_of("BuffDurationUpkeep")
    ctx.edit(ctx.insns(upkeep_b, upkeep_e), "add", "rbx, 0x418", "imm", ENTRY_SIZE + d)
    # 新字段（8 字节，+0xB0）：复制构造在 rule_float（已后移到 +0xB8）的复制前面复制它，构造函数把它清零（S7）
    insns = ctx.insns(copy_b, copy_e)
    at = next(i for i in insns if i.mnemonic == "mov" and i.op_str == f"eax, dword ptr [rdx + {lo + d:#x}]")
    ctx.grow(copy_b, {at.address: [b"\x48\x8b\x82" + lo.to_bytes(4, "little"), b"\x48\x89\x81" + lo.to_bytes(4, "little")]},
             f"Instance 复制构造：在 rule_float 的复制前面加 mov rax,[rdx+{lo:#x}h] / mov [rcx+{lo:#x}h],rax")
    insns = ctx.insns(ctor_b, ctor_e)
    at = next(i for i in insns if i.mnemonic == "mov" and i.op_str == f"dword ptr [rcx + {lo + d:#x}], edi")
    ctx.grow(ctor_b, {at.address: [b"\x48\x89\xb9" + lo.to_bytes(4, "little")]},
             f"Instance 构造：加 mov qword ptr [rcx+{lo:#x}h],rdi（新字段清零）")
    lib_b, lib_e = ctx.func_of("BuffLibraryIndex")
    insns = ctx.insns(lib_b, lib_e)
    ctx.edit(insns, "mov", "ecx, 0x438", "imm", 0x438 + d)                   # map 节点 = 0x28 + Instance
    ctx.edit(insns, "mov", "r8d, 0x410", "imm", INST_SIZE + d)                # memset Instance
    add_b, add_e = ctx.func_of("BuffGroupAdd")
    insns = ctx.insns(add_b, add_e)
    ctx.edit(insns, "sub", "rsp, 0x440", "imm", 0x440 + d)
    ctx.edit(insns, "add", "rsp, 0x440", "imm", 0x440 + d)
    ctx.edit(insns, "add", "qword ptr [rbx + 8], 0x418", "imm", ENTRY_SIZE + d)
    _shift(ctx, add_b, add_e, {"rsp"}, 0x28 + lo, 0x1000, d, "BuffGroupAdd 栈上的 Instance 副本（rsp+0x28）及以上")
    pepatch.edit_unwind(ctx.pe, add_b, lambda info: pepatch.unwind_set_alloc(info, 0x440, 0x440 + d))
    for func in _entry_vector_functions(ctx):
        _stride_edits(ctx, func, "vector<Entry> 的函数")
    truth = ctx.truth["layout"]
    for iid in ("InstRuleFloat", "InstRuleString", "InstRuleHash", "InstAdditional", "InstClearOnRule"):
        truth[iid] = ctx.manifest.items[iid].value + d
    truth["BuffInstanceSize"] = INST_SIZE + d
    truth["BuffEntrySize"] = ENTRY_SIZE + d


# ---- 10. 结构里一串同形状的 int 字段中间插入、删除一个（审核 A 的 B1，吸收 build/u1review 的两个脚本） ----

def build_inst44(ctx):
    """sub_type（char[64]，0x04..0x44）后面插入一个 int：[0x44, 0x152) 的字段后移 4，0x152..0x158 原来 6 字节的尾部填充吸收掉，
    Instance、Entry 的大小都不变。复制构造多一对 mov eax,[rdx+44h] / mov [rcx+44h],eax（和相邻 int 字段的复制同形），
    构造函数多一条 mov [rcx+44h],edi 清零新字段。"""
    lo, hi, d = 0x44, 0x152, 4
    copy_b, copy_e = ctx.func_of("BuffInstanceCopy")
    _shift(ctx, copy_b, copy_e, {"rcx", "rdx"}, lo, hi, d, "Instance 复制构造")
    ctor_b, ctor_e = ctx.func_of("BuffInstanceCtor")
    _shift(ctx, ctor_b, ctor_e, {"rcx", "rbx"}, lo, hi, d, "Instance 构造")
    rule_b, rule_e = ctx.func_of("BuffRuleActive")
    _shift(ctx, rule_b, rule_e, {"r15"}, lo, hi, d, "BuffRuleActive（r15 是 Instance）")
    erase_b, erase_e = ctx.func_of("BuffGroupErase")
    _shift(ctx, erase_b, erase_e, {"rsi", "r14"}, lo + 8, hi + 8, d, "BuffGroupErase（rsi/r14 是 Entry）")
    up_b, up_e = ctx.func_of("BuffDurationUpkeep")
    _shift(ctx, up_b, up_e, {"rbx"}, lo + 8, hi + 8, d, "BuffDurationUpkeep（rbx 是 Entry）")
    add_b, add_e = ctx.func_of("BuffGroupAdd")
    _shift(ctx, add_b, add_e, {"rsp"}, 0x28 + lo, 0x28 + hi, d, "BuffGroupAdd 栈上的 Instance 副本")
    insns = ctx.insns(copy_b, copy_e)
    at = next(i for i in insns if i.mnemonic == "mov" and i.op_str == "eax, dword ptr [rdx + 0x48]")
    ctx.grow(copy_b, {at.address: [b"\x8b\x42\x44", b"\x89\x41\x44"]},
             "Instance 复制构造：在（已后移的）sub_hash 的复制前面加 mov eax,[rdx+44h] / mov [rcx+44h],eax")
    insns = ctx.insns(ctor_b, ctor_e)
    at = next(i for i in insns if i.mnemonic == "mov" and i.op_str == "qword ptr [rcx + 0x48], rdi")
    ctx.grow(ctor_b, {at.address: [b"\x89\x79\x44"]}, "Instance 构造：加 mov [rcx+44h],edi（新字段清零）")
    for iid, item in ctx.manifest.items.items():
        if iid.startswith("Inst") and item.type == "OFFSET" and lo <= item.value < hi:
            ctx.truth["layout"][iid] = item.value + d


def build_inst_remove44(ctx):
    """去掉 sub_hash（+0x44 的 int）：[0x48, 0x152) 的字段前移 4，大小不变（尾部填充变大）。复制构造少一对 mov。
    构造函数原来用一条 mov qword ptr [rcx+44h],rdi 清零 sub_hash 和 amount，删字段以后它清零的是 amount 和 duration，
    duration 接着被设成 -1，语义不变，这条不改。"""
    copy_b, copy_e = ctx.func_of("BuffInstanceCopy")
    insns = ctx.insns(copy_b, copy_e)
    drop = {i.address for i in insns if i.op_str in ("eax, dword ptr [rdx + 0x44]", "dword ptr [rcx + 0x44], eax")}
    if len(drop) != 2:
        raise pepatch.PatchError(f"sub_hash 的复制有 {len(drop)} 条，预期 2 条")
    code, _amap, _labels = pepatch.assemble(ctx.pe, ctx.cs, copy_b, copy_e, copy_b, drop=drop)
    entries = ctx.pe.runtime_functions()
    nxt = min(e[0] for e in entries if e[0] > copy_b)
    ctx.pe.write(copy_b, code + b"\xcc" * (nxt - copy_b - len(code)))
    unwind = next(e[2] for e in entries if e[0] == copy_b)
    ctx.pe.write_runtime_functions([e for e in entries if e[0] != copy_b] + [(copy_b, copy_b + len(code), unwind)])
    ctx.note(f"Instance 复制构造 {copy_b:X} 原地重排，长度 {copy_e - copy_b:#x} → {len(code):#x}，去掉 sub_hash 的一对 mov")
    copy_e = copy_b + len(code)
    lo, hi, d = 0x48, 0x152, -4
    _shift(ctx, copy_b, copy_e, {"rcx", "rdx"}, lo, hi, d, "Instance 复制构造")
    for sid, regs, base in (("BuffInstanceCtor", {"rcx", "rbx"}, 0), ("BuffRuleActive", {"r15"}, 0),
                            ("BuffGroupErase", {"rsi", "r14"}, 8), ("BuffDurationUpkeep", {"rbx"}, 8)):
        b, e = ctx.func_of(sid)
        _shift(ctx, b, e, regs, lo + base, hi + base, d, sid)
    ctx.truth["layout"]["InstSubHash"] = ABSENT
    for iid, item in ctx.manifest.items.items():
        if iid.startswith("Inst") and item.type == "OFFSET" and lo <= item.value < hi:
            ctx.truth["layout"][iid] = item.value + d


# ---- 11. 模板孪生，两份都有调用者（审核 A「无法确认」第 2 条） ----

def build_twin_callers(ctx):
    begin, _end = ctx.func_of("EffectLookup")
    new, size = pepatch.copy_function(ctx.pe, ctx.image, begin, cs=ctx.cs)
    sites = sorted(ctx.refs.rel_sites(begin))
    moved = sites[1::2]
    for site in moved:
        head = ctx.pe.read(site, 2)
        n = 2 if head[0] == 0x0F else 1
        ctx.pe.write(site + n, struct.pack("<i", new - (site + n + 4)))
    funcs_a = {ctx.image.enclosing_function(s) for s in sites[0::2]}
    funcs_b = {ctx.image.enclosing_function(s) for s in moved}
    ctx.note(f"EffectLookup {begin:X} 复制到 {new:X}（{size:#x} 字节）；{len(sites)} 处调用按地址排序后隔一处改调副本："
             f"原函数 {len(sites) - len(moved)} 处（{len(funcs_a)} 个函数），副本 {len(moved)} 处（{len(funcs_b)} 个函数），"
             f"两边都调用的函数 {len(funcs_a & funcs_b)} 个")


# ---- 样本表 ----
# verify：failures 是 {(名字, 类别)}；repair：status 是 {名字: ok / repaired / manual}（没列出的项要求 ok 或 unverified）
# U1 修复 FB：repair 的预期里多了同组升级（S4）的项（组的定义在 layout.inc 末尾的 DD_GROUP）

ACTOR_SLOT_PROTOS = ("ProtoActorDamage", "ProtoActorHeal", "ProtoActorStress", "ProtoActorPredicate", "ProtoAddBuff", "ProtoRemoveBuffs")
INSTANCE_NONE = ("InstStat", "InstScalarSize", "InstFlagSize", "GameTextCap")
# inst44 / inst_remove44 里挪动了的 Instance 字段，和其中只有「一串 int 复制」这一条依据的四项
INST_MOVED = ("InstSubHash", "InstAmount", "InstDuration", "InstDurationType", "InstSource", "InstSourceData", "InstRule",
              "InstFalseRule", "InstId", "InstIdHash", "InstTarget", "InstRemoveInactive", "InstRemoveBattle", "InstRuleFloat",
              "InstRuleString", "InstRuleHash")
INST_RUN_ONLY = ("InstSubHash", "InstAmount", "InstSource", "InstSourceData")

SAMPLES = [
    Sample(
        "relocate", "函数搬迁",
        "ListenerAdd（必需）整个挪到新地址：链接顺序变了，函数在 .text 里换了位置，函数中间多了一条 nop（MSVC 在带异常状态的"
        "call 后面放 nop，源码改动会多出或少掉这样的指令）。84 个调用者的 call rel32、函数里的 rip 相对位移和 call 都修好，"
        "异常目录的表项指向新位置，旧位置填 CC",
        "新位置是 .text 末尾的空隙（0x110 字节，这个大小以内的函数才能搬）；真实的重新链接会让所有函数一起挪，这里只挪一个",
        build_relocate,
        verify={"exit": 0, "failures": set()},
        repair={"exit": 0, "status": {}}),
    Sample(
        "relocate_sched", "函数搬迁",
        "QueryListenerAdd 挪到新地址，同时编译器换了指令调度（mov rbx,rcx 排到 mov [rsp+28h],rdx 后面），原特征码不再命中",
        "同上，只挪一个函数",
        build_relocate_sched,
        verify={"exit": 2, "failures": {("QueryListenerAdd", "optional"), ("ListenerEntryListener", "required"),
                                         ("ListenerEntryActive", "required"), ("QueryListenerEntrySize", "optional")}},
        # U1 修复 FB（S4）：QueryListenerAdd 被重新定位，同组（FnQueryListenerAdd）的原型项要人工看
        repair={"exit": 1, "status": {"QueryListenerAdd": "repaired", "ProtoQueryListenerAdd": "manual"}}),
    Sample(
        "frame", "特征码被破坏",
        "CampaignCurrent（必需）所在的函数栈帧从 0x38 变成 0x48（局部变量多了），sub/add rsp 两处和 unwind 的 UWOP_ALLOC_SMALL 同步改，"
        "函数里没有帧以上的栈访问",
        "只改了这一个函数",
        build_frame,
        verify={"exit": 2, "failures": {("CampaignCurrent", "required")}},
        repair={"exit": 0, "status": {"CampaignCurrent": "repaired"}}),
    Sample(
        "twin", "多处命中",
        "EffectLookup 多出一份完全相同的副本：模板的另一个实例化，或者链接器没有合并相同的 COMDAT（/OPT:ICF 关掉）。副本有自己的"
        "异常目录表项，内部的 call 指向同样的目标，没有调用者",
        "副本的位置是 .text 末尾的空隙",
        build_twin,
        verify={"exit": 1, "failures": {("EffectLookup", "optional"), ("SkillEffects", "optional")}},
        repair={"exit": 1, "status": {"EffectLookup": "manual", "SkillEffects": "manual"}}),
    Sample(
        "decoy", "静默误绑定",
        "BuffRemoveByGuid 的栈帧从 0x30 变成 0x40（sub/add rsp、unwind 同步），原特征码不再命中真函数；另有一个和改之前一模一样的"
        "函数（有异常目录表项、没有调用者，像只经函数指针或虚表用的实例），原特征码唯一命中它",
        "诱饵是真函数改之前的拷贝，现实里更可能是另一个形状相近的函数",
        build_decoy,
        verify={"exit": 2, "failures": {("BuffRemoveByGuid", "misbind")}},
        repair={"exit": 0, "status": {"BuffRemoveByGuid": "repaired"}}),
    Sample(
        "enum", "枚举表变化",
        "stat 枚举的第 2、3 项在源码里调换了顺序：静态初始化里 strcpy_s(表 + i*0x58, 名字) 的两个名字对调",
        "只调换了一对，没有新增或删除项（项数变了的话表的大小、后面的初始化都会变）",
        build_enum,
        verify={"exit": 1, "failures": {("EnumStat", "optional")}},
        repair={"exit": 1, "status": {"EnumStat": "manual"}}),
    Sample(
        "hook_entry", "hook 入口不安全",
        "ConsumeHealthDamageBlock 的源码加了空指针判断，编译器把它放在序言之前（shrink-wrapping）：入口变成 test rcx,rcx / je，"
        "提前返回的路径在末尾；原来的序言整体后移 5 字节，函数挪到新地址（变长了放不回原处），调用者和 UNWIND_INFO 同步改",
        "提前返回的路径放在同一个异常目录表项里（MSVC 可能把它拆成单独的表项）；只挪了这一个函数",
        build_hook_entry,
        verify={"exit": 1, "failures": {("ConsumeHealthDamageBlock", "optional"), ("VtSlotDamage", "optional")}},
        # U1 修复 FB（S4）：ConsumeHealthDamageBlock 需要人工 → 它的原型项；VtSlotDamage 需要人工 → 虚表槽组里全部原型项
        repair={"exit": 1, "status": {"ConsumeHealthDamageBlock": "manual", "VtSlotDamage": "manual",
                                      "ProtoConsumeHealthDamageBlock": "manual",
                                      **{n: "manual" for n in ACTOR_SLOT_PROTOS}}}),
    Sample(
        "vslot", "虚表槽位变化",
        f"Actor 基类在第 {VSLOT_INSERT} 个虚函数的位置加了一个虚函数：Hero、Monster 两张虚表都在第 {VSLOT_INSERT} 槽插入新函数"
        "（空实现，链接器把它和已有的 return 0 合并成同一个），后面的槽整体后移一槽",
        "表的大小没变（去掉了最后一槽，否则要挪动虚表）；按槽号调用虚函数的游戏代码（call [rax+槽*8]）没有跟着改，"
        "清单里没有依据看那些调用点",
        build_vslot,
        # 槽 40、43、50 换成了前一槽的函数：按固定槽号取函数的依据不成立；槽 51 换成了原来槽 50 的函数，它也调用取最大生命的
        # 函数，VtSlotHeal 的依据 0 照样成立，靠依据位置核对的第 5 条（槽里的函数和基线里的不是同一个）判成误绑定。
        # U1 修复 FB 核实后改的断言：VtSlotHeal 多了一项 optional。U1 修复 FA 给它加了依据 1（槽 51 的函数里有加血的指令串
        # hp += min(治疗量, 最大生命 - hp)），槽 51 现在是伤害函数，依据 1 命中 0 次。真值：VtSlotHeal 应为 52（第 38 槽插入、
        # 后面整体后移一槽），清单里的 51 是错的，所以这是真阳性；原来只有位置核对的误绑定能发现它，不算漏报，现在运行时也能发现
        verify={"exit": 2, "failures": {("VtSlotHeal", "misbind"), ("VtSlotHeal", "optional"), ("ActorHp", "optional"),
                                         ("ActorMaxHpStat", "optional"), ("ActorMaxHpMask", "optional"),
                                         ("DamageEpsilonBits", "optional"), ("VtSlotDamage", "optional"),
                                         ("VtSlotDeathsDoor", "optional"), ("VtSlotDead", "optional")}},
        # 槽号本身按依据搜索（槽里的函数还要和基线里的是同一个）；按固定槽号取函数的项不在换了的函数里重新找，报需要人工。
        # U1 修复 FB（S4）：槽号变了 → 虚表槽组的原型项要人工看；ActorHp 需要人工 → Actor 结构组的两个宽度项要人工看
        repair={"exit": 1, "status": {"VtSlotDamage": "repaired", "VtSlotHeal": "repaired", "VtSlotDeathsDoor": "repaired",
                                      "VtSlotDead": "repaired", "ActorHp": "manual", "ActorMaxHpStat": "manual",
                                      "ActorMaxHpMask": "manual", "DamageEpsilonBits": "manual",
                                      "ActorFloatSize": "manual", "ActorIdSize": "manual",
                                      **{n: "manual" for n in ACTOR_SLOT_PROTOS}}}),
    Sample(
        "evidence_misbind", "依据误绑定",
        f"Effect 在 dot_poison（{EFFECT_INSERT:#x}）之前插入两个 int（.dotChill、.dotChillDuration）：解析函数 4F02D0 里以 rsi"
        f"（Effect）为基址、位移不小于 {EFFECT_INSERT:#x} 的访问整体后移 8，在 .dotPoison 的解析前面多两段同形状的解析（lea r8 /"
        " lea rdx 键名 / lea rcx / call 读 int 的函数），新键名在 .rdata；EffectApplyInternal 读 .swap 的那条也后移。U1d 时"
        "EffectDotBurn 的依据靠邻居字段的偏移区分，这种变化会让它唯一命中邻居的解析、读到的值恰好等于旧常量（依据误绑定）；"
        "U1 修复 FA 收紧以后它判不成立，修复工具靠指令对齐找回来",
        "EffectApplyInternal 里只改了依据读的那一条（其余访问 Effect 的代码没有类型信息、认不全）；读 Effect 的其他函数没改；"
        "解析函数变长后，后面几个函数依次后移、不再 16 字节对齐（真实的重新链接会保持对齐，后面所有函数都挪）；C++ 异常表"
        "（FuncInfo）里的 RVA 没改（工具只读异常目录）",
        build_evidence_misbind,
        # U1 修复 FB 核实后改的断言：EffectDotBurn 从「misbind」变成「optional」（依据不成立），退出码 2 → 1。U1 修复 FA 把它的
        # 依据收紧成前后两个字段（0x1BC、0x140）都带字面量，结构在它前面插入 8 字节后两个字面量都对不上，模式命中 0 次。
        # 真值：EffectDotBurn 应为 0x144（0x13C + 8），清单里的 0x13C 是错的，判失效是真阳性；原来靠位置核对判误绑定也报出来了，
        # 不是漏报，现在运行时也能发现（不用基线）。effect_field_types、effect_misc_fields 两个离线检查各管一组字段（U1 修复 FB
        # 改成按键名核对字段身份），组里有字段变了时整组的项都报失效（包括值没变的 EffectChance 等）
        verify={"exit": 1, "failures": {(name, "optional") for name in (
            "EffectTarget", "EffectChance", "EffectStress", "EffectStun", "EffectHeal", "EffectHealPercent", "EffectCombatStatBuff",
            "EffectDotPoison", "EffectDotBleed", "EffectDotBurn", "EffectDotStress", "EffectDuration", "EffectOnHit", "EffectOnMiss",
            "EffectApplyOnce", "EffectQueue", "EffectBuffAmount", "EffectBuffIds", "EffectSwap", "EffectBuffDurationType")}},
        # 有运行时依据的修好：EffectDotBurn、EffectDotStress 放宽模式多处命中，按指令对齐选中（新字段的解析带着新键名，对齐
        # 没有歧义），新模式保住原来前后两个字段的范围；只有离线依据的不给新值，报需要人工；EffectSwap 的运行时依据给出了
        # 新值，但它的离线检查和 .buff_duration_type 绑在一起，那一项修不了，所以也是需要人工。U1 修复 FB（S4）：Effect 组的
        # NONE 项（名字、读取前缀、宽度）要人工看
        repair={"exit": 1, "status": {**{name: "repaired" for name in (
            "EffectDotBurn", "EffectDotStress", "EffectOnHit", "EffectOnMiss", "EffectApplyOnce", "EffectQueue", "EffectBuffIds")},
            **{name: "manual" for name in (
                "EffectTarget", "EffectChance", "EffectStress", "EffectStun", "EffectHeal", "EffectHealPercent",
                "EffectCombatStatBuff", "EffectDotPoison", "EffectDotBleed", "EffectDuration", "EffectBuffAmount", "EffectSwap",
                "EffectBuffDurationType", "EffectName", "EffectNameCap", "EffectReadSize", "EffectScalarSize",
                "EffectFlagSize")}}}),
    Sample(
        "struct_shift", "结构偏移整体变化",
        f"Buff::Instance 在 rule_float（{INST_INSERT:#x}）之前插入 8 字节的字段：复制构造、构造、BuffRuleActive 里以 Instance 为基址、"
        f"位移不小于 {INST_INSERT:#x} 的访问后移 8；复制构造多一对 mov rax,[rdx+0B0h] / mov [rcx+0B0h],rax，构造函数多一条清零；"
        "Instance 变成 0x418、Entry 变成 0x420，元素大小的立即数、按元素个数做除法的魔数和移位（8 个函数）、栈上的 Instance 副本"
        "（BuffGroupAdd 的栈帧和 unwind）、map 节点的分配大小同步改",
        "只改了清单依据涉及的函数和按 Entry 做除法的函数，游戏里其他访问 Instance 的代码没有类型信息、认不全；复制构造变长后"
        "紧跟着的函数后移、不再 16 字节对齐；C++ 异常表（FuncInfo）里的 RVA 没改（工具只读异常目录）",
        build_struct_shift,
        # 值变了的项、依据模式里写着 0x418 的两项（模式不再命中）、DD_ENTRY 里有栈帧大小的 BuffGroupAdd、特征码里有除法
        # 魔数的 BuffBurnDecay 和 BuffRemoveByGuid。
        # U1 修复 FB 核实后改的断言：多了 InstRemoveInactive、InstRemoveBattle（optional）。它们的值 0xAC、0xAD 没变（真值不变），
        # 是 U1 修复 FA 收紧的依据带着界标（+0xB0 的 dword 复制），新字段的复制（mov rax,[rdx+0B0h]）插在那里、rule_float 的复制
        # 挪到 +0xB8，界标对不上，依据判不成立。这是假阳性（值没变却判失效），方向是安全的（停用、不会读错），是收紧的代价
        # （ADR 0036「局限」）；原来判 OK 是对的，不是漏报
        verify={"exit": 1, "failures": {(name, "optional") for name in (
            "BuffGroupAdd", "BuffBurnDecay", "BuffRemoveByGuid", "BuffEntrySize", "BuffEntryInstance", "BuffInstanceSize",
            "InstDuration", "InstRuleFloat", "InstRuleString", "InstRuleHash", "InstAdditional", "InstClearOnRule",
            "InstRemoveInactive", "InstRemoveBattle")}},
        # InstRemoveInactive、InstRemoveBattle 按指令对齐找回原来的复制（新字段的复制是 64 位的，和相邻的 8/32 位复制形状不同，
        # 对齐没有歧义），值不变、模式重新生成（保住原来的范围，新界标是新字段的复制）。U1 修复 FB（S4）：Instance 组、GroupEntry
        # 组的 NONE 项和 BuffGroupAdd 的原型项要人工看
        repair={"exit": 1, "status": {**{name: "repaired" for name in (
            "BuffGroupAdd", "BuffBurnDecay", "BuffRemoveByGuid", "BuffEntrySize", "BuffEntryInstance", "BuffInstanceSize",
            "InstDuration", "InstRuleFloat", "InstRuleString", "InstRuleHash", "InstAdditional", "InstClearOnRule",
            "InstRemoveInactive", "InstRemoveBattle")},
            **{name: "manual" for name in INSTANCE_NONE + ("BuffEntryGuid", "BuffGuidSize", "ProtoBuffGroupAdd")}}}),
    Sample(
        "inst44", "结构偏移整体变化",
        "Buff::Instance 在 sub_type（char[64]，到 0x44）后面插入一个 int（审核 A 的 B1 反例 1）：[0x44, 0x152) 的字段后移 4，"
        "0x152..0x158 的尾部填充吸收掉，Instance、Entry 大小不变（靠大小发现不了）。复制构造在一串同形状的 int 复制里多一对，"
        "构造函数多一条清零；构造、BuffRuleActive、BuffGroupErase、BuffDurationUpkeep、BuffGroupAdd 栈上副本里的访问后移 4",
        "只改了清单依据涉及的函数，游戏里其他访问 Instance 的代码没有类型信息、认不全",
        build_inst44,
        # 收紧后的依据：0x44 之后被挪动的字段全部判不成立（运行时也能发现）；BuffEntryInstance 的依据 rel 是 InstDuration，跟着不成立
        verify={"exit": 1, "failures": {(name, "optional") for name in ("BuffEntryInstance",) + INST_MOVED}},
        # 修对的：BuffRuleActive、构造函数、BuffDurationUpkeep 里只是位移变了的（位置核对对上）、和一串 int 复制隔着别的形状的
        # 复制（对齐没有歧义）；只靠一串 int 复制的四项（InstSubHash、InstAmount、InstSource、InstSourceData）放宽的模式唯一命中
        # 但对齐有歧义（插入的一对和相邻的同形状），按 B1 报需要人工（这次放宽的模式恰好命中了对的字段，但在要读的字段之后
        # 插入时它会命中后一个字段，工具分不出这两种情况）；Instance 组的 NONE 项要人工看（S4）
        repair={"exit": 1, "status": {**{name: "repaired" for name in INST_MOVED if name not in INST_RUN_ONLY},
                                      **{name: "manual" for name in INST_RUN_ONLY + INSTANCE_NONE}}}),
    Sample(
        "inst_remove44", "结构偏移整体变化",
        "Buff::Instance 删掉 sub_hash（+0x44 的 int，审核 A 的 B1 反例 2）：[0x48, 0x152) 的字段前移 4，大小不变；复制构造的一串"
        "同形状 int 复制里少一对；构造、BuffRuleActive、BuffGroupErase、BuffDurationUpkeep 里的访问前移 4。InstSubHash 这一项"
        "在新版本里没有对应的字段，任何值都是错的",
        "同 inst44；BuffGroupAdd 栈上副本的访问没改（审核的原脚本也没改）",
        build_inst_remove44,
        verify={"exit": 1, "failures": {(name, "optional") for name in ("BuffEntryInstance",) + INST_MOVED}},
        # InstSubHash 的复制被删掉了：放宽的模式不命中、对齐里这条指令不在匹配块里，报需要人工（真值是「不存在」，给出任何值都
        # 算错）；其余同 inst44
        repair={"exit": 1, "status": {**{name: "repaired" for name in INST_MOVED if name not in INST_RUN_ONLY},
                                      **{name: "manual" for name in INST_RUN_ONLY + INSTANCE_NONE}}}),
    Sample(
        "twin_callers", "多处命中",
        "EffectLookup 多出一份完全相同的副本，而且两份都有调用者：模板的两个实例化（或者 /OPT:ICF 关掉后没合并的两个相同函数），"
        "原来调用它的地方按地址隔一处改调副本（Steam 版 27 处调用分成 14、13 处，10、11 个函数，4 个函数两份都调用），"
        "两份的指纹、调用的目标完全相同，调用者个数接近",
        "副本的位置是 .text 末尾的空隙；调用点是人为分开的",
        build_twin_callers,
        # 特征码命中两次；SkillEffects 的离线检查要用 EffectLookup 的地址，跟着失效
        verify={"exit": 1, "failures": {("EffectLookup", "optional"), ("SkillEffects", "optional")}},
        # 两份指纹相同、都有调用者、调用关系对称：不能挑一个，报需要人工
        repair={"exit": 1, "status": {"EffectLookup": "manual", "SkillEffects": "manual"}}),
]


def generate(samples=None, out_dir=OUT_DIR, log=print):
    """生成样本，返回 Sample 列表（path、truth、notes 填好）。"""
    image = Image(STEAM)
    if image.sha1 != STEAM_SHA1:
        raise pepatch.PatchError(f"Steam 版的 SHA1 不是 {STEAM_SHA1}，样本的改法是照这个版本写的")
    manifest = manifest_mod.load()
    cs = engine()
    sig_results = sigs_mod.resolve_all(image, manifest, "steam", [], cs)
    index = FunctionIndex(image, log=None)
    refs = pepatch.References(image, cs, index)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    done = []
    for sample in samples or SAMPLES:
        ctx = Ctx(image, manifest, sig_results, refs, cs)
        sample.build(ctx)
        path = out_dir / f"{sample.name}.exe"
        ctx.pe.save(path)
        sample.path = str(path)
        sample.truth = ctx.truth
        sample.notes = ctx.notes
        log(f"  {sample.name:<18} {path.name}：" + "；".join(ctx.notes))
        done.append(sample)
    return done

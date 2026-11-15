#include "combat.h"

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ddc::combat {

namespace {

// ---- 结构偏移（Steam 版和离线版相同，tools/check_actor_layout.py 核对），依据 knowledge/dd1/combat.md ----
namespace layout {
constexpr std::uintptr_t kActorBuffGroup = 0x460;  // Buff::Group：vector<GroupEntry> 的 first/last/end
constexpr std::uintptr_t kActorNextGuid = 0x478;   // Group+0x18：下一个组 guid
constexpr std::size_t kEntrySize = 0x418;          // GroupEntry：+0 组 guid，+8 Instance
constexpr std::size_t kEntryInstance = 0x08;
constexpr std::size_t kInstanceSize = 0x410;
constexpr std::size_t kMaxEntries = 4096;    // 读表时的合理性上限（防读到垃圾）；超过时这个 actor 的读写都失败
constexpr std::size_t kAddMaxEntries = 1024;  // 插件添加的上限：表里已经有这么多条时 buff_add 拒绝（C-N6），留出读的余量
// Buff::Instance
constexpr std::size_t kInstStat = 0x00;
constexpr std::size_t kInstSubType = 0x04;
constexpr std::size_t kInstSubHash = 0x44;
constexpr std::size_t kInstAmount = 0x48;
constexpr std::size_t kInstDuration = 0x4C;
constexpr std::size_t kInstDurationType = 0x50;
constexpr std::size_t kInstSource = 0x54;
constexpr std::size_t kInstSourceData = 0x58;
constexpr std::size_t kInstRule = 0x5C;
constexpr std::size_t kInstFalseRule = 0x60;
constexpr std::size_t kInstId = 0x61;
constexpr std::size_t kInstIdHash = 0xA4;
constexpr std::size_t kInstTarget = 0xA8;
constexpr std::size_t kInstRemoveInactive = 0xAC;
constexpr std::size_t kInstRemoveBattle = 0xAD;
constexpr std::size_t kInstRuleFloat = 0xB0;
constexpr std::size_t kInstRuleString = 0xB4;
constexpr std::size_t kInstRuleHash = 0xF4;
constexpr std::size_t kInstAdditional = 0x158;     // 25 个 vector<char[64]>，每个 0x18
constexpr std::size_t kInstClearOnRule = 0x3B0;    // vector<char[64]>
constexpr std::size_t kInstReadSize = 0xF8;        // 读快照用到的前缀（到 rule_data 的哈希为止）
// Buff::Library
constexpr std::uintptr_t kLibraryAltMap = 0x10;
// MSVC std::map 的节点
constexpr std::uintptr_t kNodeLeft = 0x00;
constexpr std::uintptr_t kNodeRight = 0x10;
constexpr std::uintptr_t kNodeIsNil = 0x19;
constexpr std::uintptr_t kNodeKey = 0x20;
constexpr std::uintptr_t kNodeValue = 0x28;
// Effect（4F02D0 的字段，类型按写入函数：36E940 float，36ED50 int）
constexpr std::size_t kEffectTarget = 0x44;
constexpr std::size_t kEffectChance = 0x48;
constexpr std::size_t kEffectStress = 0x4C;
constexpr std::size_t kEffectStun = 0x50;
constexpr std::size_t kEffectHeal = 0xC8;
constexpr std::size_t kEffectHealPercent = 0xCC;
constexpr std::size_t kEffectCombatStatBuff = 0xEC;
constexpr std::size_t kEffectDotPoison = 0x134;
constexpr std::size_t kEffectDotBleed = 0x138;
constexpr std::size_t kEffectDotBurn = 0x13C;
constexpr std::size_t kEffectDotStress = 0x1B8;
constexpr std::size_t kEffectDuration = 0x1CC;
constexpr std::size_t kEffectOnHit = 0x345;
constexpr std::size_t kEffectOnMiss = 0x346;
constexpr std::size_t kEffectApplyOnce = 0x347;
constexpr std::size_t kEffectQueue = 0x348;
constexpr std::size_t kEffectBuffAmount = 0x3B8;
constexpr std::size_t kEffectBuffIds = 0x3C8;      // vector<uint32>
constexpr std::size_t kEffectSwap = 0x3E0;
constexpr std::size_t kEffectBuffDurationType = 0x430;
constexpr std::size_t kEffectReadSize = 0x438;
// 技能
constexpr std::uintptr_t kHeroClassSkills = 0xBD0;      // vector<vector<HeroCombatSkill>>，外层每项 0x18
constexpr std::uintptr_t kHeroClassMoveSkills = 0xBE8;  // vector<HeroCombatSkill>
constexpr std::size_t kHeroSkillSize = 0x510;
constexpr std::uintptr_t kMonsterClassSkills = 0xBA8;   // vector<MonsterSkill>
constexpr std::size_t kMonsterSkillSize = 0x500;
constexpr std::size_t kSkillId = 0x08;
constexpr std::size_t kSkillLevel = 0x48;
constexpr std::size_t kSkillType = 0x50;
constexpr std::size_t kSkillFriendly = 0x90;
constexpr std::size_t kSkillEffects = 0x3C8;            // vector<Effect*>
// 其他
constexpr std::uintptr_t kRaidPvpName = 0x52C0;  // Raid::Current+0x52C0：模式名，PvP 是 arena_mp（ADR 0011）
constexpr std::size_t kSlotAddBuff = 13;
constexpr std::size_t kSlotRemoveBuffs = 14;
}  // namespace layout

using namespace layout;

constexpr std::size_t kQueueLimit = 256;
constexpr std::size_t kEffectTextMax = 64 * 1024;

// ---- 读游戏内存（SEH） ----

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    if (src < 0x10000) {
        return false;
    }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
T Rd(std::uintptr_t address, T fallback = T{})
{
    T value{};
    return ReadBytes(address, &value, sizeof(T)) ? value : fallback;
}

void CopyText(char (&dst)[DD_COMBAT_TEXT], const std::uint8_t* src)
{
    std::memcpy(dst, src, DD_COMBAT_TEXT);
    dst[DD_COMBAT_TEXT - 1] = 0;
}

void CopyText(char (&dst)[DD_COMBAT_TEXT], const std::string& src)
{
    std::memset(dst, 0, DD_COMBAT_TEXT);
    std::memcpy(dst, src.data(), (std::min)(src.size(), static_cast<std::size_t>(DD_COMBAT_TEXT - 1)));
}

std::string FixedString(const std::uint8_t* src, std::size_t cap)
{
    return std::string(reinterpret_cast<const char*>(src), strnlen(reinterpret_cast<const char*>(src), cap));
}

// vector 的 first/last，结构不合法时返回 false
bool ReadVector(std::uintptr_t address, std::uintptr_t* first, std::uintptr_t* last)
{
    std::uintptr_t v[2] = {};
    if (!ReadBytes(address, v, sizeof(v))) {
        return false;
    }
    if (v[0] == 0 && v[1] == 0) {
        *first = *last = 0;
        return true;
    }
    if (v[0] == 0 || v[1] < v[0]) {
        return false;
    }
    *first = v[0];
    *last = v[1];
    return true;
}

// MSVC std::map<uint32, V> 的 lower_bound，命中返回节点的值地址（+0x28），否则 0
std::uintptr_t MapFind(std::uintptr_t map, std::uint32_t key)
{
    const auto head = Rd<std::uintptr_t>(map);
    if (!head) {
        return 0;
    }
    std::uintptr_t node = Rd<std::uintptr_t>(head + 8);  // 根
    std::uintptr_t result = head;
    for (int guard = 0; guard < 64 && node && !Rd<std::uint8_t>(node + kNodeIsNil, 1); ++guard) {
        if (Rd<std::uint32_t>(node + kNodeKey) < key) {
            node = Rd<std::uintptr_t>(node + kNodeRight);
        } else {
            result = node;
            node = Rd<std::uintptr_t>(node + kNodeLeft);
        }
    }
    if (result == head || Rd<std::uint32_t>(result + kNodeKey) != key) {
        return 0;
    }
    return result + kNodeValue;
}

// ---- 调用游戏函数（SEH，只用 POD） ----

struct alignas(16) InstBuf {
    std::uint8_t b[kInstanceSize];
};

using CopyFn = void* (*)(void*, const void*);
using VecDtorFn = void (*)(void*);
using EhVecDtorFn = void (*)(void*, std::size_t, std::size_t, void*);
using AddBuffFn = std::uint32_t (*)(void*, const void*, void*, bool);
using RemoveBuffsFn = bool (*)(void*, void*);
using LibraryIndexFn = void* (*)(void*, const std::uint32_t*);
using ApplyEffectFn = void* (*)(void*, void*, void*, void*, const void*, float, bool, float, int, std::uint32_t, bool, bool);
using PrngFloatFn = float (*)(void*);
using FreeVecFn = void (*)(void*);
using ParseEffectsFn = void (*)(const char*, bool);

bool G_InstCopy(void* fn, InstBuf* buf, std::uintptr_t def)
{
    __try {
        reinterpret_cast<CopyFn>(fn)(buf->b, reinterpret_cast<const void*>(def));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_InstDestroy(void* vecDtor, void* ehVecDtor, void* elemDtor, InstBuf* buf)
{
    __try {
        reinterpret_cast<VecDtorFn>(vecDtor)(buf->b + kInstClearOnRule);
        reinterpret_cast<EhVecDtorFn>(ehVecDtor)(buf->b + kInstAdditional, 0x18, 0x19, elemDtor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_AddBuff(std::uintptr_t actor, const InstBuf* buf, bool refresh, std::uint32_t* guid)
{
    __try {
        auto** vtable = *reinterpret_cast<void***>(actor);
        *guid = reinterpret_cast<AddBuffFn>(vtable[kSlotAddBuff])(reinterpret_cast<void*>(actor), buf->b, nullptr, refresh);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_RemoveBuffs(std::uintptr_t actor, void* function, bool* removed)
{
    __try {
        auto** vtable = *reinterpret_cast<void***>(actor);
        *removed = reinterpret_cast<RemoveBuffsFn>(vtable[kSlotRemoveBuffs])(reinterpret_cast<void*>(actor), function);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_LibraryIndex(void* fn, std::uintptr_t map, std::uint32_t key, std::uintptr_t* out)
{
    __try {
        *out = reinterpret_cast<std::uintptr_t>(reinterpret_cast<LibraryIndexFn>(fn)(reinterpret_cast<void*>(map), &key));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ApplyArgs {
    void* applyFn;
    void* prngFloatFn;
    void* freeFn;
    void* prng;
    std::uintptr_t performer;
    std::uintptr_t target;
    std::uintptr_t effect;
    std::uint32_t guids;  // 输出：本次加上的 buff 条数
};

bool G_ApplyEffect(ApplyArgs* a)
{
    alignas(16) std::uint8_t res[0x20] = {};
    __try {
        const float roll = reinterpret_cast<PrngFloatFn>(a->prngFloatFn)(a->prng);
        reinterpret_cast<ApplyEffectFn>(a->applyFn)(res, a->prng, reinterpret_cast<void*>(a->performer),
            reinterpret_cast<void*>(a->target), reinterpret_cast<const void*>(a->effect), roll, false, 1.0f, 0, 0, false, false);
        std::uintptr_t vec[2] = {};
        std::memcpy(vec, res + 8, sizeof(vec));
        a->guids = (vec[0] && vec[1] >= vec[0]) ? static_cast<std::uint32_t>((vec[1] - vec[0]) / 4) : 0;
        reinterpret_cast<FreeVecFn>(a->freeFn)(res + 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_ParseEffects(void* fn, const char* path)
{
    __try {
        reinterpret_cast<ParseEffectsFn>(fn)(path, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 把定义的 POD 字段写进库里的 Instance（字符串字段同时写哈希）
void PutText(std::uint8_t* p, std::size_t offset, std::size_t hashOffset, const char (&text)[DD_COMBAT_TEXT])
{
    std::memset(p + offset, 0, DD_COMBAT_TEXT);
    const std::size_t n = strnlen(text, DD_COMBAT_TEXT - 1);
    std::memcpy(p + offset, text, n);
    const std::uint32_t h = GameHash(std::string_view(text, n));
    std::memcpy(p + hashOffset, &h, 4);
}

bool G_FillDefinition(std::uintptr_t inst, const DDBuffDef* def)
{
    __try {
        auto* p = reinterpret_cast<std::uint8_t*>(inst);
        std::memcpy(p + kInstStat, &def->stat_type, 4);
        PutText(p, kInstSubType, kInstSubHash, def->stat_sub_type);
        std::memcpy(p + kInstAmount, &def->amount, 4);
        std::memcpy(p + kInstDuration, &def->duration, 4);
        std::memcpy(p + kInstDurationType, &def->duration_type, 4);
        std::memcpy(p + kInstSource, &def->source, 4);
        std::memcpy(p + kInstSourceData, &def->sourcedata, 4);
        std::memcpy(p + kInstRule, &def->rule_type, 4);
        p[kInstFalseRule] = def->is_false_rule ? 1 : 0;
        PutText(p, kInstId, kInstIdHash, def->id);
        const std::int32_t zero = 0;
        std::memcpy(p + kInstTarget, &zero, 4);
        p[kInstRemoveInactive] = def->remove_if_not_active ? 1 : 0;
        p[kInstRemoveBattle] = def->remove_on_battle_complete ? 1 : 0;
        std::memcpy(p + kInstRuleFloat, &def->rule_float, 4);
        PutText(p, kInstRuleString, kInstRuleHash, def->rule_string);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- std::function<bool(GroupEntry const&)> 兼容的谓词（槽位 14） ----
// 同 DDCore 事件桥和 probe_p4 的做法：实现对象由调用方持有，复制、移动返回自己，析构什么都不做；
// 函数对象 0x40 字节，+0x38 指向实现。5CE600 只按引用捕获它，最后经实现的 _Delete_this（+0x20）析构
// combat.buff_remove 正在删除：谓词命中的组 guid（buff.remove 的 reason = plugin）。只有这些条目算 plugin，槽位 14 删完后
// UpdateBuffsInternal 按规则顺带删掉的别的条目按调用栈判断（A-N2）
thread_local std::vector<std::uint32_t>* t_pluginHits = nullptr;
struct RemovePredicate {
    void* const* vtable;
    std::uint32_t mode;      // DDBuffMatch（BY_TAG 时已经换成 guid 列表）
    std::uint32_t idHash;
    std::int32_t source;
    const std::uint32_t* guids;  // 升序
    std::size_t guidCount;
    std::uint32_t hits;
};

void* __fastcall PredCopy(RemovePredicate* self, void*)
{
    return self;
}
void* __fastcall PredMove(RemovePredicate* self, void*)
{
    return self;
}
bool __fastcall PredDoCall(RemovePredicate* self, const std::uint8_t* entry)
{
    bool hit = false;
    switch (self->mode) {
    case DD_BUFF_BY_GUID:
    case DD_BUFF_BY_TAG: {
        std::uint32_t guid = 0;
        std::memcpy(&guid, entry, 4);
        hit = std::binary_search(self->guids, self->guids + self->guidCount, guid);
        break;
    }
    case DD_BUFF_BY_ID: {
        std::uint32_t hash = 0;
        std::memcpy(&hash, entry + kEntryInstance + kInstIdHash, 4);
        hit = hash == self->idHash;
        break;
    }
    case DD_BUFF_BY_SOURCE: {
        std::int32_t source = 0;
        std::memcpy(&source, entry + kEntryInstance + kInstSource, 4);
        hit = source == self->source;
        break;
    }
    default:
        break;
    }
    self->hits += hit ? 1 : 0;
    if (hit && t_pluginHits) {
        std::uint32_t guid = 0;
        std::memcpy(&guid, entry, 4);
        t_pluginHits->push_back(guid);
    }
    return hit;
}
const void* __fastcall PredTarget(RemovePredicate*)
{
    static const int kDummy = 0;
    return &kDummy;
}
void __fastcall PredDelete(RemovePredicate*, bool) {}
const void* __fastcall PredGet(RemovePredicate* self)
{
    return self;
}
void* const kPredVtable[] = {
    reinterpret_cast<void*>(&PredCopy),
    reinterpret_cast<void*>(&PredMove),
    reinterpret_cast<void*>(&PredDoCall),
    reinterpret_cast<void*>(&PredTarget),
    reinterpret_cast<void*>(&PredDelete),
    reinterpret_cast<void*>(&PredGet),
};

// ---- 线程状态 ----
thread_local int t_gameCallDepth = 0;  // DDCore 自己调用游戏函数的层数
thread_local bool t_inRule = false;
thread_local std::string t_message;    // out->message 指向它
// combat.buff_add 正在添加时，buff.add 的 hook 先把标签记上，负载里就有 plugin、tag
struct AddContext {
    std::uintptr_t actor = 0;
    DDActorRef ref{};
    std::uint32_t owner = 0;
    const std::string* tag = nullptr;
};
thread_local AddContext* t_addContext = nullptr;
// 47AAD0（持续时间结算）正在执行：入口时的（guid，duration），原函数里减到 0 被删掉的条目在删除前补派发一次 buff.tick（A-N1）
struct UpkeepContext {
    std::uintptr_t actor;
    const void* before;  // const std::vector<Combat::Impl::TickBefore>*
};
thread_local UpkeepContext* t_upkeep = nullptr;

struct GameCall {
    GameCall() { ++t_gameCallDepth; }
    ~GameCall() { --t_gameCallDepth; }
};

double QpcUs()
{
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1e6 / freq;
}

// ---- detour ----
using GroupAddFn = void* (__fastcall*)(void*, std::uint32_t, const void*);
using GroupEraseFn = void* (__fastcall*)(void*, void*, void*);
using UpkeepFn = std::uint64_t (__fastcall*)(void*, void*, int);
using RuleFn = bool (__fastcall*)(void*, const void*, const void*, void*, const void*);

Combat::Impl* g_impl = nullptr;

void* __fastcall GroupAddDetour(void* group, std::uint32_t guid, const void* inst);
void* __fastcall GroupEraseDetour(void* group, void* out, void* entry);
std::uint64_t __fastcall UpkeepDetour(void* actor, void* filter, int type);
bool __fastcall RuleDetour(void* self, const void* inst, const void* p3, void* target, const void* skill);

// buff.* 的字段
enum BuffField {
    BF_actor, BF_kind, BF_actorId, BF_guid, BF_id, BF_stat, BF_amount, BF_duration, BF_durationType, BF_source,
    BF_sourceData, BF_rule, BF_plugin, BF_tag, BF_count,
};
constexpr std::uint32_t BF_reason = BF_count;          // buff.remove
constexpr std::uint32_t BF_durationBefore = BF_count;  // buff.tick
constexpr std::uint32_t BF_durationAfter = BF_count + 1;

std::vector<events::FieldDesc> BuffFields(int extra)
{
    using events::FieldDesc;
    using events::FieldRange;
    using events::kAccessR;
    std::vector<FieldDesc> fields = {
        FieldDesc{ "actor", DD_FIELD_POINTER, kAccessR },
        FieldDesc{ "actor_kind", DD_FIELD_INT, kAccessR, FieldRange::U32 },
        FieldDesc{ "actor_id", DD_FIELD_INT, kAccessR, FieldRange::U32 },
        FieldDesc{ "guid", DD_FIELD_INT, kAccessR, FieldRange::U32 },
        FieldDesc{ "id", DD_FIELD_STRING, kAccessR },
        FieldDesc{ "stat_type", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "amount", DD_FIELD_FLOAT, kAccessR, FieldRange::F32 },
        FieldDesc{ "duration", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "duration_type", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "source", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "sourcedata", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "rule_type", DD_FIELD_INT, kAccessR, FieldRange::I32 },
        FieldDesc{ "plugin", DD_FIELD_STRING, kAccessR },
        FieldDesc{ "tag", DD_FIELD_STRING, kAccessR },
    };
    if (extra == 1) {
        fields.push_back(FieldDesc{ "reason", DD_FIELD_STRING, kAccessR });
    } else if (extra == 2) {
        fields.push_back(FieldDesc{ "duration_before", DD_FIELD_INT, kAccessR, FieldRange::I32 });
        fields.push_back(FieldDesc{ "duration_after", DD_FIELD_INT, kAccessR, FieldRange::I32 });
    }
    return fields;
}

// 一条 buff 的快照（hook 里复制出来，派发时字段指向这里）
struct EntrySnap {
    std::uint32_t guid = 0;
    std::int32_t stat = 0, duration = 0, durationType = 0, source = 0, sourceData = 0, rule = 0;
    float amount = 0;
    char id[DD_COMBAT_TEXT] = {};
};

bool ReadEntry(std::uintptr_t entry, EntrySnap* out)
{
    std::uint8_t raw[kEntryInstance + kInstReadSize] = {};
    if (!ReadBytes(entry, raw, sizeof(raw))) {
        return false;
    }
    const std::uint8_t* inst = raw + kEntryInstance;
    std::memcpy(&out->guid, raw, 4);
    std::memcpy(&out->stat, inst + kInstStat, 4);
    std::memcpy(&out->amount, inst + kInstAmount, 4);
    std::memcpy(&out->duration, inst + kInstDuration, 4);
    std::memcpy(&out->durationType, inst + kInstDurationType, 4);
    std::memcpy(&out->source, inst + kInstSource, 4);
    std::memcpy(&out->sourceData, inst + kInstSourceData, 4);
    std::memcpy(&out->rule, inst + kInstRule, 4);
    CopyText(out->id, inst + kInstId);
    return true;
}

}  // namespace

// ============================================================================

struct Combat::Impl {
    CombatDeps deps;
    OpQueue queue{ kQueueLimit };
    TagTable tags;
    RuleRegistry rules;
    RuleStatsAccum ruleStats;
    std::atomic<std::uint64_t> ruleSkippedOffMain{ 0 };
    OwnerTable buffOwners;
    OwnerTable effectOwners;
    std::map<std::uint32_t, std::pair<DDCombatOpFn, void*>> handlers;
    std::uint32_t tmpCounter = 0;
    std::uint32_t lastBattleSerial = 0;
    std::uint32_t lastRaidSerial = 0;
    std::uint64_t opsExecuted = 0;
    std::uint64_t opsFailed = 0;

    events::Topic* buffAdd = nullptr;
    events::Topic* buffRemove = nullptr;
    events::Topic* buffTick = nullptr;
    GroupAddFn origAdd = nullptr;
    GroupEraseFn origErase = nullptr;
    UpkeepFn origUpkeep = nullptr;
    RuleFn origRule = nullptr;

    // buff.remove 原因判断用的函数起始地址
    struct ReasonFn {
        std::uintptr_t begin;
        const char* reason;
    };
    std::vector<ReasonFn> reasonFns;
    std::uintptr_t slot14Hero = 0, slot14Monster = 0;

    void Log(int level, const std::string& text) const
    {
        if (deps.log) {
            deps.log(level, text);
        }
    }

    bool IsMain() const { return GetCurrentThreadId() == deps.mainThread; }

    bool ActivePvP() const
    {
        if (!deps.addr->raidCurrent) {
            return false;
        }
        const auto raid = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(deps.addr->raidCurrent));
        if (!raid) {
            return false;
        }
        const auto name = Rd<std::uintptr_t>(raid + kRaidPvpName);
        char text[16] = {};
        return name && ReadBytes(name, text, 9) && std::strncmp(text, "arena_mp", 9) == 0;
    }

    bool RaidActive() const
    {
        return deps.addr->raidCurrent && Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(deps.addr->raidCurrent)) != 0;
    }

    TimingState Now() const
    {
        TimingState s;
        s.mainThread = IsMain();
        s.inRule = t_inRule;
        s.campaignActive = deps.campaignActive && deps.campaignActive();
        s.loading = deps.loadingNow && deps.loadingNow();
        s.pvp = s.mainThread && ActivePvP();
        s.inFrameBody = deps.inFrameBody && deps.inFrameBody();
        s.dispatchDepth = (deps.events ? deps.events->hub().Depth() : 0) + (deps.extraDepth ? deps.extraDepth() : 0);
        s.gameCallDepth = t_gameCallDepth;
        return s;
    }

    std::uint32_t Result(DDCombatOpResult* out, OpKind kind, std::uint32_t code, const std::string& message)
    {
        t_message = message;
        if (out && out->size >= sizeof(DDCombatOpResult)) {
            out->kind = static_cast<std::uint32_t>(kind);
            out->op = 0;
            out->result = code;
            out->guid = 0;
            out->count = 0;
            out->message = t_message.c_str();
        }
        return code;
    }

    // ---- actor 和 buff 表 ----
    bool IsActor(std::uintptr_t p) const
    {
        const auto vt = Rd<std::uintptr_t>(p);
        return vt && (vt == reinterpret_cast<std::uintptr_t>(deps.addr->heroVtable) ||
                         vt == reinterpret_cast<std::uintptr_t>(deps.addr->monsterVtable));
    }

    std::uintptr_t Resolve(const DDActorRef& ref) const
    {
        if (!deps.actorEnv) {
            return 0;
        }
        const auto p = reinterpret_cast<std::uintptr_t>(game::ActorResolve(deps.actorEnv(), ref));
        return p && IsActor(p) ? p : 0;
    }

    bool ActorRefOf(std::uintptr_t actor, DDActorRef* out) const
    {
        if (!deps.actorEnv || !IsMain()) {
            return false;
        }
        out->size = sizeof(DDActorRef);
        return game::ActorFromPointer(deps.actorEnv(), reinterpret_cast<const void*>(actor), out);
    }

    bool Table(std::uintptr_t actor, std::uintptr_t* first, std::size_t* count) const
    {
        std::uintptr_t last = 0;
        if (!ReadVector(actor + kActorBuffGroup, first, &last)) {
            return false;
        }
        if ((last - *first) % kEntrySize != 0 || (last - *first) / kEntrySize > kMaxEntries) {
            return false;
        }
        *count = (last - *first) / kEntrySize;
        return true;
    }

    // 表读不出来（超过 kMaxEntries、内存读失败）时返回 false，调用方不能把它当成空表
    bool Guids(std::uintptr_t actor, std::vector<std::uint32_t>* out) const
    {
        out->clear();
        std::uintptr_t first = 0;
        std::size_t count = 0;
        if (!Table(actor, &first, &count)) {
            return false;
        }
        out->reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out->push_back(Rd<std::uint32_t>(first + i * kEntrySize));
        }
        return true;
    }

    std::uintptr_t FindEntry(std::uintptr_t actor, std::uint32_t guid) const
    {
        std::uintptr_t first = 0;
        std::size_t count = 0;
        if (!Table(actor, &first, &count)) {
            return 0;
        }
        for (std::size_t i = count; i-- > 0;) {
            if (Rd<std::uint32_t>(first + i * kEntrySize) == guid) {
                return first + i * kEntrySize;
            }
        }
        return 0;
    }

    // 标签表里这个 actor 的过期记录顺手删掉（条目不在表里了）。表读不出来时不删（C-N6：读失败当成空表会清掉全部标签）；
    // 只在主线程、没有加载线程在跑时调用（A-S3：加载线程可能正在前移或搬动这张表）
    void PruneTags(const DDActorRef& ref, std::uintptr_t actor)
    {
        if (!ReadSafe()) {
            return;
        }
        std::vector<std::uint32_t> live;
        if (!Guids(actor, &live)) {
            return;
        }
        std::sort(live.begin(), live.end());
        tags.PruneActor(ref, live);
    }

    // 读接口的前提（A-S3）：主线程，并且没有加载线程（load_campaign、load_raid、load_town）在跑——
    // 加载线程会增删 actor 的 buff 表（删除时条目前移、扩容时整张表搬家）、重建 Buff 库和 effect map
    bool ReadSafe() const { return IsMain() && !(deps.loadingNow && deps.loadingNow()); }

    std::uint32_t ActorId(std::uintptr_t actor, std::uint32_t* kind) const
    {
        const auto vt = Rd<std::uintptr_t>(actor);
        if (vt == reinterpret_cast<std::uintptr_t>(deps.addr->heroVtable)) {
            *kind = DD_ACTOR_HERO;
            return Rd<std::uint32_t>(actor + game::actor_layout::kHeroId);
        }
        *kind = DD_ACTOR_MONSTER;
        return Rd<std::uint32_t>(actor + game::actor_layout::kMonsterGuid);
    }

    // ---- 库 ----
    std::uintptr_t LibraryMap() const
    {
        if (!deps.addr->buffLibrary) {
            return 0;
        }
        const auto lib = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(deps.addr->buffLibrary));
        if (!lib) {
            return 0;
        }
        const bool alt = deps.addr->altRosterFlag && Rd<std::uint8_t>(reinterpret_cast<std::uintptr_t>(deps.addr->altRosterFlag));
        return lib + (alt ? kLibraryAltMap : 0);
    }

    std::uintptr_t FindDefinition(std::uint32_t hash) const
    {
        const auto map = LibraryMap();
        return map ? MapFind(map, hash) : 0;
    }

    std::uintptr_t FindEffect(std::uint32_t hash) const
    {
        if (!deps.addr->effectMap) {
            return 0;
        }
        const auto value = MapFind(reinterpret_cast<std::uintptr_t>(deps.addr->effectMap), hash);
        return value ? Rd<std::uintptr_t>(value) : 0;
    }

    std::string PluginName(std::uint32_t owner) const { return deps.pluginName ? deps.pluginName(owner) : "?"; }
    // persist 恢复、导出标签时用的游戏侧查询
    TagLookup GameTagLookup();

    // ---- 执行写操作（已经确认在安全点） ----
    struct Outcome {
        std::uint32_t code = DD_COMBAT_OK;
        std::uint32_t guid = 0;
        std::uint32_t count = 0;
        std::string message;
    };

    static Outcome Fail(std::uint32_t code, std::string message)
    {
        Outcome o;
        o.code = code;
        o.message = std::move(message);
        return o;
    }

    bool CanAddBuffs() const
    {
        const auto* a = deps.addr;
        return a->buffLibrary && a->buffInstanceCopy && a->vectorDtor64 && a->vectorDtor64b && a->ehVectorDtor && a->heroVtable &&
            a->monsterVtable;
    }

    Outcome DoAdd(std::uint32_t owner, const AddRequest& r)
    {
        if (!CanAddBuffs()) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "buff_add 依赖的特征码缺失（BuffLibrary、BuffInstanceCopy、析构函数或虚表）");
        }
        const std::uintptr_t actor = Resolve(r.actor);
        if (!actor) {
            return Fail(DD_COMBAT_E_ACTOR, "actor 句柄解析失败");
        }
        const std::uintptr_t def = FindDefinition(GameHash(r.id));
        if (!def) {
            return Fail(DD_COMBAT_E_NOT_FOUND, "Buff 库里没有 " + r.id);
        }
        {
            std::uintptr_t first = 0;
            std::size_t count = 0;
            if (!Table(actor, &first, &count) || count >= kAddMaxEntries) {
                return Fail(DD_COMBAT_E_INVALID, "这个 actor 的 buff 表已经有 " + std::to_string(kAddMaxEntries) + " 条以上（或读不出来），不再添加");
            }
        }
        InstBuf buf{};
        if (!G_InstCopy(deps.addr->buffInstanceCopy, &buf, def)) {
            return Fail(DD_COMBAT_E_GAME, "复制 buff 定义时出了异常");
        }
        const auto put = [&](std::size_t offset, std::int32_t value) { std::memcpy(buf.b + offset, &value, 4); };
        if (r.options.flags & DD_BUFF_ADD_DURATION) {
            put(kInstDuration, r.options.duration);
        }
        if (r.options.flags & DD_BUFF_ADD_DURATION_TYPE) {
            put(kInstDurationType, r.options.duration_type);
        }
        if (r.options.flags & DD_BUFF_ADD_SOURCE) {
            put(kInstSource, r.options.source);
        }
        if (r.options.flags & DD_BUFF_ADD_SOURCEDATA) {
            put(kInstSourceData, r.options.sourcedata);
        }
        put(kInstTarget, 0);  // target 为 1 时 Hero 的槽位 13 不判空地遍历 Raid::Current 的英雄表（城镇里会崩）
        const bool refresh = !(r.options.flags & DD_BUFF_ADD_NO_REFRESH);
        std::uint32_t guid = 0;
        bool ok = false;
        {
            AddContext context{ actor, r.actor, owner, &r.tag };
            AddContext* outer = t_addContext;
            t_addContext = &context;
            GameCall call;
            ok = G_AddBuff(actor, &buf, refresh, &guid);
            t_addContext = outer;
        }
        const bool destroyed = G_InstDestroy(deps.addr->vectorDtor64, deps.addr->ehVectorDtor, deps.addr->vectorDtor64b, &buf);
        if (!ok) {
            return Fail(DD_COMBAT_E_GAME, "调用槽位 13 添加 buff 时出了异常");
        }
        if (!destroyed) {
            Log(DD_LOG_ERROR, "combat：析构临时 Buff::Instance 时出了异常（buff 已经加上，guid " + std::to_string(guid) + "）");
        }
        tags.Set(r.actor, guid, owner, r.tag);
        Outcome o;
        o.guid = guid;
        return o;
    }

    Outcome DoRemove(std::uint32_t owner, const RemoveRequest& r)
    {
        if (!deps.addr->heroVtable || !deps.addr->monsterVtable) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "特征码 HeroVtable/MonsterVtable 缺失");
        }
        const std::uintptr_t actor = Resolve(r.actor);
        if (!actor) {
            return Fail(DD_COMBAT_E_ACTOR, "actor 句柄解析失败");
        }
        PruneTags(r.actor, actor);
        std::vector<std::uint32_t> guids;
        RemovePredicate pred{ kPredVtable, r.spec.match, 0, r.spec.source, nullptr, 0, 0 };
        switch (r.spec.match) {
        case DD_BUFF_BY_GUID:
            guids.push_back(r.spec.guid);
            break;
        case DD_BUFF_BY_TAG:
            guids = tags.Find(r.actor, owner, r.text);
            break;
        case DD_BUFF_BY_ID:
            pred.idHash = GameHash(r.text);
            break;
        default:
            break;
        }
        if ((r.spec.match == DD_BUFF_BY_GUID || r.spec.match == DD_BUFF_BY_TAG) && guids.empty()) {
            return Outcome{};  // 没有匹配的，什么都不做
        }
        std::sort(guids.begin(), guids.end());
        pred.guids = guids.data();
        pred.guidCount = guids.size();
        alignas(16) std::uint8_t function[0x40] = {};
        void* impl = &pred;
        std::memcpy(function + 0x38, &impl, sizeof(impl));
        bool removed = false;
        bool ok = false;
        {
            std::vector<std::uint32_t> hits;
            std::vector<std::uint32_t>* outer = t_pluginHits;
            t_pluginHits = &hits;
            GameCall call;
            ok = G_RemoveBuffs(actor, function, &removed);
            t_pluginHits = outer;
        }
        if (!ok) {
            return Fail(DD_COMBAT_E_GAME, "调用槽位 14 删除 buff 时出了异常");
        }
        PruneTags(r.actor, actor);
        Outcome o;
        o.count = pred.hits;
        return o;
    }

    Outcome DoDefine(std::uint32_t owner, const DDBuffDef& def)
    {
        if (!deps.addr->buffLibrary || !deps.addr->buffLibraryIndex) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "特征码 BuffLibrary/BuffLibraryIndex 缺失");
        }
        const std::uintptr_t map = LibraryMap();
        if (!map) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "Buff::Library 还不存在");
        }
        const std::string id(def.id, strnlen(def.id, DD_COMBAT_TEXT));
        const std::uint32_t hash = GameHash(id);
        const std::uintptr_t existing = MapFind(map, hash);
        const std::uint32_t recorded = buffOwners.OwnerOf(hash);
        if (existing && recorded != owner) {
            return Fail(DD_COMBAT_E_EXISTS, recorded ? "id " + id + " 已经由插件 " + PluginName(recorded) + " 注册"
                                                     : "id " + id + " 是游戏数据（或哈希和游戏数据相同），不能覆盖");
        }
        std::uintptr_t inst = existing;
        if (!inst) {
            GameCall call;
            if (!G_LibraryIndex(deps.addr->buffLibraryIndex, map, hash, &inst) || !inst) {
                return Fail(DD_COMBAT_E_GAME, "在 Buff 库里插入定义时出了异常");
            }
        }
        if (!G_FillDefinition(inst, &def)) {
            return Fail(DD_COMBAT_E_GAME, "写定义字段时出了异常");
        }
        buffOwners.Set(hash, owner, id);
        return Outcome{};
    }

    Outcome DoApply(const EffectRequest& r)
    {
        const auto* a = deps.addr;
        if (!a->applyEffect || !a->prngFloat || !a->freeU32Vector || !a->gamePrng || !a->effectMap) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "effect_apply 依赖的特征码缺失（ApplyEffect、PrngFloat、FreeU32Vector、GamePrng、EffectMap）");
        }
        if (!RaidActive()) {
            return Fail(DD_COMBAT_E_NO_RAID, "没有 Raid::Current（城镇里不能施加 effect）");
        }
        const std::uintptr_t effect = FindEffect(GameHash(r.name));
        if (!effect) {
            return Fail(DD_COMBAT_E_NOT_FOUND, "没有 effect " + r.name);
        }
        const std::uintptr_t performer = Resolve(r.performer);
        const std::uintptr_t target = r.hasTarget ? Resolve(r.target) : performer;
        if (!performer || !target) {
            return Fail(DD_COMBAT_E_ACTOR, "performer 或 target 的句柄解析失败");
        }
        ApplyArgs args{ a->applyEffect, a->prngFloat, a->freeU32Vector, a->gamePrng, performer, target, effect, 0 };
        bool ok = false;
        {
            GameCall call;
            ok = G_ApplyEffect(&args);
        }
        if (!ok) {
            return Fail(DD_COMBAT_E_GAME, "调用 ApplyEffect 时出了异常");
        }
        Outcome o;
        o.count = args.guids;
        return o;
    }

    Outcome DoDefineEffects(std::uint32_t owner, const std::string& text)
    {
        if (!deps.addr->parseEffectFile || !deps.addr->effectMap) {
            return Fail(DD_COMBAT_E_UNAVAILABLE, "特征码 ParseEffectFile/EffectMap 缺失");
        }
        std::vector<std::string> names;
        std::string why;
        if (!ExtractEffectNames(text, &names, &why)) {
            return Fail(DD_COMBAT_E_INVALID, why);
        }
        for (const auto& name : names) {
            const std::uint32_t hash = GameHash(name);
            const std::uint32_t recorded = effectOwners.OwnerOf(hash);
            if (FindEffect(hash) && recorded != owner) {
                return Fail(DD_COMBAT_E_EXISTS, recorded ? "effect " + name + " 已经由插件 " + PluginName(recorded) + " 定义"
                                                         : "effect " + name + " 是游戏数据（或哈希和游戏数据相同），不能重新定义");
            }
        }
        // 写进 <游戏根目录>\DDModLoader\tmp\，用相对游戏根目录的路径交给游戏（探针 0A-10：绝对、相对两种都能解析；
        // 相对路径不受游戏根目录里非 ASCII 字符的影响）
        // 插件名来自 DDPlugin_Query，可能含 \、..、非 ASCII：只留 [A-Za-z0-9_-]，最多 32 个字符（A-N8）
        std::string safe;
        for (const char c : PluginName(owner)) {
            if (safe.size() < 32 && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) {
                safe.push_back(c);
            }
        }
        const std::string base = (safe.empty() ? std::string("plugin") : safe) + "_" + std::to_string(++tmpCounter) + ".effects.darkest";
        const std::string dirAbs = deps.gameRoot + "\\DDModLoader\\tmp";
        const std::string fileAbs = dirAbs + "\\" + base;
        const std::string fileRel = "DDModLoader/tmp/" + base;
        const auto wide = [](const std::string& s) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
            return w;
        };
        CreateDirectoryW(wide(dirAbs).c_str(), nullptr);
        HANDLE file = CreateFileW(wide(fileAbs).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return Fail(DD_COMBAT_E_GAME, "写临时文件失败：" + fileAbs + "（错误码 " + std::to_string(GetLastError()) + "）");
        }
        DWORD written = 0;
        const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        CloseHandle(file);
        bool parsed = false;
        if (wrote) {
            GameCall call;
            parsed = G_ParseEffects(deps.addr->parseEffectFile, fileRel.c_str());
        }
        DeleteFileW(wide(fileAbs).c_str());
        if (!wrote) {
            return Fail(DD_COMBAT_E_GAME, "写临时文件失败：" + fileAbs);
        }
        if (!parsed) {
            return Fail(DD_COMBAT_E_GAME, "调用 4F02D0 解析 effect 时出了异常");
        }
        std::string missing;
        for (const auto& name : names) {
            const std::uint32_t hash = GameHash(name);
            if (FindEffect(hash)) {
                effectOwners.Set(hash, owner, name);
            } else {
                missing += (missing.empty() ? "" : "、") + name;
            }
        }
        if (!missing.empty()) {
            return Fail(DD_COMBAT_E_GAME, "解析后找不到 effect：" + missing + "（游戏的解析错误在测试目录的 app.log）");
        }
        Outcome o;
        o.count = static_cast<std::uint32_t>(names.size());
        return o;
    }

    Outcome Execute(std::uint32_t owner, const QueuedOp& op)
    {
        switch (op.kind) {
        case OpKind::BuffAdd: {
            AddRequest r{ op.actor, op.text, op.add, op.tag };
            return DoAdd(owner, r);
        }
        case OpKind::BuffRemove: {
            RemoveRequest r{ op.actor, op.remove, op.text };
            return DoRemove(owner, r);
        }
        case OpKind::BuffDefine:
            return DoDefine(owner, op.def);
        case OpKind::EffectApply: {
            EffectRequest r{ op.text, op.actor, op.actor2, true };
            return DoApply(r);
        }
        case OpKind::EffectDefine:
            return DoDefineEffects(owner, op.text);
        }
        return Fail(DD_COMBAT_E_INVALID, "未知的操作");
    }

    // 立即执行或排队，返回 DDCombatResult
    std::uint32_t Submit(std::uint32_t owner, QueuedOp op, DDCombatOpResult* out)
    {
        const TimingDecision d = DecideTiming(Now(), true);
        if (d.timing == Timing::Reject) {
            return Result(out, op.kind, d.code, std::string("拒绝：") + ResultTextOf(d.code));
        }
        if (d.timing == Timing::Queue) {
            op.owner = owner;
            const OpKind kind = op.kind;
            const std::uint64_t id = queue.Push(std::move(op));
            if (!id) {
                return Result(out, kind, DD_COMBAT_E_QUEUE_FULL, "本插件排队的操作超过 " + std::to_string(kQueueLimit) + " 个");
            }
            Result(out, kind, DD_COMBAT_QUEUED, "");
            if (out && out->size >= sizeof(DDCombatOpResult)) {
                out->op = id;
            }
            return DD_COMBAT_QUEUED;
        }
        const Outcome o = Execute(owner, op);
        ++opsExecuted;
        opsFailed += o.code == DD_COMBAT_OK ? 0 : 1;
        Result(out, op.kind, o.code, o.message);
        if (out && out->size >= sizeof(DDCombatOpResult)) {
            out->guid = o.guid;
            out->count = o.count;
        }
        return o.code;
    }

    static const char* ResultTextOf(std::uint32_t code);

    void Notify(std::uint32_t owner, const QueuedOp& op, const Outcome& o)
    {
        const auto it = handlers.find(owner);
        if (it == handlers.end() || !it->second.first) {
            if (o.code != DD_COMBAT_OK) {
                Log(DD_LOG_INFO, "[" + PluginName(owner) + "] 排队的操作 " + std::to_string(op.id) + " 失败（没有注册操作回调）：" + o.message);
            }
            return;
        }
        DDCombatOpResult result{};
        result.size = sizeof(result);
        result.kind = static_cast<std::uint32_t>(op.kind);
        result.op = op.id;
        result.result = o.code;
        result.guid = o.guid;
        result.count = o.count;
        result.message = o.message.c_str();
        if (deps.invokeOp && !deps.invokeOp(it->second.first, &result, it->second.second, owner)) {
            Log(DD_LOG_ERROR, "[" + PluginName(owner) + "] 操作回调崩溃（操作 " + std::to_string(op.id) + "）");
        }
    }

    void DrainQueue()
    {
        if (queue.Size() == 0) {
            return;
        }
        auto ops = queue.TakeAll();
        for (const auto& op : ops) {
            Outcome o;
            const TimingDecision d = DecideTiming(Now(), true);
            if (d.timing != Timing::Execute) {
                // 帧边界上只可能是不在存档、加载中、PvP：丢弃并通知
                o = Fail(d.code == DD_COMBAT_QUEUED ? DD_COMBAT_E_CANCELLED : d.code,
                    std::string("排队的操作没有执行：") + ResultTextOf(d.code));
            } else {
                o = Execute(op.owner, op);
                ++opsExecuted;
            }
            opsFailed += o.code == DD_COMBAT_OK ? 0 : 1;
            Notify(op.owner, op, o);
        }
    }

    void CancelQueue(const char* why)
    {
        auto ops = queue.TakeAll();
        for (const auto& op : ops) {
            Notify(op.owner, op, Fail(DD_COMBAT_E_CANCELLED, why));
        }
        if (!ops.empty()) {
            Log(DD_LOG_INFO, "combat：丢弃了 " + std::to_string(ops.size()) + " 个排队的操作（" + why + "）");
        }
    }

    // ---- buff.* 派发 ----
    bool Subscribed(events::Topic* topic) const { return topic && deps.events && deps.events->hub().SubscriberCount(topic) > 0; }

    // buff.* 只在存档里派发（P4-0D）：CampaignLoad 之前 load_campaign 线程上 Hero::Load 重新加 quirk、饰品的增删不派发，
    // 插件需要时在 campaign.load 里枚举；CampaignLoad 之后别的加载线程（load_raid、load_town）上的按字段复制、延迟派发。
    // 跟踪器的标志只在主线程写，这里在加载线程上读一个 bool，最多晚一帧
    bool Dispatching(events::Topic* topic) const { return Subscribed(topic) && deps.campaignActive && deps.campaignActive(); }

    struct Fill {
        events::Slot slots[BF_count + 2]{};
        char plugin[DD_COMBAT_TEXT] = {};
        char tag[DD_COMBAT_TEXT] = {};
        EntrySnap snap;
        std::uint32_t kind = 0;
    };

    // 共同字段；main 为真时按标签表补 plugin、tag
    void FillCommon(Fill& f, std::uintptr_t actor, bool main)
    {
        const std::uint32_t id = ActorId(actor, &f.kind);
        if (main) {
            DDActorRef ref{};
            if (ActorRefOf(actor, &ref)) {
                if (const TagEntry* t = tags.Get(ref, f.snap.guid)) {
                    CopyText(f.plugin, PluginName(t->owner));
                    CopyText(f.tag, t->tag);
                }
            }
        }
        const auto set = [&](std::uint32_t index) -> events::Slot& {
            f.slots[index].set = true;
            return f.slots[index];
        };
        set(BF_actor).v.p = main ? reinterpret_cast<void*>(actor) : nullptr;
        set(BF_kind).v.i = f.kind;
        set(BF_actorId).v.i = id;
        set(BF_guid).v.i = f.snap.guid;
        set(BF_id).v.s = f.snap.id;
        set(BF_stat).v.i = f.snap.stat;
        set(BF_amount).v.f = f.snap.amount;
        set(BF_duration).v.i = f.snap.duration;
        set(BF_durationType).v.i = f.snap.durationType;
        set(BF_source).v.i = f.snap.source;
        set(BF_sourceData).v.i = f.snap.sourceData;
        set(BF_rule).v.i = f.snap.rule;
        set(BF_plugin).v.s = f.plugin;
        set(BF_tag).v.s = f.tag;
    }

    void Emit(events::Topic* topic, Fill& f, bool main)
    {
        auto& hub = deps.events->hub();
        if (main) {
            events::Payload payload;
            payload.slots = f.slots;
            payload.originThread = GetCurrentThreadId();
            hub.Dispatch(topic, payload);
        } else {
            hub.EnqueueDeferred(topic, f.slots, GetCurrentThreadId());
        }
    }

    void OnAdded(void* group, std::uint32_t guid)
    {
        if (!Subscribed(buffAdd) && !t_addContext) {
            return;
        }
        const auto actor = reinterpret_cast<std::uintptr_t>(group) - kActorBuffGroup;
        if (!IsActor(actor)) {
            return;  // 不是 actor 的 buff 表
        }
        const bool main = IsMain();
        if (main && t_addContext && t_addContext->actor == actor) {
            tags.Set(t_addContext->ref, guid, t_addContext->owner, *t_addContext->tag);  // 负载里要有 plugin、tag
        }
        if (!Dispatching(buffAdd)) {
            return;
        }
        Fill f;
        const std::uintptr_t entry = FindEntry(actor, guid);
        if (!entry || !ReadEntry(entry, &f.snap)) {
            return;
        }
        FillCommon(f, actor, main);
        Emit(buffAdd, f, main);
    }

    const char* RemoveReason(std::uint32_t guid)
    {
        if (t_pluginHits && std::find(t_pluginHits->begin(), t_pluginHits->end(), guid) != t_pluginHits->end()) {
            return "plugin";
        }
        void* frames[24] = {};
        const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        const char* found = nullptr;
        bool viaSlot14 = false;
        for (USHORT i = 0; i < n && !found; ++i) {
            DWORD64 image = 0;
            const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(frames[i]), &image, nullptr);
            // 游戏函数常被拆成几段，沿链式条目找到主函数
            for (int k = 0; rf && k < 8; ++k) {
                const auto* info = reinterpret_cast<const std::uint8_t*>(image + rf->UnwindInfoAddress);
                if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) {
                    break;
                }
                const std::size_t codes = (static_cast<std::size_t>(info[2]) + 1) & ~static_cast<std::size_t>(1);
                rf = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
            }
            if (!rf) {
                continue;
            }
            const std::uintptr_t begin = static_cast<std::uintptr_t>(image + rf->BeginAddress);
            for (const auto& fn : reasonFns) {
                if (fn.begin == begin) {
                    found = fn.reason;
                    break;
                }
            }
            viaSlot14 = viaSlot14 || begin == slot14Hero || begin == slot14Monster;
        }
        return found ? found : (viaSlot14 ? "predicate" : "other");
    }

    void OnErasing(void* group, void* entry)
    {
        const bool tick = t_upkeep && Dispatching(buffTick);
        if (!Dispatching(buffRemove) && !tick) {
            return;
        }
        const auto actor = reinterpret_cast<std::uintptr_t>(group) - kActorBuffGroup;
        if (!IsActor(actor)) {
            return;
        }
        Fill f;
        if (!ReadEntry(reinterpret_cast<std::uintptr_t>(entry), &f.snap)) {
            return;
        }
        const bool main = IsMain();
        const char* reason = RemoveReason(f.snap.guid);
        // A-N1：47AAD0 里减到 0 被删掉的条目，原函数返回后已经不在表里，出口的对比看不到。删除之前补一次 buff.tick
        // （duration_before 是入口时的值，duration_after 是删除时条目里的值），再派发 buff.remove
        if (tick && t_upkeep->actor == actor && std::strcmp(reason, "expired") == 0) {
            const auto& before = *static_cast<const std::vector<TickBefore>*>(t_upkeep->before);
            const auto it = std::find_if(before.begin(), before.end(), [&](const TickBefore& b) { return b.guid == f.snap.guid; });
            if (it != before.end() && it->duration != f.snap.duration) {
                Fill t;
                t.snap = f.snap;
                FillCommon(t, actor, main);
                t.slots[BF_durationBefore].v.i = it->duration;
                t.slots[BF_durationBefore].set = true;
                t.slots[BF_durationAfter].v.i = f.snap.duration;
                t.slots[BF_durationAfter].set = true;
                Emit(buffTick, t, main);
            }
        }
        if (!Dispatching(buffRemove)) {
            return;
        }
        FillCommon(f, actor, main);
        f.slots[BF_reason].v.s = reason;
        f.slots[BF_reason].set = true;
        Emit(buffRemove, f, main);
    }

    struct TickBefore {
        std::uint32_t guid;
        std::int32_t duration;
    };

    void OnUpkeepDone(std::uintptr_t actor, const std::vector<TickBefore>& before)
    {
        if (!Dispatching(buffTick)) {
            return;
        }
        std::uintptr_t first = 0;
        std::size_t count = 0;
        if (!Table(actor, &first, &count)) {
            return;
        }
        const bool main = IsMain();
        for (std::size_t i = 0; i < count; ++i) {
            Fill f;
            if (!ReadEntry(first + i * kEntrySize, &f.snap)) {
                continue;
            }
            const auto it = std::find_if(before.begin(), before.end(), [&](const TickBefore& b) { return b.guid == f.snap.guid; });
            if (it == before.end() || it->duration == f.snap.duration) {
                continue;
            }
            FillCommon(f, actor, main);
            f.slots[BF_durationBefore].v.i = it->duration;
            f.slots[BF_durationBefore].set = true;
            f.slots[BF_durationAfter].v.i = f.snap.duration;
            f.slots[BF_durationAfter].set = true;
            Emit(buffTick, f, main);
        }
    }

    // ---- 规则 ----
    // 命中注册表的判定：返回 true 表示由回调决定（*result 是取反后的结果），false 表示交给游戏
    bool EvaluateRule(const RuleEntry& entry, void* self, const std::uint8_t* inst, void* target, const void* skill, int ruleType,
        bool* result)
    {
        if (!IsMain()) {
            ruleSkippedOffMain.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (entry.disabled.load(std::memory_order_relaxed) || ActivePvP()) {
            ++ruleStats.skipped;
            return false;
        }
        void* subject = ruleType == 42 ? self : target;
        if (!subject) {
            ++ruleStats.skipped;
            return false;  // actorStatus 没有 target：游戏的前置检查直接返回假
        }
        char buffId[DD_COMBAT_TEXT];
        CopyText(buffId, inst + kInstId);
        bool falseRule = inst[kInstFalseRule] != 0;
        float ruleFloat = 0;
        std::memcpy(&ruleFloat, inst + kInstRuleFloat, 4);
        DDRuleContext context{};
        context.size = sizeof(context);
        context.rule = entry.name.c_str();
        context.actor = self;
        context.target = target;
        context.subject = subject;
        context.skill = skill;
        context.rule_type = ruleType;
        context.is_false_rule = falseRule;
        context.rule_float = ruleFloat;
        context.buff_id = buffId;
        std::uint32_t raw = DD_RULE_ORIGINAL;
        const double t0 = QpcUs();
        const bool outer = t_inRule;
        t_inRule = true;
        const bool ok = deps.invokeRule && deps.invokeRule(entry.fn, &context, entry.user, &raw);
        t_inRule = outer;
        const double us = QpcUs() - t0;
        if (!ok) {
            entry.disabled = true;
            ++ruleStats.errors;
            Log(DD_LOG_ERROR, "规则回调崩溃：" + entry.name + "（插件 " + PluginName(entry.owner) + "），已停用，之后按没有注册处理");
            return false;
        }
        ruleStats.Add(us);
        // DD_RULE_ORIGINAL（回调出错、超时、停用中）：交还原函数，和没有注册时一样
        if (!InterpretRule(raw, falseRule, result)) {
            ++ruleStats.passed;
            return false;
        }
        return true;
    }

    void InitReasons()
    {
        const auto* a = deps.addr;
        const auto add = [&](const void* fn, const char* reason) {
            if (fn) {
                reasonFns.push_back({ reinterpret_cast<std::uintptr_t>(fn), reason });
            }
        };
        add(a->buffDurationUpkeep, "expired");
        add(a->updateBuffsInternal, "refresh");
        add(a->onStartTownVisit, "town_visit");
        add(a->buffBurnDecay, "burn_decay");
        add(a->buffRemoveByGuid, "by_guid");
        add(a->actorResetStats, "reset");
        add(a->effectApply, "effect");
        add(a->inventoryBuffRemove, "item");
        add(a->skillInstantBuffs, "skill_instant");
        add(a->updateTrinketBuffs, "trinket");
        if (a->heroVtable) {
            slot14Hero = reinterpret_cast<std::uintptr_t>(a->heroVtable[kSlotRemoveBuffs]);
        }
        if (a->monsterVtable) {
            slot14Monster = reinterpret_cast<std::uintptr_t>(a->monsterVtable[kSlotRemoveBuffs]);
        }
    }

    void RegisterTopicsAndHooks()
    {
        if (!deps.events) {
            return;
        }
        auto& hub = deps.events->hub();
        buffAdd = hub.AddTopic("buff.add", events::TopicKind::Semantic, BuffFields(0), false, false);
        buffRemove = hub.AddTopic("buff.remove", events::TopicKind::Semantic, BuffFields(1), false, false);
        buffTick = hub.AddTopic("buff.tick", events::TopicKind::Semantic, BuffFields(2), false, false);
        const auto* a = deps.addr;
        const bool vtables = a->heroVtable && a->monsterVtable;
        const auto require = [&](events::Topic* topic, const void* target, const char* sig) {
            if (!vtables) {
                hub.SetAvailability(topic, false, "特征码 HeroVtable/MonsterVtable 缺失");
            } else if (!target) {
                hub.SetAvailability(topic, false, std::string("特征码 ") + sig + " 缺失");
            }
        };
        require(buffAdd, a->buffGroupAdd, "BuffGroupAdd");
        require(buffRemove, a->buffGroupErase, "BuffGroupErase");
        require(buffTick, a->buffDurationUpkeep, "BuffDurationUpkeep");

        const auto addHook = [&](const char* name, std::uint8_t* target, const char* entryId, void* detour, void** orig,
                                 std::vector<events::Topic*> topics, std::function<bool()> extra) {
            if (!target || !vtables) {
                return;
            }
            events::ExternalHook hook;
            hook.name = name;
            hook.isVtable = false;
            hook.target = target;
            hook.entryId = entryId;
            hook.detour = detour;
            hook.orig = orig;
            hook.topics = std::move(topics);
            hook.extraDesired = std::move(extra);
            deps.events->AddHook(std::move(hook));
        };
        addHook("buff.add", a->buffGroupAdd, "BuffGroupAdd", reinterpret_cast<void*>(&GroupAddDetour),
            reinterpret_cast<void**>(&origAdd), { buffAdd }, nullptr);
        addHook("buff.remove", a->buffGroupErase, "BuffGroupErase", reinterpret_cast<void*>(&GroupEraseDetour),
            reinterpret_cast<void**>(&origErase), { buffRemove, buffTick }, nullptr);  // buff.tick 也要：减到 0 被删的条目在这里补派发（A-N1）
        addHook("buff.tick", a->buffDurationUpkeep, "BuffDurationUpkeep", reinterpret_cast<void*>(&UpkeepDetour),
            reinterpret_cast<void**>(&origUpkeep), { buffTick }, nullptr);
        addHook("buff.rule", a->buffRuleActive, "BuffRuleActive", reinterpret_cast<void*>(&RuleDetour),
            reinterpret_cast<void**>(&origRule), {}, [this] { return rules.Count() > 0; });
    }
};

const char* Combat::Impl::ResultTextOf(std::uint32_t code)
{
    switch (code) {
    case DD_COMBAT_OK:
        return "成功";
    case DD_COMBAT_QUEUED:
        return "已排队，下一个帧边界执行";
    case DD_COMBAT_E_INVALID:
        return "参数无效";
    case DD_COMBAT_E_UNAVAILABLE:
        return "依赖的特征码缺失，这项能力不可用";
    case DD_COMBAT_E_THREAD:
        return "不在主线程";
    case DD_COMBAT_E_NO_CAMPAIGN:
        return "不在存档里";
    case DD_COMBAT_E_LOADING:
        return "加载线程正在运行（读档、进地牢、进城）";
    case DD_COMBAT_E_PVP:
        return "PvP 对局中";
    case DD_COMBAT_E_IN_RULE:
        return "规则回调里不能写";
    case DD_COMBAT_E_QUEUE_FULL:
        return "排队的操作太多";
    case DD_COMBAT_E_ACTOR:
        return "actor 句柄解析失败";
    case DD_COMBAT_E_NOT_FOUND:
        return "不存在";
    case DD_COMBAT_E_NO_RAID:
        return "需要 Raid::Current（城镇里没有）";
    case DD_COMBAT_E_EXISTS:
        return "已经存在";
    case DD_COMBAT_E_GAME:
        return "调用游戏函数时出了异常";
    case DD_COMBAT_E_CANCELLED:
        return "排队的操作被丢弃";
    default:
        return "未知";
    }
}

namespace {

void* __fastcall GroupAddDetour(void* group, std::uint32_t guid, const void* inst)
{
    auto* impl = g_impl;
    void* result = impl->origAdd(group, guid, inst);
    impl->OnAdded(group, guid);
    return result;
}

void* __fastcall GroupEraseDetour(void* group, void* out, void* entry)
{
    auto* impl = g_impl;
    impl->OnErasing(group, entry);
    void* result = impl->origErase(group, out, entry);
    return result;
}

std::uint64_t __fastcall UpkeepDetour(void* actor, void* filter, int type)
{
    auto* impl = g_impl;
    const auto a = reinterpret_cast<std::uintptr_t>(actor);
    if (!impl->Dispatching(impl->buffTick) || !impl->IsActor(a)) {
        return impl->origUpkeep(actor, filter, type);
    }
    std::vector<Combat::Impl::TickBefore> before;
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (impl->Table(a, &first, &count)) {
        before.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            before.push_back({ Rd<std::uint32_t>(first + i * kEntrySize), Rd<std::int32_t>(first + i * kEntrySize + kEntryInstance + kInstDuration) });
        }
    }
    UpkeepContext context{ a, &before };
    UpkeepContext* outer = t_upkeep;
    t_upkeep = &context;
    const std::uint64_t result = impl->origUpkeep(actor, filter, type);
    t_upkeep = outer;
    impl->OnUpkeepDone(a, before);
    return result;
}

// 规则判定非常频繁（探针 0A-6：约 22 分钟 114 万次），先看规则类型，只有 13/42 且哈希命中注册表的才进入回调
bool __fastcall RuleDetour(void* self, const void* inst, const void* p3, void* target, const void* skill)
{
    auto* impl = g_impl;
    const auto* p = static_cast<const std::uint8_t*>(inst);
    if (p) {
        std::int32_t ruleType = 0;
        std::memcpy(&ruleType, p + kInstRule, 4);
        if (ruleType == 13 || ruleType == 42) {
            const RuleSnapshot* snapshot = impl->rules.Snapshot();
            if (snapshot && !snapshot->entries.empty()) {
                std::uint32_t hash = 0;
                std::memcpy(&hash, p + kInstRuleHash, 4);
                if (const RuleEntry* entry = snapshot->Find(hash)) {
                    bool result = false;
                    if (impl->EvaluateRule(*entry, self, p, target, skill, ruleType, &result)) {
                        return result;
                    }
                }
            }
        }
    }
    return impl->origRule(self, inst, p3, target, skill);
}

}  // namespace

// ============================================================================

Combat::Combat(CombatDeps deps) : impl_(std::make_unique<Impl>())
{
    impl_->deps = std::move(deps);
    g_impl = impl_.get();
    impl_->InitReasons();
    impl_->RegisterTopicsAndHooks();
}

Combat::~Combat()
{
    if (g_impl == impl_.get()) {
        g_impl = nullptr;
    }
}

bool Combat::SetOpHandler(std::uint32_t owner, DDCombatOpFn fn, void* user)
{
    if (!impl_->IsMain()) {
        return false;
    }
    if (fn) {
        impl_->handlers[owner] = { fn, user };
    } else {
        impl_->handlers.erase(owner);
    }
    return true;
}

bool Combat::SafePoint()
{
    return IsSafePoint(impl_->Now());
}

bool Combat::ReadReady()
{
    return impl_->ReadSafe();
}

bool Combat::BuffList(const DDActorRef& ref, std::uint32_t* guids, std::uint32_t capacity, std::uint32_t* count)
{
    auto& m = *impl_;
    if (!m.ReadSafe()) {
        return false;
    }
    const std::uintptr_t actor = m.Resolve(ref);
    std::vector<std::uint32_t> list;
    if (!actor || !m.Guids(actor, &list)) {
        return false;
    }
    for (std::uint32_t i = 0; guids && i < capacity && i < list.size(); ++i) {
        guids[i] = list[i];
    }
    if (count) {
        *count = static_cast<std::uint32_t>(list.size());
    }
    return true;
}

bool Combat::BuffGet(const DDActorRef& ref, std::uint32_t guid, DDBuffInfo* out)
{
    auto& m = *impl_;
    if (!m.ReadSafe() || !out || out->size < sizeof(DDBuffInfo)) {
        return false;
    }
    const std::uintptr_t actor = m.Resolve(ref);
    if (!actor) {
        return false;
    }
    const std::uintptr_t entry = m.FindEntry(actor, guid);
    std::uint8_t raw[kEntryInstance + kInstReadSize] = {};
    if (!entry || !ReadBytes(entry, raw, sizeof(raw))) {
        return false;
    }
    const std::uint8_t* inst = raw + kEntryInstance;
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(DDBuffInfo));
    out->size = size;
    out->guid = guid;
    CopyText(out->id, inst + kInstId);
    std::memcpy(&out->stat_type, inst + kInstStat, 4);
    CopyText(out->stat_sub_type, inst + kInstSubType);
    std::memcpy(&out->amount, inst + kInstAmount, 4);
    std::memcpy(&out->duration, inst + kInstDuration, 4);
    std::memcpy(&out->duration_type, inst + kInstDurationType, 4);
    std::memcpy(&out->source, inst + kInstSource, 4);
    out->source_flags = SourceFlags(out->source);
    std::memcpy(&out->sourcedata, inst + kInstSourceData, 4);
    std::memcpy(&out->rule_type, inst + kInstRule, 4);
    out->is_false_rule = inst[kInstFalseRule] != 0;
    out->remove_if_not_active = inst[kInstRemoveInactive] != 0;
    out->remove_on_battle_complete = inst[kInstRemoveBattle] != 0;
    std::memcpy(&out->rule_float, inst + kInstRuleFloat, 4);
    CopyText(out->rule_string, inst + kInstRuleString);
    m.PruneTags(ref, actor);
    if (const TagEntry* t = m.tags.Get(ref, guid)) {
        out->by_plugin = true;
        CopyText(out->plugin, m.PluginName(t->owner));
        CopyText(out->tag, t->tag);
    }
    return true;
}

bool Combat::BuffDefGet(const std::string& id, DDBuffDef* out)
{
    auto& m = *impl_;
    if (!m.ReadSafe() || !out || out->size < sizeof(DDBuffDef)) {
        return false;
    }
    const std::uintptr_t def = m.FindDefinition(GameHash(id));
    std::uint8_t inst[kInstReadSize] = {};
    if (!def || !ReadBytes(def, inst, sizeof(inst))) {
        return false;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(DDBuffDef));
    out->size = size;
    CopyText(out->id, inst + kInstId);
    std::memcpy(&out->stat_type, inst + kInstStat, 4);
    CopyText(out->stat_sub_type, inst + kInstSubType);
    std::memcpy(&out->amount, inst + kInstAmount, 4);
    std::memcpy(&out->duration, inst + kInstDuration, 4);
    std::memcpy(&out->duration_type, inst + kInstDurationType, 4);
    std::memcpy(&out->source, inst + kInstSource, 4);
    std::memcpy(&out->sourcedata, inst + kInstSourceData, 4);
    std::memcpy(&out->rule_type, inst + kInstRule, 4);
    out->is_false_rule = inst[kInstFalseRule] != 0;
    out->remove_if_not_active = inst[kInstRemoveInactive] != 0;
    out->remove_on_battle_complete = inst[kInstRemoveBattle] != 0;
    std::memcpy(&out->rule_float, inst + kInstRuleFloat, 4);
    CopyText(out->rule_string, inst + kInstRuleString);
    return true;
}

std::uint32_t Combat::BuffAdd(std::uint32_t owner, AddRequest r, DDCombatOpResult* out)
{
    auto& m = *impl_;
    std::string why;
    if (!ValidText(r.id, false) || !ValidText(r.tag, true) || !ValidateAddOptions(r.options, &why)) {
        return m.Result(out, OpKind::BuffAdd, DD_COMBAT_E_INVALID, why.empty() ? "id 或 tag 无效（最多 63 字节，不含控制字符、引号）" : why);
    }
    QueuedOp op;
    op.kind = OpKind::BuffAdd;
    op.actor = r.actor;
    op.text = std::move(r.id);
    op.tag = std::move(r.tag);
    op.add = r.options;
    op.add.tag = nullptr;
    return m.Submit(owner, std::move(op), out);
}

std::uint32_t Combat::BuffRemove(std::uint32_t owner, RemoveRequest r, DDCombatOpResult* out)
{
    auto& m = *impl_;
    std::string why;
    if (!ValidateRemoveSpec(r.spec, &why)) {
        return m.Result(out, OpKind::BuffRemove, DD_COMBAT_E_INVALID, why);
    }
    if ((r.spec.match == DD_BUFF_BY_TAG || r.spec.match == DD_BUFF_BY_ID) && !ValidText(r.text, false)) {
        return m.Result(out, OpKind::BuffRemove, DD_COMBAT_E_INVALID, "text 为空或无效（最多 63 字节）");
    }
    QueuedOp op;
    op.kind = OpKind::BuffRemove;
    op.actor = r.actor;
    op.remove = r.spec;
    op.remove.text = nullptr;
    op.text = std::move(r.text);
    return m.Submit(owner, std::move(op), out);
}

std::uint32_t Combat::BuffDefine(std::uint32_t owner, const DDBuffDef& def, DDCombatOpResult* out)
{
    auto& m = *impl_;
    std::string why;
    if (!ValidateBuffDef(def, &why)) {
        return m.Result(out, OpKind::BuffDefine, DD_COMBAT_E_INVALID, why);
    }
    QueuedOp op;
    op.kind = OpKind::BuffDefine;
    op.def = def;
    return m.Submit(owner, std::move(op), out);
}

bool Combat::EffectGet(const std::string& name, DDEffectInfo* out)
{
    auto& m = *impl_;
    if (!m.ReadSafe() || !out || out->size < sizeof(DDEffectInfo)) {
        return false;
    }
    const std::uint32_t hash = GameHash(name);
    const std::uintptr_t effect = m.FindEffect(hash);
    std::vector<std::uint8_t> raw(kEffectReadSize);
    if (!effect || !ReadBytes(effect, raw.data(), raw.size())) {
        return false;
    }
    const std::uint8_t* p = raw.data();
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(DDEffectInfo));
    out->size = size;
    CopyText(out->name, p);
    const auto i32 = [&](std::size_t o) {
        std::int32_t v;
        std::memcpy(&v, p + o, 4);
        return v;
    };
    const auto f32 = [&](std::size_t o) {
        float v;
        std::memcpy(&v, p + o, 4);
        return v;
    };
    out->target = i32(kEffectTarget);
    out->chance = f32(kEffectChance);
    out->stress = f32(kEffectStress);
    out->stun = f32(kEffectStun);
    out->heal = f32(kEffectHeal);
    out->heal_percent = f32(kEffectHealPercent);
    out->combat_stat_buff = i32(kEffectCombatStatBuff);
    out->dot_poison = i32(kEffectDotPoison);
    out->dot_bleed = i32(kEffectDotBleed);
    out->dot_burn = i32(kEffectDotBurn);
    out->dot_stress = i32(kEffectDotStress);
    out->duration = i32(kEffectDuration);
    out->buff_amount = f32(kEffectBuffAmount);
    out->buff_duration_type = i32(kEffectBuffDurationType);
    std::uintptr_t ids[2] = {};
    std::memcpy(ids, p + kEffectBuffIds, sizeof(ids));
    out->buff_id_count = (ids[0] && ids[1] >= ids[0]) ? static_cast<std::uint32_t>((ids[1] - ids[0]) / 4) : 0;
    out->on_hit = p[kEffectOnHit] != 0;
    out->on_miss = p[kEffectOnMiss] != 0;
    out->apply_once = p[kEffectApplyOnce] != 0;
    out->queue = p[kEffectQueue] != 0;
    out->swap_source_and_target = p[kEffectSwap] != 0;
    out->runtime = m.effectOwners.OwnerOf(hash) != 0;
    return true;
}

std::uint32_t Combat::EffectApply(std::uint32_t owner, EffectRequest r, DDCombatOpResult* out)
{
    auto& m = *impl_;
    if (!ValidText(r.name, false)) {
        return m.Result(out, OpKind::EffectApply, DD_COMBAT_E_INVALID, "effect 名字无效");
    }
    QueuedOp op;
    op.kind = OpKind::EffectApply;
    op.actor = r.performer;
    op.actor2 = r.hasTarget ? r.target : r.performer;
    op.text = std::move(r.name);
    return m.Submit(owner, std::move(op), out);
}

std::uint32_t Combat::EffectDefine(std::uint32_t owner, std::string text, DDCombatOpResult* out)
{
    auto& m = *impl_;
    if (text.empty() || text.size() > kEffectTextMax) {
        return m.Result(out, OpKind::EffectDefine, DD_COMBAT_E_INVALID, "effect 文本为空或超过 64 KB");
    }
    std::vector<std::string> names;
    std::string why;
    if (!ExtractEffectNames(text, &names, &why)) {
        return m.Result(out, OpKind::EffectDefine, DD_COMBAT_E_INVALID, why);
    }
    QueuedOp op;
    op.kind = OpKind::EffectDefine;
    op.text = std::move(text);
    return m.Submit(owner, std::move(op), out);
}

DDRuleId Combat::RuleRegister(std::uint32_t owner, const std::string& name, DDRuleFn fn, void* user)
{
    auto& m = *impl_;
    if (!m.IsMain() || t_inRule) {
        return 0;
    }
    std::string why;
    const DDRuleId id = m.rules.Register(owner, name, fn, user, &why);
    if (!id) {
        m.Log(DD_LOG_WARN, "[" + m.PluginName(owner) + "] 规则注册失败：" + name + "：" + why);
        return 0;
    }
    if (!m.deps.addr->buffRuleActive || !m.deps.events) {
        m.Log(DD_LOG_WARN, "[" + m.PluginName(owner) + "] 规则 " + name + " 已注册，但特征码 BuffRuleActive 缺失，判定时不会调用");
    }
    if (m.deps.events) {
        m.deps.events->RequestReconcile();
    }
    return id;
}

bool Combat::RuleUnregister(std::uint32_t owner, DDRuleId id)
{
    auto& m = *impl_;
    if (!m.IsMain() || t_inRule || !m.rules.Unregister(owner, id)) {
        return false;
    }
    if (m.deps.events) {
        m.deps.events->RequestReconcile();
    }
    return true;
}

bool Combat::RuleStats(DDRuleStats* out)
{
    auto& m = *impl_;
    if (!m.IsMain() || !out || out->size < sizeof(DDRuleStats)) {
        return false;
    }
    out->rules = static_cast<std::uint32_t>(m.rules.Count());
    out->hits = m.ruleStats.hits;
    out->skipped = m.ruleStats.skipped + m.ruleSkippedOffMain.load();
    out->passed = m.ruleStats.passed;
    out->errors = m.ruleStats.errors;
    out->total_us = m.ruleStats.totalUs;
    out->max_call_us = m.ruleStats.maxCallUs;
    out->max_frame_us = (std::max)(m.ruleStats.maxFrameUs, m.ruleStats.frameUs);
    out->max_frame_hits = (std::max)(m.ruleStats.maxFrameHits, m.ruleStats.frameHits);
    return true;
}

namespace {

// 技能指针在执行者职业的哪张表里，在的话返回 true
bool SkillInClass(std::uintptr_t performer, bool hero, std::uintptr_t skill)
{
    if (hero) {
        const auto cls = Rd<std::uintptr_t>(performer + game::actor_layout::kHeroClass);
        std::uintptr_t first = 0, last = 0;
        if (cls && ReadVector(cls + kHeroClassSkills, &first, &last) && (last - first) / 0x18 < 256) {
            for (std::uintptr_t at = first; at < last; at += 0x18) {
                std::uintptr_t a = 0, b = 0;
                if (ReadVector(at, &a, &b) && skill >= a && skill < b && (skill - a) % kHeroSkillSize == 0) {
                    return true;
                }
            }
        }
        std::uintptr_t a = 0, b = 0;
        return cls && ReadVector(cls + kHeroClassMoveSkills, &a, &b) && skill >= a && skill < b && (skill - a) % kHeroSkillSize == 0;
    }
    const auto cls = Rd<std::uintptr_t>(performer + game::actor_layout::kMonsterClass);
    std::uintptr_t a = 0, b = 0;
    return cls && ReadVector(cls + kMonsterClassSkills, &a, &b) && skill >= a && skill < b && (skill - a) % kMonsterSkillSize == 0;
}

// 在职业表里按 id、等级找技能，找不到返回 0
std::uintptr_t FindSkill(std::uintptr_t performer, bool hero, const std::string& id, std::int32_t level)
{
    const auto match = [&](std::uintptr_t skill, bool checkLevel) {
        std::uint8_t text[DD_COMBAT_TEXT] = {};
        if (!ReadBytes(skill + kSkillId, text, sizeof(text))) {
            return false;
        }
        return FixedString(text, DD_COMBAT_TEXT) == id && (!checkLevel || Rd<std::int32_t>(skill + kSkillLevel) == level);
    };
    if (hero) {
        const auto cls = Rd<std::uintptr_t>(performer + game::actor_layout::kHeroClass);
        std::uintptr_t first = 0, last = 0;
        if (cls && ReadVector(cls + kHeroClassSkills, &first, &last) && (last - first) / 0x18 < 256) {
            for (std::uintptr_t at = first; at < last; at += 0x18) {
                std::uintptr_t a = 0, b = 0;
                if (!ReadVector(at, &a, &b) || (b - a) / kHeroSkillSize > 64) {
                    continue;
                }
                for (std::uintptr_t s = a; s + kHeroSkillSize <= b; s += kHeroSkillSize) {
                    if (match(s, true)) {
                        return s;
                    }
                }
            }
        }
        std::uintptr_t a = 0, b = 0;
        if (cls && ReadVector(cls + kHeroClassMoveSkills, &a, &b) && (b - a) / kHeroSkillSize <= 64) {
            for (std::uintptr_t s = a; s + kHeroSkillSize <= b; s += kHeroSkillSize) {
                if (match(s, true)) {
                    return s;
                }
            }
        }
        return 0;
    }
    const auto cls = Rd<std::uintptr_t>(performer + game::actor_layout::kMonsterClass);
    std::uintptr_t a = 0, b = 0;
    if (cls && ReadVector(cls + kMonsterClassSkills, &a, &b) && (b - a) / kMonsterSkillSize <= 256) {
        for (std::uintptr_t s = a; s + kMonsterSkillSize <= b; s += kMonsterSkillSize) {
            if (match(s, false)) {
                return s;
            }
        }
    }
    return 0;
}

}  // namespace

bool Combat::SkillFromPointer(const void* performer, const void* skill, DDSkillRef* out)
{
    auto& m = *impl_;
    if (!m.IsMain() || !performer || !skill || !out || out->size < sizeof(DDSkillRef)) {
        return false;
    }
    const auto p = reinterpret_cast<std::uintptr_t>(performer);
    const auto s = reinterpret_cast<std::uintptr_t>(skill);
    DDActorRef ref{};
    if (!m.ActorRefOf(p, &ref)) {
        return false;
    }
    const bool hero = ref.kind == DD_ACTOR_HERO;
    if (!SkillInClass(p, hero, s)) {
        return false;
    }
    std::uint8_t text[DD_COMBAT_TEXT] = {};
    if (!ReadBytes(s + kSkillId, text, sizeof(text))) {
        return false;
    }
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(DDSkillRef));
    out->size = size;
    out->actor = ref;
    CopyText(out->id, text);
    out->level = hero ? Rd<std::int32_t>(s + kSkillLevel) : 0;
    return true;
}

bool Combat::SkillInfo(const DDSkillRef& ref, DDSkillInfo* out)
{
    auto& m = *impl_;
    if (!m.IsMain() || !out || out->size < sizeof(DDSkillInfo)) {
        return false;
    }
    const std::uintptr_t performer = m.Resolve(ref.actor);
    if (!performer) {
        return false;
    }
    const std::string id(ref.id, strnlen(ref.id, DD_COMBAT_TEXT));
    const std::uintptr_t skill = FindSkill(performer, ref.actor.kind == DD_ACTOR_HERO, id, ref.level);
    if (!skill) {
        return false;
    }
    std::uint8_t text[DD_COMBAT_TEXT] = {};
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(DDSkillInfo));
    out->size = size;
    CopyText(out->id, id);
    out->level = ref.actor.kind == DD_ACTOR_HERO ? Rd<std::int32_t>(skill + kSkillLevel) : 0;
    if (ReadBytes(skill + kSkillType, text, sizeof(text))) {
        CopyText(out->type, text);
    }
    out->friendly = Rd<std::uint8_t>(skill + kSkillFriendly) != 0;
    std::uintptr_t first = 0, last = 0;
    if (ReadVector(skill + kSkillEffects, &first, &last)) {
        const std::size_t n = (std::min)(static_cast<std::size_t>((last - first) / 8), static_cast<std::size_t>(DD_SKILL_MAX_EFFECTS));
        for (std::size_t i = 0; i < n; ++i) {
            const auto effect = Rd<std::uintptr_t>(first + i * 8);
            if (effect && ReadBytes(effect, text, sizeof(text))) {
                CopyText(out->effects[out->effect_count++], text);
            }
        }
    }
    return true;
}

void Combat::OnFrameBoundary()
{
    auto& m = *impl_;
    m.ruleStats.EndFrame();
    // 战斗或探索换了：不再能解析的 actor 的标签清掉（怪物随战斗销毁、英雄被解雇）
    if (m.deps.actorEnv && m.tags.Size() > 0) {
        const game::ActorEnv env = m.deps.actorEnv();
        if (env.battleSerial != m.lastBattleSerial || env.raidSerial != m.lastRaidSerial) {
            m.lastBattleSerial = env.battleSerial;
            m.lastRaidSerial = env.raidSerial;
            const std::size_t removed = m.tags.PruneIf([&](const DDActorRef& ref) { return game::ActorResolve(env, ref) == nullptr; });
            if (removed) {
                m.Log(DD_LOG_INFO, "combat：清掉 " + std::to_string(removed) + " 条不再能解析的 actor 的标签");
            }
        }
    }
    m.DrainQueue();
}

void Combat::OnCampaignLoad()
{
    auto& m = *impl_;
    // Buff 库和 effect map 每次读档重建（探针 0A-10、0A-11），这时表里的名字都是游戏数据
    m.buffOwners.Clear();
    m.effectOwners.Clear();
}

void Combat::OnCampaignUnload()
{
    auto& m = *impl_;
    m.CancelQueue("退出存档");
    m.tags.Clear();
    m.buffOwners.Clear();
    m.effectOwners.Clear();
    if (m.ruleStats.hits || m.ruleStats.errors) {
        char text[256];
        std::snprintf(text, sizeof(text), "规则统计（退出存档时）：命中 %llu 次（交还原函数 %llu 次），跳过 %llu 次，崩溃 %llu 次，累计 %.1f ms，单次最长 %.1f us，单帧峰值 %.1f us / %llu 次",
            static_cast<unsigned long long>(m.ruleStats.hits), static_cast<unsigned long long>(m.ruleStats.passed),
            static_cast<unsigned long long>(m.ruleStats.skipped + m.ruleSkippedOffMain.load()),
            static_cast<unsigned long long>(m.ruleStats.errors), m.ruleStats.totalUs / 1000.0, m.ruleStats.maxCallUs, m.ruleStats.maxFrameUs,
            static_cast<unsigned long long>(m.ruleStats.maxFrameHits));
        m.Log(DD_LOG_INFO, text);
    }
}

void Combat::OnPluginRollback(std::uint32_t owner)
{
    auto& m = *impl_;
    m.queue.DropOwner(owner);
    m.tags.DropOwner(owner);
    if (m.rules.DropOwner(owner) && m.deps.events) {
        m.deps.events->RequestReconcile();
    }
    m.handlers.erase(owner);
}

TagLookup Combat::Impl::GameTagLookup()
{
    TagLookup lookup;
    const game::ActorEnv env = deps.actorEnv ? deps.actorEnv() : game::ActorEnv{};
    lookup.findActor = [env](std::uint32_t kind, std::uint32_t id, DDActorRef* out) { return game::ActorFind(env, kind, id, out); };
    lookup.entry = [this](const DDActorRef& ref, std::uint32_t guid, std::string* id) {
        const std::uintptr_t actor = Resolve(ref);
        const std::uintptr_t item = actor ? FindEntry(actor, guid) : 0;
        EntrySnap snap;
        if (!item || !ReadEntry(item, &snap)) {
            return false;
        }
        *id = snap.id;
        return true;
    };
    lookup.pluginName = [this](std::uint32_t owner) { return PluginName(owner); };
    return lookup;
}

std::vector<TagRecord> Combat::ExportTags()
{
    auto& m = *impl_;
    if (!m.IsMain() || m.tags.Size() == 0) {
        return {};
    }
    return CollectTags(m.tags, m.GameTagLookup());
}

std::size_t Combat::ImportTags(const std::vector<TagRecord>& records, std::uint32_t kinds,
    const std::function<std::uint32_t(const std::string& plugin, bool* found)>& ownerOf, std::vector<std::string>* notes)
{
    auto& m = *impl_;
    if (!m.IsMain() || !m.deps.actorEnv) {
        return 0;
    }
    TagLookup lookup = m.GameTagLookup();
    lookup.ownerOf = ownerOf;
    return RestoreTags(m.tags, records, kinds, lookup, notes);
}

std::string Combat::Summary() const
{
    const auto& m = *impl_;
    return "combat：规则 " + std::to_string(m.rules.Count()) + " 条（hook " +
        (m.deps.events && m.deps.events->HookInstalled("buff.rule") ? "已装" : "未装") + "），标签 " + std::to_string(m.tags.Size()) +
        " 条，排队 " + std::to_string(m.queue.Size()) + " 个，已执行写操作 " + std::to_string(m.opsExecuted) + " 个（失败 " +
        std::to_string(m.opsFailed) + "），运行时定义 buff " + std::to_string(m.buffOwners.Size()) + " 个、effect " +
        std::to_string(m.effectOwners.Size()) + " 个";
}

const char* Combat::ResultText(std::uint32_t code)
{
    return Impl::ResultTextOf(code);
}

}  // namespace ddc::combat

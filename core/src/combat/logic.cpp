#include "logic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace ddc::combat {

std::uint32_t GameHash(std::string_view text)
{
    std::uint32_t h = 0;
    for (const char c : text) {
        h = h * game::layout::kGameHashMul + static_cast<unsigned char>(c);
    }
    return h;
}

// ---- 枚举表 ----
// 顺序就是游戏里的数值，表在 game/layout.inc，tools/dd_update.py verify 从两个 exe 的静态初始化函数里提取名字和它逐项比较
namespace {

using game::layout::IndexOf;
constexpr const auto& kStatNames = game::layout::kEnumStatNames;
constexpr const auto& kSourceNames = game::layout::kEnumSourceNames;  // 游戏里带前缀 bsrc_，这里不带
constexpr const auto& kDurationNames = game::layout::kEnumDurationNames;
constexpr const auto& kRuleNames = game::layout::kEnumRuleNames;

static_assert(std::size(kStatNames) == game::layout::kEnumStat);
static_assert(std::size(kSourceNames) == game::layout::kEnumSource);
static_assert(std::size(kDurationNames) == game::layout::kEnumDuration);
static_assert(std::size(kRuleNames) == game::layout::kEnumRule);
static_assert(game::layout::SRC_S == DD_SOURCE_F_SAVED && game::layout::SRC_T == DD_SOURCE_F_TOWN_CLEAR &&
    game::layout::SRC_Y == DD_SOURCE_F_TRAY);

// stat 的数值按名字取（ValidateSubStat 用），编译期求值
consteval std::int32_t Stat(std::string_view name)
{
    return IndexOf(kStatNames, name);
}

struct Table {
    const char* const* names;
    std::uint32_t count;
};

bool GetTable(std::uint32_t table, Table* out)
{
    switch (table) {
    case DD_ENUM_STAT:
        *out = { kStatNames, static_cast<std::uint32_t>(std::size(kStatNames)) };
        return true;
    case DD_ENUM_SOURCE:
        *out = { kSourceNames, static_cast<std::uint32_t>(std::size(kSourceNames)) };
        return true;
    case DD_ENUM_DURATION:
        *out = { kDurationNames, static_cast<std::uint32_t>(std::size(kDurationNames)) };
        return true;
    case DD_ENUM_RULE:
        *out = { kRuleNames, static_cast<std::uint32_t>(std::size(kRuleNames)) };
        return true;
    default:
        return false;
    }
}

}  // namespace

std::uint32_t EnumCount(std::uint32_t table)
{
    Table t{};
    return GetTable(table, &t) ? t.count : 0;
}

const char* EnumName(std::uint32_t table, std::int32_t value)
{
    Table t{};
    if (!GetTable(table, &t) || value < 0 || static_cast<std::uint32_t>(value) >= t.count) {
        return nullptr;
    }
    return t.names[value];
}

std::int32_t EnumValue(std::uint32_t table, std::string_view name)
{
    Table t{};
    if (!GetTable(table, &t)) {
        return -1;
    }
    if (table == DD_ENUM_SOURCE && name.substr(0, 5) == "bsrc_") {
        name.remove_prefix(5);
    }
    for (std::uint32_t i = 0; i < t.count; ++i) {
        if (name == t.names[i]) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::uint32_t SourceFlags(std::int32_t source)
{
    // 下标是来源的数值（layout.inc 的 SourceFlags）：S +0x44 进存档，T +0x46 回城清，Y +0x48 进 tray
    constexpr const auto& kFlags = game::layout::kSourceFlagsValues;
    static_assert(std::size(kFlags) == std::size(kSourceNames));
    return source >= 0 && static_cast<std::size_t>(source) < std::size(kFlags) ? kFlags[source] : 0;
}

// ---- 参数校验 ----

bool ValidText(std::string_view text, bool allowEmpty)
{
    if (text.empty()) {
        return allowEmpty;
    }
    if (text.size() > kTextMax) {
        return false;
    }
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '"') {
            return false;  // 控制字符和引号会破坏游戏的文本解析，也没有合理用途
        }
    }
    return true;
}

namespace {

std::string_view FixedText(const char (&field)[DD_COMBAT_TEXT])
{
    return std::string_view(field, strnlen(field, DD_COMBAT_TEXT));
}

bool InRange(std::int32_t value, std::uint32_t table)
{
    return value >= 0 && static_cast<std::uint32_t>(value) < EnumCount(table);
}

bool Fail(std::string* why, const char* text)
{
    if (why) {
        *why = text;
    }
    return false;
}

// stat_sub_type 的合法值（P4 审核 C-Y3、A-N4，[静态]）：游戏在属性累加（47AD50）和 buff 提示（7C6100 等）里按哈希查这些表，
// 认不出时有的弹 assert（选错按钮执行 int3）、有的越界写到相邻的累加值、有的只写日志、这条 buff 不起作用
// 表在 game/layout.inc（SubStat*，注释里有 Steam 版的表地址和查找函数）
constexpr const auto& kCombatStatNames = game::layout::kSubStatCombatNames;
constexpr const auto& kResistanceNames = game::layout::kSubStatResistanceNames;
constexpr const auto& kSideEffectNames = game::layout::kSubStatSideEffectNames;
constexpr const auto& kSkillAttributeNames = game::layout::kSubStatSkillAttributeNames;
constexpr const auto& kStressSourceNames = game::layout::kSubStatStressSourceNames;
constexpr const auto& kHealSourceNames = game::layout::kSubStatHealSourceNames;

template <std::size_t N>
bool OneOf(std::string_view text, const char* const (&names)[N])
{
    return std::any_of(std::begin(names), std::end(names), [&](const char* n) { return text == n; });
}

template <std::size_t N>
std::string Join(const char* const (&names)[N])
{
    std::string out;
    for (const char* n : names) {
        out += (out.empty() ? "" : "、") + std::string(n);
    }
    return out;
}

}  // namespace

bool ValidateSubStat(std::int32_t stat, std::string_view sub, std::string* why)
{
    const auto need = [&](bool ok, bool allowEmpty, const std::string& names, const char* consequence) {
        if (ok || (allowEmpty && sub.empty())) {
            return true;
        }
        if (why) {
            *why = std::string("stat ") + (EnumName(DD_ENUM_STAT, stat) ? EnumName(DD_ENUM_STAT, stat) : "?") + " 的 sub_stat " +
                (sub.empty() ? std::string("不能为空") : "\"" + std::string(sub) + "\" 认不出") + "，只能是 " + names + (allowEmpty ? "（或空串）" : "") +
                "（" + consequence + "）";
        }
        return false;
    };
    switch (stat) {
    case Stat("combat_stat_add"):
    case Stat("combat_stat_multiply"):
        return need(OneOf(sub, kCombatStatNames), false, Join(kCombatStatNames), "认不出时游戏把数值加到相邻的属性上，显示提示时弹 assert");
    case Stat("resistance"):
        return need(OneOf(sub, kResistanceNames), false, Join(kResistanceNames), "认不出时游戏弹 assert，再越界写到相邻的累加值");
    case Stat("activity_side_effect_chance"):
        return need(OneOf(sub, kSideEffectNames), false, Join(kSideEffectNames), "认不出时游戏弹 assert，再越界写到相邻的累加值");
    case Stat("disable_combat_skill_attribute"):
        return need(OneOf(sub, kSkillAttributeNames), false, Join(kSkillAttributeNames), "认不出时游戏弹 assert，再越界写到相邻的累加值");
    case Stat("stress_dmg_percent"):
    case Stat("stress_heal_percent"):
    case Stat("stress_heal_received_percent"):
    case Stat("stress_dmg_received_percent"):
        return need(OneOf(sub, kStressSourceNames), true, Join(kStressSourceNames), "认不出时游戏越界写到相邻的累加值");
    case Stat("hp_heal_amount"):
    case Stat("hp_heal_percent"):
    case Stat("hp_heal_received_percent"):
    case Stat("hp_heal_amount_received"):
        return need(OneOf(sub, kHealSourceNames), true, Join(kHealSourceNames), "认不出时游戏只写日志，这条 buff 不起作用");
    default:
        return true;  // 其余 stat 游戏不解析 sub_stat（disable_combat_skill 是技能 id，任意文本）
    }
}

bool ValidateBuffDef(const DDBuffDef& def, std::string* why)
{
    if (strnlen(def.id, DD_COMBAT_TEXT) >= DD_COMBAT_TEXT || strnlen(def.stat_sub_type, DD_COMBAT_TEXT) >= DD_COMBAT_TEXT ||
        strnlen(def.rule_string, DD_COMBAT_TEXT) >= DD_COMBAT_TEXT) {
        return Fail(why, "字符串字段没有以 0 结尾（最多 63 字节）");
    }
    if (!ValidText(FixedText(def.id), false)) {
        return Fail(why, "id 为空或含控制字符、引号");
    }
    if (!ValidText(FixedText(def.stat_sub_type), true) || !ValidText(FixedText(def.rule_string), true)) {
        return Fail(why, "stat_sub_type 或 rule_string 含控制字符、引号");
    }
    if (!InRange(def.stat_type, DD_ENUM_STAT)) {
        return Fail(why, "stat_type 超出 0..101");
    }
    if (!InRange(def.duration_type, DD_ENUM_DURATION)) {
        return Fail(why, "duration_type 超出 0..10");
    }
    if (!InRange(def.source, DD_ENUM_SOURCE)) {
        return Fail(why, "source 超出 0..33");
    }
    if (!InRange(def.rule_type, DD_ENUM_RULE)) {
        return Fail(why, "rule_type 超出 0..54");
    }
    if (def.duration == 0 || def.duration < -1) {
        return Fail(why, "duration 只能是 -1（不计时）或大于 0");
    }
    if (!std::isfinite(def.amount) || !std::isfinite(def.rule_float)) {
        return Fail(why, "amount 或 rule_float 不是有限的数");
    }
    return ValidateSubStat(def.stat_type, FixedText(def.stat_sub_type), why);
}

bool ValidateAddOptions(const DDBuffAddOptions& options, std::string* why)
{
    constexpr std::uint32_t kKnown = DD_BUFF_ADD_DURATION | DD_BUFF_ADD_DURATION_TYPE | DD_BUFF_ADD_SOURCE |
        DD_BUFF_ADD_SOURCEDATA | DD_BUFF_ADD_NO_REFRESH;
    if (options.flags & ~kKnown) {
        return Fail(why, "flags 里有不认识的位");
    }
    if ((options.flags & DD_BUFF_ADD_DURATION) && (options.duration == 0 || options.duration < -1)) {
        return Fail(why, "duration 只能是 -1（不计时）或大于 0");
    }
    if ((options.flags & DD_BUFF_ADD_DURATION_TYPE) && !InRange(options.duration_type, DD_ENUM_DURATION)) {
        return Fail(why, "duration_type 超出 0..10");
    }
    if ((options.flags & DD_BUFF_ADD_SOURCE) && !InRange(options.source, DD_ENUM_SOURCE)) {
        return Fail(why, "source 超出 0..33");
    }
    return true;
}

bool ValidateRemoveSpec(const DDBuffRemoveSpec& spec, std::string* why)
{
    switch (spec.match) {
    case DD_BUFF_BY_GUID:
        return true;
    case DD_BUFF_BY_SOURCE:
        return InRange(spec.source, DD_ENUM_SOURCE) ? true : Fail(why, "source 超出 0..33");
    case DD_BUFF_BY_TAG:
    case DD_BUFF_BY_ID:
        return true;  // 文本由调用方单独校验（指针在服务入口复制）
    default:
        return Fail(why, "match 取值无效");
    }
}

// ---- 写操作的时机 ----

TimingDecision DecideTiming(const TimingState& s, bool needCampaign)
{
    TimingDecision d;
    if (!s.mainThread) {
        d.code = DD_COMBAT_E_THREAD;
        return d;
    }
    if (s.inRule) {
        d.code = DD_COMBAT_E_IN_RULE;
        return d;
    }
    if (needCampaign && !s.campaignActive) {
        d.code = DD_COMBAT_E_NO_CAMPAIGN;
        return d;
    }
    if (s.loading) {
        d.code = DD_COMBAT_E_LOADING;
        return d;
    }
    if (s.pvp) {
        d.code = DD_COMBAT_E_PVP;
        return d;
    }
    const bool safe = s.inFrameBody && s.dispatchDepth == 0 && s.gameCallDepth == 0;
    d.timing = safe ? Timing::Execute : Timing::Queue;
    d.code = safe ? DD_COMBAT_OK : DD_COMBAT_QUEUED;
    return d;
}

bool IsSafePoint(const TimingState& s)
{
    return DecideTiming(s, true).timing == Timing::Execute;
}

// ---- 操作队列 ----

std::uint64_t OpQueue::Push(QueuedOp op)
{
    auto& count = perOwner_[op.owner];
    if (count >= limit_) {
        return 0;
    }
    ++count;
    op.id = nextId_++;
    const std::uint64_t id = op.id;
    items_.push_back(std::move(op));
    return id;
}

std::vector<QueuedOp> OpQueue::TakeAll()
{
    std::vector<QueuedOp> out(std::make_move_iterator(items_.begin()), std::make_move_iterator(items_.end()));
    items_.clear();
    perOwner_.clear();
    return out;
}

std::size_t OpQueue::DropOwner(std::uint32_t owner)
{
    const auto before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const QueuedOp& op) { return op.owner == owner; }),
        items_.end());
    perOwner_.erase(owner);
    return before - items_.size();
}

std::size_t OpQueue::OwnerCount(std::uint32_t owner) const
{
    const auto it = perOwner_.find(owner);
    return it == perOwner_.end() ? 0 : it->second;
}

// ---- 标签表 ----

bool TagTable::Key::operator<(const Key& o) const
{
    const std::uint32_t a[] = { kind, id, campaign, raid, battle, roster, guid };
    const std::uint32_t b[] = { o.kind, o.id, o.campaign, o.raid, o.battle, o.roster, o.guid };
    return std::lexicographical_compare(std::begin(a), std::end(a), std::begin(b), std::end(b));
}

TagTable::Key TagTable::MakeKey(const DDActorRef& a, std::uint32_t guid)
{
    return { a.kind, a.id, a.campaign_serial, a.raid_serial, a.battle_serial, a.roster, guid };
}

bool TagTable::SameActor(const Key& k, const DDActorRef& a)
{
    return k.kind == a.kind && k.id == a.id && k.campaign == a.campaign_serial && k.raid == a.raid_serial &&
        k.battle == a.battle_serial && k.roster == a.roster;
}

DDActorRef TagTable::ToRef(const Key& k)
{
    DDActorRef ref{};
    ref.size = sizeof(DDActorRef);
    ref.kind = k.kind;
    ref.id = k.id;
    ref.campaign_serial = k.campaign;
    ref.raid_serial = k.raid;
    ref.battle_serial = k.battle;
    ref.roster = k.roster;
    return ref;
}

void TagTable::Set(const DDActorRef& actor, std::uint32_t guid, std::uint32_t owner, std::string tag)
{
    map_[MakeKey(actor, guid)] = TagEntry{ owner, std::move(tag) };
}

const TagEntry* TagTable::Get(const DDActorRef& actor, std::uint32_t guid) const
{
    const auto it = map_.find(MakeKey(actor, guid));
    return it == map_.end() ? nullptr : &it->second;
}

bool TagTable::Erase(const DDActorRef& actor, std::uint32_t guid)
{
    return map_.erase(MakeKey(actor, guid)) > 0;
}

std::vector<std::uint32_t> TagTable::Find(const DDActorRef& actor, std::uint32_t owner, std::string_view tag) const
{
    std::vector<std::uint32_t> out;
    const auto begin = map_.lower_bound(MakeKey(actor, 0));
    for (auto it = begin; it != map_.end() && SameActor(it->first, actor); ++it) {
        if (it->second.owner == owner && it->second.tag == tag) {
            out.push_back(it->first.guid);
        }
    }
    return out;
}

std::size_t TagTable::PruneActor(const DDActorRef& actor, const std::vector<std::uint32_t>& live)
{
    std::size_t removed = 0;
    auto it = map_.lower_bound(MakeKey(actor, 0));
    while (it != map_.end() && SameActor(it->first, actor)) {
        if (!std::binary_search(live.begin(), live.end(), it->first.guid)) {
            it = map_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t TagTable::PruneIf(const std::function<bool(const DDActorRef&)>& gone)
{
    std::size_t removed = 0;
    auto it = map_.begin();
    while (it != map_.end()) {
        // 同一个 actor 的记录相邻，整段一起判断
        const DDActorRef ref = ToRef(it->first);
        auto end = it;
        while (end != map_.end() && SameActor(end->first, ref)) {
            ++end;
        }
        if (gone(ref)) {
            removed += static_cast<std::size_t>(std::distance(it, end));
            it = map_.erase(it, end);
        } else {
            it = end;
        }
    }
    return removed;
}

std::size_t TagTable::DropOwner(std::uint32_t owner)
{
    return std::erase_if(map_, [&](const auto& item) { return item.second.owner == owner; });
}

void TagTable::ForEach(const std::function<void(const DDActorRef& actor, std::uint32_t guid, const TagEntry& entry)>& fn) const
{
    for (const auto& [key, entry] : map_) {
        fn(ToRef(key), key.guid, entry);
    }
}

// ---- 标签表随存档保存 ----

std::vector<TagRecord> CollectTags(const TagTable& tags, const TagLookup& lookup)
{
    std::vector<TagRecord> out;
    tags.ForEach([&](const DDActorRef& ref, std::uint32_t guid, const TagEntry& entry) {
        std::string id;
        if (!lookup.entry || !lookup.entry(ref, guid, &id)) {
            return;  // actor 不再能解析或条目不在了：不保存
        }
        out.push_back(TagRecord{ ref.kind, ref.id, ref.roster, guid, std::move(id), lookup.pluginName ? lookup.pluginName(entry.owner) : "?", entry.tag });
    });
    return out;
}

std::size_t RestoreTags(TagTable& tags, const std::vector<TagRecord>& records, std::uint32_t kinds, const TagLookup& lookup,
    std::vector<std::string>* notes)
{
    std::size_t restored = 0;
    const auto note = [&](const TagRecord& r, const std::string& why) {
        if (notes) {
            notes->push_back(std::string(r.kind == DD_ACTOR_HERO ? "英雄 " : "怪物 ") + std::to_string(r.id) + " guid " + std::to_string(r.guid) + "（" +
                r.buffId + "，插件 " + r.plugin + "，标签 " + r.tag + "）：" + why);
        }
    };
    for (const auto& r : records) {
        if (r.kind >= 32 || !(kinds & (1u << r.kind))) {
            continue;
        }
        bool found = false;
        const std::uint32_t owner = lookup.ownerOf ? lookup.ownerOf(r.plugin, &found) : 0;
        if (!found) {
            note(r, "插件没有加载");
            continue;
        }
        if (r.kind == DD_ACTOR_HERO && r.roster != 0) {
            note(r, "备用名册上的英雄不恢复");
            continue;
        }
        DDActorRef ref{};
        ref.size = sizeof(ref);
        if (!lookup.findActor || !lookup.findActor(r.kind, r.id, &ref)) {
            note(r, "actor 找不到");
            continue;
        }
        std::string id;
        if (!lookup.entry || !lookup.entry(ref, r.guid, &id)) {
            note(r, "条目不在");
            continue;
        }
        if (id != r.buffId) {
            note(r, "条目的 id 对不上（现在是 " + id + "）");
            continue;
        }
        tags.Set(ref, r.guid, owner, r.tag);
        ++restored;
    }
    return restored;
}

// ---- 规则注册表 ----

const RuleEntry* RuleSnapshot::Find(std::uint32_t hash) const
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), hash,
        [](const std::shared_ptr<RuleEntry>& e, std::uint32_t h) { return e->hash < h; });
    return it != entries.end() && (*it)->hash == hash ? it->get() : nullptr;
}

RuleRegistry::RuleRegistry()
{
    Publish();
}

DDRuleId RuleRegistry::Register(std::uint32_t owner, std::string_view name, DDRuleFn fn, void* user, std::string* why)
{
    if (!fn) {
        Fail(why, "回调为空");
        return 0;
    }
    if (!ValidText(name, false)) {
        Fail(why, "规则名为空、超过 63 字节或含控制字符、引号");
        return 0;
    }
    const std::uint32_t hash = GameHash(name);
    // 479CD0 的规则 13/42 在 479F41 起按这些内置状态名的哈希判断（[静态]，A-N3）。注册同名（或同哈希）的规则后 hook 先命中，
    // 原版数据里所有 actorStatus / is_actor_status 的这类 buff 都会改由插件决定，所以拒绝
    constexpr const auto& kBuiltinStatus = game::layout::kBuiltinStatusNames;
    for (const char* builtin : kBuiltinStatus) {
        if (GameHash(builtin) == hash) {
            if (why) {
                *why = std::string("规则名和游戏内置的状态 ") + builtin + " 相同（或哈希相同），注册后原版的 actorStatus / is_actor_status 判定会被接管";
            }
            return 0;
        }
    }
    for (const auto& e : entries_) {
        if (e->hash == hash) {
            if (why) {
                *why = e->name == name ? "规则名已经注册" : "规则名和已注册的 " + e->name + " 哈希相同";
            }
            return 0;
        }
    }
    auto entry = std::make_shared<RuleEntry>();
    entry->id = nextId_++;
    entry->owner = owner;
    entry->hash = hash;
    entry->name = std::string(name);
    entry->fn = fn;
    entry->user = user;
    entries_.push_back(entry);
    Publish();
    return entry->id;
}

bool RuleRegistry::Unregister(std::uint32_t owner, DDRuleId id)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [&](const std::shared_ptr<RuleEntry>& e) { return e->id == id && e->owner == owner; });
    if (it == entries_.end()) {
        return false;
    }
    (*it)->disabled = true;
    entries_.erase(it);
    Publish();
    return true;
}

std::size_t RuleRegistry::DropOwner(std::uint32_t owner)
{
    const auto removed = std::erase_if(entries_, [&](const std::shared_ptr<RuleEntry>& e) {
        if (e->owner == owner) {
            e->disabled = true;
            return true;
        }
        return false;
    });
    if (removed) {
        Publish();
    }
    return removed;
}

std::size_t RuleRegistry::Count() const
{
    return entries_.size();
}

void RuleRegistry::Publish()
{
    auto snapshot = std::make_unique<RuleSnapshot>();
    snapshot->entries = entries_;
    std::sort(snapshot->entries.begin(), snapshot->entries.end(),
        [](const std::shared_ptr<RuleEntry>& a, const std::shared_ptr<RuleEntry>& b) { return a->hash < b->hash; });
    current_.store(snapshot.get(), std::memory_order_release);
    retired_.push_back(std::move(snapshot));  // 旧快照可能还有别的线程在读，不释放
}

bool InterpretRule(std::uint32_t raw, bool falseRule, bool* result)
{
    if (raw != DD_RULE_TRUE && raw != DD_RULE_FALSE) {
        return false;  // DD_RULE_ORIGINAL 和不认识的值：交还原函数
    }
    const bool holds = raw == DD_RULE_TRUE;
    *result = falseRule ? !holds : holds;
    return true;
}

void RuleStatsAccum::Add(double us)
{
    ++hits;
    ++frameHits;
    totalUs += us;
    frameUs += us;
    maxCallUs = (std::max)(maxCallUs, us);
}

void RuleStatsAccum::EndFrame()
{
    maxFrameUs = (std::max)(maxFrameUs, frameUs);
    maxFrameHits = (std::max)(maxFrameHits, frameHits);
    frameUs = 0.0;
    frameHits = 0;
}

// ---- effect 文本 ----
// 照游戏的解析器逐步复刻（Steam 版 RVA，[静态]）：
// - 2901B0 读文件后原地去掉 // 注释（连同结尾的换行）和 /* */ 注释，引号里的也算；
// - 2904D0 逐块读：跳过空白和 # 注释，读 [A-Za-z0-9_]* 后必须是冒号，块延伸到下一个冒号（# 到行尾之间的不算）
//   前面那个标识符的前一个字符为止，再用 strncpy_s(reader, 0x400, 块, 块长) 复制（块长 >= 0x400 时 CRT 的无效参数处理直接终止进程）；
// - 4F02D0 只处理以 "effect:" 开头的块，36F020 取字段：strstr 找键的最后一次出现，跳过空白，带引号读到下一个引号
//   （没有就到块尾），否则读到空白，最多 cap-1 字节；
// - 4F02D0 读 .kill_enemy_types、.summon_monsters、.summon_ranks（各 4 个）和 .buff_ids、.set_monster_class_ids（各 8 个）
//   的列表时，某一项以块的最后一个字符（一个引号）开头会算出 0 的缓冲大小交给 strncpy_s，同样终止进程。
// isspace、isalnum 按游戏没有调 setlocale 的 "C" 区域设置；字节 >= 0x80 落在这两个判断上的位置结果依赖区域设置，一律拒绝。

namespace {

bool CSpace(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

bool CIdent(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool High(unsigned char c)
{
    return c >= 0x80;
}

unsigned char At(std::string_view s, std::size_t i)
{
    return i < s.size() ? static_cast<unsigned char>(s[i]) : 0;  // 越过结尾读到的是 0，和游戏的缓冲一样
}

std::string BlockLabel(std::size_t index)
{
    return "第 " + std::to_string(index + 1) + " 块";
}

// 36F020 取字段：返回 false 表示块里没有这个键。value 是完整的值（不截断），locale 为真表示值的边界依赖区域设置
bool GameField(std::string_view block, std::string_view key, std::string* value, bool* locale)
{
    const std::size_t last = block.rfind(key);
    *locale = false;
    if (last == std::string_view::npos) {
        return false;
    }
    std::size_t i = last + key.size();
    while (CSpace(At(block, i))) {
        ++i;
    }
    if (High(At(block, i))) {
        *locale = true;
    }
    std::size_t end = i;
    if (At(block, i) == '"') {
        ++i;
        end = block.find('"', i);
        if (end == std::string_view::npos) {
            end = block.size();
        }
    } else {
        while (end < block.size() && !CSpace(At(block, end))) {
            *locale = *locale || High(At(block, end));
            ++end;
        }
    }
    *value = std::string(block.substr(i, end - i));
    return true;
}

// 4F02D0 的列表读取：从键的最后一次出现开始读 count 项，某一项以块的最后一个字符（引号）开头时游戏会崩
bool ListCrashes(std::string_view block, std::string_view key, int count, bool* locale)
{
    const std::size_t last = block.rfind(key);
    *locale = false;
    if (last == std::string_view::npos || block.empty() || block.back() != '"') {
        return false;  // 只有块的最后一个字符是引号时才可能出事
    }
    std::size_t pos = last + key.size();
    for (int n = 0; n < count; ++n) {
        while (CSpace(At(block, pos))) {
            ++pos;
        }
        // 列表项的边界靠 isspace 判断：判断落在非 ASCII 字节上时结果依赖区域设置
        if (High(At(block, pos))) {
            *locale = true;
        }
        if (At(block, pos) == '"') {
            const std::size_t start = pos + 1;
            const std::size_t close = block.find('"', start);
            if (close == std::string_view::npos) {
                if (start >= block.size()) {
                    return true;  // 缓冲大小 = (块尾 - 1) - start + 1 = 0
                }
                pos = block.size() - 1;
            } else {
                pos = close + 1;
            }
        } else {
            while (pos < block.size() && !CSpace(At(block, pos))) {
                *locale = *locale || High(At(block, pos));
                ++pos;
            }
        }
    }
    return false;
}

// 2901B0 的预处理：去掉 // 注释（含结尾的换行）和 /* */ 注释
std::string StripComments(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool line = false;
    bool block = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (line) {
            line = c != '\n';
        } else if (block) {
            if (c == '*' && At(text, i + 1) == '/') {
                block = false;
                ++i;
            }
        } else if (c == '/' && At(text, i + 1) == '*') {
            block = true;
            ++i;
        } else if (c == '/' && At(text, i + 1) == '/') {
            line = true;
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

bool ParseEffectText(std::string_view text, std::vector<EffectBlock>* blocks, std::string* why)
{
    blocks->clear();
    if (text.find('\0') != std::string_view::npos) {
        return Fail(why, "文本里有 0 字节（游戏读到那里就停）");
    }
    const std::string s = StripComments(text);
    std::size_t p = 0;
    for (;;) {
        // 跳过空白和 # 注释
        for (;;) {
            while (CSpace(At(s, p))) {
                ++p;
            }
            if (At(s, p) != '#') {
                break;
            }
            while (At(s, p) != 0) {
                ++p;
                if (At(s, p) == '\n') {
                    break;
                }
            }
        }
        if (p >= s.size()) {
            break;
        }
        const std::string label = BlockLabel(blocks->size());
        const std::size_t start = p;
        while (CIdent(At(s, p))) {
            ++p;
        }
        if (At(s, p) != ':') {
            if (why) {
                *why = label + "：开头不是“标识符:”（游戏的解析器在这里报错并停止，后面的块都不会读）";
            }
            return false;
        }
        ++p;
        // 找下一个冒号，# 到行尾之间的不算
        std::size_t q = p;
        bool found = false;
        for (;;) {
            const unsigned char c = At(s, q);
            if (c == ':') {
                found = true;
                break;
            }
            if (c == 0) {
                break;
            }
            if (c == '#') {
                while (At(s, q) != 0) {
                    ++q;
                    if (At(s, q) == '\n') {
                        break;
                    }
                }
                continue;
            }
            ++q;
        }
        std::size_t end = s.size();
        if (found) {
            // 退回到下一个标识符的前一个字符（这个字符不在块里）
            end = q;
            do {
                --end;
            } while (CIdent(At(s, end)));
            if (High(At(s, end))) {
                if (why) {
                    *why = label + "：下一个块的标识符前面是非 ASCII 字符（块的边界依赖区域设置）";
                }
                return false;
            }
        }
        if (end <= start) {
            if (why) {
                *why = label + "：块长度为 0（游戏的解析器会在这里死循环）";
            }
            return false;
        }
        EffectBlock b;
        b.offset = start;
        b.length = end - start;
        b.label = std::string(s.substr(start, p - 1 - start));
        if (b.length > kEffectBlockMax) {
            if (why) {
                *why = label + "（" + (b.label.empty() ? std::string("空标识符") : b.label) + ":）有 " + std::to_string(b.length) +
                    " 字节，超过游戏的上限 " + std::to_string(kEffectBlockMax) + "（注释、空白都算在块里；达到 1024 字节游戏进程会被 CRT 终止）";
            }
            return false;
        }
        const std::string_view body(s.data() + start, b.length);
        b.text = std::string(body);
        b.effect = body.substr(0, 7) == "effect:";
        if (b.effect) {
            bool locale = false;
            b.hasName = GameField(body, ".name", &b.name, &locale);
            if (locale) {
                if (why) {
                    *why = label + "：.name 的值含非 ASCII 字符且没有加引号（值的边界依赖区域设置）";
                }
                return false;
            }
            using namespace game::layout;
            static constexpr std::pair<std::string_view, int> kLists[] = {
                { ".kill_enemy_types", static_cast<int>(kEffectListKillEnemyTypes) }, { ".buff_ids", static_cast<int>(kEffectListBuffIds) },
                { ".summon_monsters", static_cast<int>(kEffectListSummonMonsters) }, { ".summon_ranks", static_cast<int>(kEffectListSummonRanks) },
                { ".set_monster_class_ids", static_cast<int>(kEffectListSetMonsterClassIds) } };
            for (const auto& [key, count] : kLists) {
                if (ListCrashes(body, key, count, &locale)) {
                    if (why) {
                        *why = label + "：" + std::string(key) + " 的某一项以块末尾的引号开头（游戏读这一项时进程会被 CRT 终止），补上右引号";
                    }
                    return false;
                }
                if (locale) {
                    if (why) {
                        *why = label + "：块以引号结尾，" + std::string(key) + " 的列表项里有非 ASCII 字符（列表项的边界依赖区域设置，"
                            "可能有一项正好从末尾的引号开始，游戏读这一项时进程会被终止）";
                    }
                    return false;
                }
            }
        }
        blocks->push_back(std::move(b));
        p = end;
    }
    return true;
}

bool ExtractEffectNames(std::string_view text, std::vector<std::string>* names, std::string* why)
{
    names->clear();
    std::vector<EffectBlock> blocks;
    if (!ParseEffectText(text, &blocks, why)) {
        return false;
    }
    std::vector<std::uint32_t> hashes;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        const std::string label = BlockLabel(i);
        if (!b.effect) {
            return Fail(why, (label + "的标识符是 \"" + b.label + "\"，不是 effect（值里的冒号会被游戏当成新块的开始，"
                                      "块里冒号后面的字段都读不到）").c_str());
        }
        if (!b.hasName) {
            return Fail(why, (label + "：effect: 块缺少 .name").c_str());
        }
        if (b.name.size() > kTextMax) {
            return Fail(why, (label + "：.name 超过 63 字节（游戏会截断）").c_str());
        }
        if (!ValidText(b.name, false) || b.name.front() == '.') {
            return Fail(why, (label + "：.name 为空、以点开头或含控制字符、引号（游戏取的是块里最后一个 .name）").c_str());
        }
        const std::uint32_t hash = GameHash(b.name);
        if (std::find(hashes.begin(), hashes.end(), hash) != hashes.end()) {
            return Fail(why, ("effect 名字重复（或哈希相同）：" + b.name).c_str());
        }
        hashes.push_back(hash);
        names->push_back(b.name);
    }
    if (names->empty()) {
        return Fail(why, "文本里没有 effect: 块");
    }
    // .target：4F02D0 按 36F020 取值后在 21 项的表（2AD58F0，静态初始化 9C560）里找，找不到（含缺少 .target）时弹 assert
    // 「EffectTarget: Invalid type」（选错按钮执行 int3），所以缺少或认不出都拒绝
    constexpr const auto& kTargets = game::layout::kEffectTargetsNames;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        std::string target;
        bool locale = false;
        const bool has = GameField(std::string_view(blocks[i].text), ".target", &target, &locale);
        const std::string label = BlockLabel(i);
        if (locale) {
            return Fail(why, (label + "：.target 的值含非 ASCII 字符且没有加引号（值的边界依赖区域设置）").c_str());
        }
        if (!has || std::none_of(std::begin(kTargets), std::end(kTargets), [&](const char* t) { return target == t; })) {
            return Fail(why, (label + "（" + blocks[i].name + "）：" + (has ? ".target \"" + target + "\" 认不出" : std::string("缺少 .target")) +
                                 "，游戏会弹 assert（EffectTarget: Invalid type）；可用 performer、target、target_group、global 等 21 种")
                                 .c_str());
        }
    }
    return true;
}

std::uint32_t OwnerTable::OwnerOf(std::uint32_t hash) const
{
    const auto it = map_.find(hash);
    return it == map_.end() ? 0 : it->second.first;
}

void OwnerTable::Set(std::uint32_t hash, std::uint32_t owner, std::string name)
{
    map_[hash] = { owner, std::move(name) };
}

const std::string* OwnerTable::NameOf(std::uint32_t hash) const
{
    const auto it = map_.find(hash);
    return it == map_.end() ? nullptr : &it->second.second;
}

}  // namespace ddc::combat

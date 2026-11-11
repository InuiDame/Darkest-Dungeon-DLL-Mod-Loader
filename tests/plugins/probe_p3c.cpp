// probe_p3c：actor 句柄（DDCore game 服务的 actor 部分，P3c）的运行验证，只读，不改任何游戏数据。
// 核对：事件里的 Hero/Monster 指针转句柄、句柄解析回同一个指针、读到的字段（HP、最大生命、压力、职业 id）
// 和独立读法一致；保存的句柄在战斗结束、下一场战斗、下一个存档时解析失败，英雄句柄跨战斗仍然有效。
// 同一次探索的下一场战斗，游戏从同一组 guid 重新给怪物编号（审核 B-B1）：battle.start 行写出上一场的句柄里 guid
// 和本场怪物相同的个数，它们也必须解析不到。出现过 actor.death（阵亡）、hero.dismiss（解雇）的英雄单独计数，
// 不计入「英雄句柄仍能解析」（阵亡的条目留在名册里，解雇的被删除，见 knowledge/dd1/combat.md）。
// 独立读法按 Steam 版 RVA 硬编码（同 probe_p3），exe 不是基准版本时只跑 game 服务这一侧。
// 日志：DDModLoader\log\probe_p3c.log，每场战斗、每个存档结束时有汇总行。依据见 knowledge/dd1/combat.md。

#include <dd/dd.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

dd::Runtime g_dd;
const DDGameService* g_game = nullptr;
std::uintptr_t g_base = 0;
bool g_raw = false;  // 基准版本：同时用硬编码 RVA 独立读一遍

constexpr const char* kExeSha1 = "9db2d73b89aa034e2055df65a139403e18df641f";
constexpr std::uintptr_t kHeroVtbl = 0xE8A288;
constexpr std::uintptr_t kMonsterVtbl = 0xEA9480;
constexpr std::uintptr_t kRaidCurrent = 0x118E2C8;
constexpr std::uintptr_t kMaxStatFn = 0x4C3F20;

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    if (src == 0) {
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
bool Read(std::uintptr_t address, T& out)
{
    return ReadBytes(address, &out, sizeof(T));
}

bool CallSlot(std::uintptr_t object, int slot, std::uintptr_t& out)
{
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        out = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtable[slot])(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallMaxStat(std::uintptr_t stat, std::uint32_t mask, float& out)
{
    __try {
        out = reinterpret_cast<float (*)(std::uintptr_t, std::uint32_t, bool)>(g_base + kMaxStatFn)(stat, mask, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* Ptr(std::uintptr_t value)
{
    return reinterpret_cast<const void*>(value);
}

bool Same(const DDActorRef& a, const DDActorRef& b)
{
    return a.kind == b.kind && a.id == b.id && a.campaign_serial == b.campaign_serial && a.raid_serial == b.raid_serial &&
        a.battle_serial == b.battle_serial && a.roster == b.roster;
}

std::string Describe(const DDActorRef& ref)
{
    std::string text = std::string(ref.kind == DD_ACTOR_HERO ? "hero#" : "monster#") + std::to_string(ref.id) + "（存档 " +
        std::to_string(ref.campaign_serial);
    if (ref.kind == DD_ACTOR_MONSTER) {
        text += "，探索 " + std::to_string(ref.raid_serial) + "，战斗 " + std::to_string(ref.battle_serial);
    }
    if (ref.roster) {
        text += "，备用名册";
    }
    return text + "）";
}

void Remember(std::vector<DDActorRef>& list, const DDActorRef& ref)
{
    for (const auto& item : list) {
        if (Same(item, ref)) {
            return;
        }
    }
    list.push_back(ref);
}

std::size_t CountResolved(const std::vector<DDActorRef>& list)
{
    std::size_t count = 0;
    for (const auto& ref : list) {
        count += g_game->actor_resolve(&ref) ? 1 : 0;
    }
    return count;
}

struct Counters {
    std::uint64_t pointers = 0, converted = 0, notActor = 0, convertFailed = 0;
    std::uint64_t resolveSame = 0, infoOk = 0;
    std::uint64_t hpSame = 0, stressSame = 0, stressChecked = 0, maxSame = 0, maxChecked = 0, classSame = 0;
    std::uint64_t maxBelowHp = 0, postChecked = 0, postSame = 0, healCapChecked = 0, healCapOk = 0;
} g_count;

std::vector<DDActorRef> g_heroes;          // 本存档见过的英雄
std::vector<DDActorRef> g_battleMonsters;  // 本场战斗见过的怪物
std::vector<DDActorRef> g_prevMonsters;    // 上一场战斗的怪物
std::vector<DDActorRef> g_prevHeroes;      // 上一个存档的英雄
std::set<std::uint32_t> g_gone;            // 本存档里阵亡、被解雇的 hero id
// 指针字段的逐条日志：每次进存档最多 kLogsPerCampaign 行，每场战斗开始时至少再给 kLogsPerBattle 行
// （原来全程只记 60 行，第二次进存档时就没有样本了）。有问题的行不受限
constexpr int kLogsPerCampaign = 60;
constexpr int kLogsPerBattle = 20;
int g_logs = 0;
int g_battles = 0;

std::uint32_t BattleSerial()
{
    return g_game->battle_serial ? g_game->battle_serial() : 0;
}

// 英雄句柄：不计阵亡、解雇的，返回 (个数, 仍能解析)；gone 是阵亡、解雇的 (个数, 仍能解析)
void CountHeroes(std::size_t* total, std::size_t* resolved, std::size_t* gone, std::size_t* goneResolved)
{
    *total = *resolved = *gone = *goneResolved = 0;
    for (const auto& ref : g_heroes) {
        const bool ok = g_game->actor_resolve(&ref) != nullptr;
        if (g_gone.count(ref.id)) {
            ++*gone;
            *goneResolved += ok ? 1 : 0;
        } else {
            ++*total;
            *resolved += ok ? 1 : 0;
        }
    }
}

// 独立读法：返回是否全部一致，text 里写不一致的项
bool CompareRaw(std::uintptr_t p, const DDActorRef& ref, const DDActorInfo& info, std::string& text)
{
    bool all = true;
    float hp = 0;
    if (Read(p + 0x10A8, hp) && hp == info.hp) {
        ++g_count.hpSame;
    } else {
        all = false;
        text += " HP 不一致（独立读 " + std::to_string(hp) + "）";
    }
    if (ref.kind == DD_ACTOR_HERO) {
        ++g_count.stressChecked;
        float stress = 0;
        if (Read(p + 0x12FC, stress) && info.has_stress && stress == info.stress) {
            ++g_count.stressSame;
        } else {
            all = false;
            text += " 压力不一致";
        }
    }
    std::uint32_t mask = 0;
    float maxHp = 0;
    if (Read(p + 0x790, mask) && CallMaxStat(p + 0x768, mask, maxHp)) {
        ++g_count.maxChecked;
        if (info.has_max_hp && maxHp == info.max_hp) {
            ++g_count.maxSame;
        } else {
            all = false;
            text += " 最大生命不一致（独立算 " + std::to_string(maxHp) + "）";
        }
    }
    std::uintptr_t actorClass = 0;
    char id[64] = {};
    if (CallSlot(p, 6, actorClass) && ReadBytes(actorClass + 8, id, sizeof(id) - 1) && std::strcmp(id, info.class_id) == 0) {
        ++g_count.classSame;
    } else {
        all = false;
        text += std::string(" 职业 id 不一致（槽位 6 读到 '") + id + "'）";
    }
    return all;
}

// 一个事件指针字段：转句柄、解析回来、读字段、和独立读法比较
bool CheckPointer(const char* topic, const char* field, void* pointer, DDActorRef* outRef = nullptr, DDActorInfo* outInfo = nullptr)
{
    if (!pointer) {
        return false;
    }
    ++g_count.pointers;
    const auto p = reinterpret_cast<std::uintptr_t>(pointer);
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (!g_game->actor_from_pointer(pointer, &ref)) {
        std::uintptr_t vtable = 0;
        Read(p, vtable);
        const bool actor = g_raw && (vtable == g_base + kHeroVtbl || vtable == g_base + kMonsterVtbl);
        (actor ? g_count.convertFailed : g_count.notActor) += 1;
        if (actor || g_logs < kLogsPerCampaign) {
            ++g_logs;
            g_dd.log().info("{}.{}={} 不是句柄：{}", topic, field, Ptr(p),
                actor ? "虚表是 Hero/Monster 但转换失败！（不在名册或不在当前战斗）" : "不是 Hero/Monster");
        }
        return false;
    }
    ++g_count.converted;
    const bool same = g_game->actor_resolve(&ref) == pointer;
    g_count.resolveSame += same ? 1 : 0;
    DDActorInfo info{};
    info.size = sizeof(info);
    const bool infoOk = g_game->actor_info(&ref, &info);
    g_count.infoOk += infoOk ? 1 : 0;
    std::string diff;
    const bool rawOk = !g_raw || !infoOk || CompareRaw(p, ref, info, diff);
    if (infoOk && info.has_max_hp && info.hp > info.max_hp + 0.01f) {
        ++g_count.maxBelowHp;
        diff += " HP 大于最大生命！";
    }
    Remember(ref.kind == DD_ACTOR_HERO ? g_heroes : g_battleMonsters, ref);
    const bool bad = !same || !infoOk || !rawOk || !diff.empty();
    if (g_logs < kLogsPerCampaign || bad) {
        ++g_logs;
        g_dd.log().info("{}.{}={} -> {} 解析回同一指针={} HP {:.1f}/{} 压力 {} 职业 '{}'{}{}", topic, field, Ptr(p), Describe(ref),
            same ? "是" : "否！", info.hp, info.has_max_hp ? std::to_string(info.max_hp) : std::string("无"),
            info.has_stress ? std::to_string(info.stress) : std::string("-"), info.class_id, infoOk ? "" : " 读取失败！",
            diff.empty() ? "" : " ！" + diff);
    }
    if (outRef) {
        *outRef = ref;
    }
    if (outInfo) {
        *outInfo = info;
    }
    return infoOk;
}

void* Pointer(DDEvent* ev, const char* field)
{
    return g_dd.events().get_pointer(ev, field);
}

void OnPair(DDEvent* ev, const char* a, const char* b)
{
    const char* topic = g_dd.events().topic(ev);
    CheckPointer(topic, a, Pointer(ev, a));
    CheckPointer(topic, b, Pointer(ev, b));
}

// .post：读到的字段和事件给出的结算后数值一致（同一时刻、同一对象）
void OnPost(DDEvent* ev, const char* valueField, bool stress)
{
    const auto& events = g_dd.events();
    DDActorRef ref{};
    DDActorInfo info{};
    if (!CheckPointer(events.topic(ev), "target", Pointer(ev, "target"), &ref, &info)) {
        return;
    }
    if (stress && ref.kind != DD_ACTOR_HERO) {
        return;
    }
    const double expected = events.get_float(ev, valueField);
    const double actual = stress ? info.stress : info.hp;
    ++g_count.postChecked;
    const bool same = std::fabs(expected - actual) < 0.001;
    g_count.postSame += same ? 1 : 0;
    if (!same) {
        g_dd.log().warn("{} {}={:.3f}，句柄读到 {:.3f}！", events.topic(ev), valueField, expected, actual);
    }
    if (!stress && std::strcmp(events.topic(ev), "heal.post") == 0 && info.has_max_hp) {
        ++g_count.healCapChecked;
        const bool ok = expected <= info.max_hp + 0.01;
        g_count.healCapOk += ok ? 1 : 0;
        g_dd.log().info("heal.post {} 治疗后 {:.1f}，最大生命 {:.1f}{}", Describe(ref), expected, info.max_hp, ok ? "" : " 超出上限！");
    }
}

std::vector<std::uintptr_t> RawBattleMonsters()
{
    std::vector<std::uintptr_t> out;
    std::uintptr_t raid = 0;
    std::uintptr_t vec[2] = {};
    if (!g_raw || !Read(g_base + kRaidCurrent, raid) || !ReadBytes(raid + 0x4898 + 0x38, vec, sizeof(vec)) || vec[1] < vec[0] ||
        vec[1] - vec[0] > 64 * 8) {
        return out;
    }
    for (std::uintptr_t at = vec[0]; at < vec[1]; at += 8) {
        std::uintptr_t monster = 0;
        if (Read(at, monster)) {
            out.push_back(monster);
        }
    }
    return out;
}

void OnBattleStart(DDEvent*)
{
    ++g_battles;
    g_logs = (std::min)(g_logs, kLogsPerCampaign - kLogsPerBattle);
    const std::size_t prevResolved = CountResolved(g_prevMonsters);
    std::string guids;
    std::size_t found = 0;
    std::size_t monsters = 0;
    std::set<std::uint32_t> current;
    for (auto p : RawBattleMonsters()) {
        std::uintptr_t vtable = 0;
        std::uint32_t guid = 0;
        if (!Read(p, vtable) || vtable != g_base + kMonsterVtbl || !Read(p + 0x1320, guid)) {
            continue;
        }
        ++monsters;
        current.insert(guid);
        DDActorRef ref{};
        ref.size = sizeof(ref);
        const bool ok = g_game->actor_find(DD_ACTOR_MONSTER, guid, &ref) && g_game->actor_resolve(&ref) == reinterpret_cast<void*>(p);
        found += ok ? 1 : 0;
        if (ok) {
            Remember(g_battleMonsters, ref);
        }
        guids += " " + std::to_string(guid);
    }
    std::size_t sameGuid = 0;
    for (const auto& ref : g_prevMonsters) {
        sameGuid += current.count(ref.id) ? 1 : 0;
    }
    g_dd.log().info("battle.start 第 {} 场：探索序号 {}，战斗序号 {}，怪物表里 {} 个（guid:{}），actor_find 找回同一指针 {} 个；"
                    "上一场战斗的怪物句柄 {} 个，仍能解析 {} 个（应为 0），其中 guid 和本场怪物相同 {} 个",
        g_battles, g_game->raid_serial(), BattleSerial(), monsters, guids, found, g_prevMonsters.size(), prevResolved, sameGuid);
}

void OnBattleEnd(DDEvent*)
{
    // BattleDestroyedEvent 在怪物表清空之后派发（5A5720 先调用 5A5BE0）
    std::size_t total = 0, resolved = 0, gone = 0, goneResolved = 0;
    CountHeroes(&total, &resolved, &gone, &goneResolved);
    g_dd.log().info("battle.end 第 {} 场：本场怪物句柄 {} 个，仍能解析 {} 个（应为 0）；本存档英雄句柄 {} 个，仍能解析 {} 个"
                    "（不计阵亡、解雇的 {} 个，其中仍能解析 {} 个）",
        g_battles, g_battleMonsters.size(), CountResolved(g_battleMonsters), total, resolved, gone, goneResolved);
    g_prevMonsters = g_battleMonsters;
    g_battleMonsters.clear();
}

void OnDeath(DDEvent* ev)
{
    const auto& events = g_dd.events();
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (events.get_bool(ev, "is_monster")) {
        const auto guid = static_cast<std::uint32_t>(events.get_int(ev, "monster_guid"));
        const bool ok = g_game->actor_find(DD_ACTOR_MONSTER, guid, &ref);
        g_dd.log().info("actor.death 怪物 guid={}：actor_find {}", guid, ok ? "成功（死亡事件时仍在战斗里）" : "失败");
    } else {
        const auto id = static_cast<std::uint32_t>(events.get_int(ev, "hero_id"));
        const bool ok = g_game->actor_find(DD_ACTOR_HERO, id, &ref);
        g_gone.insert(id);
        g_dd.log().info("actor.death 英雄 hero_id={}：actor_find {}", id, ok ? "成功" : "失败");
    }
}

void OnDismiss(DDEvent* ev)
{
    const auto id = static_cast<std::uint32_t>(g_dd.events().get_int(ev, "hero_id"));
    DDActorRef ref{};
    ref.size = sizeof(ref);
    const bool ok = g_game->actor_find(DD_ACTOR_HERO, id, &ref);
    g_gone.insert(id);
    g_dd.log().info("hero.dismiss hero_id={}：actor_find {}（解雇时条目从名册删除，之后应解析不到）", id, ok ? "成功" : "失败");
}

void Summary(const char* when)
{
    const auto& c = g_count;
    g_dd.log().info("==== 汇总（{}）====", when);
    g_dd.log().info("指针字段 {} 个：转成句柄 {}，不是 actor {}，虚表是 actor 却转换失败 {}；解析回同一指针 {}，读取成功 {}", c.pointers,
        c.converted, c.notActor, c.convertFailed, c.resolveSame, c.infoOk);
    g_dd.log().info("独立读法{}：HP 一致 {}，压力一致 {}/{}，最大生命一致 {}/{}，职业 id 一致 {}；HP 大于最大生命 {} 次", g_raw ? "" : "（未启用）",
        c.hpSame, c.stressSame, c.stressChecked, c.maxSame, c.maxChecked, c.classSame, c.maxBelowHp);
    g_dd.log().info(".post 结算值和句柄读数一致 {}/{}；治疗后不超过最大生命 {}/{}；战斗 {} 场，本存档英雄句柄 {} 个", c.postSame, c.postChecked,
        c.healCapOk, c.healCapChecked, g_battles, g_heroes.size());
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "probe_p3c";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.events()) {
        return false;
    }
    g_game = static_cast<const DDGameService*>(api->query(DD_SERVICE_GAME, DD_SERVICE_GAME_VERSION));
    if (!g_game || g_game->header.size < sizeof(DDGameService)) {
        g_dd.log().error("game 服务没有 actor 函数，探针不工作");
        return true;
    }
    const char* sha1 = g_dd.game()->exe_sha1();
    g_raw = sha1 && std::strcmp(sha1, kExeSha1) == 0;
    g_base = reinterpret_cast<std::uintptr_t>(g_dd.game()->image_base());
    g_dd.log().info("exe {}，独立读法{}", sha1 ? sha1 : "", g_raw ? "启用（基准版本）" : "不启用（不是基准版本，只检查 game 服务）");

    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_LOAD, [](const DDPhaseEvent& event) {
        const std::uint32_t kinds = g_game->actor_kinds();
        g_logs = 0;
        g_gone.clear();
        g_dd.log().info("CampaignLoad 存档序号 {}：actor_kinds 英雄 {} 怪物 {}，探索序号 {}，战斗序号 {}；上一个存档的英雄句柄 {} 个，仍能解析 {} 个（应为 0）",
            event.campaign_serial, (kinds >> DD_ACTOR_HERO) & 1, (kinds >> DD_ACTOR_MONSTER) & 1, g_game->raid_serial(), BattleSerial(),
            g_prevHeroes.size(), CountResolved(g_prevHeroes));
    });
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_UNLOAD, [](const DDPhaseEvent&) {
        Summary("退出存档");
        g_prevHeroes = g_heroes;
        g_heroes.clear();
        g_battleMonsters.clear();
        g_prevMonsters.clear();
    });

    const auto& events = g_dd.events();
    events.on("battle.start", OnBattleStart);
    events.on("battle.end", OnBattleEnd);
    events.on("damage.pre", [](DDEvent* ev) { OnPair(ev, "target", "attacker"); });
    events.on("hit.pre", [](DDEvent* ev) { OnPair(ev, "attacker", "target"); });
    events.on("skill.pre", [](DDEvent* ev) { OnPair(ev, "performer", "target"); });
    events.on("heal.pre", [](DDEvent* ev) { OnPair(ev, "target", "healer"); });
    events.on("stress.pre", [](DDEvent* ev) { OnPair(ev, "target", "source"); });
    events.on("damage.post", [](DDEvent* ev) { OnPost(ev, "hp_after", false); });
    events.on("heal.post", [](DDEvent* ev) { OnPost(ev, "hp_after", false); });
    events.on("stress.post", [](DDEvent* ev) { OnPost(ev, "stress_after", true); });
    events.on("actor.death", OnDeath);
    events.on("hero.dismiss", OnDismiss);
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}

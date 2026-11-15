// test_combat：P4a 的 combat 服务验证插件。日志 DDModLoader\log\test_combat.log，每行 [f<帧号>] <标签> k=v ...
//
// 主菜单自检（不进存档也能跑）：
//   SELF1 服务、枚举表、存档外的写操作被拒绝（DD_COMBAT_E_NO_CAMPAIGN）、safe_point 为假；5 个内联 hook 的入口没有被改
//   SELF2 订阅 buff.add/remove/tick、damage.final，注册规则 → 下一帧 DDCore 装上 hook：入口变成跳转
//   SELF3 撤销订阅和规则 → 入口恢复原样
//   SELFDONE ok=1 表示三步都符合预期
// 入口地址用 hook 服务的 scan（特征码同 DDCore 的 signatures.inc，scan 会把 DDCore 自己的补丁还原后再匹配）。
//
// 用户场景（进存档后自动进行，城镇、走廊、战斗中各一轮，每次进存档每种场合一轮）：
//   CampaignLoad：注册 buff 定义 ddtest_combat_spd（combat_stat_add speed_rating +2，after_turn 3 次）、ddtest_combat_rule
//   （is_actor_status + 规则 ddtest_rule，speed +1）、effect ddtest_combat_fx（performer 自己 speed +1，1 回合）；
//   重复注册游戏数据（SPD5、rakebuff_5）要被拒绝；注册规则 ddtest_rule（恒为真，回调里试一次写操作要被拒绝）。
//   每一轮：给一个英雄（战斗中另加一个怪物）按 id 加 buff（带标签、不带标签、叠加同 id）、施加 effect（城镇要被拒绝）、
//   在 buff.add 回调里删除和添加（要排队，下一帧经操作回调拿到结果），90 帧后按标签、guid 全部删掉并核对条数。
//   战斗中英雄每场第一次受到伤害时 damage.final 全部吸收（游戏应显示 Blocked），第二次吸收一半，在 damage.post 核对生命。
//   另外记录 buff.add/remove/tick、skill.pre（转成技能句柄）、effect.pre（effect_name）的前若干条。
// 主动测试会给存档里的英雄临时加几条 buff，90 帧内全部删掉；DDModLoader\cfg\test_combat.cfg 写 active = 0 可以只做被动记录。

#include <dd/dd.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

dd::Runtime g_dd;
const DDCombatService* g_combat = nullptr;
const DDGameService* g_game = nullptr;
std::uint64_t g_frame = 0;
bool g_active = true;

template <class... Args>
void L(const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    g_dd.log().write(DD_LOG_INFO, std::format("[f{}] {} ", g_frame, tag) + std::format(fmt, std::forward<Args>(args)...));
}

// ok 为假时写 WARN，场景检查按 WARN 找不符合预期的项
template <class... Args>
void LW(bool ok, const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    g_dd.log().write(ok ? DD_LOG_INFO : DD_LOG_WARN, std::format("[f{}] {} ", g_frame, tag) + std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void W(const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    g_dd.log().write(DD_LOG_WARN, std::format("[f{}] {} ", g_frame, tag) + std::format(fmt, std::forward<Args>(args)...));
}

const char* Text(std::uint32_t r)
{
    return g_combat ? g_combat->result_text(r) : "?";
}

// ---- 读游戏内存（SEH），只用来找英雄、怪物的指针 ----
template <class T>
T Rd(std::uintptr_t address, T fallback = T{})
{
    if (address < 0x10000) {
        return fallback;
    }
    T value{};
    __try {
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

std::vector<std::uintptr_t> PtrVec(std::uintptr_t address, std::size_t max)
{
    std::vector<std::uintptr_t> out;
    const auto first = Rd<std::uintptr_t>(address);
    const auto last = Rd<std::uintptr_t>(address + 8);
    if (!first || last < first || (last - first) / 8 > max) {
        return out;
    }
    for (std::uintptr_t p = first; p < last; p += 8) {
        out.push_back(Rd<std::uintptr_t>(p));
    }
    return out;
}

// 全局指针：hook 服务的 scan 加 rip 相对位移
std::uintptr_t ScanGlobal(const char* pattern, int capture)
{
    void* hit = nullptr;
    if (!g_dd.hooks().scan(pattern, &hit) || !hit) {
        return 0;
    }
    const auto at = reinterpret_cast<std::uintptr_t>(hit) + static_cast<std::uintptr_t>(capture);
    return at + 4 + static_cast<std::intptr_t>(Rd<std::int32_t>(at));
}

std::uintptr_t g_campaignGlobal = 0;  // Campaign::Current
std::uintptr_t g_raidGlobal = 0;      // Raid::Current

std::uintptr_t Raid()
{
    return g_raidGlobal ? Rd<std::uintptr_t>(g_raidGlobal) : 0;
}

int BattleState()
{
    const auto raid = Raid();
    return raid ? Rd<std::int32_t>(raid + 0x4B20) : 0;
}

std::vector<std::uintptr_t> Monsters()
{
    const auto raid = Raid();
    return raid ? PtrVec(raid + 0x4898 + 0x38, 32) : std::vector<std::uintptr_t>{};
}

std::optional<DDActorRef> RefOf(std::uintptr_t actor)
{
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (actor && g_game && g_game->actor_from_pointer(reinterpret_cast<const void*>(actor), &ref)) {
        return ref;
    }
    return std::nullopt;
}

// 城镇：名册里第一个能转成句柄、生命大于 0 的英雄；地牢里：队伍第一个
std::optional<DDActorRef> PickHero()
{
    std::vector<std::uintptr_t> candidates;
    if (const auto raid = Raid()) {
        candidates = PtrVec(raid + 8, 8);
    } else if (g_campaignGlobal) {
        if (const auto campaign = Rd<std::uintptr_t>(g_campaignGlobal)) {
            for (auto entry : PtrVec(campaign + 0x20, 4096)) {
                candidates.push_back(entry ? entry + 8 : 0);
            }
        }
    }
    for (auto actor : candidates) {
        auto ref = RefOf(actor);
        if (!ref) {
            continue;
        }
        DDActorInfo info{};
        info.size = sizeof(info);
        if (g_game->actor_info(&*ref, &info) && info.hp > 0.0f) {
            return ref;
        }
    }
    return std::nullopt;
}

// 速度：Actor+0x630 的属性值结构（P4-0D：类型 4 speed_rating），取值用游戏的合计函数 (结构, [结构+0x28], true)
using StatFn = float (*)(const void*, std::uint32_t, bool);
StatFn g_statFn = nullptr;

float SpeedAt(std::uintptr_t actor)
{
    __try {
        const std::uintptr_t stat = actor + 0x630;
        return g_statFn(reinterpret_cast<const void*>(stat), *reinterpret_cast<const std::uint32_t*>(stat + 0x28), true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NAN;
    }
}

float Speed(const DDActorRef& ref)
{
    void* actor = g_game ? g_game->actor_resolve(&ref) : nullptr;
    return actor && g_statFn ? SpeedAt(reinterpret_cast<std::uintptr_t>(actor)) : NAN;
}

std::string Who(const DDActorRef& ref)
{
    return std::format("{}#{}", ref.kind == DD_ACTOR_HERO ? "hero" : "monster", ref.id);
}

std::string BuffText(const DDActorRef& actor, std::uint32_t guid)
{
    DDBuffInfo info{};
    if (!g_dd.combat().buff_get(actor, guid, info)) {
        return std::format("{}:不在", guid);
    }
    return std::format("{}:{}:s{}:a{:g}:d{}:t{}:src{}(flags {}):r{}{}:plugin={}:tag={}", guid, info.id, info.stat_type, info.amount, info.duration,
        info.duration_type, info.source, info.source_flags, info.rule_type, info.is_false_rule ? "!" : "", info.plugin, info.tag);
}

std::size_t CountId(const DDActorRef& actor, const char* id)
{
    std::size_t n = 0;
    for (auto guid : g_dd.combat().buff_guids(actor)) {
        DDBuffInfo info{};
        if (g_dd.combat().buff_get(actor, guid, info) && std::strcmp(info.id, id) == 0) {
            ++n;
        }
    }
    return n;
}

// ---- 主菜单自检：内联 hook 的入口 ----
struct Target {
    const char* name;
    const char* pattern;
    std::uintptr_t address = 0;
};

Target g_targets[] = {
    { "buff.add(5C4F50)", "?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B D9 89 54 24 20 49 8B D0 48 8D 4C 24 28 E8 ?? ?? ?? ??" },
    { "buff.remove(5C4FF0)", "?? ?? ?? ?? ?? 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 20 49 8B D8" },
    { "buff.tick(47AAD0)",
        "?? ?? ?? ?? ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 20 45 8B F0" },
    { "buff.rule(479CD0)", "?? ?? ?? ?? ?? 41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC E8 00 00 00 0F 29 B4 24 D0 00 00 00" },
    { "damage.final(480400)", "?? ?? ?? ?? ?? 48 89 74 24 18 57 48 83 EC 20 40 32 F6" },
};

bool Patched(const Target& t)
{
    const auto b = Rd<std::uint8_t>(t.address);
    return b == 0xE9 || b == 0xFF;
}

std::string TargetState(bool* allPatched, bool* nonePatched)
{
    std::string out;
    *allPatched = true;
    *nonePatched = true;
    for (const auto& t : g_targets) {
        const bool p = t.address && Patched(t);
        *allPatched = *allPatched && p;
        *nonePatched = *nonePatched && !p && t.address;
        out += std::format(" {}={}", t.name, !t.address ? "未找到" : (p ? "跳转" : "原样"));
    }
    return out;
}

std::vector<DDEventSubId> g_selfSubs;
DDRuleId g_selfRule = 0;
int g_selfStep = 0;
std::uint64_t g_selfAt = 0;
bool g_selfOk = true;

void DD_CALL NoopEvent(DDEvent*, void*) {}
uint32_t DD_CALL NoopRule(const DDRuleContext*, void*)
{
    return DD_RULE_FALSE;
}

void SelfCheck()
{
    if (g_selfStep >= 4 || g_frame < g_selfAt) {
        return;
    }
    auto* events = g_dd.events().service();
    if (g_selfStep == 0) {
        for (auto& t : g_targets) {
            void* hit = nullptr;
            t.address = g_dd.hooks().scan(t.pattern, &hit) ? reinterpret_cast<std::uintptr_t>(hit) : 0;
        }
        DDActorRef fake{};
        fake.size = sizeof(fake);
        fake.kind = DD_ACTOR_HERO;
        fake.id = 1;
        DDCombatOpResult result{};
        const auto add = g_dd.combat().buff_add(fake, "SPD5", nullptr, &result);
        DDBuffDef def{};
        def.size = sizeof(def);
        strcpy_s(def.id, "ddtest_selfcheck");
        def.stat_type = 2;
        strcpy_s(def.stat_sub_type, "speed_rating");  // combat_stat_add 必须带游戏认的 sub_stat（审核 C-Y3）
        def.duration = -1;
        def.duration_type = 6;
        const auto define = g_dd.combat().buff_define(def);
        bool roundTrip = true;
        for (std::uint32_t t = 0; t < 4; ++t) {
            for (std::uint32_t i = 0; i < g_combat->enum_count(t); ++i) {
                roundTrip = roundTrip && g_combat->enum_value(t, g_combat->enum_name(t, static_cast<std::int32_t>(i))) == static_cast<std::int32_t>(i);
            }
        }
        const bool enums = g_combat->enum_count(DD_ENUM_STAT) == 102 && g_combat->enum_value(DD_ENUM_RULE, "is_actor_status") == 42 &&
            g_combat->enum_value(DD_ENUM_SOURCE, "bsrc_trinket") == 6 && roundTrip;
        bool all = false, none = false;
        const std::string state = TargetState(&all, &none);
        const bool ok = add == DD_COMBAT_E_NO_CAMPAIGN && define == DD_COMBAT_E_NO_CAMPAIGN && !g_combat->safe_point() && enums && none;
        g_selfOk = g_selfOk && ok;
        L("SELF1", "ok={} buff_add={}（{}，message={}） buff_define={} safe_point={} enums={} 入口：{}", ok ? 1 : 0, add, Text(add),
            result.message ? result.message : "-", define, g_combat->safe_point() ? 1 : 0, enums ? 1 : 0, state);
        for (const char* topic : { "buff.add", "buff.remove", "buff.tick", "damage.final" }) {
            const auto id = events->subscribe(g_dd.plugin(), topic, &NoopEvent, nullptr, nullptr);
            if (id) {
                g_selfSubs.push_back(id);
            }
        }
        g_selfRule = g_combat->rule_register(g_dd.plugin(), "ddtest_selfcheck_rule", &NoopRule, nullptr);
        L("SELF2", "phase=subscribe subs={} rule={}（下一帧 DDCore 安装 hook）", g_selfSubs.size(), g_selfRule);
        g_selfStep = 1;
        g_selfAt = g_frame + 10;
    } else if (g_selfStep == 1) {
        bool all = false, none = false;
        const std::string state = TargetState(&all, &none);
        const bool ok = all && g_selfSubs.size() == 4 && g_selfRule != 0;
        g_selfOk = g_selfOk && ok;
        L("SELF2", "ok={} 入口：{}", ok ? 1 : 0, state);
        for (auto id : g_selfSubs) {
            events->unsubscribe(g_dd.plugin(), id);
        }
        g_selfSubs.clear();
        const bool unregistered = g_combat->rule_unregister(g_dd.plugin(), g_selfRule);
        L("SELF3", "phase=unsubscribe rule_unregister={}", unregistered ? 1 : 0);
        g_selfStep = 2;
        g_selfAt = g_frame + 10;
    } else if (g_selfStep == 2) {
        bool all = false, none = false;
        const std::string state = TargetState(&all, &none);
        const bool ok = none && events->subscription_count(g_dd.plugin()) == 0;
        g_selfOk = g_selfOk && ok;
        L("SELF3", "ok={} 入口：{}", ok ? 1 : 0, state);
        L("SELFDONE", "ok={}", g_selfOk ? 1 : 0);
        g_selfStep = 4;
    }
}

// ---- 用户场景 ----
constexpr const char* kSpd = "ddtest_combat_spd";
constexpr const char* kRuleBuff = "ddtest_combat_rule";
constexpr const char* kFx = "ddtest_combat_fx";
constexpr const char* kRule = "ddtest_rule";

std::uint64_t g_ruleHits = 0;
bool g_ruleWriteTried = false;
std::map<std::string, int> g_reasons;
int g_addLogged = 0, g_removeLogged = 0, g_tickLogged = 0, g_skillLogged = 0, g_effectLogged = 0;
std::uint64_t g_adds = 0, g_removes = 0, g_ticks = 0, g_deferred = 0;

enum class Where { Town, Corridor, Battle };
const char* WhereName(Where w)
{
    return w == Where::Town ? "town" : (w == Where::Corridor ? "corridor" : "battle");
}

struct Round {
    Where where = Where::Town;
    DDActorRef hero{};
    std::optional<DDActorRef> monster;
    std::uint32_t gTag = 0, gRule = 0, gStack = 0, gQueueTrigger = 0;
    float speed0 = NAN;
    std::vector<std::uint32_t> fxGuids;
    std::uint64_t finishAt = 0;
};
std::optional<Round> g_round;
bool g_done[3] = {};
std::uint64_t g_stableSince = 0;
int g_lastWhere = -1;
std::uint32_t g_session = 0;

// buff.add 回调里的排队测试：等这条 buff 加上时，在回调里删掉它、再加一条
std::uint32_t g_queueGuid = 0;
DDActorRef g_queueActor{};
bool g_queueArmed = false;
std::uint64_t g_queueRemoveOp = 0, g_queueAddOp = 0;
std::uint32_t g_queuedAddGuid = 0;

void OnOpResult(const DDCombatOpResult& r)
{
    L("OPRESULT", "op={} kind={} result={}（{}） guid={} count={} message={} remove_op={} add_op={}", r.op, r.kind, r.result, Text(r.result), r.guid,
        r.count, r.message ? r.message : "", g_queueRemoveOp, g_queueAddOp);
    if (r.op == g_queueAddOp && r.result == DD_COMBAT_OK) {
        g_queuedAddGuid = r.guid;
    }
}

uint32_t DD_CALL RuleCallback(const DDRuleContext* ctx, void*)
{
    ++g_ruleHits;
    if (!g_ruleWriteTried) {
        g_ruleWriteTried = true;
        DDActorRef ref{};
        ref.size = sizeof(ref);
        ref.kind = DD_ACTOR_HERO;
        DDCombatOpResult result{};
        const auto r = g_dd.combat().buff_add(ref, kSpd, nullptr, &result);
        L("RULEWRITE", "rule={} rule_type={} buff={} false={} buff_add_in_rule={}（{}，期望 {}）", ctx->rule, ctx->rule_type, ctx->buff_id,
            ctx->is_false_rule ? 1 : 0, r, Text(r), static_cast<int>(DD_COMBAT_E_IN_RULE));
    }
    return DD_RULE_TRUE;
}

void DefineAll()
{
    auto& combat = g_dd.combat();
    DDBuffDef spd{};
    spd.size = sizeof(spd);
    strcpy_s(spd.id, kSpd);
    spd.stat_type = combat.service()->enum_value(DD_ENUM_STAT, "combat_stat_add");
    strcpy_s(spd.stat_sub_type, "speed_rating");
    spd.amount = 2.0f;
    spd.duration = 3;
    spd.duration_type = combat.service()->enum_value(DD_ENUM_DURATION, "after_turn");
    spd.source = combat.service()->enum_value(DD_ENUM_SOURCE, "skill");
    DDBuffDef rule = spd;
    strcpy_s(rule.id, kRuleBuff);
    rule.amount = 1.0f;
    rule.rule_type = combat.service()->enum_value(DD_ENUM_RULE, "is_actor_status");
    strcpy_s(rule.rule_string, kRule);
    DDCombatOpResult r1{}, r2{}, r3{}, r4{}, r5{}, r6{};
    const auto a = combat.buff_define(spd, &r1);
    const auto b = combat.buff_define(rule, &r2);
    spd.amount = 2.0f;
    const auto again = combat.buff_define(spd, &r3);  // 同一个插件重复注册：更新
    DDBuffDef game = spd;
    strcpy_s(game.id, "SPD5");
    const auto clash = combat.buff_define(game, &r4);
    const std::string fx = std::format("effect: .name \"{}\" .target \"performer\" .chance 100% .combat_stat_buff 1 .speed_rating_add 1 "
                                       ".on_hit true .on_miss true .duration 1\r\n",
        kFx);
    const auto e = combat.effect_define(fx.c_str(), &r5);
    const auto eClash = combat.effect_define("effect: .name \"rakebuff_5\" .target \"performer\"\r\n", &r6);
    DDBuffDef got{};
    got.size = sizeof(got);
    const bool read = combat.service()->buff_def_get(kSpd, &got);
    DDEffectInfo fxInfo{};
    fxInfo.size = sizeof(fxInfo);
    const bool fxRead = combat.service()->effect_get(kFx, &fxInfo);
    L("DEFINE", "spd={} rule={} again={} SPD5={}（期望 {}：{}） fx={}（count={} {}） rakebuff_5={}（期望 {}：{}） def_get={} stat={} amount={:g} "
                "dur={} effect_get={} target={} runtime={} duration={}",
        a, b, again, clash, static_cast<int>(DD_COMBAT_E_EXISTS), r4.message ? r4.message : "", e, r5.count, r5.message ? r5.message : "", eClash,
        static_cast<int>(DD_COMBAT_E_EXISTS), r6.message ? r6.message : "", read ? 1 : 0, got.stat_type, got.amount, got.duration, fxRead ? 1 : 0,
        fxInfo.target, fxInfo.runtime ? 1 : 0, fxInfo.duration);
    DDEffectInfo rake{};
    rake.size = sizeof(rake);
    if (combat.service()->effect_get("rakebuff_5", &rake)) {
        L("EFFECTGET", "name={} target={} chance={:g} on_hit={} on_miss={} buff_ids={} runtime={}", rake.name, rake.target, rake.chance,
            rake.on_hit ? 1 : 0, rake.on_miss ? 1 : 0, rake.buff_id_count, rake.runtime ? 1 : 0);
    }
}

std::uint32_t Add(const DDActorRef& actor, const char* id, const char* tag, const char* label)
{
    DDBuffAddOptions options{};
    options.size = sizeof(options);
    options.tag = tag;
    DDCombatOpResult r{};
    const auto code = g_dd.combat().buff_add(actor, id, &options, &r);
    L("ADD", "label={} who={} id={} tag={} result={}（{}） guid={} op={} message={} entry={}", label, Who(actor), id, tag ? tag : "-", code,
        Text(code), r.guid, r.op, r.message ? r.message : "", code == DD_COMBAT_OK ? BuffText(actor, r.guid) : std::string("-"));
    return code == DD_COMBAT_OK ? r.guid : 0;
}

std::uint32_t Remove(const DDActorRef& actor, DDBuffMatch match, std::uint32_t guid, const char* text, std::uint32_t expect, const char* label)
{
    DDCombatOpResult r{};
    const auto code = g_dd.combat().buff_remove(actor, match, guid, 0, text, &r);
    const bool ok = code == DD_COMBAT_OK && r.count == expect;
    LW(ok, "REMOVE", "label={} who={} match={} text={} guid={} result={} count={} expect={} message={}", label, Who(actor), static_cast<int>(match),
        text ? text : "-", guid, Text(code), r.count, expect, r.message ? r.message : "");
    return r.count;
}

void StartRound(Where where)
{
    auto hero = PickHero();
    if (!hero) {
        W("ROUND", "where={} 找不到可用的英雄，跳过", WhereName(where));
        g_done[static_cast<int>(where)] = true;
        return;
    }
    Round round;
    round.where = where;
    round.hero = *hero;
    L("ROUND", "where={} hero={} safe_point={} buffs={}", WhereName(where), Who(*hero), g_combat->safe_point() ? 1 : 0,
        g_dd.combat().buff_guids(*hero).size());
    // 速度 = (基础 + 加法项) × 倍率（4C3F20），英雄身上有乘法修正（例如走廊、战斗里的 +25%）时 +2 显示成 +2.5。
    // 所以逐条量增量，按比例核对：两条 +2 的增量相同，规则成立的 +1 是它的一半；u = 增量 / 2 就是倍率（A-S4）
    round.speed0 = Speed(*hero);
    round.gTag = Add(*hero, kSpd, "round", "tag");
    const float s1 = Speed(*hero);
    round.gRule = Add(*hero, kRuleBuff, "rule", "rule");
    const float s2 = Speed(*hero);
    round.gStack = Add(*hero, kSpd, nullptr, "stack");
    const float speed1 = Speed(*hero);
    const float d1 = s1 - round.speed0, d2 = s2 - s1, d3 = speed1 - s2;
    const float unit = d1 / 2.0f;
    const bool speedOk = unit > 0.01f && std::fabs(d3 - d1) < 0.02f && std::fabs(d2 - unit) < 0.02f && CountId(*hero, kSpd) == 2;
    LW(speedOk, "STACK",
        "who={} {} 的条数={}（期望 2：同 id 按游戏语义叠加） 速度 {:g}->{:g}->{:g}->{:g}，逐条增量 {:g}/{:g}/{:g}（期望 2u/u/2u：两条 +2，"
        "规则成立的一条 +1；u={:g} 是速度的倍率，没有乘法修正时为 1）",
        Who(*hero), kSpd, CountId(*hero, kSpd), round.speed0, s1, s2, speed1, d1, d2, d3, unit);
    // effect：城镇里没有 Raid::Current，要被拒绝
    const auto before = g_dd.combat().buff_guids(*hero);
    DDCombatOpResult fx{};
    const auto code = g_dd.combat().effect_apply(kFx, *hero, nullptr, &fx);
    for (auto guid : g_dd.combat().buff_guids(*hero)) {
        if (std::find(before.begin(), before.end(), guid) == before.end()) {
            round.fxGuids.push_back(guid);
        }
    }
    const bool fxOk = where == Where::Town ? code == DD_COMBAT_E_NO_RAID : (code == DD_COMBAT_OK && fx.count >= 1);
    LW(fxOk, "EFFECT", "where={} result={}（{}） count={} new_guids={} message={}", WhereName(where), code, Text(code), fx.count, round.fxGuids.size(),
        fx.message ? fx.message : "");
    // 事件回调里的写操作要排队：buff.add 回调看到这条时在回调里删掉它、再加一条
    g_queueArmed = true;
    g_queueActor = *hero;
    round.gQueueTrigger = Add(*hero, kSpd, "queue", "queue_trigger");
    g_queueArmed = false;
    if (where == Where::Battle) {
        const auto monsters = Monsters();
        for (auto m : monsters) {
            if (auto ref = RefOf(m)) {
                round.monster = *ref;
                Add(*ref, kSpd, "mon", "monster");
                break;
            }
        }
    }
    round.finishAt = g_frame + 90;
    g_round = std::move(round);
}

void FinishRound()
{
    Round& r = *g_round;
    DDRuleStats stats{};
    stats.size = sizeof(stats);
    g_combat->rule_stats(&stats);
    L("RULESTATS", "where={} rule_hits={} stats: rules={} hits={} skipped={} errors={} total_us={:.1f} max_call_us={:.2f} max_frame_us={:.1f} "
                   "max_frame_hits={} rule_entry={}",
        WhereName(r.where), g_ruleHits, stats.rules, stats.hits, stats.skipped, stats.errors, stats.total_us, stats.max_call_us, stats.max_frame_us,
        stats.max_frame_hits, BuffText(r.hero, r.gRule));
    const auto before = g_dd.combat().buff_guids(r.hero).size();
    Remove(r.hero, DD_BUFF_BY_TAG, 0, "round", r.gTag ? 1 : 0, "by_tag");
    Remove(r.hero, DD_BUFF_BY_GUID, r.gStack, nullptr, r.gStack ? 1 : 0, "by_guid");
    Remove(r.hero, DD_BUFF_BY_TAG, 0, "rule", r.gRule ? 1 : 0, "rule");
    Remove(r.hero, DD_BUFF_BY_TAG, 0, "queued_add", g_queuedAddGuid ? 1 : 0, "queued_add");
    Remove(r.hero, DD_BUFF_BY_TAG, 0, "queue", 0, "queue_trigger_already_removed");
    for (auto guid : r.fxGuids) {
        // effect 加的 buff 可能已经按持续时间到期（战斗里回合推进），还在才期望删掉一条
        DDBuffInfo info{};
        const bool present = g_dd.combat().buff_get(r.hero, guid, info);
        Remove(r.hero, DD_BUFF_BY_GUID, guid, nullptr, present ? 1 : 0, "effect_buff");
    }
    if (r.monster) {
        Remove(*r.monster, DD_BUFF_BY_TAG, 0, "mon", 1, "monster");
    }
    const auto left = CountId(r.hero, kSpd) + CountId(r.hero, kRuleBuff);
    const float speed = Speed(r.hero);
    // 战斗中怪物可能在这 90 帧里出手改变速度，只核对测试 buff 删干净了
    const bool ok = left == 0 && (r.where == Where::Battle || std::fabs(speed - r.speed0) < 0.01f);
    LW(ok, "ROUNDDONE", "where={} buffs {}->{} 剩下的测试 buff={} 速度 {:g}（开始时 {:g}）", WhereName(r.where), before, g_dd.combat().buff_guids(r.hero).size(), left,
        speed, r.speed0);
    g_done[static_cast<int>(r.where)] = true;
    g_round.reset();
    g_queuedAddGuid = 0;
}

void RunScenario()
{
    if (!g_active || !g_dd.lifecycle().campaign_active()) {
        return;
    }
    if (g_round) {
        if (g_frame >= g_round->finishAt) {
            FinishRound();
        }
        return;
    }
    const auto raid = Raid();
    Where where = Where::Town;
    if (raid) {
        where = BattleState() != 0 && !Monsters().empty() ? Where::Battle : Where::Corridor;
    }
    const int w = static_cast<int>(where);
    if (w != g_lastWhere) {
        g_lastWhere = w;
        g_stableSince = g_frame;
    }
    if (g_done[w] || g_frame < g_stableSince + 60 || !g_combat->safe_point()) {
        return;
    }
    StartRound(where);
}

// ---- 事件 ----
void LogBuffEvent(DDEvent* ev, const char* tag)
{
    const auto& e = g_dd.events();
    const bool deferred = (e.flags(ev) & DD_EVENT_F_DEFERRED) != 0;
    g_deferred += deferred ? 1 : 0;
    std::string extra;
    if (std::strcmp(tag, "BUFFREMOVE") == 0) {
        extra = std::format(" reason={}", e.get_string(ev, "reason"));
    } else if (std::strcmp(tag, "BUFFTICK") == 0) {
        extra = std::format(" before={} after={}", e.get_int(ev, "duration_before"), e.get_int(ev, "duration_after"));
    }
    L(tag, "kind={} actor_id={} guid={} id={} stat={} amount={:g} duration={} dtype={} source={} rule={} plugin={} tag={} deferred={}{}",
        e.get_int(ev, "actor_kind"), e.get_int(ev, "actor_id"), e.get_int(ev, "guid"), e.get_string(ev, "id"), e.get_int(ev, "stat_type"),
        e.get_float(ev, "amount"), e.get_int(ev, "duration"), e.get_int(ev, "duration_type"), e.get_int(ev, "source"), e.get_int(ev, "rule_type"),
        e.get_string(ev, "plugin"), e.get_string(ev, "tag"), deferred ? 1 : 0, extra);
}

void OnBuffAdd(DDEvent* ev)
{
    ++g_adds;
    const auto& e = g_dd.events();
    const auto guid = static_cast<std::uint32_t>(e.get_int(ev, "guid"));
    if (g_addLogged++ < 60 || std::string(e.get_string(ev, "plugin")) == "test_combat") {
        LogBuffEvent(ev, "BUFFADD");
    }
    if (g_queueArmed && std::string(e.get_string(ev, "tag")) == "queue") {
        g_queueArmed = false;
        g_queueGuid = guid;
        DDCombatOpResult r1{}, r2{};
        const auto remove = g_dd.combat().buff_remove(g_queueActor, DD_BUFF_BY_GUID, guid, 0, nullptr, &r1);
        DDBuffAddOptions options{};
        options.size = sizeof(options);
        options.tag = "queued_add";
        const auto add = g_dd.combat().buff_add(g_queueActor, kSpd, &options, &r2);
        g_queueRemoveOp = r1.op;
        g_queueAddOp = r2.op;
        const bool ok = remove == DD_COMBAT_QUEUED && add == DD_COMBAT_QUEUED && r1.op && r2.op;
        LW(ok, "QUEUE", "回调里：guid={} remove={}（{}） op={} add={}（{}） op={} safe_point={}（期望都排队，下一帧 OPRESULT）", guid, remove,
            Text(remove), r1.op, add, Text(add), r2.op, g_combat->safe_point() ? 1 : 0);
    }
}

void OnBuffRemove(DDEvent* ev)
{
    ++g_removes;
    const auto& e = g_dd.events();
    ++g_reasons[e.get_string(ev, "reason")];
    if (g_removeLogged++ < 60 || std::string(e.get_string(ev, "plugin")) == "test_combat") {
        LogBuffEvent(ev, "BUFFREMOVE");
    }
}

void OnBuffTick(DDEvent* ev)
{
    ++g_ticks;
    if (g_tickLogged++ < 40) {
        LogBuffEvent(ev, "BUFFTICK");
    }
}

// damage.final：每场战斗英雄第一次受伤全部吸收，第二次吸收一半，在 damage.post 核对
int g_heroHits = 0;
struct Expect {
    void* target = nullptr;
    float hpBefore = 0, damage = 0, absorb = 0;
    bool full = false, active = false;
};
Expect g_expect;

void OnDamageFinal(DDEvent* ev)
{
    const auto& e = g_dd.events();
    const bool hero = e.get_bool(ev, "is_hero");
    const double damage = e.get_float(ev, "damage");
    const bool blocked = e.get_bool(ev, "blocked");
    double absorb = 0;
    if (g_active && hero && !blocked && damage > 1.0 && !e.get_bool(ev, "deaths_door") && g_heroHits < 2) {
        ++g_heroHits;
        absorb = g_heroHits == 1 ? damage : std::floor(damage / 2);
        e.set_float(ev, "absorb", absorb);
        g_expect = { e.get_pointer(ev, "target"), static_cast<float>(e.get_float(ev, "hp_before")), static_cast<float>(damage), static_cast<float>(absorb),
            g_heroHits == 1, true };
    }
    L("DMGFINAL", "hero={} damage={:g} type={} blocked={} deaths_door={} hp={:g} attacker={} absorb={:g}", hero ? 1 : 0, damage, e.get_int(ev, "damage_type"),
        blocked ? 1 : 0, e.get_bool(ev, "deaths_door") ? 1 : 0, e.get_float(ev, "hp_before"), e.get_pointer(ev, "attacker") ? 1 : 0, absorb);
}

void OnDamagePost(DDEvent* ev)
{
    const auto& e = g_dd.events();
    if (!g_expect.active || e.get_pointer(ev, "target") != g_expect.target) {
        return;
    }
    g_expect.active = false;
    const double after = e.get_float(ev, "hp_after");
    const double loss = g_expect.hpBefore - after;
    const double want = g_expect.full ? 0.0 : g_expect.damage - g_expect.absorb;
    const bool ok = std::fabs(loss - want) < 0.01;
    LW(ok, "DMGCHECK", "kind={} damage={:g} absorb={:g} hp_before={:g} hp_after={:g} loss={:g} expect={:g}", g_expect.full ? "full(应显示 Blocked)" : "partial",
        g_expect.damage, g_expect.absorb, g_expect.hpBefore, after, loss, want);
}

void OnSkill(DDEvent* ev)
{
    if (g_skillLogged >= 30) {
        return;
    }
    ++g_skillLogged;
    const auto& e = g_dd.events();
    DDSkillRef ref{};
    ref.size = sizeof(ref);
    DDSkillInfo info{};
    info.size = sizeof(info);
    const bool got = g_combat->skill_from_pointer(e.get_pointer(ev, "performer"), e.get_pointer(ev, "skill"), &ref);
    const bool read = got && g_combat->skill_info(&ref, &info);
    std::string effects;
    for (std::uint32_t i = 0; i < info.effect_count; ++i) {
        effects += (i ? "," : "") + std::string(info.effects[i]);
    }
    L("SKILL", "handle={} who={} id={} level={} info={} type={} friendly={} effects=[{}]", got ? 1 : 0, got ? Who(ref.actor) : "-", ref.id, ref.level,
        read ? 1 : 0, info.type, info.friendly ? 1 : 0, effects);
}

void OnEffectPre(DDEvent* ev)
{
    if (g_effectLogged++ < 30) {
        L("EFFECTPRE", "effect_name={} source={}", g_dd.events().get_string(ev, "effect_name"), g_dd.events().get_int(ev, "source"));
    }
}

void OnCampaignLoad(const DDPhaseEvent& event)
{
    g_session = event.campaign_serial;
    std::fill(std::begin(g_done), std::end(g_done), false);
    g_round.reset();
    g_lastWhere = -1;
    g_heroHits = 0;
    L("CAMPAIGN", "phase=load serial={} late={} safe_point={}", event.campaign_serial, event.late, g_combat->safe_point() ? 1 : 0);
    if (!g_active) {
        return;
    }
    DefineAll();
    const DDRuleId rule = g_dd.combat().service()->rule_register(g_dd.plugin(), kRule, &RuleCallback, nullptr);
    L("RULE", "register {}={}（再进存档时已经注册过，返回 0 是预期）", kRule, rule);
}

void OnCampaignUnload(const DDPhaseEvent&)
{
    std::string reasons;
    for (const auto& [k, v] : g_reasons) {
        reasons += std::format(" {}={}", k, v);
    }
    L("SUMMARY", "session={} adds={} removes={} ticks={} deferred={} rule_hits={} reasons:{}", g_session, g_adds, g_removes, g_ticks, g_deferred,
        g_ruleHits, reasons);
    g_round.reset();
}

void ReadConfig()
{
    const std::string path = std::string(g_dd.game()->game_root()) + "\\DDModLoader\\cfg\\test_combat.cfg";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "active", 6) == 0 && std::strchr(line, '0')) {
            g_active = false;
        }
    }
    std::fclose(f);
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "test_combat";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api)) {
        return false;
    }
    g_combat = g_dd.combat().service();
    g_game = static_cast<const DDGameService*>(api->query(DD_SERVICE_GAME, DD_SERVICE_GAME_VERSION));
    if (!g_combat || !g_game || !g_dd.events()) {
        g_dd.log().error("combat / game / events 服务不可用");
        return false;
    }
    ReadConfig();
    g_campaignGlobal = ScanGlobal("48 83 EC 38 80 3D ?? ?? ?? ?? 00 48 8B 05 ?? ?? ?? ?? 74 ?? 48 85 C0 75", 14);
    void* statFn = nullptr;
    if (g_dd.hooks().scan("48 89 5C 24 08 48 89 7C 24 10 4C 8B 59 30 44 8B D2 48 8B 51 38 45 33 C9", &statFn)) {
        g_statFn = reinterpret_cast<StatFn>(statFn);
    }
    g_raidGlobal = ScanGlobal("48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 88 C0 52 00 00 48 85 C9 74 ?? 48 8D 15 ?? ?? ?? ?? E8", 3);
    g_dd.log().info("test_combat 已加载：active={} Campaign::Current={} Raid::Current={}", g_active ? 1 : 0, g_campaignGlobal ? "找到" : "没找到",
        g_raidGlobal ? "找到" : "没找到");
    g_dd.combat().on_result(&OnOpResult);
    g_dd.lifecycle().subscribe(DD_PHASE_TICK, [](const DDPhaseEvent& event) {
        g_frame = event.frame;
        if (g_selfAt == 0) {
            g_selfAt = g_frame + 60;  // 主菜单稳定一会儿再自检
        }
        SelfCheck();
        if (g_selfStep >= 4) {
            RunScenario();
        }
    });
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_LOAD, [](const DDPhaseEvent& event) {
        OnCampaignLoad(event);
        // 场景里的订阅按存档作用域，退出存档时由 DDCore 撤销
        const auto& e = g_dd.events();
        e.on("buff.add", &OnBuffAdd, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("buff.remove", &OnBuffRemove, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("buff.tick", &OnBuffTick, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("damage.final", &OnDamageFinal, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("damage.post", &OnDamagePost, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("skill.pre", &OnSkill, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("effect.pre", &OnEffectPre, 0, DD_EVENT_SCOPE_CAMPAIGN);
        e.on("battle.start", [](DDEvent*) { g_heroHits = 0; }, 0, DD_EVENT_SCOPE_CAMPAIGN);
    });
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_UNLOAD, &OnCampaignUnload);
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}

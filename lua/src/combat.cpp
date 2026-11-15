#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "combat.h"

#include "actor.h"
#include "events.h"
#include "sandbox.h"
#include "session.h"
#include "state_json.h"
#include "vm.h"

#include <combat/logic.h>
#include <lauxlib.h>
#include <lddstep.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <unordered_map>

namespace ddlua {

namespace {

constexpr const char* kEffectMeta = "dd.effect";
constexpr const char* kSkillMeta = "dd.skill";
constexpr const char* kRuleMeta = "dd.rule";
char kEffectIntern = 0;  // 注册表里 effect 句柄驻留表的键
constexpr std::size_t kTextMax = 63;
constexpr std::size_t kEffectTextMax = 64 * 1024;
// 一个 Lua Mod 最多同时排队的写操作。DDCore 给每个插件 256 个，所有 Lua Mod 共用 DDLua 的这份，一个 Mod 不能占满
constexpr std::size_t kMaxPendingPerMod = 128;
constexpr const char* kPluginName = "DDLua";  // DDCore 记下的插件名（buff.* 的 plugin 字段）

enum Cb { kApply, kRemove, kTick, kDamage, kCbCount };
const char* const kCbNames[kCbCount] = { "on_apply", "on_remove", "on_tick", "on_damage" };

// 排队的操作号 -> 绑定（操作回调按它找回会话）；规则的键 -> 绑定。表里查不到的结果和回调直接忽略
std::unordered_map<std::uint64_t, CombatBinding*>& OpRoutes()
{
    static auto* routes = new std::unordered_map<std::uint64_t, CombatBinding*>();  // 不析构
    return *routes;
}

std::unordered_map<std::uint64_t, CombatBinding*>& RuleRoutes()
{
    static auto* routes = new std::unordered_map<std::uint64_t, CombatBinding*>();
    return *routes;
}

std::uint64_t g_nextRuleKey = 1;

bool ValidText(std::string_view text, bool allowEmpty)
{
    if (text.empty()) {
        return allowEmpty;
    }
    if (text.size() > kTextMax) {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '"') {
            return false;
        }
    }
    return true;
}

std::string Fixed(const char* field)
{
    return std::string(field, strnlen(field, DD_COMBAT_TEXT));
}

void CopyText(char (&dst)[DD_COMBAT_TEXT], std::string_view src)
{
    const std::size_t n = (std::min)(src.size(), static_cast<std::size_t>(DD_COMBAT_TEXT - 1));
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

// effect 文本里每个 effect: 块游戏会用的 .name：和 DDCore 是同一份代码（combat/logic.cpp，照游戏解析器的分块和取字段规则），
// 会让游戏崩溃的文本（块达到 1024 字节等）在这里就报错
bool EffectNames(std::string_view text, std::vector<std::string>* names, std::string* why)
{
    return ddc::combat::ExtractEffectNames(text, names, why);
}

struct EffectUd {
    char name[DD_COMBAT_TEXT];
};

struct SkillUd {
    DDSkillRef ref;
};

struct RuleUd {
    std::uint64_t key;
};

}  // namespace

// ============================================================================

struct CombatBinding::Impl {
    Session& session;
    CombatBinding& self;

    struct ScriptDef {
        std::string id;
        std::uint64_t modToken = 0;
        std::string modId;
        int ref[kCbCount] = { LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF };
        std::uint32_t failures[kCbCount] = {};
        bool Script() const { return ref[kApply] != LUA_NOREF || ref[kRemove] != LUA_NOREF || ref[kTick] != LUA_NOREF || ref[kDamage] != LUA_NOREF; }
    };
    std::map<std::string, ScriptDef> defs;  // 本会话里 Lua Mod 定义的 buff id（含没有回调的，记归属）

    struct StateKey {
        std::uint32_t kind, id, campaign, raid, battle, roster, guid;
        bool operator<(const StateKey& o) const
        {
            return std::tie(kind, id, campaign, raid, battle, roster, guid) < std::tie(o.kind, o.id, o.campaign, o.raid, o.battle, o.roster, o.guid);
        }
    };
    struct Binding {
        DDActorRef actor{};
        std::uint32_t guid = 0;
        std::string buffId;
        std::uint64_t modToken = 0;
        std::string modId;
        int stateRef = LUA_NOREF;
    };
    std::map<StateKey, Binding> states;

    struct PendingOp {
        std::uint64_t op = 0;
        std::uint32_t kind = 0;
        std::uint64_t modToken = 0;
        std::string modId;
        int doneRef = LUA_NOREF;
        DDActorRef actor{};
        std::string id;                       // buff_add 的 id、buff_define 的 id
        int stateRef = LUA_NOREF;             // buff_add 的初始状态
        std::vector<std::string> effectNames; // effect_define
        bool created = false;                 // buff_define：这次才建立定义槽（失败时撤销登记；更新已有定义失败时保留原来的回调）
    };
    std::map<std::uint64_t, PendingOp> pending;

    struct Rule {
        std::uint64_t key = 0;
        std::uint64_t modToken = 0;
        std::string modId;
        std::string name;
        int fnRef = LUA_NOREF;
        DDRuleId id = 0;
        std::uint32_t failures = 0;
        bool disabled = false;
    };
    std::map<std::uint64_t, Rule> rules;

    struct Owner {
        std::uint64_t modToken = 0;
        std::string modId;
    };
    std::map<std::uint32_t, Owner> effectOwners;  // 本会话里 Lua Mod 定义的 effect：名字的游戏哈希（游戏的 effect map 按哈希存）

    // 立即执行的 add_buff 正在进行（同步派发的 buff.add 用它认出自己加的条目，把初始状态绑上去）
    struct PendingAdd {
        DDActorRef actor{};
        std::string id;
        std::string tag;
        int stateRef = LUA_NOREF;
        bool consumed = false;
        std::uint32_t guid = 0;
    };
    PendingAdd* pendingAdd = nullptr;

    std::uint64_t subAdd = 0, subRemove = 0, subTick = 0, subDamage = 0;
    bool subWarned = false;

    // C-Y7：每个 Mod 每帧的 combat 回调（规则、脚本 Buff 的回调、done）的总时间。规则按判定次数调用、脚本 Buff 按载体条数调用，
    // 单次预算管不住总量；超过 combat_frame_ms 后这一帧余下的规则交还原函数、脚本 Buff 的回调跳过。Tick（帧边界）清零
    struct FrameUse {
        double us = 0.0;
        std::uint64_t skipped = 0;     // 本帧跳过的调用
        std::uint64_t skippedTotal = 0;
        ULONGLONG lastWarn = 0;
    };
    std::unordered_map<std::uint64_t, FrameUse> frameUse;
    std::unordered_map<std::uint64_t, ULONGLONG> stateLimitWarn;  // 状态条数到上限的 WARN（每个 Mod 最多 10 秒一次）

    Impl(Session& s, CombatBinding& b) : session(s), self(b) {}

    static double QpcUs()
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

    // 这个 Mod 本帧的 combat 回调时间用完了：记一次跳过，返回 true
    bool OverFrame(std::uint64_t modToken, const std::string& modId, const char* what)
    {
        FrameUse& use = frameUse[modToken];
        if (use.us < static_cast<double>(session.config().combatFrameMs) * 1000.0) {
            return false;
        }
        ++use.skipped;
        ++use.skippedTotal;
        const ULONGLONG now = GetTickCount64();
        if (use.lastWarn == 0 || now - use.lastWarn >= 10000) {
            use.lastWarn = now;
            session.LogId(DD_LOG_WARN, modId, std::string("这一帧的 combat 回调（规则、脚本 Buff）已经用了 ") +
                std::to_string(static_cast<long long>(use.us / 1000.0)) + " 毫秒，超过 combat_frame_ms（" +
                std::to_string(session.config().combatFrameMs) + "），这一帧余下的" + what + "（规则交还游戏原来的判定，脚本 Buff 的回调跳过）；"
                "到目前共跳过 " + std::to_string(use.skippedTotal) + " 次");
        }
        return true;
    }

    void ChargeFrame(std::uint64_t modToken, double us) { frameUse[modToken].us += us; }

    std::size_t StatesOfMod(std::uint64_t modToken) const
    {
        return static_cast<std::size_t>(std::count_if(states.begin(), states.end(), [&](const auto& item) { return item.second.modToken == modToken; }));
    }

    const DDCombatService* Svc() const { return combat::Service(session.host()); }
    Vm* VmOk() const
    {
        Vm* vm = session.vm();
        return vm && vm->ok() ? vm : nullptr;
    }
    void Unref(int& ref)
    {
        if (ref != LUA_NOREF) {
            if (Vm* vm = VmOk()) {
                luaL_unref(vm->L(), LUA_REGISTRYINDEX, ref);  // 注册表里已有的键，不分配、不抛错
            }
            ref = LUA_NOREF;
        }
    }
    static StateKey Key(const DDActorRef& a, std::uint32_t guid)
    {
        return { a.kind, a.id, a.campaign_serial, a.raid_serial, a.battle_serial, a.roster, guid };
    }
    ScriptDef* FindScript(const std::string& id)
    {
        const auto it = defs.find(id);
        return it != defs.end() && it->second.Script() ? &it->second : nullptr;
    }
    bool ModOfSession(const std::string& id) const
    {
        for (const auto& mod : session.mods()) {
            if (mod->id() == id && (mod->state == ModState::Loaded || mod->state == ModState::Loading)) {
                return true;
            }
        }
        return false;
    }

    // ---- 内部订阅 ----
    void Ensure(std::uint64_t& key, const char* topic, bool want, void (Impl::*handler)(DDEvent*))
    {
        if (!session.HasRuntime()) {
            return;
        }
        EventBinding& events = session.events();
        if (want && !(key && events.Active(key))) {
            std::string reason;
            key = events.SubscribeNative(topic, [this, handler](DDEvent* ev) { (this->*handler)(ev); }, &reason);
            if (!key && !subWarned) {
                subWarned = true;
                session.Log(DD_LOG_WARN, nullptr, std::string("脚本 Buff 需要订阅 ") + topic + "，订阅失败：" + reason + "（回调不会被调用）");
            }
        } else if (!want && key) {
            events.Remove(key, true);
            key = 0;
        }
    }

    void UpdateSubs()
    {
        bool any = false, tick = false, damage = false;
        for (const auto& [id, def] : defs) {
            any = any || def.Script();
            tick = tick || def.ref[kTick] != LUA_NOREF;
            damage = damage || def.ref[kDamage] != LUA_NOREF;
        }
        Ensure(subAdd, "buff.add", any, &Impl::OnBuffAdd);
        Ensure(subRemove, "buff.remove", any, &Impl::OnBuffRemove);
        Ensure(subTick, "buff.tick", tick, &Impl::OnBuffTick);
        Ensure(subDamage, "damage.pre", damage, &Impl::OnDamagePre);
    }

    // ---- 状态 ----
    // 绑定一条载体的状态。stateRef 为 LUA_NOREF 时新建空表。成功返回 true（stateRef 交给绑定）；失败时 stateRef 仍归调用方
    bool Bind(const DDActorRef& actor, std::uint32_t guid, const ScriptDef& def, int stateRef)
    {
        Vm* vm = VmOk();
        if (!vm) {
            return false;
        }
        // C-Y7：每个 Mod 最多 max_per_mod 条载体有状态（回调按载体条数调用，随存档编码的也是这些）
        if (!states.count(Key(actor, guid)) && StatesOfMod(def.modToken) >= session.config().maxPerMod) {
            const ULONGLONG now = GetTickCount64();
            ULONGLONG& last = stateLimitWarn[def.modToken];
            if (last == 0 || now - last >= 10000) {
                last = now;
                session.LogId(DD_LOG_WARN, def.modId, "脚本 Buff 的状态达到上限 " + std::to_string(session.config().maxPerMod) +
                    " 条（DDLua.cfg 的 max_per_mod），" + def.id + " 的这条载体（guid " + std::to_string(guid) + "）不绑定状态、不调用回调");
            }
            return false;
        }
        if (stateRef == LUA_NOREF) {
            std::string error;
            if (!vm->Protected([&](lua_State* L) {
                    lua_newtable(L);
                    stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
                },
                    &error)) {
                session.LogId(DD_LOG_ERROR, def.modId, "脚本 Buff " + def.id + " 的状态表没能创建：" + error);
                return false;
            }
        }
        Binding& slot = states[Key(actor, guid)];
        Unref(slot.stateRef);
        slot.actor = actor;
        slot.guid = guid;
        slot.buffId = def.id;
        slot.modToken = def.modToken;
        slot.modId = def.modId;
        slot.stateRef = stateRef;
        return true;
    }

    // 载体的状态；没有绑定而且 create 时（读档恢复的载体第一次用到）绑一张空表
    int StateOf(const DDActorRef& actor, std::uint32_t guid, const std::string& id, bool create)
    {
        const auto it = states.find(Key(actor, guid));
        if (it != states.end()) {
            return it->second.stateRef;
        }
        ScriptDef* def = create ? FindScript(id) : nullptr;
        if (!def || !Bind(actor, guid, *def, LUA_NOREF)) {
            return LUA_NOREF;
        }
        return states[Key(actor, guid)].stateRef;
    }

    void Drop(const StateKey& key)
    {
        const auto it = states.find(key);
        if (it != states.end()) {
            Unref(it->second.stateRef);
            states.erase(it);
        }
    }

    // ---- 调用 Lua ----
    // 调用一个函数：pushArgs 压参数并返回个数（在保护模式里）。mod 代表执行；返回调用是否成功，budget 写是否超预算
    bool Invoke(int fnRef, std::uint64_t modToken, const std::string& modId, std::uint32_t budgetMs, const std::string& what,
        const std::function<int(lua_State*)>& pushArgs, bool* budget, std::string* errorOut)
    {
        *budget = false;
        Vm* vm = session.vm();
        if (!vm || !session.CheckVmHealth(false) || fnRef == LUA_NOREF) {
            return false;
        }
        const Mod* mod = session.FindToken(modToken);
        if (!mod) {
            return false;
        }
        lua_State* L = vm->L();
        const int top = lua_gettop(L);
        int nargs = 0;
        std::string error;
        if (!vm->Protected(
                [&](lua_State* L) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
                    nargs = pushArgs(L);
                },
                &error)) {
            session.LogId(DD_LOG_ERROR, modId, what + " 没能开始：" + error);
            *errorOut = error;
            return false;
        }
        bool ok;
        {
            const Session::Running running(session, mod);
            const double t0 = QpcUs();
            ok = vm->Call(nargs, 0, budgetMs, &error, budget);
            ChargeFrame(modToken, QpcUs() - t0);
        }
        if (!vm->ok()) {
            return false;
        }
        lua_settop(L, top);
        *errorOut = error;
        return ok;
    }

    // 脚本 Buff 的回调。失败计数：连续 max_failures 次或超预算时停用这个回调
    void CallScript(const std::string& id, Cb cb, const std::function<int(lua_State*)>& pushArgs)
    {
        ScriptDef* def = FindScript(id);
        if (!def || def->ref[cb] == LUA_NOREF) {
            return;
        }
        const std::string modId = def->modId;
        if (OverFrame(def->modToken, modId, "脚本 Buff 回调跳过")) {
            return;
        }
        const std::string what = "脚本 Buff " + id + " 的 " + kCbNames[cb];
        bool budget = false;
        std::string error;
        const bool ok = Invoke(def->ref[cb], def->modToken, modId, session.config().budgetMs, what, pushArgs, &budget, &error);
        def = FindScript(id);  // 回调里可能重新定义了它
        if (!def) {
            return;
        }
        if (ok) {
            def->failures[cb] = 0;
            return;
        }
        if (error.empty()) {
            return;  // 没有调用（VM 损坏、Mod 已拆除）
        }
        const std::uint32_t limit = session.config().maxFailures;
        ++def->failures[cb];
        std::string text = what + " 出错";
        if (budget || def->failures[cb] >= limit) {
            text += budget ? "（超出执行预算），已停用这个回调" : "（连续 " + std::to_string(def->failures[cb]) + " 次），已停用这个回调";
            Unref(def->ref[cb]);
            UpdateSubs();
        } else {
            text += "（连续第 " + std::to_string(def->failures[cb]) + " 次，" + std::to_string(limit) + " 次后停用）";
        }
        session.LogId(DD_LOG_ERROR, modId, text + "：" + error);
    }

    // 回调的 buff 参数：能读到条目时用快照，读不到（已经删除、actor 不能解析）时用 fallback 里的字段
    struct Fallback {
        std::string id;
        std::int32_t duration = 0;
    };
    void PushCallbackBuff(lua_State* L, const DDActorRef& actor, std::uint32_t guid, const Fallback& fb);

    // ---- 事件 ----
    bool EventActor(DDEvent* ev, DDActorRef* out)
    {
        const DDEventsService& events = *session.host().events;
        const DDGameService* game = actor::Service(session.host());
        if (!game) {
            return false;
        }
        out->size = sizeof(DDActorRef);
        void* pointer = nullptr;
        if (events.get_pointer(ev, "actor", &pointer) == DD_EV_OK && pointer) {
            return game->actor_from_pointer(pointer, out);
        }
        std::int64_t kind = 0, id = 0;
        return events.get_int(ev, "actor_kind", &kind) == DD_EV_OK && events.get_int(ev, "actor_id", &id) == DD_EV_OK &&
            game->actor_find(static_cast<std::uint32_t>(kind), static_cast<std::uint32_t>(id), out);
    }

    static std::string Str(const DDEventsService& e, DDEvent* ev, const char* field)
    {
        const char* value = nullptr;
        return e.get_string(ev, field, &value) == DD_EV_OK && value ? value : "";
    }
    static std::int64_t Int(const DDEventsService& e, DDEvent* ev, const char* field)
    {
        std::int64_t value = 0;
        e.get_int(ev, field, &value);
        return value;
    }

    void OnBuffAdd(DDEvent* ev)
    {
        const DDEventsService& e = *session.host().events;
        const std::string id = Str(e, ev, "id");
        ScriptDef* def = FindScript(id);
        if (!def) {
            return;
        }
        DDActorRef actor{};
        if (!EventActor(ev, &actor)) {
            return;
        }
        const auto guid = static_cast<std::uint32_t>(Int(e, ev, "guid"));
        const Fallback fb{ id, static_cast<std::int32_t>(Int(e, ev, "duration")) };
        if (Str(e, ev, "plugin") == kPluginName) {
            const std::string tag = Str(e, ev, "tag");
            if (pendingAdd && !pendingAdd->consumed && pendingAdd->id == id && pendingAdd->tag == tag && actor::Same(pendingAdd->actor, actor)) {
                pendingAdd->consumed = true;
                pendingAdd->guid = guid;
                int stateRef = pendingAdd->stateRef;
                pendingAdd->stateRef = LUA_NOREF;  // 交给绑定
                if (Bind(actor, guid, *def, stateRef)) {
                    CallApply(actor, guid, fb);
                } else {
                    Unref(stateRef);
                }
                return;
            }
            // 本会话的 Mod 排队的添加：操作结果随后就到（DDCore 执行一个操作后立即通知），在那里绑定初始状态
            const std::size_t colon = tag.find(':');
            if (colon != std::string::npos && ModOfSession(tag.substr(0, colon))) {
                return;
            }
        }
        if (states.count(Key(actor, guid))) {
            return;
        }
        if (Bind(actor, guid, *def, LUA_NOREF)) {
            CallApply(actor, guid, fb);
        }
    }

    void CallApply(const DDActorRef& actor, std::uint32_t guid, const Fallback& fb)
    {
        CallScript(fb.id, kApply, [&](lua_State* L) {
            PushCallbackBuff(L, actor, guid, fb);
            const int ref = StateOf(actor, guid, fb.id, false);
            ref == LUA_NOREF ? lua_pushnil(L) : (void)lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return 2;
        });
    }

    void OnBuffRemove(DDEvent* ev)
    {
        const DDEventsService& e = *session.host().events;
        const std::string id = Str(e, ev, "id");
        if (!defs.count(id)) {
            return;
        }
        DDActorRef actor{};
        if (!EventActor(ev, &actor)) {
            return;
        }
        const auto guid = static_cast<std::uint32_t>(Int(e, ev, "guid"));
        const std::string reason = Str(e, ev, "reason");
        RemoveState(actor, guid, { id, static_cast<std::int32_t>(Int(e, ev, "duration")) }, reason.empty() ? "other" : reason);
    }

    // 调用 on_remove（有的话）并丢弃状态
    void RemoveState(const DDActorRef& actor, std::uint32_t guid, const Fallback& fb, const std::string& reason)
    {
        ScriptDef* def = FindScript(fb.id);
        if (def && def->ref[kRemove] != LUA_NOREF && StateOf(actor, guid, fb.id, true) != LUA_NOREF) {
            CallScript(fb.id, kRemove, [&](lua_State* L) {
                PushCallbackBuff(L, actor, guid, fb);
                const int ref = StateOf(actor, guid, fb.id, false);
                ref == LUA_NOREF ? lua_pushnil(L) : (void)lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
                lua_pushlstring(L, reason.data(), reason.size());
                return 3;
            });
        }
        Drop(Key(actor, guid));
    }

    void OnBuffTick(DDEvent* ev)
    {
        const DDEventsService& e = *session.host().events;
        const std::string id = Str(e, ev, "id");
        ScriptDef* def = FindScript(id);
        if (!def || def->ref[kTick] == LUA_NOREF) {
            return;
        }
        DDActorRef actor{};
        if (!EventActor(ev, &actor)) {
            return;
        }
        const auto guid = static_cast<std::uint32_t>(Int(e, ev, "guid"));
        const auto before = Int(e, ev, "duration_before");
        const auto after = Int(e, ev, "duration_after");
        const Fallback fb{ id, static_cast<std::int32_t>(after) };
        if (StateOf(actor, guid, id, true) == LUA_NOREF) {
            return;  // 状态绑不上（达到上限、VM 不可用）：不调用
        }
        CallScript(id, kTick, [&](lua_State* L) {
            PushCallbackBuff(L, actor, guid, fb);
            const int ref = StateOf(actor, guid, id, false);
            ref == LUA_NOREF ? lua_pushnil(L) : (void)lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(L, static_cast<lua_Integer>(before));
            lua_pushinteger(L, static_cast<lua_Integer>(after));
            return 4;
        });
    }

    // damage.pre：目标身上每条带 on_damage 的载体调用一次，ev 可写（可以 cancel）
    void OnDamagePre(DDEvent* ev)
    {
        const DDEventsService& e = *session.host().events;
        const DDCombatService* svc = Svc();
        const DDGameService* game = actor::Service(session.host());
        void* target = nullptr;
        DDActorRef actor{};
        actor.size = sizeof(actor);
        if (!svc || !game || e.get_pointer(ev, "target", &target) != DD_EV_OK || !target || !game->actor_from_pointer(target, &actor)) {
            return;
        }
        std::uint32_t count = 0;
        if (!svc->buff_list(&actor, nullptr, 0, &count) || count == 0) {
            return;
        }
        std::vector<std::uint32_t> guids(count);
        svc->buff_list(&actor, guids.data(), count, &count);
        guids.resize((std::min)(static_cast<std::size_t>(count), guids.size()));
        struct Hit {
            std::uint32_t guid;
            std::string id;
            std::int32_t duration;
        };
        std::vector<Hit> hits;
        for (const auto guid : guids) {
            DDBuffInfo info{};
            info.size = sizeof(info);
            if (!svc->buff_get(&actor, guid, &info)) {
                continue;
            }
            const std::string id = Fixed(info.id);
            const ScriptDef* def = FindScript(id);
            if (def && def->ref[kDamage] != LUA_NOREF) {
                hits.push_back({ guid, id, info.duration });
            }
        }
        if (hits.empty()) {
            return;
        }
        Vm* vm = session.vm();
        if (!vm || !session.CheckVmHealth(false)) {
            return;
        }
        std::string reason;
        const TopicMeta* topic = session.events().Meta("damage.pre", &reason);
        if (!topic) {
            return;
        }
        // 负载 userdata 用注册表引用锚住，所有回调共用，结束后失效（保护模式的跳板有自己的栈帧，不能按外面的栈下标取）
        void* payload = nullptr;
        int payloadRef = LUA_NOREF;
        if (!vm->Protected(
                [&](lua_State* L) {
                    payload = EventBinding::PushPayload(L, ev, topic);
                    payloadRef = luaL_ref(L, LUA_REGISTRYINDEX);
                },
                &reason)) {
            return;
        }
        for (const auto& hit : hits) {
            const Fallback fb{ hit.id, hit.duration };
            if (StateOf(actor, hit.guid, hit.id, true) == LUA_NOREF) {
                continue;
            }
            CallScript(hit.id, kDamage, [&](lua_State* L) {
                PushCallbackBuff(L, actor, hit.guid, fb);
                const int ref = StateOf(actor, hit.guid, hit.id, false);
                ref == LUA_NOREF ? lua_pushnil(L) : (void)lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
                lua_rawgeti(L, LUA_REGISTRYINDEX, payloadRef);
                return 3;
            });
            if (!vm->ok()) {
                return;
            }
        }
        EventBinding::ExpirePayload(payload);
        Unref(payloadRef);
    }

    // ---- 清理 ----
    // 不能解析的 actor 上的状态：调用 on_remove(..., "gone") 并丢弃。只在会话进行中每秒的清理里调用，拆除时不调用（F7）
    void SweepGone()
    {
        std::vector<Binding> gone;
        for (const auto& [key, binding] : states) {
            if (!actor::Valid(session.host(), binding.actor)) {
                gone.push_back(binding);
            }
        }
        for (const auto& b : gone) {
            RemoveState(b.actor, b.guid, { b.buffId, 0 }, "gone");
        }
        if (!gone.empty()) {
            session.Log(DD_LOG_DEBUG, nullptr, "脚本 Buff：" + std::to_string(gone.size()) + " 条载体的 actor 不再能解析，状态已丢弃（on_remove 的原因 gone）");
        }
    }

    void Clear(bool touchLua)
    {
        for (auto& [key, rule] : rules) {
            if (rule.id) {
                if (const DDCombatService* svc = Svc()) {
                    svc->rule_unregister(session.host().plugin, rule.id);
                }
            }
            RuleRoutes().erase(rule.key);
            if (touchLua) {
                Unref(rule.fnRef);
            }
        }
        rules.clear();
        for (auto& [op, p] : pending) {
            if (touchLua) {
                Unref(p.doneRef);
                Unref(p.stateRef);
            }
        }
        pending.clear();
        for (auto it = OpRoutes().begin(); it != OpRoutes().end();) {
            it = it->second == &self ? OpRoutes().erase(it) : std::next(it);
        }
        for (auto& [key, b] : states) {
            if (touchLua) {
                Unref(b.stateRef);
            }
        }
        states.clear();
        for (auto& [id, def] : defs) {
            for (int& ref : def.ref) {
                if (touchLua) {
                    Unref(ref);
                }
            }
        }
        defs.clear();
        effectOwners.clear();
        frameUse.clear();
        stateLimitWarn.clear();
        if (session.HasEvents()) {
            for (std::uint64_t* key : { &subAdd, &subRemove, &subTick, &subDamage }) {
                if (*key) {
                    session.events().Remove(*key, true);
                    *key = 0;
                }
            }
        }
    }
};

namespace {

// ---- 共用 ----

CombatBinding::Impl& B(lua_State* L)
{
    return SessionOf(L).combat().impl();
}

const DDCombatService& Svc(lua_State* L)
{
    const DDCombatService* svc = combat::Service(SessionOf(L).host());
    if (!svc) {
        luaL_error(L, "combat 服务不可用（DDCore 没有提供，或事件系统没有初始化）");
    }
    return *svc;
}

// actor 方法的调用方：正在代表执行的 Mod（Session::CurrentMod）
Mod& CallerMod(lua_State* L, const char* what)
{
    Session& session = SessionOf(L);
    const Mod* current = session.CurrentMod();
    Mod* mod = current ? session.FindToken(current->token) : nullptr;
    if (!mod) {
        luaL_error(L, "%s：找不到正在执行的 Mod", what);
    }
    return *mod;
}

// 写操作和注册类函数的公共检查：规则回调里只读；Mod 拆除中不能再写
void CheckWrite(lua_State* L, Mod& mod, const char* what)
{
    if (SessionOf(L).InRule()) {
        luaL_error(L, "%s：规则回调里只读，不能调用", what);
    }
    if (mod.tearingDown) {
        luaL_error(L, "%s：Mod %s 正在拆除", what, mod.id().c_str());
    }
}

std::string TextArg(lua_State* L, int index, const char* what, const char* field, bool allowEmpty)
{
    if (lua_type(L, index) != LUA_TSTRING) {
        luaL_error(L, "%s 的 %s 必须是字符串，给的是 %s", what, field, luaL_typename(L, index));
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    const std::string_view value(text, length);
    if (!ValidText(value, allowEmpty) || value.find('\0') != std::string_view::npos) {
        luaL_error(L, "%s 的 %s 无效：%s，最多 63 字节，不含控制字符和引号", what, field, allowEmpty ? "" : "不能为空");
    }
    return std::string(value);
}

lua_Integer IntArg(lua_State* L, int index, const char* what, const char* field, lua_Integer lo, lua_Integer hi)
{
    int isInteger = 0;
    const lua_Integer value = lua_type(L, index) == LUA_TNUMBER ? lua_tointegerx(L, index, &isInteger) : 0;
    if (!isInteger || value < lo || value > hi) {
        luaL_error(L, "%s 的 %s 必须是 %I 到 %I 之间的整数", what, field, static_cast<LUAI_UACINT>(lo), static_cast<LUAI_UACINT>(hi));
    }
    return value;
}

double NumberArg(lua_State* L, int index, const char* what, const char* field)
{
    if (lua_type(L, index) != LUA_TNUMBER) {
        luaL_error(L, "%s 的 %s 必须是数字", what, field);
    }
    const double value = static_cast<double>(lua_tonumber(L, index));
    if (!std::isfinite(value) || std::fabs(value) > 3.4e38) {
        luaL_error(L, "%s 的 %s 必须是有限的 32 位浮点数", what, field);
    }
    return value;
}

bool BoolArg(lua_State* L, int index, const char* what, const char* field)
{
    if (lua_type(L, index) != LUA_TBOOLEAN) {
        luaL_error(L, "%s 的 %s 必须是布尔", what, field);
    }
    return lua_toboolean(L, index) != 0;
}

const char* TableName(std::uint32_t table)
{
    switch (table) {
    case DD_ENUM_STAT:
        return "stat";
    case DD_ENUM_SOURCE:
        return "source";
    case DD_ENUM_DURATION:
        return "duration_type";
    default:
        return "rule";
    }
}

// 枚举参数：名字（字符串）或数值（整数）
std::int32_t EnumArg(lua_State* L, int index, std::uint32_t table, const char* what, const char* field)
{
    const DDCombatService& svc = Svc(L);
    if (lua_type(L, index) == LUA_TSTRING) {
        const char* name = lua_tostring(L, index);
        const std::int32_t value = svc.enum_value(table, name);
        if (value < 0) {
            luaL_error(L, "%s 的 %s 不认识：%s（名字见 dd.buff.enum(\"%s\")）", what, field, name, TableName(table));
        }
        return value;
    }
    const lua_Integer count = static_cast<lua_Integer>(svc.enum_count(table));
    return static_cast<std::int32_t>(IntArg(L, index, what, field, 0, count - 1));
}

void PushEnum(lua_State* L, std::uint32_t table, std::int32_t value)
{
    const DDCombatService* svc = combat::Service(SessionOf(L).host());
    const char* name = svc ? svc->enum_name(table, value) : nullptr;
    if (name) {
        lua_pushstring(L, name);
    } else {
        lua_pushinteger(L, value);
    }
}

DDActorRef ActorArg(lua_State* L, int index, const char* what, const char* field)
{
    DDActorRef ref{};
    if (!actor::Test(L, index, &ref)) {
        luaL_error(L, "%s 的 %s 必须是 actor 句柄，给的是 %s", what, field, luaL_typename(L, index));
    }
    ref.size = sizeof(ref);
    return ref;
}

// 交给 DDCore 的标签：<mod id>:<tag>
std::string ComposeTag(lua_State* L, const Mod& mod, const std::string& tag, const char* what)
{
    std::string composed = mod.id() + ":" + tag;
    if (composed.size() > kTextMax) {
        luaL_error(L, "%s 的 tag 太长：Mod id（%s）加上 tag 最多 62 字节", what, mod.id().c_str());
    }
    return composed;
}

// 拆开 buff 的 plugin 和 tag：Lua Mod 加的条目（plugin 是 DDLua）返回 Mod id 和 tag
void SplitTag(const std::string& plugin, const std::string& tag, std::string* modId, std::string* userTag)
{
    modId->clear();
    *userTag = tag;
    if (plugin == kPluginName) {
        const std::size_t colon = tag.find(':');
        if (colon != std::string::npos) {
            *modId = tag.substr(0, colon);
            *userTag = tag.substr(colon + 1);
        }
    }
}

std::string Reason(const DDCombatService& svc, std::uint32_t code, const DDCombatOpResult& out)
{
    if (out.message && *out.message) {
        return out.message;
    }
    return svc.result_text(code);
}

// 调用可能同步回到 Lua 的 SDK 函数之后：嵌套的回调里发生原生异常、VM 已损坏时，不能再回到 Lua
void After(lua_State* L)
{
    AfterReentry(L);
}

// 写操作的 C 函数返回之前的检查点：嵌套的回调和游戏函数用掉的时间可能已经让外层过了截止时间（补时间有上限），
// 这里发现就报错，不等 1000 条指令之后的计数 hook（循环里反复调用 add_buff 时每轮只有几条指令）。
// 放在记账（引用、排队记录）之后，报错时不会漏掉引用
int Finish(lua_State* L, int results)
{
    Vm::From(L)->CheckBudget(L);
    return results;
}

void PushBuffInfo(lua_State* L, const DDBuffInfo& info, const DDActorRef& actorRef, const std::string& callerId)
{
    CombatBinding::Impl& b = B(L);
    lua_createtable(L, 0, 24);
    lua_pushinteger(L, info.guid);
    lua_setfield(L, -2, "guid");
    const std::string id = Fixed(info.id);
    lua_pushlstring(L, id.data(), id.size());
    lua_setfield(L, -2, "id");
    PushEnum(L, DD_ENUM_STAT, info.stat_type);
    lua_setfield(L, -2, "stat");
    lua_pushstring(L, Fixed(info.stat_sub_type).c_str());
    lua_setfield(L, -2, "sub_stat");
    lua_pushnumber(L, info.amount);
    lua_setfield(L, -2, "amount");
    lua_pushinteger(L, info.duration);
    lua_setfield(L, -2, "duration");
    PushEnum(L, DD_ENUM_DURATION, info.duration_type);
    lua_setfield(L, -2, "duration_type");
    PushEnum(L, DD_ENUM_SOURCE, info.source);
    lua_setfield(L, -2, "source");
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, (info.source_flags & DD_SOURCE_F_SAVED) != 0);
    lua_setfield(L, -2, "saved");
    lua_pushboolean(L, (info.source_flags & DD_SOURCE_F_TOWN_CLEAR) != 0);
    lua_setfield(L, -2, "town_clear");
    lua_pushboolean(L, (info.source_flags & DD_SOURCE_F_TRAY) != 0);
    lua_setfield(L, -2, "tray");
    lua_setfield(L, -2, "source_flags");
    lua_pushinteger(L, info.sourcedata);
    lua_setfield(L, -2, "sourcedata");
    PushEnum(L, DD_ENUM_RULE, info.rule_type);
    lua_setfield(L, -2, "rule");
    lua_pushnumber(L, info.rule_float);
    lua_setfield(L, -2, "rule_float");
    lua_pushstring(L, Fixed(info.rule_string).c_str());
    lua_setfield(L, -2, "rule_string");
    lua_pushboolean(L, info.is_false_rule);
    lua_setfield(L, -2, "is_false_rule");
    lua_pushboolean(L, info.remove_if_not_active);
    lua_setfield(L, -2, "remove_if_not_active");
    lua_pushboolean(L, info.remove_on_battle_complete);
    lua_setfield(L, -2, "remove_on_battle_complete");
    std::string modId, tag;
    if (info.by_plugin) {
        const std::string plugin = Fixed(info.plugin);
        SplitTag(plugin, Fixed(info.tag), &modId, &tag);
        lua_pushstring(L, plugin.c_str());
        lua_setfield(L, -2, "plugin");
        if (!modId.empty()) {
            lua_pushstring(L, modId.c_str());
            lua_setfield(L, -2, "mod");
        }
        if (!tag.empty()) {
            lua_pushstring(L, tag.c_str());
            lua_setfield(L, -2, "tag");
        }
    }
    lua_pushboolean(L, !modId.empty() && modId == callerId);
    lua_setfield(L, -2, "mine");
    lua_pushboolean(L, b.FindScript(id) != nullptr);
    lua_setfield(L, -2, "script");
    actor::Push(L, actorRef);
    lua_setfield(L, -2, "actor");
}

}  // namespace

void CombatBinding::Impl::PushCallbackBuff(lua_State* L, const DDActorRef& actor, std::uint32_t guid, const Fallback& fb)
{
    const DDCombatService* svc = Svc();
    DDBuffInfo info{};
    info.size = sizeof(info);
    const Mod* current = session.CurrentMod();
    if (svc && svc->buff_get(&actor, guid, &info)) {
        PushBuffInfo(L, info, actor, current ? current->id() : std::string());
        return;
    }
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, guid);
    lua_setfield(L, -2, "guid");
    lua_pushlstring(L, fb.id.data(), fb.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, fb.duration);
    lua_setfield(L, -2, "duration");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "script");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "mine");
    actor::Push(L, actor);
    lua_setfield(L, -2, "actor");
}

namespace {

// ---- 写操作的公共部分 ----

// 本 Mod 排队的操作太多时拒绝（只在不会立即执行时检查）
bool PendingFull(lua_State* L, CombatBinding::Impl& b, const Mod& mod)
{
    const DDCombatService& svc = Svc(L);
    if (svc.safe_point()) {
        return false;
    }
    std::size_t n = 0;
    for (const auto& [op, p] : b.pending) {
        n += p.modToken == mod.token ? 1 : 0;
    }
    return n >= kMaxPendingPerMod;
}

int PushRejected(lua_State* L, const std::string& reason)
{
    lua_pushnil(L);
    lua_pushlstring(L, reason.data(), reason.size());
    return 2;
}

// 读接口（buff 表、Buff 库、effect 表）在加载线程运行时一律读不到（DDCore 的 A-S3），这时给出以「加载中」开头的原因，
// 和「条目、定义真的不在」分开（审核 R2A-S1）。Mod 按原因的前缀判断，加载结束后再读
constexpr const char* kLoadingReason =
    "加载中：游戏的加载线程在跑（读档进地牢、回城的加载画面），buff 表、Buff 库和 effect 表暂时读不到，加载结束后再读";

bool ReadsBlocked(const DDCombatService& svc)
{
    return svc.read_ready && !svc.read_ready();
}

// opts.done（可为 nil），返回注册表引用
int DoneRef(lua_State* L, int index, const char* what)
{
    if (lua_isnoneornil(L, index)) {
        return LUA_NOREF;
    }
    if (lua_type(L, index) != LUA_TFUNCTION) {
        luaL_error(L, "%s 的 done 必须是函数", what);
    }
    lua_pushvalue(L, index);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

void AddPending(lua_State* L, CombatBinding::Impl& b, const Mod& mod, std::uint64_t op, CombatBinding::Impl::PendingOp p)
{
    (void)L;
    p.op = op;
    p.modToken = mod.token;
    p.modId = mod.id();
    b.pending[op] = std::move(p);
    OpRoutes()[op] = &b.self;
}

void UnrefIn(lua_State* L, int& ref)
{
    if (ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

// 选项表的原始遍历：fn(key) 处理栈顶的值，返回 false 表示不认识的键
template <class Fn>
void ForOptions(lua_State* L, int index, const char* what, const char* known, Fn&& fn)
{
    if (lua_isnoneornil(L, index)) {
        return;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "%s 的选项名必须是字符串", what);
        }
        const char* key = lua_tostring(L, -2);
        if (!fn(key)) {
            luaL_error(L, "%s 没有选项 %s（可用：%s）", what, key, known);
        }
        lua_pop(L, 1);
    }
}

// ---- dd.buff.define ----

int BuffDefine(lua_State* L)
{
    constexpr const char* what = "dd.buff.define";
    Mod& mod = CheckMod(L);
    CheckWrite(L, mod, what);
    luaL_checktype(L, 1, LUA_TTABLE);
    const DDCombatService& svc = Svc(L);
    DDBuffDef def{};
    def.size = sizeof(def);
    def.duration = -1;
    def.duration_type = svc.enum_value(DD_ENUM_DURATION, "till_removed");
    def.source = svc.enum_value(DD_ENUM_SOURCE, "skill");
    def.rule_type = svc.enum_value(DD_ENUM_RULE, "always");
    bool hasId = false, hasStat = false;
    int cbIndex[kCbCount] = {};
    int doneIndex = 0;
    std::string id;
    ForOptions(L, 1, what,
        "id、stat、sub_stat、amount、duration、duration_type、source、sourcedata、rule、rule_string、rule_float、is_false_rule、"
        "remove_if_not_active、remove_on_battle_complete、on_apply、on_remove、on_tick、on_damage、done",
        [&](const char* key) {
            const int v = lua_gettop(L);
            if (std::strcmp(key, "id") == 0) {
                id = TextArg(L, v, what, "id", false);
                CopyText(def.id, id);
                hasId = true;
            } else if (std::strcmp(key, "stat") == 0) {
                def.stat_type = EnumArg(L, v, DD_ENUM_STAT, what, "stat");
                hasStat = true;
            } else if (std::strcmp(key, "sub_stat") == 0) {
                CopyText(def.stat_sub_type, TextArg(L, v, what, "sub_stat", true));
            } else if (std::strcmp(key, "amount") == 0) {
                def.amount = static_cast<float>(NumberArg(L, v, what, "amount"));
            } else if (std::strcmp(key, "duration") == 0) {
                def.duration = static_cast<std::int32_t>(IntArg(L, v, what, "duration", -1, std::numeric_limits<std::int32_t>::max()));
                if (def.duration == 0) {
                    luaL_error(L, "%s 的 duration 只能是 -1（不计时）或大于 0", what);
                }
            } else if (std::strcmp(key, "duration_type") == 0) {
                def.duration_type = EnumArg(L, v, DD_ENUM_DURATION, what, "duration_type");
            } else if (std::strcmp(key, "source") == 0) {
                def.source = EnumArg(L, v, DD_ENUM_SOURCE, what, "source");
            } else if (std::strcmp(key, "sourcedata") == 0) {
                def.sourcedata = static_cast<std::int32_t>(
                    IntArg(L, v, what, "sourcedata", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
            } else if (std::strcmp(key, "rule") == 0) {
                def.rule_type = EnumArg(L, v, DD_ENUM_RULE, what, "rule");
            } else if (std::strcmp(key, "rule_string") == 0) {
                CopyText(def.rule_string, TextArg(L, v, what, "rule_string", true));
            } else if (std::strcmp(key, "rule_float") == 0) {
                def.rule_float = static_cast<float>(NumberArg(L, v, what, "rule_float"));
            } else if (std::strcmp(key, "is_false_rule") == 0) {
                def.is_false_rule = BoolArg(L, v, what, key);
            } else if (std::strcmp(key, "remove_if_not_active") == 0) {
                def.remove_if_not_active = BoolArg(L, v, what, key);
            } else if (std::strcmp(key, "remove_on_battle_complete") == 0) {
                def.remove_on_battle_complete = BoolArg(L, v, what, key);
            } else if (std::strcmp(key, "done") == 0) {
                luaL_checktype(L, v, LUA_TFUNCTION);
                doneIndex = 1;
            } else {
                for (int cb = 0; cb < kCbCount; ++cb) {
                    if (std::strcmp(key, kCbNames[cb]) == 0) {
                        if (lua_type(L, v) != LUA_TFUNCTION) {
                            luaL_error(L, "%s 的 %s 必须是函数", what, key);
                        }
                        cbIndex[cb] = 1;
                        return true;
                    }
                }
                return false;
            }
            return true;
        });
    if (!hasId || !hasStat) {
        return luaL_error(L, "%s 必须给出 id 和 stat", what);
    }
    CombatBinding::Impl& b = B(L);
    if (const auto it = b.defs.find(id); it != b.defs.end() && it->second.modToken != mod.token && SessionOf(L).FindToken(it->second.modToken)) {
        return PushRejected(L, "buff id " + id + " 已经由 Lua Mod " + it->second.modId + " 定义");
    }
    // Buff 库按 id 的游戏哈希存：不同的 id 哈希相同时会改到同一条定义上（DDCore 只认 DDLua 一个插件，分不出 Mod）
    const std::uint32_t idHash = ddc::combat::GameHash(id);
    for (const auto& [otherId, other] : b.defs) {
        if (otherId != id && ddc::combat::GameHash(otherId) == idHash && SessionOf(L).FindToken(other.modToken)) {
            return PushRejected(L, "buff id " + id + " 和 Lua Mod " + other.modId + " 定义的 " + otherId + " 哈希相同");
        }
    }
    // 回调和 done 的引用先建好（可能抛内存错误），再调用 DDCore
    int refs[kCbCount] = { LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF };
    for (int cb = 0; cb < kCbCount; ++cb) {
        if (cbIndex[cb]) {
            lua_getfield(L, 1, kCbNames[cb]);
            refs[cb] = luaL_ref(L, LUA_REGISTRYINDEX);
        }
    }
    int doneRef = LUA_NOREF;
    if (doneIndex) {
        lua_getfield(L, 1, "done");
        doneRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (PendingFull(L, b, mod)) {
        for (int& ref : refs) {
            UnrefIn(L, ref);
        }
        UnrefIn(L, doneRef);
        return PushRejected(L, "本 Mod 排队的写操作超过 " + std::to_string(kMaxPendingPerMod) + " 个");
    }
    DDCombatOpResult out{};
    out.size = sizeof(out);
    const std::uint32_t code = svc.buff_define(SessionOf(L).host().plugin, &def, &out);
    After(L);
    if (code != DD_COMBAT_OK && code != DD_COMBAT_QUEUED) {
        for (int& ref : refs) {
            UnrefIn(L, ref);
        }
        UnrefIn(L, doneRef);
        return Finish(L, PushRejected(L, Reason(svc, code, out)));
    }
    // 接受：登记归属和回调（排队的也先登记，失败时在操作结果里撤销）
    const bool created = b.defs.find(id) == b.defs.end();
    CombatBinding::Impl::ScriptDef& slot = b.defs[id];
    for (int& ref : slot.ref) {
        UnrefIn(L, ref);
    }
    slot.id = id;
    slot.modToken = mod.token;
    slot.modId = mod.id();
    for (int cb = 0; cb < kCbCount; ++cb) {
        slot.ref[cb] = refs[cb];
        slot.failures[cb] = 0;
    }
    b.UpdateSubs();
    lua_pushboolean(L, 1);
    if (code == DD_COMBAT_QUEUED) {
        CombatBinding::Impl::PendingOp p;
        p.kind = DD_COMBAT_OP_BUFF_DEFINE;
        p.doneRef = doneRef;
        p.id = id;
        p.created = created;
        AddPending(L, b, mod, out.op, std::move(p));
        lua_pushnil(L);
    } else {
        UnrefIn(L, doneRef);
        lua_pushlstring(L, id.data(), id.size());
    }
    return Finish(L, 2);
}

// ---- dd.buff.def ----

int BuffDef(lua_State* L)
{
    CheckMod(L);
    const std::string id = TextArg(L, 1, "dd.buff.def", "id", false);
    const DDCombatService& svc = Svc(L);
    DDBuffDef def{};
    def.size = sizeof(def);
    if (!svc.buff_def_get(id.c_str(), &def)) {
        return PushRejected(L, ReadsBlocked(svc) ? std::string(kLoadingReason) : "Buff 库里没有 " + id + "（或不在存档里）");
    }
    CombatBinding::Impl& b = B(L);
    lua_createtable(L, 0, 18);
    lua_pushstring(L, Fixed(def.id).c_str());
    lua_setfield(L, -2, "id");
    PushEnum(L, DD_ENUM_STAT, def.stat_type);
    lua_setfield(L, -2, "stat");
    lua_pushstring(L, Fixed(def.stat_sub_type).c_str());
    lua_setfield(L, -2, "sub_stat");
    lua_pushnumber(L, def.amount);
    lua_setfield(L, -2, "amount");
    lua_pushinteger(L, def.duration);
    lua_setfield(L, -2, "duration");
    PushEnum(L, DD_ENUM_DURATION, def.duration_type);
    lua_setfield(L, -2, "duration_type");
    PushEnum(L, DD_ENUM_SOURCE, def.source);
    lua_setfield(L, -2, "source");
    lua_pushinteger(L, def.sourcedata);
    lua_setfield(L, -2, "sourcedata");
    PushEnum(L, DD_ENUM_RULE, def.rule_type);
    lua_setfield(L, -2, "rule");
    lua_pushstring(L, Fixed(def.rule_string).c_str());
    lua_setfield(L, -2, "rule_string");
    lua_pushnumber(L, def.rule_float);
    lua_setfield(L, -2, "rule_float");
    lua_pushboolean(L, def.is_false_rule);
    lua_setfield(L, -2, "is_false_rule");
    lua_pushboolean(L, def.remove_if_not_active);
    lua_setfield(L, -2, "remove_if_not_active");
    lua_pushboolean(L, def.remove_on_battle_complete);
    lua_setfield(L, -2, "remove_on_battle_complete");
    if (const auto it = b.defs.find(id); it != b.defs.end()) {
        lua_pushstring(L, it->second.modId.c_str());
        lua_setfield(L, -2, "mod");
    }
    lua_pushboolean(L, b.FindScript(id) != nullptr);
    lua_setfield(L, -2, "script");
    return 1;
}

// ---- dd.buff.enum ----

int BuffEnum(lua_State* L)
{
    CheckMod(L);
    const char* name = luaL_checkstring(L, 1);
    std::uint32_t table = 0;
    if (std::strcmp(name, "stat") == 0) {
        table = DD_ENUM_STAT;
    } else if (std::strcmp(name, "source") == 0) {
        table = DD_ENUM_SOURCE;
    } else if (std::strcmp(name, "duration") == 0 || std::strcmp(name, "duration_type") == 0) {
        table = DD_ENUM_DURATION;
    } else if (std::strcmp(name, "rule") == 0) {
        table = DD_ENUM_RULE;
    } else {
        return luaL_error(L, "dd.buff.enum 的表只能是 \"stat\"、\"source\"、\"duration\"、\"rule\"");
    }
    const DDCombatService& svc = Svc(L);
    if (lua_isnoneornil(L, 2)) {
        const std::uint32_t count = svc.enum_count(table);
        lua_createtable(L, static_cast<int>(count), 0);
        for (std::uint32_t i = 0; i < count; ++i) {
            const char* text = svc.enum_name(table, static_cast<std::int32_t>(i));
            lua_pushstring(L, text ? text : "");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
        }
        return 1;
    }
    if (lua_type(L, 2) == LUA_TSTRING) {
        const std::int32_t value = svc.enum_value(table, lua_tostring(L, 2));
        value < 0 ? lua_pushnil(L) : lua_pushinteger(L, value);
        return 1;
    }
    int isInteger = 0;
    const lua_Integer value = lua_type(L, 2) == LUA_TNUMBER ? lua_tointegerx(L, 2, &isInteger) : 0;
    if (!isInteger) {
        return luaL_error(L, "dd.buff.enum 的第二个参数是名字（字符串）或数值（整数）");
    }
    const char* text = value >= 0 && value <= std::numeric_limits<std::int32_t>::max() ? svc.enum_name(table, static_cast<std::int32_t>(value)) : nullptr;
    text ? (void)lua_pushstring(L, text) : lua_pushnil(L);
    return 1;
}

// ---- dd.buff.state ----

int BuffState(lua_State* L)
{
    CheckMod(L);
    const DDActorRef actor = ActorArg(L, 1, "dd.buff.state", "actor");
    const auto guid = static_cast<std::uint32_t>(IntArg(L, 2, "dd.buff.state", "guid", 0, 0xFFFFFFFFll));
    CombatBinding::Impl& b = B(L);
    if (const auto it = b.states.find(CombatBinding::Impl::Key(actor, guid)); it != b.states.end()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.stateRef);
        return 1;
    }
    // 没有绑定：条目存在、是脚本 Buff 时绑一张空表（读档恢复的载体）
    const DDCombatService& svc = Svc(L);
    DDBuffInfo info{};
    info.size = sizeof(info);
    if (!svc.buff_get(&actor, guid, &info)) {
        if (ReadsBlocked(svc)) {
            return PushRejected(L, kLoadingReason);
        }
        lua_pushnil(L);
        return 1;
    }
    const int ref = b.StateOf(actor, guid, Fixed(info.id), true);
    ref == LUA_NOREF ? lua_pushnil(L) : (void)lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return 1;
}

// ---- 规则 ----

void FilterRuleFault(CombatBinding* binding, EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode != 0xE06D7363) {
        binding->NoteNativeFault(record->ExceptionCode, record->ExceptionAddress);
    }
}

int RuleFilter(CombatBinding* binding, EXCEPTION_POINTERS* info)
{
    FilterRuleFault(binding, info);
    return EXCEPTION_CONTINUE_SEARCH;  // 交给 DDCore 的隔离：它停用这条规则、写日志
}

std::uint32_t RuleBody(CombatBinding* binding, std::uint64_t key, const DDRuleContext* context)
{
    return binding->OnRule(key, *context);
}

std::uint32_t GuardedRule(CombatBinding* binding, std::uint64_t key, const DDRuleContext* context)
{
    __try {
        return RuleBody(binding, key, context);
    } __except (RuleFilter(binding, GetExceptionInformation())) {
        return DD_RULE_ORIGINAL;
    }
}

uint32_t DD_CALL RuleThunk(const DDRuleContext* context, void* user)
{
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(user));
    const auto it = RuleRoutes().find(key);
    if (it == RuleRoutes().end() || !context) {
        return DD_RULE_ORIGINAL;
    }
    return GuardedRule(it->second, key, context);
}

RuleUd& CheckRule(lua_State* L, int index)
{
    return *static_cast<RuleUd*>(luaL_checkudata(L, index, kRuleMeta));
}

int RuleOff(lua_State* L)
{
    RuleUd& ud = CheckRule(L, 1);
    Session& session = SessionOf(L);
    if (session.InRule()) {
        return luaL_error(L, "rule:off()：规则回调里不能撤销规则");
    }
    CombatBinding::Impl& b = B(L);
    const auto it = b.rules.find(ud.key);
    if (it == b.rules.end()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    CheckOwner(L, session.FindToken(it->second.modToken), it->second.modId, "rule:off()");
    if (it->second.id) {
        Svc(L).rule_unregister(session.host().plugin, it->second.id);
    }
    RuleRoutes().erase(it->first);
    UnrefIn(L, it->second.fnRef);
    b.rules.erase(it);
    lua_pushboolean(L, 1);
    return 1;
}

int RuleActive(lua_State* L)
{
    RuleUd& ud = CheckRule(L, 1);
    const auto& rules = B(L).rules;
    const auto it = rules.find(ud.key);
    lua_pushboolean(L, it != rules.end() && !it->second.disabled);
    return 1;
}

int RuleIndex(lua_State* L)
{
    RuleUd& ud = CheckRule(L, 1);
    const char* key = lua_tostring(L, 2);
    if (!key) {
        lua_pushnil(L);
    } else if (std::strcmp(key, "off") == 0) {
        lua_pushcfunction(L, RuleOff);
    } else if (std::strcmp(key, "active") == 0) {
        lua_pushcfunction(L, RuleActive);
    } else if (std::strcmp(key, "name") == 0) {
        lua_getiuservalue(L, 1, 1);
    } else {
        lua_pushnil(L);
    }
    (void)ud;
    return 1;
}

int RuleToString(lua_State* L)
{
    CheckRule(L, 1);
    lua_getiuservalue(L, 1, 1);
    lua_pushfstring(L, "dd.rule(%s)", lua_tostring(L, -1));
    return 1;
}

int BuffRule(lua_State* L)
{
    constexpr const char* what = "dd.buff.rule";
    Mod& mod = CheckMod(L);
    CheckWrite(L, mod, what);
    const std::string name = TextArg(L, 1, what, "name", false);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    Session& session = SessionOf(L);
    CombatBinding::Impl& b = B(L);
    const DDCombatService& svc = Svc(L);
    if (session.SubscriptionLoad(mod) >= session.config().maxPerMod) {
        return luaL_error(L, "%s：订阅数达到上限 %d（DDLua.cfg 的 max_per_mod，规则也算）", what, static_cast<int>(session.config().maxPerMod));
    }
    auto* ud = static_cast<RuleUd*>(lua_newuserdatauv(L, sizeof(RuleUd), 1));
    ud->key = 0;
    lua_pushlstring(L, name.data(), name.size());
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, kRuleMeta);
    lua_pushvalue(L, 2);
    const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
    const std::uint64_t key = g_nextRuleKey++;
    RuleRoutes()[key] = &b.self;
    const DDRuleId id = svc.rule_register(session.host().plugin, name.c_str(), &RuleThunk, reinterpret_cast<void*>(static_cast<std::uintptr_t>(key)));
    if (!id) {
        RuleRoutes().erase(key);
        luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        return PushRejected(L, "规则 " + name + " 注册失败（名字已经注册过，或哈希和已注册的名字相同；原因见 DDCore.log）");
    }
    CombatBinding::Impl::Rule rule;
    rule.key = key;
    rule.modToken = mod.token;
    rule.modId = mod.id();
    rule.name = name;
    rule.fnRef = fnRef;
    rule.id = id;
    b.rules[key] = std::move(rule);
    ud->key = key;
    return 1;
}

// ---- effect 句柄 ----

EffectUd& CheckEffect(lua_State* L, int index)
{
    return *static_cast<EffectUd*>(luaL_checkudata(L, index, kEffectMeta));
}

int EffectValid(lua_State* L)
{
    EffectUd& ud = CheckEffect(L, 1);
    const DDCombatService* svc = combat::Service(SessionOf(L).host());
    DDEffectInfo info{};
    info.size = sizeof(info);
    const bool ok = svc && svc->effect_get(ud.name, &info);
    lua_pushboolean(L, ok);
    if (!ok && svc && ReadsBlocked(*svc)) {
        lua_pushstring(L, kLoadingReason);
        return 2;
    }
    return 1;
}

// e:apply(performer[, target][, opts])
int EffectApply(lua_State* L)
{
    constexpr const char* what = "effect:apply";
    EffectUd& ud = CheckEffect(L, 1);
    Mod& mod = CallerMod(L, what);
    CheckWrite(L, mod, what);
    const DDActorRef performer = ActorArg(L, 2, what, "performer");
    DDActorRef target = performer;
    bool hasTarget = false;
    int optsIndex = 3;
    if (!lua_isnoneornil(L, 3) && lua_type(L, 3) != LUA_TTABLE) {
        target = ActorArg(L, 3, what, "target");
        hasTarget = true;
        optsIndex = 4;
    } else if (lua_isnil(L, 3) && !lua_isnone(L, 4)) {
        optsIndex = 4;
    }
    int doneIndex = 0;
    ForOptions(L, optsIndex, what, "done", [&](const char* key) {
        if (std::strcmp(key, "done") != 0) {
            return false;
        }
        doneIndex = lua_gettop(L);
        return true;
    });
    int doneRef = LUA_NOREF;
    if (doneIndex) {
        lua_getfield(L, optsIndex, "done");
        doneRef = DoneRef(L, lua_gettop(L), what);
        lua_pop(L, 1);
    }
    const DDCombatService& svc = Svc(L);
    CombatBinding::Impl& b = B(L);
    if (PendingFull(L, b, mod)) {
        UnrefIn(L, doneRef);
        return PushRejected(L, "本 Mod 排队的写操作超过 " + std::to_string(kMaxPendingPerMod) + " 个");
    }
    // 执行者和目标施加前的 guid（立即执行时比较出新增的条目）
    const auto snapshot = [&](const DDActorRef& actor) {
        std::vector<std::uint32_t> guids;
        std::uint32_t count = 0;
        if (svc.buff_list(&actor, nullptr, 0, &count) && count) {
            guids.resize(count);
            svc.buff_list(&actor, guids.data(), count, &count);
            guids.resize((std::min)(static_cast<std::size_t>(count), guids.size()));
        }
        return guids;
    };
    const bool same = actor::Same(performer, target);
    const auto beforeP = snapshot(performer);
    const auto beforeT = same ? std::vector<std::uint32_t>{} : snapshot(target);
    DDCombatOpResult out{};
    out.size = sizeof(out);
    const std::string name = Fixed(ud.name);
    const std::uint32_t code = svc.effect_apply(SessionOf(L).host().plugin, name.c_str(), &performer, hasTarget ? &target : nullptr, &out);
    After(L);
    if (code == DD_COMBAT_QUEUED) {
        CombatBinding::Impl::PendingOp p;
        p.kind = DD_COMBAT_OP_EFFECT_APPLY;
        p.doneRef = doneRef;
        p.id = name;
        AddPending(L, b, mod, out.op, std::move(p));
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return Finish(L, 2);
    }
    UnrefIn(L, doneRef);
    if (code != DD_COMBAT_OK) {
        return Finish(L, PushRejected(L, Reason(svc, code, out)));
    }
    lua_pushboolean(L, 1);
    lua_createtable(L, 4, 1);
    lua_pushinteger(L, out.count);
    lua_setfield(L, -2, "count");
    lua_Integer n = 0;
    const auto diff = [&](const DDActorRef& actor, const std::vector<std::uint32_t>& before) {
        for (const auto guid : snapshot(actor)) {
            if (std::find(before.begin(), before.end(), guid) != before.end()) {
                continue;
            }
            DDBuffInfo info{};
            info.size = sizeof(info);
            lua_createtable(L, 0, 3);
            actor::Push(L, actor);
            lua_setfield(L, -2, "actor");
            lua_pushinteger(L, guid);
            lua_setfield(L, -2, "guid");
            if (svc.buff_get(&actor, guid, &info)) {
                lua_pushstring(L, Fixed(info.id).c_str());
                lua_setfield(L, -2, "id");
            }
            lua_rawseti(L, -2, ++n);
        }
    };
    diff(performer, beforeP);
    if (!same) {
        diff(target, beforeT);
    }
    return Finish(L, 2);
}

bool IsEffectField(const char* key)
{
    for (const char* name : { "target", "chance", "stress", "stun", "heal", "heal_percent", "combat_stat_buff", "dot_poison", "dot_bleed",
             "dot_burn", "dot_stress", "duration", "buff_amount", "buff_duration_type", "buff_id_count", "on_hit", "on_miss", "apply_once",
             "queue", "swap_source_and_target", "runtime" }) {
        if (std::strcmp(key, name) == 0) {
            return true;
        }
    }
    return false;
}

int EffectIndex(lua_State* L)
{
    EffectUd& ud = CheckEffect(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "effect 句柄的字段名必须是字符串");
    }
    const char* key = lua_tostring(L, 2);
    if (std::strcmp(key, "name") == 0) {
        lua_pushstring(L, ud.name);
        return 1;
    }
    if (std::strcmp(key, "apply") == 0) {
        lua_pushcfunction(L, EffectApply);
        return 1;
    }
    if (std::strcmp(key, "valid") == 0) {
        lua_pushcfunction(L, EffectValid);
        return 1;
    }
    if (!IsEffectField(key)) {
        return luaL_error(L,
            "effect 句柄没有字段 %s（可用：name、target、chance、stress、stun、heal、heal_percent、combat_stat_buff、dot_poison、dot_bleed、"
            "dot_burn、dot_stress、duration、buff_amount、buff_duration_type、buff_id_count、on_hit、on_miss、apply_once、queue、"
            "swap_source_and_target、runtime、apply、valid）",
            key);
    }
    const DDCombatService* svc = combat::Service(SessionOf(L).host());
    DDEffectInfo info{};
    info.size = sizeof(info);
    if (!svc || !svc->effect_get(ud.name, &info)) {
        lua_pushnil(L);
        return 1;
    }
    const auto num = [&](double v) { lua_pushnumber(L, static_cast<lua_Number>(v)); };
    const auto integer = [&](std::int64_t v) { lua_pushinteger(L, static_cast<lua_Integer>(v)); };
    const auto boolean = [&](bool v) { lua_pushboolean(L, v); };
    const std::string_view k(key);
    if (k == "target") integer(info.target);
    else if (k == "chance") num(info.chance);
    else if (k == "stress") num(info.stress);
    else if (k == "stun") num(info.stun);
    else if (k == "heal") num(info.heal);
    else if (k == "heal_percent") num(info.heal_percent);
    else if (k == "combat_stat_buff") integer(info.combat_stat_buff);
    else if (k == "dot_poison") integer(info.dot_poison);
    else if (k == "dot_bleed") integer(info.dot_bleed);
    else if (k == "dot_burn") integer(info.dot_burn);
    else if (k == "dot_stress") integer(info.dot_stress);
    else if (k == "duration") integer(info.duration);
    else if (k == "buff_amount") num(info.buff_amount);
    else if (k == "buff_duration_type") PushEnum(L, DD_ENUM_DURATION, info.buff_duration_type);
    else if (k == "buff_id_count") integer(info.buff_id_count);
    else if (k == "on_hit") boolean(info.on_hit);
    else if (k == "on_miss") boolean(info.on_miss);
    else if (k == "apply_once") boolean(info.apply_once);
    else if (k == "queue") boolean(info.queue);
    else if (k == "swap_source_and_target") boolean(info.swap_source_and_target);
    else boolean(info.runtime);
    return 1;
}

int EffectNewIndex(lua_State* L)
{
    CheckEffect(L, 1);
    return luaL_error(L, "effect 句柄只读");
}

int EffectEq(lua_State* L)
{
    auto* a = static_cast<EffectUd*>(luaL_testudata(L, 1, kEffectMeta));
    auto* b = static_cast<EffectUd*>(luaL_testudata(L, 2, kEffectMeta));
    lua_pushboolean(L, a && b && std::strcmp(a->name, b->name) == 0);
    return 1;
}

int EffectToString(lua_State* L)
{
    EffectUd& ud = CheckEffect(L, 1);
    lua_pushfstring(L, "dd.effect(%s)", ud.name);
    return 1;
}

int EffectGet(lua_State* L)
{
    CheckMod(L);
    const std::string name = TextArg(L, 1, "dd.effect.get", "name", false);
    const DDCombatService& svc = Svc(L);
    DDEffectInfo info{};
    info.size = sizeof(info);
    if (!svc.effect_get(name.c_str(), &info)) {
        return PushRejected(L, ReadsBlocked(svc) ? std::string(kLoadingReason) : "effect " + name + " 不存在（或不在存档里）");
    }
    combat::PushEffect(L, name.c_str());
    return 1;
}

// ---- dd.effect.define ----

int EffectDefine(lua_State* L)
{
    constexpr const char* what = "dd.effect.define";
    Mod& mod = CheckMod(L);
    CheckWrite(L, mod, what);
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    if (length == 0 || length > kEffectTextMax) {
        return luaL_error(L, "%s 的文本为空或超过 64 KB", what);
    }
    std::vector<std::string> names;
    std::string why;
    if (!EffectNames(std::string_view(text, length), &names, &why)) {
        return luaL_error(L, "%s：%s", what, why.c_str());
    }
    int doneIndex = 0;
    ForOptions(L, 2, what, "done", [&](const char* key) {
        if (std::strcmp(key, "done") != 0) {
            return false;
        }
        doneIndex = 1;
        return true;
    });
    CombatBinding::Impl& b = B(L);
    Session& session = SessionOf(L);
    for (const auto& name : names) {
        const auto it = b.effectOwners.find(ddc::combat::GameHash(name));
        if (it != b.effectOwners.end() && it->second.modToken != mod.token && session.FindToken(it->second.modToken)) {
            return PushRejected(L, "effect " + name + " 已经由 Lua Mod " + it->second.modId + " 定义（或和它定义的名字哈希相同）");
        }
    }
    int doneRef = LUA_NOREF;
    if (doneIndex) {
        lua_getfield(L, 2, "done");
        doneRef = DoneRef(L, lua_gettop(L), what);
        lua_pop(L, 1);
    }
    if (PendingFull(L, b, mod)) {
        UnrefIn(L, doneRef);
        return PushRejected(L, "本 Mod 排队的写操作超过 " + std::to_string(kMaxPendingPerMod) + " 个");
    }
    const DDCombatService& svc = Svc(L);
    DDCombatOpResult out{};
    out.size = sizeof(out);
    const std::string copy(text, length);
    const std::uint32_t code = svc.effect_define(session.host().plugin, copy.c_str(), &out);
    After(L);
    if (code != DD_COMBAT_OK && code != DD_COMBAT_QUEUED) {
        UnrefIn(L, doneRef);
        return Finish(L, PushRejected(L, Reason(svc, code, out)));
    }
    for (const auto& name : names) {
        b.effectOwners[ddc::combat::GameHash(name)] = { mod.token, mod.id() };
    }
    lua_pushboolean(L, 1);
    if (code == DD_COMBAT_QUEUED) {
        CombatBinding::Impl::PendingOp p;
        p.kind = DD_COMBAT_OP_EFFECT_DEFINE;
        p.doneRef = doneRef;
        p.effectNames = names;
        AddPending(L, b, mod, out.op, std::move(p));
        lua_pushnil(L);
    } else {
        UnrefIn(L, doneRef);
        lua_pushinteger(L, out.count);
    }
    return Finish(L, 2);
}

// ---- dd.effect.register ----

int EffectRegister(lua_State* L)
{
    constexpr const char* what = "dd.effect.register";
    CheckMod(L);
    std::size_t length = 0;
    const char* pattern = luaL_checklstring(L, 1, &length);
    std::string text(pattern, length);
    EventBinding::NameMatch match;
    if (!text.empty() && text.back() == '*') {
        match.prefix = true;
        text.pop_back();
    }
    if ((!match.prefix && text.empty()) || text.size() > kTextMax || text.find('*') != std::string::npos ||
        !ValidText(text, match.prefix)) {
        return luaL_error(L, "%s 的 pattern 是 effect 名字，或以 * 结尾的前缀（* 只能在结尾），最多 63 字节", what);
    }
    match.text = text;
    return EventBinding::LuaSubscribe(L, what, "effect.pre", 2, 3, false, &match);
}

// ---- 技能句柄 ----

SkillUd& CheckSkill(lua_State* L, int index)
{
    return *static_cast<SkillUd*>(luaL_checkudata(L, index, kSkillMeta));
}

bool ReadSkill(lua_State* L, const DDSkillRef& ref, DDSkillInfo* info)
{
    const DDCombatService* svc = combat::Service(SessionOf(L).host());
    *info = {};
    info->size = sizeof(DDSkillInfo);
    return svc && svc->skill_info(&ref, info);
}

int SkillValid(lua_State* L)
{
    SkillUd& ud = CheckSkill(L, 1);
    DDSkillInfo info{};
    lua_pushboolean(L, ReadSkill(L, ud.ref, &info));
    return 1;
}

int SkillIndex(lua_State* L)
{
    SkillUd& ud = CheckSkill(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "技能句柄的字段名必须是字符串");
    }
    const std::string_view key = lua_tostring(L, 2);
    if (key == "id") {
        lua_pushstring(L, Fixed(ud.ref.id).c_str());
    } else if (key == "level") {
        lua_pushinteger(L, ud.ref.level);
    } else if (key == "performer") {
        actor::Push(L, ud.ref.actor);
    } else if (key == "valid") {
        lua_pushcfunction(L, SkillValid);
    } else if (key == "type" || key == "friendly" || key == "effects") {
        DDSkillInfo info{};
        if (!ReadSkill(L, ud.ref, &info)) {
            lua_pushnil(L);
        } else if (key == "type") {
            lua_pushstring(L, Fixed(info.type).c_str());
        } else if (key == "friendly") {
            lua_pushboolean(L, info.friendly);
        } else {
            const std::uint32_t n = (std::min)(info.effect_count, static_cast<std::uint32_t>(DD_SKILL_MAX_EFFECTS));
            lua_createtable(L, static_cast<int>(n), 0);
            for (std::uint32_t i = 0; i < n; ++i) {
                lua_pushstring(L, Fixed(info.effects[i]).c_str());
                lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
            }
        }
    } else {
        return luaL_error(L, "技能句柄没有字段 %s（可用：id、level、type、friendly、effects、performer、valid）", lua_tostring(L, 2));
    }
    return 1;
}

int SkillNewIndex(lua_State* L)
{
    CheckSkill(L, 1);
    return luaL_error(L, "技能句柄只读（skill.pre 不开放改技能，见 docs/03-lua-system.md）");
}

int SkillEq(lua_State* L)
{
    auto* a = static_cast<SkillUd*>(luaL_testudata(L, 1, kSkillMeta));
    auto* b = static_cast<SkillUd*>(luaL_testudata(L, 2, kSkillMeta));
    lua_pushboolean(L, a && b && actor::Same(a->ref.actor, b->ref.actor) && std::strcmp(a->ref.id, b->ref.id) == 0 && a->ref.level == b->ref.level);
    return 1;
}

int SkillToString(lua_State* L)
{
    SkillUd& ud = CheckSkill(L, 1);
    const std::string text = "dd.skill(" + Fixed(ud.ref.id) + "，等级 " + std::to_string(ud.ref.level) + "，" + actor::Describe(ud.ref.actor) + ")";
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

void NewMeta(lua_State* L, const char* name, lua_CFunction index, lua_CFunction newindex, lua_CFunction eq, lua_CFunction tostring)
{
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    if (newindex) {
        lua_pushcfunction(L, newindex);
        lua_setfield(L, -2, "__newindex");
    }
    if (eq) {
        lua_pushcfunction(L, eq);
        lua_setfield(L, -2, "__eq");
    }
    lua_pushcfunction(L, tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

// ---- 操作回调 ----

void FilterOpFault(CombatBinding* binding, EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode != 0xE06D7363) {
        binding->NoteNativeFault(record->ExceptionCode, record->ExceptionAddress);
    }
}

int OpFilter(CombatBinding* binding, EXCEPTION_POINTERS* info)
{
    const unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    if (code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FilterOpFault(binding, info);
    return EXCEPTION_EXECUTE_HANDLER;  // 自己接住：VM 已标记损坏，DDCore 的操作回调照常
}

void OpBody(CombatBinding* binding, const DDCombatOpResult* result)
{
    binding->OnOpResult(*result);
}

void GuardedOp(CombatBinding* binding, const DDCombatOpResult* result)
{
    __try {
        OpBody(binding, result);
    } __except (OpFilter(binding, GetExceptionInformation())) {
        if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
    }
}

void DD_CALL OpThunk(const DDCombatOpResult* result, void*)
{
    if (!result) {
        return;
    }
    const auto it = OpRoutes().find(result->op);
    if (it == OpRoutes().end()) {
        return;
    }
    CombatBinding* binding = it->second;
    OpRoutes().erase(it);
    GuardedOp(binding, result);
}

}  // namespace

// ============================================================================

namespace combat {

const DDCombatService* Service(const Host& host)
{
    const DDCombatService* svc = host.combat;
    return svc && svc->header.size >= sizeof(DDCombatService) && svc->buff_get ? svc : nullptr;
}

const char* LoadingReason(const Host& host)
{
    const DDCombatService* svc = Service(host);
    return svc && ReadsBlocked(*svc) ? kLoadingReason : nullptr;
}

void PushEffect(lua_State* L, const char* name)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kEffectIntern);
    if (lua_getfield(L, -1, name) == LUA_TUSERDATA) {
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    auto* ud = static_cast<EffectUd*>(lua_newuserdatauv(L, sizeof(EffectUd), 0));
    CopyText(ud->name, name);
    luaL_setmetatable(L, kEffectMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    lua_remove(L, -2);
}

void PushSkill(lua_State* L, const DDSkillRef& ref)
{
    auto* ud = static_cast<SkillUd*>(lua_newuserdatauv(L, sizeof(SkillUd), 0));
    ud->ref = ref;
    ud->ref.size = sizeof(DDSkillRef);
    luaL_setmetatable(L, kSkillMeta);
}

// actor:buffs([filter])
int ActorBuffs(lua_State* L)
{
    constexpr const char* what = "actor:buffs";
    Mod& mod = CallerMod(L, what);
    const DDActorRef actor = ActorArg(L, 1, what, "self");
    std::string id, tag;
    bool byId = false, byTag = false, bySource = false, byStat = false, byMine = false, mine = false;
    std::int32_t source = 0, stat = 0;
    ForOptions(L, 2, what, "id、source、tag、stat、mine", [&](const char* key) {
        const int v = lua_gettop(L);
        if (std::strcmp(key, "id") == 0) {
            id = TextArg(L, v, what, "id", false);
            byId = true;
        } else if (std::strcmp(key, "tag") == 0) {
            tag = TextArg(L, v, what, "tag", true);
            byTag = true;
        } else if (std::strcmp(key, "source") == 0) {
            source = EnumArg(L, v, DD_ENUM_SOURCE, what, "source");
            bySource = true;
        } else if (std::strcmp(key, "stat") == 0) {
            stat = EnumArg(L, v, DD_ENUM_STAT, what, "stat");
            byStat = true;
        } else if (std::strcmp(key, "mine") == 0) {
            mine = BoolArg(L, v, what, "mine");
            byMine = true;
        } else {
            return false;
        }
        return true;
    });
    const DDCombatService& svc = Svc(L);
    std::uint32_t count = 0;
    if (!svc.buff_list(&actor, nullptr, 0, &count)) {
        return PushRejected(L, ReadsBlocked(svc) ? std::string(kLoadingReason) : "actor 句柄解析失败（或不在存档里）：" + actor::Describe(actor));
    }
    std::vector<std::uint32_t> guids(count);
    if (count) {
        svc.buff_list(&actor, guids.data(), count, &count);
        guids.resize((std::min)(static_cast<std::size_t>(count), guids.size()));
    }
    lua_createtable(L, static_cast<int>(guids.size()), 0);
    lua_Integer n = 0;
    for (const auto guid : guids) {
        DDBuffInfo info{};
        info.size = sizeof(info);
        if (!svc.buff_get(&actor, guid, &info)) {
            continue;
        }
        std::string modId, userTag;
        if (info.by_plugin) {
            SplitTag(Fixed(info.plugin), Fixed(info.tag), &modId, &userTag);
        }
        if ((byId && Fixed(info.id) != id) || (bySource && info.source != source) || (byStat && info.stat_type != stat) ||
            (byTag && (modId != mod.id() || userTag != tag)) || (byMine && (modId == mod.id()) != mine)) {
            continue;
        }
        PushBuffInfo(L, info, actor, mod.id());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// actor:buff(guid)
int ActorBuff(lua_State* L)
{
    constexpr const char* what = "actor:buff";
    Mod& mod = CallerMod(L, what);
    const DDActorRef actor = ActorArg(L, 1, what, "self");
    const auto guid = static_cast<std::uint32_t>(IntArg(L, 2, what, "guid", 0, 0xFFFFFFFFll));
    DDBuffInfo info{};
    info.size = sizeof(info);
    const DDCombatService& svc = Svc(L);
    if (!svc.buff_get(&actor, guid, &info)) {
        // 条目不在、句柄解析失败：只返回 nil；加载中：nil 和原因（R2A-S1：这时的 nil 不能当成「条目已经没了」）
        if (ReadsBlocked(svc)) {
            return PushRejected(L, kLoadingReason);
        }
        lua_pushnil(L);
        return 1;
    }
    PushBuffInfo(L, info, actor, mod.id());
    return 1;
}

// actor:add_buff(id[, opts])
int ActorAddBuff(lua_State* L)
{
    constexpr const char* what = "actor:add_buff";
    Mod& mod = CallerMod(L, what);
    CheckWrite(L, mod, what);
    const DDActorRef actor = ActorArg(L, 1, what, "self");
    const std::string id = TextArg(L, 2, what, "id", false);
    const DDCombatService& svc = Svc(L);
    DDBuffAddOptions options{};
    options.size = sizeof(options);
    std::string tag;
    int stateIndex = 0, doneIndex = 0;
    ForOptions(L, 3, what, "duration、duration_type、source、sourcedata、tag、refresh、state、done", [&](const char* key) {
        const int v = lua_gettop(L);
        if (std::strcmp(key, "duration") == 0) {
            options.duration = static_cast<std::int32_t>(IntArg(L, v, what, "duration", -1, std::numeric_limits<std::int32_t>::max()));
            if (options.duration == 0) {
                luaL_error(L, "%s 的 duration 只能是 -1（不计时）或大于 0", what);
            }
            options.flags |= DD_BUFF_ADD_DURATION;
        } else if (std::strcmp(key, "duration_type") == 0) {
            options.duration_type = EnumArg(L, v, DD_ENUM_DURATION, what, "duration_type");
            options.flags |= DD_BUFF_ADD_DURATION_TYPE;
        } else if (std::strcmp(key, "source") == 0) {
            options.source = EnumArg(L, v, DD_ENUM_SOURCE, what, "source");
            options.flags |= DD_BUFF_ADD_SOURCE;
        } else if (std::strcmp(key, "sourcedata") == 0) {
            options.sourcedata = static_cast<std::int32_t>(
                IntArg(L, v, what, "sourcedata", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
            options.flags |= DD_BUFF_ADD_SOURCEDATA;
        } else if (std::strcmp(key, "tag") == 0) {
            tag = TextArg(L, v, what, "tag", true);
        } else if (std::strcmp(key, "refresh") == 0) {
            if (!BoolArg(L, v, what, "refresh")) {
                options.flags |= DD_BUFF_ADD_NO_REFRESH;
            }
        } else if (std::strcmp(key, "state") == 0) {
            luaL_checktype(L, v, LUA_TTABLE);
            stateIndex = 1;
        } else if (std::strcmp(key, "done") == 0) {
            luaL_checktype(L, v, LUA_TFUNCTION);
            doneIndex = 1;
        } else {
            return false;
        }
        return true;
    });
    CombatBinding::Impl& b = B(L);
    CombatBinding::Impl::ScriptDef* def = b.FindScript(id);
    if (stateIndex && !def) {
        return luaL_error(L, "%s 的 state 只能给脚本 Buff（dd.buff.define 带回调定义的 id），%s 不是", what, id.c_str());
    }
    if (def && b.StatesOfMod(def->modToken) >= SessionOf(L).config().maxPerMod) {
        return PushRejected(L, "脚本 Buff 的状态达到上限 " + std::to_string(SessionOf(L).config().maxPerMod) + " 条（DDLua.cfg 的 max_per_mod）");
    }
    const std::string composed = ComposeTag(L, mod, tag, what);
    int stateRef = LUA_NOREF;
    if (stateIndex) {
        lua_getfield(L, 3, "state");
        std::string json, why;
        if (!state_json::Encode(L, -1, "state", json, &why)) {
            return luaL_error(L, "%s 的 state 不能保存成 JSON：%s", what, why.c_str());
        }
        stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int doneRef = LUA_NOREF;
    if (doneIndex) {
        lua_getfield(L, 3, "done");
        doneRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (PendingFull(L, b, mod)) {
        UnrefIn(L, stateRef);
        UnrefIn(L, doneRef);
        return PushRejected(L, "本 Mod 排队的写操作超过 " + std::to_string(kMaxPendingPerMod) + " 个");
    }
    options.tag = composed.c_str();
    CombatBinding::Impl::PendingAdd pa;
    pa.actor = actor;
    pa.id = id;
    pa.tag = composed;
    pa.stateRef = stateRef;
    if (def) {
        b.pendingAdd = &pa;
    }
    DDCombatOpResult out{};
    out.size = sizeof(out);
    const std::uint32_t code = svc.buff_add(SessionOf(L).host().plugin, &actor, id.c_str(), &options, &out);
    b.pendingAdd = nullptr;
    After(L);
    if (code == DD_COMBAT_OK) {
        UnrefIn(L, doneRef);
        if (def && !pa.consumed) {
            // buff.add 的 hook 还没装上（定义后的第一帧）：在这里绑定、调用 on_apply
            def = b.FindScript(id);
            if (def && b.Bind(actor, out.guid, *def, pa.stateRef)) {
                pa.stateRef = LUA_NOREF;
                b.CallApply(actor, out.guid, { id, options.duration });
                After(L);
            }
        }
        UnrefIn(L, pa.stateRef);
        lua_pushboolean(L, 1);
        lua_pushinteger(L, out.guid);
        return Finish(L, 2);
    }
    if (code == DD_COMBAT_QUEUED) {
        CombatBinding::Impl::PendingOp p;
        p.kind = DD_COMBAT_OP_BUFF_ADD;
        p.doneRef = doneRef;
        p.actor = actor;
        p.id = id;
        p.stateRef = pa.stateRef;
        AddPending(L, b, mod, out.op, std::move(p));
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return Finish(L, 2);
    }
    UnrefIn(L, pa.stateRef);
    UnrefIn(L, doneRef);
    return Finish(L, PushRejected(L, Reason(svc, code, out)));
}

// actor:remove_buffs(filter)：guid、tag、id、source 中的一个，另可带 done
int ActorRemoveBuffs(lua_State* L)
{
    constexpr const char* what = "actor:remove_buffs";
    Mod& mod = CallerMod(L, what);
    CheckWrite(L, mod, what);
    const DDActorRef actor = ActorArg(L, 1, what, "self");
    luaL_checktype(L, 2, LUA_TTABLE);
    DDBuffRemoveSpec spec{};
    spec.size = sizeof(spec);
    std::string text;
    int kinds = 0;
    int doneIndex = 0;
    ForOptions(L, 2, what, "guid、tag、id、source（只能给一个）、done", [&](const char* key) {
        const int v = lua_gettop(L);
        if (std::strcmp(key, "guid") == 0) {
            spec.match = DD_BUFF_BY_GUID;
            spec.guid = static_cast<std::uint32_t>(IntArg(L, v, what, "guid", 0, 0xFFFFFFFFll));
            ++kinds;
        } else if (std::strcmp(key, "tag") == 0) {
            spec.match = DD_BUFF_BY_TAG;
            text = ComposeTag(L, mod, TextArg(L, v, what, "tag", true), what);
            ++kinds;
        } else if (std::strcmp(key, "id") == 0) {
            spec.match = DD_BUFF_BY_ID;
            text = TextArg(L, v, what, "id", false);
            ++kinds;
        } else if (std::strcmp(key, "source") == 0) {
            spec.match = DD_BUFF_BY_SOURCE;
            spec.source = EnumArg(L, v, DD_ENUM_SOURCE, what, "source");
            ++kinds;
        } else if (std::strcmp(key, "done") == 0) {
            luaL_checktype(L, v, LUA_TFUNCTION);
            doneIndex = 1;
        } else {
            return false;
        }
        return true;
    });
    if (kinds != 1) {
        return luaL_error(L, "%s 的条件要在 guid、tag、id、source 里恰好给一个", what);
    }
    spec.text = text.empty() ? nullptr : text.c_str();
    const DDCombatService& svc = Svc(L);
    CombatBinding::Impl& b = B(L);
    int doneRef = LUA_NOREF;
    if (doneIndex) {
        lua_getfield(L, 2, "done");
        doneRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (PendingFull(L, b, mod)) {
        UnrefIn(L, doneRef);
        return PushRejected(L, "本 Mod 排队的写操作超过 " + std::to_string(kMaxPendingPerMod) + " 个");
    }
    DDCombatOpResult out{};
    out.size = sizeof(out);
    const std::uint32_t code = svc.buff_remove(SessionOf(L).host().plugin, &actor, &spec, &out);
    After(L);
    if (code == DD_COMBAT_QUEUED) {
        CombatBinding::Impl::PendingOp p;
        p.kind = DD_COMBAT_OP_BUFF_REMOVE;
        p.doneRef = doneRef;
        p.actor = actor;
        AddPending(L, b, mod, out.op, std::move(p));
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return Finish(L, 2);
    }
    UnrefIn(L, doneRef);
    if (code != DD_COMBAT_OK) {
        return Finish(L, PushRejected(L, Reason(svc, code, out)));
    }
    lua_pushboolean(L, 1);
    lua_pushinteger(L, out.count);
    return Finish(L, 2);
}

}  // namespace combat

// ============================================================================

CombatBinding::CombatBinding(Session& session) : impl_(std::make_unique<Impl>(session, *this))
{
    if (const DDCombatService* svc = combat::Service(session.host())) {
        svc->set_op_handler(session.host().plugin, &OpThunk, nullptr);  // 同一个函数，两个会话都设置也一样
    }
}

CombatBinding::~CombatBinding()
{
    impl_->Clear(impl_->VmOk() != nullptr);
}

void CombatBinding::RegisterMetatables(lua_State* L)
{
    NewMeta(L, kEffectMeta, EffectIndex, EffectNewIndex, EffectEq, EffectToString);
    NewMeta(L, kSkillMeta, SkillIndex, SkillNewIndex, SkillEq, SkillToString);
    NewMeta(L, kRuleMeta, RuleIndex, nullptr, nullptr, RuleToString);
    // effect 句柄的驻留表：名字 -> userdata，弱值
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kEffectIntern);
}

void CombatBinding::AddFunctions(lua_State* L, int dd, std::uint64_t token)
{
    lua_createtable(L, 0, 5);
    const int buff = lua_gettop(L);
    SetClosure(L, buff, "define", BuffDefine, token);
    SetClosure(L, buff, "def", BuffDef, token);
    SetClosure(L, buff, "enum", BuffEnum, token);
    SetClosure(L, buff, "state", BuffState, token);
    SetClosure(L, buff, "rule", BuffRule, token);
    lua_setfield(L, dd, "buff");
    lua_createtable(L, 0, 3);
    const int effect = lua_gettop(L);
    SetClosure(L, effect, "get", EffectGet, token);
    SetClosure(L, effect, "define", EffectDefine, token);
    SetClosure(L, effect, "register", EffectRegister, token);
    lua_setfield(L, dd, "effect");
}

void CombatBinding::ReleaseMod(const Mod& mod)
{
    Impl& b = *impl_;
    const DDCombatService* svc = b.Svc();
    for (auto it = b.rules.begin(); it != b.rules.end();) {
        if (it->second.modToken != mod.token) {
            ++it;
            continue;
        }
        if (svc && it->second.id) {
            svc->rule_unregister(b.session.host().plugin, it->second.id);
        }
        RuleRoutes().erase(it->first);
        b.Unref(it->second.fnRef);
        it = b.rules.erase(it);
    }
    for (auto it = b.pending.begin(); it != b.pending.end();) {
        if (it->second.modToken != mod.token) {
            ++it;
            continue;
        }
        b.Unref(it->second.doneRef);
        b.Unref(it->second.stateRef);
        OpRoutes().erase(it->first);
        it = b.pending.erase(it);
    }
    for (auto it = b.states.begin(); it != b.states.end();) {
        if (it->second.modToken != mod.token) {
            ++it;
            continue;
        }
        b.Unref(it->second.stateRef);
        it = b.states.erase(it);
    }
    for (auto it = b.defs.begin(); it != b.defs.end();) {
        if (it->second.modToken != mod.token) {
            ++it;
            continue;
        }
        for (int& ref : it->second.ref) {
            b.Unref(ref);
        }
        it = b.defs.erase(it);
    }
    for (auto it = b.effectOwners.begin(); it != b.effectOwners.end();) {
        it = it->second.modToken == mod.token ? b.effectOwners.erase(it) : std::next(it);
    }
    if (!b.pendingAdd) {
        b.UpdateSubs();
    }
}

void CombatBinding::Tick(bool sweep)
{
    Impl& b = *impl_;
    for (auto& [token, use] : b.frameUse) {
        use.us = 0.0;
        use.skipped = 0;
    }
    // 停用了的规则（回调里不能撤销）在帧边界向 DDCore 撤销，之后由游戏判定
    for (auto it = b.rules.begin(); it != b.rules.end();) {
        if (!it->second.disabled) {
            ++it;
            continue;
        }
        if (const DDCombatService* svc = b.Svc(); svc && it->second.id) {
            svc->rule_unregister(b.session.host().plugin, it->second.id);
        }
        b.session.LogId(DD_LOG_INFO, it->second.modId, "规则 " + it->second.name + " 已向 DDCore 撤销，之后由游戏按原样判定");
        RuleRoutes().erase(it->first);
        b.Unref(it->second.fnRef);
        it = b.rules.erase(it);
    }
    if (sweep && !b.states.empty()) {
        b.SweepGone();
    }
}

void CombatBinding::BeforeTeardown()
{
    Impl& b = *impl_;
    if (!b.VmOk()) {
        return;
    }
    // 拆除时不调用任何 on_remove，也不按 gone 清扫（F7）：载体还在游戏存档里，状态在之前的 campaign.save 已写进旁路。
    // DDCore 在派发 CampaignUnload 之前就把存档标成不活跃，这时英雄句柄全都解析不了，清扫会把它们都当成 gone
    const std::size_t left = b.states.size();
    for (auto& [key, binding] : b.states) {
        b.Unref(binding.stateRef);
    }
    b.states.clear();
    if (left) {
        b.session.Log(DD_LOG_INFO, nullptr, "脚本 Buff：退出存档，丢弃 " + std::to_string(left) + " 条载体的状态（载体还在游戏存档里，不调用 on_remove）");
    }
}

std::size_t CombatBinding::PendingFor(const Mod* mod) const
{
    std::size_t n = 0;
    for (const auto& [op, p] : impl_->pending) {
        n += (!mod || p.modToken == mod->token) && p.doneRef != LUA_NOREF ? 1 : 0;
    }
    return n;
}

std::size_t CombatBinding::RulesFor(const Mod* mod) const
{
    std::size_t n = 0;
    for (const auto& [key, rule] : impl_->rules) {
        n += !mod || rule.modToken == mod->token ? 1 : 0;
    }
    return n;
}

std::size_t CombatBinding::StatesFor(const Mod* mod) const
{
    std::size_t n = 0;
    for (const auto& [key, binding] : impl_->states) {
        n += !mod || binding.modToken == mod->token ? 1 : 0;
    }
    return n;
}

std::size_t CombatBinding::InternalSubscriptions() const
{
    const Impl& b = *impl_;
    return (b.subAdd ? 1 : 0) + (b.subRemove ? 1 : 0) + (b.subTick ? 1 : 0) + (b.subDamage ? 1 : 0);
}

std::vector<CombatBinding::StateRecord> CombatBinding::ExportStates(std::vector<std::string>* errors, std::size_t perModBytes,
    std::vector<std::string>* overLimit)
{
    Impl& b = *impl_;
    std::vector<StateRecord> out;
    Vm* vm = b.VmOk();
    if (!vm) {
        return out;
    }
    std::map<std::string, std::size_t> used;  // 每个 Mod 已经导出的字节数（perModBytes 不为 0 时）
    std::vector<std::string> over;
    for (const auto& [key, binding] : b.states) {
        if (std::find(over.begin(), over.end(), binding.modId) != over.end()) {
            continue;
        }
        StateRecord record;
        record.mod = binding.modId;
        record.buffId = binding.buffId;
        record.actor = binding.actor;
        record.guid = binding.guid;
        std::string why;
        bool encoded = false, tooLarge = false;
        const std::size_t room = perModBytes ? perModBytes - (std::min)(perModBytes, used[binding.modId]) : state_json::kMaxBytes;
        const bool ran = vm->Protected(
            [&](lua_State* L) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, binding.stateRef);
                encoded = state_json::Encode(L, -1, "state", record.json, &why, room, &tooLarge);
                lua_pop(L, 1);
            },
            &why);
        if (ran && encoded) {
            used[binding.modId] += record.json.size();
            out.push_back(std::move(record));
        } else if (perModBytes && tooLarge && room < state_json::kMaxBytes) {
            over.push_back(binding.modId);
        } else if (errors) {
            errors->push_back("[" + binding.modId + "] " + binding.buffId + "（" + actor::Describe(binding.actor) + "，guid " +
                std::to_string(binding.guid) + "）：" + why);
        }
    }
    if (!over.empty()) {
        std::erase_if(out, [&](const StateRecord& r) { return std::find(over.begin(), over.end(), r.mod) != over.end(); });
        if (overLimit) {
            *overLimit = std::move(over);
        }
    }
    return out;
}

bool CombatBinding::ImportState(const StateRecord& record, std::string* error)
{
    Impl& b = *impl_;
    Vm* vm = b.VmOk();
    const auto fail = [&](std::string why) {
        if (error) {
            *error = std::move(why);
        }
        return false;
    };
    if (!vm) {
        return fail("VM 不可用");
    }
    Impl::ScriptDef* def = b.FindScript(record.buffId);
    if (!def || def->modId != record.mod) {
        return fail("Mod " + record.mod + " 没有定义脚本 Buff " + record.buffId);
    }
    const DDCombatService* svc = b.Svc();
    DDBuffInfo info{};
    info.size = sizeof(info);
    if (!svc || !svc->buff_get(&record.actor, record.guid, &info) || Fixed(info.id) != record.buffId) {
        return fail("条目不存在或 id 不一致（" + actor::Describe(record.actor) + "，guid " + std::to_string(record.guid) + "）");
    }
    int ref = LUA_NOREF;
    std::string why;
    bool decoded = false;
    if (!vm->Protected(
            [&](lua_State* L) {
                decoded = state_json::Decode(L, record.json, &why);
                if (decoded) {
                    ref = luaL_ref(L, LUA_REGISTRYINDEX);
                }
            },
            &why) ||
        !decoded) {
        return fail("状态解析失败：" + why);
    }
    return b.Bind(record.actor, record.guid, *def, ref) ? true : fail("绑定失败");
}

void CombatBinding::OnOpResult(const DDCombatOpResult& r)
{
    Impl& b = *impl_;
    const auto it = b.pending.find(r.op);
    if (it == b.pending.end()) {
        return;
    }
    Impl::PendingOp p = std::move(it->second);
    b.pending.erase(it);
    const bool ok = r.result == DD_COMBAT_OK;
    const std::string message = r.message && *r.message ? r.message : (b.Svc() ? b.Svc()->result_text(r.result) : "失败");
    const bool alive = b.session.FindToken(p.modToken) != nullptr;
    switch (p.kind) {
    case DD_COMBAT_OP_BUFF_ADD:
        if (ok && alive) {
            Impl::ScriptDef* def = b.FindScript(p.id);
            DDBuffInfo info{};
            info.size = sizeof(info);
            const DDCombatService* svc = b.Svc();
            if (def && svc && svc->buff_get(&p.actor, r.guid, &info) && b.Bind(p.actor, r.guid, *def, p.stateRef)) {
                p.stateRef = LUA_NOREF;
                b.CallApply(p.actor, r.guid, { p.id, info.duration });
            }
        }
        break;
    case DD_COMBAT_OP_BUFF_DEFINE:
        if (!ok && p.created) {
            // 只撤销这次新建的定义槽；更新已有定义失败时 DDCore 里原来的定义还在，回调保留。
            // 同一个 id 后面还有排队的定义时不撤销，把「新建」交给后面那次（排队顺序就是操作号的顺序）：
            // 后面那次也失败时由它撤销，不留下 DDCore 里没有对应定义的槽（审核 R2A-N6）
            const auto later = std::find_if(b.pending.begin(), b.pending.end(), [&](const auto& item) {
                return item.second.kind == DD_COMBAT_OP_BUFF_DEFINE && item.second.id == p.id && item.second.modToken == p.modToken;
            });
            if (later != b.pending.end()) {
                later->second.created = true;
            }
            const auto d = b.defs.find(p.id);
            if (later == b.pending.end() && d != b.defs.end() && d->second.modToken == p.modToken) {
                for (int& ref : d->second.ref) {
                    b.Unref(ref);
                }
                b.defs.erase(d);
                b.UpdateSubs();
            }
        }
        break;
    case DD_COMBAT_OP_EFFECT_DEFINE:
        if (!ok) {
            for (const auto& name : p.effectNames) {
                const auto o = b.effectOwners.find(ddc::combat::GameHash(name));
                if (o != b.effectOwners.end() && o->second.modToken == p.modToken) {
                    b.effectOwners.erase(o);
                }
            }
        }
        break;
    default:
        break;
    }
    b.Unref(p.stateRef);
    if (p.doneRef != LUA_NOREF && alive) {
        bool budget = false;
        std::string error;
        const bool called = b.Invoke(p.doneRef, p.modToken, p.modId, b.session.config().budgetMs, "写操作的 done 回调",
            [&](lua_State* L) {
                if (!ok) {
                    lua_pushnil(L);
                    lua_pushlstring(L, message.data(), message.size());
                    return 2;
                }
                lua_pushboolean(L, 1);
                switch (p.kind) {
                case DD_COMBAT_OP_BUFF_ADD:
                    lua_pushinteger(L, r.guid);
                    break;
                case DD_COMBAT_OP_BUFF_REMOVE:
                case DD_COMBAT_OP_EFFECT_DEFINE:
                    lua_pushinteger(L, r.count);
                    break;
                case DD_COMBAT_OP_EFFECT_APPLY:
                    lua_createtable(L, 0, 1);
                    lua_pushinteger(L, r.count);
                    lua_setfield(L, -2, "count");
                    break;
                default:
                    lua_pushlstring(L, p.id.data(), p.id.size());
                    break;
                }
                return 2;
            },
            &budget, &error);
        if (!called && !error.empty()) {
            b.session.LogId(DD_LOG_ERROR, p.modId, std::string("写操作的 done 回调出错") + (budget ? "（超出执行预算）" : "") + "：" + error);
        }
    }
    b.Unref(p.doneRef);
}

std::uint32_t CombatBinding::OnRule(std::uint64_t key, const DDRuleContext& context)
{
    Impl& b = *impl_;
    try {
        const auto it = b.rules.find(key);
        if (it == b.rules.end() || it->second.disabled) {
            return DD_RULE_ORIGINAL;
        }
        Session& session = b.session;
        Vm* vm = session.vm();
        if (!vm || !session.CheckVmHealth(false)) {
            return DD_RULE_ORIGINAL;
        }
        const Mod* mod = session.FindToken(it->second.modToken);
        if (!mod) {
            return DD_RULE_ORIGINAL;
        }
        if (b.OverFrame(it->second.modToken, it->second.modId, "规则交还原函数")) {
            return DD_RULE_ORIGINAL;
        }
        const std::uint64_t modToken = it->second.modToken;
        const int fnRef = it->second.fnRef;
        const std::string modId = it->second.modId;
        const std::string name = it->second.name;
        const DDGameService* game = actor::Service(session.host());
        lua_State* L = vm->L();
        const int top = lua_gettop(L);
        std::string error;
        const auto pushActor = [&](lua_State* L, void* pointer) {
            DDActorRef ref{};
            ref.size = sizeof(ref);
            if (pointer && game && game->actor_from_pointer(pointer, &ref)) {
                actor::Push(L, ref);
            } else {
                lua_pushnil(L);
            }
        };
        if (!vm->Protected(
                [&](lua_State* L) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
                    pushActor(L, context.actor);
                    pushActor(L, context.target);
                    lua_createtable(L, 0, 6);
                    lua_pushstring(L, context.buff_id ? context.buff_id : "");
                    lua_setfield(L, -2, "id");
                    lua_pushstring(L, context.rule ? context.rule : "");
                    lua_setfield(L, -2, "rule_string");
                    PushEnum(L, DD_ENUM_RULE, context.rule_type);
                    lua_setfield(L, -2, "rule");
                    lua_pushnumber(L, context.rule_float);
                    lua_setfield(L, -2, "rule_float");
                    lua_pushboolean(L, context.is_false_rule);
                    lua_setfield(L, -2, "is_false_rule");
                    pushActor(L, context.subject);
                    lua_setfield(L, -2, "subject");
                },
                &error)) {
            session.LogId(DD_LOG_ERROR, modId, "规则 " + name + " 的回调没能开始：" + error);
            return DD_RULE_ORIGINAL;
        }
        bool budget = false;
        bool ok;
        {
            const Session::Running running(session, mod);
            const Session::RuleScope scope(session);
            const double t0 = Impl::QpcUs();
            ok = vm->Call(3, 1, session.config().ruleBudgetMs, &error, &budget);
            b.ChargeFrame(modToken, Impl::QpcUs() - t0);
        }
        if (!vm->ok()) {
            return DD_RULE_ORIGINAL;
        }
        // 返回 true / false：规则成立 / 不成立（DDCore 再按 is_false_rule 取反）；返回 nil（或不返回）：交还游戏原来的判定
        // （DD_RULE_ORIGINAL，不算出错，Y6）；别的类型算出错
        std::uint32_t verdict = DD_RULE_ORIGINAL;
        if (ok) {
            if (lua_type(L, -1) == LUA_TBOOLEAN) {
                verdict = lua_toboolean(L, -1) != 0 ? DD_RULE_TRUE : DD_RULE_FALSE;
            } else if (!lua_isnil(L, -1)) {
                ok = false;
                error = std::string("规则回调必须返回布尔或 nil，返回了 ") + luaL_typename(L, -1);
            }
        }
        lua_settop(L, top);
        const auto again = b.rules.find(key);  // 回调里可能 off() 了自己
        if (again == b.rules.end()) {
            return ok ? verdict : DD_RULE_ORIGINAL;
        }
        Impl::Rule& rule = again->second;
        if (ok) {
            rule.failures = 0;
            return verdict;
        }
        ++rule.failures;
        const std::uint32_t limit = session.config().maxFailures;
        std::string text = "规则 " + name + " 的回调出错";
        if (budget || rule.failures >= limit) {
            rule.disabled = true;
            text += budget ? "（超出 rule_budget_ms）" : "（连续 " + std::to_string(rule.failures) + " 次）";
            text += "，已停用：之后交还游戏原来的判定，下一帧向 DDCore 撤销";
        } else {
            text += "（连续第 " + std::to_string(rule.failures) + " 次，" + std::to_string(limit) + " 次后停用；这次交还游戏原来的判定）";
        }
        session.LogId(DD_LOG_ERROR, modId, text + "：" + error);
        return DD_RULE_ORIGINAL;
    } catch (const std::exception& e) {
        b.session.Log(DD_LOG_ERROR, nullptr, std::string("规则回调出错：") + e.what());
    } catch (...) {
        b.session.Log(DD_LOG_ERROR, nullptr, "规则回调出错：未知的 C++ 异常");
    }
    return DD_RULE_ORIGINAL;
}

void CombatBinding::NoteNativeFault(unsigned long code, const void* address)
{
    impl_->session.NoteNativeFault("combat 回调", code, address);
}

}  // namespace ddlua

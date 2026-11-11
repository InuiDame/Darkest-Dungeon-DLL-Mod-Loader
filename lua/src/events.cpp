#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "events.h"

#include "actor.h"
#include "sandbox.h"
#include "scheduler.h"
#include "session.h"
#include "vm.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <limits>

namespace ddlua {

namespace {

constexpr const char* kEventMeta = "dd.event";
constexpr const char* kSubMeta = "dd.subscription";
constexpr std::size_t kMaxFilterItems = 16;
constexpr std::size_t kMaxTopicInReason = 200;  // 失败原因里引用 Mod 给的主题名，最多这么多字节

// SDK 的 user 是一个键，回调按键找到所属的绑定。键全进程唯一，订阅撤销后从表里删掉，迟到的回调查不到就忽略
std::uint64_t g_nextKey = 1;

std::unordered_map<std::uint64_t, EventBinding*>& Routes()
{
    static auto* routes = new std::unordered_map<std::uint64_t, EventBinding*>();  // 不析构，进程退出时不碰
    return *routes;
}

std::uint64_t KeyOf(void* user)
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(user));
}

// 第二轮审核 R2A-应修2：Lua 回调执行期间发生原生异常（SEH）时，DDCore 只对仍然有效的订阅发撤销通知。dd.once（调用前
// 已撤销）或回调里先 off() 了自己，DDLua 收不到通知，VM 要到下一帧才被发现损坏，这期间照常派发。
// 这里的过滤器在异常的第一遍搜索里先给会话置损坏标记，然后返回 EXCEPTION_CONTINUE_SEARCH，异常照常交给 DDCore 的隔离
// 处理（撤销订阅、写 DDCore.log），不改 DDCore 的 ABI。过滤器里不分配、不写日志，下一次进入 DDLua 时补日志。
// C++ 异常（0xE06D7363）不管：OnEvent 自己接住所有 C++ 异常，Lua 的错误在 lua_pcall 里接住，都到不了这里
constexpr unsigned long kCxxException = 0xE06D7363;

int FilterEventFault(EventBinding* binding, EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode != kCxxException) {
        binding->NoteNativeFault(record->ExceptionCode, record->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 函数里不能有需要析构的对象（__try 的限制）
void DispatchGuarded(EventBinding* binding, std::uint64_t key, DDEvent* event)
{
    __try {
        binding->OnEvent(key, event);
    } __except (FilterEventFault(binding, GetExceptionInformation())) {
        // 不会到这里：过滤器总是继续搜索
    }
}

void DD_CALL EventThunk(DDEvent* event, void* user)
{
    const auto it = Routes().find(KeyOf(user));
    if (it != Routes().end()) {
        DispatchGuarded(it->second, it->first, event);
    }
}

void DD_CALL EndThunk(DDEventSubId, void* user, std::uint32_t reason)
{
    const auto it = Routes().find(KeyOf(user));
    if (it != Routes().end()) {
        it->second->OnEnd(it->first, reason);
    }
}

// 负载 userdata：回调返回后 event 置空，之后的访问都报错
struct PayloadUd {
    DDEvent* event;
    const TopicMeta* topic;
};

struct SubUd {
    std::uint64_t key;
};

const char* TypeName(std::uint32_t type)
{
    switch (type) {
    case DD_FIELD_INT:
        return "整数";
    case DD_FIELD_FLOAT:
        return "数字";
    case DD_FIELD_BOOL:
        return "布尔";
    case DD_FIELD_STRING:
        return "字符串";
    case DD_FIELD_POINTER:
        return "指针";
    default:
        return "未知类型";
    }
}

const char* ResultText(DDEventResult result)
{
    switch (result) {
    case DD_EV_OK:
        return "成功";
    case DD_EV_E_INVALID:
        return "事件或参数无效";
    case DD_EV_E_NOT_FOUND:
        return "字段不存在";
    case DD_EV_E_TYPE:
        return "类型不符";
    case DD_EV_E_READ_ONLY:
        return "只读";
    case DD_EV_E_NOT_CANCELLABLE:
        return "不可取消";
    case DD_EV_E_RANGE:
        return "超出字段的取值范围";
    default:
        return "未知错误";
    }
}

const char* ScopeName(std::uint32_t scope, bool actorScope = false)
{
    if (actorScope) {
        return "actor";
    }
    switch (scope) {
    case DD_EVENT_SCOPE_CAMPAIGN:
        return "campaign";
    case DD_EVENT_SCOPE_RAID:
        return "raid";
    case DD_EVENT_SCOPE_BATTLE:
        return "battle";
    default:
        return "mod";
    }
}

const DDEventsService& Events(lua_State* L)
{
    const DDEventsService* events = SessionOf(L).host().events;
    if (!events) {
        luaL_error(L, "events 服务不可用");
    }
    return *events;
}

// 读一个字段压栈（在 Lua 的 C 函数里或保护模式内调用，出错时抛 Lua 错误）
void PushFieldValue(lua_State* L, Session& session, DDEvent* event, const TopicMeta& topic, const FieldMeta& field,
    std::uint32_t flags)
{
    const DDEventsService& events = *session.host().events;
    const char* name = field.name.c_str();
    DDEventResult result = DD_EV_OK;
    switch (field.type) {
    case DD_FIELD_INT: {
        std::int64_t value = 0;
        result = events.get_int(event, name, &value);
        lua_pushinteger(L, static_cast<lua_Integer>(value));
        break;
    }
    case DD_FIELD_FLOAT: {
        double value = 0;
        result = events.get_float(event, name, &value);
        lua_pushnumber(L, value);
        break;
    }
    case DD_FIELD_BOOL: {
        bool value = false;
        result = events.get_bool(event, name, &value);
        lua_pushboolean(L, value);
        break;
    }
    case DD_FIELD_STRING: {
        const char* value = nullptr;
        result = events.get_string(event, name, &value);
        lua_pushstring(L, value ? value : "");
        break;
    }
    case DD_FIELD_POINTER: {
        void* value = nullptr;
        result = events.get_pointer(event, name, &value);
        PushPointerField(L, session, topic, field, value, (flags & DD_EVENT_F_DEFERRED) != 0);
        break;
    }
    default:
        lua_pushnil(L);
        break;
    }
    if (result != DD_EV_OK) {
        luaL_error(L, "读取主题 %s 的字段 %s 失败（%s）", topic.name.c_str(), name, ResultText(result));
    }
}

PayloadUd& LivePayload(lua_State* L)
{
    auto* ud = static_cast<PayloadUd*>(luaL_checkudata(L, 1, kEventMeta));
    if (!ud->event) {
        luaL_error(L, "事件负载已失效：只能在回调执行期间访问（主题 %s）。要保存数据请复制字段，或用 dd.wait_event 的快照",
            ud->topic->name.c_str());
    }
    return *ud;
}

int PayloadCancel(lua_State* L)
{
    PayloadUd& ud = LivePayload(L);
    const DDEventsService& events = Events(L);
    if (events.event_flags(ud.event) & DD_EVENT_F_DEFERRED) {
        return luaL_error(L, "延迟事件（加载线程上发生、排队到主线程的）不能取消：%s", ud.topic->name.c_str());
    }
    const DDEventResult result = events.cancel(ud.event);
    if (result == DD_EV_E_NOT_CANCELLABLE) {
        return luaL_error(L, "主题 %s 不能取消", ud.topic->name.c_str());
    }
    if (result != DD_EV_OK) {
        return luaL_error(L, "取消 %s 失败（%s）", ud.topic->name.c_str(), ResultText(result));
    }
    return 0;
}

bool IsMetaName(const char* key)
{
    return std::strcmp(key, "topic") == 0 || std::strcmp(key, "deferred") == 0 || std::strcmp(key, "cancelled") == 0 ||
        std::strcmp(key, "writable") == 0 || std::strcmp(key, "cancel") == 0;
}

// ev.<字段>：主题的字段优先，然后是元信息 topic、deferred、cancelled、writable 和方法 cancel
int PayloadIndex(lua_State* L)
{
    PayloadUd& ud = LivePayload(L);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "事件的字段名必须是字符串");
    }
    const char* key = lua_tostring(L, 2);
    Session& session = SessionOf(L);
    const DDEventsService& events = Events(L);
    const std::uint32_t flags = events.event_flags(ud.event);
    if (const FieldMeta* field = ud.topic->Find(key)) {
        PushFieldValue(L, session, ud.event, *ud.topic, *field, flags);
        return 1;
    }
    if (std::strcmp(key, "topic") == 0) {
        lua_pushstring(L, ud.topic->name.c_str());
    } else if (std::strcmp(key, "deferred") == 0) {
        lua_pushboolean(L, (flags & DD_EVENT_F_DEFERRED) != 0);
    } else if (std::strcmp(key, "cancelled") == 0) {
        lua_pushboolean(L, (flags & DD_EVENT_F_CANCELLED) != 0);
    } else if (std::strcmp(key, "writable") == 0) {
        lua_pushboolean(L, (flags & DD_EVENT_F_WRITABLE) != 0);
    } else if (std::strcmp(key, "cancel") == 0) {
        lua_pushcfunction(L, PayloadCancel);
    } else {
        return luaL_error(L, "主题 %s 没有字段 %s", ud.topic->name.c_str(), key);
    }
    return 1;
}

// ev.<字段> = 值：只有 .pre 和 query.* 的可写字段，类型必须一致，指针和字符串字段不能写
int PayloadNewIndex(lua_State* L)
{
    PayloadUd& ud = LivePayload(L);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "事件的字段名必须是字符串");
    }
    const char* key = lua_tostring(L, 2);
    const TopicMeta& topic = *ud.topic;
    const FieldMeta* field = topic.Find(key);
    if (!field) {
        if (IsMetaName(key)) {
            return luaL_error(L, "%s 是事件的元信息，不能赋值", key);
        }
        return luaL_error(L, "主题 %s 没有字段 %s", topic.name.c_str(), key);
    }
    const DDEventsService& events = Events(L);
    const std::uint32_t flags = events.event_flags(ud.event);
    if (flags & DD_EVENT_F_DEFERRED) {
        return luaL_error(L, "延迟事件（加载线程上发生、排队到主线程的）只读：%s.%s", topic.name.c_str(), key);
    }
    if (!(flags & DD_EVENT_F_WRITABLE)) {
        return luaL_error(L, "主题 %s 只读，只有 .pre 和 query.* 主题可写", topic.name.c_str());
    }
    if (!(field->access & DD_FIELD_WRITE)) {
        return luaL_error(L, "主题 %s 的字段 %s 只读", topic.name.c_str(), key);
    }
    const auto mismatch = [&]() {
        return luaL_error(L, "主题 %s 的字段 %s 是%s，不能写入 %s", topic.name.c_str(), key, TypeName(field->type),
            luaL_typename(L, 3));
    };
    // 超出范围由 DDCore 按字段在游戏里的宽度判定（float、int32、uint32，DD_EV_E_RANGE），native 插件同样受限
    const auto outOfRange = [&]() {
        return luaL_error(L, "主题 %s 的字段 %s 在游戏里是 %s，写入的值 %s 超出范围，没有写入", topic.name.c_str(), key,
            field->type == DD_FIELD_FLOAT ? "32 位浮点数" : "32 位整数", lua_tostring(L, -1));
    };
    DDEventResult result = DD_EV_OK;
    switch (field->type) {
    case DD_FIELD_INT: {
        int isInteger = 0;
        const lua_Integer value = lua_type(L, 3) == LUA_TNUMBER ? lua_tointegerx(L, 3, &isInteger) : 0;
        if (!isInteger) {
            if (lua_type(L, 3) == LUA_TNUMBER) {
                const double number = static_cast<double>(lua_tonumber(L, 3));
                if (std::isfinite(number) && std::floor(number) == number) {
                    lua_pushfstring(L, "%f", static_cast<lua_Number>(number));
                    return outOfRange();  // 值是整数，但超出 64 位整数（更超出字段的宽度）
                }
                return luaL_error(L, "主题 %s 的字段 %s 是整数，不能写入非整数的数字", topic.name.c_str(), key);
            }
            return mismatch();
        }
        result = events.set_int(ud.event, key, static_cast<std::int64_t>(value));
        if (result == DD_EV_E_RANGE) {
            lua_pushfstring(L, "%I", static_cast<LUAI_UACINT>(value));
            return outOfRange();
        }
        break;
    }
    case DD_FIELD_FLOAT: {
        if (lua_type(L, 3) != LUA_TNUMBER) {
            return mismatch();
        }
        const double value = static_cast<double>(lua_tonumber(L, 3));
        if (!std::isfinite(value)) {
            return luaL_error(L, "主题 %s 的字段 %s 只能写入有限的数（不能是 nan 或 inf）", topic.name.c_str(), key);
        }
        result = events.set_float(ud.event, key, value);
        if (result == DD_EV_E_RANGE) {
            lua_pushfstring(L, "%f", static_cast<lua_Number>(value));
            return outOfRange();
        }
        break;
    }
    case DD_FIELD_BOOL:
        if (lua_type(L, 3) != LUA_TBOOLEAN) {
            return mismatch();
        }
        result = events.set_bool(ud.event, key, lua_toboolean(L, 3) != 0);
        break;
    case DD_FIELD_STRING:
        return luaL_error(L, "主题 %s 的字段 %s 是字符串，Lua 暂不支持写字符串字段", topic.name.c_str(), key);
    case DD_FIELD_POINTER:
        return luaL_error(L, "主题 %s 的字段 %s 是指针，Lua 不能写指针字段", topic.name.c_str(), key);
    default:
        return mismatch();
    }
    if (result != DD_EV_OK) {
        return luaL_error(L, "写入 %s.%s 失败（%s）", topic.name.c_str(), key, ResultText(result));
    }
    return 0;
}

int PayloadToString(lua_State* L)
{
    auto* ud = static_cast<PayloadUd*>(luaL_checkudata(L, 1, kEventMeta));
    lua_pushfstring(L, "dd.event(%s%s)", ud->topic->name.c_str(), ud->event ? "" : "，已失效");
    return 1;
}

PayloadUd* NewPayload(lua_State* L, DDEvent* event, const TopicMeta* topic)
{
    auto* ud = static_cast<PayloadUd*>(lua_newuserdatauv(L, sizeof(PayloadUd), 0));
    ud->event = event;
    ud->topic = topic;
    luaL_setmetatable(L, kEventMeta);
    return ud;
}

// ---- 订阅对象 ----

SubUd& CheckSub(lua_State* L, int index)
{
    return *static_cast<SubUd*>(luaL_checkudata(L, index, kSubMeta));
}

// sub:off() / dd.off(sub)：已经无效的返回 false；有效的只有所属 Mod 自己的代码能撤销（审核 B-N4）
int OffImpl(lua_State* L, std::uint64_t key, const char* what)
{
    EventBinding& binding = SessionOf(L).events();
    const std::shared_ptr<EventBinding::Sub> sub = binding.Find(key);
    if (!sub) {
        lua_pushboolean(L, 0);
        return 1;
    }
    CheckOwner(L, sub->mod, sub->modId, what);
    lua_pushboolean(L, binding.Remove(key, true));
    return 1;
}

int SubOff(lua_State* L)
{
    SubUd& ud = CheckSub(L, 1);
    return OffImpl(L, ud.key, "sub:off()");
}

int SubActive(lua_State* L)
{
    SubUd& ud = CheckSub(L, 1);
    lua_pushboolean(L, SessionOf(L).events().Active(ud.key));
    return 1;
}

int SubIndex(lua_State* L)
{
    CheckSub(L, 1);
    const char* key = lua_tostring(L, 2);
    if (!key) {
        lua_pushnil(L);
    } else if (std::strcmp(key, "off") == 0) {
        lua_pushcfunction(L, SubOff);
    } else if (std::strcmp(key, "active") == 0) {
        lua_pushcfunction(L, SubActive);
    } else if (std::strcmp(key, "topic") == 0) {
        lua_getiuservalue(L, 1, 1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int SubToString(lua_State* L)
{
    SubUd& ud = CheckSub(L, 1);
    lua_getiuservalue(L, 1, 1);
    lua_pushfstring(L, "dd.subscription(%s%s)", lua_tostring(L, -1), SessionOf(L).events().Active(ud.key) ? "" : "，已撤销");
    return 1;
}

// dd.on / dd.once
int Subscribe(lua_State* L, bool once)
{
    const char* what = once ? "dd.once" : "dd.on";
    Mod& mod = CheckLiveMod(L, what);
    const char* topicName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    std::int32_t priority = 0;
    std::uint32_t scope = DD_EVENT_SCOPE_PLUGIN;
    bool actorScope = false;
    DDActorRef scopeActor{};
    bool hasFilter = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 3)) {  // 原始遍历，不触发 Mod 的元方法
            if (lua_type(L, -2) != LUA_TSTRING) {
                return luaL_error(L, "%s 的选项名必须是字符串", what);
            }
            const char* key = lua_tostring(L, -2);
            if (std::strcmp(key, "priority") == 0) {
                int isInteger = 0;
                const lua_Integer value = lua_type(L, -1) == LUA_TNUMBER ? lua_tointegerx(L, -1, &isInteger) : 0;
                if (!isInteger || value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
                    return luaL_error(L, "%s 的 priority 必须是 32 位整数", what);
                }
                priority = static_cast<std::int32_t>(value);
            } else if (std::strcmp(key, "scope") == 0) {
                const char* value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
                if (value && std::strcmp(value, "mod") == 0) {
                    scope = DD_EVENT_SCOPE_PLUGIN;
                } else if (value && std::strcmp(value, "campaign") == 0) {
                    scope = DD_EVENT_SCOPE_CAMPAIGN;
                } else if (value && std::strcmp(value, "raid") == 0) {
                    scope = DD_EVENT_SCOPE_RAID;
                } else if (value && std::strcmp(value, "battle") == 0) {
                    scope = DD_EVENT_SCOPE_BATTLE;
                } else if (!value && actor::Test(L, -1, &scopeActor)) {
                    // actor 作用域：怪物只活在一场战斗里，SDK 一侧用 battle 作用域（战斗销毁时游戏先清空怪物表）；
                    // 英雄的句柄带存档序号，用 campaign 作用域。另外派发前和每秒检查一次句柄
                    actorScope = true;
                    scope = scopeActor.kind == DD_ACTOR_MONSTER ? DD_EVENT_SCOPE_BATTLE : DD_EVENT_SCOPE_CAMPAIGN;
                } else {
                    return luaL_error(L, "%s 的 scope 只能是 \"mod\"、\"campaign\"、\"raid\"、\"battle\" 或 actor 句柄", what);
                }
            } else if (std::strcmp(key, "filter") == 0) {
                if (lua_type(L, -1) != LUA_TTABLE) {
                    return luaL_error(L, "%s 的 filter 必须是表", what);
                }
                hasFilter = true;
            } else {
                return luaL_error(L, "%s 没有选项 %s（可用：priority、scope、filter）", what, key);
            }
            lua_pop(L, 1);
        }
    }
    Session& session = SessionOf(L);
    EventBinding& binding = session.events();
    const auto counts = binding.CountFor(&mod);
    if (counts.subscriptions + counts.waiters >= session.config().maxPerMod) {
        return luaL_error(L, "%s：订阅数达到上限 %d（DDLua.cfg 的 max_per_mod）", what, static_cast<int>(session.config().maxPerMod));
    }
    std::string reason;
    const TopicMeta* topic = binding.Meta(topicName, &reason);
    if (!topic) {
        session.Log(DD_LOG_WARN, &mod, std::string(what) + " 失败：" + reason);
        lua_pushnil(L);
        lua_pushstring(L, reason.c_str());
        return 2;
    }
    std::vector<FilterItem> filter;
    if (hasFilter) {
        lua_pushliteral(L, "filter");
        lua_rawget(L, 3);
        filter = ParseFilter(L, lua_gettop(L), *topic);
        lua_pop(L, 1);
    }
    if (actorScope && !actor::Valid(session.host(), scopeActor)) {
        reason = "scope 的 actor 句柄 " + actor::Describe(scopeActor) + " 已经失效";
        session.Log(DD_LOG_WARN, &mod, std::string(what) + " 失败：" + reason);
        lua_pushnil(L);
        lua_pushstring(L, reason.c_str());
        return 2;
    }
    // 先做完所有可能抛 Lua 错误的事（userdata、函数引用），最后才订阅，避免泄漏 SDK 订阅
    auto* ud = static_cast<SubUd*>(lua_newuserdatauv(L, sizeof(SubUd), 1));
    ud->key = 0;
    lua_pushstring(L, topic->name.c_str());
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, kSubMeta);
    lua_pushvalue(L, 2);
    const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);

    auto sub = std::make_shared<EventBinding::Sub>();
    sub->mod = &mod;
    sub->modId = mod.id();
    sub->topic = topic;
    sub->fnRef = fnRef;
    sub->filter = std::move(filter);
    sub->priority = priority;
    sub->scope = scope;
    sub->actorScope = actorScope;
    sub->scopeActor = scopeActor;
    sub->once = once;
    if (!binding.Subscribe(sub, &reason)) {
        luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        session.Log(DD_LOG_WARN, &mod, std::string(what) + " 失败：" + reason);
        lua_pushnil(L);
        lua_pushstring(L, reason.c_str());
        return 2;
    }
    ud->key = sub->key;
    return 1;
}

int LuaOn(lua_State* L)
{
    return Subscribe(L, false);
}

int LuaOnce(lua_State* L)
{
    return Subscribe(L, true);
}

int LuaOff(lua_State* L)
{
    CheckMod(L);
    SubUd& ud = CheckSub(L, 1);
    return OffImpl(L, ud.key, "dd.off(sub)");
}

int LuaSubscriptions(lua_State* L)
{
    Mod& mod = CheckMod(L);
    lua_pushinteger(L, static_cast<lua_Integer>(SessionOf(L).events().CountFor(&mod).subscriptions));
    return 1;
}

}  // namespace

const FieldMeta* TopicMeta::Find(std::string_view field) const
{
    for (const auto& f : fields) {
        if (f.name == field) {
            return &f;
        }
    }
    return nullptr;
}

void PushPointerField(lua_State* L, Session& session, const TopicMeta& topic, const FieldMeta& field, void* pointer,
    bool deferred)
{
    (void)topic;
    (void)field;
    // 延迟事件的指针恒为 NULL（DDCore 不复制指针），保持 nil；不是 Hero、Monster 的指针不可见
    const DDGameService* game = actor::Service(session.host());
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (deferred || !pointer || !game || !game->actor_from_pointer(pointer, &ref)) {
        lua_pushnil(L);
        return;
    }
    actor::Push(L, ref);
}

std::vector<FilterItem> ParseFilter(lua_State* L, int index, const TopicMeta& topic)
{
    index = lua_absindex(L, index);
    std::vector<FilterItem> items;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "filter 的键必须是字段名（字符串）");
        }
        const char* name = lua_tostring(L, -2);
        const FieldMeta* field = topic.Find(name);
        if (!field) {
            luaL_error(L, "filter：主题 %s 没有字段 %s", topic.name.c_str(), name);
        }
        if (items.size() >= kMaxFilterItems) {
            luaL_error(L, "filter 最多 %d 项", static_cast<int>(kMaxFilterItems));
        }
        FilterItem item;
        item.field = field;
        bool ok = false;
        switch (field->type) {
        case DD_FIELD_INT:
            if (lua_type(L, -1) == LUA_TNUMBER) {
                int isInteger = 0;
                item.i = static_cast<std::int64_t>(lua_tointegerx(L, -1, &isInteger));
                ok = isInteger != 0;
            }
            break;
        case DD_FIELD_FLOAT:
            ok = lua_type(L, -1) == LUA_TNUMBER;
            item.f = ok ? static_cast<double>(lua_tonumber(L, -1)) : 0;
            break;
        case DD_FIELD_BOOL:
            ok = lua_type(L, -1) == LUA_TBOOLEAN;
            item.b = lua_toboolean(L, -1) != 0;
            break;
        case DD_FIELD_STRING:
            if (lua_type(L, -1) == LUA_TSTRING) {
                std::size_t length = 0;
                const char* text = lua_tolstring(L, -1, &length);
                item.s.assign(text, length);
                ok = true;
            }
            break;
        case DD_FIELD_POINTER:
            if (!actor::Test(L, -1, &item.actor)) {
                luaL_error(L, "filter：字段 %s 是指针，只能按 actor 句柄过滤，给的是 %s", name, luaL_typename(L, -1));
            }
            ok = true;
            break;
        default:
            break;
        }
        if (!ok) {
            luaL_error(L, "filter：主题 %s 的字段 %s 是%s，给的是 %s", topic.name.c_str(), name, TypeName(field->type),
                luaL_typename(L, -1));
        }
        items.push_back(std::move(item));
        lua_pop(L, 1);
    }
    return items;
}

EventBinding::EventBinding(Session& session) : session_(session) {}

EventBinding::~EventBinding()
{
    std::vector<std::uint64_t> keys;
    for (const auto& [key, sub] : subs_) {
        keys.push_back(key);
    }
    for (const auto key : keys) {
        Remove(key, true);
    }
}

void EventBinding::RegisterMetatables(lua_State* L)
{
    luaL_newmetatable(L, kEventMeta);
    lua_pushcfunction(L, PayloadIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, PayloadNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, PayloadToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, kSubMeta);
    lua_pushcfunction(L, SubIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, SubToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

void EventBinding::AddFunctions(lua_State* L, int dd, std::uint64_t token)
{
    SetClosure(L, dd, "on", LuaOn, token);
    SetClosure(L, dd, "once", LuaOnce, token);
    SetClosure(L, dd, "off", LuaOff, token);
    SetClosure(L, dd, "subscriptions", LuaSubscriptions, token);
}

const TopicMeta* EventBinding::Meta(const char* topic, std::string* reason)
{
    const DDEventsService* events = session_.host().events;
    if (!events) {
        *reason = "events 服务不可用（DDCore 的事件系统没有初始化）";
        return nullptr;
    }
    DDEventTopicInfo info{};
    if (!events->topic_info(topic, &info)) {
        // 主题名是 Mod 给的，原因会写进日志、返回给 Mod：截断（第二轮 R2A-应修1）
        *reason = "主题不存在：" + ClipUtf8(topic, kMaxTopicInReason);
        return nullptr;
    }
    if (!info.available) {
        const char* why = nullptr;
        events->topic_available(topic, &why);
        *reason = "主题不可用：" + ClipUtf8(topic, kMaxTopicInReason) + "（" + (why && *why ? why : "未知原因") + "）";
        return nullptr;
    }
    if (const auto it = metas_.find(topic); it != metas_.end()) {
        return it->second.get();
    }
    auto meta = std::make_unique<TopicMeta>();
    meta->name = topic;
    meta->writable = info.writable;
    meta->cancellable = info.cancellable;
    for (std::uint32_t i = 0; i < info.field_count; ++i) {
        DDEventFieldInfo field{};
        if (events->topic_field_at(topic, i, &field) && field.name) {
            meta->fields.push_back({ field.name, field.type, field.access });
        }
    }
    const TopicMeta* raw = meta.get();
    metas_.emplace(topic, std::move(meta));
    return raw;
}

bool EventBinding::Subscribe(const std::shared_ptr<Sub>& sub, std::string* reason)
{
    const Host& host = session_.host();
    if (!host.events) {
        *reason = "events 服务不可用";
        return false;
    }
    sub->key = g_nextKey++;
    subs_.emplace(sub->key, sub);
    Routes().emplace(sub->key, this);
    DDEventOptions options{};
    options.size = sizeof(options);
    options.priority = sub->priority;
    options.scope = sub->scope;
    options.on_end = &EndThunk;
    sub->sdkId = host.events->subscribe(host.plugin, sub->topic->name.c_str(), &EventThunk,
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(sub->key)), &options);
    if (sub->sdkId == 0) {
        subs_.erase(sub->key);
        Routes().erase(sub->key);
        *reason = "主题 " + sub->topic->name + " 订阅失败（原因见 DDCore.log）";
        return false;
    }
    return true;
}

std::shared_ptr<EventBinding::Sub> EventBinding::Find(std::uint64_t key) const
{
    const auto it = subs_.find(key);
    return it == subs_.end() ? nullptr : it->second;
}

bool EventBinding::Remove(std::uint64_t key, bool sdk)
{
    const auto it = subs_.find(key);
    if (it == subs_.end()) {
        return false;
    }
    const std::shared_ptr<Sub> sub = it->second;
    subs_.erase(it);
    Routes().erase(key);
    const Host& host = session_.host();
    if (sdk && sub->sdkId && host.events) {
        host.events->unsubscribe(host.plugin, sub->sdkId);
    }
    sub->sdkId = 0;
    if (sub->fnRef != LUA_NOREF) {
        // 注册表里已有的键，luaL_unref 不分配内存、不抛错，在哪个线程上下文里调用都可以
        Vm* vm = session_.vm();
        if (vm && vm->ok()) {
            luaL_unref(vm->L(), LUA_REGISTRYINDEX, sub->fnRef);
        }
        sub->fnRef = LUA_NOREF;
    }
    return true;
}

void EventBinding::ReleaseMod(const Mod& mod)
{
    std::vector<std::uint64_t> keys;
    for (const auto& [key, sub] : subs_) {
        if (sub->mod == &mod) {
            keys.push_back(key);
        }
    }
    for (const auto key : keys) {
        Remove(key, true);
    }
}

EventBinding::Counts EventBinding::CountFor(const Mod* mod) const
{
    Counts counts;
    for (const auto& [key, sub] : subs_) {
        if (mod && sub->mod != mod) {
            continue;
        }
        (sub->waiter ? counts.waiters : counts.subscriptions) += 1;
    }
    return counts;
}

std::size_t EventBinding::GlobalSdkCount()
{
    return Routes().size();
}

std::uint64_t EventBinding::AddWaiter(lua_State* L, const Mod& mod, int topicIndex, int filterIndex, std::uint64_t taskId,
    std::string* reason)
{
    const char* topicName = luaL_checkstring(L, topicIndex);
    const auto counts = CountFor(&mod);
    if (counts.subscriptions + counts.waiters >= session_.config().maxPerMod) {
        luaL_error(L, "dd.wait_event：订阅数达到上限 %d（DDLua.cfg 的 max_per_mod）", static_cast<int>(session_.config().maxPerMod));
    }
    const TopicMeta* topic = Meta(topicName, reason);
    if (!topic) {
        return 0;
    }
    std::vector<FilterItem> filter;
    if (filterIndex) {
        filter = ParseFilter(L, filterIndex, *topic);
    }
    auto sub = std::make_shared<Sub>();
    sub->mod = &mod;
    sub->modId = mod.id();
    sub->topic = topic;
    sub->filter = std::move(filter);
    sub->priority = std::numeric_limits<std::int32_t>::max();  // 在普通订阅之后取快照，看到 .pre 的最终值
    sub->waiter = true;
    sub->taskId = taskId;
    if (!Subscribe(sub, reason)) {
        return 0;
    }
    return sub->key;
}

void EventBinding::RemoveWaiter(std::uint64_t key)
{
    Remove(key, true);
}

bool EventBinding::MatchFilter(const Sub& sub, const DDEvent* event) const
{
    const DDEventsService& events = *session_.host().events;
    for (const auto& item : sub.filter) {
        const char* name = item.field->name.c_str();
        switch (item.field->type) {
        case DD_FIELD_INT: {
            std::int64_t value = 0;
            if (events.get_int(event, name, &value) != DD_EV_OK || value != item.i) {
                return false;
            }
            break;
        }
        case DD_FIELD_FLOAT: {
            double value = 0;
            if (events.get_float(event, name, &value) != DD_EV_OK || value != item.f) {
                return false;
            }
            break;
        }
        case DD_FIELD_BOOL: {
            bool value = false;
            if (events.get_bool(event, name, &value) != DD_EV_OK || value != item.b) {
                return false;
            }
            break;
        }
        case DD_FIELD_STRING: {
            const char* value = nullptr;
            if (events.get_string(event, name, &value) != DD_EV_OK || !value || item.s != value) {
                return false;
            }
            break;
        }
        case DD_FIELD_POINTER: {
            // 解析句柄，和字段的指针比较（延迟事件的指针是 NULL，永远不匹配）
            void* value = nullptr;
            const DDGameService* game = actor::Service(session_.host());
            if (events.get_pointer(event, name, &value) != DD_EV_OK || !value || !game || game->actor_resolve(&item.actor) != value) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

void EventBinding::OnEvent(std::uint64_t key, DDEvent* event)
{
    try {
        const auto it = subs_.find(key);
        if (it == subs_.end()) {
            return;
        }
        const std::shared_ptr<Sub> sub = it->second;  // 回调里撤销订阅也不会释放它
        Vm* vm = session_.vm();
        if (!vm || !session_.CheckVmHealth(false) || !session_.host().events) {
            return;  // VM 损坏后不再派发
        }
        if (sub->actorScope && !actor::Valid(session_.host(), sub->scopeActor)) {
            session_.LogId(DD_LOG_DEBUG, sub->modId, "订阅 " + sub->topic->name + " 的 actor " + actor::Describe(sub->scopeActor) +
                " 已失效，撤销（派发时发现）");
            Remove(key, true);
            return;
        }
        if (!MatchFilter(*sub, event)) {
            return;
        }
        if (sub->waiter) {
            DeliverWaiter(sub, event);
        } else {
            InvokeCallback(sub, event);
        }
    } catch (const std::exception& e) {
        session_.Log(DD_LOG_ERROR, nullptr, std::string("事件派发出错：") + e.what());
    } catch (...) {
        session_.Log(DD_LOG_ERROR, nullptr, "事件派发出错：未知的 C++ 异常");
    }
}

void EventBinding::InvokeCallback(const std::shared_ptr<Sub>& sub, DDEvent* event)
{
    Vm* vm = session_.vm();
    lua_State* L = vm->L();
    const int top = lua_gettop(L);
    PayloadUd* ud = nullptr;
    std::string error;
    // 栈：[负载（锚）, 函数, 负载]。锚保证调用结束后 userdata 还活着，可以安全地标记失效
    const bool pushed = vm->Protected(
        [&](lua_State* L) {
            ud = NewPayload(L, event, sub->topic);
            lua_rawgeti(L, LUA_REGISTRYINDEX, sub->fnRef);
            lua_pushvalue(L, -2);
        },
        &error);
    if (!pushed) {
        session_.LogId(DD_LOG_ERROR, sub->modId, sub->topic->name + " 的回调没能开始：" + error);
        return;
    }
    if (sub->once) {
        Remove(sub->key, true);  // 先撤销：回调里再派发同一主题也不会调用第二次
    }
    bool budget = false;
    bool ok;
    {
        const Session::Running running(session_, sub->mod);
        ok = vm->Call(1, 0, session_.config().budgetMs, &error, &budget);
    }
    if (!vm->ok()) {
        return;  // 原生异常之后不再碰 VM（正常走不到这里：异常已经把栈展开到 DDCore）
    }
    ud->event = nullptr;  // 负载失效
    lua_settop(L, top);
    if (ok) {
        sub->failures = 0;
        return;
    }
    ++sub->failures;
    const bool active = Active(sub->key);
    const std::uint32_t limit = session_.config().maxFailures;
    std::string text = sub->topic->name + " 的回调出错";
    if (active && (budget || sub->failures >= limit)) {
        text += budget ? "（超出执行预算），已停用该订阅" : "（连续 " + std::to_string(sub->failures) + " 次），已停用该订阅";
        Remove(sub->key, true);
    } else if (active) {
        text += "（连续第 " + std::to_string(sub->failures) + " 次，" + std::to_string(limit) + " 次后停用）";
    }
    session_.LogId(DD_LOG_ERROR, sub->modId, text + "：" + error);
}

void EventBinding::DeliverWaiter(const std::shared_ptr<Sub>& sub, DDEvent* event)
{
    Vm* vm = session_.vm();
    const std::uint32_t flags = session_.host().events->event_flags(event);
    int ref = LUA_NOREF;
    std::string error;
    // 快照：字段（指针字段经过 PushPointerField）加 topic、deferred、cancelled，字段名优先
    const bool ok = vm->Protected(
        [&](lua_State* L) {
            lua_createtable(L, 0, static_cast<int>(sub->topic->fields.size()) + 3);
            lua_pushstring(L, sub->topic->name.c_str());
            lua_setfield(L, -2, "topic");
            lua_pushboolean(L, (flags & DD_EVENT_F_DEFERRED) != 0);
            lua_setfield(L, -2, "deferred");
            lua_pushboolean(L, (flags & DD_EVENT_F_CANCELLED) != 0);
            lua_setfield(L, -2, "cancelled");
            for (const auto& field : sub->topic->fields) {
                PushFieldValue(L, session_, event, *sub->topic, field, flags);
                lua_setfield(L, -2, field.name.c_str());
            }
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        },
        &error);
    const std::uint64_t taskId = sub->taskId;
    Remove(sub->key, true);
    if (!ok) {
        session_.LogId(DD_LOG_ERROR, sub->modId, "dd.wait_event(" + sub->topic->name + ") 的快照失败，协程收到 nil：" + error);
        ref = LUA_NOREF;
    }
    if (session_.HasRuntime()) {
        session_.scheduler().WakeTask(taskId, ref);
    }
}

std::size_t EventBinding::SweepActorScopes()
{
    std::vector<std::shared_ptr<Sub>> dead;
    for (const auto& [key, sub] : subs_) {
        if (sub->actorScope && !actor::Valid(session_.host(), sub->scopeActor)) {
            dead.push_back(sub);
        }
    }
    for (const auto& sub : dead) {
        session_.LogId(DD_LOG_DEBUG, sub->modId, "订阅 " + sub->topic->name + " 的 actor " + actor::Describe(sub->scopeActor) +
            " 已失效，撤销（定期检查）");
        Remove(sub->key, true);
    }
    return dead.size();
}

void EventBinding::NoteNativeFault(unsigned long code, const void* address)
{
    session_.NoteNativeFault("事件回调", code, address);
}

void EventBinding::OnEnd(std::uint64_t key, std::uint32_t reason)
{
    const auto it = subs_.find(key);
    if (it == subs_.end()) {
        return;
    }
    const std::shared_ptr<Sub> sub = it->second;
    sub->sdkId = 0;  // DDCore 已经撤销
    if (reason == DD_EVENT_END_CRASH) {
        // 回调执行期间发生了原生异常（SEH），DDCore 的隔离把栈展开到了它自己：lua_State、预算入口都停在异常发生时，
        // 之后不能再碰 VM（下面的 Remove 也不会 luaL_unref）。审核 B-Y3
        session_.MarkBroken("订阅 " + sub->topic->name + " 的回调里发生了原生异常（SEH），DDCore 已撤销该订阅并写进 DDCore.log");
    }
    const char* why = reason == DD_EVENT_END_SCOPE ? "作用域结束" : (reason == DD_EVENT_END_CRASH ? "回调崩溃" : "未知原因");
    session_.LogId(reason == DD_EVENT_END_SCOPE ? DD_LOG_DEBUG : DD_LOG_ERROR, sub->modId,
        std::string(sub->waiter ? "dd.wait_event(" : "订阅 ") + sub->topic->name + (sub->waiter ? ")" : "") + " 被 DDCore 撤销（" +
            why + (reason == DD_EVENT_END_SCOPE ? std::string("，scope=") + ScopeName(sub->scope, sub->actorScope) +
                (sub->actorScope ? " " + actor::Describe(sub->scopeActor) : std::string()) : std::string()) + "）");
    Remove(key, false);
    if (sub->waiter && session_.HasRuntime()) {
        session_.scheduler().WakeTask(sub->taskId, LUA_NOREF);
    }
}

}  // namespace ddlua

#include "actor.h"

#include "combat.h"
#include "host.h"
#include "sandbox.h"
#include "session.h"

#include <lauxlib.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace ddlua::actor {

namespace {

constexpr const char* kActorMeta = "dd.actor";
char kInternKey = 0;  // 注册表里驻留表的键（取地址用）

struct ActorUd {
    DDActorRef ref;
};

const Host& HostOf(lua_State* L)
{
    return SessionOf(L).host();
}

const char* KindName(std::uint32_t kind)
{
    return kind == DD_ACTOR_HERO ? "hero" : (kind == DD_ACTOR_MONSTER ? "monster" : "none");
}

// 驻留表的键：标识的六个字段（怪物的 guid 每场战斗重新计数，战斗序号不同的是不同的 actor）
void InternKey(const DDActorRef& ref, char* buffer, std::size_t size)
{
    std::snprintf(buffer, size, "%u:%u:%u:%u:%u:%u", ref.kind, ref.roster, ref.id, ref.campaign_serial, ref.raid_serial,
        ref.battle_serial);
}

ActorUd& Check(lua_State* L, int index)
{
    return *static_cast<ActorUd*>(luaL_checkudata(L, index, kActorMeta));
}

int ActorValid(lua_State* L)
{
    ActorUd& ud = Check(L, 1);
    lua_pushboolean(L, Valid(HostOf(L), ud.ref));
    return 1;
}

// 读取字段：解析失败时返回 false（字段读出 nil）
bool ReadInfo(lua_State* L, const DDActorRef& ref, DDActorInfo* info)
{
    const DDGameService* game = Service(HostOf(L));
    if (!game) {
        return false;
    }
    *info = {};
    info->size = sizeof(DDActorInfo);
    const bool ok = game->actor_info(&ref, info);
    AfterReentry(L);  // 最大生命由游戏的合计函数算，可能判定规则、进入嵌套的规则回调（P4b）
    Vm::From(L)->CheckBudget(L);  // 嵌套的回调用掉的时间可能让外层过了截止时间
    return ok;
}

bool IsField(const char* key)
{
    for (const char* name : { "hp", "max_hp", "stress", "class" }) {
        if (std::strcmp(key, name) == 0) {
            return true;
        }
    }
    return false;
}

// h.<字段>：标识（kind、id、is_hero、is_monster）总能读；hp、max_hp、stress、class 每次重新解析，失败时为 nil
int ActorIndex(lua_State* L)
{
    ActorUd& ud = Check(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "actor 句柄的字段名必须是字符串");
    }
    const char* key = lua_tostring(L, 2);
    const DDActorRef& ref = ud.ref;
    if (std::strcmp(key, "kind") == 0) {
        lua_pushstring(L, KindName(ref.kind));
    } else if (std::strcmp(key, "id") == 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(ref.id));
    } else if (std::strcmp(key, "is_hero") == 0) {
        lua_pushboolean(L, ref.kind == DD_ACTOR_HERO);
    } else if (std::strcmp(key, "is_monster") == 0) {
        lua_pushboolean(L, ref.kind == DD_ACTOR_MONSTER);
    } else if (std::strcmp(key, "valid") == 0) {
        lua_pushcfunction(L, ActorValid);
    } else if (std::strcmp(key, "buffs") == 0) {
        lua_pushcfunction(L, combat::ActorBuffs);
    } else if (std::strcmp(key, "buff") == 0) {
        lua_pushcfunction(L, combat::ActorBuff);
    } else if (std::strcmp(key, "add_buff") == 0) {
        lua_pushcfunction(L, combat::ActorAddBuff);
    } else if (std::strcmp(key, "remove_buffs") == 0) {
        lua_pushcfunction(L, combat::ActorRemoveBuffs);
    } else if (IsField(key)) {
        DDActorInfo info{};
        if (!ReadInfo(L, ref, &info)) {
            lua_pushnil(L);
        } else if (std::strcmp(key, "hp") == 0) {
            lua_pushnumber(L, static_cast<lua_Number>(info.hp));
        } else if (std::strcmp(key, "max_hp") == 0) {
            info.has_max_hp ? lua_pushnumber(L, static_cast<lua_Number>(info.max_hp)) : lua_pushnil(L);
        } else if (std::strcmp(key, "stress") == 0) {
            info.has_stress ? lua_pushnumber(L, static_cast<lua_Number>(info.stress)) : lua_pushnil(L);
        } else {
            info.class_id[0] ? (void)lua_pushstring(L, info.class_id) : lua_pushnil(L);
        }
    } else {
        return luaL_error(L,
            "actor 句柄没有字段 %s（可用：kind、id、is_hero、is_monster、hp、max_hp、stress、class、valid、buffs、buff、add_buff、remove_buffs）", key);
    }
    return 1;
}

int ActorNewIndex(lua_State* L)
{
    Check(L, 1);
    return luaL_error(L, "actor 句柄只读（字段不能赋值；buff 用 add_buff / remove_buffs）");
}

int ActorEq(lua_State* L)
{
    DDActorRef a{};
    DDActorRef b{};
    lua_pushboolean(L, Test(L, 1, &a) && Test(L, 2, &b) && Same(a, b));
    return 1;
}

int ActorToString(lua_State* L)
{
    ActorUd& ud = Check(L, 1);
    const std::string text = "dd.actor(" + Describe(ud.ref) + (Valid(HostOf(L), ud.ref) ? "" : "，已失效") + ")";
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int Find(lua_State* L, std::uint32_t kind, const char* what)
{
    CheckMod(L);
    int isInteger = 0;
    const lua_Integer id = lua_type(L, 1) == LUA_TNUMBER ? lua_tointegerx(L, 1, &isInteger) : 0;
    if (!isInteger || id < 0 || id > 0xFFFFFFFFll) {
        return luaL_error(L, "%s 的参数必须是非负整数", what);
    }
    const DDGameService* game = Service(HostOf(L));
    DDActorRef ref{};
    ref.size = sizeof(ref);
    if (!game || !game->actor_find(kind, static_cast<std::uint32_t>(id), &ref)) {
        lua_pushnil(L);
        return 1;
    }
    Push(L, ref);
    return 1;
}

int LuaHero(lua_State* L)
{
    return Find(L, DD_ACTOR_HERO, "dd.actor.hero");
}

int LuaMonster(lua_State* L)
{
    return Find(L, DD_ACTOR_MONSTER, "dd.actor.monster");
}

int LuaIs(lua_State* L)
{
    CheckMod(L);
    lua_pushboolean(L, Test(L, 1, nullptr));
    return 1;
}

// 暂时读不到：加载线程在跑时的原因以「加载中」开头（和 dd.buff、actor:buffs 一样，审核 R2A-S1），其余是存档外、DDCore 太旧
int PushUnreadable(lua_State* L, const char* fallback)
{
    const char* loading = combat::LoadingReason(HostOf(L));
    lua_pushnil(L);
    lua_pushstring(L, loading ? loading : fallback);
    return 2;
}

constexpr std::uint32_t kListCapacity = 64;  // 和 DDCore 的怪物表上限相同（队伍最多 16）

// dd.actor.party() / dd.actor.monsters()（F5）：DDCore 枚举出的句柄数组，顺序同游戏的表（队伍按站位，怪物按站位）
int List(lua_State* L, bool party)
{
    CheckMod(L);
    const DDGameService* game = Service(HostOf(L));
    const auto fn = game ? (party ? game->party_heroes : game->battle_monsters) : nullptr;
    if (!fn) {
        return PushUnreadable(L, "DDCore 没有提供枚举（game 服务太旧）");
    }
    DDActorRef refs[kListCapacity] = {};
    std::uint32_t count = 0;
    if (!fn(refs, kListCapacity, &count)) {
        return PushUnreadable(L, "现在读不到（不在主线程、不在存档里，或者加载线程在跑）");
    }
    const std::uint32_t n = count < kListCapacity ? count : kListCapacity;
    lua_createtable(L, static_cast<int>(n), 0);
    for (std::uint32_t i = 0; i < n; ++i) {
        Push(L, refs[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

int LuaParty(lua_State* L)
{
    return List(L, true);
}

int LuaMonsters(lua_State* L)
{
    return List(L, false);
}

// dd.actor.in_battle()（F5）：战斗中（含从存档恢复、没有派发 battle.start 的战斗）返回 true 和战斗状态（Battle+0x288），
// 不在战斗中返回 false, 0
int LuaInBattle(lua_State* L)
{
    CheckMod(L);
    const DDGameService* game = Service(HostOf(L));
    if (!game || !game->battle_state) {
        return PushUnreadable(L, "DDCore 没有提供战斗状态（game 服务太旧）");
    }
    const std::int32_t state = game->battle_state();
    if (state < 0) {
        return PushUnreadable(L, "现在读不到（不在主线程、不在存档里，或者加载线程在跑）");
    }
    lua_pushboolean(L, state != 0);
    lua_pushinteger(L, state);
    return 2;
}

}  // namespace

const DDGameService* Service(const Host& host)
{
    const DDGameService* game = host.game;
    return game && game->header.size >= sizeof(DDGameService) && game->actor_info ? game : nullptr;
}

bool Same(const DDActorRef& a, const DDActorRef& b)
{
    return a.kind == b.kind && a.id == b.id && a.campaign_serial == b.campaign_serial && a.raid_serial == b.raid_serial &&
        a.battle_serial == b.battle_serial && a.roster == b.roster;
}

std::string Describe(const DDActorRef& ref)
{
    std::string text = std::string(KindName(ref.kind)) + "#" + std::to_string(ref.id);
    if (ref.kind == DD_ACTOR_MONSTER) {
        text += "（探索 " + std::to_string(ref.raid_serial) + "，战斗 " + std::to_string(ref.battle_serial) + "）";
    } else if (ref.roster != 0) {
        text += "（备用名册）";
    }
    return text;
}

bool Valid(const Host& host, const DDActorRef& ref)
{
    const DDGameService* game = Service(host);
    return game && game->actor_resolve(&ref) != nullptr;
}

void RegisterMetatables(lua_State* L)
{
    luaL_newmetatable(L, kActorMeta);
    lua_pushcfunction(L, ActorIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ActorNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, ActorEq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, ActorToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    // 驻留表：标识 -> userdata，弱值（没有别的引用时回收）
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kInternKey);
}

void AddFunctions(lua_State* L, int dd, std::uint64_t token)
{
    lua_createtable(L, 0, 6);
    const int table = lua_gettop(L);
    SetClosure(L, table, "hero", LuaHero, token);
    SetClosure(L, table, "monster", LuaMonster, token);
    SetClosure(L, table, "is", LuaIs, token);
    SetClosure(L, table, "party", LuaParty, token);
    SetClosure(L, table, "monsters", LuaMonsters, token);
    SetClosure(L, table, "in_battle", LuaInBattle, token);
    lua_setfield(L, dd, "actor");
}

void Push(lua_State* L, const DDActorRef& ref)
{
    char key[80];
    InternKey(ref, key, sizeof(key));
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kInternKey);
    if (lua_getfield(L, -1, key) == LUA_TUSERDATA) {
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    auto* ud = static_cast<ActorUd*>(lua_newuserdatauv(L, sizeof(ActorUd), 0));
    ud->ref = ref;
    ud->ref.size = sizeof(DDActorRef);
    luaL_setmetatable(L, kActorMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, key);
    lua_remove(L, -2);
}

bool Test(lua_State* L, int index, DDActorRef* out)
{
    auto* ud = static_cast<ActorUd*>(luaL_testudata(L, index, kActorMeta));
    if (!ud) {
        return false;
    }
    if (out) {
        *out = ud->ref;
    }
    return true;
}

}  // namespace ddlua::actor

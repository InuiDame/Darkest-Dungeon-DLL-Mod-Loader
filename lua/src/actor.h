#pragma once
// actor 句柄的 Lua 绑定：dd.actor.hero(id)、dd.actor.monster(guid)、dd.actor.is(v)，事件指针字段转句柄。
//
// 句柄 userdata（dd.actor）只存 DDActorRef（值，不含指针），每次读字段都经 game 服务重新解析（DDCore 负责
// 存档序号、探索序号、战斗序号的比较），解析失败时字段读出 nil、:valid() 为 false。同一个 actor 在一个 VM 里只有一个
// userdata：注册表里的弱值表按标识驻留，所以句柄可以当表的键，== 也成立（另有 __eq 按值比较兜底）。
// 句柄可以跨回调、跨协程保存，VM 销毁时随之消失。只在主线程使用。

#include <dd/dd_api.h>
#include <lua.h>

#include <cstdint>
#include <string>

namespace ddlua {

struct Host;

namespace actor {

// game 服务有 actor 函数时返回它，否则返回 nullptr（DDCore 太旧，或离线测试没给）
const DDGameService* Service(const Host& host);

// 元表和驻留表，VM 创建时调用一次（保护模式内）
void RegisterMetatables(lua_State* L);
// dd.actor 子表（保护模式内）
void AddFunctions(lua_State* L, int dd, std::uint64_t token);

// 压一个句柄（驻留：同一个标识返回同一个 userdata）。可能抛 Lua 错误（内存）
void Push(lua_State* L, const DDActorRef& ref);
// index 处是句柄时复制到 out 并返回 true
bool Test(lua_State* L, int index, DDActorRef* out);
// 两个句柄是不是同一个 actor（六个标识字段都相等）
bool Same(const DDActorRef& a, const DDActorRef& b);
// 日志用的描述，如 "hero#12"、"monster#4501（探索 2，战斗 3）"
std::string Describe(const DDActorRef& ref);
// 现在还能不能解析
bool Valid(const Host& host, const DDActorRef& ref);

}  // namespace actor

}  // namespace ddlua

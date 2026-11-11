#pragma once
// 事件绑定：dd.on / dd.once / dd.off、负载 userdata、filter，以及 dd.wait_event 的等待者。
//
// 每个 Lua 订阅对应一个 SDK 订阅（events 服务），优先级交错、PvP 屏蔽、作用域都沿用 DDCore 的语义。
// SDK 的 user 指针是一个进程内唯一的键，回调时按键查表，查不到就什么都不做，不会有悬垂指针。
// DDCore 按作用域（或因为回调崩溃）撤销订阅时通过 on_end 通知，这里释放 Lua 函数引用、删掉记录。
//
// 回调：先在 C++ 里比较 filter，匹配了才进入 Lua。负载是 userdata（dd.event），回调返回后标记失效，
// 之后任何访问都报错；userdata 在调用期间被栈锚住，失效标记不会写到已释放的内存。回调用 Vm::Call
// （预算、traceback），连续出错 max_failures 次停用，超预算立即停用。
//
// 只在主线程使用（DDCore 的回调全部在主线程）。

#include <dd/dd_api.h>
#include <lauxlib.h>
#include <lua.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ddlua {

class Session;
struct Mod;

struct FieldMeta {
    std::string name;
    std::uint32_t type = 0;    // DDEventFieldType
    std::uint32_t access = 0;  // DDEventFieldAccess
};

// 主题的字段表（从 events 服务查一次后缓存，字段在进程内不变）
struct TopicMeta {
    std::string name;
    bool writable = false;
    bool cancellable = false;
    std::vector<FieldMeta> fields;
    const FieldMeta* Find(std::string_view field) const;
};

// filter 的一项：字段等于某个值。指针字段只能按 actor 句柄过滤（派发时解析句柄，和字段的指针比较）
struct FilterItem {
    const FieldMeta* field = nullptr;
    std::int64_t i = 0;
    double f = 0;
    bool b = false;
    std::string s;
    DDActorRef actor{};  // 指针字段：要匹配的 actor
};

class EventBinding
{
public:
    explicit EventBinding(Session& session);
    ~EventBinding();  // 撤销剩下的全部 SDK 订阅（正常情况下 Mod 拆除时已经撤销）
    EventBinding(const EventBinding&) = delete;
    EventBinding& operator=(const EventBinding&) = delete;

    // 元表（dd.event、dd.subscription），VM 创建时调用一次（保护模式内）
    static void RegisterMetatables(lua_State* L);
    // dd.on、dd.once、dd.off、dd.subscriptions，装进 dd 表（保护模式内）
    static void AddFunctions(lua_State* L, int dd, std::uint64_t token);

    // wait_event 的等待者：订阅 topic（栈上 topicIndex），filterIndex 为 0 表示没有。事件匹配时取一份字段快照，
    // 撤销订阅，交给调度器（Scheduler::WakeTask）。主题不存在或不可用时返回 0 并写 reason；filter 不合法时抛 Lua 错误
    std::uint64_t AddWaiter(lua_State* L, const Mod& mod, int topicIndex, int filterIndex, std::uint64_t taskId, std::string* reason);
    void RemoveWaiter(std::uint64_t key);

    // 撤销 Mod 的全部订阅和等待者，不运行 Lua 代码
    void ReleaseMod(const Mod& mod);

    struct Counts {
        std::size_t subscriptions = 0;  // dd.on / dd.once
        std::size_t waiters = 0;        // dd.wait_event
    };
    Counts CountFor(const Mod* mod) const;  // mod 为空时统计整个会话
    // 所有会话持有的 SDK 订阅数（和 DDCore 的 subscription_count 核对）
    static std::size_t GlobalSdkCount();

    // 主题的字段表；主题不存在或不可用时返回空并写 reason
    const TopicMeta* Meta(const char* topic, std::string* reason);

    // actor 作用域：撤销句柄已经解析不了的订阅（Session::Tick 每秒调用一次），返回撤销的个数
    std::size_t SweepActorScopes();

    // SDK 回调入口（events.cpp 的静态函数转过来）
    void OnEvent(std::uint64_t key, DDEvent* event);
    void OnEnd(std::uint64_t key, std::uint32_t reason);
    // 事件跳板的 SEH 过滤器里调用：给会话的 VM 置损坏标记（不分配、不写日志）
    void NoteNativeFault(unsigned long code, const void* address);

    struct Sub;
    // 新订阅（不抛 Lua 错误）：成功时登记并返回 true；SDK 订阅失败时返回 false 并写 reason
    bool Subscribe(const std::shared_ptr<Sub>& sub, std::string* reason);
    // 撤销一个订阅：sdk 为 true 时调用 SDK 的 unsubscribe（DDCore 已经撤销的传 false）。返回它原来是否有效
    bool Remove(std::uint64_t key, bool sdk);
    bool Active(std::uint64_t key) const { return subs_.count(key) != 0; }
    std::shared_ptr<Sub> Find(std::uint64_t key) const;

private:
    void DeliverWaiter(const std::shared_ptr<Sub>& sub, DDEvent* event);
    void InvokeCallback(const std::shared_ptr<Sub>& sub, DDEvent* event);
    bool MatchFilter(const Sub& sub, const DDEvent* event) const;

    Session& session_;
    std::unordered_map<std::string, std::unique_ptr<TopicMeta>> metas_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Sub>> subs_;
};

// 订阅记录
struct EventBinding::Sub {
    std::uint64_t key = 0;     // SDK 的 user（进程内唯一）
    const Mod* mod = nullptr;
    std::string modId;
    const TopicMeta* topic = nullptr;
    DDEventSubId sdkId = 0;
    int fnRef = LUA_NOREF;
    std::vector<FilterItem> filter;
    std::int32_t priority = 0;
    std::uint32_t scope = DD_EVENT_SCOPE_PLUGIN;
    bool actorScope = false;   // scope 是 actor 句柄：派发前检查句柄，解析不了就撤销
    DDActorRef scopeActor{};
    bool once = false;
    bool waiter = false;       // wait_event 的等待者
    std::uint64_t taskId = 0;
    std::uint32_t failures = 0;  // 连续出错次数
};

// 读事件字段时指针字段的转换：Hero、Monster 转成 actor 句柄（game 服务的 actor_from_pointer），
// 其余指针不可见（nil）；延迟事件的指针字段恒为 NULL，也是 nil
void PushPointerField(lua_State* L, Session& session, const TopicMeta& topic, const FieldMeta& field, void* pointer,
    bool deferred);

// 解析 filter 表（栈上 index）：键是主题的字段名，值是同类型的数字、布尔或字符串。不合法时抛 Lua 错误
std::vector<FilterItem> ParseFilter(lua_State* L, int index, const TopicMeta& topic);

}  // namespace ddlua

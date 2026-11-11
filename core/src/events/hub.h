#pragma once
// 事件中枢：主题注册、订阅、派发、作用域、延迟队列。不依赖游戏，单元测试用同一份源码。
//
// 线程约定：Subscribe/Unsubscribe/RemoveOwner/EndScope/EnqueueDeferred 线程安全；
// Dispatch 和 DrainDeferred 只在主线程调用。

#include <dd/dd_api.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ddc::events {

inline constexpr std::uint32_t kAccessR = DD_FIELD_READ;
inline constexpr std::uint32_t kAccessRW = DD_FIELD_READ | DD_FIELD_WRITE;

enum class TopicKind : std::uint32_t {
    Native = 0,
    Query = 1,
    Semantic = 2,
    Custom = 3,
    App = 4,
};

// 字段在游戏里的原始宽度，写入时按它检查取值范围（超出返回 DD_EV_E_RANGE，不写入）。
// 自定义主题的值按 int64 / double 保存和交还，不限制
enum class FieldRange : std::uint8_t {
    Full,  // int64 / double
    I32,   // int32
    U32,   // uint32
    F32,   // float：有限值，绝对值不超过 FLT_MAX（nan、inf 也拒绝）
};

struct FieldDesc {
    std::string name;
    DDEventFieldType type = DD_FIELD_INT;
    std::uint32_t access = kAccessR;
    FieldRange range = FieldRange::Full;
};

struct Slot {
    union {
        std::int64_t i;
        double f;
        bool b;
        const char* s;
        void* p;
    } v{};
    bool set = false;
};

struct Topic;
struct Subscription;

// 这是跨边界的 DDEvent*，只在回调期间有效
struct Payload {
    Topic* topic = nullptr;
    Slot* slots = nullptr;
    std::uint32_t flags = 0;
    std::uint32_t originThread = 0;
    bool allowWrite = false;
    bool cancelled = false;
};

struct Subscription {
    std::uint64_t id = 0;
    std::uint32_t owner = 0;
    Topic* topic = nullptr;
    DDEventFn fn = nullptr;
    void* user = nullptr;
    DDEventEndFn onEnd = nullptr;  // 被中枢撤销（作用域结束、崩溃）时通知
    std::int32_t priority = 0;
    std::uint64_t seq = 0;
    std::uint32_t scope = 0;
    bool internal = false;
    std::atomic<bool> alive{ true };
};

struct Topic {
    std::string name;
    TopicKind kind = TopicKind::Semantic;
    std::uint32_t index = 0;
    bool writable = false;
    bool cancellable = false;
    std::uint32_t phase = 0;  // DD_EVENT_F_PRE / DD_EVENT_F_POST
    std::vector<FieldDesc> fields;
    std::vector<std::shared_ptr<Subscription>> subs;  // 按 (priority, seq) 升序，受 hub 的 mutex 保护
    bool available = true;
    std::string unavailableReason;
    void* bridgeData = nullptr;  // 桥接层的挂载点，中枢不解读
    std::uint64_t dispatchCount = 0;
    std::uint64_t firstThread = 0;
    bool firstSeen = false;
};

class Hub
{
public:
    // 返回 false 表示回调崩溃，该订阅被取消
    using Invoke = std::function<bool(DDEventFn fn, DDEvent* event, void* user, std::uint32_t owner)>;
    using LogFn = std::function<void(int level, const std::string& message)>;
    // 撤销通知的调用方式（DDCore 用 SEH 隔离），没有设置时直接调用
    using EndInvoke = std::function<void(DDEventEndFn fn, std::uint64_t id, void* user, std::uint32_t reason,
        std::uint32_t owner)>;

    Hub(Invoke invoke, LogFn log);
    void SetEndInvoke(EndInvoke invoke) { endInvoke_ = std::move(invoke); }
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // 主题 ---------------------------------------------------------------------
    Topic* AddTopic(const std::string& name, TopicKind kind, std::vector<FieldDesc> fields, bool writable,
        bool cancellable);
    Topic* Find(std::string_view name);
    void SetAvailability(Topic* topic, bool available, const std::string& reason);

    // 按注册顺序的主题快照（下标稳定，只增不减）
    std::vector<Topic*> Topics() const;
    std::uint32_t TopicCount() const;
    Topic* TopicAt(std::uint32_t index) const;

    // 订阅 ---------------------------------------------------------------------
    // 成功返回 id，失败返回 0 并通过 reason 给出可读原因（可为 NULL）
    std::uint64_t Subscribe(std::uint32_t owner, const std::string& topic, DDEventFn fn, void* user,
        std::int32_t priority, std::uint32_t scope, std::string* reason, bool internal = false,
        DDEventEndFn onEnd = nullptr);
    // 插件主动取消和回滚（RemoveOwner）不发撤销通知
    bool Unsubscribe(std::uint32_t owner, std::uint64_t id);
    std::size_t RemoveOwner(std::uint32_t owner);
    // 撤销作用域内的订阅，锁外逐个发撤销通知（DD_EVENT_END_SCOPE）
    std::size_t EndScope(std::uint32_t scope);
    // 某个 owner 当前有效的订阅数（诊断，events.subscription_count）
    std::size_t OwnerCount(std::uint32_t owner) const;

    // 某主题当前活跃的订阅数（含内部订阅）。桥接层据此决定 listener 和 hook 的安装
    std::size_t SubscriberCount(const Topic* topic) const;
    // 作用域订阅总数（内部订阅不算）。桥接层据此决定是否注册作用域 tracker
    std::size_t ScopedCount(std::uint32_t scope) const;
    // 带 owner 的订阅总数（内部订阅不算），用于“无订阅时 hook 和 listener 数为零”的自检
    std::size_t ExternalSubscriberCount() const;

    // 派发 ---------------------------------------------------------------------
    // payload.slots 必须按 topic->fields 的长度准备好。返回 false 表示因深度超限被丢弃。
    bool Dispatch(Topic* topic, Payload& payload);
    int Depth() const { return depth_; }

    // 非主线程事件复制到队列：字符串深拷贝（最长 1023 字节），指针字段清零，只读，不可取消
    void EnqueueDeferred(Topic* topic, const Slot* slots, std::uint32_t originThread);
    std::size_t DeferredCount() const;
    // 主线程调用，派发并清空当前队列（派发期间新入队的留到下一次）
    std::size_t DrainDeferred();

    // 负载访问 -----------------------------------------------------------------
    static const FieldDesc* FindField(const Topic* topic, const char* name, std::uint32_t* index);

    // 发生变化（订阅增删）后置位，桥接层在帧边界检查
    bool TestAndClearDirty();
    void MarkDirty();

private:
    struct Deferred {
        Topic* topic = nullptr;
        std::vector<Slot> slots;
        std::vector<std::unique_ptr<char[]>> strings;
        std::uint32_t originThread = 0;
    };

    void InsertSorted(Topic& topic, std::shared_ptr<Subscription> sub);
    void NotifyEnd(const Subscription& sub, std::uint32_t reason);

    Invoke invoke_;
    LogFn log_;
    EndInvoke endInvoke_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Topic>> topics_;
    std::unordered_map<std::string, Topic*> byName_;
    std::uint64_t nextId_ = 1;
    std::uint64_t nextSeq_ = 1;
    std::atomic<bool> dirty_{ true };
    int depth_ = 0;
    bool draining_ = false;
    std::vector<Deferred> deferred_;
};

}  // namespace ddc::events

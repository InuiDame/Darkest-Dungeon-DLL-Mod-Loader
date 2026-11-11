#pragma once
// 生命周期阶段的订阅与派发。Subscribe 线程安全，Fire 和 ProcessLate 只在主线程调用。
// 调用插件回调的方式（SEH 隔离）由 invoke 注入，单元测试可以直接传普通调用。

#include <dd/dd_api.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ddc {

class Lifecycle
{
public:
    // 返回 false 表示回调崩溃，该订阅被自动取消
    using Invoke = std::function<bool(DDPhaseFn, const DDPhaseEvent*, void*, std::uint32_t owner)>;

    explicit Lifecycle(Invoke invoke);

    std::uint64_t Subscribe(std::uint32_t owner, std::uint32_t phase, DDPhaseFn fn, void* user);
    bool Unsubscribe(std::uint32_t owner, std::uint64_t id);
    std::size_t RemoveOwner(std::uint32_t owner);

    // 派发一次阶段，派发前把一次性阶段标记为已过
    void Fire(std::uint32_t phase, std::uint64_t frame, std::uint32_t campaignSerial);

    // 执行等待补调的迟到订阅（下一帧主线程调用）
    void ProcessLate(std::uint64_t frame, std::uint32_t campaignSerial);

    void SetCampaignActive(bool active);
    bool CampaignActive() const;

    // 已到达的最靠后的一次性阶段；未到达时 DD_PHASE_COUNT
    std::uint32_t CurrentPhase() const;

private:
    struct Subscription {
        std::uint64_t id;
        std::uint32_t owner;
        std::uint32_t phase;
        DDPhaseFn fn;
        void* user;
        bool alive;
    };

    Invoke invoke_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    std::vector<std::shared_ptr<Subscription>> late_;
    bool reached_[DD_PHASE_COUNT] = {};
    bool campaignActive_ = false;
    std::uint64_t nextId_ = 1;
};

}  // namespace ddc

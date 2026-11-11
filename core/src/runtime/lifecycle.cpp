#include "lifecycle.h"

#include <algorithm>
#include <memory>

namespace ddc {

namespace {

bool IsOneShot(std::uint32_t phase)
{
    return phase == DD_PHASE_EARLY || phase == DD_PHASE_APP_READY || phase == DD_PHASE_ASSETS_READY;
}

}  // namespace

Lifecycle::Lifecycle(Invoke invoke) : invoke_(std::move(invoke)) {}

std::uint64_t Lifecycle::Subscribe(std::uint32_t owner, std::uint32_t phase, DDPhaseFn fn, void* user)
{
    if (!fn || phase >= DD_PHASE_COUNT) {
        return 0;
    }
    std::lock_guard lock(mutex_);
    auto sub = std::make_shared<Subscription>();
    sub->id = nextId_++;
    sub->owner = owner;
    sub->phase = phase;
    sub->fn = fn;
    sub->user = user;
    sub->alive = true;
    subscriptions_.push_back(sub);

    // 阶段已过之后注册：一次性阶段和当前处于存档内的 CampaignLoad 补调一次
    if ((IsOneShot(phase) && reached_[phase]) || (phase == DD_PHASE_CAMPAIGN_LOAD && campaignActive_)) {
        late_.push_back(sub);
    }
    return sub->id;
}

bool Lifecycle::Unsubscribe(std::uint32_t owner, std::uint64_t id)
{
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
        [&](const std::shared_ptr<Subscription>& s) { return s->id == id && s->owner == owner; });
    if (it == subscriptions_.end()) {
        return false;
    }
    (*it)->alive = false;
    subscriptions_.erase(it);
    return true;
}

std::size_t Lifecycle::RemoveOwner(std::uint32_t owner)
{
    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
        if ((*it)->owner == owner) {
            (*it)->alive = false;
            it = subscriptions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void Lifecycle::Fire(std::uint32_t phase, std::uint64_t frame, std::uint32_t campaignSerial)
{
    if (phase >= DD_PHASE_COUNT) {
        return;
    }

    std::vector<std::shared_ptr<Subscription>> snapshot;
    {
        std::lock_guard lock(mutex_);
        if (IsOneShot(phase)) {
            reached_[phase] = true;
        }
        for (const auto& sub : subscriptions_) {
            if (sub->phase == phase) {
                snapshot.push_back(sub);
            }
        }
    }

    const DDPhaseEvent event{ sizeof(DDPhaseEvent), phase, frame, campaignSerial, 0 };
    for (const auto& sub : snapshot) {
        // 派发期间被取消的不再调用
        if (!sub->alive) {
            continue;
        }
        if (!invoke_(sub->fn, &event, sub->user, sub->owner)) {
            std::lock_guard lock(mutex_);
            if (sub->alive) {
                sub->alive = false;
                subscriptions_.erase(std::remove(subscriptions_.begin(), subscriptions_.end(), sub),
                    subscriptions_.end());
            }
        }
    }
}

void Lifecycle::ProcessLate(std::uint64_t frame, std::uint32_t campaignSerial)
{
    std::vector<std::shared_ptr<Subscription>> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(late_);
    }
    if (pending.empty()) {
        return;
    }
    std::stable_sort(pending.begin(), pending.end(),
        [](const auto& a, const auto& b) { return a->phase < b->phase; });
    for (const auto& sub : pending) {
        if (!sub->alive) {
            continue;
        }
        const DDPhaseEvent event{ sizeof(DDPhaseEvent), sub->phase, frame, campaignSerial, 1 };
        if (!invoke_(sub->fn, &event, sub->user, sub->owner)) {
            std::lock_guard lock(mutex_);
            if (sub->alive) {
                sub->alive = false;
                subscriptions_.erase(std::remove(subscriptions_.begin(), subscriptions_.end(), sub),
                    subscriptions_.end());
            }
        }
    }
}

void Lifecycle::SetCampaignActive(bool active)
{
    std::lock_guard lock(mutex_);
    campaignActive_ = active;
}

bool Lifecycle::CampaignActive() const
{
    std::lock_guard lock(mutex_);
    return campaignActive_;
}

std::uint32_t Lifecycle::CurrentPhase() const
{
    std::lock_guard lock(mutex_);
    std::uint32_t current = DD_PHASE_COUNT;
    for (std::uint32_t phase = 0; phase < DD_PHASE_COUNT; ++phase) {
        if (IsOneShot(phase) && reached_[phase]) {
            current = phase;
        }
    }
    return current;
}

}  // namespace ddc

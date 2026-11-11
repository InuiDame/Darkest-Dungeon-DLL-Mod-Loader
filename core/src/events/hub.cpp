#include "hub.h"

#include <algorithm>
#include <cstring>

namespace ddc::events {

Hub::Hub(Invoke invoke, LogFn log) : invoke_(std::move(invoke)), log_(std::move(log)) {}

Topic* Hub::AddTopic(const std::string& name, TopicKind kind, std::vector<FieldDesc> fields, bool writable,
    bool cancellable)
{
    std::lock_guard lock(mutex_);
    if (const auto it = byName_.find(name); it != byName_.end()) {
        return nullptr;
    }
    auto topic = std::make_unique<Topic>();
    topic->name = name;
    topic->kind = kind;
    topic->index = static_cast<std::uint32_t>(topics_.size());
    topic->writable = writable;
    topic->cancellable = cancellable;
    topic->fields = std::move(fields);
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".pre") == 0) {
        topic->phase = DD_EVENT_F_PRE;
    } else if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".post") == 0) {
        topic->phase = DD_EVENT_F_POST;
    }
    Topic* raw = topic.get();
    byName_.emplace(name, raw);
    topics_.push_back(std::move(topic));
    return raw;
}

Topic* Hub::Find(std::string_view name)
{
    std::lock_guard lock(mutex_);
    const auto it = byName_.find(std::string(name));
    return it == byName_.end() ? nullptr : it->second;
}

void Hub::SetAvailability(Topic* topic, bool available, const std::string& reason)
{
    if (!topic) {
        return;
    }
    std::lock_guard lock(mutex_);
    topic->available = available;
    topic->unavailableReason = reason;
}

std::vector<Topic*> Hub::Topics() const
{
    std::lock_guard lock(mutex_);
    std::vector<Topic*> out;
    out.reserve(topics_.size());
    for (const auto& topic : topics_) {
        out.push_back(topic.get());
    }
    return out;
}

std::uint32_t Hub::TopicCount() const
{
    std::lock_guard lock(mutex_);
    return static_cast<std::uint32_t>(topics_.size());
}

Topic* Hub::TopicAt(std::uint32_t index) const
{
    std::lock_guard lock(mutex_);
    return index < topics_.size() ? topics_[index].get() : nullptr;
}

void Hub::InsertSorted(Topic& topic, std::shared_ptr<Subscription> sub)
{
    const auto position = std::upper_bound(topic.subs.begin(), topic.subs.end(), sub,
        [](const std::shared_ptr<Subscription>& a, const std::shared_ptr<Subscription>& b) {
            if (a->priority != b->priority) {
                return a->priority < b->priority;
            }
            return a->seq < b->seq;
        });
    topic.subs.insert(position, std::move(sub));
}

void Hub::NotifyEnd(const Subscription& sub, std::uint32_t reason)
{
    if (!sub.onEnd) {
        return;
    }
    if (endInvoke_) {
        endInvoke_(sub.onEnd, sub.id, sub.user, reason, sub.owner);
    } else {
        sub.onEnd(sub.id, sub.user, reason);
    }
}

std::uint64_t Hub::Subscribe(std::uint32_t owner, const std::string& topic, DDEventFn fn, void* user,
    std::int32_t priority, std::uint32_t scope, std::string* reason, bool internal, DDEventEndFn onEnd)
{
    if (!fn) {
        if (reason) {
            *reason = "回调为空";
        }
        return 0;
    }
    if (scope > DD_EVENT_SCOPE_BATTLE) {
        if (reason) {
            *reason = "scope 取值无效";
        }
        return 0;
    }
    std::lock_guard lock(mutex_);
    const auto it = byName_.find(topic);
    if (it == byName_.end()) {
        if (reason) {
            *reason = "主题不存在";
        }
        return 0;
    }
    Topic& t = *it->second;
    if (!t.available) {
        if (reason) {
            *reason = "主题不可用：" + (t.unavailableReason.empty() ? std::string("未知原因") : t.unavailableReason);
        }
        return 0;
    }
    auto sub = std::make_shared<Subscription>();
    sub->id = nextId_++;
    sub->owner = owner;
    sub->topic = &t;
    sub->fn = fn;
    sub->user = user;
    sub->onEnd = onEnd;
    sub->priority = priority;
    sub->seq = nextSeq_++;
    sub->scope = scope;
    sub->internal = internal;
    const std::uint64_t id = sub->id;
    InsertSorted(t, std::move(sub));
    dirty_.store(true);
    return id;
}

bool Hub::Unsubscribe(std::uint32_t owner, std::uint64_t id)
{
    std::lock_guard lock(mutex_);
    for (const auto& topic : topics_) {
        const auto it = std::find_if(topic->subs.begin(), topic->subs.end(),
            [&](const std::shared_ptr<Subscription>& s) { return s->id == id && s->owner == owner; });
        if (it != topic->subs.end()) {
            (*it)->alive.store(false);
            topic->subs.erase(it);
            dirty_.store(true);
            return true;
        }
    }
    return false;
}

std::size_t Hub::RemoveOwner(std::uint32_t owner)
{
    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (const auto& topic : topics_) {
        auto& subs = topic->subs;
        for (auto it = subs.begin(); it != subs.end();) {
            if ((*it)->owner == owner) {
                (*it)->alive.store(false);
                it = subs.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    if (removed) {
        dirty_.store(true);
    }
    return removed;
}

std::size_t Hub::EndScope(std::uint32_t scope)
{
    std::vector<std::shared_ptr<Subscription>> ended;
    {
        std::lock_guard lock(mutex_);
        for (const auto& topic : topics_) {
            auto& subs = topic->subs;
            for (auto it = subs.begin(); it != subs.end();) {
                if ((*it)->scope == scope && !(*it)->internal) {
                    (*it)->alive.store(false);
                    ended.push_back(*it);
                    it = subs.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!ended.empty()) {
            dirty_.store(true);
        }
    }
    // 锁外通知：插件在通知里可以再订阅、取消别的订阅
    for (const auto& sub : ended) {
        NotifyEnd(*sub, DD_EVENT_END_SCOPE);
    }
    return ended.size();
}

std::size_t Hub::OwnerCount(std::uint32_t owner) const
{
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& topic : topics_) {
        for (const auto& sub : topic->subs) {
            if (sub->owner == owner && !sub->internal) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t Hub::SubscriberCount(const Topic* topic) const
{
    std::lock_guard lock(mutex_);
    return topic ? topic->subs.size() : 0;
}

std::size_t Hub::ScopedCount(std::uint32_t scope) const
{
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& topic : topics_) {
        for (const auto& sub : topic->subs) {
            if (sub->scope == scope && !sub->internal) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t Hub::ExternalSubscriberCount() const
{
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& topic : topics_) {
        for (const auto& sub : topic->subs) {
            if (!sub->internal) {
                ++count;
            }
        }
    }
    return count;
}

bool Hub::Dispatch(Topic* topic, Payload& payload)
{
    if (!topic) {
        return true;
    }
    std::vector<std::shared_ptr<Subscription>> snapshot;
    {
        std::lock_guard lock(mutex_);
        if (topic->subs.empty()) {
            return true;
        }
        snapshot = topic->subs;
    }

    if (depth_ >= DD_EVENT_MAX_DEPTH) {
        log_(DD_LOG_ERROR, "事件嵌套深度超过上限 " + std::to_string(DD_EVENT_MAX_DEPTH) + "，主题 " + topic->name +
            " 的本次派发已停止");
        return false;
    }

    ++depth_;
    payload.topic = topic;
    payload.flags |= topic->phase | (payload.allowWrite ? DD_EVENT_F_WRITABLE : 0);
    auto* event = reinterpret_cast<DDEvent*>(&payload);
    for (const auto& sub : snapshot) {
        if (!sub->alive.load()) {
            continue;
        }
        if (!invoke_(sub->fn, event, sub->user, sub->owner)) {
            log_(DD_LOG_ERROR, "主题 " + topic->name + " 的订阅 " + std::to_string(sub->id) +
                " 回调崩溃，已取消该订阅");
            bool removed = false;
            {
                std::lock_guard lock(mutex_);
                if (sub->alive.exchange(false)) {
                    auto& subs = sub->topic->subs;
                    subs.erase(std::remove(subs.begin(), subs.end(), sub), subs.end());
                    dirty_.store(true);
                    removed = true;
                }
            }
            if (removed) {
                NotifyEnd(*sub, DD_EVENT_END_CRASH);
            }
        }
    }
    --depth_;
    return true;
}

void Hub::EnqueueDeferred(Topic* topic, const Slot* slots, std::uint32_t originThread)
{
    if (!topic) {
        return;
    }
    Deferred item;
    item.topic = topic;
    item.originThread = originThread;
    item.slots.assign(slots, slots + topic->fields.size());
    for (std::size_t i = 0; i < topic->fields.size(); ++i) {
        Slot& slot = item.slots[i];
        if (!slot.set) {
            continue;
        }
        if (topic->fields[i].type == DD_FIELD_POINTER) {
            slot.v.p = nullptr;  // 对象不一定还活着，延迟负载不暴露指针
        } else if (topic->fields[i].type == DD_FIELD_STRING && slot.v.s) {
            std::size_t length = strnlen(slot.v.s, 1023);
            auto copy = std::make_unique<char[]>(length + 1);
            std::memcpy(copy.get(), slot.v.s, length);
            copy[length] = 0;
            slot.v.s = copy.get();
            item.strings.push_back(std::move(copy));
        }
    }
    std::lock_guard lock(mutex_);
    deferred_.push_back(std::move(item));
}

std::size_t Hub::DeferredCount() const
{
    std::lock_guard lock(mutex_);
    return deferred_.size();
}

std::size_t Hub::DrainDeferred()
{
    if (draining_) {
        return 0;  // 派发回调里再次 Drain：留给外层
    }
    std::vector<Deferred> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(deferred_);
    }
    draining_ = true;
    for (auto& item : batch) {
        Payload payload;
        payload.slots = item.slots.data();
        payload.flags = DD_EVENT_F_DEFERRED;
        payload.originThread = item.originThread;
        payload.allowWrite = false;
        Dispatch(item.topic, payload);
    }
    draining_ = false;
    return batch.size();
}

const FieldDesc* Hub::FindField(const Topic* topic, const char* name, std::uint32_t* index)
{
    if (!topic || !name) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < topic->fields.size(); ++i) {
        if (topic->fields[i].name == name) {
            if (index) {
                *index = i;
            }
            return &topic->fields[i];
        }
    }
    return nullptr;
}

bool Hub::TestAndClearDirty()
{
    return dirty_.exchange(false);
}

void Hub::MarkDirty()
{
    dirty_.store(true);
}

}  // namespace ddc::events

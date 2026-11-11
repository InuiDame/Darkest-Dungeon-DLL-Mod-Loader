#include "task_queue.h"

#include <algorithm>

namespace ddc {

std::uint64_t TaskQueue::Post(std::uint32_t owner, DDTaskFn fn, void* user, std::uint64_t now, std::uint32_t delayMs)
{
    if (!fn) {
        return 0;
    }
    std::lock_guard lock(mutex_);
    const std::uint64_t id = nextId_++;
    tasks_.push_back({ id, owner, fn, user, now + delayMs });
    return id;
}

bool TaskQueue::Cancel(std::uint32_t owner, std::uint64_t id)
{
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(tasks_.begin(), tasks_.end(),
        [&](const Task& t) { return t.id == id && t.owner == owner; });
    if (it == tasks_.end()) {
        return false;
    }
    tasks_.erase(it);
    return true;
}

std::size_t TaskQueue::RemoveOwner(std::uint32_t owner)
{
    std::lock_guard lock(mutex_);
    const auto before = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [&](const Task& t) { return t.owner == owner; }),
        tasks_.end());
    return before - tasks_.size();
}

void TaskQueue::Drain(std::uint64_t now, const Invoke& invoke)
{
    std::uint64_t batchEnd = 0;
    {
        std::lock_guard lock(mutex_);
        batchEnd = nextId_;
    }

    while (true) {
        Task task;
        {
            std::lock_guard lock(mutex_);
            // 同一批内按提交顺序（id 递增），取到期且编号在本批之内的最小编号
            auto next = tasks_.end();
            for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
                if (it->id < batchEnd && it->due <= now && (next == tasks_.end() || it->id < next->id)) {
                    next = it;
                }
            }
            if (next == tasks_.end()) {
                return;
            }
            task = *next;
            tasks_.erase(next);
        }
        invoke(task.fn, task.user, task.owner, task.id);
    }
}

std::size_t TaskQueue::Pending() const
{
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

}  // namespace ddc

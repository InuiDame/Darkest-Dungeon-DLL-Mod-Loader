#pragma once
// 主线程任务队列。Post 线程安全，Drain 只在主线程调用。
// 时间由调用方传入，便于单元测试。

#include <dd/dd_api.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ddc {

class TaskQueue
{
public:
    using Invoke = std::function<void(DDTaskFn, void*, std::uint32_t owner, std::uint64_t id)>;

    std::uint64_t Post(std::uint32_t owner, DDTaskFn fn, void* user, std::uint64_t now, std::uint32_t delayMs);
    bool Cancel(std::uint32_t owner, std::uint64_t id);
    // 移除 owner 全部尚未执行的任务，返回数量
    std::size_t RemoveOwner(std::uint32_t owner);

    // 执行到期的任务：同一批内按提交顺序；任务里新提交的留到下一次 Drain
    void Drain(std::uint64_t now, const Invoke& invoke);

    std::size_t Pending() const;

private:
    struct Task {
        std::uint64_t id = 0;
        std::uint32_t owner = 0;
        DDTaskFn fn = nullptr;
        void* user = nullptr;
        std::uint64_t due = 0;
    };

    mutable std::mutex mutex_;
    std::vector<Task> tasks_;
    std::uint64_t nextId_ = 1;
};

}  // namespace ddc

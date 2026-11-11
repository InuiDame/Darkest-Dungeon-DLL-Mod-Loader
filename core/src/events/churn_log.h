#pragma once
// 语义 hook 安装、移除和 listener 注册、注销的日志限速（第三轮审核 R3A-备注3）。
//
// Mod 隔帧订阅、撤销同一个语义主题（或原生主题）时，FrameBoundary 的 Reconcile 每帧安装或移除一次 hook、注册或注销一次
// listener，每次写一行 INFO，每秒约 60 行。DDLua 的日志限速和总量只管 DDLua.log，管不到 DDCore.log。
// 这里按名字限速：每个名字在一个窗口（kWindowMs）里最多逐条写 kVerbatim 行，多出来的只计数；窗口结束后写一行汇总
// （这段时间里另外装、卸各几次）。正常使用（进出存档、偶尔订阅）一个窗口里只有几次装卸，照常逐条写。
// 时间由调用方给（毫秒，单调），方便离线测试。

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ddc::events {

class ChurnLog {
public:
    static constexpr std::uint32_t kVerbatim = 8;     // 每个名字每个窗口逐条写的行数
    static constexpr std::uint64_t kWindowMs = 60000; // 窗口长度

    // 名字为 name 的 hook 或 listener 装上（on）或卸下了一次。返回真时调用方照常写这一行；返回假时已计入汇总
    bool Allow(const std::string& name, bool on, std::uint64_t nowMs)
    {
        Entry& entry = entries_[name];
        if (entry.lines == 0 || nowMs - entry.windowStart >= kWindowMs) {
            if (entry.suppressedOn || entry.suppressedOff) {
                pending_.push_back(Summary(name, entry, nowMs));
            }
            entry = Entry{};
            entry.windowStart = nowMs;
        }
        if (entry.lines < kVerbatim) {
            ++entry.lines;
            return true;
        }
        if (entry.suppressedOn == 0 && entry.suppressedOff == 0) {
            ++suppressing_;
        }
        ++(on ? entry.suppressedOn : entry.suppressedOff);
        return false;
    }

    // 写出到期的汇总（每帧调用；没有被压下的行时只比较一个计数）。force 为真时不等窗口结束，全部写出（退出时）
    void Flush(std::uint64_t nowMs, const std::function<void(const std::string&)>& write, bool force = false)
    {
        for (auto& line : pending_) {
            write(line);
        }
        pending_.clear();
        if (suppressing_ == 0) {
            return;
        }
        for (auto& [name, entry] : entries_) {
            if ((entry.suppressedOn || entry.suppressedOff) && (force || nowMs - entry.windowStart >= kWindowMs)) {
                write(Summary(name, entry, nowMs));  // Summary 里减 suppressing_
                entry = Entry{};                     // 下一次装卸开新窗口
            }
        }
    }

    std::size_t Suppressing() const { return suppressing_; }
    // 没有要写的汇总（Flush 什么都不会做）
    bool Idle() const { return pending_.empty() && suppressing_ == 0; }

private:
    struct Entry {
        std::uint64_t windowStart = 0;
        std::uint32_t lines = 0;  // 这个窗口里逐条写了几行
        std::uint32_t suppressedOn = 0;
        std::uint32_t suppressedOff = 0;
    };

    std::string Summary(const std::string& name, Entry& entry, std::uint64_t nowMs)
    {
        if (entry.suppressedOn || entry.suppressedOff) {
            if (suppressing_ > 0) {
                --suppressing_;
            }
        }
        const std::string text = name + "：最近 " + std::to_string((nowMs - entry.windowStart) / 1000) + " 秒里另外装上 " +
            std::to_string(entry.suppressedOn) + " 次、卸下 " + std::to_string(entry.suppressedOff) + " 次（逐条日志每 " +
            std::to_string(kWindowMs / 1000) + " 秒最多 " + std::to_string(kVerbatim) + " 行，其余合并到这一行；" +
            "通常是 Mod 反复订阅、撤销同一个主题）";
        entry.suppressedOn = 0;
        entry.suppressedOff = 0;
        return text;
    }

    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> pending_;  // Allow 开新窗口时发现的上一个窗口的汇总，下一次 Flush 写出
    std::size_t suppressing_ = 0;       // 有被压下的行的名字数
};

}  // namespace ddc::events

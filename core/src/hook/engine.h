#pragma once
// hook 引擎：内联 hook 和虚表 hook 的安装、串链和移除。
//
// 同一目标只改一次，所有调用者串在一条链上。链的每一环是一个可重定向的小跳板，
// 移除中间一环只需要改前一环的跳转目标，不再动目标函数。
// 不依赖游戏，单元测试用假函数验证。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ddc::hook {

// 内部 owner 编号，0 留给 DDCore 自身
inline constexpr std::uint32_t kCoreOwner = 0;

enum class Result {
    Ok = 0,
    BadArgument = 1,
    ExpectedMismatch = 2,
    AlreadyHooked = 3,
    UnsafePrologue = 4,
    NoMemory = 5,
    ProtectFailed = 6,
    NotFound = 7,
    NotOwner = 8,
    BadTiming = 9,
};

const char* ResultText(Result result);

struct InlineRequest {
    const char* name;
    void* target;
    const std::uint8_t* expected;
    std::uint32_t expectedLength;
    std::int32_t priority;
    void* detour;
    void** original;
    // 只给 DDCore 自身用：目标入口已被旧式 DLL 打成 E9/FF25 时，把入口跳转搬进 gate 改成绝对跳转，
    // 串联而不是拒绝。hook 服务（插件）不会设置它，对外语义不变。
    bool chainForeignJump = false;
};

struct VtableRequest {
    const char* name;
    void** slot;
    void* expectedValue;  // 可为空，表示不校验
    std::int32_t priority;
    void* detour;
    void** original;
};

struct InstallResult {
    Result result = Result::Ok;
    std::string detail;  // 失败时的可读原因，供日志和返回错误
    std::uint64_t handle = 0;
    bool freshPatch = false;  // true 表示这次打了补丁，false 表示串到已有的链
    std::uint32_t chainDepth = 0;
    bool chainedForeign = false;  // true 表示把入口的第三方跳转搬进 gate 串联
};

struct NodeInfo {
    std::uint32_t owner = 0;
    std::string name;
    std::int32_t priority = 0;
    std::uint64_t handle = 0;
};

struct TargetInfo {
    bool isInline = true;
    const void* address = nullptr;  // 内联：函数入口；虚表：槽位地址
    std::uint32_t stealLength = 0;  // 内联
    bool intact = true;             // 补丁是否还在
    bool orphaned = false;          // 链空了但恢复不了原始字节（被他人覆盖或写入失败），只保留直通
    bool foreignChained = false;    // 入口是第三方跳转，由 gate 串联
    std::vector<NodeInfo> chain;    // 按调用顺序
};

// 写代码段的函数，成功返回 true
using MemoryWriter = bool (*)(void* address, const void* source, std::size_t size);

class Engine
{
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    InstallResult InstallInline(std::uint32_t owner, const InlineRequest& request);
    InstallResult InstallVtable(std::uint32_t owner, const VtableRequest& request);

    // 移除后其余环不受影响。目标被他人覆盖时不恢复字节，只把 detour 摘出链；detail 说明情况。
    Result Remove(std::uint32_t owner, std::uint64_t handle, std::string* detail);
    // 移除 owner 的全部 hook，返回移除数量，每项的说明写入 details
    std::size_t RemoveOwner(std::uint32_t owner, std::vector<std::string>* details);

    // 当前全部目标的状态，补丁是否仍在会重新比对内存
    std::vector<TargetInfo> Snapshot() const;

    // 把本引擎打过补丁的内联目标在 copy（base 开始的完整副本，长度 size）里还原成原始字节。
    // 特征码扫描用：后加载的插件要能看到被先加载的插件 hook 过的函数入口
    void RestoreOriginalBytes(const std::uint8_t* base, std::uint8_t* copy, std::size_t size) const;

    // 只给单元测试模拟 VirtualProtect 失败，传 nullptr 恢复默认
    void SetMemoryWriterForTest(MemoryWriter writer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddc::hook

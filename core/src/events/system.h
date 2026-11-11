#pragma once
// 事件系统的游戏侧粘合层：把事件中枢（hub）和游戏事件总线、Query、语义 hook 接起来。
// 只在 DDCore 中编译，单元测试只用 hub。

#include "hub.h"

#include "../game/game.h"
#include "../game/phase_tracker.h"
#include "../hook/engine.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ddc::events {

struct SystemDeps {
    game::Addresses* addr = nullptr;
    hook::Engine* hooks = nullptr;
    DWORD mainThread = 0;
    HMODULE exe = nullptr;
    Hub::Invoke invoke;
    std::function<void(int level, const std::string& message)> log;
};

class System
{
public:
    explicit System(SystemDeps deps);
    ~System();

    Hub& hub();

    // EventManager 构造完成（CreateAppSystems 返回后）调用一次
    void OnAppReady();
    // 帧边界（UpdateStateInternal 入口，主线程）：同步 listener 和 hook，处理延迟事件，派发 app.state / app.tick
    void FrameBoundary(const GameSample& sample, std::uint64_t frame, std::uint32_t campaignSerial, bool assetsReady);
    void OnCampaignLoad(std::uint32_t serial);
    void OnCampaignUnload(std::uint32_t serial);

    // 三条加载线程之一正在运行。只在主线程读
    bool Loading() const;

    // 诊断：已注册的 native listener 数和已安装的语义 hook 数
    std::size_t ListenerCount() const;
    std::size_t InstalledHookCount() const;
    std::string HookReportSummary() const;
    // 在日志里写一行排序汇总（通用 listener 相对游戏回调的顺序）
    void LogOrderSummary(const char* when);

    struct Impl;  // 回调入口需要访问

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddc::events

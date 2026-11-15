#pragma once
// persist 服务的游戏侧（P4c1）：4 个观察 hook、当前 profile 和账号、campaign.save 主题、旁路的读写和一致性核对、
// DDCore 自己的标签表随存档保存。不依赖游戏的部分（格式、判断、写盘线程、backup 同步）在 store.h，单元测试覆盖。
//
// 依据：knowledge/dd1/campaign.md 的「存档」各节，探针 probe_p4 在用户场景里跑通过的做法（P4-0C、P4-0D）。
// 设计见 docs/adr/0031-persist-sidecar-content-hash.md、docs/02-architecture.md 的「persist 服务」。
//
// 线程：观察 hook 在任何线程上都只读内存、算哈希、加锁记录；其余（派发、读写旁路、回调）都在主线程。

#include "store.h"

#include "../combat/combat.h"
#include "../events/system.h"
#include "../game/actors.h"
#include "../game/game.h"
#include "../hook/engine.h"

#include <dd/dd_api.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ddc::persist {

struct PersistDeps {
    game::Addresses* addr = nullptr;
    events::System* events = nullptr;   // campaign.save 主题；为空时主题不可用，保存回调照常
    hook::Engine* hooks = nullptr;
    combat::Combat* combat = nullptr;   // DDCore 的标签表；为空时不保存标签
    DWORD mainThread = 0;
    std::wstring gameRoot;
    std::string buildLabel;              // "steam"、"nosteam"、"unknown"
    std::function<void(int level, const std::string& message)> log;
    std::function<std::string(std::uint32_t owner)> pluginName;
    // 插件名 → owner（只认加载成功的插件），found 写是否找到
    std::function<std::uint32_t(const std::string& name, bool* found)> ownerOf;
    std::function<bool()> campaignActive;
    std::function<bool()> loadingNow;
    std::function<std::uint32_t()> campaignSerial;
    std::function<game::ActorEnv()> actorEnv;
    // SEH 隔离的回调调用，崩溃返回 false
    std::function<bool(DDPersistSaveFn fn, const DDPersistSaveInfo* info, void* user, std::uint32_t owner)> invokeSave;
    std::function<bool(DDPersistVerifyFn fn, const DDPersistVerify* verify, void* user, std::uint32_t owner)> invokeVerify;
};

class Persist
{
public:
    explicit Persist(PersistDeps deps);
    ~Persist();
    Persist(const Persist&) = delete;
    Persist& operator=(const Persist&) = delete;

    // 服务（DDPersistResult）
    std::uint32_t Declare(std::uint32_t owner, DDPersistSaveFn onSave, DDPersistVerifyFn onVerify, void* user);
    std::uint32_t Load(std::uint32_t owner, const std::string& key, DDPersistData* out);
    std::uint32_t Save(std::uint32_t owner, const std::string& key, std::string data, std::string version);
    bool Info(DDPersistInfo* out);
    static const char* StatusText(std::uint32_t status);

    // DDCore 的时机（都在主线程）
    void OnAppReady();                    // 有插件声明时安装观察 hook
    void OnFrameBoundary();               // 派发 campaign.save；地牢存档的 raid 核对
    void BeforeCampaignUnload(bool alive);  // 还没派发的 campaign.save 先派发（alive：Campaign 还在）
    void OnCampaignLoad();                // 读档的一致性依据；恢复 DDCore 的标签（英雄）
    void OnCampaignUnload();              // 等后台写完，清掉这次进存档的状态
    void OnPluginRollback(std::uint32_t owner);
    bool InSaveCallback() const;          // 正在派发 campaign.save 或调用保存回调（combat 按事件回调处理写操作）
    std::size_t HookCount() const;        // 装上的观察 hook 数
    std::string Summary() const;          // hook 报告里的一行

    struct Impl;  // detour 需要访问

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddc::persist

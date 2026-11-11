#pragma once
// 把 signatures.inc 的特征码解析成进程内地址，并做布局校验。

#include "phase_tracker.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ddc::game {

struct Addresses {
    std::uint8_t* createAppSystems = nullptr;
    std::uint8_t* updateState = nullptr;
    volatile int* stateLast = nullptr;
    volatile int* stateNext = nullptr;
    void* volatile* campaignCurrent = nullptr;
    void* volatile* buffLibrary = nullptr;  // 可选，可能为空
    std::vector<const char*> stateNames;    // 下标是状态序号
    StateIds states;

    // 事件总线（必需）
    void** eventManager = nullptr;
    const int* eventTypeCount = nullptr;
    const char* eventClassNames = nullptr;  // 0x80 字节一项
    void* listenerAdd = nullptr;            // ListenerListData::Add
    // Query（可选，全部解析到才可用）
    void** queryDispatcher = nullptr;
    const char* queryClassNames = nullptr;  // 运行时按内容选定
    int queryTypeCount = 0;                 // 类名表项数
    void* queryListenerAdd = nullptr;
    // 语义 hook（可选）
    void* const* heroVtable = nullptr;
    void* const* monsterVtable = nullptr;
    void** raidCurrent = nullptr;
    std::uint8_t* combatStartTurn = nullptr;
    std::uint8_t* applyHit = nullptr;
    std::uint8_t* applyMiss = nullptr;
    std::uint8_t* doAttackSkill = nullptr;
    std::uint8_t* doSkillFriendly = nullptr;
    std::uint8_t* advanceRound = nullptr;
    std::uint8_t* effectApply = nullptr;
    // 启用 Mod 列表（可选，P3）
    void* const* userServices = nullptr;    // UserServicesInterface* 全局
    const void* appliedUgcVector = nullptr; // 当前存档启用列表的 vector<UGCId>
    // actor 句柄（可选，P3c）：最大生命的计算函数
    const void* actorMaxStat = nullptr;
};

struct ResolveReport {
    std::vector<std::string> missing;      // 必需特征码缺失的说明
    std::vector<std::string> layoutErrors; // 布局校验失败的说明
    std::vector<std::string> notes;        // 可选特征码缺失等提示
};

// 在 exe 中解析全部特征码并校验布局。返回 false 时 report 写明缺了什么。
bool Resolve(HMODULE exe, Addresses& out, ResolveReport& report);

// HOOK 类特征码的前 steal 字节，就是安装 hook 时要比对的入口字节。id 不存在或含通配时返回 false
bool HookEntryBytes(const char* id, std::vector<std::uint8_t>& out);

// 已知版本的名字（"steam"/"nosteam"），未知返回 nullptr
const char* BuildLabel(const std::string& sha1);

}  // namespace ddc::game

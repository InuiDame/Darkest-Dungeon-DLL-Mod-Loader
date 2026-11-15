#pragma once
// DDLua 的宿主接口：运行时只通过这里拿到的 SDK 服务表调用下层（ADR 0003）。
// 真实宿主由 plugin.cpp 从 DDApi 填写；离线测试（dd_lua_tests）填写假的服务表。

#include <dd/dd_api.h>

#include <string>
#include <string_view>

namespace ddlua {

struct Host {
    DDPluginHandle plugin = nullptr;
    const DDLogService* log = nullptr;
    const DDGameService* game = nullptr;            // 启用 Mod 列表
    const DDGameInfoService* info = nullptr;
    const DDLifecycleService* lifecycle = nullptr;
    const DDEventsService* events = nullptr;        // P3b
    const DDTaskService* task = nullptr;            // P3b
    const DDCombatService* combat = nullptr;        // P4b：dd.buff、dd.effect、actor 的 buff 方法；没有时这些 API 返回「不可用」
    const DDPersistService* persist = nullptr;      // P4c1：dd.store 和脚本 Buff 状态随存档保存；没有时 dd.store 只在内存里

    void Log(int level, std::string_view text) const
    {
        if (log) {
            const std::string line(text);
            log->write(plugin, level, line.c_str());
        }
    }
};

}  // namespace ddlua

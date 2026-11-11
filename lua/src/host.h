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

    void Log(int level, std::string_view text) const
    {
        if (log) {
            const std::string line(text);
            log->write(plugin, level, line.c_str());
        }
    }
};

}  // namespace ddlua

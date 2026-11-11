#pragma once
// 当前存档的启用 Mod 列表（game 服务）。读游戏内存全部在 SEH 保护下进行，结构校验失败时返回 false。
// 结构依据：knowledge/dd1/data-and-mods.md 的「运行时的结构」「已安装 Mod 和目录」。

#include <cstdint>
#include <string>
#include <vector>

namespace ddc::game {

struct EnabledMod {
    std::string name;    // UGCId.name
    std::string source;  // UGCId.source
    std::string dir;     // 绝对路径（UTF-8），没找到时为空
    bool found = false;  // 在已安装 UGC 中找到且目录存在
    std::string rawPath; // 游戏里记录的原始路径（诊断用）
};

struct EnabledModsResult {
    bool ok = false;
    std::string error;   // ok 为 false 时的原因
    std::size_t installedCount = 0;
    std::vector<EnabledMod> mods;
};

// userServices：UserServicesInterface* 全局（118DFE8）；appliedVector：当前存档启用列表的 vector（2A6D8C0）。
// gameRoot 用来把本地 Mod 的相对路径变成绝对路径（UTF-8，不带结尾分隔符）。
EnabledModsResult ReadEnabledMods(void* const* userServices, const void* appliedVector, const std::string& gameRoot);

}  // namespace ddc::game

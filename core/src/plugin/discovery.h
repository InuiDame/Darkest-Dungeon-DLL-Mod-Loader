#pragma once
// 新协议插件的发现：目录顺序、启用状态和同名去重都和加载器的旧式路径一致。
// 只读 PE 导出表判断协议，不加载 DLL。

#include <string>
#include <vector>

namespace ddc {

struct PluginCandidate {
    std::wstring path;      // 完整路径
    std::wstring name;      // 文件名
    bool forced = false;    // 来自 DD Mod Lib
};

struct DiscoveryResult {
    std::vector<PluginCandidate> candidates;
    std::vector<std::wstring> notes;  // 跳过等信息，写日志用
};

// 加载器定义的排除名单，复制一份，保持一致
bool IsExcludedDllName(const std::wstring& lowerName);

DiscoveryResult DiscoverPlugins(const std::wstring& gameRoot);

// 配置文件解析（DDModLoader\cfg\DDModLoader.cfg），路径小写化后作键
void LoadDllConfig(const std::wstring& path);
// 返回 false 表示配置禁用。isNew 写入“不在 cfg 中”。
bool DllEnabled(const std::wstring& fullPath, bool& isNew);

}  // namespace ddc

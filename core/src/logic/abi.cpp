#include "abi.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace ddc {

namespace {

std::string AbiText(std::uint32_t abi)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u", DD_ABI_GET_MAJOR(abi), DD_ABI_GET_MINOR(abi));
    return buffer;
}

bool ValidName(const char* name, std::string* reason)
{
    if (!name || !name[0]) {
        if (reason) {
            *reason = "name 为空";
        }
        return false;
    }
    std::size_t length = 0;
    for (; name[length]; ++length) {
        if (length >= DD_PLUGIN_MAX_NAME) {
            if (reason) {
                *reason = "name 超过长度限制";
            }
            return false;
        }
        const auto c = static_cast<unsigned char>(name[length]);
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') {
            if (reason) {
                *reason = "name 含有非法字符";
            }
            return false;
        }
    }
    return true;
}

}  // namespace

bool AbiCompatible(std::uint32_t pluginAbi, std::uint32_t coreAbi, std::string* reason)
{
    if (DD_ABI_GET_MAJOR(pluginAbi) != DD_ABI_GET_MAJOR(coreAbi)) {
        if (reason) {
            *reason = "ABI 主版本不匹配：插件 " + AbiText(pluginAbi) + "，DDCore " + AbiText(coreAbi);
        }
        return false;
    }
    if (DD_ABI_GET_MINOR(pluginAbi) > DD_ABI_GET_MINOR(coreAbi)) {
        if (reason) {
            *reason = "插件需要 ABI " + AbiText(pluginAbi) + "，DDCore 只提供 " + AbiText(coreAbi);
        }
        return false;
    }
    return true;
}

bool ValidatePluginInfo(const DDPluginInfo& info, std::uint32_t coreAbi, std::string* reason)
{
    if (info.abi_version == 0) {
        if (reason) {
            *reason = "插件没有填写 abi_version";
        }
        return false;
    }
    if (!AbiCompatible(info.abi_version, coreAbi, reason)) {
        return false;
    }
    if (!ValidName(info.name, reason)) {
        return false;
    }
    if (info.dep_count > 64) {
        if (reason) {
            *reason = "依赖数量超过上限";
        }
        return false;
    }
    if (info.dep_count > 0 && !info.deps) {
        if (reason) {
            *reason = "dep_count 不为 0 但 deps 为空";
        }
        return false;
    }
    for (std::uint32_t i = 0; i < info.dep_count; ++i) {
        const auto& dep = info.deps[i];
        if (dep.reserved != 0) {
            if (reason) {
                *reason = "依赖项 reserved 非 0，当前 ABI 不支持";
            }
            return false;
        }
        std::string depReason;
        if (!ValidName(dep.name, &depReason)) {
            if (reason) {
                *reason = "依赖项名称无效：" + depReason;
            }
            return false;
        }
    }
    return true;
}

}  // namespace ddc

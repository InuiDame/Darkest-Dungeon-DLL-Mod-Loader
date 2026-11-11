#pragma once
// ABI 协商和 DDPluginInfo 校验。纯逻辑，单元测试直接覆盖。

#include <dd/dd_api.h>

#include <cstdint>
#include <string>

namespace ddc {

// 主版本必须相同；插件的次版本高于 DDCore 时拒绝（需要 DDCore 尚不具备的追加能力）。
bool AbiCompatible(std::uint32_t pluginAbi, std::uint32_t coreAbi, std::string* reason);

// 校验 Query 填写的结构：ABI、名称、依赖等。不通过时 reason 写明原因。
bool ValidatePluginInfo(const DDPluginInfo& info, std::uint32_t coreAbi, std::string* reason);

}  // namespace ddc

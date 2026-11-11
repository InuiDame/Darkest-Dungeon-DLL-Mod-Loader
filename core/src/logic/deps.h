#pragma once
// 插件依赖的拓扑排序。纯逻辑，不依赖游戏，单元测试直接覆盖。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ddc {

struct PluginDep {
    std::string name;
    std::uint32_t minVersion = 0;
};

struct PluginNode {
    std::string name;
    std::uint32_t version = 0;
    std::vector<PluginDep> deps;
};

struct DepRejection {
    std::size_t index = 0;
    std::string reason;
};

struct DepResult {
    std::vector<std::size_t> order;       // 可加载的结点下标，满足依赖的前提下保持输入顺序
    std::vector<DepRejection> rejected;   // 缺依赖、版本不够、依赖被拒绝或成环
};

// 输入假定 name 已去重。缺失依赖和循环依赖的插件都被拒绝，并写明原因。
DepResult ResolveDependencies(const std::vector<PluginNode>& nodes);

}  // namespace ddc

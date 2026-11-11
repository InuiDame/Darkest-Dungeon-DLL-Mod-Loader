#include "deps.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace ddc {

namespace {

enum class State { Pending, Emitted, Rejected };

std::string VersionText(std::uint32_t version)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u", version >> 24, (version >> 12) & 0xFFF, version & 0xFFF);
    return buffer;
}

// 从 start 沿着未决的依赖走，找到一个环并写成 a -> b -> a
std::string FindCycle(std::size_t start, const std::vector<PluginNode>& nodes,
    const std::unordered_map<std::string, std::size_t>& indexOf, const std::vector<State>& states)
{
    std::vector<std::size_t> path;
    std::vector<int> visitedAt(nodes.size(), -1);
    std::size_t current = start;
    while (true) {
        if (visitedAt[current] >= 0) {
            std::string text;
            for (std::size_t i = static_cast<std::size_t>(visitedAt[current]); i < path.size(); ++i) {
                text += nodes[path[i]].name + " -> ";
            }
            return text + nodes[current].name;
        }
        visitedAt[current] = static_cast<int>(path.size());
        path.push_back(current);

        bool advanced = false;
        for (const auto& dep : nodes[current].deps) {
            const auto it = indexOf.find(dep.name);
            if (it != indexOf.end() && states[it->second] == State::Pending) {
                current = it->second;
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            return nodes[start].name;
        }
    }
}

}  // namespace

DepResult ResolveDependencies(const std::vector<PluginNode>& nodes)
{
    DepResult result;
    std::unordered_map<std::string, std::size_t> indexOf;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        indexOf.emplace(nodes[i].name, i);
    }

    std::vector<State> states(nodes.size(), State::Pending);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (states[i] != State::Pending) {
                continue;
            }
            std::string rejectReason;
            bool blocked = false;
            for (const auto& dep : nodes[i].deps) {
                if (dep.name == nodes[i].name) {
                    rejectReason = "插件依赖自己";
                    break;
                }
                const auto it = indexOf.find(dep.name);
                if (it == indexOf.end()) {
                    rejectReason = "缺少依赖 " + dep.name;
                    break;
                }
                if (states[it->second] == State::Rejected) {
                    rejectReason = "依赖 " + dep.name + " 不可用";
                    break;
                }
                if (nodes[it->second].version < dep.minVersion) {
                    rejectReason = "依赖 " + dep.name + " 的版本 " + VersionText(nodes[it->second].version) +
                        " 低于要求的 " + VersionText(dep.minVersion);
                    break;
                }
                if (states[it->second] != State::Emitted) {
                    blocked = true;
                }
            }
            if (!rejectReason.empty()) {
                states[i] = State::Rejected;
                result.rejected.push_back({ i, std::move(rejectReason) });
                changed = true;
            } else if (!blocked) {
                states[i] = State::Emitted;
                result.order.push_back(i);
                changed = true;
            }
        }
    }

    // 剩下的都在环上或依赖环上的结点
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (states[i] == State::Pending) {
            result.rejected.push_back({ i, "依赖循环：" + FindCycle(i, nodes, indexOf, states) });
        }
    }
    return result;
}

}  // namespace ddc

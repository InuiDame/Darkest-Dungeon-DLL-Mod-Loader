#include "manifest.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <unordered_map>

namespace ddlua {

bool Version::Parse(std::string_view text, Version& out)
{
    std::uint32_t parts[3] = {};
    int count = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string_view piece = text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (piece.empty() || count == 3 || piece.size() > 9) {
            return false;
        }
        for (const char ch : piece) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        std::from_chars(piece.data(), piece.data() + piece.size(), parts[count]);
        ++count;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    out = { parts[0], parts[1], parts[2] };
    return true;
}

std::string Version::ToString() const
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool ValidModId(std::string_view id)
{
    if (id.empty() || id.size() > 63 || id.front() == '.') {
        return false;
    }
    for (const char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                        ch == '.' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool ParseManifest(std::string_view text, Manifest& out, std::string* error)
{
    const auto fail = [&](std::string reason) {
        if (error) {
            *error = std::move(reason);
        }
        return false;
    };
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    const auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded()) {
        return fail("manifest.json 不是合法的 JSON");
    }
    if (!json.is_object()) {
        return fail("manifest.json 的顶层必须是对象");
    }
    out = Manifest{};

    const auto id = json.find("id");
    if (id == json.end() || !id->is_string() || !ValidModId(id->get_ref<const std::string&>())) {
        return fail("id 缺失或不合法（字母数字和 _ . -，1..63 字节，不能以点开头）");
    }
    out.id = id->get<std::string>();

    const auto version = json.find("version");
    if (version == json.end() || !version->is_string() || !Version::Parse(version->get_ref<const std::string&>(), out.version)) {
        return fail("version 缺失或不合法（形如 \"1.2.0\"）");
    }
    out.versionText = version->get<std::string>();

    const auto api = json.find("api");
    if (api == json.end() || !api->is_number_integer()) {
        return fail("api 缺失或不是整数");
    }
    out.api = api->get<std::int64_t>();

    const auto deps = json.find("dependencies");
    if (deps != json.end() && !deps->is_null()) {
        if (!deps->is_array()) {
            return fail("dependencies 必须是数组");
        }
        for (const auto& item : *deps) {
            ManifestDep dep;
            if (item.is_string()) {
                dep.id = item.get<std::string>();
            } else if (item.is_object()) {
                const auto depId = item.find("id");
                if (depId == item.end() || !depId->is_string()) {
                    return fail("dependencies 的对象项缺少字符串 id");
                }
                dep.id = depId->get<std::string>();
                const auto depVersion = item.find("version");
                if (depVersion != item.end() && !depVersion->is_null()) {
                    if (!depVersion->is_string() || !Version::Parse(depVersion->get_ref<const std::string&>(), dep.minVersion)) {
                        return fail("依赖 " + dep.id + " 的 version 不合法");
                    }
                }
            } else {
                return fail("dependencies 的项必须是字符串或对象");
            }
            if (!ValidModId(dep.id)) {
                return fail("依赖 id 不合法：" + dep.id);
            }
            out.deps.push_back(std::move(dep));
        }
    }
    return true;
}

LoadOrder ResolveLoadOrder(const std::vector<Manifest>& mods, std::int64_t apiVersion)
{
    enum class State { Pending, Emitted, Rejected };
    LoadOrder result;
    const std::size_t n = mods.size();
    std::vector<State> states(n, State::Pending);
    const auto reject = [&](std::size_t i, std::string reason) {
        states[i] = State::Rejected;
        result.rejected.emplace_back(i, std::move(reason));
    };

    std::unordered_map<std::string, std::vector<std::size_t>> byId;
    for (std::size_t i = 0; i < n; ++i) {
        byId[mods[i].id].push_back(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto& same = byId[mods[i].id];
        if (same.size() > 1) {
            std::string others;
            for (const auto j : same) {
                if (j != i) {
                    others += (others.empty() ? "" : "、") + std::string("第 ") + std::to_string(j + 1) + " 个";
                }
            }
            reject(i, "id " + mods[i].id + " 重复（和启用列表里" + others + " Lua Mod 相同），全部不加载");
        } else if (mods[i].api != apiVersion) {
            reject(i, "api " + std::to_string(mods[i].api) + " 和 DDLua 的 Lua API 版本 " + std::to_string(apiVersion) + " 不符");
        }
    }
    // 唯一 id 到下标（重复的 id 不在表里，依赖它们按不可用处理）
    std::unordered_map<std::string, std::size_t> indexOf;
    for (const auto& [id, list] : byId) {
        if (list.size() == 1) {
            indexOf.emplace(id, list[0]);
        }
    }

    while (true) {
        // 先把能确定的拒绝传播完
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 0; i < n; ++i) {
                if (states[i] != State::Pending) {
                    continue;
                }
                for (const auto& dep : mods[i].deps) {
                    if (dep.id == mods[i].id) {
                        reject(i, "依赖自己");
                        break;
                    }
                    const auto it = indexOf.find(dep.id);
                    if (it == indexOf.end()) {
                        reject(i, byId.count(dep.id) ? "依赖 " + dep.id + " 的 id 重复，不可用" : "缺少依赖 " + dep.id);
                        break;
                    }
                    if (states[it->second] == State::Rejected) {
                        reject(i, "依赖 " + dep.id + " 不加载");
                        break;
                    }
                    if (mods[it->second].version < dep.minVersion) {
                        reject(i, "依赖 " + dep.id + " 的版本 " + mods[it->second].version.ToString() + " 低于要求的 " +
                                      dep.minVersion.ToString());
                        break;
                    }
                }
                if (states[i] == State::Rejected) {
                    changed = true;
                }
            }
        }

        // 一层：依赖全部已经排出的，按输入顺序
        std::vector<std::size_t> layer;
        for (std::size_t i = 0; i < n; ++i) {
            if (states[i] != State::Pending) {
                continue;
            }
            bool ready = true;
            for (const auto& dep : mods[i].deps) {
                if (states[indexOf.at(dep.id)] != State::Emitted) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                layer.push_back(i);
            }
        }
        if (layer.empty()) {
            break;
        }
        for (const auto i : layer) {
            states[i] = State::Emitted;
            result.order.push_back(i);
        }
    }

    // 剩下的在环上或依赖环
    for (std::size_t i = 0; i < n; ++i) {
        if (states[i] != State::Pending) {
            continue;
        }
        std::string path = mods[i].id;
        std::vector<bool> seen(n, false);
        std::size_t current = i;
        seen[current] = true;
        for (std::size_t step = 0; step < n; ++step) {
            std::size_t next = n;
            for (const auto& dep : mods[current].deps) {
                const auto j = indexOf.at(dep.id);
                if (states[j] == State::Pending) {
                    next = j;
                    break;
                }
            }
            if (next == n) {
                break;
            }
            path += " -> " + mods[next].id;
            if (seen[next]) {
                break;
            }
            seen[next] = true;
            current = next;
        }
        result.rejected.emplace_back(i, "依赖循环：" + path);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (states[i] == State::Pending) {
            states[i] = State::Rejected;
        }
    }
    return result;
}

}  // namespace ddlua

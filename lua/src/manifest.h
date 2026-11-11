#pragma once
// <mod>/lua/manifest.json 的解析，以及 Lua Mod 的加载顺序。纯逻辑，离线测试直接覆盖。
//
// manifest.json：
//   {
//     "id": "my_mod",                 必填，字母数字和 _ . -，1..63 字节，依赖、日志、dd.import 都用它
//     "version": "1.2.0",             必填，1 到 3 段十进制数字
//     "api": 1,                       必填，DDLua 的 Lua API 版本，和 kLuaApiVersion 不同时拒绝
//     "dependencies": [ "lib_a", { "id": "lib_b", "version": "2.0" } ]   可选，version 是最低版本
//   }
// 其他字段（name、description、author……）忽略。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

inline constexpr int kLuaApiVersion = 1;

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    static bool Parse(std::string_view text, Version& out);
    std::string ToString() const;
    friend bool operator<(const Version& a, const Version& b)
    {
        if (a.major != b.major) {
            return a.major < b.major;
        }
        if (a.minor != b.minor) {
            return a.minor < b.minor;
        }
        return a.patch < b.patch;
    }
};

struct ManifestDep {
    std::string id;
    Version minVersion;  // 没写时为 0.0.0
};

struct Manifest {
    std::string id;
    Version version;
    std::string versionText;
    std::int64_t api = 0;
    std::vector<ManifestDep> deps;
};

bool ValidModId(std::string_view id);
bool ParseManifest(std::string_view json, Manifest& out, std::string* error);

struct LoadOrder {
    std::vector<std::size_t> order;                             // 可加载的下标
    std::vector<std::pair<std::size_t, std::string>> rejected;  // 下标和原因
};

// 依赖拓扑排序，同层按输入（启用列表）顺序。id 重复的全部拒绝；api 不符、缺依赖、依赖版本不够、
// 依赖被拒绝、循环依赖的拒绝并写明原因。
LoadOrder ResolveLoadOrder(const std::vector<Manifest>& mods, std::int64_t apiVersion = kLuaApiVersion);

}  // namespace ddlua

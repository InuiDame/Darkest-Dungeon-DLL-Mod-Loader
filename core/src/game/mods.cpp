#include "mods.h"

#include <windows.h>

#include <cstring>

namespace ddc::game {

namespace {

// 布局（Steam 版和离线版的 UGC 查找函数 38F0B0 / 383F90 一致，见知识库）
constexpr std::size_t kUgcIdSize = 0xC0;       // UGCId：name char[0x80] + source char[0x40]
constexpr std::size_t kUgcIdSource = 0x80;
constexpr std::size_t kListKeyOffset = 0x200;  // UGCIdList 的键名（+0x08）在 vector（+0x208）之前 0x200 字节
constexpr std::size_t kInstalledVector = 0x20; // UserServicesInterface+0x20：vector<UserGeneratedContent*>
constexpr std::size_t kUgcPath = 0x20;         // UGC 对象的目录 char[0x200]
constexpr std::size_t kUgcId = 0x620;          // UGC 对象的 UGCId
constexpr std::size_t kMaxEntries = 8192;

struct Vec {
    std::uintptr_t first;
    std::uintptr_t last;
    std::uintptr_t end;
};

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    if (src == 0) {
        return false;
    }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadVec(std::uintptr_t address, std::size_t elementSize, Vec& out, std::size_t& count)
{
    if (!ReadBytes(address, &out, sizeof(out))) {
        return false;
    }
    if (out.first == 0 && out.last == 0 && out.end == 0) {
        count = 0;
        return true;
    }
    if (out.first == 0 || out.last < out.first || out.end < out.last || (out.last - out.first) % elementSize != 0) {
        return false;
    }
    count = (out.last - out.first) / elementSize;
    return count <= kMaxEntries;
}

// 定长 char 数组，要求在容量内以 NUL 结尾
bool ReadFixedString(std::uintptr_t address, std::size_t capacity, std::string& out)
{
    char buffer[0x200];
    if (capacity > sizeof(buffer) || !ReadBytes(address, buffer, capacity)) {
        return false;
    }
    const std::size_t length = strnlen(buffer, capacity);
    if (length == capacity) {
        return false;
    }
    out.assign(buffer, length);
    return true;
}

std::wstring Decode(const std::string& text, UINT codepage)
{
    const DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(codepage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(codepage, flags, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// 游戏记录的路径（本地 "mods/<目录>/"，Workshop 是绝对路径，分隔符都是 '/'）转成绝对路径。
// 编码先按 UTF-8，目录不存在再按 ANSI 代码页试一次（游戏的文件 API 可能是窄字符版本）。
bool ResolveDirectory(const std::string& raw, const std::string& gameRoot, std::string& out)
{
    if (raw.empty()) {
        return false;
    }
    const bool absolute = raw.size() >= 2 && raw[1] == ':';
    const std::wstring root = Decode(gameRoot, CP_UTF8);
    for (UINT codepage : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
        std::wstring path = Decode(raw, codepage);
        if (path.empty()) {
            continue;
        }
        for (auto& ch : path) {
            if (ch == L'/') {
                ch = L'\\';
            }
        }
        while (!path.empty() && path.back() == L'\\') {
            path.pop_back();
        }
        if (!absolute) {
            path = root + L"\\" + path;
        }
        if (DirectoryExists(path)) {
            out = ToUtf8(path);
            return true;
        }
    }
    return false;
}

struct Installed {
    std::string name;
    std::string source;
    std::string path;
};

}  // namespace

EnabledModsResult ReadEnabledMods(void* const* userServices, const void* appliedVector, const std::string& gameRoot)
{
    EnabledModsResult result;
    if (!userServices || !appliedVector) {
        result.error = "特征码 UserServices / AppliedUgcVector 缺失";
        return result;
    }
    const auto applied = reinterpret_cast<std::uintptr_t>(appliedVector);

    // 键名校验：UGCIdList 构造时写入 "applied_ugcs_1_0"（38EA40），复制（415EA0）时一并复制
    std::string key;
    if (!ReadFixedString(applied - kListKeyOffset, 0x200, key) || key != "applied_ugcs_1_0") {
        result.error = "启用列表的键名不是 applied_ugcs_1_0（读到 '" + key + "'），结构可能已变化";
        return result;
    }

    Vec appliedVec{};
    std::size_t appliedCount = 0;
    if (!ReadVec(applied, kUgcIdSize, appliedVec, appliedCount)) {
        result.error = "启用列表的 vector 不合法";
        return result;
    }

    std::uintptr_t services = 0;
    if (!ReadBytes(reinterpret_cast<std::uintptr_t>(userServices), &services, sizeof(services)) || services == 0) {
        result.error = "UserServicesInterface 为空";
        return result;
    }
    Vec installedVec{};
    std::size_t installedCount = 0;
    if (!ReadVec(services + kInstalledVector, sizeof(void*), installedVec, installedCount)) {
        result.error = "已安装 Mod 的 vector 不合法";
        return result;
    }
    std::vector<Installed> installed;
    installed.reserve(installedCount);
    for (std::size_t i = 0; i < installedCount; ++i) {
        std::uintptr_t ugc = 0;
        Installed item;
        if (!ReadBytes(installedVec.first + i * sizeof(void*), &ugc, sizeof(ugc)) || ugc == 0 ||
            !ReadFixedString(ugc + kUgcId, 0x80, item.name) || !ReadFixedString(ugc + kUgcId + kUgcIdSource, 0x40, item.source)) {
            result.error = "已安装 Mod 第 " + std::to_string(i) + " 项读取失败";
            return result;
        }
        ReadFixedString(ugc + kUgcPath, 0x200, item.path);  // 目录读不出来时按没找到处理
        installed.push_back(std::move(item));
    }
    result.installedCount = installed.size();

    for (std::size_t i = 0; i < appliedCount; ++i) {
        EnabledMod mod;
        const std::uintptr_t entry = appliedVec.first + i * kUgcIdSize;
        if (!ReadFixedString(entry, 0x80, mod.name) || !ReadFixedString(entry + kUgcIdSource, 0x40, mod.source)) {
            result.error = "启用列表第 " + std::to_string(i) + " 项读取失败";
            return result;
        }
        // 和游戏自己的查找（38F0B0）相同：name 和 source 都相等，取第一个
        for (const auto& item : installed) {
            if (item.name == mod.name && item.source == mod.source) {
                mod.rawPath = item.path;
                mod.found = ResolveDirectory(item.path, gameRoot, mod.dir);
                break;
            }
        }
        result.mods.push_back(std::move(mod));
    }
    result.ok = true;
    return result;
}

}  // namespace ddc::game

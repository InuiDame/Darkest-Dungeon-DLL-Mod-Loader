#include "discovery.h"

#include "../../../common/pe_exports.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "shlwapi.lib")

namespace ddc {

namespace {

const wchar_t* const kForceFolder = L"DD Mod Lib";

std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), ::towlower);
    return text;
}

std::wstring NormalizePath(std::wstring path)
{
    path = Lower(std::move(path));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

std::unordered_map<std::wstring, bool> g_config;

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 同 ScanAndLoadDLLs：加载器的强制目录优先，其余目录的同名 DLL 不再加载
struct Scanner {
    DiscoveryResult& result;
    std::unordered_set<std::wstring> forcedNames;

    void Scan(const std::wstring& directory, bool force, const wchar_t* skipSubDir)
    {
        if (!DirectoryExists(directory)) {
            return;
        }
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((directory + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring fullPath = directory + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (skipSubDir && _wcsicmp(fd.cFileName, skipSubDir) == 0) {
                    continue;
                }
                Scan(fullPath, force, nullptr);
                continue;
            }
            if (!PathMatchSpecW(fd.cFileName, L"*.dll")) {
                continue;
            }
            const std::wstring lowerName = Lower(fd.cFileName);
            if (IsExcludedDllName(lowerName)) {
                continue;
            }
            if (!force) {
                if (forcedNames.count(lowerName)) {
                    result.notes.push_back(L"跳过（前置目录已有同名 DLL）-> " + fullPath);
                    continue;
                }
                bool isNew = false;
                if (!DllEnabled(fullPath, isNew)) {
                    result.notes.push_back(L"跳过（配置已禁用）-> " + fullPath);
                    continue;
                }
            }
            if (lowerName != L"ddcore.dll" && dd::common::DllIsNewProtocol(fullPath.c_str())) {
                result.candidates.push_back({ fullPath, fd.cFileName, force });
                result.notes.push_back(L"发现新协议插件 -> " + fullPath);
            }
            if (force) {
                forcedNames.insert(lowerName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
};

std::vector<std::wstring> ReadExtraDirs(const std::wstring& gameRoot)
{
    std::vector<std::wstring> dirs;
    const std::wstring path = gameRoot + L"\\DDModLoader\\cfg\\extra_mods.cfg";
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8");
    if (!file) {
        return dirs;
    }
    wchar_t line[4096];
    while (fgetws(line, 4096, file)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) {
            s.pop_back();
        }
        size_t begin = 0;
        while (begin < s.size() && (s[begin] == L' ' || s[begin] == L'\t')) {
            ++begin;
        }
        s = s.substr(begin);
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) {
            s.pop_back();
        }
        if (!s.empty()) {
            dirs.push_back(s);
        }
    }
    fclose(file);
    return dirs;
}

}  // namespace

bool IsExcludedDllName(const std::wstring& lowerName)
{
    static const std::unordered_set<std::wstring> kExcluded = {
        L"dbghelp.dll", L"fmod.dll", L"fmod64.dll", L"fmodstudio.dll", L"fmodstudio64.dll", L"sdl2.dll",
        L"glew32.dll", L"glew64.dll", L"steam_api.dll", L"steam_api64.dll", L"vcruntime140.dll",
        L"vcruntime140_1.dll", L"msvcp140.dll",
    };
    return kExcluded.count(lowerName) > 0;
}

void LoadDllConfig(const std::wstring& path)
{
    g_config.clear();
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8");
    if (!file) {
        return;
    }
    wchar_t line[4096];
    while (fgetws(line, 4096, file)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) {
            s.pop_back();
        }
        if (s.size() < 3 || s[1] != L'|') {
            continue;
        }
        g_config[NormalizePath(s.substr(2))] = (s[0] == L'1');
    }
    fclose(file);
}

bool DllEnabled(const std::wstring& fullPath, bool& isNew)
{
    const auto it = g_config.find(NormalizePath(fullPath));
    if (it != g_config.end()) {
        isNew = false;
        return it->second;
    }
    isNew = true;
    return true;
}

DiscoveryResult DiscoverPlugins(const std::wstring& gameRoot)
{
    DiscoveryResult result;
    Scanner scanner{ result };

    LoadDllConfig(gameRoot + L"\\DDModLoader\\cfg\\DDModLoader.cfg");

    const std::wstring mods = gameRoot + L"\\mods";
    const std::wstring forced = mods + L"\\" + kForceFolder;
    if (DirectoryExists(forced)) {
        scanner.Scan(forced, true, nullptr);
    }
    scanner.Scan(mods, false, kForceFolder);

    std::wstring steamApps = gameRoot;
    for (int i = 0; i < 2; ++i) {
        const auto pos = steamApps.find_last_of(L"\\/");
        if (pos == std::wstring::npos) {
            break;
        }
        steamApps.resize(pos);
    }
    const std::wstring workshop = steamApps + L"\\workshop\\content\\262060";
    if (DirectoryExists(workshop)) {
        scanner.Scan(workshop, false, nullptr);
    }

    for (const auto& dir : ReadExtraDirs(gameRoot)) {
        scanner.Scan(dir, false, nullptr);
    }
    return result;
}

}  // namespace ddc

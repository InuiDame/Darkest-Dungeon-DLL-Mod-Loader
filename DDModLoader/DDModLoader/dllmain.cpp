#include "pch.h"
#include <windows.h>
#include <shlwapi.h>
#include <unordered_set>   
#include <algorithm>       
#define MiniDumpWriteDump MiniDumpWriteDump_Original
#include <dbghelp.h>
#undef MiniDumpWriteDump
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// ===================== 日志 =====================
static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logCs;

// 优先写到 <游戏根目录>\DDModLoader\log，游戏目录不可写时退回用户目录和系统临时目录
static void LogInit(const std::wstring& logPath) {
    InitializeCriticalSection(&g_logCs);

    _wfopen_s(&g_logFile, logPath.c_str(), L"w, ccs=UTF-8");

    if (!g_logFile) {
        wchar_t localAppData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
            std::wstring dir = std::wstring(localAppData) + L"\\DDModLoader";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring path = dir + L"\\DDModLoader.log";
            _wfopen_s(&g_logFile, path.c_str(), L"w, ccs=UTF-8");
        }
    }

    if (!g_logFile) {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        std::wstring path = std::wstring(temp) + L"DDModLoader.log";
        _wfopen_s(&g_logFile, path.c_str(), L"w, ccs=UTF-8");
    }
}

static void Log(const std::wstring& msg) {
    EnterCriticalSection(&g_logCs);
    OutputDebugStringW((L"[DDModLoader] " + msg + L"\n").c_str());
    if (g_logFile) {
        fwprintf(g_logFile, L"[DDModLoader] %s\n", msg.c_str());
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logCs);
}

static void LogClose() {
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    DeleteCriticalSection(&g_logCs);
}

// ===================== MiniDumpWriteDump 转发 =====================
typedef BOOL(WINAPI* MiniDumpWriteDump_t)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

static HMODULE g_hSysDbghelp = nullptr;
static MiniDumpWriteDump_t g_pfnMiniDumpWriteDump = nullptr;

static bool InitMiniDumpForwarder() {
    if (g_pfnMiniDumpWriteDump) return true;
    wchar_t sysDir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) return false;
    std::wstring dllPath = std::wstring(sysDir) + L"\\dbghelp.dll";
    g_hSysDbghelp = LoadLibraryW(dllPath.c_str());
    if (!g_hSysDbghelp) return false;
    g_pfnMiniDumpWriteDump = (MiniDumpWriteDump_t)
        GetProcAddress(g_hSysDbghelp, "MiniDumpWriteDump");
    return g_pfnMiniDumpWriteDump != nullptr;
}

extern "C" __declspec(dllexport) BOOL WINAPI MiniDumpWriteDump(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam)
{
    if (!InitMiniDumpForwarder()) return FALSE;
    return g_pfnMiniDumpWriteDump(hProcess, ProcessId, hFile, DumpType,
        ExceptionParam, UserStreamParam, CallbackParam);
}

// ===================== 路径工具 =====================
static std::wstring GetModuleDirectory(HMODULE hModule) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(hModule, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

static std::wstring GetGameRootFromBinDir(const std::wstring& binDir) {
    std::wstring root = binDir;
    PathRemoveFileSpecW(root.data());
    root.resize(wcslen(root.c_str()));
    PathRemoveFileSpecW(root.data());
    root.resize(wcslen(root.c_str()));
    return root;
}

// 配置和日志统一放在 <游戏根目录>\DDModLoader 的 cfg、log 子目录，与 GUI 保持一致
static std::wstring GetDataDir(const std::wstring& gameRoot, const wchar_t* subDir) {
    std::wstring dir = gameRoot + L"\\DDModLoader";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    dir += subDir;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static bool DirectoryExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ===================== MOD 扫描加载 =====================
static const std::wstring kForceLoadFolderName = L"DD Mod Lib";

static int g_loadedCount = 0;
static int g_failedCount = 0;

// 前置目录中已成功加载的 DLL 文件名（小写）。其他目录的同名 DLL 不再加载，
// 兼容仍在使用独立版的创意工坊玩家，避免同一个插件被按不同路径加载两次
static std::unordered_set<std::wstring> g_forceLoadedNames;

static std::wstring ToLowerName(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

static const std::unordered_set<std::wstring> kExcludedDlls = {
    L"dbghelp.dll",
    L"fmod.dll",
    L"fmod64.dll",
    L"fmodstudio.dll",
    L"fmodstudio64.dll",
    L"sdl2.dll",
    L"glew32.dll",
    L"glew64.dll",
    L"steam_api.dll",
    L"steam_api64.dll",
    L"vcruntime140.dll",
    L"vcruntime140_1.dll",
    L"msvcp140.dll"
};

static bool IsExcludedDll(const std::wstring& fileName) {
    std::wstring lower = fileName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return kExcludedDlls.count(lower) > 0;
}

// ---------- 配置文件 ----------
static std::unordered_map<std::wstring, bool> g_dllConfig;
static std::wstring g_configPath;

static std::wstring NormalizePath(const std::wstring& path) {
    std::wstring res = path;
    std::transform(res.begin(), res.end(), res.begin(), ::towlower);
    std::replace(res.begin(), res.end(), L'/', L'\\');
    return res;
}

static void LoadConfig(const std::wstring& path) {
    g_configPath = path;
    g_dllConfig.clear();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8");
    if (!f) {
        Log(L"未找到配置文件 " + path + L" ，所有 DLL 将默认加载。");
        return;
    }

    int count = 0;
    wchar_t line[4096];
    while (fgetws(line, 4096, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (s.size() < 3 || s[1] != L'|') continue;

        std::wstring rawPath = s.substr(2);
        g_dllConfig[NormalizePath(rawPath)] = (s[0] == L'1');
        count++;
    }
    fclose(f);
    Log(L"成功加载配置文件，读取到 " + std::to_wstring(count) + L" 条规则。");
}

static bool ShouldLoadDll(const std::wstring& fullPath, bool& isNew) {
    auto it = g_dllConfig.find(NormalizePath(fullPath));
    if (it != g_dllConfig.end()) {
        isNew = false;
        return it->second;
    }
    isNew = true;
    return true;
}

// ===================== 扫描加载（修改）=====================
// forceLoad=true 时忽略配置文件，强制加载
// skipDirectSubDir 非空时，跳过当前目录下直接子文件夹（不递归到该子文件夹），用于跳过 mods\DD Mod Lib
static void ScanAndLoadDLLs(const std::wstring& directory,
    bool forceLoad = false,
    const std::wstring& skipDirectSubDir = L"") {
    if (!DirectoryExists(directory)) {
        Log(L"目录不存在，跳过: " + directory);
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((directory + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        Log(L"无法枚举目录: " + directory);
        return;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring fullPath = directory + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skipDirectSubDir.empty() &&
                _wcsicmp(fd.cFileName, skipDirectSubDir.c_str()) == 0) {
                Log(L"跳过前置 MOD 目录（已优先加载）: " + fullPath);
                continue;
            }
            ScanAndLoadDLLs(fullPath, forceLoad, L"");
        }
        else {
            if (!PathMatchSpecW(fd.cFileName, L"*.dll")) continue;
            if (IsExcludedDll(fd.cFileName)) continue;

            if (!forceLoad) {
                if (g_forceLoadedNames.count(ToLowerName(fd.cFileName))) {
                    Log(L"跳过（前置目录已加载同名 DLL）-> " + fullPath);
                    continue;
                }

                bool isNew = false;
                if (!ShouldLoadDll(fullPath, isNew)) {
                    Log(L"跳过（配置已禁用）-> " + fullPath);
                    continue;
                }
                if (isNew) {
                    Log(L"新 DLL（默认启用，未在 cfg 中）-> " + fullPath);
                }
            }
            else {
                Log(L"强制加载前置 DLL -> " + fullPath);
            }

            std::wstring oldDir;
            DWORD oldDirLen = GetDllDirectoryW(0, nullptr);
            if (oldDirLen > 0) {
                oldDir.resize(oldDirLen);
                GetDllDirectoryW(oldDirLen, &oldDir[0]);
            }

            SetDllDirectoryW(directory.c_str());
            HMODULE hMod = LoadLibraryW(fullPath.c_str());
            DWORD err = GetLastError();

            if (oldDir.empty()) SetDllDirectoryW(nullptr);
            else SetDllDirectoryW(oldDir.c_str());

            if (hMod) {
                g_loadedCount++;
                if (forceLoad) g_forceLoadedNames.insert(ToLowerName(fd.cFileName));
                Log(L"加载成功 -> " + fullPath);
            }
            else {
                g_failedCount++;
                Log(L"加载失败 -> " + fullPath + L"  (错误码 " + std::to_wstring(err) + L")");
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}


// ===================== 额外 MOD 目录 =====================
// 与 GUI 共用：<游戏根目录>\DDModLoader\cfg\extra_mods.cfg，每行一个绝对路径。
static std::vector<std::wstring> LoadExtraDirs(const std::wstring& gameRoot) {
    std::vector<std::wstring> dirs;
    std::wstring p = GetDataDir(gameRoot, L"cfg") + L"\\extra_mods.cfg";
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8");
    if (!f) return dirs;

    wchar_t line[4096];
    while (fgetws(line, 4096, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();

        size_t b = 0;
        while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) ++b;
        s = s.substr(b);
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();

        if (!s.empty()) dirs.push_back(s);
    }
    fclose(f);
    return dirs;
}

// ===================== MOD 加载线程（修改）=====================
static DWORD WINAPI ModLoaderThread(LPVOID lpParam) {
    HMODULE hSelf = (HMODULE)lpParam;
    Sleep(1500);

    std::wstring binDir = GetModuleDirectory(hSelf);
    std::wstring gameRoot = GetGameRootFromBinDir(binDir);

    Log(L"代理 DLL 目录: " + binDir);
    Log(L"游戏根目录: " + gameRoot);

    std::wstring localMods = gameRoot + L"\\mods";
    std::wstring forceDir = localMods + L"\\" + kForceLoadFolderName;

    // 1. 优先加载前置 MOD（DD Mod Lib），强制启用
    if (DirectoryExists(forceDir)) {
        Log(L"[优先] 加载前置 MOD 目录: " + forceDir);
        ScanAndLoadDLLs(forceDir, true);
    }
    else {
        Log(L"未找到前置 MOD 目录: " + forceDir);
    }

    // 2. 再加载本地 mods 下其余 DLL（跳过 DD Mod Lib）
    Log(L"开始扫描本地 MOD: " + localMods);
    ScanAndLoadDLLs(localMods, false, kForceLoadFolderName);

    std::wstring steamApps = gameRoot;
    PathRemoveFileSpecW(steamApps.data());
    steamApps.resize(wcslen(steamApps.c_str()));
    PathRemoveFileSpecW(steamApps.data());
    steamApps.resize(wcslen(steamApps.c_str()));
    std::wstring workshop = steamApps + L"\\workshop\\content\\262060";
    if (DirectoryExists(workshop)) {
        Log(L"开始扫描创意工坊 MOD: " + workshop);
        ScanAndLoadDLLs(workshop);
    }
    else {
        Log(L"创意工坊目录不存在，跳过: " + workshop);
    }

    auto extraDirs = LoadExtraDirs(gameRoot);
    if (extraDirs.empty()) {
        Log(L"未配置额外 MOD 目录。");
    }
    for (auto& d : extraDirs) {
        Log(L"开始扫描额外 MOD 目录: " + d);
        ScanAndLoadDLLs(d);
    }

    Log(L"MOD 扫描完成。成功: " + std::to_wstring(g_loadedCount) +
        L"，失败: " + std::to_wstring(g_failedCount));
    return 0;
}

// ===================== DllMain（修改改名调用）=====================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        std::wstring binDir = GetModuleDirectory(hModule);
        std::wstring gameRoot = GetGameRootFromBinDir(binDir);
        LogInit(GetDataDir(gameRoot, L"log") + L"\\DDModLoader.log");
        LoadConfig(GetDataDir(gameRoot, L"cfg") + L"\\DDModLoader.cfg");
        Log(L"========== DDModLoader 启动 ==========");

        if (!InitMiniDumpForwarder()) {
            Log(L"警告：无法加载系统 dbghelp.dll，MiniDumpWriteDump 将不可用");
        }

        HANDLE hThread = CreateThread(nullptr, 0, ModLoaderThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Log(L"========== DDModLoader 退出 ==========");
        LogClose();
    }
    return TRUE;
}
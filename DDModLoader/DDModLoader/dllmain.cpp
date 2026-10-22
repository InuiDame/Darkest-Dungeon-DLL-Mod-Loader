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
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// ===================== 日志 =====================
static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logCs;

static void LogInit(const std::wstring& fallbackPath) {
    InitializeCriticalSection(&g_logCs);

    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring dir = std::wstring(localAppData) + L"\\DDModLoader";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\DDModLoader.log";
        _wfopen_s(&g_logFile, path.c_str(), L"w, ccs=UTF-8");
    }

    if (!g_logFile) {
        // 回退到游戏根目录
        _wfopen_s(&g_logFile, fallbackPath.c_str(), L"w, ccs=UTF-8");
    }

    if (!g_logFile) {
        // 再回退到临时目录
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

static std::wstring GetGameRootFromWin64(const std::wstring& win64Dir) {
    std::wstring root = win64Dir;
    PathRemoveFileSpecW(root.data());
    root.resize(wcslen(root.c_str()));   // 同步 size
    PathRemoveFileSpecW(root.data());
    root.resize(wcslen(root.c_str()));   // 同步 size
    return root;
}

static bool DirectoryExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ===================== MOD 扫描加载 =====================
static int g_loadedCount = 0;
static int g_failedCount = 0;

// 新增：排除名单与判断函数
static const std::unordered_set<std::wstring> kExcludedDlls = {
    L"dbghelp.dll",
    L"fmod.dll",
    L"fmodstudio.dll",
    L"sdl2.dll",
    L"glew32.dll",
    L"steam_api.dll",
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

// 统一路径格式：转小写，把 / 换成 \ 这段真气笑了,读cfg还能读出问题来.

static std::wstring NormalizePath(const std::wstring& path) {
std::wstring res = path;
std::transform(res.begin(), res.end(), res.begin(), ::towlower);
std::replace(res.begin(), res.end(), L'/', L'\\');
return res;
}

// 修改 LoadConfig，存储规范化后的 Key
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

        // 路径规范化
        std::wstring rawPath = s.substr(2);
        g_dllConfig[NormalizePath(rawPath)] = (s[0] == L'1');
        count++;
    }
    fclose(f);
    Log(L"成功加载配置文件，读取到 " + std::to_wstring(count) + L" 条规则。");
}

static bool ShouldLoadDll(const std::wstring& fullPath, bool& isNew) {
    // 查询时把当前扫描到的路径规范化
    auto it = g_dllConfig.find(NormalizePath(fullPath));
    if (it != g_dllConfig.end()) {
        isNew = false;
        return it->second;
    }
    isNew = true;
    return true;
}

static void ScanAndLoadDLLs(const std::wstring& directory) {
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
            ScanAndLoadDLLs(fullPath);
        }
        else {
            if (!PathMatchSpecW(fd.cFileName, L"*.dll")) continue;
            if (IsExcludedDll(fd.cFileName)) continue; 

            std::wstring oldDir;
            DWORD oldDirLen = GetDllDirectoryW(0, nullptr);
            if (oldDirLen > 0) {
                oldDir.resize(oldDirLen);
                GetDllDirectoryW(oldDirLen, &oldDir[0]);
            }
            bool isNew = false;
            if (!ShouldLoadDll(fullPath, isNew)) {
                Log(L"跳过（配置已禁用）-> " + fullPath);
                continue;
            }
            if (isNew) {
                Log(L"新 DLL（默认启用，未在 cfg 中）-> " + fullPath);
            }

            SetDllDirectoryW(directory.c_str());

            HMODULE hMod = LoadLibraryW(fullPath.c_str());
            DWORD err = GetLastError();

            if (oldDir.empty()) SetDllDirectoryW(nullptr);
            else SetDllDirectoryW(oldDir.c_str());

            if (hMod) {
                g_loadedCount++;
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

// ===================== MOD 加载线程 =====================
static DWORD WINAPI ModLoaderThread(LPVOID lpParam) {
    HMODULE hSelf = (HMODULE)lpParam;
    Sleep(1500);

    std::wstring win64Dir = GetModuleDirectory(hSelf);
    std::wstring gameRoot = GetGameRootFromWin64(win64Dir);

    Log(L"代理 DLL 目录: " + win64Dir);
    Log(L"游戏根目录: " + gameRoot);

    std::wstring localMods = gameRoot + L"\\mods";
    Log(L"开始扫描本地 MOD: " + localMods);
    ScanAndLoadDLLs(localMods);

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

    Log(L"MOD 扫描完成。成功: " + std::to_wstring(g_loadedCount) +
        L"，失败: " + std::to_wstring(g_failedCount));
    return 0;
}

// ===================== DllMain =====================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        std::wstring win64Dir = GetModuleDirectory(hModule);
        std::wstring gameRoot = GetGameRootFromWin64(win64Dir);
        std::wstring logPath = gameRoot + L"\\DDModLoader.log";
        LogInit(logPath);
        LoadConfig(gameRoot + L"\\DDModLoader.cfg");
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

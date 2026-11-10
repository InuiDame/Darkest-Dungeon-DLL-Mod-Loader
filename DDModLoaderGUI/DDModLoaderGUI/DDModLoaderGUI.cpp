#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "framework.h"
#include "DDModLoaderGUI.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cwctype>
#include <cstring>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------- 本地化字符串 ----------
struct LocStrings {
    const wchar_t* appTitle;
    const wchar_t* colModName;
    const wchar_t* colDllName;
    const wchar_t* colFullPath;
    const wchar_t* btnRefresh;
    const wchar_t* btnEnableAll;
    const wchar_t* btnDisableAll;
    const wchar_t* btnGroupView;
    const wchar_t* btnFlatView;
    const wchar_t* btnChooseRoot;
    const wchar_t* btnLaunchSteam;
    const wchar_t* btnLaunchOffline;
    const wchar_t* menuLaunchOffline64;
    const wchar_t* menuLaunchOffline32;
    const wchar_t* btnSave;
    const wchar_t* msgSaved;
    const wchar_t* msgInfoTitle;
    const wchar_t* msgErrorTitle;
    const wchar_t* msgSteamFail;
    const wchar_t* msgOfflineNotFoundFmt;
    const wchar_t* msgOfflineFail;
    const wchar_t* msgNoGameRoot;
    const wchar_t* msgPickRootTitle;
    const wchar_t* msgInvalidRootFmt;
    const wchar_t* unknownMod;
    const wchar_t* menuEnableDll;
    const wchar_t* menuDisableDll;
    const wchar_t* menuEnableMod;
    const wchar_t* menuDisableMod;
    // ---- 新增：额外 MOD 目录 ----
    const wchar_t* btnAddExtra;
    const wchar_t* btnRemoveExtra;
    const wchar_t* msgPickExtraTitle;
    const wchar_t* msgExtraInvalidFmt;
    const wchar_t* msgExtraDup;
    const wchar_t* msgNoExtraDirs;
};

static const LocStrings kZh = {
    L"DDModLoader 配置工具",
    L"MOD 名称",
    L"DLL 名称",
    L"完整路径",
    L"刷新",
    L"全启用",
    L"全禁用",
    L"按 MOD 收纳",
    L"平铺列表",
    L"选择游戏目录",
    L"从 Steam 启动",
    L"启动离线版",
    L"启动 64 位版本 (win64)",
    L"启动 32 位版本 (win32)",
    L"保存",
    L"配置已保存到 DDModLoader.cfg。",
    L"提示",
    L"错误",
    L"无法通过 Steam 启动游戏。\n请确认 Steam 已安装并处于登录状态。",
    L"未找到离线版可执行文件：\n%s\n\n如果你只安装了 Steam 版，请使用「从 Steam 启动」。",
    L"启动离线版游戏失败。",
    L"未能自动找到《暗黑地牢》的游戏根目录。\n\n"
    L"请点击「选择游戏目录」手动指定游戏根（含 _windows 或 _windowsnosteam 的目录），"
    L"或将本程序放到游戏根目录附近。",
    L"选择《暗黑地牢》游戏根目录",
    L"所选目录不是有效的《暗黑地牢》游戏根目录：\n%s\n\n"
    L"目录中应包含 _windows\\win64\\Darkest.exe 或 _windowsnosteam\\win64\\Darkest.exe。",
    L"(未知)",
    L"启用此 DLL",
    L"禁用此 DLL",
    L"启用此 MOD 全部",
    L"禁用此 MOD 全部",
    // 新增
    L"添加 MOD 目录",
    L"移除 MOD 目录",
    L"选择额外 MOD 目录（例如 MO2 的 mods 目录）",
    L"所选目录无效或不存在：\n%s",
    L"该目录已在列表中。",
    L"当前没有额外的 MOD 目录。",
};

static const LocStrings kEn = {
    L"DDModLoader Config Tool",
    L"Mod Name",
    L"DLL Name",
    L"Full Path",
    L"Refresh",
    L"Enable All",
    L"Disable All",
    L"Group by Mod",
    L"Flat List",
    L"Choose Game Folder",
    L"Launch via Steam",
    L"Launch Offline",
    L"Launch 64-bit version (win64)",
    L"Launch 32-bit version (win32)",
    L"Save",
    L"Configuration saved to DDModLoader.cfg.",
    L"Info",
    L"Error",
    L"Failed to launch the game via Steam.\nPlease make sure Steam is installed and signed in.",
    L"Offline executable not found:\n%s\n\nIf you only have the Steam version, please use \"Launch via Steam\".",
    L"Failed to launch the offline game.",
    L"Unable to locate the Darkest Dungeon game root folder.\n\n"
    L"Please click \"Choose Game Folder\" to pick the game root manually "
    L"(the folder containing _windows or _windowsnosteam), "
    L"or place this program near the game root folder.",
    L"Choose Darkest Dungeon game root folder",
    L"The selected folder is not a valid Darkest Dungeon game root:\n%s\n\n"
    L"It should contain _windows\\win64\\Darkest.exe or _windowsnosteam\\win64\\Darkest.exe.",
    L"(Unknown)",
    L"Enable this DLL",
    L"Disable this DLL",
    L"Enable all in this MOD",
    L"Disable all in this MOD",
    // 新增
    L"Add Mod Folder",
    L"Remove Mod Folder",
    L"Choose an extra mod folder (e.g. MO2's mods folder)",
    L"The selected folder is invalid or does not exist:\n%s",
    L"This folder is already in the list.",
    L"There are no extra mod folders configured.",
};

static const LocStrings* g_loc = nullptr;

// ---------- 与加载器保持一致的排除名单 ----------
static const std::wstring kForceLoadFolderName = L"DD Mod Lib";

static const std::vector<std::wstring> kExcludedDlls = {
    L"dbghelp.dll", L"fmod.dll", L"fmod64.dll",
    L"fmodstudio.dll", L"fmodstudio64.dll", L"sdl2.dll",
    L"glew32.dll", L"glew64.dll", L"steam_api.dll", L"steam_api64.dll",
    L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll"
};

static bool IsExcluded(const std::wstring& name) {
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    for (auto& e : kExcludedDlls) if (e == lower) return true;
    return false;
}

// ---------- 数据结构 ----------
struct DllEntry {
    std::wstring modName;
    std::wstring dllName;
    std::wstring fullPath;
};

static std::wstring g_gameRoot;

// ---------- 基础工具 ----------
static bool DirectoryExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring TrimTrailingSlash(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

static std::wstring NormalizePath(const std::wstring& path) {
    std::wstring res = path;
    std::transform(res.begin(), res.end(), res.begin(), ::towlower);
    std::replace(res.begin(), res.end(), L'/', L'\\');
    return res;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------- 从 project.xml / project.json 提取 Title ----------
static std::wstring ParseTitle(const std::string& utf8) {
    size_t pos = utf8.find("Title");
    if (pos == std::string::npos) return L"";
    pos += 5;
    while (pos < utf8.size() && (utf8[pos] == ' ' || utf8[pos] == '\t')) ++pos;
    if (pos < utf8.size() && (utf8[pos] == '>' || utf8[pos] == ':')) ++pos;
    while (pos < utf8.size() && (utf8[pos] == ' ' || utf8[pos] == '\t')) ++pos;
    if (pos < utf8.size() && utf8[pos] == '"') ++pos;
    size_t start = pos;
    while (pos < utf8.size() && utf8[pos] != '<' && utf8[pos] != '"' &&
        utf8[pos] != '\r' && utf8[pos] != '\n')
        ++pos;
    if (pos == start) return L"";
    std::string title = utf8.substr(start, pos - start);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), (int)title.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), (int)title.size(), &w[0], wlen);
    return w;
}

static std::wstring ReadTitleFromDir(const std::wstring& dir) {
    for (auto fn : { L"project.xml", L"project.json" }) {
        std::wstring p = dir + L"\\" + fn;
        FILE* f = nullptr;
        _wfopen_s(&f, p.c_str(), L"rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buf;
        if (sz > 0 && sz < 2 * 1024 * 1024) {
            buf.resize(sz);
            fread(&buf[0], 1, sz, f);
        }
        fclose(f);
        if (!buf.empty()) {
            std::wstring t = ParseTitle(buf);
            if (!t.empty()) return t;
        }
    }
    return L"";
}

// ---------- 配置 ----------
static std::unordered_map<std::wstring, bool> LoadConfig(const std::wstring& path) {
    std::unordered_map<std::wstring, bool> m;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8");
    if (!f) return m;
    wchar_t line[4096];
    while (fgetws(line, 4096, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (s.size() < 3 || s[1] != L'|') continue;
        m[NormalizePath(s.substr(2))] = (s[0] == L'1');
    }
    fclose(f);
    return m;
}

static void SaveConfig(const std::wstring& path,
    const std::vector<std::pair<std::wstring, bool>>& items) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    for (auto& [p, e] : items) fwprintf(f, L"%d|%s\n", e ? 1 : 0, p.c_str());
    fclose(f);
}

// 配置统一放在 <游戏根目录>\DDModLoader\cfg，与加载器保持一致
static std::wstring GetCfgFilePath(const wchar_t* fileName) {
    std::wstring d = g_gameRoot + L"\\DDModLoader";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\cfg";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\" + fileName;
}

// ---------- 额外 MOD 目录（与加载器共用） ----------
// 存放于 <游戏根目录>\DDModLoader\cfg\extra_mods.cfg，每行一个绝对路径。
static std::wstring GetExtraModsCfgPath() {
    return GetCfgFilePath(L"extra_mods.cfg");
}

static std::vector<std::wstring> LoadExtraDirs() {
    std::vector<std::wstring> dirs;
    std::wstring p = GetExtraModsCfgPath();
    if (p.empty()) return dirs;
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8");
    if (!f) return dirs;
    wchar_t line[4096];
    while (fgetws(line, 4096, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) ++b;
        s = TrimTrailingSlash(s.substr(b));
        if (!s.empty()) dirs.push_back(s);
    }
    fclose(f);
    return dirs;
}

static void SaveExtraDirs(const std::vector<std::wstring>& dirs) {
    std::wstring p = GetExtraModsCfgPath();
    if (p.empty()) return;
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    for (auto& d : dirs) fwprintf(f, L"%s\n", d.c_str());
    fclose(f);
}

// ---------- 扫描 ----------
static void ScanDllsRec(const std::wstring& dir, const std::wstring& modName,
    std::vector<DllEntry>& out) {
    if (!DirectoryExists(dir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDllsRec(full, modName, out);
        }
        else if (PathMatchSpecW(fd.cFileName, L"*.dll") && !IsExcluded(fd.cFileName)) {
            DllEntry e;
            e.dllName = fd.cFileName;
            e.fullPath = full;
            e.modName = modName;
            out.push_back(e);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ScanModsFolder(const std::wstring& modsDir, std::vector<DllEntry>& out,
    bool skipForceLoad = false) {
    if (!DirectoryExists(modsDir)) return;

    std::wstring rootName = PathFindFileNameW(modsDir.c_str());
    if (rootName.empty()) rootName = L"mods";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((modsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = modsDir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (skipForceLoad &&
                _wcsicmp(fd.cFileName, kForceLoadFolderName.c_str()) == 0)
                continue;
            ScanDllsRec(full, fd.cFileName, out);
        }
        else if (PathMatchSpecW(fd.cFileName, L"*.dll") && !IsExcluded(fd.cFileName)) {
            DllEntry e;
            e.dllName = fd.cFileName;
            e.fullPath = full;
            e.modName = rootName;
            out.push_back(e);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ScanWorkshopFolder(const std::wstring& wsDir, std::vector<DllEntry>& out) {
    if (!DirectoryExists(wsDir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((wsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring itemDir = wsDir + L"\\" + fd.cFileName;
        std::wstring modName = ReadTitleFromDir(itemDir);
        if (modName.empty()) modName = fd.cFileName;
        ScanDllsRec(itemDir, modName, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------- 控件 ID / 菜单 ID ----------
#define IDC_LIST            1001
#define IDC_REFRESH         1002
#define IDC_SAVE            1003
#define IDC_ALL             1004
#define IDC_NONE            1005
#define IDC_CHOOSE_ROOT     1006
#define IDC_LAUNCH_STEAM    1007
#define IDC_LAUNCH_OFFLINE  1008
#define IDC_TOGGLE_GROUP    1009
#define IDC_ADD_EXTRA       1010
#define IDC_REMOVE_EXTRA    1011

#define IDM_ENABLE_DLL      2001
#define IDM_DISABLE_DLL     2002
#define IDM_ENABLE_MOD      2003
#define IDM_DISABLE_MOD     2004

static std::wstring g_configPath;
static std::vector<DllEntry> g_entries;
static std::vector<std::wstring> g_extraDirs;   // 额外 MOD 目录
static HWND g_hList = nullptr;
static HWND g_hWnd = nullptr;
static bool g_groupView = true;
static bool g_suppressGroupUpdate = false;

// ---------- 游戏根验证 ----------
static bool IsValidGameRoot(const std::wstring& p0) {
    std::wstring p = TrimTrailingSlash(p0);
    if (p.empty() || !DirectoryExists(p)) return false;
    static const wchar_t* kExeRels[] = {
        L"\\_windows\\win64\\Darkest.exe",
        L"\\_windows\\win32\\Darkest.exe",
        L"\\_windowsnosteam\\win64\\Darkest.exe",
        L"\\_windowsnosteam\\win32\\Darkest.exe",
    };
    for (auto rel : kExeRels) {
        if (FileExists(p + rel)) return true;
    }
    return false;
}

static std::wstring DeriveSteamApps(const std::wstring& gameRoot) {
    std::wstring p = TrimTrailingSlash(gameRoot);
    for (int i = 0; i < 8 && !p.empty(); ++i) {
        const wchar_t* name = PathFindFileNameW(p.c_str());
        if (name && _wcsicmp(name, L"steamapps") == 0) return p;
        PathRemoveFileSpecW(p.data());
        p.resize(wcslen(p.c_str()));
    }
    return L"";
}

static std::wstring GetWorkshopDir(const std::wstring& gameRoot) {
    std::wstring sa = DeriveSteamApps(gameRoot);
    if (sa.empty()) return L"";
    return sa + L"\\workshop\\content\\262060";
}

// ---------- 注册表 / Steam 库枚举 ----------
static std::wstring ReadRegString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    std::wstring result;
    if (RegQueryValueExW(hKey, name, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        result = buf;
    }
    RegCloseKey(hKey);
    return result;
}

static std::vector<std::string> VdfStringTokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '"') {
            ++i;
            std::string t;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) { ++i; t.push_back(s[i]); }
                else t.push_back(s[i]);
                ++i;
            }
            if (i < s.size()) ++i;
            out.push_back(std::move(t));
        }
        else {
            ++i;
        }
    }
    return out;
}

static void AddUnique(std::vector<std::wstring>& v, const std::wstring& p0) {
    std::wstring p = TrimTrailingSlash(p0);
    if (p.empty()) return;
    for (auto& e : v) if (_wcsicmp(e.c_str(), p.c_str()) == 0) return;
    v.push_back(p);
}

static std::vector<std::wstring> GetSteamLibraries() {
    std::vector<std::wstring> libs;

    std::wstring steamPath = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty())
        steamPath = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty())
        steamPath = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    AddUnique(libs, steamPath);

    for (size_t li = 0; li < libs.size(); ++li) {
        std::wstring vdf = libs[li] + L"\\steamapps\\libraryfolders.vdf";
        FILE* f = nullptr;
        _wfopen_s(&f, vdf.c_str(), L"rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buf;
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            buf.resize(sz);
            fread(&buf[0], 1, sz, f);
        }
        fclose(f);
        if (buf.empty()) continue;

        auto toks = VdfStringTokens(buf);

        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            if (_stricmp(toks[i].c_str(), "path") == 0) {
                AddUnique(libs, Utf8ToWide(toks[i + 1]));
            }
        }
        for (size_t i = 0; i + 1 < toks.size(); ++i) {
            const std::string& k = toks[i];
            if (k.empty()) continue;
            bool numeric = true;
            for (char c : k) if (!isdigit((unsigned char)c)) { numeric = false; break; }
            if (!numeric) continue;
            const std::string& v = toks[i + 1];
            if (v.size() > 2 && isalpha((unsigned char)v[0]) && v[1] == ':') {
                AddUnique(libs, Utf8ToWide(v));
            }
        }
        break;
    }
    return libs;
}

static std::wstring AutoDetectGameRoot() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    std::wstring dir = exePath;
    for (int i = 0; i < 4; ++i) {
        if (IsValidGameRoot(dir)) return dir;
        PathRemoveFileSpecW(dir.data());
        dir.resize(wcslen(dir.c_str()));
    }

    static const wchar_t* kFolderNames[] = {
        L"DarkestDungeon", L"Darkest Dungeon", L"DarkestDungeon_Windows"
    };
    for (auto& lib : GetSteamLibraries()) {
        for (auto name : kFolderNames) {
            std::wstring c = lib + L"\\steamapps\\common\\" + name;
            if (IsValidGameRoot(c)) return c;
        }
    }

    const wchar_t* common[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\DarkestDungeon",
        L"D:\\Program Files (x86)\\Steam\\steamapps\\common\\DarkestDungeon",
        L"E:\\Program Files (x86)\\Steam\\steamapps\\common\\DarkestDungeon",
        L"F:\\Program Files (x86)\\Steam\\steamapps\\common\\DarkestDungeon",
        L"D:\\Steam\\steamapps\\common\\DarkestDungeon",
        L"C:\\Steam\\steamapps\\common\\DarkestDungeon",
        L"E:\\Steam\\steamapps\\common\\DarkestDungeon",
        L"F:\\Steam\\steamapps\\common\\DarkestDungeon",
    };
    for (auto p : common) if (IsValidGameRoot(p)) return p;

    return L"";
}

// ---------- GUI 配置持久化 ----------
// 放在程序目录的 ini 子目录，默认安装时即 <游戏根目录>\DDModLoader\ini。
// 这个文件保存着游戏目录，所以不能靠游戏目录来定位
static std::wstring GetGuiIniPath() {
    wchar_t exeDir[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) return L"";
    PathRemoveFileSpecW(exeDir);
    std::wstring d = std::wstring(exeDir) + L"\\ini";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\gui.ini";
}

static std::wstring LoadSavedGameRoot() {
    std::wstring ini = GetGuiIniPath();
    if (ini.empty()) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"Paths", L"GameRoot", L"", buf, MAX_PATH * 2, ini.c_str());
    return buf;
}

static void SaveGameRoot(const std::wstring& root) {
    std::wstring ini = GetGuiIniPath();
    if (ini.empty()) return;
    WritePrivateProfileStringW(L"Paths", L"GameRoot", root.c_str(), ini.c_str());
}

static bool LoadSavedGroupView() {
    std::wstring ini = GetGuiIniPath();
    if (ini.empty()) return true;
    return GetPrivateProfileIntW(L"View", L"GroupView", 1, ini.c_str()) != 0;
}

static void SaveGroupView(bool v) {
    std::wstring ini = GetGuiIniPath();
    if (ini.empty()) return;
    WritePrivateProfileStringW(L"View", L"GroupView", v ? L"1" : L"0", ini.c_str());
}

// ---------- 选择目录 ----------
static bool PickFolder(HWND owner, const wchar_t* title, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg))))
        return false;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title ? title : L"Choose folder");

    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = TrimTrailingSlash(path);
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ---------- 组头刷新 / 整 MOD 操作 / 右键菜单 ----------
static void UpdateGroupHeaders() {
    if (!g_groupView || !g_hList) return;
    int count = ListView_GetGroupCount(g_hList);
    for (int gi = 0; gi < count; ++gi) {
        LVGROUP grp = {};
        grp.cbSize = sizeof(grp);
        grp.mask = LVGF_GROUPID;
        if (ListView_GetGroupInfoByIndex(g_hList, gi, &grp) == -1) continue;
        int groupId = grp.iGroupId;
        int startIdx = groupId - 1;
        if (startIdx < 0 || startIdx >= (int)g_entries.size()) continue;
        const std::wstring& modName = g_entries[startIdx].modName;

        int enabled = 0, total = 0;
        for (size_t i = 0; i < g_entries.size(); ++i) {
            if (_wcsicmp(g_entries[i].modName.c_str(), modName.c_str()) != 0) continue;
            ++total;
            if (ListView_GetCheckState(g_hList, (int)i)) ++enabled;
        }

        wchar_t header[1024];
        swprintf_s(header, L"%s  [%d/%d]", modName.c_str(), enabled, total);

        LVGROUP g2 = {};
        g2.cbSize = sizeof(g2);
        g2.mask = LVGF_HEADER;
        g2.pszHeader = header;
        ListView_SetGroupInfo(g_hList, groupId, &g2);
    }
}

static void SetModAll(const std::wstring& modName, bool enabled) {
    g_suppressGroupUpdate = true;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        if (_wcsicmp(g_entries[i].modName.c_str(), modName.c_str()) == 0) {
            ListView_SetCheckState(g_hList, (int)i, enabled ? TRUE : FALSE);
        }
    }
    g_suppressGroupUpdate = false;
    UpdateGroupHeaders();
}

static int HitTestGroupHeader(HWND hList, POINT pt) {
    if (!g_groupView) return 0;
    int count = ListView_GetGroupCount(hList);
    for (int i = 0; i < count; ++i) {
        LVGROUP grp = {};
        grp.cbSize = sizeof(grp);
        grp.mask = LVGF_GROUPID;
        if (ListView_GetGroupInfoByIndex(hList, i, &grp) == -1) continue;
        RECT rc;
        if (ListView_GetGroupRect(hList, grp.iGroupId, LVGGR_HEADER, &rc)) {
            if (PtInRect(&rc, pt)) return grp.iGroupId;
        }
    }
    return 0;
}

static void ShowContextMenu(HWND hWnd, int itemIndex, int groupId) {
    std::wstring modName;
    if (itemIndex >= 0 && itemIndex < (int)g_entries.size()) {
        modName = g_entries[itemIndex].modName;
    }
    else if (groupId > 0) {
        int startIdx = groupId - 1;
        if (startIdx >= 0 && startIdx < (int)g_entries.size())
            modName = g_entries[startIdx].modName;
    }

    HMENU menu = CreatePopupMenu();
    if (itemIndex >= 0) {
        AppendMenuW(menu, MF_STRING, IDM_ENABLE_DLL, g_loc->menuEnableDll);
        AppendMenuW(menu, MF_STRING, IDM_DISABLE_DLL, g_loc->menuDisableDll);
        if (!modName.empty())
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (!modName.empty()) {
        AppendMenuW(menu, MF_STRING, IDM_ENABLE_MOD, g_loc->menuEnableMod);
        AppendMenuW(menu, MF_STRING, IDM_DISABLE_MOD, g_loc->menuDisableMod);
    }

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_ENABLE_DLL:
        if (itemIndex >= 0) {
            ListView_SetCheckState(g_hList, itemIndex, TRUE);
            UpdateGroupHeaders();
        }
        break;
    case IDM_DISABLE_DLL:
        if (itemIndex >= 0) {
            ListView_SetCheckState(g_hList, itemIndex, FALSE);
            UpdateGroupHeaders();
        }
        break;
    case IDM_ENABLE_MOD:
        if (!modName.empty()) SetModAll(modName, true);
        break;
    case IDM_DISABLE_MOD:
        if (!modName.empty()) SetModAll(modName, false);
        break;
    }
}

// ---------- 列表 / 保存 / 启动 ----------
static void UpdateToggleButtonText() {
    HWND h = GetDlgItem(g_hWnd, IDC_TOGGLE_GROUP);
    if (h) SetWindowTextW(h, g_groupView ? g_loc->btnFlatView : g_loc->btnGroupView);
}

static void CountModStats(const std::wstring& modName,
    const std::unordered_map<std::wstring, bool>& cfg,
    int& enabled, int& total)
{
    enabled = 0;
    total = 0;
    for (auto& x : g_entries) {
        if (_wcsicmp(x.modName.c_str(), modName.c_str()) != 0) continue;
        ++total;
        auto it = cfg.find(NormalizePath(x.fullPath));
        if (it == cfg.end() || it->second) ++enabled;
    }
}

static void RefreshList() {
    if (!g_hList) return;

    g_suppressGroupUpdate = true;

    ListView_DeleteAllItems(g_hList);
    ListView_RemoveAllGroups(g_hList);
    g_entries.clear();

    ScanModsFolder(g_gameRoot + L"\\mods", g_entries, true);
    for (auto& extra : g_extraDirs) ScanModsFolder(extra, g_entries);
    std::wstring ws = GetWorkshopDir(g_gameRoot);
    if (!ws.empty()) ScanWorkshopFolder(ws, g_entries);

    // 按规范化路径去重（防止 mods 与额外目录重复）
    {
        std::unordered_map<std::wstring, bool> seen;
        std::vector<DllEntry> uniq;
        uniq.reserve(g_entries.size());
        for (auto& e : g_entries) {
            std::wstring key = NormalizePath(e.fullPath);
            if (seen.count(key)) continue;
            seen[key] = true;
            uniq.push_back(std::move(e));
        }
        g_entries.swap(uniq);
    }

    // 前置目录已有同名 DLL 时，加载器不会加载其他目录的副本，这里也不列出
    {
        std::vector<DllEntry> forceLoaded;
        ScanDllsRec(g_gameRoot + L"\\mods\\" + kForceLoadFolderName, L"", forceLoaded);
        std::unordered_map<std::wstring, bool> names;
        for (auto& e : forceLoaded) names[NormalizePath(e.dllName)] = true;
        g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
            [&](const DllEntry& e) { return names.count(NormalizePath(e.dllName)) > 0; }),
            g_entries.end());
    }

    std::stable_sort(g_entries.begin(), g_entries.end(),
        [](const DllEntry& a, const DllEntry& b) {
            int c = _wcsicmp(a.modName.c_str(), b.modName.c_str());
            if (c != 0) return c < 0;
            return _wcsicmp(a.dllName.c_str(), b.dllName.c_str()) < 0;
        });

    auto cfg = LoadConfig(g_configPath);

    ListView_EnableGroupView(g_hList, g_groupView ? TRUE : FALSE);

    int currentGroupId = -1;
    std::wstring currentMod;

    for (size_t i = 0; i < g_entries.size(); ++i) {
        auto& e = g_entries[i];

        if (g_groupView &&
            (currentGroupId < 0 || _wcsicmp(e.modName.c_str(), currentMod.c_str()) != 0))
        {
            currentMod = e.modName;

            int enabled = 0, total = 0;
            CountModStats(currentMod, cfg, enabled, total);

            wchar_t header[1024];
            swprintf_s(header, L"%s  [%d/%d]", currentMod.c_str(), enabled, total);

            LVGROUP grp = {};
            grp.cbSize = sizeof(grp);
            grp.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
            grp.pszHeader = header;
            grp.iGroupId = (int)i + 1;
            grp.stateMask = LVGS_COLLAPSIBLE;
            grp.state = LVGS_COLLAPSIBLE;
            ListView_InsertGroup(g_hList, -1, &grp);

            currentGroupId = (int)i + 1;
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT | (g_groupView ? LVIF_GROUPID : 0);
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.iGroupId = currentGroupId;
        item.pszText = (LPWSTR)e.modName.c_str();
        ListView_InsertItem(g_hList, &item);

        ListView_SetItemText(g_hList, (int)i, 1, (LPWSTR)e.dllName.c_str());
        ListView_SetItemText(g_hList, (int)i, 2, (LPWSTR)e.fullPath.c_str());

        auto it = cfg.find(NormalizePath(e.fullPath));
        bool enabled = (it == cfg.end()) ? true : it->second;
        ListView_SetCheckState(g_hList, (int)i, enabled ? TRUE : FALSE);
    }

    g_suppressGroupUpdate = false;
    UpdateGroupHeaders();

    SetWindowTextW(g_hWnd, (std::wstring(g_loc->appTitle) + L" - " + g_gameRoot).c_str());
    UpdateToggleButtonText();
}

static void SaveAll() {
    std::vector<std::pair<std::wstring, bool>> items;
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) {
        items.push_back({ g_entries[i].fullPath,
                          ListView_GetCheckState(g_hList, i) != FALSE });
    }
    SaveConfig(g_configPath, items);
    MessageBoxW(g_hWnd, g_loc->msgSaved, g_loc->msgInfoTitle, MB_OK | MB_ICONINFORMATION);
}

static void LaunchViaSteam() {
    HINSTANCE r = ShellExecuteW(nullptr, L"open",
        L"steam://rungameid/262060", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(g_hWnd, g_loc->msgSteamFail, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
    }
}

static bool LaunchOneOffline(const std::wstring& exe, const std::wstring& workDir) {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
        workDir.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

static void LaunchOffline() {
    const std::wstring exe64 = g_gameRoot + L"\\_windowsnosteam\\win64\\Darkest.exe";
    const std::wstring dir64 = g_gameRoot + L"\\_windowsnosteam\\win64";
    const std::wstring exe32 = g_gameRoot + L"\\_windowsnosteam\\win32\\Darkest.exe";
    const std::wstring dir32 = g_gameRoot + L"\\_windowsnosteam\\win32";

    const bool has64 = FileExists(exe64);
    const bool has32 = FileExists(exe32);

    if (!has64 && !has32) {
        wchar_t buf[2048];
        std::wstring shown = exe64 + L"\n  " + exe32;
        swprintf_s(buf, g_loc->msgOfflineNotFoundFmt, shown.c_str());
        MessageBoxW(g_hWnd, buf, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }

    // 只有 64 位
    if (has64 && !has32) {
        if (!LaunchOneOffline(exe64, dir64))
            MessageBoxW(g_hWnd, g_loc->msgOfflineFail,
                g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }

    // 只有 32 位
    if (!has64 && has32) {
        if (!LaunchOneOffline(exe32, dir32))
            MessageBoxW(g_hWnd, g_loc->msgOfflineFail,
                g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }

    // 两个都存在：让玩家选择
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, g_loc->menuLaunchOffline64);
    AppendMenuW(menu, MF_STRING, 2, g_loc->menuLaunchOffline32);

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 0) return; // 玩家取消

    bool ok;
    if (cmd == 1) ok = LaunchOneOffline(exe64, dir64);
    else          ok = LaunchOneOffline(exe32, dir32);

    if (!ok) {
        MessageBoxW(g_hWnd, g_loc->msgOfflineFail,
            g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
    }
}

static void SetAll(BOOL state) {
    g_suppressGroupUpdate = true;
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_hList, i, state);
    g_suppressGroupUpdate = false;
    UpdateGroupHeaders();
}

static void ApplyNewGameRoot(const std::wstring& root) {
    g_gameRoot = TrimTrailingSlash(root);
    g_configPath = GetCfgFilePath(L"DDModLoader.cfg");
    g_extraDirs = LoadExtraDirs();
    SaveGameRoot(g_gameRoot);
    RefreshList();
}

static void OnChooseRoot() {
    std::wstring picked;
    if (!PickFolder(g_hWnd, g_loc->msgPickRootTitle, picked)) return;
    if (!IsValidGameRoot(picked)) {
        wchar_t buf[2048];
        swprintf_s(buf, g_loc->msgInvalidRootFmt, picked.c_str());
        MessageBoxW(g_hWnd, buf, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }
    ApplyNewGameRoot(picked);
}

static void OnToggleGroup() {
    g_groupView = !g_groupView;
    SaveGroupView(g_groupView);
    RefreshList();
}

// ---------- 额外 MOD 目录管理 ----------
static void OnAddExtraDir() {
    std::wstring picked;
    if (!PickFolder(g_hWnd, g_loc->msgPickExtraTitle, picked)) return;
    picked = TrimTrailingSlash(picked);

    if (!DirectoryExists(picked)) {
        wchar_t buf[2048];
        swprintf_s(buf, g_loc->msgExtraInvalidFmt, picked.c_str());
        MessageBoxW(g_hWnd, buf, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }

    for (auto& d : g_extraDirs) {
        if (_wcsicmp(d.c_str(), picked.c_str()) == 0) {
            MessageBoxW(g_hWnd, g_loc->msgExtraDup,
                g_loc->msgInfoTitle, MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    g_extraDirs.push_back(picked);
    SaveExtraDirs(g_extraDirs);
    RefreshList();
}

static void OnRemoveExtraDir() {
    if (g_extraDirs.empty()) {
        MessageBoxW(g_hWnd, g_loc->msgNoExtraDirs,
            g_loc->msgInfoTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < g_extraDirs.size(); ++i) {
        AppendMenuW(menu, MF_STRING, (UINT_PTR)(i + 1), g_extraDirs[i].c_str());
    }

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd > 0 && (size_t)cmd <= g_extraDirs.size()) {
        g_extraDirs.erase(g_extraDirs.begin() + (cmd - 1));
        SaveExtraDirs(g_extraDirs);
        RefreshList();
    }
}

// ---------- 窗口过程 ----------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;

        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)g_loc->colModName;
        col.cx = 260;
        ListView_InsertColumn(g_hList, 0, &col);
        col.pszText = (LPWSTR)g_loc->colDllName;
        col.cx = 200;
        ListView_InsertColumn(g_hList, 1, &col);
        col.pszText = (LPWSTR)g_loc->colFullPath;
        col.cx = 480;
        ListView_InsertColumn(g_hList, 2, &col);

        CreateWindowW(L"BUTTON", g_loc->btnRefresh, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_REFRESH, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnEnableAll, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_ALL, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnDisableAll, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_NONE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnGroupView, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_TOGGLE_GROUP, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnChooseRoot, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_CHOOSE_ROOT, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnAddExtra, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_ADD_EXTRA, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnRemoveExtra, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_REMOVE_EXTRA, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnLaunchSteam, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_LAUNCH_STEAM, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnLaunchOffline, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_LAUNCH_OFFLINE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", g_loc->btnSave, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, (HMENU)IDC_SAVE, nullptr, nullptr);

        RefreshList();
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int clientW = rc.right;
        int clientH = rc.bottom;

        // 两行按钮区
        int bottomBarH = 90;
        int listH = clientH - bottomBarH - 10;
        if (listH < 100) listH = 100;

        SetWindowPos(g_hList, nullptr, 10, 10, clientW - 20, listH, SWP_NOZORDER);

        int row1Y = clientH - 80;
        int row2Y = clientH - 40;

        SetWindowPos(GetDlgItem(hWnd, IDC_REFRESH), nullptr, 10, row1Y, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_ALL), nullptr, 120, row1Y, 110, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_NONE), nullptr, 240, row1Y, 110, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_TOGGLE_GROUP), nullptr, 360, row1Y, 120, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_CHOOSE_ROOT), nullptr, 490, row1Y, 140, 30, SWP_NOZORDER);

        SetWindowPos(GetDlgItem(hWnd, IDC_ADD_EXTRA), nullptr, 10, row2Y, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_REMOVE_EXTRA), nullptr, 170, row2Y, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_LAUNCH_STEAM), nullptr, 330, row2Y, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_LAUNCH_OFFLINE), nullptr, 490, row2Y, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_SAVE), nullptr, clientW - 110, row2Y, 100, 30, SWP_NOZORDER);

        return 0;
    }
    case WM_CONTEXTMENU: {
        if ((HWND)w != g_hList) break;

        POINT pt;
        if (l == (LPARAM)-1) {
            int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
            if (sel < 0) return 0;
            RECT rc;
            if (!ListView_GetItemRect(g_hList, sel, &rc, LVIR_BOUNDS)) return 0;
            pt.x = (rc.left + rc.right) / 2;
            pt.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(g_hList, &pt);
        }
        else {
            GetCursorPos(&pt);
        }

        POINT client = pt;
        ScreenToClient(g_hList, &client);

        int groupId = HitTestGroupHeader(g_hList, client);
        int itemIndex = -1;
        if (groupId == 0) {
            LVHITTESTINFO hit = {};
            hit.pt = client;
            int idx = ListView_HitTest(g_hList, &hit);
            if (idx >= 0) itemIndex = idx;
        }
        if (itemIndex < 0 && groupId == 0) break;

        ShowContextMenu(hWnd, itemIndex, groupId);
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)l;
        if (hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED) {
            LPNMLISTVIEW nv = (LPNMLISTVIEW)l;
            if ((nv->uChanged & LVIF_STATE) &&
                ((nv->uOldState ^ nv->uNewState) & LVIS_STATEIMAGEMASK) &&
                !g_suppressGroupUpdate) {
                UpdateGroupHeaders();
            }
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_REFRESH:        RefreshList(); break;
        case IDC_SAVE:           SaveAll();     break;
        case IDC_ALL:            SetAll(TRUE);  break;
        case IDC_NONE:           SetAll(FALSE); break;
        case IDC_TOGGLE_GROUP:   OnToggleGroup(); break;
        case IDC_CHOOSE_ROOT:    OnChooseRoot(); break;
        case IDC_ADD_EXTRA:      OnAddExtraDir(); break;
        case IDC_REMOVE_EXTRA:   OnRemoveExtraDir(); break;
        case IDC_LAUNCH_STEAM:   LaunchViaSteam(); break;
        case IDC_LAUNCH_OFFLINE: LaunchOffline();  break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, w, l);
}

// ---------- 入口 ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    LANGID lang = GetUserDefaultUILanguage();
    g_loc = (PRIMARYLANGID(lang) == LANG_CHINESE) ? &kZh : &kEn;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (lpCmdLine && *lpCmdLine) {
        std::wstring arg = lpCmdLine;
        if (arg.size() >= 2 && arg.front() == L'"' && arg.back() == L'"')
            arg = arg.substr(1, arg.size() - 2);
        arg = TrimTrailingSlash(arg);
        if (IsValidGameRoot(arg)) g_gameRoot = arg;
    }

    if (g_gameRoot.empty()) {
        std::wstring saved = LoadSavedGameRoot();
        if (IsValidGameRoot(saved)) g_gameRoot = TrimTrailingSlash(saved);
    }

    if (g_gameRoot.empty()) {
        g_gameRoot = AutoDetectGameRoot();
    }

    if (g_gameRoot.empty()) {
        std::wstring picked;
        if (PickFolder(nullptr, g_loc->msgPickRootTitle, picked) && IsValidGameRoot(picked)) {
            g_gameRoot = picked;
        }
        else {
            MessageBoxW(nullptr, g_loc->msgNoGameRoot, g_loc->appTitle, MB_OK | MB_ICONERROR);
            CoUninitialize();
            return 1;
        }
    }

    g_gameRoot = TrimTrailingSlash(g_gameRoot);
    g_configPath = GetCfgFilePath(L"DDModLoader.cfg");
    g_extraDirs = LoadExtraDirs();
    SaveGameRoot(g_gameRoot);
    g_groupView = LoadSavedGroupView();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DDModLoaderConfig";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"DDModLoaderConfig", g_loc->appTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 600,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
#include "framework.h"
#include "DDModLoaderGUI.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h> 
#include <shellapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cwctype>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
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
    const wchar_t* btnLaunchSteam;
    const wchar_t* btnLaunchOffline;
    const wchar_t* btnSave;
    const wchar_t* msgSaved;
    const wchar_t* msgInfoTitle;
    const wchar_t* msgErrorTitle;
    const wchar_t* msgSteamFail;
    const wchar_t* msgOfflineNotFoundFmt;   // %s = 路径
    const wchar_t* msgOfflineFail;
    const wchar_t* msgNoGameRoot;
    const wchar_t* unknownMod;
};

static const LocStrings kZh = {
    L"DDModLoader 配置工具",
    L"MOD 名称",
    L"DLL 名称",
    L"完整路径",
    L"刷新",
    L"全启用",
    L"全禁用",
    L"从 Steam 启动",
    L"启动离线版",
    L"保存",
    L"配置已保存到 DDModLoader.cfg。",
    L"提示",
    L"错误",
    L"无法通过 Steam 启动游戏。\n请确认 Steam 已安装并处于登录状态。",
    L"未找到离线版可执行文件：\n%s\n\n如果你只安装了 Steam 版，请使用「从 Steam 启动」。",
    L"启动离线版游戏失败。",
    L"未能自动找到《暗黑地牢》的游戏根目录。\n\n"
    L"请将本程序放到游戏根目录（含 mods 文件夹的那个目录），"
    L"或在命令行中指定路径后重试。",
    L"(未知)",
};

static const LocStrings kEn = {
    L"DDModLoader Config Tool",
    L"Mod Name",
    L"DLL Name",
    L"Full Path",
    L"Refresh",
    L"Enable All",
    L"Disable All",
    L"Launch via Steam",
    L"Launch Offline",
    L"Save",
    L"Configuration saved to DDModLoader.cfg.",
    L"Info",
    L"Error",
    L"Failed to launch the game via Steam.\nPlease make sure Steam is installed and signed in.",
    L"Offline executable not found:\n%s\n\nIf you only have the Steam version, please use \"Launch via Steam\".",
    L"Failed to launch the offline game.",
    L"Unable to locate the Darkest Dungeon game root folder.\n\n"
    L"Please place this program in the game root folder (the folder containing the 'mods' directory), "
    L"or pass the path as a command-line argument and try again.",
    L"(Unknown)",
};

static const LocStrings* g_loc = nullptr;

// ---------- 与加载器保持一致的排除名单 ----------
static const std::vector<std::wstring> kExcludedDlls = {
    L"dbghelp.dll", L"fmod.dll", L"fmodstudio.dll", L"sdl2.dll",
    L"glew32.dll", L"steam_api.dll", L"vcruntime140.dll",
    L"vcruntime140_1.dll", L"msvcp140.dll"
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
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), (int)title.size(), &w[0], wlen);
    return w;
}

// 从 DLL 所在目录向上找最多 3 层，找 project 文件拿 Title
static std::wstring FindModName(const std::wstring& dllDir) {
    std::wstring dir = dllDir;
    for (int depth = 0; depth < 3; ++depth) {
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
        if (dir.empty()) break;
        PathRemoveFileSpecW(dir.data());
        dir.resize(wcslen(dir.c_str()));
    }
    // 兜底：DLL 所在目录名
    std::wstring fallback = dllDir;
    const wchar_t* name = PathFindFileNameW(fallback.c_str());
    if (name && *name) return name;
    return g_loc ? g_loc->unknownMod : L"(未知)";
}

static bool DirectoryExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring NormalizePath(const std::wstring& path) {
    std::wstring res = path;
    std::transform(res.begin(), res.end(), res.begin(), ::towlower);
    std::replace(res.begin(), res.end(), L'/', L'\\');
    return res;
}

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

static void ScanDlls(const std::wstring& dir, std::vector<DllEntry>& out) {
    if (!DirectoryExists(dir)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDlls(full, out);
        }
        else if (PathMatchSpecW(fd.cFileName, L"*.dll") && !IsExcluded(fd.cFileName)) {
            DllEntry e;
            e.dllName = fd.cFileName;
            e.fullPath = full;
            e.modName = FindModName(dir);
            out.push_back(e);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void SaveConfig(const std::wstring& path,
    const std::vector<std::pair<std::wstring, bool>>& items) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    for (auto& [p, e] : items) fwprintf(f, L"%d|%s\n", e ? 1 : 0, p.c_str());
    fclose(f);
}

// ---------- 控件 ID ----------
#define IDC_LIST      1001
#define IDC_REFRESH   1002
#define IDC_SAVE      1003
#define IDC_ALL       1004
#define IDC_NONE      1005
#define IDC_LAUNCH_STEAM    1007
#define IDC_LAUNCH_OFFLINE  1008

static std::wstring g_gameRoot;
static std::wstring g_configPath;
static std::vector<DllEntry> g_entries;
static HWND g_hList = nullptr;
static HWND g_hWnd = nullptr;

static bool IsValidGameRoot(const std::wstring& p) {
    if (p.empty() || !DirectoryExists(p)) return false;
    return DirectoryExists(p + L"\\mods") ||
        DirectoryExists(p + L"\\_windows") ||
        DirectoryExists(p + L"\\_windowsnosteam");
}

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

static std::wstring AutoDetectGameRoot() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    std::wstring dir = exePath;
    for (int i = 0; i < 3; ++i) {
        if (IsValidGameRoot(dir)) return dir;
        PathRemoveFileSpecW(dir.data());
        dir.resize(wcslen(dir.c_str()));
    }

    std::wstring steam = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        std::wstring c = steam + L"\\steamapps\\common\\DarkestDungeon";
        if (IsValidGameRoot(c)) return c;
    }

    steam = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!steam.empty()) {
        std::wstring c = steam + L"\\steamapps\\common\\DarkestDungeon";
        if (IsValidGameRoot(c)) return c;
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

static void RefreshList() {
    ListView_DeleteAllItems(g_hList);
    g_entries.clear();

    ScanDlls(g_gameRoot + L"\\mods", g_entries);

    std::wstring steamApps = g_gameRoot;
    PathRemoveFileSpecW(steamApps.data()); steamApps.resize(wcslen(steamApps.c_str()));
    PathRemoveFileSpecW(steamApps.data()); steamApps.resize(wcslen(steamApps.c_str()));
    ScanDlls(steamApps + L"\\workshop\\content\\262060", g_entries);

    auto cfg = LoadConfig(g_configPath);

    for (size_t i = 0; i < g_entries.size(); ++i) {
        auto& e = g_entries[i];

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = (LPWSTR)e.modName.c_str();
        ListView_InsertItem(g_hList, &item);

        ListView_SetItemText(g_hList, (int)i, 1, (LPWSTR)e.dllName.c_str());
        ListView_SetItemText(g_hList, (int)i, 2, (LPWSTR)e.fullPath.c_str());

        auto it = cfg.find(NormalizePath(e.fullPath));
        bool enabled = (it == cfg.end()) ? true : it->second;
        ListView_SetCheckState(g_hList, (int)i, enabled ? TRUE : FALSE);
    }

    SetWindowTextW(g_hWnd, (std::wstring(g_loc->appTitle) + L" - " + g_gameRoot).c_str());
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

static void LaunchOffline() {
    std::wstring exe = g_gameRoot + L"\\_windowsnosteam\\win64\\Darkest.exe";
    std::wstring workDir = g_gameRoot + L"\\_windowsnosteam\\win64";

    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t buf[2048];
        swprintf_s(buf, g_loc->msgOfflineNotFoundFmt, exe.c_str());
        MessageBoxW(g_hWnd, buf, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
        return;
    }

    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
        workDir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(g_hWnd, g_loc->msgOfflineFail, g_loc->msgErrorTitle, MB_OK | MB_ICONERROR);
    }
}

static void SetAll(BOOL state) {
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_hList, i, state);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
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

        int bottomBarH = 50;
        int listH = clientH - bottomBarH - 10;
        if (listH < 100) listH = 100;

        SetWindowPos(g_hList, nullptr, 10, 10, clientW - 20, listH, SWP_NOZORDER);

        int btnY = clientH - 40;

        // 宽度调整后同时容纳中英文文案
        SetWindowPos(GetDlgItem(hWnd, IDC_REFRESH), nullptr, 10, btnY, 100, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_ALL), nullptr, 120, btnY, 110, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_NONE), nullptr, 240, btnY, 110, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_LAUNCH_STEAM), nullptr, 370, btnY, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_LAUNCH_OFFLINE), nullptr, 530, btnY, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hWnd, IDC_SAVE), nullptr, clientW - 110, btnY, 100, 30, SWP_NOZORDER);

        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_REFRESH: RefreshList(); break;
        case IDC_SAVE:    SaveAll();     break;
        case IDC_ALL:     SetAll(TRUE);  break;
        case IDC_NONE:    SetAll(FALSE); break;
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // 根据系统 UI 语言选择本地化字符串
    LANGID lang = GetUserDefaultUILanguage();
    g_loc = (PRIMARYLANGID(lang) == LANG_CHINESE) ? &kZh : &kEn;

    if (lpCmdLine && *lpCmdLine) {
        g_gameRoot = lpCmdLine;
        if (g_gameRoot.size() >= 2 && g_gameRoot.front() == L'"' && g_gameRoot.back() == L'"')
            g_gameRoot = g_gameRoot.substr(1, g_gameRoot.size() - 2);
    }
    if (!IsValidGameRoot(g_gameRoot)) {
        g_gameRoot = AutoDetectGameRoot();
    }
    if (g_gameRoot.empty()) {
        MessageBoxW(nullptr, g_loc->msgNoGameRoot, g_loc->appTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    g_configPath = g_gameRoot + L"\\DDModLoader.cfg";

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DDModLoaderConfig";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"DDModLoaderConfig", g_loc->appTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 540,
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
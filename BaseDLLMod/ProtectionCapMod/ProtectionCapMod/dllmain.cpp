// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
// dllmain.cpp  —  x64 Release
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#pragma comment(lib, "psapi.lib")

// ============ 全局 ============
static BYTE* g_base = nullptr;
static SIZE_T g_size = 0;
static BYTE* g_textStart = nullptr;
static BYTE* g_textEnd = nullptr;
static char  g_cfgPath[MAX_PATH] = { 0 };
static float g_cap = 0.95f;     // 默认上限
static bool  g_cfgLoaded = false;

// ============ 日志 ============
static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA("[ProtCap] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

static bool IsReadable(BYTE* addr, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return (addr + n <= (BYTE*)mbi.BaseAddress + mbi.RegionSize);
}

// ============ cfg 路径 ============
static void BuildCfgPath(HMODULE hMod) {
    char dllPath[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(hMod, dllPath, MAX_PATH)) {
        strcpy_s(g_cfgPath, MAX_PATH, "ProtectionCap.cfg");
        return;
    }
    char* lastSlash = strrchr(dllPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dllPath, '/');
    if (lastSlash) {
        *(lastSlash + 1) = 0;
        _snprintf_s(g_cfgPath, MAX_PATH, _TRUNCATE, "%sProtectionCap.cfg", dllPath);
    }
    else {
        strcpy_s(g_cfgPath, MAX_PATH, "ProtectionCap.cfg");
    }
}

// ============ 读取 cfg ============
static void WriteDefaultCfg() {
    FILE* g = nullptr;
    if (fopen_s(&g, g_cfgPath, "w") != 0 || !g) {
        Log("[!] cannot write default cfg: %s", g_cfgPath);
        return;
    }
    fprintf(g, "# ProtectionCapMod config\n");
    fprintf(g, "# protection_rating 上限, 范围 0.0 ~ 10.0\n");
    fprintf(g, "# 0.80 = 原版 80%%\n");
    fprintf(g, "# 0.95 = 95%% (默认值)\n");
    fprintf(g, "# 1.00 = 100%%, 承受 0 伤害\n");
    fprintf(g, "# 超过 1.0 可能导致负伤害, 慎用\n");
    fprintf(g, "cap = %.2f\n", g_cap);
    fclose(g);
    Log("[*] default cfg written: %s", g_cfgPath);
}

static void Trim(char* s) {
    char* p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = 0;
}

static void LoadCfg() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_cfgPath, "r") != 0 || !f) {
        Log("[!] cfg not found: %s, using default %.3f", g_cfgPath, g_cap);
        WriteDefaultCfg();
        return;
    }

    char line[512];
    int lineNo = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineNo;

        // 去 # 注释
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        // 去 // 注释（可选）
        char* slash = strstr(line, "//");
        if (slash) *slash = 0;
        // 去换行
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;

        Trim(line);
        if (!*line) continue;

        // 找 =
        char* eq = strchr(line, '=');
        if (!eq) {
            Log("[!] cfg line %d has no '=': %s", lineNo, line);
            continue;
        }
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        Trim(key);
        Trim(val);

        if (_stricmp(key, "cap") != 0) {
            Log("[!] cfg line %d unknown key: %s", lineNo, key);
            continue;
        }

        // 解析浮点（支持百分号：95% -> 0.95）
        bool hasPct = (val[0] && val[strlen(val) - 1] == '%');
        if (hasPct) val[strlen(val) - 1] = 0;

        char* endp = nullptr;
        float v = strtof(val, &endp);
        if (endp == val) {
            Log("[!] cfg line %d invalid float: %s", lineNo, val);
            continue;
        }
        if (hasPct) v /= 100.0f;   // 95% -> 0.95

        // 范围保护
        if (v < 0.0f) v = 0.0f;
        if (v > 10.0f) v = 10.0f;
        g_cap = v;
        g_cfgLoaded = true;
        Log("[*] cfg line %d: cap=%.4f", lineNo, v);
    }
    fclose(f);

    if (!g_cfgLoaded)
        Log("[!] cfg loaded but no 'cap' found, using default %.3f", g_cap);
}

// ============ 数据段全量替换 ============
static void PatchAllDataByValue(uint32_t old_bits, uint32_t new_bits, const char* tag) {
    if (old_bits == new_bits) {
        Log("[*] %s: old==new, skip", tag);
        return;
    }
    int count = 0;
    BYTE* p = g_base;
    BYTE* end = g_base + g_size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) { p += 0x1000; continue; }
        DWORD prot = mbi.Protect;
        if (mbi.State == MEM_COMMIT
            && (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
            && !(prot & PAGE_GUARD))
        {
            BYTE* start = (BYTE*)mbi.BaseAddress;
            SIZE_T sz = mbi.RegionSize;
            for (SIZE_T i = 0; i + 4 <= sz; i += 4) {
                uint32_t v; memcpy(&v, start + i, 4);
                if (v == old_bits) {
                    DWORD oldProt;
                    if (VirtualProtect(start + i, 4, PAGE_READWRITE, &oldProt)) {
                        memcpy(start + i, &new_bits, 4);
                        VirtualProtect(start + i, 4, oldProt, &oldProt);
                        float fOld, fNew;
                        memcpy(&fOld, &old_bits, 4);
                        memcpy(&fNew, &new_bits, 4);
                        Log("[+] %s @RVA=0x%llX: %.4f -> %.4f", tag,
                            (unsigned long long)(start + i - g_base), fOld, fNew);
                        count++;
                    }
                }
            }
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    Log("[*] %s: %d locations patched", tag, count);
}

// ============ .text 立即数替换 ============
static void PatchImmediate(uint32_t oldBits, uint32_t newBits, const char* tag) {
    if (oldBits == newBits) {
        Log("[*] %s imm: old==new, skip", tag);
        return;
    }
    int count = 0;
    for (BYTE* p = g_textStart; p + 10 <= g_textEnd; ++p) {
        // C7 /0 rip 相对: mov dword ptr [rip+disp32], imm32
        if (p[0] == 0xC7 && (p[1] & 0xC7) == 0x05) {
            uint32_t imm; memcpy(&imm, p + 6, 4);
            if (imm == oldBits) {
                DWORD oldProt;
                if (VirtualProtect(p + 6, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    memcpy(p + 6, &newBits, 4);
                    VirtualProtect(p + 6, 4, oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), p, 10);
                    Log("[+] %s imm(mem) @RVA=0x%llX: 0x%08X -> 0x%08X",
                        tag, (unsigned long long)(p - g_base), oldBits, newBits);
                    count++;
                }
            }
        }
        // B8+r: mov r32, imm32
        if (p[0] >= 0xB8 && p[0] <= 0xBF) {
            uint32_t imm; memcpy(&imm, p + 1, 4);
            if (imm == oldBits) {
                DWORD oldProt;
                if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    memcpy(p + 1, &newBits, 4);
                    VirtualProtect(p + 1, 4, oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), p, 5);
                    Log("[+] %s imm(reg) @RVA=0x%llX: 0x%08X -> 0x%08X",
                        tag, (unsigned long long)(p - g_base), oldBits, newBits);
                    count++;
                }
            }
        }
    }
    Log("[*] %s imm: %d patched", tag, count);
}

// ============ 主流程 ============
static DWORD WINAPI Worker(LPVOID) {
    Sleep(3000);

    HMODULE h = GetModuleHandleW(L"Darkest.exe");
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h) { Log("[-] main module null"); return 0; }

    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) {
        Log("[-] GetModuleInformation failed: %lu", GetLastError());
        return 0;
    }
    g_base = (BYTE*)mi.lpBaseOfDll;
    g_size = mi.SizeOfImage;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = { 0 }; memcpy(name, sec[i].Name, 8);
        if (strcmp(name, ".text") == 0) {
            g_textStart = g_base + sec[i].VirtualAddress;
            g_textEnd = g_textStart + sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (!g_textStart) { Log("[-] .text not found"); return 0; }

    Log("[*] base=%p size=0x%llX .text=0x%llX-0x%llX",
        g_base, (unsigned long long)g_size,
        (unsigned long long)(g_textStart - g_base),
        (unsigned long long)(g_textEnd - g_base));

    // 1) 读 cfg
    Log("[*] cfg path: %s", g_cfgPath);
    LoadCfg();
    Log("[*] effective cap = %.4f (%s)", g_cap,
        g_cfgLoaded ? "from cfg" : "default");

    // 2) 计算新旧 bits
    float capF = g_cap;
    float pctF = g_cap * 100.0f;
    uint32_t capBits, pctBits;
    memcpy(&capBits, &capF, 4);
    memcpy(&pctBits, &pctF, 4);
    Log("[*] target: 0.8f->%.4f (bits 0x%08X), 80.0f->%.4f (bits 0x%08X)",
        capF, capBits, pctF, pctBits);

    // 3) 数据段替换
    PatchAllDataByValue(0x3F4CCCCD, capBits, "0.8f");   // 0.8f  -> cap
    PatchAllDataByValue(0x42A00000, pctBits, "80.0f");  // 80.0f -> cap*100

    // 4) .text 立即数替换（防重初始化覆盖）
    PatchImmediate(0x3F4CCCCD, capBits, "0.8f");
    PatchImmediate(0x42A00000, pctBits, "80.0f");

    Log("[*] done");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        BuildCfgPath(hMod);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
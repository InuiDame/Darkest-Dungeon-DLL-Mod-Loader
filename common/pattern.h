#pragma once
// 特征码解析和扫描，加载器和 DDCore 共用。特征码文本形如 "48 89 ?? 24 08"，?? 是通配。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dd::common {

inline bool ParsePattern(const char* text, std::vector<int>& out)
{
    out.clear();
    if (!text) {
        return false;
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?' && p[1] == '?') {
            out.push_back(-1);
            p += 2;
        } else {
            const int high = hex(p[0]);
            const int low = high < 0 ? -1 : hex(p[1]);
            if (high < 0 || low < 0) {
                return false;
            }
            out.push_back((high << 4) | low);
            p += 2;
        }
        if (*p != ' ' && *p != 0) {
            return false;
        }
    }
    return !out.empty();
}

// 返回命中次数，first 写入第一个命中的地址。首字节不是通配时用 memchr 加速。
inline std::size_t ScanPattern(const std::uint8_t* data, std::size_t size, const std::vector<int>& pattern,
    const std::uint8_t** first)
{
    const std::size_t n = pattern.size();
    if (n == 0 || size < n) {
        return 0;
    }
    std::size_t hits = 0;
    const std::uint8_t* const end = data + size - n + 1;
    for (const std::uint8_t* p = data; p < end;) {
        if (pattern[0] != -1) {
            p = static_cast<const std::uint8_t*>(
                std::memchr(p, pattern[0], static_cast<std::size_t>(end - p)));
            if (!p) {
                break;
            }
        }
        std::size_t i = 0;
        for (; i < n; ++i) {
            if (pattern[i] != -1 && p[i] != static_cast<std::uint8_t>(pattern[i])) {
                break;
            }
        }
        if (i == n) {
            if (hits == 0 && first) {
                *first = p;
            }
            ++hits;
        }
        ++p;
    }
    return hits;
}

// 取已加载模块的 .text 节范围
inline bool ModuleTextSection(HMODULE module, const std::uint8_t** begin, std::size_t* size)
{
    if (!module) {
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0 && section->Name[5] == 0) {
            *begin = base + section->VirtualAddress;
            *size = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

}  // namespace dd::common

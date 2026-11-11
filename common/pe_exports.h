#pragma once
// 解析 PE 文件的导出表，判断一个 DLL 是否为新协议插件。
//
// 整个过程只读文件内容，不把 DLL 载入进程，因此不会触发旧式 DLL 的 DllMain，
// 也不会让它多运行一次。加载器和 DDCore 共用这一份实现，单元测试直接覆盖。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dd::common {

// 判定新协议的两个导出名
inline constexpr const char* kPluginQueryExport = "DDPlugin_Query";
inline constexpr const char* kPluginLoadExport = "DDPlugin_Load";

namespace pe_detail {

// 从文件偏移 off 处取 count 个 T，越界返回空
template <class T>
const T* At(const std::uint8_t* file, std::size_t size, std::size_t off, std::size_t count = 1)
{
    if (off > size || count > (size - off) / sizeof(T)) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(file + off);
}

inline bool RvaToOffset(const std::uint8_t* file, std::size_t size, const IMAGE_SECTION_HEADER* sections,
    WORD sectionCount, DWORD rva, std::size_t& offset)
{
    for (WORD i = 0; i < sectionCount; ++i) {
        const auto& s = sections[i];
        const DWORD span = s.SizeOfRawData > s.Misc.VirtualSize ? s.SizeOfRawData : s.Misc.VirtualSize;
        if (rva >= s.VirtualAddress && rva - s.VirtualAddress < span) {
            offset = static_cast<std::size_t>(rva - s.VirtualAddress) + s.PointerToRawData;
            return offset < size;
        }
    }
    return false;
}

// 比较文件内以 NUL 结尾的字符串，结尾越界视为不相等
inline bool NameEquals(const std::uint8_t* file, std::size_t size, std::size_t offset, const char* name)
{
    const std::size_t want = std::strlen(name);
    if (offset > size || want >= size - offset) {
        return false;
    }
    return std::memcmp(file + offset, name, want) == 0 && file[offset + want] == 0;
}

}  // namespace pe_detail

// file/size 是 DLL 文件的原始内容（不是加载后的映像）。
// names 全部出现在导出表中才返回 true；任何畸形结构都按“没有导出”处理。只接受 x64 映像。
inline bool PeExportsAll(const std::uint8_t* file, std::size_t size, const char* const* names, std::size_t nameCount)
{
    using namespace pe_detail;
    if (nameCount == 0 || nameCount > 8) {
        return false;
    }

    const auto* dos = At<IMAGE_DOS_HEADER>(file, size, 0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return false;
    }
    const auto ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    const auto* nt = At<IMAGE_NT_HEADERS64>(file, size, ntOffset);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return false;
    }
    const auto& exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.VirtualAddress == 0 || exportDir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return false;
    }

    const WORD sectionCount = nt->FileHeader.NumberOfSections;
    const std::size_t sectionOffset =
        ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
    const auto* sections = At<IMAGE_SECTION_HEADER>(file, size, sectionOffset, sectionCount);
    if (!sections || sectionCount == 0) {
        return false;
    }

    std::size_t exportOffset = 0;
    if (!RvaToOffset(file, size, sections, sectionCount, exportDir.VirtualAddress, exportOffset)) {
        return false;
    }
    const auto* exports = At<IMAGE_EXPORT_DIRECTORY>(file, size, exportOffset);
    if (!exports || exports->NumberOfNames == 0 || exports->NumberOfNames > (size / sizeof(DWORD))) {
        return false;
    }

    std::size_t nameTableOffset = 0;
    if (!RvaToOffset(file, size, sections, sectionCount, exports->AddressOfNames, nameTableOffset)) {
        return false;
    }
    const auto* nameRvas = At<DWORD>(file, size, nameTableOffset, exports->NumberOfNames);
    if (!nameRvas) {
        return false;
    }

    std::size_t matched = 0;
    for (std::size_t want = 0; want < nameCount; ++want) {
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            std::size_t nameOffset = 0;
            if (RvaToOffset(file, size, sections, sectionCount, nameRvas[i], nameOffset) &&
                NameEquals(file, size, nameOffset, names[want])) {
                ++matched;
                break;
            }
        }
    }
    return matched == nameCount;
}

inline bool PeIsNewProtocol(const std::uint8_t* file, std::size_t size)
{
    const char* const names[] = { kPluginQueryExport, kPluginLoadExport };
    return PeExportsAll(file, size, names, 2);
}

// 判断磁盘上的 DLL 是否为新协议插件。只映射文件，绝不调用 LoadLibrary。
// 文件打不开或解析失败都返回 false，交给旧式流程。
inline bool DllIsNewProtocol(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool result = false;
    LARGE_INTEGER length{};
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 64 && length.QuadPart < 0x40000000) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) {
            void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (view) {
                __try {
                    result = PeIsNewProtocol(static_cast<const std::uint8_t*>(view),
                        static_cast<std::size_t>(length.QuadPart));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    result = false;  // 映射期间文件被截断会触发 EXCEPTION_IN_PAGE_ERROR
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(mapping);
        }
    }
    CloseHandle(file);
    return result;
}

}  // namespace dd::common

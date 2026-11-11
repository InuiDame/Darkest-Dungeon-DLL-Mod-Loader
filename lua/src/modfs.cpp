#include "modfs.h"

#include <lauxlib.h>
#include <lddstep.h>

#include <windows.h>

#include <algorithm>

namespace ddlua {

namespace {

constexpr ULONGLONG kMaxFileSize = 256ull * 1024 * 1024;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle()
    {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value != INVALID_HANDLE_VALUE; }
};

struct FindHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FindHandle()
    {
        if (value != INVALID_HANDLE_VALUE) {
            FindClose(value);
        }
    }
};

std::wstring FinalPath(HANDLE handle)
{
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            while (!buffer.empty() && buffer.back() == L'\\') {
                buffer.pop_back();
            }
            return buffer;
        }
        buffer.resize(length + 1);
    }
    return {};
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool Inside(const std::wstring& root, const std::wstring& path)
{
    if (path.size() == root.size()) {
        return EqualsNoCase(path, root);
    }
    return path.size() > root.size() && path[root.size()] == L'\\' &&
           EqualsNoCase(std::wstring_view(path).substr(0, root.size()), root);
}

HANDLE OpenForQuery(const std::wstring& path)
{
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

enum class Kind { Missing, File, Directory, Outside, Invalid };

// 校验相对路径并打开，handle 只在返回 File / Directory 时有效
Kind OpenChecked(const std::wstring& root, std::string_view rel, Handle& handle, std::wstring* full, std::string* error)
{
    std::wstring normalized;
    if (!NormalizeRelative(rel, normalized, error)) {
        return Kind::Invalid;
    }
    std::wstring path = normalized.empty() ? root : root + L"\\" + normalized;
    HANDLE raw = OpenForQuery(path);
    if (raw == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "不存在或无法打开";
        }
        return Kind::Missing;
    }
    handle.value = raw;
    const std::wstring final = FinalPath(raw);
    if (final.empty() || !Inside(root, final)) {
        if (error) {
            *error = "路径指向 Mod 目录之外";
        }
        return Kind::Outside;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(raw, &info)) {
        if (error) {
            *error = "无法读取文件信息";
        }
        return Kind::Missing;
    }
    if (full) {
        *full = final;
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::Directory : Kind::File;
}

}  // namespace

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

bool NormalizeRelative(std::string_view path, std::wstring& out, std::string* error)
{
    const auto fail = [&](const char* reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    out.clear();
    if (path.size() > 1024) {
        return fail("路径太长");
    }
    if (path.find('\0') != std::string_view::npos) {
        return fail("路径含空字节");
    }
    if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        return fail("不允许绝对路径");
    }
    std::string normalized;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view segment = path.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") {
            if (end == path.size()) {
                break;
            }
            continue;
        }
        if (segment == "..") {
            return fail("不允许 '..'");
        }
        bool onlyDotsAndSpaces = true;
        for (const char ch : segment) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || ch == ':' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
                return fail("路径含盘符、冒号、通配符或非法字符");
            }
            if (ch != '.' && ch != ' ') {
                onlyDotsAndSpaces = false;
            }
        }
        if (onlyDotsAndSpaces) {
            return fail("路径段不能只由点和空格组成");
        }
        if (segment.back() == '.' || segment.back() == ' ') {
            return fail("路径段不能以点或空格结尾");
        }
        if (!normalized.empty()) {
            normalized += '\\';
        }
        normalized += segment;
        if (end == path.size()) {
            break;
        }
    }
    if (!normalized.empty()) {
        out = Utf8ToWide(normalized);
        if (out.empty()) {
            return fail("路径不是合法的 UTF-8");
        }
    }
    return true;
}

bool ModFs::Open(const std::string& root, std::string* error)
{
    rootFinal_.clear();
    const std::wstring wide = Utf8ToWide(root);
    if (wide.empty()) {
        if (error) {
            *error = "根目录路径为空或不是 UTF-8";
        }
        return false;
    }
    Handle handle(OpenForQuery(wide));
    if (!handle.valid()) {
        if (error) {
            *error = "无法打开目录（错误码 " + std::to_string(GetLastError()) + "）";
        }
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) {
            *error = "不是目录";
        }
        return false;
    }
    rootFinal_ = FinalPath(handle.value);
    if (rootFinal_.empty()) {
        if (error) {
            *error = "无法取得目录的最终路径";
        }
        return false;
    }
    return true;
}

bool ModFs::Exists(std::string_view rel) const
{
    if (!IsOpen()) {
        return false;
    }
    Handle handle;
    const Kind kind = OpenChecked(rootFinal_, rel, handle, nullptr, nullptr);
    return kind == Kind::File || kind == Kind::Directory;
}

bool ModFs::PushFile(lua_State* L, std::string_view rel, std::string* error) const
{
    if (!IsOpen()) {
        if (error) {
            *error = "根目录不可用";
        }
        return false;
    }
    Handle handle;
    const Kind kind = OpenChecked(rootFinal_, rel, handle, nullptr, error);
    if (kind == Kind::Directory) {
        if (error) {
            *error = "是目录";
        }
        return false;
    }
    if (kind != Kind::File) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.value, &size) || static_cast<ULONGLONG>(size.QuadPart) > kMaxFileSize) {
        if (error) {
            *error = "文件太大或无法取得大小";
        }
        return false;
    }
    const auto total = static_cast<std::size_t>(size.QuadPart);
    luaL_Buffer buffer;
    char* data = luaL_buffinitsize(L, &buffer, total);  // 超出内存上限时抛 Lua 内存错误，句柄由析构关闭
    std::size_t done = 0;
    while (done < total) {
        DWORD got = 0;
        // 每次 1 MB，读完一块报告一次工作量：main.lua、require 的模块、dd.fs.read 的读取也受预算约束（第二轮 R2A-阻断3），
        // 超时在这里抛 Lua 错误，句柄由析构关闭
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(total - done, 1u << 20));
        if (!ReadFile(handle.value, data + done, want, &got, nullptr) || got == 0) {
            break;
        }
        done += got;
        luai_ddstep(L, got >> 4);
    }
    luaL_pushresultsize(&buffer, done);
    if (done != total) {
        lua_pop(L, 1);
        if (error) {
            *error = "读取失败";
        }
        return false;
    }
    return true;
}

bool ModFs::ReadSmall(std::string_view rel, std::string& out, std::string* error) const
{
    out.clear();
    if (!IsOpen()) {
        if (error) {
            *error = "根目录不可用";
        }
        return false;
    }
    Handle handle;
    const Kind kind = OpenChecked(rootFinal_, rel, handle, nullptr, error);
    if (kind != Kind::File) {
        if (kind == Kind::Directory && error) {
            *error = "是目录";
        }
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.value, &size) || size.QuadPart > 1024 * 1024) {
        if (error) {
            *error = "文件太大（上限 1 MB）或无法取得大小";
        }
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    if (!out.empty() && (!ReadFile(handle.value, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) || got != out.size())) {
        if (error) {
            *error = "读取失败";
        }
        return false;
    }
    return true;
}

bool ModFs::List(std::string_view rel, std::vector<std::string>& out, std::string* error) const
{
    out.clear();
    if (!IsOpen()) {
        if (error) {
            *error = "根目录不可用";
        }
        return false;
    }
    Handle handle;
    std::wstring full;
    const Kind kind = OpenChecked(rootFinal_, rel, handle, &full, error);
    if (kind == Kind::File) {
        if (error) {
            *error = "不是目录";
        }
        return false;
    }
    if (kind != Kind::Directory) {
        return false;
    }
    WIN32_FIND_DATAW data{};
    FindHandle find;
    find.value = FindFirstFileExW((full + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find.value == INVALID_HANDLE_VALUE) {
        return true;  // 空目录
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        std::string item = WideToUtf8(name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            item += '/';
        }
        out.push_back(std::move(item));
    } while (FindNextFileW(find.value, &data));
    std::sort(out.begin(), out.end());
    return true;
}

}  // namespace ddlua

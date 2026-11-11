#pragma once
// Mod 目录内的只读文件访问（dd.fs、require、main.lua 都走这里）。
//
// 相对路径先做语法检查（NormalizeRelative），拼成 \\?\ 前缀的完整路径（跳过 Win32 的路径规范化，
// CON 之类的设备名和结尾的点、空格都按字面处理），打开句柄后用 GetFinalPathNameByHandle 取最终路径，
// 和根目录的最终路径比较前缀（不分大小写，要求在分隔符处结束），联接和符号链接逃逸到外面的一律拒绝。
// 读文件和判断类型都用同一个句柄，没有检查和使用之间的时间窗口。

#include <lua.h>

#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

// 语法检查和规范化：分隔符可以是 / 或 \，结果用 \ 连接（空串表示根目录本身）。
// 拒绝：空字节、绝对路径（/ \ 开头）、盘符和其他 ':'、'..' 段、只由点和空格组成的段、以点或空格结尾的段、
// 通配符和 Windows 文件名里的非法字符（< > " | ? * 和控制字符）、非 UTF-8。
bool NormalizeRelative(std::string_view path, std::wstring& out, std::string* error);

class ModFs
{
public:
    // root 是绝对路径（UTF-8）。根目录不存在或打不开时返回 false
    bool Open(const std::string& root, std::string* error);
    bool IsOpen() const { return !rootFinal_.empty(); }
    const std::wstring& RootFinal() const { return rootFinal_; }

    // 文件或目录存在且在根目录内
    bool Exists(std::string_view rel) const;
    // 读取文件，成功时把内容作为 Lua 字符串压栈（内容占用的内存计入 VM 的上限，超出时抛内存错误）。
    // 失败时不压栈，返回 false 并写 error
    bool PushFile(lua_State* L, std::string_view rel, std::string* error) const;
    // 读取小文件（manifest.json 等，上限 1 MB）到 C++ 字符串
    bool ReadSmall(std::string_view rel, std::string& out, std::string* error) const;
    // 列目录，按名字排序，子目录名后面加 '/'
    bool List(std::string_view rel, std::vector<std::string>& out, std::string* error) const;

private:
    std::wstring rootFinal_;  // \\?\D:\... 形式，不带结尾分隔符
};

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

}  // namespace ddlua

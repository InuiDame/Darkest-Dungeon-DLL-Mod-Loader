#pragma once
// Mod 目录内的只读文件访问（dd.fs、require、main.lua 都走这里），以及 P4b 的数据目录写入（dd.fs.write / remove）。
//
// 相对路径先做语法检查（NormalizeRelative），拼成 \\?\ 前缀的完整路径（跳过 Win32 的路径规范化，
// CON 之类的设备名和结尾的点、空格都按字面处理），打开句柄后用 GetFinalPathNameByHandle 取最终路径，
// 和根目录的最终路径比较前缀（不分大小写，要求在分隔符处结束），联接和符号链接逃逸到外面的一律拒绝。
// 读文件和判断类型都用同一个句柄，没有检查和使用之间的时间窗口。
//
// 写入（D10）：根目录是 <游戏根目录>\DDModLoader\data\<mod id>\，不存在时逐级创建。父目录按需逐级创建，每一级都取最终路径
// 再和根目录比较，下一级从这个最终路径接着拼（联接、符号链接逃不出去）。内容先写进同目录的临时文件，FlushFileBuffers 后用
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) 替换，要么是旧内容要么是新内容。
// 检查和写入之间仍有很短的窗口（并发的外部进程把刚检查过的目录换成联接），这里防的是 Mod 的路径，不防并发的恶意进程。

#include <lua.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

// 语法检查和规范化：分隔符可以是 / 或 \，结果用 \ 连接（空串表示根目录本身）。
// 拒绝：空字节、绝对路径（/ \ 开头）、盘符和其他 ':'、'..' 段、只由点和空格组成的段、以点或空格结尾的段、
// 通配符和 Windows 文件名里的非法字符（< > " | ? * 和控制字符）、Windows 保留的设备名（CON、NUL、COM1 等，带扩展名也算；
// \\?\ 前缀下会按字面建出来，资源管理器删不掉）、非 UTF-8。
bool NormalizeRelative(std::string_view path, std::wstring& out, std::string* error);

// 数据目录的写入最多 8 层（含文件名）：每一级父目录都要打开并取最终路径，层数越深越慢（C-B2），
// 扫描用量的 ScanUsage 能看到的深度（16）也要覆盖写入能建出的深度（C-Y1）
inline constexpr std::size_t kDataMaxSegments = 8;

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

    // ---- 写入（数据目录）----
    // 根目录不存在时逐级创建再打开
    bool OpenOrCreate(const std::string& root, std::string* error);
    // 原子写入 rel（不能是根目录本身，最多 kDataMaxSegments 层）。oldSize 输出被替换的文件原来的大小，原来没有时为 -1；
    // 缺的父目录逐级新建，最多新建 maxNewDirs 个（超出时什么都不建、返回 false），createdDirs 输出新建的个数；
    // ops 输出做了多少次文件系统操作（打开、建目录、取最终路径），调用方据此报告工作量
    bool Write(std::string_view rel, std::string_view data, std::uint64_t maxNewDirs, std::int64_t* oldSize,
        std::uint32_t* createdDirs, std::uint32_t* ops, std::string* error) const;
    // 删除文件或空目录（不能是根目录本身），removedSize 输出文件的大小，isDir 输出删掉的是不是目录
    bool Remove(std::string_view rel, std::uint64_t* removedSize, bool* isDir, std::string* error) const;
    // 文件的大小：不存在时 -1，是目录或在根目录外时 -2
    std::int64_t FileSize(std::string_view rel) const;
    // 根目录下的文件总大小和条目数（文件和目录都算，不跟联接，深度 16 层、条目 10 万个为止）。
    // 返回 false 表示碰到了条目上限、没扫完（用量按扫到的算，已经超过任何配置的上限）
    bool Usage(std::uint64_t* bytes, std::uint64_t* entries) const;

private:
    std::wstring rootFinal_;  // \\?\D:\... 形式，不带结尾分隔符
};

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

}  // namespace ddlua

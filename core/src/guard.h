#pragma once
// SEH 隔离：调用插件代码，异常不会传到 DDCore 外面。

#include <windows.h>

#include <string>

namespace ddc {

struct Fault {
    DWORD code = 0;
    void* address = nullptr;
};

// 在 __try 中执行 fn(arg)。返回 true 表示正常结束，false 表示发生异常，fault 写入详情。
// 栈溢出时恢复保护页。
bool GuardedInvoke(void (*fn)(void*), void* arg, Fault* fault);

// “异常码 C0000005，地址 00007FF...（xx.dll+0x1234）”
std::string DescribeFault(const Fault& fault);

// 地址所属模块，“xx.dll+0x1234” 或 “未知模块”
std::string DescribeAddress(const void* address);

}  // namespace ddc

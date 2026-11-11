// DDCore.dll：native 插件运行时。见 docs/02-architecture.md。
//
// DllMain 什么也不做，真正的启动由加载器在主线程调用 DDCore_Boot 完成：
// 此时已经脱离加载器锁，可以加载插件、打 hook。

#include "core.h"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl DDCore_Boot(const dd::common::CoreBootInfo* info)
{
    return ddc::CoreBoot(info);
}

extern "C" __declspec(dllexport) void __cdecl DDCore_AppSystemsCreated()
{
    ddc::CoreAppSystemsCreated();
}

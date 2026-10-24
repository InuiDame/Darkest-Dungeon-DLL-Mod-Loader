// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"

#include <windows.h>
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        OutputDebugStringW(L"[BeaconDLL] 测试DLL已被加载！\n");
    }
    return TRUE;
}


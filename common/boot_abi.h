#pragma once
// 加载器（dbghelp 代理）和 DDCore.dll 之间的内部约定，不属于插件 ABI。
//
// 开发期不保证新旧版本搭配（ADR 0018），加载器和 DDCore 要一起更新。

#include <cstdint>

namespace dd::common {

inline constexpr std::uint32_t kCoreBootVersion = 1;

// DDCore 启动导出的返回值
enum CoreBootResult : int {
    kCoreBootDisabled = -1,  // 启动失败或被停用（如特征码缺失）
    kCoreBootDormant = 0,    // 没有新协议插件，保持休眠，未安装任何 hook
    kCoreBootRunning = 1,
};

struct CoreBootInfo {
    std::uint32_t size;
    std::uint32_t version;
    const wchar_t* gameRoot;  // 游戏根目录，无结尾斜杠
    const wchar_t* binDir;    // Darkest.exe 所在目录
    void* createAppSystems;   // 已校验的 CreateAppSystems 入口
};

// 导出名。这两个函数都在主线程调用：
//   DDCore_Boot                  CreateAppSystems 执行之前
//   DDCore_AppSystemsCreated     CreateAppSystems 返回之后
inline constexpr const char* kCoreBootExport = "DDCore_Boot";
inline constexpr const char* kCoreAppSystemsCreatedExport = "DDCore_AppSystemsCreated";

using CoreBootFn = int(__cdecl*)(const CoreBootInfo*);
using CoreAppSystemsCreatedFn = void(__cdecl*)();

}  // namespace dd::common

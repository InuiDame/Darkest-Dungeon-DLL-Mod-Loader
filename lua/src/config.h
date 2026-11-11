#pragma once
// DDLua.cfg：<游戏根目录>\DDModLoader\cfg\DDLua.cfg，"key = value" 一行一项，# 开头是注释。
// 文件不存在时全部用默认值（非开发模式）。未知的键和不合法的值写警告后忽略。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua {

struct Config {
    std::uint32_t memoryLimitMb = 128;  // memory_limit_mb：整个 VM 的内存上限
    std::uint32_t budgetMs = 50;        // budget_ms：每次从 C++ 进入 Lua（回调、定时器、协程恢复）的执行预算
    std::uint32_t loadBudgetMs = 500;   // load_budget_ms：执行 main.lua（含其中的 require）的预算
    std::uint32_t maxFailures = 3;      // max_failures：订阅、定时器的回调连续出错这么多次后停用（超预算立即停用）
    std::uint32_t maxPerMod = 1000;     // max_per_mod：每个 Mod 的订阅（含 wait_event）、定时器、协程各自的上限
    std::uint32_t frameBudgetMs = 20;   // frame_budget_ms：一帧里调度器工作（重载、定时器、协程恢复）的总时间，超出的顺延到下一帧
    std::uint32_t logLinesPerSec = 200; // log_lines_per_sec：每个 Mod 每秒最多写这么多行日志（print、warn、dd.log，以及 DDLua
                                        // 替这个 Mod 写的错误、失败原因等），超出的丢弃
    std::uint32_t logMaxMb = 32;        // log_max_mb：一个会话（一次进存档）里和 Mod 有关的日志总量上限，超出后不再写入
    std::uint32_t teardownBudgetMs = 200;  // teardown_budget_ms：一次拆除或重载里关闭协程（<close>）的总时间，超出后剩下的协程直接丢弃
    bool devMode = false;               // dev_mode：开发模式（dd.reload、文件监视）
    std::vector<std::string> devGlobalDirs;  // dev_global_dirs：开发用全局加载的 Mod 目录，分号分隔，ASSETS_READY 时加载
};

// 解析配置文本。warnings 收集可读的警告（未知键、值不合法）
Config ParseConfig(std::string_view text, std::vector<std::string>* warnings);

}  // namespace ddlua

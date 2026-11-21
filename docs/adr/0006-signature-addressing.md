# 地址只用特征码，缺失时停用而不是崩溃

所有游戏地址都用特征码定位，唯一来源是 `core/src/game/signatures.inc`，加载器、`DDCore` 和 `tools/dd_update.py`（原 `tools/check_sigs.py`）共用。基准是 guest 的 Steam 版，离线版能兼容就兼容，不当作目标。

失败时分级降级，不能因为新功能让游戏起不来：

- 核心特征码（P1 的核心项和事件总线本体）缺失时，`DDCore` 整体停用，日志写明缺了哪些，旧式 DLL 不受影响。
- 可选特征码（`OPT`，Query 和各语义 hook）缺失时，只有依赖它的主题不可用，订阅返回失败。
- exe 哈希未知但特征码全部命中时只警告。

## 后果

游戏更新后先离线校验再启动游戏：现在是 `python -B DD.ModLoader/tools/dd_update.py verify <新 exe>`，完整步骤见 [06-workflow.md](../06-workflow.md) 的「游戏更新后」。`tools/check_sigs.py` 留作 `verify --only sigs,identity` 的薄封装。

被 [ADR 0035](0035-update-pipeline.md) 和 [ADR 0036](0036-runtime-layout-check.md) 补充：0035 把地址之外的游戏布局也登记进清单（`layout.inc`），校验加了同一性检查（防误绑定）和依据位置核对，并有修复工具；0036 取代上面第三条，未知版本上除了特征码全部命中，还要过运行时的布局核对，不能核对的写能力停用。其余两条（核心特征码缺失时整体停用、可选特征码缺失只影响依赖它的主题）不变。

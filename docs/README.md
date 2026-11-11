# DD.ModLoader 文档

项目目标：在 DDModLoader 的基础上，为《暗黑地牢》增加 native 插件接口（DLL）和 Lua 脚本接口，用 xmake 构建，工程形式参照 `DD.Plugins`。

当前状态：P0–P3 已通过验收，下一步是 P4（Effect 与 Buff），进度见 [04-roadmap.md](04-roadmap.md)，接手请先读 [08-handoff.md](08-handoff.md)。

| 文档 | 内容 |
| --- | --- |
| [01-research.md](01-research.md) | 启动时的调研：现有 modding 手段、原版加载器的问题、后续阶段的接入点线索 |
| [02-architecture.md](02-architecture.md) | 分层、工程结构、加载流程、native ABI、hook 仲裁 |
| [03-lua-system.md](03-lua-system.md) | Lua 系统的设计，对应五项需求 |
| [04-roadmap.md](04-roadmap.md) | 分阶段目标、交付物、验收标准、审核重点 |
| [06-workflow.md](06-workflow.md) | 阶段流程、验收分级、工具、派发和审核简报 |
| [07-event-coverage.md](07-event-coverage.md) | 事件主题覆盖表，由 `tools/event_coverage.py` 从 `test_events` 日志生成 |
| [08-handoff.md](08-handoff.md) | 接手说明：当前状态、下一步、未验证项 |
| [09-lua-scenario.md](09-lua-scenario.md) | Lua 运行时的用户场景验收：部署、存档、流程、检查脚本 |
| [history/](history/) | 已完成阶段的审核发现索引（代码注释里的审核编号查这里） |
| [adr/](adr/) | 架构决策记录，见下表 |

游戏本体的逆向事实在 `../knowledge/`（同步区根目录），这里只引用。

## 架构决策记录

| ADR | 决策 |
| --- | --- |
| [0001](adr/0001-repo-is-loader-workspace.md) | 本目录就是加载器仓库的工作区 |
| [0002](adr/0002-mit-license.md) | 使用 MIT 协议，不分发 Spine 运行时 |
| [0003](adr/0003-thin-proxy-core-layering.md) | 代理保持精简，功能放在 DDCore，DDLua 只走 SDK |
| [0004](adr/0004-early-load-before-createappsystems.md) | 新协议插件在 CreateAppSystems 之前加载，旧式 DLL 不变 |
| [0005](adr/0005-c-abi-service-tables.md) | Native ABI 是纯 C 的服务表 |
| [0006](adr/0006-signature-addressing.md) | 地址只用特征码，缺失时停用而不是崩溃 |
| [0007](adr/0007-hooks-on-demand.md) | Hook 按需安装，优先虚表，内联补丁只在安全时机修改 |
| [0008](adr/0008-generic-listener-priority.md) | 通用 listener 在 Event 中排最前，在 Query 中排最后 |
| [0009](adr/0009-off-thread-events-deferred.md) | 非主线程的原生事件复制后排队到主线程 |
| [0010](adr/0010-damage-cancel-zero-damage.md) | damage.pre 的取消是把伤害改成 0，照常调用原函数 |
| [0011](adr/0011-pvp-no-mutating-dispatch.md) | PvP 对局中不派发会改变结算的订阅 |
| [0012](adr/0012-lua54-sol2.md) | Lua 5.4.9 + sol2 3.5.0，不用 LuaJIT |
| [0013](adr/0013-single-vm-per-mod-env.md) | 单个 Lua VM，每个 Mod 一张独立的 _ENV |
| [0014](adr/0014-sandbox-no-raw-memory.md) | Lua 默认沙箱，不开放裸内存 |
| [0015](adr/0015-scripts-follow-save-mods.md) | Lua 脚本按存档启用，DLL 仍然全局 |
| [0016](adr/0016-test-rig-and-manual-scenarios.md) | 只在测试目录跑游戏，进战斗的场景由用户验收 |
| [0017](adr/0017-no-legacy-plugin-compat.md) | 不兼容现有 DD 插件 |
| [0018](adr/0018-no-version-compat-until-p7.md) | P7 之前不做版本兼容 |
| [0019](adr/0019-lua-as-cpp-with-patches.md) | Lua 按 C++ 编译，并给源码打补丁 |
| [0020](adr/0020-lua-budget-best-effort.md) | 执行预算按真实时间，防住意外，不承诺挡住刻意构造的卡顿 |
| [0021](adr/0021-lua-vm-per-campaign.md) | 每次进存档新建 Lua VM |
| [0022](adr/0022-lua-subscription-per-sdk-subscription.md) | 每个 Lua 订阅对应一个 SDK 订阅，过滤放在 DDLua |
| [0023](adr/0023-lua-payload-and-coroutines.md) | 事件负载在回调返回后失效，协程只在帧边界恢复 |
| [0024](adr/0024-actor-handles-are-values.md) | Actor 句柄是不带指针的值 |

新的决策按 `NNNN-slug.md` 顺序编号，同时加进上表。原决策表里的 RE 工具选型（Ghidra，见 `knowledge/README.md`）和「阶段顺序按路线图」没有单独成文。

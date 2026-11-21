# 分阶段路线

阶段流程、验收分级和工具见 [06-workflow.md](06-workflow.md)。上一阶段通过用户手动验收后，下一阶段才启动。

**当前进度**：P0–P4 已完成；U1（游戏更新的校验与修复）在 2026-11-15 插在 P5 之前，四个小阶段做完，等待对抗审核和用户验收。接手说明见 [08-handoff.md](08-handoff.md)。

## 已完成

| 阶段 | 验收日期 | 交付 |
| --- | --- | --- |
| P0 奠基与风险验证 | 2026-11-09 | xmake 工程骨架，加载器迁入 `loader/`；spike 验证通用 listener、早期 hook、虚表 hook 共存、跨 CRT 内存，结论写入知识库 |
| P1 Native ABI 与 DDCore | 2026-11-10 | 协议识别、早期加载、版本协商；DDCore 的日志、调度、生命周期、hook 仲裁；SDK、插件模板、示例；`check_sigs.py`（U1 起是 `dd_update.py verify` 的封装） |
| P2 事件系统（native） | 2026-11-10 | 91 个原生事件、11 个 Query 的桥接（各有 1 个因游戏缺类型描述符不可用），13 个语义 hook（`.pre` 可改、可取消），订阅优先级和作用域，非主线程事件排队；场景验收中 146 个主题收到 67 个，覆盖见 [07-event-coverage.md](07-event-coverage.md) |
| P3 Lua 运行时 | 2026-11-11 | `DDLua.dll`：单 VM、每个 Mod 一张 `_ENV` 的沙箱，按真实时间的执行预算和内存上限（Lua 按 C++ 编译并打补丁，[ADR 0019](adr/0019-lua-as-cpp-with-patches.md)、[0020](adr/0020-lua-budget-best-effort.md)），按存档发现和加载、拆除（[0021](adr/0021-lua-vm-per-campaign.md)），事件绑定、调度器和协程、热重载（[0022](adr/0022-lua-subscription-per-sdk-subscription.md)、[0023](adr/0023-lua-payload-and-coroutines.md)），DDCore 的 `game` 服务（启用 Mod 列表、actor 句柄，[0024](adr/0024-actor-handles-are-values.md)）；LuaLS 声明、Lua Mod 模板和示例；三轮对抗审核加一轮核对，Lua 官方测试集只有按设计的差异；场景验收见 [09-lua-scenario.md](09-lua-scenario.md) |
| P4 Effect 与 Buff | 2026-11-15 | DDCore 的 `combat` 服务（buff 的读写和标签、运行时定义、effect 的施加、规则扩展、技能句柄，`buff.*` 和 `damage.final` 主题，[ADR 0025](adr/0025-combat-write-timing.md)–[0030](adr/0030-damage-final-absorb.md)、[0033](adr/0033-skill-pre-read-only.md)）和 `persist` 服务（存档旁路，[0031](adr/0031-persist-sidecar-content-hash.md)），DDLua 的 `dd.buff`、`dd.effect`、脚本 Buff、规则、`dd.store`、`dd.fs` 写入（[0032](adr/0032-fs-write-data-dir.md)）和队伍、怪物的只读枚举（[0034](adr/0034-party-monster-enumeration.md)），DDShield 护盾的 Lua 复刻 `examples/lua/shield_lite`；四轮对抗审核，修复 F1–F7；场景验收分两次运行（第二次补做战斗中读档），见 [10-p4-scenario.md](10-p4-scenario.md) |

## U1 游戏更新的校验与修复

2026-11-15 用户新增，插在 P5 之前。**状态**：四个小阶段（U1a 清单、基线、校验；U1b 运行时核对和停用；U1c 修复；U1d 自测、文档、全量回归）做完，对抗审核 A 有 2 个阻断项，已按修复简报修完（FA 清单和 C++、FB Python 工具和文档），等待重新审核和用户验收；设计见 [ADR 0035](adr/0035-update-pipeline.md)、[0036](adr/0036-runtime-layout-check.md)（0036 的未知版本默认值、accept 的人工确认待用户决定，见 08 的「U1 需要用户决定的事」），工作文件在 `docs/u1/`。游戏更新改动 exe 的二进制布局后，DDModLoader 依赖的地址和结构偏移要能一条命令查出哪些失效，失效的能自动给出修复，应用后再校验通过；DDCore 遇到未知版本时自己能发现布局变化并按功能停用，不靠人记得先跑工具。

- 布局清单：特征码之外，DDCore 和加载器用到的结构偏移、大小、虚表槽位、枚举表、事件负载偏移全部登记成清单，每项带校验依据（离线、运行时）或写明没有静态依据时怎么核实；C++ 从清单取常量。
- 校验：一条命令对任意 exe 逐项判定，包括命中的是不是基线里的同一个函数、依据命中的是不是基线里那条指令（防误绑定），报出受影响的服务和主题；取代 `check_sigs.py`、`check_actor_layout.py`（两个脚本留作 `dd_update.py verify` 的薄封装）。
- 基线快照：已知版本的函数指纹、调用关系、锚点，存进仓库，可复现。
- 修复：在新 exe 里定位失效项，重新生成特征码和偏移，输出补丁、逐项可信度和旧→新 RVA 对照表；应用后自动再校验，人工确认后记为已知版本（运行时核对不了的项逐项确认，记录进仓库）。宁可报需要人工，不给错的修复；「没法确认」不算通过。
- 运行时：DDCore 按同一份清单核对能在运行时核对的项，不一致时只停用依赖它的能力；未知版本的策略写成 ADR。
- 自测：Steam 版和离线版互相盲定位、变异样本（偏移变化、特征码被破坏、多处命中、静默误绑定、函数搬迁、枚举表变化、hook 入口不安全、虚表槽位变化、依据误绑定）、应用后的往返校验，以及 C++ 和 Python 两份实现的结论对照（`dd_update.py selftest`）。

**验收**：自测一条命令全部通过，错误修复数为 0；Steam 版和离线版校验全部通过；构建、单元测试、测试目录自检不回归；「游戏更新后」的操作手册照着能走通。

**审核重点**：误绑定和错误修复（比漏报更危险）、C++ 和 Python 的语义是否一致、运行时停用的范围是否覆盖所有依赖、清单是否真的覆盖全部布局常量、自测样本是否真实而不是迁就实现。

## P5 回合外系统

- 各系统单例和字段的逆向，写入知识库。
- 只读接口，然后逐个开放写操作，覆盖庄园、名册、英雄、库存、任务、地牢、城镇事件。
- 屏幕与状态机。
- P4 留下的：`skill.pre` 的写入（挪到 `5B9B50` 之前，先查清冷却和每场次数的计数，[ADR 0033](adr/0033-skill-pre-read-only.md)）；`dd.raid` 在 P4 的只读枚举上扩展（站位、火把、房间和走廊，[ADR 0034](adr/0034-party-monster-enumeration.md)）；读档恢复的战斗要不要补派发 `battle.start`、`battle.end` 要不要带结果（打完还是撤退），以及给 Lua 一个查询当前场合（城镇、走廊、战斗）的接口。

**验收**：场景脚本覆盖每个模块的读写，改动反映在界面和存档里，改动后存档可以正常读取。

**审核重点**：写操作的时机、UI 缓存不刷新、对存档的破坏。

## P6 Spine

- 定位 `SpineSprite` 并校验 spine-c 结构。
- 三层能力依次完成：动画控制、骨骼级操作和事件、新实例。

**验收**：Lua 让英雄播放并排队动画；动画事件回调触发；更换附件生效；加载 Mod 自带骨骼显示正常；反复创建和销毁没有泄漏。

**审核重点**：资源在加载线程和主线程之间的所有权、动画切换时的内存安全。

## P7 打包发布

- 敲定公开 API 规范和兼容策略（取代 [ADR 0018](adr/0018-no-version-compat-until-p7.md)），并决定现有 DD 插件的处理（[ADR 0017](adr/0017-no-legacy-plugin-compat.md)）。
- `DDModLoaderGUI/`、`BaseDLLMod/`、`BeaconDLL/` 改为 xmake，清理仓库里被跟踪的构建产物。
- 填好 `loader/DDModLoader.rc` 的版本信息（现在是占位符，`FILETYPE` 还写成了 `VFT_APP`）。
- 安装包和 GUI 集成，GUI 显示新协议插件和 Lua Mod。
- 双语功能清单、Mod 作者指南、SDK 文档、示例。
- GitHub 仓库同步，验证 Workshop 能否分发 `.lua`。

**验收**：全新安装和升级安装都正常；旧 Mod 环境回归通过；公开 API 文档和实际接口一致；文档中的例子能跑。

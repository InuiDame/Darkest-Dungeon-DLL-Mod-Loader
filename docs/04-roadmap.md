# 分阶段路线

阶段流程、验收分级和工具见 [06-workflow.md](06-workflow.md)。上一阶段通过用户手动验收后，下一阶段才启动。

**当前进度**：P0–P3 已完成，下一步是 P4。接手说明见 [08-handoff.md](08-handoff.md)。

## 已完成

| 阶段 | 验收日期 | 交付 |
| --- | --- | --- |
| P0 奠基与风险验证 | 2026-11-09 | xmake 工程骨架，加载器迁入 `loader/`；spike 验证通用 listener、早期 hook、虚表 hook 共存、跨 CRT 内存，结论写入知识库 |
| P1 Native ABI 与 DDCore | 2026-11-10 | 协议识别、早期加载、版本协商；DDCore 的日志、调度、生命周期、hook 仲裁；SDK、插件模板、示例；`check_sigs.py` |
| P2 事件系统（native） | 2026-11-10 | 91 个原生事件、11 个 Query 的桥接（各有 1 个因游戏缺类型描述符不可用），13 个语义 hook（`.pre` 可改、可取消），订阅优先级和作用域，非主线程事件排队；场景验收中 146 个主题收到 67 个，覆盖见 [07-event-coverage.md](07-event-coverage.md) |
| P3 Lua 运行时 | 2026-11-11 | `DDLua.dll`：单 VM、每个 Mod 一张 `_ENV` 的沙箱，按真实时间的执行预算和内存上限（Lua 按 C++ 编译并打补丁，[ADR 0019](adr/0019-lua-as-cpp-with-patches.md)、[0020](adr/0020-lua-budget-best-effort.md)），按存档发现和加载、拆除（[0021](adr/0021-lua-vm-per-campaign.md)），事件绑定、调度器和协程、热重载（[0022](adr/0022-lua-subscription-per-sdk-subscription.md)、[0023](adr/0023-lua-payload-and-coroutines.md)），DDCore 的 `game` 服务（启用 Mod 列表、actor 句柄，[0024](adr/0024-actor-handles-are-values.md)）；LuaLS 声明、Lua Mod 模板和示例；三轮对抗审核加一轮核对，Lua 官方测试集只有按设计的差异；场景验收见 [09-lua-scenario.md](09-lua-scenario.md) |

## P4 Effect 与 Buff

- 查询、枚举、施加、移除，定位 buff 增删函数。
- P3 留下的：`skill.pre` 的写入验证（字段是指针，要等技能和 Effect 有句柄）；跨 VM 的嵌套入口拿不到 GC 停顿的补偿，开放写操作后要再测；`dd.fs` 的写入。
- 文本定义、脚本 Effect、脚本 Buff、规则扩展。
- 存档旁路持久化，tray 图标接入。

**验收**：用 Lua 复刻 DDShield 的一部分（如 `shield_gain_N`）行为一致；脚本 Buff 的状态经过存档和读档后保持一致；重复施加、战斗结束清理等边界正确。

**审核重点**：buff 表扩容时的指针失效、存档和旁路不一致。

## P5 回合外系统

- 各系统单例和字段的逆向，写入知识库。
- 只读接口，然后逐个开放写操作，覆盖庄园、名册、英雄、库存、任务、地牢、城镇事件。
- 屏幕与状态机。

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

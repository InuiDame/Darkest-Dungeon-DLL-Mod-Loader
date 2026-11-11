# DD1 Modding 调研

项目启动时（2026-11-09）的调研结论。逆向事实以 `../knowledge/` 为准，这里不重复。

## 现有手段

| 层 | 手段 | 能做什么 | 做不到什么 |
| --- | --- | --- | --- |
| 数据层（官方） | 按相对路径覆盖 `.darkest`、`.json`、美术音频 | 英雄、怪物、技能、Effect、Buff、饰品、怪癖、任务、地牢、城镇、奇物、本地化 | 新地点、新 AI 机制；任何没有对应字段的逻辑，比如「受击时触发」 |
| DLL 层（DDModLoader） | 代理 `dbghelp.dll` 加载 DLL，DLL 自行 hook | 任意修改 | 没有公共接口，每个 DLL 各自找地址、各自 hook，互相不协作 |
| 脚本层 | 不存在 | | 游戏没有内嵌脚本引擎 |

数据层的字段和覆盖规则见 `knowledge/dd1/data-and-mods.md`。

## 原版 DDModLoader 的问题

原版是单文件 `dllmain.cpp`：代理 `dbghelp.dll!MiniDumpWriteDump`，`DllMain` 开线程 `Sleep(1500)` 后依次 `LoadLibraryW` 加载 `mods\DD Mod Lib`、其余本地 `mods`、Workshop 和 `extra_mods.cfg` 里的 DLL。P0 把它迁到 `loader/`，旧式 DLL 的加载行为保持不变。

新接口要解决的问题，以及对应的决策：

- **时序不确定**：1.5 秒的等待和主线程竞争，DLL 不知道游戏初始化到哪一步。见 [ADR 0004](adr/0004-early-load-before-createappsystems.md)。
- **没有插件协议**：没有版本协商、依赖声明、阶段回调和插件间通信。见 [ADR 0005](adr/0005-c-abi-service-tables.md)。
- **没有 hook 协调**：两个 DLL hook 同一入口时，后来者的特征码匹配失败，功能静默丢失。见 [ADR 0007](adr/0007-hooks-on-demand.md)。
- **DLL 全局生效，而 Mod 按存档启用**。见 [ADR 0015](adr/0015-scripts-follow-save-mods.md)。

## 现有插件积累的技术

P4 以后要把这些手法做成通用能力：

| 技术 | 例子 | 可复用的结论 |
| --- | --- | --- |
| 特征码加布局校验 | 三个插件都有 | 特征码同时覆盖 Steam 和离线版，布局不符时放弃安装，只写警告 |
| 内联 hook、cave hook | DKUtil，DDShield | 远跳用 trampoline + `AddCaveHook` |
| 按字符串键控扩展 effect | DDShield 的 `shield_gain_37` | hook `EffectApplyInternal`，按名字解析，不改游戏数据结构 |
| 给 JSON 增加自定义字段 | BuffIcon 的 `buff_icon`、DDShield 的 `shield_*` | 插件自己再读一遍数据文件，按 id 哈希关联运行时对象 |
| 自定义规则状态 | DDShield 的 `shielded` | cave hook 接管规则分支 |
| 用游戏自己的函数操作容器 | BuffIcon 的 tray 图标表 | 跨 CRT 的容器扩容要用游戏的函数 |

## 后续阶段的接入点线索

事件总线、Query、战斗节点和 Effect 已在 P2 接入（[07-event-coverage.md](07-event-coverage.md)）。剩下的：

| 需求 | 线索 | 阶段 |
| --- | --- | --- |
| Buff | `Actor+0x460` 的 buff 表，增删函数待定位，`UpdateBuffsInternal` 是线索（`combat.md`） | P4 |
| 回合外 | `Roster::System`、`Estate::System`、`Progression::System` 等系统对象和 `OnRaidStart` 系列回调（`campaign.md`） | P5 |
| Spine | spine-c 2.1.27 静态编入，`SpineSprite` 成员待定位（`spine.md`） | P6 |

## 同类方案参考

- **SKSE / F4SE**：DLL 导出查询和加载函数，加载时拿到版本化的接口表，CommonLibSSE 把 C++ 封装放在 ABI 之上。本项目的 native 接口沿用这个模型。
- **BG3 Script Extender**：DLL 扩展内嵌 Lua，脚本通过事件和 Osiris 调用驱动游戏，是 Lua 层的主要参考。

## 资料来源

- [Darkest Dungeon Modding Guide（官方 Steam 指南）](https://steamcommunity.com/sharedfiles/filedetails/?id=819597757)
- [Darkest Dungeon Wiki: Modder tools](https://darkestdungeon.wiki.gg/wiki/Modder_tools)
- [erri120/DarkestModdingTools](https://github.com/erri120/DarkestModdingTools)
- [Spine 论坛：darkest dungeon modding](https://ar.esotericsoftware.com/forum/d/8939-darkest-dungeon-modding)
- [Steam 讨论：Spine 版本与导入流程](https://steamcommunity.com/app/262060/discussions/4/1727575977545379683)
- [Norbyte/bg3se](https://github.com/Norbyte/bg3se/releases)
- [sol2](https://github.com/ThePhD/sol2)

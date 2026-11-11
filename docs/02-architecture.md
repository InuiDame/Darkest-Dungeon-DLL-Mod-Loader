# 总体架构

选型的理由在 [adr/](adr/)，这里只写结构和机制。

## 分层

```
 Lua Mod (<mod>/lua/main.lua)
────────────────────────────────────────────────
 DDLua.dll     Lua 5.4（按 C++ 编译、打补丁），本身是一个 native 插件，只通过 SDK 调用下层
────────────────────────────────────────────────
 native 插件（新协议）            旧式 DLL（行为不变）
────────────────────────────────────────────────
 DDCore.dll    事件中枢、hook 仲裁、主线程调度、生命周期；后续加游戏对象绑定、存档旁路
────────────────────────────────────────────────
 dbghelp.dll   代理：早期 hook、启动 DDCore、旧式 DLL 的扫描和加载
────────────────────────────────────────────────
 Darkest.exe
```

分层理由见 [ADR 0003](adr/0003-thin-proxy-core-layering.md)。`DDCore` 和 `DDLua` 预装在 `DD Mod Lib`，玩家关不掉，没有订阅时必须保持原版行为。

## 工程结构

沿用 `DD.Plugins` 的形式：根 `xmake.lua` 自动包含子目录，公共规则放 `xmake/`，C++23，`/MT`，默认 releasedbg，DKUtil 走 `DKUtilPath`，构建后按 `dd.mod` 部署。

```
DD.ModLoader/
├─ xmake/          公共规则 dd.native、Plugin.h 模板、版本资源
├─ common/         加载器和 DDCore 共用的头：PE 导出解析、特征码扫描、启动约定
├─ loader/         dbghelp 代理
├─ core/           DDCore.dll（events/ game/ hook/ logic/ plugin/ runtime/）
├─ sdk/include/dd/ 公共头：C ABI 和 header-only 的 C++ 封装
├─ sdk/lua/        Lua Mod 用的 LuaLS 类型声明（dd_topics.lua 由 tools/gen_lua_types.py 生成）
├─ lua/            DDLua.dll：VM、沙箱、预算、按存档的会话、事件绑定、调度器、actor 句柄
├─ templates/      native 插件模板、Lua Mod 模板（templates/lua/）
├─ examples/       hello_native、hello_events、lua/hello_lua
├─ tests/          unit/ 不依赖游戏的单元测试，plugins/ 游戏内自检插件
├─ tools/          rig.py、check_sigs.py、event_coverage.py 等，见 06-workflow.md
├─ spikes/p0/      P0 的验证代码，不在构建中，只作参考
└─ docs/
```

依赖：`DKUtil`、`spdlog`、`nlohmann_json`、`xbyak`、`simpleini`（同 DD.Plugins）。Lua 相关的依赖在 `xmake/lua.lua`：Lua 5.4.9 是工程内的本地包，编成静态库、运行库跟随 `/MT`，按 C++ 编译（`lua_error` 是 C++ 异常，不会跳过析构），安装时按锚点给源码打补丁（`dd_patch`），补丁的内容和理由见 [ADR 0019](adr/0019-lua-as-cpp-with-patches.md)；sol2 3.5.0 只取头文件（`includes_lua = false`），DDLua 实际直接用 Lua C API。`tests/unit/lua_smoke.cpp`（目标 `lua_smoke`）用和 DDLua 相同的 `dd.native` 选项验证版本、绑定、错误捕获、计数 hook 和析构，`dd_lua_tests` 覆盖 DDLua 本身。

## 加载流程和阶段

```
进程启动       代理 DllMain：定位 CreateAppSystems，装早期 hook，启动旧式加载线程（行为不变）
Early          主线程进入 CreateAppSystems 之前：加载 DDCore，扫描并加载新协议插件，回调 Load
AppReady       CreateAppSystems 返回，事件系统注册通用 listener，输出 hook 报告
AssetsReady    LoadAppAssets 完成，本地化和字体可用
CampaignLoad   进入存档：Buff/Effect 库可用，存档读取完成
Tick           每帧在主线程执行，AssetsReady 之前不派发
CampaignUnload 退出存档（回主菜单、切换存档），Campaign 销毁之前
```

所有阶段回调都在主线程。阶段在 `UpdateStateInternal` 入口（`3DCE10`，每帧一次）判定，这个 hook 只在有新协议插件时安装。`CampaignLoad` 的判定是「状态机离开 `as_load_campaign` 且 `Campaign::Current` 非空」，依据见 `knowledge/dd1/runtime.md` 的「状态机」。

代理和 `DDCore` 之间只有 `DDCore_Boot`、`DDCore_AppSystemsCreated` 两个导出（`common/boot_abi.h`）。旧式加载线程跳过新协议 DLL（解析 PE 导出表判断，不运行 DllMain）和 `DDCore.dll` 本身。新协议插件的扫描由 `DDCore` 负责，目录顺序、`DDModLoader.cfg` 启用状态、`DD Mod Lib` 强制和同名去重的语义与旧式路径一致。

## Native ABI

纯 C 接口（[ADR 0005](adr/0005-c-abi-service-tables.md)），定义在 `sdk/include/dd/dd_api.h`，各服务的语义以头文件注释为准。

```c
DD_EXPORT bool DDPlugin_Query(DDPluginInfo* out);   // 名称、版本、需要的 ABI、依赖
DD_EXPORT bool DDPlugin_Load(const DDApi* api);
DD_EXPORT void DDPlugin_Unload(void);               // 可选，暂不热卸载，宿主不调用
```

ABI 版本是 `major.minor`，主版本不同拒绝，插件的次版本高于 `DDCore` 时拒绝。开发期不做前后兼容（[ADR 0018](adr/0018-no-version-compat-until-p7.md)）。

| 服务 | 内容 | 状态 |
| --- | --- | --- |
| `log` | 按插件分文件的日志，放在 `DDModLoader\log\` | P1 完成 |
| `task` | 主线程任务队列、延时 | P1 完成 |
| `lifecycle` | 阶段回调 | P1 完成 |
| `hook` | 仲裁后的 hook 安装，见下节 | P1 完成 |
| `game_info` | 版本、路径 | P1 完成 |
| `events` | 订阅、发布、优先级、作用域、取消 | P2 完成 |
| `game` | 当前存档的启用 Mod 列表和目录；actor 句柄（[ADR 0024](adr/0024-actor-handles-are-values.md)：英雄用 hero id 走名册反查，怪物用 guid 加 DDCore 的战斗序号走当前战斗反查，guid 只在一场战斗内唯一）和 HP、最大生命、压力、职业的读取 | P3 完成，读写随 P4、P5 扩充 |
| `effect`、`buff`、`save` | 查询、施加、定义；存档旁路数据 | P4 |
| `campaign` | 回合外系统 | P5 |
| `spine` | 动画控制 | P6 |
| `message` | 插件间消息 | 未排期 |

## Hook 仲裁

原则见 [ADR 0007](adr/0007-hooks-on-demand.md)：优先虚表 hook，语义 hook 按需安装，内联补丁只在主线程的安全时机修改，冲突时拒绝并写日志。

- 同一目标只打一次补丁，多个调用者按 priority 串成链（DDCore 内部用小跳板实现，移除中间一环不用再动目标函数），插件的 `hook` 服务也合并到这条链上。
- AppReady、AssetsReady 和每次 CampaignLoad 时输出 hook 报告到 `DDCore.log`，并核对补丁是否还在，失效的项写 WARN。
- 现有插件的 hook 清单见 `knowledge/dd1/hooks.md`；和框架冲突的插件直接卸载（[ADR 0017](adr/0017-no-legacy-plugin-compat.md)）。

## 特征码

地址只用特征码，规则见 [ADR 0006](adr/0006-signature-addressing.md)。`signatures.inc` 的写法约定：内联语义 hook 的特征码把入口 steal 字节写成通配（入口可能已被别的 DLL hook），`DD_ENTRY` 给出真实入口字节，`DD_CHAIN_TARGET` 标记 DDCore 内部串联的目标（`check_sigs.py` 不算重叠错误）。

## 主要风险

| 风险 | 对策 |
| --- | --- |
| 游戏更新使特征码失效 | 离线校验，失败时停用，不崩溃（ADR 0006） |
| 脚本改动战斗导致 PvP 不同步 | PvP 中不派发会改结算的订阅（ADR 0011） |
| 脚本错误拖垮游戏 | 回调 SEH 隔离；Lua 全部保护调用，执行预算按真实时间（[ADR 0020](adr/0020-lua-budget-best-effort.md)），原生异常后丢弃 VM |
| 悬垂的游戏对象 | 事件负载只在回调期间有效（[ADR 0023](adr/0023-lua-payload-and-coroutines.md)）；跨回调保存用不带指针的句柄 |
| 跨 CRT 的内存 | 用 `ucrtbase` 的 `malloc/free` 或游戏自己的函数 |
| 杀软拦截 | 不做规避，失败时提示用户 |

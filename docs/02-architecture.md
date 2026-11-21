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
 DDCore.dll    事件中枢、hook 仲裁、主线程调度、生命周期、游戏对象（actor、buff、effect）、存档旁路
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
├─ core/           DDCore.dll（combat/ events/ game/ hook/ logic/ persist/ plugin/ runtime/）
├─ sdk/include/dd/ 公共头：C ABI 和 header-only 的 C++ 封装
├─ sdk/lua/        Lua Mod 用的 LuaLS 类型声明（dd_topics.lua 由 tools/gen_lua_types.py 生成）
├─ lua/            DDLua.dll：VM、沙箱、预算、按存档的会话、事件绑定、调度器、actor 句柄、Buff 与 Effect 的绑定（combat.cpp）
├─ templates/      native 插件模板、Lua Mod 模板（templates/lua/）
├─ examples/       hello_native、hello_events、lua/hello_lua、lua/shield_lite（DDShield 护盾的 Lua 复刻）
├─ tests/          unit/ 不依赖游戏的单元测试，plugins/ 游戏内自检和探针插件，lua/ 测试用和场景用的 Lua Mod
├─ tools/          rig.py、dd_update.py（校验、修复、自测，模块在 ddupdate/，基线在 baseline/）、event_coverage.py 等，见 06-workflow.md
├─ spikes/p0/      P0 的验证代码，不在构建中，只作参考
└─ docs/
```

依赖：`DKUtil`、`spdlog`、`nlohmann_json`、`xbyak`、`simpleini`（同 DD.Plugins）。Lua 相关的依赖在 `xmake/lua.lua`：Lua 5.4.9 是工程内的本地包，编成静态库、运行库跟随 `/MT`，按 C++ 编译（`lua_error` 是 C++ 异常，不会跳过析构），安装时按锚点给源码打补丁（`dd_patch`），补丁的内容和理由见 [ADR 0019](adr/0019-lua-as-cpp-with-patches.md)；sol2 3.5.0 只取头文件（`includes_lua = false`），DDLua 实际直接用 Lua C API。`tests/unit/lua_smoke.cpp`（目标 `lua_smoke`）用和 DDLua 相同的 `dd.native` 选项验证版本、绑定、错误捕获、计数 hook 和析构，`dd_lua_tests` 覆盖 DDLua 本身。

## 加载流程和阶段

```
进程启动       代理 DllMain：定位 CreateAppSystems，装早期 hook，启动旧式加载线程（行为不变）
Early          主线程进入 CreateAppSystems 之前：加载 DDCore，扫描并加载新协议插件，回调 Load
AppReady       CreateAppSystems 返回，事件系统注册通用 listener，有插件声明持久化时装存档观察 hook，输出 hook 报告
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
| `game` | 当前存档的启用 Mod 列表和目录；actor 句柄（[ADR 0024](adr/0024-actor-handles-are-values.md)：英雄用 hero id 走名册反查，怪物用 guid 加 DDCore 的战斗序号走当前战斗反查，guid 只在一场战斗内唯一）和 HP、最大生命、压力、职业的读取；当前队伍、当前战斗的怪物、战斗状态的只读枚举（`party_heroes`、`battle_monsters`、`battle_state`，每个指针按 ADR 0024 核对成员关系才给句柄，P5 的 `dd.raid` 在这上面扩展，[ADR 0034](adr/0034-party-monster-enumeration.md)） | P3 完成，P4 加只读枚举，P5 扩充 |
| `combat` | buff 的读写和标签、定义注册、effect 的查询施加和文本定义、规则扩展、技能句柄，见下节 | P4 完成 |
| `persist` | 存档旁路：插件的数据按存档保存在游戏存档旁边，读档时按内容哈希核对一致才交回，见下节 | P4 完成 |
| `campaign` | 回合外系统 | P5 |
| `spine` | 动画控制 | P6 |
| `message` | 插件间消息 | 未排期 |

## combat 服务

P4 的 Buff、Effect、技能都经 `combat` 服务开放（取舍见 [ADR 0025](adr/0025-combat-write-timing.md)–[0030](adr/0030-damage-final-absorb.md)、[0033](adr/0033-skill-pre-read-only.md)），游戏函数、hook、结构偏移都在 `DDCore`（`core/src/combat/`），DDLua 只走 SDK。不依赖游戏的部分（枚举表、参数校验含 `sub_stat` 的游戏名字表、写操作的时机判断、操作队列、标签表、规则注册表、effect 文本按游戏解析器规则的分块和取名）在 `logic.cpp`，单元测试覆盖；`combat.cpp` 是游戏侧。DDLua 也编译同一份 `logic.cpp`（effect 文本的检查、名字的哈希），两边的判断一致。

- **句柄是值**（沿用 [ADR 0024](adr/0024-actor-handles-are-values.md)，[ADR 0027](adr/0027-script-buff-carrier.md)）：buff 是（actor 句柄，组 guid），effect 是名字，技能是（执行者句柄，技能 id，等级）。读取得到的是快照，每次使用都重新解析。
- **写操作的时机**（[ADR 0025](adr/0025-combat-write-timing.md)）：buff 增删、施加 effect、注册定义只在主线程、存档里、不在加载和 PvP 中进行。帧边界（lifecycle 回调、task 任务、操作回调，并且 DDCore 没有在派发事件、没有在调用游戏函数）立即执行；事件回调里和立即执行时同步派发的嵌套回调里排进队列（每个插件最多 256 个），下一个帧边界按顺序执行，结果经插件注册的操作回调通知；规则回调里一律拒绝。判断依据是 `UpdateStateDetour` 正在执行 `OnFrameBody`、事件中枢的派发深度和 DDCore 自己的游戏调用层数。
- **游戏调用**（定义的取舍见 [ADR 0028](adr/0028-runtime-definitions.md)）：添加是复制库里的定义（`482480`）、改字段（target 一律 0）、经 Hero/Monster 虚表槽位 13 加上、按游戏的写法析构临时 Instance；删除是带兼容 `std::function` 谓词的槽位 14；施加 effect 是 `4CB4D0`（用游戏的 prng，需要 `Raid::Current`）；buff 定义经 Buff 库 map 的 `operator[]`（`4ACFA0`）原生插入并只写 POD 字段；effect 定义写成 `DDModLoader\tmp\` 下的临时文件交给游戏的解析器 `4F02D0`，之前按游戏的规则（`2901B0` 去注释、`2904D0` 分块、`36F020` 取字段）检查文本：块达到 1024 字节、列表项从块末尾的引号开始会让 CRT 直接终止进程，名字按游戏实际会用的 `.name` 核对归属（P4 审核 A-B1、A-S1）。Buff 库和 effect map 每次进存档重建，所以运行时定义按存档注册，DDCore 只记「哪个插件定义了哪个名字」，进出存档时清空；`CampaignLoad` 时已经在表里的名字算游戏数据，不能覆盖。
- **标签**：（actor，guid）→（插件，标签）记在 DDCore 里，经 persist 服务随存档旁路保存（见下节）。组 guid 在一个 actor 上只增不减，所以不装 hook，查询时发现条目不在了就删掉记录；战斗或探索换了时清掉不再能解析的 actor 的记录，退出存档时全部清空。
- **读取的时机**：buff、Buff 库、effect 的读取只在主线程、没有加载线程在跑时进行，否则返回 false（加载线程会前移、搬动 buff 表，审核 A-S3）。`read_ready` 报告现在能不能读，插件用它区分「加载中」和「真的没有」（R2A-S1，DDLua 据此返回以「加载中」开头的原因）。
- **主题**（[ADR 0026](adr/0026-buff-topics-on-container-ops.md)、[0030](adr/0030-damage-final-absorb.md)）：`buff.add`（`5C4F50` 出口）、`buff.remove`（`5C4FF0` 入口，原因按调用栈里最内层认得的游戏函数判断；`buff_remove` 只把谓词命中的条目算 `plugin`）、`buff.tick`（`47AAD0` 入口和出口对比，原函数里减到 0 被删的条目在删除前补派发）、`damage.final`（`480400`，攻击者在槽位 50 记下；部分吸收按调用者区分英雄和怪物的扣血规则，怪物的返回点和免疫表由特征码 `MonsterBlockReturn` 定位），都按需安装。只在存档里派发，进存档之后加载线程上的按 [ADR 0009](adr/0009-off-thread-events-deferred.md) 延迟派发。`effect.*` 加了 `effect_name`，`skill.pre` 改成只读。
- **规则扩展**（[ADR 0029](adr/0029-rule-extension-entry-hook.md)）：`479CD0` 入口 hook，有规则注册时才装。`rule_type` 是 13/42 且 `rule_data.string` 的哈希命中注册表时调用回调，回调返回 `DD_RULE_TRUE` / `DD_RULE_FALSE` 时 DDCore 按 `is_false_rule` 取反；回调返回 `DD_RULE_ORIGINAL`（出错、超时、停用中）、不在主线程、PvP、回调崩溃过时转原函数，和没有注册时一样。游戏内置的状态名（`stunned` 等 8 个，规则 13/42 自己判断的）不能注册。命中次数、耗时和单帧峰值有统计（`rule_stats`），退出存档时写进 `DDCore.log`。
- 依赖的特征码缺失时只有对应的能力不可用（返回 `DD_COMBAT_E_UNAVAILABLE` 或主题不可用）。结构偏移登记在布局清单里，离线由 `dd_update.py verify` 核对，运行时由 DDCore 核对，不成立时按依赖表停用（见下面「地址、布局清单和校验」）。

外部模块的 hook 经 `events::System::AddHook` 挂到事件系统的同一套按需装卸里（订阅数或额外条件），和语义 hook 一样只在帧边界、避开加载线程修改。

DDLua 这一侧（`lua/src/combat.cpp`）：所有 Lua Mod 共用 DDLua 这一个插件身份，所以 DDLua 自己管 Mod 之间的边界（标签加 `<mod id>:` 前缀、定义和 effect 名字按 Mod 记归属、每个 Mod 最多排 128 个写操作），按操作号把排队操作的结果路由回会话，按 buff id 过滤 `buff.*` 后调用脚本 Buff 的回调，规则回调走单独的预算；每个 Mod 每帧的规则和脚本 Buff 回调合计受 `combat_frame_ms` 约束，脚本 Buff 的状态条数受 `max_per_mod` 约束（审核 C-Y7）。接口和语义见 [03-lua-system.md](03-lua-system.md) 的需求三。

## persist 服务

设计取舍见 [ADR 0031](adr/0031-persist-sidecar-content-hash.md)，依据见 `knowledge/dd1/campaign.md` 的「存档」。不依赖游戏的部分（内容哈希、旁路文件的格式、一致性判断、后台写盘、backup 同步、标签表的序列化）在 `core/src/persist/store.cpp`，单元测试覆盖；`persist.cpp` 是游戏侧。

- **观察 hook**：有插件在 `Load` 里 `declare`（DDLua 总是声明）时，DDCore 在 AppReady 装 4 个：StorageManager 的排队写文件（`2487E0` 入口，所有二进制存档写入都经过它，主线程和加载线程上都有）、读文件（`248D60`，调用原函数之后看读到的内容）、前端选存档槽时复制 backup（`2923A0` 之后）、退到桌面（`3D1CC0` 入口）。它们只读内存、算 FNV-1a 64、加锁记录，不改游戏行为；没有插件声明时一个都不装，`campaign.save` 主题不可用。存档的读取早于 `CampaignLoad`，所以这几个 hook 不能按需安装，`lua` 方案的 hook 报告因此是 1 + 4 个目标。
- **记录**：按（设备，文件名）记每个 `persist.*.json` 最后一次写入或读取的内容哈希和大小（同名文件会写到 `steam_profile_N` 和联机的 `steam_profile_9` 两个设备）。`2487E0` 的大小（第 6 个参数）和 `248D60` 的容量、返回值都是 32 位，栈上传的参数调用方只写低 32 位，detour 一律截成 32 位再用（P4 审核 B 的阻断 B1：原来按 64 位读，每次写入都被当成超过 64 MB 跳过）。当前 profile 是槽 5 的设备（`[[StorageManager]+0x38]+0x20`，前端选槽时 `App+1` 还没更新，所以不用它），账号是 SteamID32（`steam_api64` 的 `SteamAPI_SteamUser_vNNN` + `SteamAPI_ISteamUser_GetSteamID`），离线版、没有 `steam_api64.dll`、取不到 SteamID 时是 `local`。
- **`campaign.save`**：当前 profile 上写了任何 `persist.*.json` 时记下这一组（城镇里升级建筑、城镇事件只写庄园、城镇文件，之后改的 Lua 状态也要进旁路；一致性仍只比较名册和 raid），下一个帧边界（没有加载线程在跑时；加载线程上写的等加载结束）派发，同一帧写的几个文件只派发一次，`CampaignLoad` 时丢掉读档过程中的写入。派发放在帧边界逻辑的最前面（任务、延迟事件、定时器之前），快照和刚写的存档对应（探针 0B-13：写存档到帧边界之间没有 buff 增删、持续时间结算、技能执行）。退出存档前、退到桌面前还有没派发的先派发（退到桌面时在加载中就不派发，旁路落后一次，下次读档判不一致）。派发完订阅以后按插件加载顺序调用各插件的 `on_save`，插件在里面 `save`；这期间 combat 的写操作按事件回调处理（排队）。没有旁路要写的派发只在每次进存档的前 3 次写日志。
- **写**：DDCore 在主线程把插件的数据和当时的名册（地牢里加 raid）哈希拼成旁路文件：文本头（格式版本、插件名、key、插件给的版本、设备、账号、写入时间、是否在地牢和战斗中、`file <名字> <哈希> <大小>`、`payload <大小> <哈希>`）加数据。路径 `<游戏根目录>\DDModLoader\save\<账号>\<设备>\<插件名>\<key>.ddsave`，插件名不能以点结尾、不能是 `backup`、`DDCore`，和别的插件、同一插件的 key 之间都不能只差大小写。后台线程写盘：同一路径还没写的被新的替换（合并），先清掉 `.tmp`、`.prev`、当前文件的只读属性，写 `.tmp`、`FlushFileBuffers`，再 `ReplaceFileW`（原文件成为 `.prev`），失败时退回两次 `MoveFileExW`；整次失败按 200、500、1000、2000 毫秒退避重试，第 5 次还失败才放弃（ERROR，同一文件连续放弃时每 20 次写一行）。退出存档、选存档槽时等后台写完（3 秒），退到桌面时等 10 秒；「已写完」只在队列空了而且没有放弃的文件时写，否则 WARN 列出路径。
- **读**：`CampaignLoad` 时取这次读档读到的名册（只认上次退出存档以来读到的；路径带 `backup/` 时旁路也读 `backup\` 目录）、「存档在地牢里」（状态机的下一个状态是 `as_raid_start`）、地牢存档在状态 1 由 `3DBC20` 读到的 raid、是不是新战役（名册和 `scripts\starting_save\persist.roster.json` 内容相同：`3DB3B0` 用 `2493E0` 原样复制 starting_save）。插件 `load` 时读当前一代和 `.prev`，按文件头的插件名和 key、设备、名册哈希和大小、城镇/地牢核对，地牢里知道 raid 时再比 raid：当前一致是 LOADED，否则上一代一致是 PREV，都不一致（含写到一半、校验和不符的文件）是 MISMATCH（新战役时是 EMPTY），都没有是 EMPTY，没观察到名册读取等是 UNAVAILABLE。地牢里的存档照样带 `raid_pending`：`as_raid_start` 加载结束后的帧边界拿 `load_raid` 读到的 raid 再确认一次，结果经 `on_verify` 通知，这之后才恢复怪物的标签和 Lua 状态；这一步不一致时数据已经交出去了，这次进存档不再写这个 key（`save` 返回 `DD_PERSIST_E_INCONSISTENT`），免得下一次存档把它按新哈希写回去。不自动修复；判成 MISMATCH（或新战役）的 key 在这次进存档第一次写之前，原来的两代改名为 `<key>.ddsave.mismatch-<时间>`、`<key>.ddsave.prev.mismatch-<时间>`（新战役是 `oldcampaign-<时间>`），每种留最近 2 份，不再参与读档判断，也不会被之后的写入轮换掉。
- **backup**：`2923A0` 之后把这个设备目录下各插件的 `*.ddsave`、`*.ddsave.prev` 同步到 `<设备>\backup\`（多出来的删掉，归档的 `*.mismatch-*`、`*.oldcampaign-*` 不同步）。游戏的 `2923A0` 只在根目录的文件都通过检查时才复制，DDCore 看不到它的结果，总是同步；没复制的那次读 backup 时旁路会判成不一致（安全的一侧）。
- **DDCore 的标签表**：combat 的标签（（actor，guid）→（插件，标签））作为 `DDCore\tags.ddsave` 随存档保存（有标签或者这次进存档读到过、写过时才写），读档后英雄的在 `CampaignLoad` 恢复，怪物的等 raid 核对通过后恢复，条目不在、id 对不上、插件没有加载的丢弃。只有观察 hook 装上时（有插件声明持久化）才保存。

## Hook 仲裁

原则见 [ADR 0007](adr/0007-hooks-on-demand.md)：优先虚表 hook，语义 hook 按需安装，内联补丁只在主线程的安全时机修改，冲突时拒绝并写日志。

- 同一目标只打一次补丁，多个调用者按 priority 串成链（DDCore 内部用小跳板实现，移除中间一环不用再动目标函数），插件的 `hook` 服务也合并到这条链上。
- AppReady、AssetsReady 和每次 CampaignLoad 时输出 hook 报告到 `DDCore.log`，并核对补丁是否还在，失效的项写 WARN。
- 现有插件的 hook 清单见 `knowledge/dd1/hooks.md`；和框架冲突的插件直接卸载（[ADR 0017](adr/0017-no-legacy-plugin-compat.md)）。

## 地址、布局清单和校验

地址只用特征码，规则见 [ADR 0006](adr/0006-signature-addressing.md)；地址之外的游戏布局和整条更新流水线见 [ADR 0035](adr/0035-update-pipeline.md)，运行时核对和停用见 [ADR 0036](adr/0036-runtime-layout-check.md)。

- **布局清单**：`core/src/game/signatures.inc`（地址）和 `layout.inc`（结构偏移和大小、虚表槽位、枚举表、事件和 Query 负载偏移、命中位置上的固定偏移）是唯一来源，加载器、DDCore、`tools/dd_update.py` 都读它们，C++ 经 `layout.h` 取常量（`layout::k<名字>`）。每一项带依赖表（失效时影响哪些服务和主题）和依据：运行时可求值的 `DD_EVIDENCE`、只能离线求值的 `DD_OFFLINE`，或者 `DD_UNVERIFIED` 写明怎么核实。格式写在两个文件开头。
- **`signatures.inc` 的写法约定**：内联语义 hook 的特征码把入口 steal 字节写成通配（入口可能已被别的 DLL hook），`DD_ENTRY` 给出真实入口字节，`DD_CHAIN_TARGET` 标记 DDCore 内部串联的目标（校验时不算重叠错误），`ANCHOR` 是只给依据当锚点的特征码。
- **运行时核对**（`core/src/game/layout_check.cpp`）：`game::Resolve` 扫描特征码之后、装任何 hook 之前，对全部运行时依据求值（自己解析异常目录，语义和 `tools/ddupdate` 的 Python 实现相同，`tests/layout_compare` 的对照测试逐项比较）。约 60 ms 扫描加 13 ms 求值。
- **停用**：不成立的项按依赖表停用读写能力和主题；未知版本上只能离线核对、没有静态依据的项按「不能核对」处理，停用写能力和可写的主题，只读的照常；依赖表里有 `core` 的必需项落到停用时 DDCore 整体停用。开关是 `game::Addresses::gate`，服务入口查 `Gated(...)`，主题经 `Hub::Gate` 标成不可用。特征码的同一性依据不成立时这条特征码不给地址。
- **离线校验和修复**（`tools/dd_update.py`，[06-workflow.md](06-workflow.md) 的「游戏更新后」）：`verify` 在运行时核对之外还做要反汇编或要基线的检查：HOOK 偷取范围和入口、命中的函数是不是基线里的同一个（同一性）、依据唯一命中的位置是不是基线里那条指令（依据位置核对，没法对照又没有佐证时记为无法确认、不算通过）、同组（`DD_GROUP`）有成员变了时列出组里要人工核实的项；`repair` 定位失效项、给出补丁和逐项可信度，`accept` 把新版本记为已知、生成基线（只放行运行时自己会发现的失效，NONE 项、同组升级的项、无法确认的项要逐项确认，确认记录进仓库）；`selftest` 用盲定位和变异样本自测。

## 主要风险

| 风险 | 对策 |
| --- | --- |
| 游戏更新使特征码或结构偏移失效 | 离线校验和修复（ADR 0035）；运行时核对布局清单，失败时按依赖表停用，不崩溃、不读写错位置（ADR 0006、0036） |
| 脚本改动战斗导致 PvP 不同步 | PvP 中不派发会改结算的订阅（ADR 0011） |
| 脚本错误拖垮游戏 | 回调 SEH 隔离；Lua 全部保护调用，执行预算按真实时间（[ADR 0020](adr/0020-lua-budget-best-effort.md)），原生异常后丢弃 VM |
| 悬垂的游戏对象 | 事件负载只在回调期间有效（[ADR 0023](adr/0023-lua-payload-and-coroutines.md)）；跨回调保存用不带指针的句柄 |
| 跨 CRT 的内存 | 用 `ucrtbase` 的 `malloc/free` 或游戏自己的函数 |
| 杀软拦截 | 不做规避，失败时提示用户 |

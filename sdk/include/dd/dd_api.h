/*
 * DD.ModLoader native 插件 ABI（纯 C）。
 *
 * 约定：
 * - 开发期（最终阶段验收前）不做前后兼容，结构、函数表和版本规则都可能直接修改，
 *   插件要和 DDCore 一起重新构建。公开规范在 P7 敲定。
 * - 服务按名字和版本查询。
 * - 所有结构体以 size 开头。
 * - 跨边界的字符串都是 UTF-8。除非另有说明，核心返回的字符串在进程生命周期内有效。
 * - STL 对象不跨边界：插件和 DDCore 各用自己的静态 CRT。
 * - 插件导出缺任一必须导出（Query、Load）时按旧式 DLL 处理。
 */
#ifndef DD_API_H
#define DD_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DD_EXPORT __declspec(dllexport)
#define DD_CALL __cdecl

/* ------------------------------------------------------------------ ABI 版本 */

/* major 不同直接拒绝，插件的 minor 高于 DDCore 时拒绝。 */
#define DD_ABI_MAJOR 1u
#define DD_ABI_MINOR 0u
#define DD_ABI_MAKE(major, minor) (((uint32_t)(major) << 16) | (uint32_t)(minor))
#define DD_ABI_VERSION DD_ABI_MAKE(DD_ABI_MAJOR, DD_ABI_MINOR)
#define DD_ABI_GET_MAJOR(v) ((uint32_t)(v) >> 16)
#define DD_ABI_GET_MINOR(v) ((uint32_t)(v) & 0xFFFFu)

/* 把主、次、修订号合成一个可比较的版本号，用于 DDPluginInfo.version 和依赖 */
#define DD_VERSION_MAKE(major, minor, patch) \
    (((uint32_t)(major) << 24) | ((uint32_t)(minor) << 12) | (uint32_t)(patch))

/* 插件的不透明身份，每个服务函数的第一个参数。无效的句柄一律返回错误。 */
typedef struct DDPluginHandle_* DDPluginHandle;

/* ------------------------------------------------------------------ 插件导出 */

#define DD_PLUGIN_MAX_NAME 63

/* 依赖项。name 必须在 DDPluginInfo.deps 的生命周期内有效。 */
typedef struct DDPluginDep {
    const char* name;
    uint32_t min_version; /* 依赖的最低版本，0 表示不限 */
    uint32_t reserved;    /* 必须为 0，非 0 时按 ABI 不匹配拒绝 */
} DDPluginDep;

/*
 * Query 的输出。调用前核心把缓冲区清零并写入 size（核心认识的结构大小），
 * 插件填写其余字段，abi_version 必须写成自己编译时的 DD_ABI_VERSION，留空时拒绝加载。
 * 核心提供的缓冲区足够容纳未来追加的字段，插件只写自己认识的字段。
 * 指针字段指向的内容要在进程生命周期内有效（通常是字符串常量）。
 */
typedef struct DDPluginInfo {
    uint32_t size;             /* 入参：核心的结构大小 */
    uint32_t abi_version;      /* 插件写入编译时的 DD_ABI_VERSION */
    const char* name;          /* 插件唯一名，字母数字下划线点横线，1..DD_PLUGIN_MAX_NAME 字节 */
    uint32_t version;          /* DD_VERSION_MAKE 生成 */
    uint32_t dep_count;        /* deps 的长度，无依赖时为 0 */
    const DDPluginDep* deps;
} DDPluginInfo;

typedef struct DDApi {
    uint32_t size;        /* 核心写入：结构大小 */
    uint32_t abi_version; /* 核心写入：核心的 ABI */
    /* 按名字查询服务，返回版本不低于 min_version 的函数表，未实现或版本过高返回 NULL。
       返回的表在进程生命周期内有效，可以缓存。 */
    const void*(DD_CALL* query)(const char* service, uint32_t min_version);
    DDPluginHandle plugin; /* 本插件的身份，传给各服务函数 */
} DDApi;

/* Query 只填写信息，不要做别的事。Load 在主线程执行，早于 CreateAppSystems。 */
DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out);
DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api);
/* 可选。P1 不做热卸载，宿主目前不会调用它；Load 失败的插件也不调用。 */
DD_EXPORT void DD_CALL DDPlugin_Unload(void);

/* 服务表公共头 */
typedef struct DDServiceHeader {
    uint32_t size;    /* 函数表的大小，判断可选字段是否存在用 */
    uint32_t version; /* 表的版本 */
} DDServiceHeader;

/* ------------------------------------------------------------------ log */

#define DD_SERVICE_LOG "log"
#define DD_SERVICE_LOG_VERSION 1u

typedef enum DDLogLevel {
    DD_LOG_TRACE = 0,
    DD_LOG_DEBUG = 1,
    DD_LOG_INFO = 2,
    DD_LOG_WARN = 3,
    DD_LOG_ERROR = 4,
} DDLogLevel;

typedef struct DDLogService {
    DDServiceHeader header;
    /* 写入 <游戏根目录>\DDModLoader\log\<插件名>.log（每次启动覆盖），线程安全。 */
    void(DD_CALL* write)(DDPluginHandle plugin, int level, const char* message);
} DDLogService;

/* ------------------------------------------------------------------ task */

#define DD_SERVICE_TASK "task"
#define DD_SERVICE_TASK_VERSION 1u

typedef uint64_t DDTaskId; /* 0 表示失败 */
typedef void(DD_CALL* DDTaskFn)(void* user);

typedef struct DDTaskService {
    DDServiceHeader header;
    /* 排到主线程执行，线程安全。任务从第一帧开始执行，可能早于 ASSETS_READY；同一帧内按提交顺序，
       任务里再提交的任务下一帧才执行。回调崩溃会被捕获并写日志，同一插件后续任务不受影响。 */
    DDTaskId(DD_CALL* post)(DDPluginHandle plugin, DDTaskFn fn, void* user);
    /* 延时不短于 delay_ms 毫秒，到点后的下一帧在主线程执行 */
    DDTaskId(DD_CALL* post_delayed)(DDPluginHandle plugin, DDTaskFn fn, void* user, uint32_t delay_ms);
    /* 取消尚未开始的任务，返回是否取消成功 */
    bool(DD_CALL* cancel)(DDPluginHandle plugin, DDTaskId id);
    bool(DD_CALL* is_main_thread)(void);
} DDTaskService;

/* ------------------------------------------------------------------ lifecycle */

#define DD_SERVICE_LIFECYCLE "lifecycle"
#define DD_SERVICE_LIFECYCLE_VERSION 1u

/*
 * 阶段，全部在主线程回调，同一阶段内按注册顺序（即插件加载顺序）。
 *
 * EARLY         全部插件 Load 完成后，CreateAppSystems 之前
 * APP_READY     CreateAppSystems 返回后
 * ASSETS_READY  LoadAppAssets 完成后
 * CAMPAIGN_LOAD 进入存档：Buff/Effect 库已加载，存档已读完，详见 knowledge/dd1/runtime.md
 * TICK          每帧。ASSETS_READY 之前不派发，保证 TICK 之前 ASSETS_READY 已经发生
 * CAMPAIGN_UNLOAD 退出存档（回主菜单、切换存档），正常路径在 Campaign 对象销毁之前回调；
 *               直接退出游戏（退到桌面）时不保证回调
 *
 * 阶段已过之后才注册的语义：
 * - EARLY、APP_READY、ASSETS_READY 只发生一次。已过之后注册，回调仍会被调用一次，
 *   event->late 为 1，时机是下一帧主线程。
 * - CAMPAIGN_LOAD：注册时若存档已载入，补调一次（late=1）；否则等下一次进入存档。
 * - CAMPAIGN_UNLOAD：不补调。
 * - TICK：从下一帧开始。
 */
typedef enum DDPhase {
    DD_PHASE_EARLY = 0,
    DD_PHASE_APP_READY = 1,
    DD_PHASE_ASSETS_READY = 2,
    DD_PHASE_CAMPAIGN_LOAD = 3,
    DD_PHASE_TICK = 4,
    DD_PHASE_CAMPAIGN_UNLOAD = 5,
    DD_PHASE_COUNT = 6,
} DDPhase;

typedef struct DDPhaseEvent {
    uint32_t size;
    uint32_t phase;           /* DDPhase */
    uint64_t frame;           /* DDCore 的帧计数 */
    uint32_t campaign_serial; /* 第几次进入存档，从 1 开始，进存档前为 0 */
    uint32_t late;            /* 1 表示补调 */
} DDPhaseEvent;

typedef uint64_t DDSubscriptionId; /* 0 表示失败 */
typedef void(DD_CALL* DDPhaseFn)(const DDPhaseEvent* event, void* user);

typedef struct DDLifecycleService {
    DDServiceHeader header;
    /* 线程安全。回调里可以 subscribe/unsubscribe，对正在派发的本阶段生效：
       取消的不再被调用，新注册的下次才派发。回调崩溃时该订阅被自动取消并写日志。 */
    DDSubscriptionId(DD_CALL* subscribe)(DDPluginHandle plugin, DDPhase phase, DDPhaseFn fn, void* user);
    bool(DD_CALL* unsubscribe)(DDPluginHandle plugin, DDSubscriptionId id);
    /* 已到达的最靠后的一次性阶段（DDPhase，从未到达时为 DD_PHASE_COUNT） */
    uint32_t(DD_CALL* current_phase)(void);
    bool(DD_CALL* campaign_active)(void);
} DDLifecycleService;

/* ------------------------------------------------------------------ hook */

#define DD_SERVICE_HOOK "hook"
#define DD_SERVICE_HOOK_VERSION 1u

typedef uint64_t DDHookHandle; /* 0 表示无效 */

typedef enum DDHookKind {
    DD_HOOK_INLINE = 0, /* 函数入口内联 hook */
    DD_HOOK_VTABLE = 1, /* 虚表槽位，优先使用，不碰函数体 */
} DDHookKind;

/*
 * 优先使用虚表 hook。
 *
 * 内联 hook：target 是函数入口，expected/expected_len 是入口的期望字节，
 * 长度至少覆盖 5 个字节且结束在指令边界上。字节比对不符时拒绝：
 *   入口已是跳转         -> DD_HOOK_E_ALREADY_HOOKED（别的插件先改了，不破坏对方）
 *   其他差异             -> DD_HOOK_E_EXPECTED_MISMATCH
 *   含 rip 相对或分支指令，或要搬走的指令超过 16 字节 -> DD_HOOK_E_UNSAFE_PROLOGUE
 * 同一目标只打一次补丁，多个调用者按 priority 升序串成链（同优先级按注册先后），
 * priority 小的先被调用，每个 detour 通过 *original 调用链上的下一环。
 *
 * 内联补丁的写入不是原子的。在 DDPlugin_Load 或 EARLY 阶段安装和移除内联 hook；
 * 之后的阶段只能 hook 确定没有其他线程正在执行的函数。
 *
 * 注意：DDCore 和旧式 DLL 的加载先后不固定（Steam 版旧式 DLL 通常更早，离线版相反）。
 * 目标已被它们 hook 时安装会失败并返回 DD_HOOK_E_ALREADY_HOOKED；反过来，如果插件抢先
 * 内联 hook 了 DDShield、buff_icon、SpineCompat 等的目标，它们之后的特征码就会匹配失败。
 * 这些目标的清单见 knowledge/dd1/hooks.md。
 *
 * 虚表 hook：target 是虚表槽位的地址（void**），expected_value 非 NULL 时要求槽位当前值
 * 与之相等，否则 DD_HOOK_E_EXPECTED_MISMATCH。expected/expected_len 被忽略。
 */
typedef struct DDHookSpec {
    uint32_t size;           /* sizeof(DDHookSpec) */
    uint32_t kind;           /* DDHookKind */
    const char* name;        /* 诊断名，进日志和 hook 报告 */
    void* target;
    const uint8_t* expected; /* 内联：入口期望字节 */
    uint32_t expected_len;   /* 内联：>= 5 */
    int32_t priority;
    void* expected_value;    /* 虚表：期望的当前值，NULL 表示不校验 */
    void* detour;
    void** original;         /* 输出：调用链上下一环的函数指针 */
} DDHookSpec;

typedef enum DDHookResult {
    DD_HOOK_OK = 0,
    DD_HOOK_E_BAD_ARGUMENT = 1,
    DD_HOOK_E_EXPECTED_MISMATCH = 2,
    DD_HOOK_E_ALREADY_HOOKED = 3,
    DD_HOOK_E_UNSAFE_PROLOGUE = 4,
    DD_HOOK_E_NO_MEMORY = 5,
    DD_HOOK_E_PROTECT_FAILED = 6,
    DD_HOOK_E_NOT_FOUND = 7,
    DD_HOOK_E_NOT_OWNER = 8,
    DD_HOOK_E_BAD_TIMING = 9, /* 不在主线程，或加载线程正在运行 */
} DDHookResult;

typedef struct DDHookService {
    DDServiceHeader header;
    /* 成功和失败都会写入 DDCore.log 和插件日志，失败不做任何修改。
       spec 里的指针不可访问时返回 DD_HOOK_E_BAD_ARGUMENT */
    DDHookResult(DD_CALL* install)(DDPluginHandle plugin, const DDHookSpec* spec, DDHookHandle* out_handle);
    /* 移除后链上其余环不受影响。目标被他人再次覆盖时保留直通，不恢复字节。
       内联 hook 的 install 和所有 remove 只能在主线程调用，读档、进地牢、进城的加载过程中
       返回 DD_HOOK_E_BAD_TIMING，要在加载结束后重试。 */
    DDHookResult(DD_CALL* remove)(DDPluginHandle plugin, DDHookHandle handle);
    const char*(DD_CALL* result_text)(DDHookResult result);
    /* 在 Darkest.exe 的 .text 中查找特征码（格式 "48 89 ?? 24"），要求唯一命中。
       扫描时 DDCore 自己打过补丁的入口按原始字节匹配，先加载的插件 hook 过的函数仍能被后加载的
       插件找到（其他代码打的补丁不还原，匹配不到时返回 false）。 */
    bool(DD_CALL* scan)(const char* pattern, void** out_address);
} DDHookService;

/* ------------------------------------------------------------------ game_info */

#define DD_SERVICE_GAME_INFO "game_info"
#define DD_SERVICE_GAME_INFO_VERSION 1u

typedef struct DDGameInfoService {
    DDServiceHeader header;
    const char*(DD_CALL* core_version)(void); /* DDCore 版本 "1.0.0" */
    const char*(DD_CALL* exe_sha1)(void);     /* 小写十六进制 */
    const char*(DD_CALL* build_label)(void);  /* "steam"、"nosteam"，未知版本为 "unknown" */
    const char*(DD_CALL* svn_revision)(void); /* 游戏根目录 svn_revision.txt 的内容（十六进制文本），没有时为空串 */
    const char*(DD_CALL* game_root)(void);    /* 游戏根目录 */
    const char*(DD_CALL* exe_dir)(void);      /* Darkest.exe 所在目录 */
    const char*(DD_CALL* mods_dir)(void);     /* <游戏根目录>\mods */
    const char*(DD_CALL* log_dir)(void);      /* <游戏根目录>\DDModLoader\log */
    const char*(DD_CALL* plugin_path)(DDPluginHandle plugin); /* 插件 DLL 的完整路径 */
    void*(DD_CALL* image_base)(void);         /* Darkest.exe 的加载基址 */
    uint64_t(DD_CALL* image_size)(void);      /* 映像大小 */
} DDGameInfoService;

/* ------------------------------------------------------------------ events */

#define DD_SERVICE_EVENTS "events"
#define DD_SERVICE_EVENTS_VERSION 1u

/*
 * 事件系统。详细的主题清单和字段见 docs/07-event-coverage.md，运行时用 topic_* 函数查询。
 *
 * 主题分三类：
 *   native.<类名>  游戏事件总线（Event），只读
 *   query.<类名>   游戏询问（Query），可写结果，回调不可取消
 *   语义主题       damage.pre/post/final、effect.pre/post、skill.pre/post、hit.pre/post、
 *                  battle.round.start/end、battle.turn.start/end、stress.pre/post、heal.pre/post、
 *                  buff.add/remove/tick、battle.start/end、actor.*、raid.*、town.*、hero.*、inventory.*、
 *                  estate.currency、app.tick、app.state、campaign.load/unload/save，以及插件自定义主题
 *
 * 负载 DDEvent* 是不透明指针，只在回调期间有效，不能保存。游戏对象以指针形式暴露，
 * 有效期同样只到回调结束；要跨回调保存 Hero、Monster，用 game 服务的 actor_from_pointer 转成句柄。
 *
 * 派发语义：
 *   - 派发遍历快照，按 priority 升序，同优先级按注册先后；
 *   - 派发中新增的订阅下一次才生效，派发中取消的订阅立即不再被调用；
 *   - 回调在 SEH 隔离下运行，崩溃后取消该订阅并写日志；
 *   - 所有回调都在主线程。非主线程派发的原生事件按字段复制后排队到主线程，
 *     负载带 DD_EVENT_F_DEFERRED，只读，不可取消，指针字段读出来恒为 NULL；
 *   - 嵌套派发计深度，超过上限（DD_EVENT_MAX_DEPTH）时写错误日志并停止该次派发。
 *
 * 只有名字以 .pre 结尾的主题、query.* 和 damage.final 可写（event_flags 带 DD_EVENT_F_WRITABLE）。
 * 对只读字段写入返回 DD_EV_E_READ_ONLY，.pre 以外的主题调用 cancel 返回 DD_EV_E_NOT_CANCELLABLE。
 * 内置主题的数字字段在游戏里是 float、int32 或 uint32：写入的值超出这个宽度（float 的 nan、inf 和绝对值
 * 超过 FLT_MAX 的数，int32 / uint32 范围以外的整数）返回 DD_EV_E_RANGE，不写入。自定义主题按 int64 / double 保存，不限制。
 * PvP 对局中（ADR 0011）.pre 和 query.* 的订阅不派发，.post 和只读主题照常。
 *
 * hook 按需安装，订阅后下一帧生效。内联类的主题（effect、skill、hit、battle.round、battle.turn）
 * 在加载线程运行期间顺延到加载结束。
 *
 * cancel 的含义按主题不同，定义见下面各主题的注释：
 *   damage.pre   调用原函数时把伤害改成 0，不扣血、不进死门，
 *                Timeline 仍由游戏创建（调用者会用，绝不返回空或垃圾）；界面可能显示 0。
 *   effect.pre   跳过这一个目标的 Effect 应用，EffectApplyResult 保持调用者初始化的状态。
 *   其余 .pre    不支持 cancel，写字段即可（hit.pre 的 hit=false 走未命中路径）。
 *
 * effect.* 的 performer 和 target 是实际生效的双方：部分 Effect 由游戏交换执行者和目标，
 * 负载里已经按交换后的结果给出，压力和 HP 字段也读实际目标。effect_name 是 effect 的名字。
 *
 * skill.pre / skill.post 只读（skill.pre 的 hook 点在游戏分派之后，执行者已经记下原技能、即时 buff 已经施加，
 * 改技能和目标会和游戏状态对不上），主题和事件都不带可写标志（DD_EVENT_F_WRITABLE）。skill 字段经 combat.skill_from_pointer(performer, skill) 转成技能句柄。
 *
 * damage.final：游戏算完护体、格挡判定之后、扣血之前（Actor::ConsumeHealthDamageBlock，480400），每次命中一次。
 *   字段 target、attacker、damage（最终伤害，只读）、damage_type、blocked（游戏自己是否已经格挡，只读）、
 *   deaths_door（目标在死门上，只读）、hp_before、is_hero，可写的是 absorb（吸收量，0 到 damage）：
 *   全部吸收按格挡处理（不扣血，游戏显示 Blocked）；部分吸收时把吸收的部分先补回生命，净损失 = damage - absorb，
 *   死门按补回后的生命判定。blocked 为真时 absorb 不起作用；目标已经在死门上或已死时只有全部吸收有效。
 *   怪物（5FC3E0）的扣血规则和英雄不同（审核 A-S2）：.can_be_damaged_directly 为假的怪物（cauldron_empty、bulrush 等）
 *   对伤害类型 0–7、9、10、13–19 免疫，这时 damage 字段是 0、absorb 不起作用、游戏照常显示 immune；其余情况游戏一律扣血
 *   （不看死门、已死），所以部分吸收照样补回，只是 HP 已经不大于 0（被 .kill 之类的 effect 杀死）时不补。英雄型怪物
 *   （从 heroes 目录加载的）按英雄的规则。定位怪物路径的特征码 MonsterBlockReturn 缺失时，怪物的部分吸收不起作用。
 *   怪物路径按 480400 的返回地址认（审核 R2A-N1）：别的 native 插件经 hook 服务在 480400 上串链、排在 DDCore 前面
 *   （priority 更小，或相同 priority 先注册）时，DDCore 的 detour 从插件里进入，认不出怪物，按英雄的规则补回生命。
 *   要和 damage.final 一起用的插件不要在 480400 上挂 priority ≤ 0 的 hook。
 *   伤害数字的弹字仍显示 damage。PvP 对局中不派发。attacker 只在伤害经 Hero/Monster 虚表槽位 50 进来时有值。
 *
 * buff.add / buff.remove / buff.tick：只读。字段 actor（指针）、actor_kind、actor_id（同 DDActorRef 的 kind、id，
 * 延迟事件里指针为 NULL 时用它们识别）、guid、id、stat_type、amount、duration、duration_type、source、sourcedata、
 * rule_type、plugin、tag（由 combat.buff_add 加的条目才有）。
 *   buff.add    新条目追加进 buff 表之后（Buff::Group::Add，5C4F50）：游戏的添加（技能、effect、饰品、建筑……）、
 *               叠灼烧、怪物同伴和 combat.buff_add 都经过这里；读档恢复的条目不经过，不派发。
 *   buff.remove 条目删除之前（Buff::Group::erase，5C4FF0，全部删除路径的汇合点），字段是删除前的值。
 *               reason：expired（持续时间结算删掉 duration==0 的）、plugin（combat.buff_remove 的条件命中的条目；
 *               删完后游戏刷新属性时顺带删掉的别的条目按下面的规则判断，通常是 refresh）、refresh（刷新属性时按
 *               规则删除）、town_visit（回城清理：刚回来的队伍英雄身上 DD_SOURCE_F_TOWN_CLEAR 来源的 buff）、
 *               skill_instant（技能的即时 buff）、trinket（UpdateTrinketBuffs）、burn_decay（灼烧衰减）、
 *               by_guid（游戏按 guid 删除）、reset（ResetStats）、effect（effect 结算里删除）、item（inventory.cpp 的 429750，
 *               推测是物品）、predicate（其他经虚函数槽位 14 的删除）、other。按调用栈里最内层认得的游戏函数判断，
 *               依赖的特征码缺失时归入 predicate 或 other。
 *   buff.tick   持续时间结算（Actor::BuffDurationUpkeep，47AAD0）之后，每条 duration 变了且还在表里的条目一次，
 *               duration_before / duration_after 是递减前后的值。战斗回合、走廊格子（before_turn/after_turn 也会递减）、
 *               撤退、战斗结束、回城都经过这里。减到 0 被删掉的条目在原函数里就被删了：删除之前先派发它的 buff.tick
 *               （duration_after 是删除时的值，通常是 0），紧接着派发 buff.remove（reason expired）；同一次结算里
 *               其他条目的 buff.tick 在原函数返回后派发，所以晚于这些 buff.remove。订阅 buff.tick 时也会装删除的 hook。
 *   只在存档里派发：进存档之前（CampaignLoad 之前，load_campaign 线程上 Hero::Load 重新加 quirk、饰品）的增删和读档时
 *   直接追加的条目（5C4D10）都不派发，需要时在 campaign.load 里用 combat.buff_list 枚举。进存档之后别的加载线程
 *   （load_raid、load_town）上的增删按字段复制后排到主线程，作为延迟事件派发（actor 为 NULL，用 actor_kind、actor_id）。
 *
 * campaign.save：只读，有插件声明持久化（persist.declare，DDLua 总是声明）时才有。游戏在当前 profile 上写完一组存档文件
 *   （任何 persist.*.json：城镇里写名册的一组、只写庄园或城镇文件的升级建筑和城镇事件，地牢里一次探索存档 60D610 的四个文件）
 *   之后的下一个帧边界派发；加载线程（load_raid、load_town）上写的，等加载结束后的帧边界派发；读档过程中的写入（load_campaign
 *   重写 persist.game.json）不算。同一帧里写的几个文件只派发一次。字段 files（这组在当前 profile 设备上写了哪些 persist.*.json，逗号分隔）、
 *   in_raid（Raid::Current 非空）、in_battle、main_thread（这组都是主线程写的）、serial（存档序号）、index（本次进存档的
 *   第几次 campaign.save）。派发完所有订阅之后，DDCore 依次调用各插件的 persist 保存回调，见 persist 服务。
 *   帧边界里 campaign.save 在延迟事件之前派发（快照对应刚写的存档，之后的定时器、延迟事件不改它）：加载线程上删掉的
 *   buff 要到之后的 buff.remove 才送到，这一帧的快照还带着它，读档时按「条目不在」丢弃（审核 R2B 备注 3）。
 *
 * battle.turn.* 只读，只在英雄回合派发（Combat_StartTurn 只有英雄路径调用）：
 * start 在 Combat_StartTurn 之前，end 在其返回后（不是英雄行动结束）。
 * battle.round.end 在回合推进前，第一回合之前不派发，战斗结束后最后一回合没有 end，用 battle.end。
 */

#define DD_EVENT_MAX_DEPTH 16

typedef struct DDEvent DDEvent;
typedef uint64_t DDEventSubId; /* 0 表示失败 */

typedef enum DDEventScope {
    DD_EVENT_SCOPE_PLUGIN = 0,   /* 默认，插件存在期间一直有效 */
    DD_EVENT_SCOPE_CAMPAIGN = 1, /* 退出存档时撤销 */
    DD_EVENT_SCOPE_RAID = 2,     /* 探索结算（RaidFinishResultsEvent）或退出存档时撤销 */
    DD_EVENT_SCOPE_BATTLE = 3,   /* 战斗销毁（BattleDestroyedEvent）或退出存档时撤销 */
} DDEventScope;

typedef enum DDEventFieldType {
    DD_FIELD_INT = 0,
    DD_FIELD_FLOAT = 1,
    DD_FIELD_BOOL = 2,
    DD_FIELD_STRING = 3,
    DD_FIELD_POINTER = 4,
} DDEventFieldType;

typedef enum DDEventFieldAccess {
    DD_FIELD_READ = 1,
    DD_FIELD_WRITE = 2,
} DDEventFieldAccess;

typedef enum DDEventResult {
    DD_EV_OK = 0,
    DD_EV_E_INVALID = 1,          /* 事件或参数无效 */
    DD_EV_E_NOT_FOUND = 2,        /* 字段不存在 */
    DD_EV_E_TYPE = 3,             /* 字段类型不符 */
    DD_EV_E_READ_ONLY = 4,        /* 字段或事件只读 */
    DD_EV_E_NOT_CANCELLABLE = 5,  /* 该事件不可取消 */
    DD_EV_E_RANGE = 6,            /* 值超出字段在游戏里的宽度（float、int32、uint32），没有写入 */
} DDEventResult;

/* 事件标志（event_flags） */
#define DD_EVENT_F_PRE 1u          /* .pre 主题 */
#define DD_EVENT_F_POST 2u         /* .post 主题 */
#define DD_EVENT_F_DEFERRED 4u     /* 非主线程派发后排队到主线程，只读 */
#define DD_EVENT_F_CANCELLED 8u    /* 已被某个回调取消 */
#define DD_EVENT_F_WRITABLE 16u    /* 当前可写（.pre 或 query.*，主线程） */

typedef void(DD_CALL* DDEventFn)(DDEvent* event, void* user);

/* 订阅被 DDCore 撤销的原因（DDEventEndFn 的 reason） */
#define DD_EVENT_END_SCOPE 1u /* 作用域结束（战斗销毁、探索结算、退出存档） */
#define DD_EVENT_END_CRASH 2u /* 回调崩溃 */

/*
 * 订阅被 DDCore 撤销时的通知：作用域结束、回调崩溃。插件自己 unsubscribe、插件被回滚时不通知。
 * 在撤销发生的线程调用（目前总是主线程），调用之后该订阅的回调不会再被调用，user 可以释放。
 * 通知在 SEH 隔离下运行，崩溃只写日志。通知里可以调用 events 服务，但不要再 unsubscribe 这个 id。
 */
typedef void(DD_CALL* DDEventEndFn)(DDEventSubId id, void* user, uint32_t reason);

typedef struct DDEventOptions {
    uint32_t size;        /* sizeof(DDEventOptions)，0 或 NULL options 表示全部默认；
                             只认识到 scope 的旧结构（不含 on_end）照样读 priority 和 scope */
    int32_t priority;     /* 升序，同优先级按注册先后，默认 0 */
    uint32_t scope;       /* DDEventScope，默认 DD_EVENT_SCOPE_PLUGIN */
    DDEventEndFn on_end;  /* 可为 NULL，见 DDEventEndFn */
} DDEventOptions;

/* 字段描述，字符串在进程生命周期内有效 */
typedef struct DDEventFieldInfo {
    const char* name;
    uint32_t type;   /* DDEventFieldType */
    uint32_t access; /* DDEventFieldAccess 位或 */
} DDEventFieldInfo;

/* 主题信息（可用性和原因由 topic_available 查询，字段由 field_* 查询） */
typedef struct DDEventTopicInfo {
    const char* name;
    uint32_t kind;        /* 0 native，1 query，2 语义，3 自定义，4 应用 */
    uint32_t field_count;
    bool writable;        /* .pre 或 query.* */
    bool cancellable;
    bool available;
} DDEventTopicInfo;

/* 发布自定义主题时的字段初值，name 必须对应注册的字段 */
typedef struct DDEventValue {
    const char* name;
    uint32_t type;
    union {
        int64_t i;
        double f;
        bool b;
        const char* s;
        void* p;
    } v;
} DDEventValue;

typedef struct DDEventsService {
    DDServiceHeader header;

    /* 订阅。主题不存在或不可用时返回 0，原因写入 DDCore.log 和插件日志。
       options 可为 NULL（默认优先级和 plugin 作用域）。线程安全。
       订阅只在下一次派发生效，回调里调用也一样。 */
    DDEventSubId(DD_CALL* subscribe)(DDPluginHandle plugin, const char* topic, DDEventFn fn, void* user,
        const DDEventOptions* options);
    bool(DD_CALL* unsubscribe)(DDPluginHandle plugin, DDEventSubId id);

    /* 主题枚举。topic_count 返回总数，topic_at 按下标取，下标超界时返回 false。
       可用性原因通过 topic_available 的 reason 输出（在进程生命周期内有效，可为 NULL）。 */
    uint32_t(DD_CALL* topic_count)(void);
    bool(DD_CALL* topic_at)(uint32_t index, DDEventTopicInfo* out);
    bool(DD_CALL* topic_info)(const char* topic, DDEventTopicInfo* out);
    bool(DD_CALL* topic_available)(const char* topic, const char** reason);
    bool(DD_CALL* topic_field_at)(const char* topic, uint32_t index, DDEventFieldInfo* out);

    /* 自定义主题。名字不能以 native./query./damage. 等保留前缀开头，已存在时返回 false。
       名字以 .pre 结尾时字段可以标记 DD_FIELD_WRITE，并且可以被取消。 */
    bool(DD_CALL* register_topic)(DDPluginHandle plugin, const char* topic, const DDEventFieldInfo* fields,
        uint32_t field_count);
    /* 发布只在主线程调用（否则返回 false）。values 按字段名给初值，未给的字段默认 0/NULL。
       values 可写：.pre 主题派发结束后最终值写回 values（字符串除外）。
       out_cancelled 可为 NULL。派发深度超限时写日志并返回 false。 */
    bool(DD_CALL* publish)(DDPluginHandle plugin, const char* topic, DDEventValue* values, uint32_t value_count,
        bool* out_cancelled);

    /* 负载读取，回调期间有效。字段不存在或类型不符返回错误码，out 不变。 */
    DDEventResult(DD_CALL* get_int)(const DDEvent* event, const char* field, int64_t* out);
    DDEventResult(DD_CALL* get_float)(const DDEvent* event, const char* field, double* out);
    DDEventResult(DD_CALL* get_bool)(const DDEvent* event, const char* field, bool* out);
    DDEventResult(DD_CALL* get_string)(const DDEvent* event, const char* field, const char** out);
    DDEventResult(DD_CALL* get_pointer)(const DDEvent* event, const char* field, void** out);

    /* 负载写入，.pre 和 query.* 且字段可写时成功。 */
    DDEventResult(DD_CALL* set_int)(DDEvent* event, const char* field, int64_t value);
    DDEventResult(DD_CALL* set_float)(DDEvent* event, const char* field, double value);
    DDEventResult(DD_CALL* set_bool)(DDEvent* event, const char* field, bool value);
    DDEventResult(DD_CALL* set_string)(DDEvent* event, const char* field, const char* value);
    DDEventResult(DD_CALL* set_pointer)(DDEvent* event, const char* field, void* value);

    /* 取消 .pre 事件，含义见本节开头。其余主题返回 DD_EV_E_NOT_CANCELLABLE。 */
    DDEventResult(DD_CALL* cancel)(DDEvent* event);

    /* 事件元信息 */
    uint32_t(DD_CALL* event_flags)(const DDEvent* event);
    const char*(DD_CALL* event_topic)(const DDEvent* event);
    uint32_t(DD_CALL* event_origin_thread)(const DDEvent* event); /* 派发线程 id，延迟事件是原线程 */

    /* 诊断：该插件当前有效的订阅数（已撤销的不算），用于核对插件自己的记录。线程安全。 */
    uint32_t(DD_CALL* subscription_count)(DDPluginHandle plugin);
} DDEventsService;

/* ------------------------------------------------------------------ game */

#define DD_SERVICE_GAME "game"
#define DD_SERVICE_GAME_VERSION 2u

/* 启用 Mod 列表的一项 */
typedef struct DDModEntry {
    uint32_t size;      /* 调用方写入 sizeof(DDModEntry) */
    const char* name;   /* Workshop 是 PublishedFileId（十进制），本地 Mod 是 project.xml 的标题（不是目录名） */
    const char* source; /* "Steam" 或 "mod_local_source" */
    const char* dir;    /* Mod 目录的绝对路径，结尾不带分隔符；没找到时为空串 */
    bool found;         /* 在已安装的 Mod 中找到，且目录存在 */
} DDModEntry;

/* actor 的种类（DDActorRef.kind） */
#define DD_ACTOR_NONE 0u
#define DD_ACTOR_HERO 1u
#define DD_ACTOR_MONSTER 2u

/*
 * actor 句柄：一个值，不含指针，可以随意复制、保存、比较（六个标识字段都相等就是同一个 actor）。
 * 每次使用时由 DDCore 重新解析：
 *   英雄  在创建时所在的名册（roster）里按 hero id 找，要求存档序号没变；
 *   怪物  在当前战斗（Raid::Current 的 Battle+0x38）里按 guid 找，要求存档序号、探索序号、战斗序号都没变。
 * 怪物的 guid 只在一场战斗内唯一：每场战斗开始时游戏把计数清零（Battle::Create 的 5A40F7），同一次探索的
 * 下一场战斗会重新用到同样的 guid，所以怪物句柄带着战斗序号，跨战斗、跨探索、跨存档的旧句柄都解析失败，
 * 不会指到别的对象。怪物只在战斗里能解析（战斗销毁时游戏先清空怪物表）。
 * 英雄只要名册里还有这个条目就能解析：阵亡的英雄条目留在名册里（状态 3），句柄仍然有效；被解雇的英雄
 * 条目被删除并释放，句柄立即失效（knowledge/dd1/combat.md）。
 */
typedef struct DDActorRef {
    uint32_t size;            /* sizeof(DDActorRef)，填写方写入 */
    uint32_t kind;            /* DD_ACTOR_HERO / DD_ACTOR_MONSTER */
    uint32_t id;              /* 英雄：hero id（名册 id，事件里的 hero_id）；怪物：guid（MonsterDeathEvent 的 monster_guid） */
    uint32_t campaign_serial; /* 创建时的存档序号（DDPhaseEvent.campaign_serial） */
    uint32_t raid_serial;     /* 怪物：创建时的探索序号（game.raid_serial）；英雄为 0 */
    uint32_t battle_serial;   /* 怪物：创建时的战斗序号（game.battle_serial）；英雄为 0 */
    uint32_t roster;          /* 英雄：0 主名册，1 备用名册（游戏临时切换的 Campaign+0x728）；怪物为 0 */
} DDActorRef;

/* actor_info 的输出。只放偏移核实过的字段 */
typedef struct DDActorInfo {
    uint32_t size;      /* 调用方写入 sizeof(DDActorInfo) */
    uint32_t kind;
    uint32_t id;
    float hp;           /* 当前生命（Actor+0x10A8） */
    float max_hp;       /* 最大生命（游戏自己的计算函数，和治疗时的上限相同），has_max_hp 为 false 时无意义 */
    float stress;       /* 压力（Actor+0x12FC），只有英雄，has_stress 为 false 时无意义 */
    bool has_max_hp;    /* 特征码 ActorMaxStat 缺失或读取失败时为 false */
    bool has_stress;
    char class_id[64];  /* 职业 id（ActorClass+8，如 "crusader"、"skeleton_soldier"），读不出来时为空串 */
} DDActorInfo;

/*
 * 游戏状态的只读查询，全部只在主线程调用（其他线程返回 0 / false）。
 *
 * 启用 Mod 列表：当前存档的 applied_ugcs_1_0，顺序和存档一致。DDCore 在 CAMPAIGN_LOAD 回调之前读取
 * 并复制，CAMPAIGN_UNLOAD 回调之后清空，返回的字符串在这期间有效。存档外 mod_count 为 0。
 * 特征码缺失或结构校验失败时 mods_available 为 false，mod_count 为 0，原因写在 DDCore.log。
 *
 * actor：见 DDActorRef。探索序号由 DDCore 在每帧的状态机取样里维护（每次进入 as_raid_start 加一，
 * 那时 load_raid 线程新建 Raid 对象）。战斗序号由 DDCore 在每帧和每次 actor 函数调用时取样当前战斗
 * （Battle+0x288 的状态、+0x38 的怪物表、+0x2D4 的 guid 计数）维护：看到「没有战斗」（状态 bs_none 且怪物表为空）
 * 之后又看到战斗（状态非零，或者怪物表不空），或者 guid 计数变小（不管状态），就加一。游戏在 Battle::Create 里
 * 填好怪物表之后才置 bs_create、再派发 BattleCreatedEvent，所以 battle.start 回调、以及它之前 Battle::Create 里
 * 派发的 heal.pre/heal.post 回调（只有 flashback 战斗有，普通战斗不执行那段循环）里调用 actor 函数时都已经是新的序号。
 * 不派发 BattleCreatedEvent 的开战路径（从存档恢复的战斗）同样加一。两者都不订阅事件、不装 hook。
 * actor_kinds 给出可用的种类（DD_ACTOR_HERO、DD_ACTOR_MONSTER 左移成位：1u << kind），依赖的特征码缺失时
 * 对应的位为 0，函数返回 false。
 */
typedef struct DDGameService {
    DDServiceHeader header;
    bool(DD_CALL* mods_available)(void);
    uint32_t(DD_CALL* mod_count)(void);
    /* 下标超界或 out->size 太小时返回 false */
    bool(DD_CALL* mod_at)(uint32_t index, DDModEntry* out);

    /* 可用的 actor 种类位：(1u << DD_ACTOR_HERO) | (1u << DD_ACTOR_MONSTER) */
    uint32_t(DD_CALL* actor_kinds)(void);
    /* 当前的探索序号（从 1 开始，还没进过地牢时为 0），诊断用 */
    uint32_t(DD_CALL* raid_serial)(void);
    /* 游戏对象指针（事件里的 actor 字段）转句柄：虚表是 Hero 且在名册里、或虚表是 Monster 且在当前战斗里时成功。
       指针不可读或不是 actor 时返回 false。out->size 由调用方写入 */
    bool(DD_CALL* actor_from_pointer)(const void* actor, DDActorRef* out);
    /* 按 id 创建当前能解析的句柄：英雄在主名册里找 hero id，怪物在当前战斗里找 guid。找不到返回 false */
    bool(DD_CALL* actor_find)(uint32_t kind, uint32_t id, DDActorRef* out);
    /* 解析句柄，成功时返回对象指针（只在本次回调或本帧内使用，不要保存），失败返回 NULL */
    void*(DD_CALL* actor_resolve)(const DDActorRef* ref);
    /* 解析并读取字段，解析失败返回 false。out->size 太小时返回 false */
    bool(DD_CALL* actor_info)(const DDActorRef* ref, DDActorInfo* out);
    /* 当前的战斗序号（从 1 开始，还没打过仗时为 0；战斗结束后保持不变，下一场战斗开始时加一），诊断用 */
    uint32_t(DD_CALL* battle_serial)(void);

    /*
     * 只读枚举（F5 起，P5 的 dd.raid 在这上面扩展）。三个函数都只在主线程、存档里、没有加载线程在跑时可用
     * （加载线程在新建 Raid、重建怪物表：as_load_campaign、as_raid_start、as_town_visit_start 停留期间），否则
     * party_heroes / battle_monsters 返回 false、*out_count 为 0，battle_state 返回 -1。不装 hook，不订阅事件；
     * 读游戏内存在 SEH 保护下进行，读不出来按「没有」处理。
     *
     * party_heroes：当前探索队伍的英雄，Raid::Current+8 的 vector<Hero*>（effect 的组类目标 4C41F0 读的就是它），按这张表的
     *   顺序：换位后游戏（Battle::HeroesRerank，5B8F80）按站位重排写回 [静态]，所以一般是第 1 位到第 4 位 [推测]。每个指针都要
     *   虚表是 Hero、并且在名册里（ADR 0024 的成员关系）才给句柄，不满足的跳过。没有 Raid（城镇里）时成功、个数 0。
     * battle_monsters：当前战斗的怪物，Battle+0x38 的 vector<Monster*>（按站位插入），只算虚表是 Monster 的（表里的英雄跳过）。
     *   怪物死后变成尸体（同一个 guid，职业 id 换成 corpse_* 一类，身上的 buff 被游戏删掉）仍在表里，要不要算由调用方按职业 id
     *   或生命判断。不在战斗里时个数 0（战斗销毁时游戏先清空这张表）。
     *   两者都写入至多 capacity 个句柄（out 可为 NULL，只取个数；DDCore 写整个结构，含 size），*out_count 是总个数（可能大于 capacity）。
     * battle_state：Battle+0x288（bs_none = 0）。非零就是战斗中，包括从存档恢复的战斗（游戏不派发 battle.start，读档线程已经
     *   把状态写成非零）；撤退时游戏先把它置 0（5A93B3），之后怪物表才清空。没有 Raid 时是 0。
     */
    bool(DD_CALL* party_heroes)(DDActorRef* out, uint32_t capacity, uint32_t* out_count);
    bool(DD_CALL* battle_monsters)(DDActorRef* out, uint32_t capacity, uint32_t* out_count);
    int32_t(DD_CALL* battle_state)(void);
} DDGameService;

/* ------------------------------------------------------------------ combat */

#define DD_SERVICE_COMBAT "combat"
#define DD_SERVICE_COMBAT_VERSION 1u

/*
 * Buff、Effect、技能的读写，定义注册，规则注册（P4）。依据见 knowledge/dd1/combat.md，设计见 docs/02-architecture.md。
 *
 * 句柄都是值，不保存游戏指针（ADR 0024 的做法）：
 *   buff  （actor 句柄，组 guid）。组 guid 在一个 actor 上只增不减，随存档保存，是条目唯一稳定的标识。
 *   effect 名字。
 *   技能  DDSkillRef（执行者句柄，技能 id，等级），每次在执行者职业的技能表里重新解析。
 * 读取得到的是快照（字段值），不是活对象。
 *
 * 线程：全部只在主线程调用，其他线程返回 DD_COMBAT_E_THREAD 或 false。
 *
 * 写操作（buff_add、buff_remove、buff_define、effect_apply、effect_define）的时机：
 *   - 安全点立即执行，同步返回 DD_COMBAT_OK 和结果。安全点是帧边界：lifecycle 的回调（TICK、CAMPAIGN_LOAD 等）、
 *     task 服务的任务、操作回调（DDCombatOpFn），并且 DDCore 没有在派发事件、没有在调用游戏函数。
 *   - 事件回调里（DDCore 正在派发原生事件、语义 hook 或 buff.* 主题，游戏可能持有 buff 表内的指针），以及安全点上
 *     立即执行时同步派发的嵌套回调里：不执行，排进队列，返回 DD_COMBAT_QUEUED 和操作号（out->op）。
 *     队列在下一个帧边界按调用顺序执行，执行前重新解析句柄、重新检查条件，结果经 set_op_handler 注册的回调通知；
 *     退出存档时还没执行的操作丢弃，回调的 result 为 DD_COMBAT_E_CANCELLED。每个插件最多排 256 个，超出返回
 *     DD_COMBAT_E_QUEUE_FULL。
 *   - 规则回调（DDRuleFn）里一律返回 DD_COMBAT_E_IN_RULE，不排队。
 *   - 不在存档里、加载线程正在运行（读档、进地牢、进城）、PvP 对局中（ADR 0011）：拒绝。
 * 立即执行时游戏会同步派发 buff.add / buff.remove 等主题，订阅者的回调在这次调用返回之前运行（嵌套），
 * 回调里的写操作照样排队。
 *
 * 失败原因除了返回码，out->message 给出文字（本线程下一次调用 combat 服务之前有效）。
 */

typedef enum DDCombatResult {
    DD_COMBAT_OK = 0,
    DD_COMBAT_QUEUED = 1,         /* 排进队列，结果经操作回调通知 */
    DD_COMBAT_E_INVALID = 2,      /* 参数无效 */
    DD_COMBAT_E_UNAVAILABLE = 3,  /* 依赖的特征码缺失，这项能力不可用 */
    DD_COMBAT_E_THREAD = 4,       /* 不在主线程 */
    DD_COMBAT_E_NO_CAMPAIGN = 5,  /* 不在存档里 */
    DD_COMBAT_E_LOADING = 6,      /* 加载线程正在运行 */
    DD_COMBAT_E_PVP = 7,          /* PvP 对局中 */
    DD_COMBAT_E_IN_RULE = 8,      /* 在规则回调里 */
    DD_COMBAT_E_QUEUE_FULL = 9,   /* 本插件排队的操作太多 */
    DD_COMBAT_E_ACTOR = 10,       /* actor 句柄解析失败 */
    DD_COMBAT_E_NOT_FOUND = 11,   /* buff 定义、effect、条目不存在 */
    DD_COMBAT_E_NO_RAID = 12,     /* 施加 effect 需要 Raid::Current，城镇里没有 */
    DD_COMBAT_E_EXISTS = 13,      /* 定义已经存在（游戏数据或别的插件定义的） */
    DD_COMBAT_E_GAME = 14,        /* 调用游戏函数时出了异常（已被捕获） */
    DD_COMBAT_E_CANCELLED = 15,   /* 排队的操作没有执行就被丢弃（退出存档、插件回滚） */
} DDCombatResult;

typedef uint64_t DDCombatOpId; /* 0 表示没有排队 */

typedef enum DDCombatOpKind {
    DD_COMBAT_OP_BUFF_ADD = 1,
    DD_COMBAT_OP_BUFF_REMOVE = 2,
    DD_COMBAT_OP_EFFECT_APPLY = 3,
    DD_COMBAT_OP_BUFF_DEFINE = 4,
    DD_COMBAT_OP_EFFECT_DEFINE = 5,
} DDCombatOpKind;

/*
 * 写操作的结果。立即执行时由调用方提供（可为 NULL，size 由调用方写入），排队时 op 是操作号；
 * 排队的操作执行后以同样的结构交给操作回调。
 */
typedef struct DDCombatOpResult {
    uint32_t size;
    uint32_t kind;        /* DDCombatOpKind */
    DDCombatOpId op;      /* 操作号；立即执行时为 0 */
    uint32_t result;      /* DDCombatResult：DD_COMBAT_OK 或失败原因（回调里不会是 QUEUED） */
    uint32_t guid;        /* buff_add：新条目的组 guid */
    uint32_t count;       /* buff_remove：删掉的条数；effect_apply：本次加上的 buff 条数；effect_define：定义的 effect 数 */
    const char* message;  /* 失败原因的文字，成功时为空串 */
} DDCombatOpResult;

/* 排队的操作执行完（或被丢弃）时调用，在主线程的帧边界（安全点，回调里的写操作立即执行）。SEH 隔离，崩溃只写日志 */
typedef void(DD_CALL* DDCombatOpFn)(const DDCombatOpResult* result, void* user);

/* 枚举表（enum_* 的 table） */
#define DD_ENUM_STAT 0u      /* Buff stat_type，0..101 */
#define DD_ENUM_SOURCE 1u    /* Buff::Source，0..33；游戏里的名字带前缀 bsrc_，这里去掉，查找时两种写法都认 */
#define DD_ENUM_DURATION 2u  /* Buff::Duration::Types，0..10 */
#define DD_ENUM_RULE 3u      /* Buff::Rule::Types，0..54 */

#define DD_COMBAT_TEXT 64 /* 游戏里 char[64] 的字段，最多 63 字节加结尾的 0 */

/* 来源表（Buff::Source）的标志，source_flags 和 DDBuffInfo.source_flags（P4-0D，静态初始化 78500 的值） */
#define DD_SOURCE_F_SAVED 0x1u      /* +0x44：这个来源的 buff 写进存档（Actor::Save 只存这些） */
#define DD_SOURCE_F_TOWN_CLEAR 0x2u /* +0x46：回城时从刚回来的队伍英雄身上删掉（OnStartTownVisit 经槽位 14） */
#define DD_SOURCE_F_TRAY 0x4u       /* +0x48：显示在探索中的 buff/debuff tray 里 */

/* 一条 buff 的快照（buff_get）。字段含义见 knowledge/dd1/combat.md 的 Buff::Instance */
typedef struct DDBuffInfo {
    uint32_t size;        /* 调用方写入 sizeof(DDBuffInfo) */
    uint32_t guid;        /* 组 guid */
    char id[DD_COMBAT_TEXT];
    int32_t stat_type;
    char stat_sub_type[DD_COMBAT_TEXT];
    float amount;
    int32_t duration;     /* 剩余次数，-1 不计时 */
    int32_t duration_type;
    int32_t source;
    uint32_t source_flags;  /* 来源的 DD_SOURCE_F_* */
    int32_t sourcedata;
    int32_t rule_type;
    bool is_false_rule;
    bool remove_if_not_active;
    bool remove_on_battle_complete;
    bool by_plugin;       /* 由插件经 buff_add 添加，并且 DDCore 还记着它（标签不随游戏存档保存） */
    float rule_float;
    char rule_string[DD_COMBAT_TEXT];
    char plugin[DD_COMBAT_TEXT];  /* 添加它的插件名，by_plugin 为 false 时为空串 */
    char tag[DD_COMBAT_TEXT];     /* 添加时给的标签 */
} DDBuffInfo;

/*
 * buff 定义的 POD 字段（buff_def_get 的输出、buff_define 的输入）。只开放这些字段：附加效果、force_positive 之类
 * 「只在定义里」的字段不开放，所以读档时游戏查不到运行时定义也不丢东西（combat.md「Buff 存档」）。
 */
typedef struct DDBuffDef {
    uint32_t size;        /* sizeof(DDBuffDef) */
    char id[DD_COMBAT_TEXT];
    int32_t stat_type;    /* 0..101 */
    char stat_sub_type[DD_COMBAT_TEXT];
    float amount;
    int32_t duration;     /* 默认持续次数，-1 不计时，0 不允许（游戏会在下一次持续时间结算时删掉） */
    int32_t duration_type;  /* 0..10 */
    int32_t source;       /* 0..33 */
    int32_t sourcedata;
    int32_t rule_type;    /* 0..54 */
    bool is_false_rule;
    bool remove_if_not_active;
    bool remove_on_battle_complete;
    float rule_float;
    char rule_string[DD_COMBAT_TEXT];
} DDBuffDef;

/* buff_add 的选项，flags 标记哪些字段有效，其余沿用定义里的值 */
#define DD_BUFF_ADD_DURATION 0x1u
#define DD_BUFF_ADD_DURATION_TYPE 0x2u
#define DD_BUFF_ADD_SOURCE 0x4u
#define DD_BUFF_ADD_SOURCEDATA 0x8u
#define DD_BUFF_ADD_NO_REFRESH 0x10u /* 不刷新属性（默认刷新：游戏的 refresh=true，UpdateBuffsInternal 重算） */

typedef struct DDBuffAddOptions {
    uint32_t size;        /* sizeof(DDBuffAddOptions) */
    uint32_t flags;
    int32_t duration;     /* -1 或大于 0 */
    int32_t duration_type;
    int32_t source;
    int32_t sourcedata;
    const char* tag;      /* 可为 NULL；最多 63 字节。DDCore 记下（actor，guid）→（插件，标签），可按标签移除 */
} DDBuffAddOptions;

typedef enum DDBuffMatch {
    DD_BUFF_BY_GUID = 0,
    DD_BUFF_BY_TAG = 1,     /* 只匹配本插件加的、标签相同的条目 */
    DD_BUFF_BY_ID = 2,      /* 按 buff id，不管是谁加的 */
    DD_BUFF_BY_SOURCE = 3,  /* 按来源（Buff::Source），不管是谁加的 */
} DDBuffMatch;

typedef struct DDBuffRemoveSpec {
    uint32_t size;        /* sizeof(DDBuffRemoveSpec) */
    uint32_t match;       /* DDBuffMatch */
    uint32_t guid;        /* BY_GUID */
    int32_t source;       /* BY_SOURCE */
    const char* text;     /* BY_TAG：标签；BY_ID：buff id */
} DDBuffRemoveSpec;

/* effect 的只读字段（effect_get），偏移见 combat.md「Effect」的结构 */
typedef struct DDEffectInfo {
    uint32_t size;        /* sizeof(DDEffectInfo) */
    char name[DD_COMBAT_TEXT];
    int32_t target;       /* .target 枚举：0 performer、1 target、2 performer_group …（combat.md） */
    float chance;
    float stress;
    float stun;
    float heal;
    float heal_percent;
    int32_t combat_stat_buff;
    int32_t dot_poison;
    int32_t dot_bleed;
    int32_t dot_burn;
    int32_t dot_stress;
    int32_t duration;
    float buff_amount;
    int32_t buff_duration_type;
    uint32_t buff_id_count;  /* .buff_ids 的个数（游戏只存哈希） */
    bool on_hit;
    bool on_miss;
    bool apply_once;
    bool queue;
    bool swap_source_and_target;
    bool runtime;         /* 由插件经 effect_define 定义 */
} DDEffectInfo;

/* 技能句柄：执行者 + 技能 id + 等级，每次在执行者职业的技能表里重新解析（英雄 HeroClass+0xBD0/+0xBE8，怪物 MonsterClass+0xBA8） */
typedef struct DDSkillRef {
    uint32_t size;        /* sizeof(DDSkillRef) */
    DDActorRef actor;     /* 执行者 */
    char id[DD_COMBAT_TEXT];
    int32_t level;        /* 怪物技能没有等级，为 0 */
} DDSkillRef;

#define DD_SKILL_MAX_EFFECTS 16

typedef struct DDSkillInfo {
    uint32_t size;        /* sizeof(DDSkillInfo) */
    char id[DD_COMBAT_TEXT];
    int32_t level;
    char type[DD_COMBAT_TEXT];  /* .type，如 melee、ranged、move */
    bool friendly;        /* 友方技能（技能 +0x90） */
    uint32_t effect_count;  /* .effect 的个数（最多 16） */
    char effects[DD_SKILL_MAX_EFFECTS][DD_COMBAT_TEXT];  /* effect 名字 */
} DDSkillInfo;

/*
 * 规则扩展（D8）：数据层的 buff 写 rule_type "is_actor_status"（检查自己）或 "actorStatus"（检查 target），
 * rule_data.string 填注册的规则名。游戏判定这条规则时调用回调，回调返回规则本身是否成立，DDCore 再按 is_false_rule 取反。
 *
 * 回调在游戏遍历 buff 表的过程中运行（判定非常频繁，主要在 UpdateBuffsInternal 收集生效 buff 时）：
 *   - 只读：combat 服务的写操作一律返回 DD_COMBAT_E_IN_RULE；不要调用会改游戏状态的东西；
 *   - 要快：只有 rule_data.string 的哈希命中注册表的判定才进入回调，其余直接交给游戏；
 *   - 不在主线程、PvP 对局中、回调崩溃过（自动停用）时不调用回调，交给游戏原来的判定（认不出的字符串恒为假）；
 *   - 回调返回 DDRuleResult：DD_RULE_TRUE / DD_RULE_FALSE 是规则本身是否成立（DDCore 再按 is_false_rule 取反），
 *     DD_RULE_ORIGINAL 表示这次交还游戏原来的判定（等同没有注册：认不出的字符串原始结果为假，再由游戏按 is_false_rule 取反；
 *     城镇里规则 42 在前置检查就返回假、不取反）。回调出错、超时、停用中都应返回它，其他值按 DD_RULE_ORIGINAL 处理；
 *   - 指针只在回调期间有效。
 */
typedef struct DDRuleContext {
    uint32_t size;
    const char* rule;     /* 规则名（rule_data.string） */
    void* actor;          /* 拥有这条 buff 的 actor（Hero/Monster） */
    void* target;         /* 判定时的 target，可能为 NULL */
    void* subject;        /* 要检查的角色：is_actor_status 是 actor，actorStatus 是 target */
    const void* skill;    /* 判定时的技能，可能为 NULL */
    int32_t rule_type;    /* 13 actorStatus 或 42 is_actor_status */
    bool is_false_rule;   /* 回调不用自己取反，DDCore 会处理 */
    float rule_float;     /* rule_data.float */
    const char* buff_id;
} DDRuleContext;

typedef enum DDRuleResult {
    DD_RULE_FALSE = 0,     /* 规则不成立 */
    DD_RULE_TRUE = 1,      /* 规则成立 */
    DD_RULE_ORIGINAL = 2,  /* 交还游戏原来的判定（等同这次没有注册） */
} DDRuleResult;

typedef uint32_t(DD_CALL* DDRuleFn)(const DDRuleContext* context, void* user); /* 返回 DDRuleResult */
typedef uint64_t DDRuleId; /* 0 表示失败 */

/* 命中脚本规则的判定的统计（从进程启动累计；frame 是单帧的峰值） */
typedef struct DDRuleStats {
    uint32_t size;
    uint32_t rules;           /* 当前注册的规则数 */
    uint64_t hits;            /* 进入回调的次数 */
    uint64_t skipped;         /* 命中但没有调用回调（不在主线程、PvP、停用）的次数 */
    uint64_t passed;          /* 回调返回 DD_RULE_ORIGINAL、交还原函数的次数（计入 hits） */
    uint64_t errors;          /* 回调崩溃的次数 */
    double total_us;          /* 回调累计耗时（微秒） */
    double max_call_us;       /* 单次最长 */
    double max_frame_us;      /* 单帧累计耗时的峰值 */
    uint64_t max_frame_hits;  /* 单帧命中次数的峰值 */
} DDRuleStats;

typedef struct DDCombatService {
    DDServiceHeader header;

    /* 返回码的文字，静态字符串 */
    const char*(DD_CALL* result_text)(uint32_t result);
    /* 注册本插件的操作回调（排队的写操作执行后通知）。fn 为 NULL 时取消。主线程 */
    bool(DD_CALL* set_op_handler)(DDPluginHandle plugin, DDCombatOpFn fn, void* user);
    /* 现在调用写操作会不会立即执行（主线程、存档里、安全点、不在加载和 PvP 中） */
    bool(DD_CALL* safe_point)(void);
    /* 读接口（buff_list、buff_get、buff_def_get、effect_get）现在能不能用：主线程，并且没有加载线程在跑。
       读接口返回 false 时用它区分「加载中，暂时读不到」和「条目、定义真的不在」（审核 R2A-S1）。任意线程 */
    bool(DD_CALL* read_ready)(void);

    /* 枚举表：名字和数值互查。找不到时 enum_name 返回 NULL，enum_value 返回 -1。任意线程 */
    uint32_t(DD_CALL* enum_count)(uint32_t table);
    const char*(DD_CALL* enum_name)(uint32_t table, int32_t value);
    int32_t(DD_CALL* enum_value)(uint32_t table, const char* name);
    /* 来源（0..33）的 DD_SOURCE_F_* 标志，超界返回 0。任意线程 */
    uint32_t(DD_CALL* source_flags)(int32_t source);

    /* buff 读取（buff_list、buff_get、buff_def_get、effect_get 都一样）：只在主线程，并且没有加载线程在跑时可用，
       否则返回 false——加载线程（load_campaign、load_raid、load_town）会增删 buff 表（删除时条目前移、扩容时整张表搬家）、
       重建 Buff 库和 effect map，这时读到的不可信（A-S3）。加载画面期间（读档进地牢的 as_raid_start、回城的
       as_town_visit_start）的定时器、延迟事件回调里读会失败，read_ready 为假，等加载结束再读。
       buff_list 把组 guid 按表里的顺序写进 guids（最多 capacity 个），*out_count 是总条数；
       guids 可为 NULL（只取条数）。actor 解析失败、表读不出来（超过 4096 条）返回 false */
    bool(DD_CALL* buff_list)(const DDActorRef* actor, uint32_t* guids, uint32_t capacity, uint32_t* out_count);
    /* 按组 guid 读一条，条目不存在返回 false */
    bool(DD_CALL* buff_get)(const DDActorRef* actor, uint32_t guid, DDBuffInfo* out);
    /* Buff 库里的定义（游戏数据和插件注册的），不存在返回 false。只在存档里可用 */
    bool(DD_CALL* buff_def_get)(const char* id, DDBuffDef* out);

    /* buff 写入，时机见本节开头，返回 DDCombatResult。
       buff_add：按 id 复制库里的定义，按 options 改字段（target 一律 0），经虚函数槽位 13 添加；同 id 重复添加按游戏的
       语义叠加（两条），调用方要替换时自己先删。立即执行时 out->guid 是新条目的组 guid。options 可为 NULL。
       城镇、走廊、战斗中都可以加。actor 的 buff 表已经有 1024 条以上时返回 DD_COMBAT_E_INVALID（读接口的上限是 4096 条）。 */
    uint32_t(DD_CALL* buff_add)(DDPluginHandle plugin, const DDActorRef* actor, const char* id,
        const DDBuffAddOptions* options, DDCombatOpResult* out);
    /* 经虚函数槽位 14 删除匹配的条目（删到东西时游戏刷新属性），out->count 是删掉的条数（0 也算成功） */
    uint32_t(DD_CALL* buff_remove)(DDPluginHandle plugin, const DDActorRef* actor, const DDBuffRemoveSpec* spec,
        DDCombatOpResult* out);
    /* 在 Buff 库里注册运行时定义（D4）。只在存档里；Buff 库每次读档重建，每次进存档要重新注册。
       stat_sub_type 按 stat 校验（审核 C-Y3，游戏认不出时有的弹 assert、有的越界写到相邻的累加值）：combat_stat_add /
       combat_stat_multiply 必须是 damage_low、damage_high、defense_rating、protection_rating、speed_rating、attack_rating、
       crit_chance、max_hp、riposte_on_hit_chance、riposte_on_miss_chance 之一；resistance 是 stun、move、poison、bleed、
       disease、debuff、death_blow、trap；activity_side_effect_chance、disable_combat_skill_attribute 也只认游戏表里的名字；
       压力类（9、10、29、30）和治疗类（6、7、8、98）可以为空或是游戏认的来源名；其余 stat 不限。不合法时返回 DD_COMBAT_E_INVALID，
       完整的表见 core/src/combat/logic.cpp 的 ValidateSubStat。
       id 已经存在（游戏数据或别的插件注册的）返回 DD_COMBAT_E_EXISTS；同一个插件在本存档里重复注册同一个 id 时更新字段。 */
    uint32_t(DD_CALL* buff_define)(DDPluginHandle plugin, const DDBuffDef* def, DDCombatOpResult* out);

    /* effect：按名字读只读字段，不存在返回 false（不写游戏日志）。只在存档里可用 */
    bool(DD_CALL* effect_get)(const char* name, DDEffectInfo* out);
    /* 施加 effect（游戏的 ApplyEffect，4CB4D0）：按 effect 的 .target 从 performer、target 出发选目标，逐个结算。
       需要 Raid::Current（走廊、战斗中），城镇里返回 DD_COMBAT_E_NO_RAID。target 可为 NULL（等于 performer）。
       立即执行时 out->count 是本次加上的 buff 条数。会触发 effect.pre/post、buff.add 等主题 */
    uint32_t(DD_CALL* effect_apply)(DDPluginHandle plugin, const char* name, const DDActorRef* performer,
        const DDActorRef* target, DDCombatOpResult* out);
    /* 用游戏自己的解析器定义 effect（D5）：text 是 .effects.darkest 的内容（一个或多个 effect: 块，每块都要有 .name），
       DDCore 写成临时文件（DDModLoader\tmp\）后调用 4F02D0，用完删除。DDCore 先按游戏解析器的规则检查文本
       （A-B1、A-S1）：去掉 // 行注释和块注释（斜杠星号到星号斜杠，引号里的也算）后按「标识符:」分块，块从标识符开始，到下一个冒号（# 到行尾之间的不算）前面
       那个标识符为止，# 注释和空白都算在块里，所以值里的冒号（x:y）会开始一个新块；每块不能超过 1023 字节（达到 1024 时
       游戏进程会被 CRT 直接终止）；每块都必须是 effect: 块；.name 取块里最后一次出现的（# 注释里的、紧跟引号的都算），
       名字为空、超过 63 字节、和同一段文本里别的名字哈希相同都拒绝；.kill_enemy_types、.buff_ids、.summon_monsters、
       .summon_ranks、.set_monster_class_ids 的列表项不能以块末尾的引号开头（游戏读这一项时进程会被终止）；
       0 字节、标识符和 .name 的值周围的非 ASCII 字符（结果依赖区域设置）也拒绝；每块的 .target 必须是游戏认的 21 种之一
       （performer、target、target_group、global……，缺少或认不出时游戏弹 assert「EffectTarget: Invalid type」）。名字的归属按游戏的哈希判断。
       只在存档里；effect map 每次读档重建，
       每次进存档要重新定义。CampaignLoad 时已经在表里的名字算游戏数据，和别的插件定义的名字一样返回 DD_COMBAT_E_EXISTS；
       同一个插件可以重新定义自己的，但游戏的解析器复用原来的对象（4F02D0）：标量字段被新值覆盖，列表字段
       （.buff_ids、.kill_enemy_types、.summon_monsters、.set_monster_class_id(s)）在原来的后面追加，不清空——重新定义带
       .buff_ids 的 effect 之后施加时这些 buff 会加两遍（审核 R2A-N4）。要改列表就换一个名字。
       运行时定义的 effect 能被 buff 的附加效果、effect 的 buff_ids 和 effect_apply 使用，
       不能被技能引用（技能在职业加载时就解析好了）。out->count 是定义的 effect 数。
       buff_define、effect_define 都没有个数上限：每条定义在游戏里常驻到退出存档（effect 每个约 0x618 字节），
       不要每帧定义新名字（审核 R2A-N5）；DDCore 在退出存档时报个数 */
    uint32_t(DD_CALL* effect_define)(DDPluginHandle plugin, const char* text, DDCombatOpResult* out);

    /* 规则注册（D8），主线程，不能在规则回调里调用。名字最多 63 字节，已经注册（含哈希相同的不同名字）时返回 0。
       游戏内置的状态名 stunned、tagged、poisoned、bleeding、burning、virtued、afflicted、dazed（规则 13/42 自己会判断的，
       含哈希相同的）也返回 0：注册了就会接管原版数据里所有这类判定（审核 A-N3）。
       注册后下一个帧边界装上 479CD0 的 hook（没有注册时不装），撤销最后一个后拆除。 */
    DDRuleId(DD_CALL* rule_register)(DDPluginHandle plugin, const char* name, DDRuleFn fn, void* user);
    bool(DD_CALL* rule_unregister)(DDPluginHandle plugin, DDRuleId id);
    bool(DD_CALL* rule_stats)(DDRuleStats* out);

    /* 技能句柄：从事件里的执行者和技能指针（skill.pre/post 的 performer、skill）得到。技能不在执行者职业的表里时返回 false */
    bool(DD_CALL* skill_from_pointer)(const void* performer, const void* skill, DDSkillRef* out);
    /* 重新解析并读字段，解析失败返回 false */
    bool(DD_CALL* skill_info)(const DDSkillRef* ref, DDSkillInfo* out);
} DDCombatService;

/* ------------------------------------------------------------------ persist */

#define DD_SERVICE_PERSIST "persist"
#define DD_SERVICE_PERSIST_VERSION 1u

/*
 * 存档旁路（P4c）：插件的数据按存档保存在游戏存档旁边，读档时和游戏存档核对一致才交回。设计见 docs/02-architecture.md 的
 * 「persist 服务」，依据见 knowledge/dd1/campaign.md 的「存档」。
 *
 * 位置：<游戏根目录>\DDModLoader\save\<账号>\<设备名>\<插件名>\<key>.ddsave。账号是 SteamID32（离线版、取不到 Steam 时是 local），
 *   设备名是当前 profile（steam_profile_N、profile_N）。每次写入保留上一代 <key>.ddsave.prev。游戏选存档槽时把存档复制到
 *   backup/，旁路同时把这个设备的目录复制到 <设备名>\backup\；游戏读 backup 时旁路也读 backup。
 * 文件：文本头（格式版本、插件名、key、插件给的版本、设备名、账号、写入时间、是否在地牢和战斗中、快照时游戏文件的内容哈希）
 *   加插件的数据（字节串，带长度和校验和）。
 *
 * 声明：插件在 DDPlugin_Load 里调用 declare。插件名就是旁路的子目录名：不能以点结尾，不能是 backup、DDCore，和已经声明的插件
 *   不能只差大小写，否则返回 DD_PERSIST_E_INVALID。有插件声明时 DDCore 在 AppReady 装 4 个观察 hook（StorageManager 的排队写文件
 *   2487E0、读文件 248D60、选存档槽复制 backup 2923A0、退到桌面 3D1CC0），只读内存、算哈希、记录，不改游戏行为；没有插件声明时
 *   一个都不装。存档的读取早于 CAMPAIGN_LOAD，所以这些 hook 不能按需安装。
 *
 * 写：每次 campaign.save（见 events 一节）派发完所有订阅之后，DDCore 按插件加载顺序调用 on_save，插件在 on_save 或 campaign.save
 *   的回调里调用 save 交出数据（只有这两处能调用）。DDCore 在主线程记下数据和当时的游戏文件哈希（名册，在地牢里加 raid），
 *   后台线程写盘：同一个文件还没写的被新的替换（只写最新的），先写临时文件、FlushFileBuffers，再 ReplaceFileW。写失败（别的程序
 *   正占着文件）按退避重试，最多写 5 次（约 4 秒），重试用完才放弃并写 ERROR。退出存档时等 3 秒、退到桌面时等 10 秒，没写完或
 *   有放弃的写 WARN 并列出路径。某一次没有调用 save 的 key 保持原来的文件（下次读档多半不一致）。同一插件只差大小写的两个 key
 *   是同一个文件，后交的拒绝。
 *   这次进存档第一次写某个 key 之前，读档时判成 MISMATCH（或新战役）的，旁路里原来的两代先改名为 <key>.ddsave.mismatch-<时间>、
 *   <key>.ddsave.prev.mismatch-<时间>（新战役是 oldcampaign-<时间>），不再参与读档判断，每种留最近 2 份，要找回时手动改回名字。
 *   on_save 里不要做 combat 的写操作（按事件回调处理，排队到下一帧，和刚写的存档对不上）。
 * 读：CAMPAIGN_LOAD 回调里及之后（到 CAMPAIGN_UNLOAD 为止）调用 load。名册在 CAMPAIGN_LOAD 之前已经读完，DDCore 比较这次读到的
 *   名册哈希和旁路记录的：当前一代一致是 LOADED，否则上一代一致是 PREV，都不一致是 MISMATCH（不给数据，reason 写原因），没有文件
 *   是 EMPTY。还要求文件头里的插件名、key 和请求的完全相同，旁路的「地牢/城镇」和存档一致（CAMPAIGN_LOAD 时状态机的下一个状态是
 *   as_raid_start 就是地牢里的存档）。新战役（这次读到的名册就是 scripts/starting_save 的初始名册）两代都不一致时是 EMPTY，
 *   不是 MISMATCH。
 *   地牢里的存档：游戏在 CAMPAIGN_LOAD 之前（状态 1）已经读过一次探索存档 persist.raid.json，DDCore 拿它和名册一起判断两代，
 *   raid 不一致的一代不用（可能因此是 PREV）。数据照样带 raid_pending = 1：as_raid_start 加载结束后的帧边界拿 load_raid 读到的
 *   raid 再确认一次，结果经 on_verify 通知（怪物的数据这时才能恢复）。读档前没有读到 raid 时只按名册判断，raid 只在这一步核对。
 *   这一步不一致时已经给出的数据收不回来，DDCore 改为 MISMATCH，这次进存档里这个 key 的 save 返回 DD_PERSIST_E_INCONSISTENT、
 *   不再写，原来的两代保留（否则下一次存档会把来自不一致那一代的数据连同新的哈希写回去）。
 *   一次进存档里同一个 key 只判断一次，之后的 load 返回同样的结果。
 *
 * 线程：全部只在主线程调用。回调（on_save、on_verify）都在主线程，SEH 隔离，崩溃只写日志。
 */

typedef enum DDPersistStatus {
    DD_PERSIST_LOADED = 0,      /* 当前一代和存档一致 */
    DD_PERSIST_PREV = 1,        /* 当前一代不一致，上一代一致 */
    DD_PERSIST_EMPTY = 2,       /* 没有旁路文件（新战役、从没写过） */
    DD_PERSIST_MISMATCH = 3,    /* 有旁路文件但和存档不一致（或文件损坏），不给数据 */
    DD_PERSIST_UNAVAILABLE = 4, /* 这次读档不能判断（观察 hook 没装上、没观察到名册的读取、取不到账号或设备） */
} DDPersistStatus;

typedef enum DDPersistResult {
    DD_PERSIST_OK = 0,
    DD_PERSIST_E_INVALID = 1,       /* 参数无效（key 不合法、数据太大） */
    DD_PERSIST_E_NOT_DECLARED = 2,  /* 插件没有在 Load 里 declare */
    DD_PERSIST_E_TIMING = 3,        /* 时机不对：save 只在 campaign.save 回调和 on_save 里，declare 只在 AppReady 之前 */
    DD_PERSIST_E_THREAD = 4,        /* 不在主线程 */
    DD_PERSIST_E_NO_CAMPAIGN = 5,   /* 不在存档里 */
    DD_PERSIST_E_UNAVAILABLE = 6,   /* 持久化不可用（特征码缺失、hook 没装上） */
    DD_PERSIST_E_INCONSISTENT = 7,  /* save：这个 key 这次进存档的 raid 核对不一致（或归档失败），这次进存档不再写，原来的两代保留 */
} DDPersistResult;

#define DD_PERSIST_MAX_KEY 63                       /* key：字母数字和 _ . -，不能以点开头 */
#define DD_PERSIST_MAX_DATA (16u * 1024u * 1024u)   /* 一个 key 的数据上限 */

/* load 的输出。data 和 version 在 CAMPAIGN_UNLOAD 之前有效；reason 在 raid 核对时会更新，要保存就复制 */
typedef struct DDPersistData {
    uint32_t size;          /* 调用方写入 sizeof(DDPersistData) */
    uint32_t status;        /* DDPersistStatus */
    const char* reason;     /* 说明：不一致的原因、地牢里待核对等；没有时为空串 */
    const void* data;       /* LOADED / PREV 时是数据，否则为 NULL */
    uint32_t data_size;
    const char* version;    /* 写入时插件给的版本文字 */
    uint64_t written_ms;    /* 写入时间（Unix 毫秒） */
    bool in_raid;           /* 写入时在地牢里 */
    bool raid_pending;      /* 地牢里的存档：raid 还要等探索存档读完再核对，结果经 on_verify 通知 */
    bool from_backup;       /* 游戏这次读的是 backup/，旁路也读了 backup */
} DDPersistData;

/* on_save 的参数，和 campaign.save 的字段相同。字符串只在回调期间有效 */
typedef struct DDPersistSaveInfo {
    uint32_t size;
    const char* files;      /* 这组写了哪些文件，逗号分隔 */
    bool in_raid;
    bool in_battle;
    bool main_thread;
    uint32_t campaign_serial;
    uint32_t index;         /* 本次进存档的第几次 campaign.save，从 1 开始 */
} DDPersistSaveInfo;

/* on_verify 的参数：地牢存档的 raid 核对结果，每个 load 过、raid_pending 为 1 的 key 一次。字符串只在回调期间有效 */
typedef struct DDPersistVerify {
    uint32_t size;
    const char* key;
    uint32_t status;        /* 核对通过时是 load 时的结果（LOADED / PREV），不通过是 MISMATCH */
    const char* reason;
} DDPersistVerify;

typedef void(DD_CALL* DDPersistSaveFn)(const DDPersistSaveInfo* info, void* user);
typedef void(DD_CALL* DDPersistVerifyFn)(const DDPersistVerify* verify, void* user);

/* 诊断信息。字符串在下一次调用 info 之前有效 */
typedef struct DDPersistInfo {
    uint32_t size;          /* 调用方写入 sizeof(DDPersistInfo) */
    bool hooks;             /* 观察 hook 已装上 */
    uint32_t hook_count;
    const char* account;    /* SteamID32 或 local；还没取到时为空串 */
    const char* device;     /* 当前 profile 的设备名，存档外为空串 */
    const char* directory;  /* 当前旁路目录（这次读档用的，读 backup 时是 backup 目录），存档外为空串 */
    uint32_t saves;         /* 本次运行派发 campaign.save 的次数 */
    uint64_t written;       /* 写盘成功的文件数 */
    uint64_t merged;        /* 被后来的写入替换、没有写盘的次数 */
    uint64_t failed;        /* 重试用完放弃的次数（那份数据没写进去） */
    uint64_t retries;       /* 写失败后安排重试的次数 */
} DDPersistInfo;

typedef struct DDPersistService {
    DDServiceHeader header;
    /* DDPersistStatus 的文字，静态字符串 */
    const char*(DD_CALL* status_text)(uint32_t status);
    /* 声明本插件使用持久化，只在 AppReady 之前（DDPlugin_Load、EARLY）调用。on_save 不能为 NULL，on_verify 可为 NULL。
       重复调用替换回调。返回 DDPersistResult */
    uint32_t(DD_CALL* declare)(DDPluginHandle plugin, DDPersistSaveFn on_save, DDPersistVerifyFn on_verify, void* user);
    /* 读本插件 key 的旁路，结果写 out（out->status 等）。返回 DDPersistResult */
    uint32_t(DD_CALL* load)(DDPluginHandle plugin, const char* key, DDPersistData* out);
    /* 交出本插件 key 的数据（复制），同一次 campaign.save 里重复调用时后一次替换前一次。version 可为 NULL，最多 63 字节。
       返回 DDPersistResult */
    uint32_t(DD_CALL* save)(DDPluginHandle plugin, const char* key, const void* data, uint32_t size, const char* version);
    bool(DD_CALL* info)(DDPersistInfo* out);
} DDPersistService;

#ifdef __cplusplus
}
#endif

#endif /* DD_API_H */

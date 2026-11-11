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
 *   语义主题       damage.pre/post、effect.pre/post、skill.pre/post、hit.pre/post、
 *                  battle.round.start/end、battle.turn.start/end、stress.pre/post、heal.pre/post、
 *                  battle.start/end、actor.*、raid.*、town.*、hero.*、inventory.*、estate.currency、
 *                  app.tick、app.state、campaign.load/unload，以及插件自定义主题
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
 * 只有名字以 .pre 结尾的主题和 query.* 可写。对只读字段写入返回 DD_EV_E_READ_ONLY，
 * .pre 以外的主题调用 cancel 返回 DD_EV_E_NOT_CANCELLABLE。
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
 * 负载里已经按交换后的结果给出，压力和 HP 字段也读实际目标。
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
#define DD_SERVICE_GAME_VERSION 1u

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
} DDGameService;

#ifdef __cplusplus
}
#endif

#endif /* DD_API_H */

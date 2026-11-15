#pragma once
// combat 服务的游戏侧实现（P4a）：buff 的读写、定义注册、effect 的查询和施加、规则扩展、技能句柄，
// 以及 buff.add / buff.remove / buff.tick 主题。只在 DDCore 中编译；不依赖游戏的部分在 logic.h，单元测试覆盖。
//
// 依据：knowledge/dd1/combat.md（Buff、Effect、技能各节和函数表，P4a 新查的静态结论也在那里），探针 probe_p4
// 在用户场景里跑通过的调用方式（tests/plugins/probe_p4.cpp）。设计见 docs/adr/ 的 0025–0030、0033。
// 全部游戏调用都在主线程、SEH 保护下进行；hook 按需安装（没有订阅、没有规则注册时一个都不装）。

#include "logic.h"

#include "../events/system.h"
#include "../game/actors.h"
#include "../game/game.h"

#include <dd/dd_api.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ddc::combat {

struct CombatDeps {
    game::Addresses* addr = nullptr;
    events::System* events = nullptr;  // 可为空：buff.* 主题和规则 hook 不可用
    DWORD mainThread = 0;
    std::string gameRoot;              // UTF-8
    std::function<void(int level, const std::string& message)> log;
    std::function<std::string(std::uint32_t owner)> pluginName;
    std::function<game::ActorEnv()> actorEnv;  // 每次调用都取样当前战斗（同 game 服务）
    std::function<bool()> inFrameBody;         // DDCore 的每帧 hook 正在执行帧边界的逻辑
    std::function<bool()> campaignActive;
    std::function<bool()> loadingNow;          // 按当前状态机判断三条加载线程是否在运行
    // SEH 隔离的回调调用：返回 false 表示崩溃
    std::function<bool(DDCombatOpFn fn, const DDCombatOpResult* result, void* user, std::uint32_t owner)> invokeOp;
    // 规则回调：result 是回调返回的 DDRuleResult
    std::function<bool(DDRuleFn fn, const DDRuleContext* context, void* user, std::uint32_t* result)> invokeRule;
    // 额外的派发层数（persist 的保存回调期间为 1：写操作按事件回调处理，排队）
    std::function<int()> extraDepth;
};


// 服务入口复制好的参数（插件指针在 core.cpp 里经 SEH 复制）
struct AddRequest {
    DDActorRef actor{};
    std::string id;
    DDBuffAddOptions options{};
    std::string tag;
};

struct RemoveRequest {
    DDActorRef actor{};
    DDBuffRemoveSpec spec{};
    std::string text;
};

struct EffectRequest {
    std::string name;
    DDActorRef performer{};
    DDActorRef target{};
    bool hasTarget = false;
};

class Combat
{
public:
    explicit Combat(CombatDeps deps);
    ~Combat();
    Combat(const Combat&) = delete;
    Combat& operator=(const Combat&) = delete;

    // 服务 ---------------------------------------------------------------------
    bool SetOpHandler(std::uint32_t owner, DDCombatOpFn fn, void* user);
    bool SafePoint();
    // 读接口现在能不能用（主线程，没有加载线程在跑；R2A-S1）
    bool ReadReady();

    bool BuffList(const DDActorRef& actor, std::uint32_t* guids, std::uint32_t capacity, std::uint32_t* count);
    bool BuffGet(const DDActorRef& actor, std::uint32_t guid, DDBuffInfo* out);
    bool BuffDefGet(const std::string& id, DDBuffDef* out);

    std::uint32_t BuffAdd(std::uint32_t owner, AddRequest request, DDCombatOpResult* out);
    std::uint32_t BuffRemove(std::uint32_t owner, RemoveRequest request, DDCombatOpResult* out);
    std::uint32_t BuffDefine(std::uint32_t owner, const DDBuffDef& def, DDCombatOpResult* out);

    bool EffectGet(const std::string& name, DDEffectInfo* out);
    std::uint32_t EffectApply(std::uint32_t owner, EffectRequest request, DDCombatOpResult* out);
    std::uint32_t EffectDefine(std::uint32_t owner, std::string text, DDCombatOpResult* out);

    DDRuleId RuleRegister(std::uint32_t owner, const std::string& name, DDRuleFn fn, void* user);
    bool RuleUnregister(std::uint32_t owner, DDRuleId id);
    bool RuleStats(DDRuleStats* out);

    bool SkillFromPointer(const void* performer, const void* skill, DDSkillRef* out);
    bool SkillInfo(const DDSkillRef& ref, DDSkillInfo* out);

    // DDCore 的时机 -------------------------------------------------------------
    // 每帧（帧边界，事件系统同步过 hook、派发过延迟事件之后）：执行排队的操作，统计规则耗时，清理过期的标签
    void OnFrameBoundary();
    void OnCampaignLoad();
    void OnCampaignUnload();
    void OnPluginRollback(std::uint32_t owner);
    // persist（P4c1）：标签表随存档保存。导出当前能解析的 actor 上、条目还在的标签；导入只认 kinds 里的种类
    // （1u << DD_ACTOR_HERO 等），按当前存档重新取句柄，条目不在或 id 对不上、插件没有加载的丢弃，notes 写丢弃的原因
    std::vector<TagRecord> ExportTags();
    std::size_t ImportTags(const std::vector<TagRecord>& records, std::uint32_t kinds,
        const std::function<std::uint32_t(const std::string& plugin, bool* found)>& ownerOf, std::vector<std::string>* notes);
    // hook 报告里的一行
    std::string Summary() const;
    // DDCombatResult 的文字
    static const char* ResultText(std::uint32_t code);

    struct Impl;  // detour 需要访问

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddc::combat

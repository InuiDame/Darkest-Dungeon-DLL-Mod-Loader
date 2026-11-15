#pragma once
// P4b：Effect 与 Buff 的 Lua API。只经 SDK 的 combat 服务（ADR 0003），设计见 docs/03-lua-system.md 的需求三和
// ADR 0025、0027、0028、0029（下面的 D4–D8 是 P4 简报的设计编号，对应见 docs/history/p4-review-findings.md）。
//
// - dd.buff：define（D4 的 POD 字段，带 on_apply/on_remove/on_tick/on_damage 时是脚本 Buff）、def、enum、state、rule。
//   dd.effect：get（effect 句柄）、define（D5 的文本）、register（D6 的脚本 Effect，建在 effect.pre 上，名字在 C++ 里匹配）。
//   actor 句柄的方法：buffs、buff、add_buff、remove_buffs（actor.cpp 的 __index 转到这里）。
// - 写操作的返回约定：参数写错直接报错；被拒绝（存档外、加载中、PvP、id 不存在、队列满、城镇里施加 effect……）返回 nil 和原因；
//   接受时返回 true 和结果：立即执行时结果是 guid（添加）、删掉的条数（移除）、新增 buff 的表（施加）、id / 定义的个数（定义），
//   排队时结果是 nil，opts.done 在执行后收到 (true, 结果) 或 (nil, 原因)（只有排队的才调用 done）。
// - 标签：DDLua 对 DDCore 是一个插件，所有 Lua Mod 共用它的标签空间，所以交给 DDCore 的标签是「<mod id>:<tag>」，
//   不给 tag 时是「<mod id>:」（这样能认出条目是哪个 Mod 加的）。两部分合计最多 63 字节。
// - 脚本 Buff 的状态：每条载体（actor，组 guid）一张 Lua 表，键是 actor 的标识和 guid。绑定的时机：
//   立即执行的 add_buff 在同步派发的 buff.add 里（hook 还没装上时在 buff_add 返回之后）；排队的在操作结果里；
//   游戏或别的插件加的同 id 载体在 buff.add 里（空表）；读档恢复的载体在第一次用到时（空表，不调用 on_apply）。
//   buff.remove 时调用 on_remove 并丢弃；会话进行中 actor 不再能解析（怪物随战斗销毁、英雄被解雇）时，每秒的清理调用
//   on_remove(..., "gone") 并丢弃；拆除（退出存档、重载拆除 Mod）时不调用 on_remove，状态直接丢弃（载体还在游戏存档里，
//   由 P4c 的旁路保存状态）。
// - 规则（D8）：dd.buff.rule(name, fn) 注册到 DDCore，fn(self, target, buff) 返回布尔。回调里只读：写操作、注册类的函数报错。
//   预算 rule_budget_ms；连续出错 max_failures 次或超预算后停用。出错、超预算、停用中都返回 DD_RULE_ORIGINAL（交还游戏原来的判定，
//   和没有注册时一样，P4c1 的遗留修正），停用的下一帧向 DDCore 撤销。
// - 嵌套：立即执行的写操作会同步派发 buff.add 等，回调是隔离的嵌套入口（vm.h）。调用 SDK 之后用 AfterReentry 检查嵌套的
//   回调里有没有发生原生异常。

#include <dd/dd_api.h>
#include <lua.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ddlua {

class Session;
struct Mod;
struct Host;

namespace combat {

// combat 服务可用时返回它
const DDCombatService* Service(const Host& host);
// 加载线程在跑（读接口暂时读不到）时返回以「加载中」开头的原因，否则返回 nullptr（dd.actor 的枚举也用它，F5）
const char* LoadingReason(const Host& host);
// 事件负载的指针字段：effect 句柄、技能句柄（压栈，可能抛内存错误）
void PushEffect(lua_State* L, const char* name);
void PushSkill(lua_State* L, const DDSkillRef& ref);
// actor 句柄的方法（actor.cpp 的 __index 压这些函数，调用方是 Session::CurrentMod）
int ActorBuffs(lua_State* L);
int ActorBuff(lua_State* L);
int ActorAddBuff(lua_State* L);
int ActorRemoveBuffs(lua_State* L);

}  // namespace combat

class CombatBinding
{
public:
    explicit CombatBinding(Session& session);
    ~CombatBinding();  // 撤销规则和内部订阅，丢弃状态和排队记录（不运行 Lua；VM 损坏时不碰 Lua 引用）
    CombatBinding(const CombatBinding&) = delete;
    CombatBinding& operator=(const CombatBinding&) = delete;

    // 元表（dd.effect 句柄、dd.skill 句柄、dd.rule），VM 创建时调用一次（保护模式内）
    static void RegisterMetatables(lua_State* L);
    // dd.buff、dd.effect，装进 dd 表（保护模式内）
    static void AddFunctions(lua_State* L, int dd, std::uint64_t token);

    // Mod 拆除时（不运行 Lua）：撤销它的规则，丢弃它的脚本 Buff 定义、状态、排队记录、effect 归属
    void ReleaseMod(const Mod& mod);
    // 每帧（Session::Tick）：撤销停用了的规则；sweep 为真时（每秒）清理不能解析的 actor 的状态
    void Tick(bool sweep);
    // 拆除之前（VM 可用、Mod 还在）：丢弃全部状态，不调用 on_remove（F7）
    void BeforeTeardown();

    // 本 Mod 排队等结果的操作、规则、脚本 Buff 定义的数目（max_per_mod 的计数）
    std::size_t PendingFor(const Mod* mod) const;
    std::size_t RulesFor(const Mod* mod) const;
    std::size_t StatesFor(const Mod* mod) const;
    std::size_t InternalSubscriptions() const;

    // ---- 给 P4c 的接口：脚本 Buff 状态的导出和导入 ----
    struct StateRecord {
        std::string mod;     // 定义这个脚本 Buff 的 Lua Mod
        std::string buffId;
        DDActorRef actor{};
        std::uint32_t guid = 0;
        std::string json;    // state_json::Encode 的结果
    };
    // 导出全部状态（不能序列化的记一条错误，跳过）。在 Lua 代码之外调用（会进保护模式）。
    // perModBytes 不为 0 时每个 Mod 的状态累计到这么多字节就停下，这个 Mod 记进 overLimit、它的状态都不导出（存档时按
    // store_max_kb 传，编码的工作量有上限，审核 B-N6）
    std::vector<StateRecord> ExportStates(std::vector<std::string>* errors, std::size_t perModBytes = 0, std::vector<std::string>* overLimit = nullptr);
    // 导入一条状态：mod 必须定义了 buffId 的脚本 Buff，条目必须存在且 id 一致；已有的状态被替换（不调用 on_apply）
    bool ImportState(const StateRecord& record, std::string* error);

    // SDK 回调入口
    void OnOpResult(const DDCombatOpResult& result);
    // 返回 DDRuleResult：回调正常返回布尔时是 DD_RULE_TRUE / DD_RULE_FALSE，其余（出错、超预算、停用、VM 不可用）是 DD_RULE_ORIGINAL
    std::uint32_t OnRule(std::uint64_t key, const DDRuleContext& context);
    void NoteNativeFault(unsigned long code, const void* address);

    // Lua C 函数用（combat.cpp 内部）
    struct Impl;
    Impl& impl() { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddlua

#pragma once
// combat 服务里不依赖游戏的部分：枚举表、字符串哈希、参数校验、写操作的时机判断、操作队列、标签表、
// 规则注册表、effect 文本的名字提取。DDCore 和单元测试（tests/unit）用同一份源码。
// 依据：knowledge/dd1/combat.md 的「Buff 结构」「Buff 规则」，设计见 ADR 0025（写操作的时机）、0028（运行时定义）、
// 0029（规则扩展）。下面的 D3、D8 是 P4 简报的设计编号，和 ADR 的对应见 docs/history/p4-review-findings.md。

#include <dd/dd_api.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ddc::combat {

// 游戏的字符串哈希 h = h*53 + c（无符号 32 位），buff id、规则字符串、effect 名字都用它
std::uint32_t GameHash(std::string_view text);

// ---- 枚举表 ----
// 名字和数值来自游戏的静态初始化函数（stat 75820、source 78500、duration 78FB0、rule 79340，Steam 版 RVA），
// tools/check_actor_layout.py 离线核对两个 exe。source 的名字在游戏里带前缀 bsrc_，这里去掉，查找时两种写法都认
std::uint32_t EnumCount(std::uint32_t table);
const char* EnumName(std::uint32_t table, std::int32_t value);  // 超界或表号不对返回 nullptr
std::int32_t EnumValue(std::uint32_t table, std::string_view name);  // 找不到返回 -1
// 来源的 DD_SOURCE_F_* 标志（来源表 +0x44、+0x46、+0x48，静态初始化 78500 写的常量，check_actor_layout.py 核对），超界返回 0
std::uint32_t SourceFlags(std::int32_t source);

// ---- 参数校验 ----
inline constexpr std::size_t kTextMax = 63;  // 游戏里 char[64] 字段的最大长度
bool ValidText(std::string_view text, bool allowEmpty);
// stat_sub_type 按 stat 校验：combat_stat_add/multiply、resistance、activity_side_effect_chance、disable_combat_skill_attribute
// 必须是游戏认的名字；压力和治疗类的可以为空或是游戏认的来源；其余 stat 不限（见 logic.cpp 的表）
bool ValidateSubStat(std::int32_t stat, std::string_view sub, std::string* why);
// buff 定义的 POD 字段（含 ValidateSubStat），失败时 why 写原因
bool ValidateBuffDef(const DDBuffDef& def, std::string* why);
// 添加选项（flags 里没有的字段不检查）
bool ValidateAddOptions(const DDBuffAddOptions& options, std::string* why);
// 移除条件
bool ValidateRemoveSpec(const DDBuffRemoveSpec& spec, std::string* why);

// ---- 写操作的时机（D3）----
struct TimingState {
    bool mainThread = false;
    bool inRule = false;          // 规则回调里：一律拒绝，不排队
    bool campaignActive = false;
    bool loading = false;         // 三条加载线程之一正在运行
    bool pvp = false;
    bool inFrameBody = false;     // DDCore 的每帧 hook 正在执行帧边界的逻辑（游戏代码不在栈上）
    int dispatchDepth = 0;        // DDCore 正在派发的事件层数
    int gameCallDepth = 0;        // DDCore 自己调用游戏函数的层数（槽位 13/14、4CB4D0 等）
};

enum class Timing {
    Execute,  // 安全点，立即执行
    Queue,    // 事件回调里或游戏函数里，排到下一个帧边界
    Reject,   // 拒绝，code 是原因
};

struct TimingDecision {
    Timing timing = Timing::Reject;
    std::uint32_t code = DD_COMBAT_E_INVALID;  // Reject 时的 DDCombatResult
};

// needCampaign：buff、effect 的读写都要在存档里（actor 句柄只在存档里能解析）
TimingDecision DecideTiming(const TimingState& state, bool needCampaign);
// 是否在安全点（给 combat.safe_point）
bool IsSafePoint(const TimingState& state);

// ---- 操作队列 ----
enum class OpKind : std::uint32_t {
    BuffAdd = DD_COMBAT_OP_BUFF_ADD,
    BuffRemove = DD_COMBAT_OP_BUFF_REMOVE,
    EffectApply = DD_COMBAT_OP_EFFECT_APPLY,
    BuffDefine = DD_COMBAT_OP_BUFF_DEFINE,
    EffectDefine = DD_COMBAT_OP_EFFECT_DEFINE,
};

struct QueuedOp {
    std::uint64_t id = 0;
    std::uint32_t owner = 0;
    OpKind kind = OpKind::BuffAdd;
    DDActorRef actor{};
    DDActorRef actor2{};      // effect_apply 的 target
    std::string text;         // buff id / effect 名字 / effect 文本 / 移除用的 tag 或 id
    std::string tag;          // buff_add 的标签
    DDBuffAddOptions add{};   // tag 指针不用，见上
    DDBuffRemoveSpec remove{};  // text 指针不用，见上
    DDBuffDef def{};
};

class OpQueue
{
public:
    explicit OpQueue(std::size_t perOwnerLimit) : limit_(perOwnerLimit) {}

    // 成功返回操作号（从 1 开始），超过该插件的上限返回 0
    std::uint64_t Push(QueuedOp op);
    // 取出当前全部操作（按入队顺序），执行期间新入队的留到下一次
    std::vector<QueuedOp> TakeAll();
    // 丢弃某个插件的全部操作（插件回滚），返回丢弃的条数
    std::size_t DropOwner(std::uint32_t owner);
    std::size_t Size() const { return items_.size(); }
    std::size_t OwnerCount(std::uint32_t owner) const;
    std::size_t Limit() const { return limit_; }

private:
    std::size_t limit_;
    std::uint64_t nextId_ = 1;
    std::deque<QueuedOp> items_;
    std::map<std::uint32_t, std::size_t> perOwner_;
};

// ---- 标签表：(actor, 组 guid) → (插件, 标签) ----
// 不装 hook：组 guid 在一个 actor 上只增不减（Actor+0x478，随存档恢复），条目被删后同一个 guid 不会再出现，
// 所以查询时发现条目不在了就当作过期删掉；actor 不再能解析（解雇、怪物随战斗销毁）时在战斗结束和拆除时清掉
struct TagEntry {
    std::uint32_t owner = 0;
    std::string tag;
};

class TagTable
{
public:
    void Set(const DDActorRef& actor, std::uint32_t guid, std::uint32_t owner, std::string tag);
    const TagEntry* Get(const DDActorRef& actor, std::uint32_t guid) const;
    bool Erase(const DDActorRef& actor, std::uint32_t guid);
    // 这个 actor 上属于 owner、标签为 tag 的全部 guid（升序）
    std::vector<std::uint32_t> Find(const DDActorRef& actor, std::uint32_t owner, std::string_view tag) const;
    // 这个 actor 上不在 live 里的 guid 全部删掉（live 必须升序），返回删掉的条数
    std::size_t PruneActor(const DDActorRef& actor, const std::vector<std::uint32_t>& live);
    // 按条件删除整个 actor 的记录（例如 actor 不再能解析），返回删掉的条数
    std::size_t PruneIf(const std::function<bool(const DDActorRef&)>& gone);
    std::size_t DropOwner(std::uint32_t owner);
    void Clear() { map_.clear(); }
    std::size_t Size() const { return map_.size(); }
    // 逐条访问（persist 的导出用）
    void ForEach(const std::function<void(const DDActorRef& actor, std::uint32_t guid, const TagEntry& entry)>& fn) const;

private:
    struct Key {
        std::uint32_t kind, id, campaign, raid, battle, roster, guid;
        bool operator<(const Key& o) const;
    };
    static Key MakeKey(const DDActorRef& actor, std::uint32_t guid);
    static bool SameActor(const Key& key, const DDActorRef& actor);
    static DDActorRef ToRef(const Key& key);
    std::map<Key, TagEntry> map_;
};

// ---- 标签表随存档保存（P4c1）----
// 一条标签的存档形式：actor 的标识（英雄 hero id + 名册；怪物 guid）、组 guid、条目的 buff id（恢复时核对）、插件名、标签
struct TagRecord {
    std::uint32_t kind = 0;
    std::uint32_t id = 0;
    std::uint32_t roster = 0;
    std::uint32_t guid = 0;
    std::string buffId;
    std::string plugin;
    std::string tag;
};

// 游戏侧提供的查询（DDCore 读 buff 表，单元测试用假的）
struct TagLookup {
    std::function<bool(std::uint32_t kind, std::uint32_t id, DDActorRef* out)> findActor;  // 按当前存档重新取句柄
    std::function<bool(const DDActorRef& actor, std::uint32_t guid, std::string* buffId)> entry;  // 条目在时写 id 返回 true
    std::function<std::string(std::uint32_t owner)> pluginName;
    std::function<std::uint32_t(const std::string& plugin, bool* found)> ownerOf;  // 只认加载成功的插件
};
// 导出：actor 还能解析、条目还在的标签
std::vector<TagRecord> CollectTags(const TagTable& tags, const TagLookup& lookup);
// 导入：只认 kinds 里的种类（1u << DD_ACTOR_HERO 等），备用名册上的英雄、actor 找不到、条目不在、id 对不上、插件没有加载的丢弃，
// notes 写丢弃的原因。返回恢复的条数
std::size_t RestoreTags(TagTable& tags, const std::vector<TagRecord>& records, std::uint32_t kinds, const TagLookup& lookup,
    std::vector<std::string>* notes);

// ---- 规则注册表（D8）----
struct RuleEntry {
    DDRuleId id = 0;
    std::uint32_t owner = 0;
    std::uint32_t hash = 0;
    std::string name;
    DDRuleFn fn = nullptr;
    void* user = nullptr;
    mutable std::atomic<bool> disabled{ false };  // 回调崩溃后停用，等同没有注册（hook 里经只读快照置位）
};

// hook 里读的只读快照：按哈希升序，查找不加锁
struct RuleSnapshot {
    std::vector<std::shared_ptr<RuleEntry>> entries;  // 按 hash 升序
    const RuleEntry* Find(std::uint32_t hash) const;
};

class RuleRegistry
{
public:
    RuleRegistry();
    // 名字为空、过长、已经注册（含哈希相同的不同名字）时返回 0，why 写原因
    DDRuleId Register(std::uint32_t owner, std::string_view name, DDRuleFn fn, void* user, std::string* why);
    bool Unregister(std::uint32_t owner, DDRuleId id);
    std::size_t DropOwner(std::uint32_t owner);
    std::size_t Count() const;
    // 当前快照（hook 里用）。快照一旦发布就不释放（注册变动很少，内存可以忽略），读的一方不用加锁
    const RuleSnapshot* Snapshot() const { return current_.load(std::memory_order_acquire); }

private:
    void Publish();
    std::vector<std::shared_ptr<RuleEntry>> entries_;
    std::vector<std::unique_ptr<RuleSnapshot>> retired_;
    std::atomic<const RuleSnapshot*> current_{ nullptr };
    DDRuleId nextId_ = 1;
};

// 规则回调的返回值（DDRuleResult）：DD_RULE_TRUE / DD_RULE_FALSE 时返回 true，*result 是按 is_false_rule 取反后的结果；
// DD_RULE_ORIGINAL 和其他值返回 false，表示交还游戏原来的判定（P4c1 的遗留修正）
bool InterpretRule(std::uint32_t raw, bool falseRule, bool* result);

// 规则判定的统计（命中脚本规则的那部分）
struct RuleStatsAccum {
    std::uint64_t hits = 0;
    std::uint64_t passed = 0;    // 回调交还原函数（DD_RULE_ORIGINAL）
    std::uint64_t errors = 0;
    std::uint64_t skipped = 0;   // 命中但不在主线程、PvP、停用，按没有注册处理
    double totalUs = 0.0;
    double maxCallUs = 0.0;
    double frameUs = 0.0;        // 本帧累计
    double maxFrameUs = 0.0;
    std::uint64_t frameHits = 0;
    std::uint64_t maxFrameHits = 0;
    void Add(double us);
    void EndFrame();
    void Reset() { *this = RuleStatsAccum{}; }
};

// ---- effect 文本 ----
// 按游戏解析器（2901B0 去注释、2904D0 分块、36F020 取字段，见 logic.cpp）切出来的一块
struct EffectBlock {
    std::size_t offset = 0;  // 在去掉 // 和 /* */ 注释之后的文本里的位置
    std::size_t length = 0;  // 游戏复制进 0x400 缓冲的字节数（含块里的 # 注释和空白）
    std::string label;       // 冒号前的标识符
    bool effect = false;     // 以 "effect:" 开头（4F02D0 只处理这种块）
    bool hasName = false;
    std::string name;        // 游戏会用的 .name（块里最后一次出现），没有截断
    std::string text;        // 块的内容（游戏复制进缓冲的那一段）
};
// 块的最大长度：2904D0 用 strncpy_s(reader, 0x400, 块, 块长) 复制，块长 >= 0x400 时进程被 CRT 终止
inline constexpr std::size_t kEffectBlockMax = 0x3FF;
// 按游戏的规则分块，并拒绝会让游戏崩溃、死循环或结果依赖区域设置的文本（见 logic.cpp 的说明），why 写原因
bool ParseEffectText(std::string_view text, std::vector<EffectBlock>* blocks, std::string* why);
// 提取文本里每个 effect: 块游戏会用的 .name。除 ParseEffectText 的检查以外，有不是 effect: 的块、块没有 .name、
// 名字为空或超过 63 字节、同一段文本里重名（或哈希相同）时返回 false
bool ExtractEffectNames(std::string_view text, std::vector<std::string>* names, std::string* why);

// 定义的归属：名字（或 id）哈希 → 插件。每次进存档清空（Buff 库、effect map 每次读档重建，P4-0C 探针 0A-10、0A-11）
class OwnerTable
{
public:
    // 0 表示没有记录
    std::uint32_t OwnerOf(std::uint32_t hash) const;
    void Set(std::uint32_t hash, std::uint32_t owner, std::string name);
    const std::string* NameOf(std::uint32_t hash) const;
    void Clear() { map_.clear(); }
    std::size_t Size() const { return map_.size(); }

private:
    std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> map_;
};

}  // namespace ddc::combat

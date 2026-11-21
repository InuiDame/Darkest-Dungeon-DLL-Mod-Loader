#pragma once
// 布局清单的运行时核对（U1b，ADR 0036）：特征码解析、依据（layout.inc 的 DD_EVIDENCE）求值、按依赖表停用能力。
//
// 语义和 tools/ddupdate 的 sigs.resolve、evidence.evaluate 一致（权威说明在 layout.inc 开头），两边对同一个映像给出
// 同样的结论：tests/layout_compare 把 exe 按节映射（不执行）跑这里的代码，tools/ddupdate/cpp_compare.py 和
// dd_update.py verify --json 逐项比较。所以这里只依赖 PE 格式，不用 RtlLookupFunctionEntry、不依赖 DDCore 的其他模块，
// 映像可以是已加载的模块，也可以是自己映射的文件。
//
// 有两处 Python 做而这里不做：HOOK 偷取范围的指令检查和 hooks.md 的重叠检查（要反汇编，运行时由 hook 引擎在安装时
// 校验入口），以及 DD_ENTRY 和入口字节的比较（运行时入口可能已被旧式 DLL 改过，hook 引擎会串联）。对照测试把 Python
// 只因这些检查判失效的特征码单列（cpp_compare.py 的说明）。HOOK 的命中位置必须是函数入口（sigs.py 的 check_hook 里
// 「命中位置不是函数入口」）这一条两边都做（U1 修复 FA，CheckHookEntry），已知版本和未知版本都做。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ddc::game::check {

// ---------------------------------------------------------------- 映像

struct Section {
    std::string name;
    std::uint32_t begin = 0;  // VirtualAddress
    std::uint32_t end = 0;    // VirtualAddress + VirtualSize
};

// 按 RVA 排布的映像（RVA 0 就是 base）。pointerBase：映像里的绝对指针（虚表项）减去它得到 RVA，已加载的模块是实际基址
// （指针已重定位），没有做重定位的映射是 OptionalHeader.ImageBase
struct ImageView {
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;  // SizeOfImage
    std::uint64_t pointerBase = 0;
    std::uint64_t preferredBase = 0;  // OptionalHeader.ImageBase
    std::uint32_t textBegin = 0;
    std::uint32_t textEnd = 0;
    std::uint32_t exceptionRva = 0;   // DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]
    std::uint32_t exceptionSize = 0;
    std::vector<Section> sections;

    bool InText(std::int64_t rva) const { return rva >= textBegin && rva < textEnd; }
    const char* SectionOf(std::int64_t rva) const;
};

// 从 PE 头填 ImageView。mem 按 RVA 排布，至少 SizeOfImage 字节可读
bool DescribeImage(const std::uint8_t* mem, std::uint64_t pointerBase, ImageView* out, std::string* error);

// 异常目录里的函数表：表项按 BeginAddress 排序（跳过为 0 的），沿间接表项（UnwindData 低位为 1）和
// UNW_FLAG_CHAININFO 找主函数（最多 32 次），主函数相同的表项是同一个函数的分块
class FunctionTable
{
public:
    explicit FunctionTable(const ImageView& image);
    // 包含 rva 的函数的全部分块 [begin, end)，按地址排序；rva 不在任何表项里时返回 false
    bool Ranges(std::int64_t rva, std::vector<std::pair<std::uint32_t, std::uint32_t>>* out) const;
    // 包含 rva 的表项的主函数起点（image.py 的 enclosing_function）；rva 不在任何表项里时返回 false
    bool Enclosing(std::int64_t rva, std::uint32_t* primary) const;
    // rva 是某个表项的 BeginAddress，并且这个表项的主函数就是它自己（image.py 的 is_function_start）
    bool IsFunctionStart(std::int64_t rva) const;
    std::size_t EntryCount() const { return entries_.size(); }

private:
    struct Entry {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        std::uint32_t unwind = 0;
        std::uint32_t primary = 0;
    };
    std::uint32_t PrimaryOf(Entry entry) const;

    const ImageView& image_;
    std::vector<Entry> entries_;                                // 按 begin 排序、begin 唯一
    std::vector<std::pair<std::uint32_t, std::uint32_t>> byPrimary_;  // (主函数, 表项下标)，排序
};

// ---------------------------------------------------------------- 特征码扫描

// "48 8B ?? 05" 形式，?? 是通配（-1）。anchor 是扫描时用 memchr 找的那个固定字节（.text 里最少见的）
struct Pattern {
    std::vector<int> bytes;
    std::size_t anchor = 0;
    bool ok = false;
};

class Scanner
{
public:
    // 采样 .text 的字节频率，给每个特征码挑最少见的固定字节当锚
    explicit Scanner(const ImageView& image);
    bool Compile(const char* text, Pattern* out) const;
    // [begin, end) 内完整落在范围里的全部命中（每个字节位置都试，可以重叠），按地址升序追加到 hits
    void Scan(const Pattern& pattern, std::int64_t begin, std::int64_t end, std::vector<std::uint32_t>* hits) const;

private:
    const ImageView& image_;
    std::uint32_t freq_[256] = {};
};

// ---------------------------------------------------------------- 清单（编译期从 signatures.inc、layout.inc 生成）

enum class SigKind { Hook, Global, GlobalAny, Code };
enum class SigNeed { Required, Loader, Optional, Anchor };

struct SigDef {
    const char* id;
    SigKind kind;
    SigNeed need;
    std::uint32_t steal;
    std::int32_t capture;
    const char* pattern;
    const char* deps = "";      // DD_DEPS
    bool runtimeSelect = false; // DD_RUNTIME_SELECT
};

enum class ItemNeed { Required, Optional };
enum class ItemCheck { Runtime, Offline, None };

struct ItemDef {
    const char* id;
    const char* type;  // OFFSET、SIZE……（只用于日志）
    std::int64_t value;
    ItemNeed need;
    ItemCheck check;
    const char* deps;
};

enum class Origin { Hit, Target, HitValue, VSlot, VSlotValue, NoSig };
enum class Where { Entry, Window, Func, Text };
enum class Read { None, U8, U16, U32, U64, I8, I32, DerefU32, Rel32 };

struct EvidenceDef {
    const char* item;  // 清单项，或者特征码名字（特征码的同一性依据）
    const char* sig;   // NONE 表示没有起点
    Origin origin;
    std::int32_t arg;
    Where where;
    std::uint32_t span;
    const char* pattern;
    std::uint32_t at;
    Read read;
    std::int64_t mul;
    const char* rel;   // NONE、清单项或特征码
    std::int64_t add;
    std::uint32_t index = 0;  // 同一项里的序号（从 0 起），和 verify 的 JSON 一致
};

const std::vector<SigDef>& Signatures();
const std::vector<ItemDef>& Items();
const std::vector<EvidenceDef>& Evidence();
int FindSig(std::string_view id);   // 没有时 -1
int FindItem(std::string_view id);  // 没有时 -1
std::string Describe(const EvidenceDef& ev);

// ---------------------------------------------------------------- 特征码解析

struct SigResult {
    bool scanned = false;   // false：没有扫描（DDCore 里的 LOADER，入口已被加载器打了补丁）
    bool ok = false;
    std::uint32_t hits = 0;
    bool hasHit = false;    // 命中位置（GLOBAL_ANY 是最低的命中）
    std::uint32_t hit = 0;
    bool hasResolved = false;
    std::uint32_t resolved = 0;  // HOOK/CODE 是命中位置，GLOBAL 是 rip 相对的目标，GLOBAL_ANY 是选中的候选
    bool selected = false;       // DD_RUNTIME_SELECT 按内容选中了候选
    std::uint32_t selectedCount = 0;  // 选中的类名表的项数
    std::vector<std::uint32_t> candidates;
    std::string problem;
};

// HOOK 的入口检查（sigs.py 的 check_hook 里同名的一条）：命中位置落在异常目录记录的某个函数里、却不是那个函数的入口时
// 返回问题说明，否则返回空。编译器在原来的序言前面插了指令（栈探测、提前返回的判断）时，带通配的特征码可能命中挪下去的
// 旧序言，hook 会装在函数中间。不在任何函数里的命中不算问题（和 Python 一样，那边另外记一笔）
std::string CheckHookEntry(const FunctionTable& table, std::uint32_t hit);

// 按 Signatures() 的顺序解析全部特征码（和 sigs.resolve 同义，另加 CheckHookEntry）。scanLoader 为 false 时 LOADER 不扫描。
// table 是这个映像的函数表（RunCheck 可以共用同一个），为空时临时建一个
std::vector<SigResult> ResolveSignatures(const ImageView& image, const Scanner& scanner, bool scanLoader,
    const FunctionTable* table = nullptr);

// ---------------------------------------------------------------- 依据求值

enum class EvStatus { Ok, Fail, Blocked };  // Blocked：依赖的特征码失效（起点或 rel）

struct EvidenceResult {
    EvStatus status = EvStatus::Fail;
    bool hasOrigin = false;
    std::int64_t origin = 0;
    std::uint32_t hits = 0;
    bool hasHit = false;
    std::uint32_t hit = 0;
    bool hasCaptured = false;
    std::int64_t captured = 0;
    bool hasExpected = false;
    std::int64_t expected = 0;
    bool hasImplied = false;
    std::int64_t implied = 0;
    std::string detail;
    double micros = 0;  // 求值耗时
};

const char* StatusText(EvStatus status);

class Evaluator
{
public:
    // table 为空时第一次用到 FUNC 范围才建函数表（耗时记在 FunctionTableMicros）；给了就共用，耗时由调用方自己记
    Evaluator(const ImageView& image, const Scanner& scanner, const std::vector<SigResult>& sigs,
        const FunctionTable* table = nullptr);
    EvidenceResult Evaluate(const EvidenceDef& ev);
    double FunctionTableMicros() const { return tableMicros_; }

private:
    const FunctionTable& Table();

    const ImageView& image_;
    const Scanner& scanner_;
    const std::vector<SigResult>& sigs_;
    const FunctionTable* shared_ = nullptr;
    std::unique_ptr<FunctionTable> table_;
    double tableMicros_ = 0;
};

// ---------------------------------------------------------------- 依赖表和停用开关

// 依赖表的词汇（layout.inc 开头、tools/ddupdate/manifest.py 的 DEPENDENTS）。writes：能力会写游戏内存、调用游戏函数
// 或改变游戏的结算（ADR 0036 的「写」；装内联 hook、改虚表槽也算，U1 修复 FA），依赖的布局项不能核对时停用；其余是只读
// 能力，警告后照常。core 和 loader 是例外，见 BuildGate
enum class Dep : std::uint8_t {
    Core,
    Loader,
    EventsNative,
    EventsQuery,
    Pvp,
    GameActor,
    GameActorInfo,
    GameActorList,
    GameBattle,
    GameMods,
    CombatBuffRead,
    CombatBuffAdd,
    CombatBuffRemove,
    CombatBuffDefine,
    CombatEffectGet,
    CombatEffectApply,
    CombatEffectDefine,
    CombatRule,
    CombatSkill,
    CombatEnum,
    Persist,
    Check,
    Count
};

struct DepInfo {
    const char* word;
    bool writes;
};

const DepInfo& InfoOf(Dep dep);
bool ParseDep(std::string_view word, Dep* out);

// 停用开关的状态。只在启动时写（单线程），之后各服务只读
class Gate
{
public:
    bool Off(Dep dep) const { return off_[static_cast<std::size_t>(dep)]; }
    const std::string& Reason(Dep dep) const { return reason_[static_cast<std::size_t>(dep)]; }
    void Disable(Dep dep, const std::string& reason);
    // 主题：pattern 是 topic: 之后的部分（可以 * 结尾表示前缀）；writeOnly 只停用「写」的主题（见 TopicOff 的 writes）
    void DisableTopic(const std::string& pattern, bool writeOnly, const std::string& reason);
    // name 的主题是否被停用。writes：主题可写或可取消，或者背后装内联 hook、改虚表槽（Topic::hooked，ADR 0036 的「写」）；
    // 只读、只靠往游戏的 listener 列表注册的主题（native.* 和映射来的语义主题）是 false，注册本身归 events.native 管
    bool TopicOff(std::string_view name, bool writes, std::string* reason) const;
    bool Any() const;

    struct TopicRule {
        std::string pattern;
        bool writeOnly = false;
        std::string reason;
    };
    const std::vector<TopicRule>& TopicRules() const { return topics_; }

private:
    bool off_[static_cast<std::size_t>(Dep::Count)] = {};
    std::string reason_[static_cast<std::size_t>(Dep::Count)];
    std::vector<TopicRule> topics_;
};

// ---------------------------------------------------------------- 一次完整的核对

enum class ItemState {
    Ok,          // 运行时依据全部成立
    Fail,        // 有依据不成立（命中次数不对、读到的值不对、起点不在函数里……）
    Blocked,     // 没有不成立的，但有依据因为特征码失效（或误绑定）没法求值
    Offline,     // check=OFFLINE：只能离线核对
    Unverified,  // check=NONE：没有静态依据
};
const char* StateText(ItemState state);

struct CheckReport {
    std::vector<EvidenceResult> evidence;  // 按 Evidence() 的顺序
    std::vector<ItemState> items;          // 按 Items() 的顺序
    std::vector<std::string> misbound;     // 同一性依据不成立的特征码
    double micros = 0;                     // 全部依据的求值耗时（含函数表）
    double tableMicros = 0;
};

// table 同 ResolveSignatures，为空时 Evaluator 自己建（耗时记在 tableMicros）
CheckReport RunCheck(const ImageView& image, const Scanner& scanner, const std::vector<SigResult>& sigs,
    const FunctionTable* table = nullptr);

// 按 ADR 0036 的策略生成停用开关。knownBuild：exe 的 SHA1 在 DD_KNOWN_BUILD 里（accept 时离线核对过）。
// log 收到人读的说明（每个停用、每个只读能力的警告各一行）。
// 例外：依赖表里的 core（DDCore 整体）不因「只能离线核对、没有静态依据」停用，只警告（DDCore 自己的每帧 hook 原型
// 不能运行时核对，按「写」停用就等于未知版本 DDCore 整体停用，即方案 C；ADR 0036 列为局限，由用户定），core 仍在
// 不成立、没法求值时停用；loader 由加载器自己核对（唯一命中、DD_ENTRY、函数入口、偷取字节数），DDCore 不管
struct PolicyLine {
    int level = 0;  // 0 信息、1 警告、2 错误
    std::string text;
};
Gate BuildGate(const CheckReport& report, const std::vector<SigResult>& sigs, bool knownBuild, std::vector<PolicyLine>* log);

}  // namespace ddc::game::check

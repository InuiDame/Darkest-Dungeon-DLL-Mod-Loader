// probe_p4：P4-0C 探针。按 P4 逆向阶段的待核实清单（0A 是 Buff、Effect、规则、tray 的 13 项，0B 是存档、profile、
// 技能的 14 项，下面的 0A#n、0B#n 就是清单编号）在游戏里观察 buff、effect、规则、tray、存档读写和技能执行，
// 结果写进 DDModLoader\log\probe_p4.log，用 tools/p4_probe_report.py 逐项分析。各项的结论已写进
// knowledge/dd1/combat.md、campaign.md（2026-11-13 的「P4 探针场景」）。地址是基准版 Darkest.exe 的 RVA，exe 不是基准版时只写一行日志、什么都不装。
//
// 被动观察（只读，不调游戏函数）：
//   虚表 Hero/Monster 槽位 13（AddBuff）           0A#1、#13
//   5C4FF0 Buff::Group::erase                      0A#2（带调用栈，能分出回城清理、到期、槽位 14 等路径）
//   47AAD0 BuffDurationUpkeep 入口和出口            0A#3
//   479CD0 IsRuleActive                             0A#6、#13（按调用者、规则类型计数，每类只写前 3 条样本）
//   700A00 tray 普通 buff/debuff 组的成员谓词       0A#12（只统计探针自己的测试 buff）
//   2487E0 排队写文件、3875B0/376A00 设备写盘       0B#1、#2、#13、#14
//   372840 FlushSaveDataAsync                       0B#3
//   248D60 读文件出口                               0B#5（和本次运行、上次运行最后写入的哈希比较）
//   2923A0 选存档槽时复制 backup                    0B#6
//   60EEE0 读探索存档、5AD160 读战斗、600230 怪物读档出口、5AC440 战斗存档   0B#7、#8、#9
//   4A9FA0 Buff::Instance::Load 出口                0B#10
//   5B9B50 技能执行总入口（入口和出口）             0B#11
//   3D1CC0 退到桌面                                 0B#14（可在 cfg 里关掉）
//   4AA770、4F02D0 入口：记下游戏自己传的 buff/effect 文件路径形式（给 0A#7、#10 参考）
// 这些 hook 有的会在加载线程、IO 线程上触发，只读内存、写日志，不调用游戏函数。
// 不碰 DDCore 语义 hook 的入口（4C5050、5BE500、5BF440、5BA3F0、5BAE30、5A64B0、4A5470，虚表槽位 36/50/51），
// 也不订阅会让 DDCore 装这些 hook 的主题；只订阅 battle.start、battle.end、raid.step（原生事件 listener）。
// buff_icon（测试目录里有）占着 6F2DC0、6FBF50、6FCD60、6FC9C0、5C48B0，这里一个都不碰。
//
// 主动测试（DDModLoader\cfg\probe_p4.cfg 的 active = 1 时，默认打开；只在主线程帧边界、不在加载中执行，
// 每次进入存档每项最多一次，SEH 包住，出一次异常就停掉之后的全部主动测试）：
//   CampaignLoad：0B#4 读 profile/设备；0A#11 观察上次加的测试 buff、按 guid 移除，再原生插入定义并加给英雄；
//                 0A#8 EffectLookup；0A#10 4F02D0 运行时解析 effect；0A#7 用 4AA770 解析带未知枚举的 buff 文件。
//   城镇帧边界：0A#4 复制原版 SPD5（combat_stat_add speed_rating +5）加给英雄；0A#6 规则 42 + "lua_test" 的两条 buff
//               （is_false_rule 0/1）；0A#7 两条未知枚举的 buff。
//   走廊帧边界：0A#4 同上；0A#6 同上；0A#12 来源 1（进 tray）和来源 6（不进）的两条 buff；
//               0A#9 4CB4D0 施加原版 effect rakebuff_5（.target performer，.buff_ids rake_buff_5l/5h，规则 skill=rake，
//               走廊里不生效），prng 用全局 2ADD250 的副本，不推进游戏自己的随机数。
//   每条测试 buff 的持续时间都有限，观察 90~150 帧后由探针按组 guid 经槽位 14 移除（顺便核实移除路径）。
//   0A#11 的测试 buff（ddprobe_p4_main、ddprobe_p4_load：combat_stat_add speed_rating +5/+3，来源 1、
//   duration 1、duration_type idle_start_town_visit）要跨一次存档和读档才观察得到：本次运行第一次进存档时加上，
//   下次进存档时观察并按 guid 移除，之后本次运行不再加。万一留在存档里，下次回城（城镇访问开始）游戏会自己删掉。
//   load_inject = 1 时另外在 4AA580 出口（load_campaign 线程）向 Buff 库插入 ddprobe_p4_load 的定义，
//   对比读档后定义专有字段（force_positive）能不能恢复。
// 原版数据（只读游戏目录确认）：shared/buffs/base.buffs.json 的 SPD5；effects/base.effects.darkest 的 rakebuff_5。
// 依据：knowledge/dd1/combat.md、campaign.md、runtime.md、hooks.md。
//
// 用法（guest 的 Git Bash，工作目录 D:\workplace）：
//   1. python -B DD.ModLoader/tools/rig.py deploy probe4（DDCore + probe_p4 + DDModLoader\cfg\probe_p4.cfg，不带 DDLua），
//      status 确认方案和「真实安装未被改动」，用测试目录的 start.bat 启动。cfg 的 active、load_inject、exit_hook 默认都是 1。
//   2. 进一个没有勾选 Lua Mod 的存档（2026-11-13 用的是 P3 的存档 B）。主动测试会给名册第一个英雄临时加测试 buff，
//      都有持续时间，来源 skill 的最迟下次回城时被游戏删掉，所以不要用正在玩的存档。
//   3. 那次场景的流程：城镇里停 15 秒；招募、装上再取下饰品、送英雄进建筑；出征（队伍里带给自己加增益和让怪物流血的技能），
//      进地牢后停 15 秒再走五六步；开一个奇物；一场战斗里同一个增益技能连放两回合，怪物流血时退回主菜单再读档，
//      打完；回城后停 15 秒；退回主菜单，从菜单退出游戏。
//   4. rig.py save-log <名字>，把测试目录根下的 app.log（游戏自己的日志，下次启动会被改名）复制进这个目录，
//      再 python -B DD.ModLoader/tools/p4_probe_report.py --log-dir DD.ModLoader/build/rig_logs/<名字>。
//      退出码 0 没有「不一致」，1 有「不一致」或主动测试出过异常，2 日志缺失或不是 probe4 方案。

#include <dd/dd.hpp>

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

dd::Runtime g_dd;
std::uintptr_t g_base = 0;
std::uintptr_t g_imageEnd = 0;
bool g_enabled = false;
DWORD g_mainTid = 0;
std::atomic<std::uint64_t> g_frame{ 0 };
LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_qpcStart{};
std::atomic<std::uint32_t> g_session{ 0 };  // DDPhaseEvent.campaign_serial，0 是主菜单
std::atomic<bool> g_inCampaign{ false };
std::atomic<bool> g_activeDisabled{ false };  // 主动测试出过异常
std::atomic<std::uint32_t> g_steps{ 0 };       // 本存档的走廊步数（raid.step）
std::string g_gameRoot;

constexpr const char* kExeSha1 = "9db2d73b89aa034e2055df65a139403e18df641f";

struct Config {
    bool active = true;      // 主动测试总开关
    bool loadInject = true;  // 0A#11：在 load_campaign 线程上插入 ddprobe_p4_load 的定义
    bool exitHook = true;    // 0B#14：hook 3D1CC0（退到桌面）
} g_cfg;
std::string g_cfgText = "cfg=默认（没有 probe_p4.cfg）";

namespace rva {
constexpr std::uintptr_t kHeroVtbl = 0xE8A288;
constexpr std::uintptr_t kMonsterVtbl = 0xEA9480;
constexpr std::uintptr_t kSlot13Hero = 0x5CE3C0;
constexpr std::uintptr_t kSlot13Monster = 0x5FD0D0;
constexpr std::uintptr_t kCampaignCurrent = 0x118E138;
constexpr std::uintptr_t kAltRoster = 0x2A6D4BB;
constexpr std::uintptr_t kRaidCurrent = 0x118E2C8;
constexpr std::uintptr_t kRaidDisplay = 0x118E570;
constexpr std::uintptr_t kBuffLibrary = 0x118E0E0;
constexpr std::uintptr_t kStateNext = 0x2A6E664;
constexpr std::uintptr_t kStateLast = 0x2A6E668;
constexpr std::uintptr_t kStorageManager = 0x1185248;
constexpr std::uintptr_t kAppDevice = 0x2A6CF71;  // App+1，当前 profile 设备名
constexpr std::uintptr_t kFlushMainThread = 0x2A96CD8;
constexpr std::uintptr_t kBackupFlag = 0x2624021;
constexpr std::uintptr_t kSteamDevVtbl = 0xE670A0;
constexpr std::uintptr_t kWinDevVtbl = 0xE666C0;
constexpr std::uintptr_t kPrng = 0x2ADD250;
constexpr std::uintptr_t kEffectMap = 0x2ADD290;  // +0 head，+8 size
constexpr std::uintptr_t kStatTable = 0x2AB84D0;  // 每项 0x58
constexpr std::uintptr_t kSourceTable = 0x2AB7790; // 每项 0x4C，+0x44 存档，+0x48 tray
// 游戏函数（主动测试用）
constexpr std::uintptr_t kInstCopy = 0x482480;
constexpr std::uintptr_t kInstCtor = 0x431240;
constexpr std::uintptr_t kVecDtor = 0x2345D0;
constexpr std::uintptr_t kEhVecDtor = 0xB9FBD0;
constexpr std::uintptr_t kElemDtor = 0x2342E0;
constexpr std::uintptr_t kMapIndex = 0x4ACFA0;
constexpr std::uintptr_t kStatTotal = 0x4C3F20;
constexpr std::uintptr_t kEffectLookup = 0x4F3E50;
constexpr std::uintptr_t kApplyEffect = 0x4CB4D0;
constexpr std::uintptr_t kRandFloat = 0x28FCD0;
constexpr std::uintptr_t kFreeU32Vec = 0x243C40;
constexpr std::uintptr_t kParseEffects = 0x4F02D0;
constexpr std::uintptr_t kParseBuffs = 0x4AA770;
}  // namespace rva

// 测试用的 id
constexpr const char* kIdMain = "ddprobe_p4_main";
constexpr const char* kIdLoad = "ddprobe_p4_load";
constexpr const char* kIdRule42 = "ddprobe_p4_rule42";
constexpr const char* kIdRule42n = "ddprobe_p4_rule42n";
constexpr const char* kIdBadStat = "ddprobe_p4_badstat";
constexpr const char* kIdBadRule = "ddprobe_p4_badrule";
constexpr const char* kIdTray1 = "ddprobe_p4_tray1";
constexpr const char* kIdTray6 = "ddprobe_p4_tray6";
constexpr const char* kTestIds[] = { kIdMain, kIdLoad, kIdRule42, kIdRule42n, kIdBadStat, kIdBadRule, kIdTray1, kIdTray6 };
std::uint32_t g_testHashes[std::size(kTestIds)] = {};

// ------------------------------------------------------------------ 基础工具

std::uint32_t Hash(const char* text)
{
    std::uint32_t h = 0;
    for (; *text; ++text) {
        h = h * 53 + static_cast<unsigned char>(*text);
    }
    return h;
}

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    if (src < 0x10000) {
        return false;
    }
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
T Rd(std::uintptr_t address, T fallback = T{})
{
    T value{};
    return ReadBytes(address, &value, sizeof(T)) ? value : fallback;
}

std::string RdStr(std::uintptr_t address, std::size_t capacity)
{
    char buffer[0x204] = {};
    capacity = (std::min)(capacity, sizeof(buffer) - 1);
    if (!ReadBytes(address, buffer, capacity)) {
        // 可能跨过了不可读的页，逐字节再试
        std::size_t i = 0;
        for (; i < capacity; ++i) {
            if (!ReadBytes(address + i, buffer + i, 1) || buffer[i] == 0) {
                break;
            }
        }
        buffer[i] = 0;
    }
    buffer[capacity] = 0;
    return std::string(buffer, strnlen(buffer, capacity));
}

bool IsTestHash(std::uint32_t hash)
{
    for (auto h : g_testHashes) {
        if (h == hash) {
            return true;
        }
    }
    return false;
}

const char* TestName(std::uint32_t hash)
{
    for (std::size_t i = 0; i < std::size(kTestIds); ++i) {
        if (g_testHashes[i] == hash) {
            return kTestIds[i];
        }
    }
    return "?";
}

std::string Q(const std::string& text)
{
    std::string out = "\"";
    for (char c : text) {
        out += (c == '"' || c == '\r' || c == '\n') ? '\'' : c;
    }
    return out + "\"";
}

bool InImage(std::uintptr_t address)
{
    return address >= g_base && address < g_imageEnd;
}

std::uintptr_t RvaOf(const void* address)
{
    const auto a = reinterpret_cast<std::uintptr_t>(address);
    return InImage(a) ? a - g_base : 0;
}

std::string Basename(const std::string& path)
{
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::wstring Wide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// 函数起始 RVA：沿 UNWIND_INFO 的链式条目找到主函数（游戏函数常被拆成几段）
std::uint32_t FuncBegin(std::uintptr_t rvaValue)
{
    if (rvaValue == 0) {
        return 0;
    }
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(g_base + rvaValue, &image, nullptr);
    for (int i = 0; rf && i < 8; ++i) {
        const auto* info = reinterpret_cast<const std::uint8_t*>(image + rf->UnwindInfoAddress);
        const std::uint8_t flags = info[0] >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) {
            break;
        }
        const std::size_t codes = (static_cast<std::size_t>(info[2]) + 1) & ~static_cast<std::size_t>(1);
        rf = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
    }
    return rf ? rf->BeginAddress : 0;
}

// 调用栈：只列 Darkest.exe 里的帧，格式「函数起始+偏移」
__declspec(noinline) std::string Stack(int maxFrames, std::vector<std::uint32_t>* funcs = nullptr)
{
    void* frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, static_cast<ULONG>((std::min)(maxFrames + 6, 24)), frames, nullptr);
    std::string out;
    int printed = 0;
    for (USHORT i = 0; i < n && printed < maxFrames; ++i) {
        const std::uintptr_t r = RvaOf(frames[i]);
        if (!r) {
            continue;
        }
        const std::uint32_t begin = FuncBegin(r);
        if (!out.empty()) {
            out += '<';
        }
        out += std::format("{:X}+{:X}", begin, r - begin);
        if (funcs) {
            funcs->push_back(begin);
        }
        ++printed;
    }
    return out.empty() ? std::string("-") : out;
}

std::uint64_t Fnv64(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool SafeFnv(const void* data, std::size_t size, std::uint64_t* out)
{
    if (!data || size > (64u << 20)) {
        return false;
    }
    __try {
        *out = Fnv64(static_cast<const std::uint8_t*>(data), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeContains(const void* data, std::size_t size, const char* needle)
{
    const std::size_t n = std::strlen(needle);
    if (!data || size < n || size > (64u << 20)) {
        return false;
    }
    __try {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i + n <= size; ++i) {
            if (p[i] == static_cast<std::uint8_t>(needle[0]) && std::memcmp(p + i, needle, n) == 0) {
                return true;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ------------------------------------------------------------------ 日志
// 每行：[t<线程 id> 主|副 f<帧号> <相对秒数>s] <标签> k=v ...
// 帧号是 lifecycle TICK 的计数，相对时间从插件加载算起。DDCore 的 log 服务线程安全，每行直接 WriteFile。

double Secs()
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return g_qpcFreq.QuadPart ? static_cast<double>(now.QuadPart - g_qpcStart.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart) : 0.0;
}

bool IsMain()
{
    return GetCurrentThreadId() == g_mainTid;
}

void Emit(int level, const char* tag, const std::string& body)
{
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "[t%lu %s f%llu %.3fs] ", GetCurrentThreadId(), IsMain() ? "主" : "副",
        static_cast<unsigned long long>(g_frame.load(std::memory_order_relaxed)), Secs());
    std::string line = prefix;
    line += tag;
    if (!body.empty()) {
        line += ' ';
        line += body;
    }
    g_dd.log().write(level, line);
}

template <class... Args>
void L(const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    Emit(DD_LOG_INFO, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void W(const char* tag, std::format_string<Args...> fmt, Args&&... args)
{
    Emit(DD_LOG_WARN, tag, std::format(fmt, std::forward<Args>(args)...));
}

bool Take(std::atomic<std::uint32_t>& counter, std::uint32_t limit)
{
    return counter.fetch_add(1, std::memory_order_relaxed) < limit;
}

thread_local bool t_ours = false;  // 探针自己的主动调用期间为真，hook 的日志标 ours=1

struct OursScope {
    bool previous;
    OursScope() : previous(t_ours) { t_ours = true; }
    ~OursScope() { t_ours = previous; }
};

// ------------------------------------------------------------------ 游戏状态

int StateNext()
{
    return Rd<int>(g_base + rva::kStateNext, -1);
}

int StateLast()
{
    return Rd<int>(g_base + rva::kStateLast, -1);
}

bool Loading()
{
    const int last = StateLast();
    return last == StateNext() && (last == 2 || last == 6 || last == 18);
}

std::uintptr_t RaidPtr()
{
    return Rd<std::uintptr_t>(g_base + rva::kRaidCurrent);
}

int BattleState(std::uintptr_t raid)
{
    return raid ? Rd<int>(raid + 0x4B20) : 0;
}

std::string StateText()
{
    const auto raid = RaidPtr();
    return std::format("st={}/{} raid={} batt={}", StateLast(), StateNext(), raid ? 1 : 0, BattleState(raid));
}

std::vector<std::uintptr_t> PtrVec(std::uintptr_t address, std::size_t max = 2048)
{
    std::vector<std::uintptr_t> out;
    std::uintptr_t v[2] = {};
    if (!ReadBytes(address, v, sizeof(v)) || v[0] == 0 || v[1] < v[0] || (v[1] - v[0]) / 8 > max) {
        return out;
    }
    out.resize((v[1] - v[0]) / 8);
    if (!out.empty() && !ReadBytes(v[0], out.data(), out.size() * 8)) {
        out.clear();
    }
    return out;
}

enum Kind { kNone = 0, kHero = 1, kMonster = 2 };

Kind ActorKind(std::uintptr_t actor)
{
    const auto vt = Rd<std::uintptr_t>(actor);
    if (vt == g_base + rva::kHeroVtbl) {
        return kHero;
    }
    if (vt == g_base + rva::kMonsterVtbl) {
        return kMonster;
    }
    return kNone;
}

std::uint32_t ActorIdOf(std::uintptr_t actor, Kind kind)
{
    return Rd<std::uint32_t>(actor + (kind == kHero ? 0x137C : 0x1320));
}

std::string ActorClass(std::uintptr_t actor, Kind kind)
{
    const auto cls = Rd<std::uintptr_t>(actor + (kind == kHero ? 0x1320 : 0x1468));
    return cls ? RdStr(cls + 8, 0x40) : std::string("?");
}

std::string Desc(std::uintptr_t actor)
{
    const Kind kind = ActorKind(actor);
    if (kind == kNone) {
        return actor ? std::format("actor?{:X}", actor) : std::string("null");
    }
    return std::format("{}#{}/{}", kind == kHero ? "hero" : "monster", ActorIdOf(actor, kind), ActorClass(actor, kind));
}

struct Entry {
    std::uint32_t guid = 0;
    std::int32_t stat = 0, dur = 0, dtype = 0, src = 0, rule = 0, target = 0;
    float amount = 0;
    std::uint32_t idHash = 0, ruleStrHash = 0;
    std::uint8_t falseRule = 0, forcePos = 0, forceNeg = 0;
    std::string id;
};

bool ReadEntryAt(std::uintptr_t entry, Entry& out)
{
    std::uint8_t head[0xB8] = {};
    if (!ReadBytes(entry, head, sizeof(head))) {
        return false;
    }
    const std::uint8_t* inst = head + 8;
    auto i32 = [&](std::size_t off) {
        std::int32_t v;
        std::memcpy(&v, inst + off, 4);
        return v;
    };
    std::memcpy(&out.guid, head, 4);
    out.stat = i32(0x00);
    std::memcpy(&out.amount, inst + 0x48, 4);
    out.dur = i32(0x4C);
    out.dtype = i32(0x50);
    out.src = i32(0x54);
    out.rule = i32(0x5C);
    out.falseRule = inst[0x60];
    char id[0x41] = {};
    std::memcpy(id, inst + 0x61, 0x40);
    out.id.assign(id, strnlen(id, 0x40));
    out.idHash = static_cast<std::uint32_t>(i32(0xA4));
    out.target = i32(0xA8);
    out.ruleStrHash = Rd<std::uint32_t>(entry + 8 + 0xF4);
    std::uint8_t flags[4] = {};
    ReadBytes(entry + 8 + 0x408, flags, 4);
    out.forcePos = flags[2];
    out.forceNeg = flags[3];
    return true;
}

std::vector<Entry> ReadTable(std::uintptr_t actor, std::size_t max = 96)
{
    std::vector<Entry> out;
    const auto first = Rd<std::uintptr_t>(actor + 0x460);
    const auto last = Rd<std::uintptr_t>(actor + 0x468);
    if (!first || last < first || (last - first) % 0x418 != 0 || (last - first) / 0x418 > 512) {
        return out;
    }
    const std::size_t n = (std::min)((last - first) / 0x418, max);
    for (std::size_t i = 0; i < n; ++i) {
        Entry e;
        if (ReadEntryAt(first + i * 0x418, e)) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::uint32_t TableCount(std::uintptr_t actor)
{
    const auto first = Rd<std::uintptr_t>(actor + 0x460);
    const auto last = Rd<std::uintptr_t>(actor + 0x468);
    return (first && last >= first) ? static_cast<std::uint32_t>((last - first) / 0x418) : 0;
}

std::string EntryText(const Entry& e)
{
    return std::format("{}:{}:s{}:a{:g}:d{}:t{}:src{}:r{}{}", e.guid, e.id, e.stat, e.amount, e.dur, e.dtype, e.src, e.rule,
        e.falseRule ? "!" : "");
}

std::uintptr_t RosterSystem()
{
    const auto campaign = Rd<std::uintptr_t>(g_base + rva::kCampaignCurrent);
    if (!campaign) {
        return 0;
    }
    return Rd<std::uint8_t>(g_base + rva::kAltRoster) ? campaign + 0x728 : campaign;
}

std::vector<std::uintptr_t> RosterHeroes()
{
    std::vector<std::uintptr_t> out;
    for (auto entry : PtrVec(RosterSystem() + 0x20)) {
        if (ActorKind(entry + 8) == kHero) {
            out.push_back(entry + 8);
        }
    }
    return out;
}

std::uintptr_t FindHero(std::uint32_t heroId)
{
    for (auto hero : RosterHeroes()) {
        if (ActorIdOf(hero, kHero) == heroId) {
            return hero;
        }
    }
    return 0;
}

std::uintptr_t ChooseRosterHero()
{
    for (auto hero : RosterHeroes()) {
        // Entry+0x15B0（= Hero+0x15A8）为 3 是阵亡
        if (Rd<std::int32_t>(hero + 0x15A8) != 3) {
            return hero;
        }
    }
    return 0;
}

std::vector<std::uintptr_t> PartyHeroes()
{
    const auto raid = RaidPtr();
    std::vector<std::uintptr_t> out;
    if (!raid) {
        return out;
    }
    for (auto hero : PtrVec(raid + 8, 16)) {
        if (ActorKind(hero) == kHero) {
            out.push_back(hero);
        }
    }
    return out;
}

std::vector<std::uintptr_t> BattleMonsters()
{
    const auto raid = RaidPtr();
    return raid ? PtrVec(raid + 0x4898 + 0x38, 32) : std::vector<std::uintptr_t>{};
}

std::uintptr_t LibraryMap()
{
    const auto lib = Rd<std::uintptr_t>(g_base + rva::kBuffLibrary);
    if (!lib) {
        return 0;
    }
    return lib + (Rd<std::uint8_t>(g_base + rva::kAltRoster) ? 0x10 : 0);
}

// MSVC std::map<uint32, V> 的 lower_bound：节点 +0 左、+0x10 右、+0x19 isnil、+0x20 键、+0x28 值
std::uintptr_t MapFind(std::uintptr_t map, std::uint32_t key)
{
    const auto head = Rd<std::uintptr_t>(map);
    if (!head) {
        return 0;
    }
    std::uintptr_t node = Rd<std::uintptr_t>(head + 8);
    std::uintptr_t result = head;
    for (int guard = 0; guard < 64 && node && !Rd<std::uint8_t>(node + 0x19, 1); ++guard) {
        if (Rd<std::uint32_t>(node + 0x20) < key) {
            node = Rd<std::uintptr_t>(node + 0x10);
        } else {
            result = node;
            node = Rd<std::uintptr_t>(node);
        }
    }
    if (result == head || Rd<std::uint32_t>(result + 0x20) != key) {
        return 0;
    }
    return result + 0x28;
}

std::string CurrentProfileDevice()
{
    return RdStr(g_base + rva::kAppDevice, 0x7F);
}

// ------------------------------------------------------------------ 统计（hook 里更新，汇总时输出）

SRWLOCK g_statLock = SRWLOCK_INIT;

struct RuleStat {
    std::uint64_t calls = 0, trues = 0, main = 0, other = 0;
    std::uint32_t samples = 0;
};
struct TestRuleStat {
    std::uint64_t calls = 0, trues = 0;
    std::set<std::uint32_t> callers;
    std::uint32_t logged = 0;
};
struct TrayStat {
    std::uint64_t calls = 0, trues = 0;
};
struct UpkeepStat {
    std::uint64_t calls = 0, dec = 0, filt = 0, rm0 = 0, rmx = 0, bad = 0, changedCalls = 0;
};
struct Stats {
    std::map<std::uint64_t, RuleStat> rule;          // (调用者 RVA << 8) | 规则类型
    std::map<std::uint32_t, TestRuleStat> testRule;  // 测试 buff 的 id 哈希
    std::map<std::uint64_t, TrayStat> tray;          // (id 哈希 << 8) | 极性
    std::map<std::uint32_t, std::uint64_t> slot13ByCaller;
    std::uint64_t slot13 = 0, slot13Realloc = 0, slot13Unexpected = 0, slot13Other = 0;
    std::map<std::string, std::uint64_t> erase;  // "src=<来源> via=<调用者>"
    std::uint64_t erases = 0;
    std::map<std::uint64_t, UpkeepStat> upkeep;  // (类型 << 32) | 调用者 RVA
    std::uint64_t writes = 0, writesDone = 0, writesFailed = 0, reads = 0, readsSave = 0, flushes = 0;
    std::uint64_t instFound = 0, instMissing = 0;
    std::set<std::string> instMissingIds;
    std::uint64_t skills = 0, skillInClass = 0, skillNotInClass = 0, skill8E0 = 0, skill8E8 = 0;
    std::uint64_t buffPaths = 0, effectPaths = 0;
} g_stats;

// 每次进存档重置的日志条数上限
struct Caps {
    std::atomic<std::uint32_t> slot13{ 0 }, erase{ 0 }, upkeep{ 0 }, readOther{ 0 }, instLoad{ 0 }, skill{ 0 }, buffPath{ 0 },
        effectPath{ 0 }, monLoad{ 0 };
    void Reset()
    {
        slot13 = erase = upkeep = readOther = instLoad = skill = buffPath = effectPath = monLoad = 0;
    }
} g_caps;

// 0B#13：主线程上的游戏活动计数（只在主线程上自增）
struct Activity {
    std::atomic<std::uint64_t> adds{ 0 }, erases{ 0 }, upkeeps{ 0 }, skills{ 0 };
} g_act;

struct SaveGap {
    std::uint64_t frame;
    std::string file;
    std::uint64_t adds, erases, upkeeps, skills;
};
std::mutex g_gapMutex;
std::vector<SaveGap> g_gaps;

// 0B#1/#2/#5：写入记录
struct WriteRecord {
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
    std::uint64_t frame = 0;
    double secs = 0;
};
std::mutex g_ioMutex;
std::map<std::string, WriteRecord> g_lastWrites;  // 本次运行，按文件名（不含目录）
std::map<std::string, std::uint64_t> g_prevWrites; // 上次运行最后写入的哈希（probe_p4_state\writes.tsv）
std::map<std::string, int> g_pendingWrites;       // 已排队、还没看到设备写盘的
HANDLE g_stateFile = INVALID_HANDLE_VALUE;

int PendingTotal()
{
    std::lock_guard lock(g_ioMutex);
    int total = 0;
    for (const auto& [name, count] : g_pendingWrites) {
        total += count > 0 ? count : 0;
    }
    return total;
}

std::string PendingText()
{
    std::lock_guard lock(g_ioMutex);
    std::string out;
    for (const auto& [name, count] : g_pendingWrites) {
        if (count > 0) {
            out += std::format("{}{}x{}", out.empty() ? "" : ",", name, count);
        }
    }
    return out.empty() ? std::string("-") : out;
}

// ------------------------------------------------------------------ hook 安装

bool InstallInline(const char* name, std::uintptr_t rvaValue, std::initializer_list<std::uint8_t> expected, void* detour, void** original)
{
    const std::vector<std::uint8_t> bytes(expected);
    std::uint8_t current[32] = {};
    const auto target = g_base + rvaValue;
    if (!ReadBytes(target, current, bytes.size()) || std::memcmp(current, bytes.data(), bytes.size()) != 0) {
        std::string actual, want;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            actual += std::format("{:02X} ", current[i]);
            want += std::format("{:02X} ", bytes[i]);
        }
        W("HOOKSKIP", "name={} rva={:X} 入口字节不符，跳过这一项：实际 {}期望 {}", name, rvaValue, actual, want);
        return false;
    }
    DDHookSpec spec{};
    spec.size = sizeof(spec);
    spec.kind = DD_HOOK_INLINE;
    spec.name = name;
    spec.target = reinterpret_cast<void*>(target);
    spec.expected = bytes.data();
    spec.expected_len = static_cast<std::uint32_t>(bytes.size());
    spec.detour = detour;
    spec.original = original;
    DDHookHandle handle = 0;
    const auto result = g_dd.hooks().service()->install(g_dd.plugin(), &spec, &handle);
    if (result == DD_HOOK_OK) {
        L("HOOK", "name={} rva={:X} kind=inline ok=1", name, rvaValue);
        return true;
    }
    W("HOOK", "name={} rva={:X} kind=inline ok=0 result={}", name, rvaValue, g_dd.hooks().text(result));
    return false;
}

bool InstallVtable(const char* name, std::uintptr_t vtableRva, int slot, std::uintptr_t expectedRva, void* detour, void** original)
{
    auto** slotAddress = reinterpret_cast<void**>(g_base + vtableRva + slot * 8);
    const auto current = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(slotAddress));
    if (current != g_base + expectedRva) {
        W("HOOKSKIP", "name={} vtbl={:X} slot={} 槽位当前值 {:X} 不是期望的 {:X}，跳过", name, vtableRva, slot,
            current ? current - g_base : 0, expectedRva);
        return false;
    }
    DDHookSpec spec{};
    spec.size = sizeof(spec);
    spec.kind = DD_HOOK_VTABLE;
    spec.name = name;
    spec.target = slotAddress;
    spec.expected_value = reinterpret_cast<void*>(g_base + expectedRva);
    spec.detour = detour;
    spec.original = original;
    DDHookHandle handle = 0;
    const auto result = g_dd.hooks().service()->install(g_dd.plugin(), &spec, &handle);
    if (result == DD_HOOK_OK) {
        L("HOOK", "name={} rva={:X} kind=vtable slot={} ok=1", name, expectedRva, slot);
        return true;
    }
    W("HOOK", "name={} rva={:X} kind=vtable slot={} ok=0 result={}", name, expectedRva, slot, g_dd.hooks().text(result));
    return false;
}

// ------------------------------------------------------------------ 0A#1、#13：槽位 13

using Slot13Fn = std::uint32_t (*)(void*, const void*, void*, bool);
Slot13Fn g_origSlot13Hero = nullptr;
Slot13Fn g_origSlot13Monster = nullptr;

__declspec(noinline) std::uint32_t Slot13Common(Slot13Fn orig, void* actorPtr, const void* inst, void* source, bool refresh, std::uintptr_t ret)
{
    const auto actor = reinterpret_cast<std::uintptr_t>(actorPtr);
    const auto in = reinterpret_cast<std::uintptr_t>(inst);
    const auto next0 = Rd<std::uint32_t>(actor + 0x478);
    const auto first0 = Rd<std::uintptr_t>(actor + 0x460);
    const auto n0 = TableCount(actor);
    const std::string id = RdStr(in + 0x61, 0x40);
    const auto idHash = Rd<std::uint32_t>(in + 0xA4);
    const auto stat = Rd<std::int32_t>(in);
    const auto amount = Rd<float>(in + 0x48);
    const auto dur = Rd<std::int32_t>(in + 0x4C);
    const auto dtype = Rd<std::int32_t>(in + 0x50);
    const auto src = Rd<std::int32_t>(in + 0x54);
    const auto target = Rd<std::int32_t>(in + 0xA8);
    const auto raid = RaidPtr();

    const std::uint32_t result = orig(actorPtr, inst, source, refresh);

    const auto next1 = Rd<std::uint32_t>(actor + 0x478);
    const auto first1 = Rd<std::uintptr_t>(actor + 0x460);
    const auto n1 = TableCount(actor);
    const bool realloc = first0 != first1;
    std::uint32_t sameId = 0;
    bool present = false;
    for (const auto& e : ReadTable(actor, 256)) {
        sameId += e.idHash == idHash ? 1 : 0;
        present = present || e.guid == result;
    }
    const bool expected = result == next0 && next1 == next0 + 1 && present;
    const std::uintptr_t callerRva = ret >= g_base && ret < g_imageEnd ? ret - g_base : 0;
    if (IsMain()) {
        g_act.adds.fetch_add(1, std::memory_order_relaxed);
    }
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.slot13;
    ++g_stats.slot13ByCaller[static_cast<std::uint32_t>(callerRva)];
    g_stats.slot13Realloc += realloc ? 1 : 0;
    g_stats.slot13Unexpected += expected ? 0 : 1;
    g_stats.slot13Other += IsMain() ? 0 : 1;
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.slot13, 300) || t_ours || IsTestHash(idHash) || !expected) {
        L("SLOT13", "who={} {} id={} stat={} amt={:g} dur={} dtype={} src={} tgt={} refresh={} srcActor={} next0={} ret={} next1={} "
                    "n0={} n1={} present={} realloc={} sameid={} ok={} ours={} caller={:X}",
            Desc(actor), std::format("st={}/{} raid={} batt={}", StateLast(), StateNext(), raid ? 1 : 0, BattleState(raid)), Q(id),
            stat, amount, dur, dtype, src, target, refresh ? 1 : 0, Desc(reinterpret_cast<std::uintptr_t>(source)), next0, result,
            next1, n0, n1, present ? 1 : 0, realloc ? 1 : 0, sameId, expected ? 1 : 0, t_ours ? 1 : 0, callerRva);
    }
    return result;
}

std::uint32_t Slot13HeroDetour(void* actor, const void* inst, void* source, bool refresh)
{
    return Slot13Common(g_origSlot13Hero, actor, inst, source, refresh, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

std::uint32_t Slot13MonsterDetour(void* actor, const void* inst, void* source, bool refresh)
{
    return Slot13Common(g_origSlot13Monster, actor, inst, source, refresh, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

// ------------------------------------------------------------------ 0A#2：5C4FF0 erase

using EraseFn = void* (*)(void*, void*, void*);
EraseFn g_origErase = nullptr;

void* EraseDetour(void* group, void* out, void* entryPtr)
{
    Entry e;
    const bool ok = ReadEntryAt(reinterpret_cast<std::uintptr_t>(entryPtr), e);
    const auto actor = reinterpret_cast<std::uintptr_t>(group) - 0x460;
    std::vector<std::uint32_t> funcs;
    const std::string stack = Stack(6, &funcs);
    // 「经谁删的」：跳过 erase 本身和 RemoveIf（5C46D0）、槽位 14 的两个实现，取第一个调用者
    std::uint32_t via = 0, via2 = 0;
    for (auto f : funcs) {
        if (f == 0x5C4FF0 || f == 0x5C46D0) {
            continue;
        }
        if (!via) {
            via = f;
        } else if (!via2) {
            via2 = f;
            break;
        }
    }
    if (IsMain()) {
        g_act.erases.fetch_add(1, std::memory_order_relaxed);
    }
    const std::string key = std::format("src={} via={:X}{}", e.src, via,
        (via == 0x5CE600 || via == 0x5FD280) ? std::format("<{:X}", via2) : std::string());
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.erases;
    ++g_stats.erase[key];
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.erase, 400) || t_ours || IsTestHash(e.idHash)) {
        L("ERASE", "who={} {} entry={} srcSave={} ok={} ours={} via={:X} stack={}", Desc(actor), StateText(),
            ok ? EntryText(e) : std::string("?"), Rd<std::uint8_t>(g_base + rva::kSourceTable + static_cast<std::uintptr_t>(e.src) * 0x4C + 0x44),
            ok ? 1 : 0, t_ours ? 1 : 0, via, stack);
    }
    return g_origErase(group, out, entryPtr);
}

// ------------------------------------------------------------------ 0A#3：47AAD0 BuffDurationUpkeep

using UpkeepFn = std::uint64_t (*)(void*, void*, int);
UpkeepFn g_origUpkeep = nullptr;

std::uint64_t UpkeepDetour(void* actorPtr, void* filter, int type)
{
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto actor = reinterpret_cast<std::uintptr_t>(actorPtr);
    const auto before = ReadTable(actor, 64);
    const std::uint64_t result = g_origUpkeep(actorPtr, filter, type);
    const auto after = ReadTable(actor, 64);
    if (IsMain()) {
        g_act.upkeeps.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint32_t dec = 0, filt = 0, rm0 = 0, rmx = 0, same = 0, bad = 0;
    std::string detail;
    for (const auto& b : before) {
        const Entry* a = nullptr;
        for (const auto& x : after) {
            if (x.guid == b.guid) {
                a = &x;
                break;
            }
        }
        const bool shouldDec = b.dtype == type && b.dur > 0;
        const int expect = shouldDec ? b.dur - 1 : b.dur;
        if (!a) {
            // duration==0 的（不论类型）会被删除；过滤器排除的、减到 0 的也算
            if (expect == 0 || b.dur == 0) {
                ++rm0;
            } else {
                ++rmx;  // 可能是刷新属性时 remove_if_not_active 之类顺带删的
            }
            detail += std::format(" {}:{}:t{}:d{}->x", b.guid, b.id, b.dtype, b.dur);
        } else if (a->dur == expect) {
            if (shouldDec) {
                ++dec;
                detail += std::format(" {}:{}:t{}:d{}->{}", b.guid, b.id, b.dtype, b.dur, a->dur);
            } else {
                ++same;
            }
        } else if (shouldDec && a->dur == b.dur) {
            ++filt;  // 过滤器排除（例如来源 guard、battle_modifier）
        } else {
            ++bad;
            detail += std::format(" {}:{}:t{}:d{}->{}!", b.guid, b.id, b.dtype, b.dur, a->dur);
        }
    }
    const std::uintptr_t callerRva = ret >= g_base && ret < g_imageEnd ? ret - g_base : 0;
    const bool changed = dec || rm0 || rmx || bad;
    AcquireSRWLockExclusive(&g_statLock);
    auto& s = g_stats.upkeep[(static_cast<std::uint64_t>(static_cast<std::uint32_t>(type)) << 32) | callerRva];
    ++s.calls;
    s.dec += dec;
    s.filt += filt;
    s.rm0 += rm0;
    s.rmx += rmx;
    s.bad += bad;
    s.changedCalls += changed ? 1 : 0;
    ReleaseSRWLockExclusive(&g_statLock);
    if ((changed && Take(g_caps.upkeep, 300)) || bad) {
        L("UPKEEP", "who={} {} type={} caller={:X} n0={} n1={} dec={} filt={} rm0={} rmx={} same={} bad={} |{}", Desc(actor), StateText(), type,
            callerRva, before.size(), after.size(), dec, filt, rm0, rmx, same, bad, detail);
    }
    return result;
}

// ------------------------------------------------------------------ 0A#6、#13：479CD0 IsRuleActive

using RuleFn = std::uint64_t (*)(void*, const void*, const void*, void*, const void*);
RuleFn g_origRule = nullptr;

std::uint64_t RuleDetour(void* actor, const void* inst, const void* p3, void* target, const void* skill)
{
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uint64_t result = g_origRule(actor, inst, p3, target, skill);
    const auto in = reinterpret_cast<std::uintptr_t>(inst);
    const int rule = Rd<std::int32_t>(in + 0x5C, -1);
    const auto idHash = Rd<std::uint32_t>(in + 0xA4);
    const bool truth = (result & 0xFF) != 0;
    const std::uintptr_t callerRva = ret >= g_base && ret < g_imageEnd ? ret - g_base : 0;
    const bool test = IsTestHash(idHash);
    bool sample = false;
    bool testLog = false;
    AcquireSRWLockExclusive(&g_statLock);
    auto& s = g_stats.rule[(static_cast<std::uint64_t>(callerRva) << 8) | static_cast<std::uint8_t>(rule)];
    ++s.calls;
    s.trues += truth ? 1 : 0;
    (IsMain() ? s.main : s.other) += 1;
    if (s.samples < 3) {
        ++s.samples;
        sample = true;
    }
    if (test) {
        auto& t = g_stats.testRule[idHash];
        ++t.calls;
        t.trues += truth ? 1 : 0;
        t.callers.insert(static_cast<std::uint32_t>(callerRva));
        if (t.logged < 12) {
            ++t.logged;
            testLog = true;
        }
    }
    ReleaseSRWLockExclusive(&g_statLock);
    if (sample || testLog) {
        L(testLog ? "RULETEST" : "RULE", "who={} {} id={} rule={} false={} rstr={:X} tgt={} skill={:X} res={} caller={:X}",
            Desc(reinterpret_cast<std::uintptr_t>(actor)), StateText(), Q(RdStr(in + 0x61, 0x40)), rule, Rd<std::uint8_t>(in + 0x60),
            Rd<std::uint32_t>(in + 0xF4), Desc(reinterpret_cast<std::uintptr_t>(target)), reinterpret_cast<std::uintptr_t>(skill),
            truth ? 1 : 0, callerRva);
    }
    return result;
}

// ------------------------------------------------------------------ 0A#12：700A00 tray 组成员谓词

using TrayFn = std::uint64_t (*)(void*, const void*);
TrayFn g_origTray = nullptr;

std::uint64_t TrayDetour(void* self, const void* entry)
{
    const std::uint64_t result = g_origTray(self, entry);
    const auto idHash = Rd<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry) + 8 + 0xA4);
    if (IsTestHash(idHash)) {
        const auto polarity = Rd<std::uint8_t>(reinterpret_cast<std::uintptr_t>(self) + 8);
        const bool truth = (result & 0xFF) != 0;
        bool first = false;
        AcquireSRWLockExclusive(&g_statLock);
        auto& s = g_stats.tray[(static_cast<std::uint64_t>(idHash) << 8) | polarity];
        first = s.calls == 0;
        ++s.calls;
        s.trues += truth ? 1 : 0;
        ReleaseSRWLockExclusive(&g_statLock);
        if (first) {
            L("TRAY", "id={} polarity={} res={} src={} (首次)", TestName(idHash), polarity, truth ? 1 : 0,
                Rd<std::int32_t>(reinterpret_cast<std::uintptr_t>(entry) + 8 + 0x54));
        }
    }
    return result;
}

// ------------------------------------------------------------------ 0B#1、#13：2487E0 排队写文件

using QueueWriteFn = void* (*)(void*, void*, const char*, const char*, const void*, std::uint64_t, std::uint64_t, std::uint64_t, void*);
QueueWriteFn g_origQueueWrite = nullptr;

void PersistWrite(const std::string& file, std::uint64_t hash, std::uint32_t size)
{
    if (g_stateFile == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string line = std::format("{}\t{:016X}\t{}\t{:.3f}\r\n", file, hash, size, Secs());
    DWORD written = 0;
    WriteFile(g_stateFile, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void* QueueWriteDetour(void* sm, void* handle, const char* dev, const char* file, const void* buffer, std::uint64_t size, std::uint64_t p7,
    std::uint64_t p8, void* p9)
{
    const std::string device = RdStr(reinterpret_cast<std::uintptr_t>(dev), 0x40);
    const std::string path = RdStr(reinterpret_cast<std::uintptr_t>(file), 0x100);
    const std::string name = Basename(path);
    const auto bytes = static_cast<std::uint32_t>(size);
    std::uint64_t hash = 0;
    const bool hashed = SafeFnv(buffer, bytes, &hash);
    std::string ids;
    if (name.find("roster") != std::string::npos) {
        ids = std::format(" has_main={} has_load={}", SafeContains(buffer, bytes, kIdMain) ? 1 : 0, SafeContains(buffer, bytes, kIdLoad) ? 1 : 0);
    }
    const std::string stack = Stack(4);
    const bool profile = !device.empty() && device == CurrentProfileDevice();
    {
        std::lock_guard lock(g_ioMutex);
        ++g_pendingWrites[name];
        if (hashed) {
            g_lastWrites[name] = { hash, bytes, g_frame.load(), Secs() };
            PersistWrite(name, hash, bytes);
        }
    }
    if (IsMain()) {
        std::lock_guard lock(g_gapMutex);
        g_gaps.push_back({ g_frame.load(), name, g_act.adds.load(), g_act.erases.load(), g_act.upkeeps.load(), g_act.skills.load() });
    }
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.writes;
    ReleaseSRWLockExclusive(&g_statLock);
    L("WRITE", "dev={} file={} size={} hash={} prof={} free={} {}{} stack={}", Q(device), Q(path), bytes,
        hashed ? std::format("{:016X}", hash) : std::string("?"), profile ? 1 : 0, p8 & 0xFF, StateText(), ids, stack);
    return g_origQueueWrite(sm, handle, dev, file, buffer, size, p7, p8, p9);
}

// ------------------------------------------------------------------ 0B#2：设备写盘（Steam 3875B0、Windows 376A00）

using SteamWriteFn = std::uint64_t (*)(void*, const char*, const void*, std::uint64_t);
SteamWriteFn g_origSteamWrite = nullptr;
using WinWriteFn = std::uint64_t (*)(void*, const char*, const void*, std::uint64_t, std::uint64_t);
WinWriteFn g_origWinWrite = nullptr;

void DeviceWriteDone(const char* kind, void* dev, const char* file, std::uint64_t size, std::uint64_t result, double t0)
{
    const std::string name = Basename(RdStr(reinterpret_cast<std::uintptr_t>(file), 0x100));
    const bool ok = (result & 0xFF) != 0;
    {
        std::lock_guard lock(g_ioMutex);
        --g_pendingWrites[name];
    }
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.writesDone;
    g_stats.writesFailed += ok ? 0 : 1;
    ReleaseSRWLockExclusive(&g_statLock);
    L("WDONE", "kind={} dev={} file={} size={} ok={} ms={:.2f} pending={}", kind, Q(RdStr(reinterpret_cast<std::uintptr_t>(dev) + 0x20, 0x80)),
        Q(name), static_cast<std::uint32_t>(size), ok ? 1 : 0, (Secs() - t0) * 1000.0, PendingTotal());
}

std::uint64_t SteamWriteDetour(void* dev, const char* file, const void* buffer, std::uint64_t size)
{
    const double t0 = Secs();
    const std::uint64_t result = g_origSteamWrite(dev, file, buffer, size);
    DeviceWriteDone("steam", dev, file, size, result, t0);
    return result;
}

std::uint64_t WinWriteDetour(void* dev, const char* file, const void* buffer, std::uint64_t size, std::uint64_t p5)
{
    const double t0 = Secs();
    const std::uint64_t result = g_origWinWrite(dev, file, buffer, size, p5);
    DeviceWriteDone("windows", dev, file, size, result, t0);
    return result;
}

// ------------------------------------------------------------------ 0B#3：372840 FlushSaveDataAsync

using FlushFn = std::uint64_t (*)(void*);
FlushFn g_origFlush = nullptr;

std::uint64_t FlushDetour(void* callback)
{
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto sm = Rd<std::uintptr_t>(g_base + rva::kStorageManager);
    const auto slot7 = Rd<std::uintptr_t>(sm + 0x48);
    const int pending0 = PendingTotal();
    const std::string pendingText = PendingText();
    const double t0 = Secs();
    const std::uint64_t result = g_origFlush(callback);
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.flushes;
    ReleaseSRWLockExclusive(&g_statLock);
    L("FLUSH", "caller={:X} slot7={:X} pending0={} [{}] pending1={} ms={:.2f} {}", ret >= g_base && ret < g_imageEnd ? ret - g_base : 0, slot7,
        pending0, pendingText, PendingTotal(), (Secs() - t0) * 1000.0, StateText());
    return result;
}

// ------------------------------------------------------------------ 0B#5：248D60 读文件

using ReadFileFn = std::uint64_t (*)(void*, const char*, const char*, void*, std::uint64_t, std::uint64_t);
ReadFileFn g_origReadFile = nullptr;

std::uint64_t ReadFileDetour(void* sm, const char* dev, const char* path, void* buffer, std::uint64_t size, std::uint64_t decrypt)
{
    const double t0 = Secs();
    const std::uint64_t result = g_origReadFile(sm, dev, path, buffer, size, decrypt);
    const std::string device = RdStr(reinterpret_cast<std::uintptr_t>(dev), 0x40);
    const std::string file = RdStr(reinterpret_cast<std::uintptr_t>(path), 0x100);
    const std::string name = Basename(file);
    const auto got = static_cast<std::uint32_t>(result);
    const auto cap = static_cast<std::uint32_t>(size);
    const bool save = name.find("persist.") != std::string::npos;
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.reads;
    g_stats.readsSave += save ? 1 : 0;
    ReleaseSRWLockExclusive(&g_statLock);
    if (save) {
        std::uint64_t hash = 0;
        const bool hashed = SafeFnv(buffer, (std::min)(got, cap), &hash);
        std::string run = "-", prev = "-";
        {
            std::lock_guard lock(g_ioMutex);
            if (auto it = g_lastWrites.find(name); it != g_lastWrites.end()) {
                run = it->second.hash == hash ? "same" : "diff";
                run += std::format("@f{}", it->second.frame);
            }
            if (auto it = g_prevWrites.find(name); it != g_prevWrites.end()) {
                prev = it->second == hash ? "same" : "diff";
            }
        }
        L("READ", "dev={} path={} cap={} got={} decrypt={} hash={} vs_run={} vs_prev={} ms={:.2f} {} stack={}", Q(device), Q(file), cap, got,
            decrypt & 0xFF, hashed ? std::format("{:016X}", hash) : std::string("?"), run, prev, (Secs() - t0) * 1000.0, StateText(), Stack(3));
    } else if (Take(g_caps.readOther, 30)) {
        L("READO", "dev={} path={} cap={} got={} decrypt={}", Q(device), Q(file), cap, got, decrypt & 0xFF);
    }
    return result;
}

// ------------------------------------------------------------------ 0B#6：2923A0 复制 backup

using Void4Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
Void4Fn g_origBackup = nullptr;

std::string Slot5Device()
{
    const auto sm = Rd<std::uintptr_t>(g_base + rva::kStorageManager);
    const auto dev = Rd<std::uintptr_t>(sm + 0x38);
    return dev ? RdStr(dev + 0x20, 0x80) : std::string("-");
}

std::uint64_t BackupDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    L("BACKUP", "phase=enter slot5={} app={} {}", Q(Slot5Device()), Q(CurrentProfileDevice()), StateText());
    const double t0 = Secs();
    const std::uint64_t result = g_origBackup(a, b, c, d);
    L("BACKUP", "phase=exit ms={:.1f}", (Secs() - t0) * 1000.0);
    return result;
}

// ------------------------------------------------------------------ 0B#7：60EEE0 读探索存档、5AD160 读战斗

Void4Fn g_origRaidLoad = nullptr;
Void4Fn g_origBattleLoad = nullptr;

std::uint64_t RaidLoadDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    L("RAIDLOAD", "phase=enter session={} {}", g_session.load(), StateText());
    const std::uint64_t result = g_origRaidLoad(a, b, c, d);
    L("RAIDLOAD", "phase=exit ret={} {}", result & 0xFF, StateText());
    return result;
}

std::uint64_t BattleLoadDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    L("BATTLELOAD", "phase=enter battle={:X} caller={} {}", a, Stack(2), StateText());
    const std::uint64_t result = g_origBattleLoad(a, b, c, d);
    L("BATTLELOAD", "phase=exit monsters={}", BattleMonsters().size());
    return result;
}

// ------------------------------------------------------------------ 0B#8、#9：5AC440 战斗存档、600230 怪物读档

Void4Fn g_origBattleSave = nullptr;
Void4Fn g_origMonsterLoad = nullptr;

std::string MonsterBuffs(std::uintptr_t monster)
{
    std::string out;
    for (const auto& e : ReadTable(monster, 24)) {
        out += std::format("{}{}:{}", out.empty() ? "" : ",", e.guid, e.id);
    }
    return out.empty() ? std::string("-") : out;
}

std::uint64_t BattleSaveDetour(std::uint64_t battle, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    const auto monsters = PtrVec(battle + 0x38, 32);
    L("BSAVE", "state={} round={} monsters={} caller={} {}", Rd<std::int32_t>(battle + 0x288), Rd<std::int32_t>(battle + 0xA8), monsters.size(),
        Stack(2), StateText());
    for (auto m : monsters) {
        if (ActorKind(m) == kMonster) {
            L("BSAVE_MON", "guid={} cls={} n={} next={} buffs={}", ActorIdOf(m, kMonster), ActorClass(m, kMonster), TableCount(m),
                Rd<std::uint32_t>(m + 0x478), MonsterBuffs(m));
        }
    }
    return g_origBattleSave(battle, b, c, d);
}

std::uint64_t MonsterLoadDetour(std::uint64_t monster, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    const std::uint64_t result = g_origMonsterLoad(monster, b, c, d);
    if (Take(g_caps.monLoad, 64)) {
        L("MONLOAD", "guid={} cls={} n={} next={} buffs={}", ActorIdOf(monster, kMonster), ActorClass(monster, kMonster), TableCount(monster),
            Rd<std::uint32_t>(monster + 0x478), MonsterBuffs(monster));
    }
    return result;
}

// ------------------------------------------------------------------ 0B#10：4A9FA0 Buff::Instance::Load

Void4Fn g_origInstLoad = nullptr;

std::uint64_t InstLoadDetour(std::uint64_t inst, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    const std::uint64_t result = g_origInstLoad(inst, b, c, d);
    const std::string id = RdStr(inst + 0x61, 0x40);
    const auto hash = Rd<std::uint32_t>(inst + 0xA4);
    const auto def = MapFind(LibraryMap(), hash);
    const bool test = IsTestHash(hash);
    AcquireSRWLockExclusive(&g_statLock);
    (def ? g_stats.instFound : g_stats.instMissing) += 1;
    if (!def && g_stats.instMissingIds.size() < 40) {
        g_stats.instMissingIds.insert(id);
    }
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.instLoad, 60) || !def || test) {
        L("INSTLOAD", "id={} hash={:X} found={} src={} dur={} dtype={} fp={} def_fp={} hash_ok={}", Q(id), hash, def ? 1 : 0, Rd<std::int32_t>(inst + 0x54),
            Rd<std::int32_t>(inst + 0x4C), Rd<std::int32_t>(inst + 0x50), Rd<std::uint8_t>(inst + 0x40A),
            def ? std::to_string(Rd<std::uint8_t>(def + 0x40A)) : std::string("-"), Hash(id.c_str()) == hash ? 1 : 0);
    }
    return result;
}

// ------------------------------------------------------------------ 0B#11：5B9B50 技能执行总入口

using SkillFn = std::uint64_t (*)(void*, void*, const void*, void*, std::uint64_t);
SkillFn g_origSkill = nullptr;

// 技能指针在执行者职业的哪张表里：hero 的 +0xBD0（按 id、等级两层）、+0xBE8（移动技能），monster 的 +0xBA8
std::string SkillTable(std::uintptr_t performer, std::uintptr_t skill)
{
    const Kind kind = ActorKind(performer);
    if (kind == kHero) {
        const auto cls = Rd<std::uintptr_t>(performer + 0x1320);
        std::uintptr_t outer[2] = {};
        if (ReadBytes(cls + 0xBD0, outer, sizeof(outer)) && outer[1] >= outer[0] && (outer[1] - outer[0]) / 0x18 < 64) {
            for (std::uintptr_t at = outer[0]; at < outer[1]; at += 0x18) {
                std::uintptr_t inner[2] = {};
                if (ReadBytes(at, inner, sizeof(inner)) && skill >= inner[0] && skill < inner[1] && (skill - inner[0]) % 0x510 == 0) {
                    return "BD0";
                }
            }
        }
        std::uintptr_t move[2] = {};
        if (ReadBytes(cls + 0xBE8, move, sizeof(move)) && skill >= move[0] && skill < move[1] && (skill - move[0]) % 0x510 == 0) {
            return "BE8";
        }
    } else if (kind == kMonster) {
        const auto cls = Rd<std::uintptr_t>(performer + 0x1468);
        std::uintptr_t v[2] = {};
        if (ReadBytes(cls + 0xBA8, v, sizeof(v)) && skill >= v[0] && skill < v[1] && (skill - v[0]) % 0x500 == 0) {
            return "BA8";
        }
    }
    return "none";
}

std::uint64_t SkillDetour(void* battle, void* performerPtr, const void* skillPtr, void* targets, std::uint64_t flag)
{
    const auto performer = reinterpret_cast<std::uintptr_t>(performerPtr);
    const auto skill = reinterpret_cast<std::uintptr_t>(skillPtr);
    const std::string table = SkillTable(performer, skill);
    const auto targetVec = PtrVec(reinterpret_cast<std::uintptr_t>(targets), 16);
    const std::string id = RdStr(skill + 8, 0x40);
    const int level = Rd<std::int32_t>(skill + 0x48);
    const std::string type = RdStr(skill + 0x50, 0x40);
    const int friendly = Rd<std::uint8_t>(skill + 0x90);
    const auto mask = Rd<std::uint32_t>(skill + 0x3A8);
    std::uintptr_t fx[2] = {};
    ReadBytes(skill + 0x3C8, fx, sizeof(fx));
    const std::uint64_t result = g_origSkill(battle, performerPtr, skillPtr, targets, flag);
    if (IsMain()) {
        g_act.skills.fetch_add(1, std::memory_order_relaxed);
    }
    const bool e0 = Rd<std::uintptr_t>(performer + 0x8E0) == skill;
    const bool e8 = Rd<std::uintptr_t>(performer + 0x8E8) == skill;
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.skills;
    (table == "none" ? g_stats.skillNotInClass : g_stats.skillInClass) += 1;
    g_stats.skill8E0 += e0 ? 1 : 0;
    g_stats.skill8E8 += e8 ? 1 : 0;
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.skill, 150) || table == "none") {
        L("SKILL", "who={} skill={} lvl={} type={} friendly={} mask={:X} b100={} nfx={} table={} targets={} t0={} flag={} p8E0={} p8E8={}",
            Desc(performer), Q(id), level, Q(type), friendly, mask, (mask & 0x100) ? 1 : 0, fx[1] >= fx[0] ? (fx[1] - fx[0]) / 8 : 0, table,
            targetVec.size(), targetVec.empty() ? std::string("-") : Desc(targetVec[0]), flag & 0xFF, e0 ? 1 : 0, e8 ? 1 : 0);
    }
    return result;
}

// ------------------------------------------------------------------ 4AA770、4F02D0：游戏传的文件路径形式；4AA580：加载线程注入

using BuffFileFn = std::uint64_t (*)(void*, const char*, std::uint64_t, std::uint64_t);
BuffFileFn g_origBuffFile = nullptr;
using EffectFileFn = std::uint64_t (*)(const char*, std::uint64_t, std::uint64_t, std::uint64_t);
EffectFileFn g_origEffectFile = nullptr;
Void4Fn g_origLibLoadAll = nullptr;

std::uint64_t BuffFileDetour(void* lib, const char* path, std::uint64_t network, std::uint64_t validate)
{
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.buffPaths;
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.buffPath, 6) || t_ours) {
        L("BUFFFILE", "path={} network={} validate={} ours={}", Q(RdStr(reinterpret_cast<std::uintptr_t>(path), 0x100)), network & 0xFF,
            validate & 0xFF, t_ours ? 1 : 0);
    }
    return g_origBuffFile(lib, path, network, validate);
}

std::uint64_t EffectFileDetour(const char* path, std::uint64_t network, std::uint64_t c, std::uint64_t d)
{
    AcquireSRWLockExclusive(&g_statLock);
    ++g_stats.effectPaths;
    ReleaseSRWLockExclusive(&g_statLock);
    if (Take(g_caps.effectPath, 6) || t_ours) {
        L("FXFILE", "path={} network={} ours={}", Q(RdStr(reinterpret_cast<std::uintptr_t>(path), 0x100)), network & 0xFF, t_ours ? 1 : 0);
    }
    return g_origEffectFile(path, network, c, d);
}

// 测试 buff 的定义和实例字段
struct BuffSpec {
    const char* id;
    int stat;
    const char* sub;
    float amount;
    int dur;
    int dtype;
    int src;
    int rule;
    bool falseRule;
    const char* ruleStr;
    bool forcePos;
};

void PutStr(std::uint8_t* p, std::size_t off, std::size_t cap, std::size_t hashOff, const char* text)
{
    std::memset(p + off, 0, cap);
    const std::size_t n = (std::min)(std::strlen(text), cap - 1);
    std::memcpy(p + off, text, n);
    const std::uint32_t h = Hash(text);
    std::memcpy(p + hashOff, &h, 4);
}

void Put32(std::uint8_t* p, std::size_t off, std::int32_t v)
{
    std::memcpy(p + off, &v, 4);
}

DWORD g_lastExc = 0;

// 只写 POD 字段（vector 部分保持默认构造的空表）
bool FillInstance(std::uint8_t* p, const BuffSpec* s)
{
    __try {
        Put32(p, 0x00, s->stat);
        PutStr(p, 0x04, 0x40, 0x44, s->sub);
        std::memcpy(p + 0x48, &s->amount, 4);
        Put32(p, 0x4C, s->dur);
        Put32(p, 0x50, s->dtype);
        Put32(p, 0x54, s->src);
        Put32(p, 0x58, 0);
        Put32(p, 0x5C, s->rule);
        p[0x60] = s->falseRule ? 1 : 0;
        PutStr(p, 0x61, 0x40, 0xA4, s->id);
        Put32(p, 0xA8, 0);
        PutStr(p, 0xB4, 0x40, 0xF4, s->ruleStr ? s->ruleStr : "");
        p[0x40A] = s->forcePos ? 1 : 0;
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using MapIndexFn = void* (*)(void*, const std::uint32_t*);

bool G_MapIndex(std::uintptr_t map, std::uint32_t key, std::uintptr_t* out)
{
    __try {
        *out = reinterpret_cast<std::uintptr_t>(reinterpret_cast<MapIndexFn>(g_base + rva::kMapIndex)(reinterpret_cast<void*>(map), &key));
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const BuffSpec kSpecMain = { kIdMain, 2, "speed_rating", 5.0f, -1, 9, 0, 0, false, "", true };
const BuffSpec kSpecLoad = { kIdLoad, 2, "speed_rating", 3.0f, -1, 9, 0, 0, false, "", true };

std::uint64_t LibLoadAllDetour(std::uint64_t lib, std::uint64_t validate, std::uint64_t c, std::uint64_t d)
{
    const std::uint64_t result = g_origLibLoadAll(lib, validate, c, d);
    if (g_cfg.active && g_cfg.loadInject && !g_activeDisabled) {
        std::uintptr_t def = 0;
        const bool ok = G_MapIndex(lib, Hash(kIdLoad), &def) && def && FillInstance(reinterpret_cast<std::uint8_t*>(def), &kSpecLoad);
        if (!ok) {
            g_activeDisabled = true;
        }
        L("LOADINJ", "lib={:X} def={:X} ok={} code={:X} id={}（load_campaign 线程，4AA580 出口）", lib, def, ok ? 1 : 0, ok ? 0 : g_lastExc, kIdLoad);
    }
    return result;
}

// ------------------------------------------------------------------ 0B#14：3D1CC0 退到桌面

Void4Fn g_origExit = nullptr;
void Summary(const char* when);

std::uint64_t ExitDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    L("EXIT", "phase=enter pending={} [{}] {}", PendingTotal(), PendingText(), StateText());
    Summary("exit");
    const std::uint64_t result = g_origExit(a, b, c, d);
    L("EXIT", "phase=after3D1CC0 pending={} [{}]", PendingTotal(), PendingText());
    return result;
}

// ------------------------------------------------------------------ 汇总

void Summary(const char* when)
{
    Stats copy;
    AcquireSRWLockShared(&g_statLock);
    copy = g_stats;
    ReleaseSRWLockShared(&g_statLock);
    L("SUM", "when={} session={} slot13={} realloc={} unexpected={} offmain={} erases={} writes={} done={} failed={} reads={} save_reads={} flush={} "
             "inst_found={} inst_missing={} skills={} in_class={} not_in_class={} p8E0={} p8E8={} buff_paths={} fx_paths={} pending={} steps={}",
        when, g_session.load(), copy.slot13, copy.slot13Realloc, copy.slot13Unexpected, copy.slot13Other, copy.erases, copy.writes, copy.writesDone,
        copy.writesFailed, copy.reads, copy.readsSave, copy.flushes, copy.instFound, copy.instMissing, copy.skills, copy.skillInClass,
        copy.skillNotInClass, copy.skill8E0, copy.skill8E8, copy.buffPaths, copy.effectPaths, PendingTotal(), g_steps.load());
    for (const auto& [caller, count] : copy.slot13ByCaller) {
        L("SUM_SLOT13", "when={} caller={:X} func={:X} count={}", when, caller, FuncBegin(caller), count);
    }
    for (const auto& [key, count] : copy.erase) {
        L("SUM_ERASE", "when={} {} count={}", when, key, count);
    }
    for (const auto& [key, s] : copy.upkeep) {
        const auto caller = static_cast<std::uint32_t>(key & 0xFFFFFFFF);
        L("SUM_UPKEEP", "when={} type={} caller={:X} func={:X} calls={} changed={} dec={} filt={} rm0={} rmx={} bad={}", when, key >> 32, caller,
            FuncBegin(caller), s.calls, s.changedCalls, s.dec, s.filt, s.rm0, s.rmx, s.bad);
    }
    for (const auto& [key, s] : copy.rule) {
        const auto caller = static_cast<std::uint32_t>(key >> 8);
        L("SUM_RULE", "when={} caller={:X} func={:X} rule={} calls={} true={} main={} other={}", when, caller, FuncBegin(caller), key & 0xFF, s.calls,
            s.trues, s.main, s.other);
    }
    for (const auto& [hash, s] : copy.testRule) {
        std::string callers;
        for (auto c : s.callers) {
            callers += std::format("{}{:X}", callers.empty() ? "" : ",", c);
        }
        L("SUM_RULETEST", "when={} id={} calls={} true={} callers={}", when, TestName(hash), s.calls, s.trues, callers);
    }
    for (const auto& [key, s] : copy.tray) {
        L("SUM_TRAY", "when={} id={} polarity={} calls={} true={}", when, TestName(static_cast<std::uint32_t>(key >> 8)), key & 0xFF, s.calls, s.trues);
    }
    if (!copy.instMissingIds.empty()) {
        std::string ids;
        for (const auto& id : copy.instMissingIds) {
            ids += (ids.empty() ? "" : ",") + id;
        }
        L("SUM_INSTMISSING", "when={} ids={}", when, ids);
    }
}

void ResetSession()
{
    AcquireSRWLockExclusive(&g_statLock);
    g_stats = Stats{};
    ReleaseSRWLockExclusive(&g_statLock);
    g_caps.Reset();
    g_steps = 0;
}

// ------------------------------------------------------------------ 主动测试用的游戏调用（SEH 包住，只用 POD）

struct alignas(16) InstBuf {
    std::uint8_t b[0x410];
};

using CtorFn = void* (*)(void*);
using CopyFn = void* (*)(void*, const void*);
using VecDtorFn = void (*)(void*);
using EhVecDtorFn = void (*)(void*, std::size_t, std::size_t, void*);
using StatTotalFn = float (*)(const void*, std::uint32_t, bool);
using EffectLookupFn = void* (*)(void*, const char*);
using ApplyEffectFn = void* (*)(void*, void*, void*, void*, const void*, float, bool, float, int, std::uint32_t, bool, bool);
using RandFloatFn = float (*)(void*);
using FreeVecFn = void (*)(void*);
using ParseEffectsFn = void (*)(const char*, bool);
using ParseBuffsFn = void (*)(void*, const char*, bool, bool);
using RemoveFn = bool (*)(void*, void*);

template <class F>
F Fn(std::uintptr_t rvaValue)
{
    return reinterpret_cast<F>(g_base + rvaValue);
}

bool G_InstDefault(InstBuf* b)
{
    __try {
        std::memset(b->b, 0, sizeof(b->b));
        Fn<CtorFn>(rva::kInstCtor)(b->b);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_InstCopy(InstBuf* b, std::uintptr_t src)
{
    __try {
        Fn<CopyFn>(rva::kInstCopy)(b->b, reinterpret_cast<const void*>(src));
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 和 482360 尾部相同的析构：+0x3B0 调 2345D0，+0x158 的 25 个 vector 调 B9FBD0(.., 0x18, 0x19, 2342E0)
bool G_InstDestroy(InstBuf* b)
{
    __try {
        Fn<VecDtorFn>(rva::kVecDtor)(b->b + 0x3B0);
        Fn<EhVecDtorFn>(rva::kEhVecDtor)(b->b + 0x158, 0x18, 0x19, reinterpret_cast<void*>(g_base + rva::kElemDtor));
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 经虚表槽位 13 添加（会经过探针自己的槽位 13 hook），refresh=true，source actor 为空
bool G_AddBuff(std::uintptr_t actor, const InstBuf* b, std::uint32_t* guid)
{
    __try {
        auto** vtable = *reinterpret_cast<void***>(actor);
        *guid = reinterpret_cast<Slot13Fn>(vtable[13])(reinterpret_cast<void*>(actor), b->b, nullptr, true);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 槽位 14 的谓词：MSVC std::function 兼容对象（同 DDCore 事件桥的做法：实现对象常驻，复制、移动都返回自己，析构什么都不做）
struct GuidPredicate {
    void* const* vtable;
    std::uint32_t guid;
    std::uint32_t hits;
};
void* PredCopy(GuidPredicate* self, void*)
{
    return self;
}
void* PredMove(GuidPredicate* self, void*)
{
    return self;
}
bool PredDoCall(GuidPredicate* self, const std::uint8_t* entry)
{
    std::uint32_t guid = 0;
    std::memcpy(&guid, entry, 4);
    const bool hit = guid == self->guid;
    self->hits += hit ? 1 : 0;
    return hit;
}
const void* PredTarget(GuidPredicate*)
{
    static const int kDummy = 0;
    return &kDummy;
}
void PredDelete(GuidPredicate*, bool) {}
const void* PredGet(GuidPredicate* self)
{
    return self;
}
void* const kPredVtable[] = {
    reinterpret_cast<void*>(&PredCopy),
    reinterpret_cast<void*>(&PredMove),
    reinterpret_cast<void*>(&PredDoCall),
    reinterpret_cast<void*>(&PredTarget),
    reinterpret_cast<void*>(&PredDelete),
    reinterpret_cast<void*>(&PredGet),
};
GuidPredicate g_pred{ kPredVtable, 0, 0 };

bool G_RemoveByGuid(std::uintptr_t actor, std::uint32_t guid, bool* removed, std::uint32_t* hits)
{
    alignas(16) std::uint8_t function[0x40] = {};
    void* impl = &g_pred;
    std::memcpy(function + 0x38, &impl, sizeof(impl));
    g_pred.guid = guid;
    g_pred.hits = 0;
    __try {
        auto** vtable = *reinterpret_cast<void***>(actor);
        *removed = reinterpret_cast<RemoveFn>(vtable[14])(reinterpret_cast<void*>(actor), function);
        *hits = g_pred.hits;
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 8 个战斗属性（+0x490 起每个 0x68，掩码在 +0x28）：attack、crit、dmg_low、dmg_high、defense、protection、speed、max_hp
// 顺序按 ActorCombatStat::GetTypeFromId 的推测，speed 在下标 6（+0x700），探针用 SPD5 核对
bool G_Stats(std::uintptr_t actor, float* out)
{
    __try {
        for (int i = 0; i < 8; ++i) {
            const std::uintptr_t stat = actor + 0x490 + static_cast<std::uintptr_t>(i) * 0x68;
            out[i] = Fn<StatTotalFn>(rva::kStatTotal)(reinterpret_cast<const void*>(stat), *reinterpret_cast<const std::uint32_t*>(stat + 0x28), true);
        }
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_EffectLookup(const char* name, std::uintptr_t* out)
{
    __try {
        *out = reinterpret_cast<std::uintptr_t>(Fn<EffectLookupFn>(rva::kEffectLookup)(nullptr, name));
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint8_t g_prngCopy[0x40] = {};  // 全局 prng 的副本，不推进游戏自己的随机数；放在全局，effect 即使留了引用也不会悬空

bool G_ApplyEffect(std::uintptr_t effect, std::uintptr_t hero, std::uint8_t* res, float* roll)
{
    __try {
        std::memcpy(g_prngCopy, reinterpret_cast<const void*>(g_base + rva::kPrng), sizeof(g_prngCopy));
        *roll = Fn<RandFloatFn>(rva::kRandFloat)(g_prngCopy);
        Fn<ApplyEffectFn>(rva::kApplyEffect)(res, g_prngCopy, reinterpret_cast<void*>(hero), reinterpret_cast<void*>(hero),
            reinterpret_cast<const void*>(effect), *roll, false, 1.0f, 0, 0, false, false);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_FreeU32Vec(std::uint8_t* vec)
{
    __try {
        Fn<FreeVecFn>(rva::kFreeU32Vec)(vec);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_ParseEffects(const char* path)
{
    __try {
        Fn<ParseEffectsFn>(rva::kParseEffects)(path, false);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool G_ParseBuffs(std::uintptr_t lib, const char* path)
{
    __try {
        Fn<ParseBuffsFn>(rva::kParseBuffs)(reinterpret_cast<void*>(lib), path, false, false);
        return true;
    } __except (g_lastExc = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ActiveAllowed()
{
    return g_cfg.active && !g_activeDisabled.load();
}

void ActiveFailed(const char* step)
{
    g_activeDisabled = true;
    W("ACTFAIL", "step={} code={:X}，停掉之后的全部主动测试", step, g_lastExc);
}

std::string StatsText(const float* s)
{
    return std::format("{:g}/{:g}/{:g}/{:g}/{:g}/{:g}/{:g}/{:g}", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

// 把一条实例加给英雄：from 非 0 时复制库里的定义再改字段，否则按 spec 从头构造
bool AddInstance(std::uintptr_t hero, std::uintptr_t from, const BuffSpec& spec, bool overrideOnly, std::uint32_t* guid, const char* step)
{
    InstBuf b;
    const bool built = from ? G_InstCopy(&b, from) : G_InstDefault(&b);
    if (!built) {
        ActiveFailed(step);
        return false;
    }
    bool ok = true;
    if (from && overrideOnly) {
        Put32(b.b, 0x4C, spec.dur);
        Put32(b.b, 0x50, spec.dtype);
        Put32(b.b, 0x54, spec.src);
        Put32(b.b, 0x58, 0);
        Put32(b.b, 0xA8, 0);
    } else {
        ok = FillInstance(b.b, &spec);
    }
    std::uint32_t g = 0;
    {
        OursScope ours;
        ok = ok && G_AddBuff(hero, &b, &g);
    }
    const bool destroyed = G_InstDestroy(&b);
    if (!ok || !destroyed) {
        ActiveFailed(step);
        return false;
    }
    *guid = g;
    return true;
}

// ------------------------------------------------------------------ 主动测试：调度

struct Pending {
    std::string label;
    std::uint32_t heroId = 0;
    std::vector<std::uint32_t> guids;
    std::vector<std::uint32_t> idHashes;
    std::uint64_t observeAt = 0;
    bool haveStats = false;
    float before[8]{};
    float afterAdd[8]{};
};
std::optional<Pending> g_pending;
std::uint64_t g_nextTestFrame = 0;
std::uint64_t g_townSince = 0, g_corridorSince = 0;

struct DoneFlags {
    bool townAdd = false, townRule = false, townEnum = false;
    bool corrAdd = false, corrRule = false, corrTray = false, corrEffect = false;
} g_done;

bool g_t11Added = false;     // 本次运行已经加过 0A#11 的测试 buff
bool g_t11Observed = false;  // 已经观察到并移除

std::string RuleTestText(const std::vector<std::uint32_t>& hashes)
{
    std::string out;
    AcquireSRWLockShared(&g_statLock);
    for (auto h : hashes) {
        if (auto it = g_stats.testRule.find(h); it != g_stats.testRule.end()) {
            out += std::format(" rule[{}]={}/{}", TestName(h), it->second.trues, it->second.calls);
        }
        for (const auto& [key, s] : g_stats.tray) {
            if (static_cast<std::uint32_t>(key >> 8) == h) {
                out += std::format(" tray[{} p{}]={}/{}", TestName(h), key & 0xFF, s.trues, s.calls);
            }
        }
    }
    ReleaseSRWLockShared(&g_statLock);
    return out;
}

void ObserveAndRemove(const char* when)
{
    Pending p = *g_pending;
    g_pending.reset();
    const auto hero = FindHero(p.heroId);
    if (!hero) {
        W("ACTOBS", "label={} hero#{} 找不到了（解雇？），测试 buff 没有移除 guids={}", p.label, p.heroId, p.guids.size());
        return;
    }
    float now[8]{};
    const bool haveNow = G_Stats(hero, now);
    const auto table = ReadTable(hero, 256);
    std::string present;
    for (auto g : p.guids) {
        const Entry* e = nullptr;
        for (const auto& x : table) {
            if (x.guid == g) {
                e = &x;
            }
        }
        present += e ? " " + EntryText(*e) : std::format(" {}:不在了", g);
    }
    L("ACTOBS", "label={} when={} hero={} {} stats_before={} stats_added={} stats_now={} entries:{}{}", p.label, when, Desc(hero), StateText(),
        p.haveStats ? StatsText(p.before) : std::string("-"), p.haveStats ? StatsText(p.afterAdd) : std::string("-"),
        haveNow ? StatsText(now) : std::string("-"), present, RuleTestText(p.idHashes));
    for (auto g : p.guids) {
        const auto n0 = TableCount(hero);
        bool removed = false;
        std::uint32_t hits = 0;
        bool ok = false;
        {
            OursScope ours;
            ok = G_RemoveByGuid(hero, g, &removed, &hits);
        }
        if (!ok) {
            ActiveFailed("remove_by_guid");
            return;
        }
        L("ACTDEL", "label={} hero={} guid={} slot14_ret={} pred_hits={} n0={} n1={}", p.label, Desc(hero), g, removed ? 1 : 0, hits, n0, TableCount(hero));
    }
    float after[8]{};
    if (G_Stats(hero, after)) {
        L("ACTDONE", "label={} hero={} stats_after_remove={}", p.label, Desc(hero), StatsText(after));
    }
}

void StartPending(Pending p, std::uintptr_t hero, int frames)
{
    p.observeAt = g_frame.load() + static_cast<std::uint64_t>(frames);
    p.heroId = ActorIdOf(hero, kHero);
    g_pending = std::move(p);
}

// 0A#4：复制原版 SPD5（combat_stat_add speed_rating +5），来源 1、target 0、duration 3、after_turn
void TestAddCopy(const char* label, std::uintptr_t hero)
{
    const auto def = MapFind(LibraryMap(), Hash("SPD5"));
    if (!def) {
        W("ACT", "label={} Buff 库里没有 SPD5，跳过", label);
        return;
    }
    Pending p;
    p.label = label;
    p.haveStats = G_Stats(hero, p.before);
    const auto n0 = TableCount(hero);
    const auto next0 = Rd<std::uint32_t>(hero + 0x478);
    const BuffSpec spec = { "SPD5", 0, "", 0, 3, 9, 1, 0, false, "", false };
    std::uint32_t guid = 0;
    if (!AddInstance(hero, def, spec, true, &guid, label)) {
        return;
    }
    G_Stats(hero, p.afterAdd);
    p.guids = { guid };
    L("ACT", "label={} hero={} {} def=SPD5 def_stat={} def_sub={} def_amt={:g} guid={} next0={} next1={} n0={} n1={} spd {:g}->{:g}", label, Desc(hero),
        StateText(), Rd<std::int32_t>(def), Q(RdStr(def + 4, 0x40)), Rd<float>(def + 0x48), guid, next0, Rd<std::uint32_t>(hero + 0x478), n0,
        TableCount(hero), p.before[6], p.afterAdd[6]);
    StartPending(std::move(p), hero, 90);
}

// 0A#6：rule_type 42（is_actor_status）+ rule_data.string "lua_test"，is_false_rule 0 和 1 各一条（speed +3、+4）
void TestRule(const char* label, std::uintptr_t hero)
{
    Pending p;
    p.label = label;
    p.haveStats = G_Stats(hero, p.before);
    const BuffSpec a = { kIdRule42, 2, "speed_rating", 3.0f, 3, 9, 0, 42, false, "lua_test", false };
    const BuffSpec b = { kIdRule42n, 2, "speed_rating", 4.0f, 3, 9, 0, 42, true, "lua_test", false };
    std::uint32_t ga = 0, gb = 0;
    if (!AddInstance(hero, 0, a, false, &ga, label) || !AddInstance(hero, 0, b, false, &gb, label)) {
        return;
    }
    G_Stats(hero, p.afterAdd);
    p.guids = { ga, gb };
    p.idHashes = { Hash(kIdRule42), Hash(kIdRule42n) };
    L("ACT", "label={} hero={} {} guid42={} guid42n={} rstr_hash={:X} spd {:g}->{:g}", label, Desc(hero), StateText(), ga, gb, Hash("lua_test"),
        p.before[6], p.afterAdd[6]);
    StartPending(std::move(p), hero, 90);
}

// stat 102 的安全检查：47AD50 先看 stat 表 +0x52/+0x53（按 actor 种类选一个），为 0 就跳过，不会写 Actor+0xB70[102]
bool Stat102Safe(std::string* why)
{
    const std::uintptr_t row = g_base + rva::kStatTable + 102 * 0x58;
    const auto b52 = Rd<std::uint8_t>(row + 0x52, 0xFF);
    const auto b53 = Rd<std::uint8_t>(row + 0x53, 0xFF);
    *why = std::format("stat表[102]+0x52={} +0x53={} +0x44={}", b52, b53, Rd<std::uint8_t>(row + 0x44, 0xFF));
    return b52 == 0 && b53 == 0;
}

// 0A#7：库里 4AA770 解析出的 ddprobe_p4_badstat（stat_type lua_stat）、ddprobe_p4_badrule（rule_type lua_rule）
void TestBadEnum(const char* label, std::uintptr_t hero)
{
    const auto map = LibraryMap();
    const auto defStat = MapFind(map, Hash(kIdBadStat));
    const auto defRule = MapFind(map, Hash(kIdBadRule));
    std::string why;
    const bool safe = Stat102Safe(&why);
    Pending p;
    p.label = label;
    p.haveStats = G_Stats(hero, p.before);
    const BuffSpec over = { "", 0, "", 0, 3, 9, 0, 0, false, "", false };
    std::uint32_t g = 0;
    if (defStat && safe && Rd<std::int32_t>(defStat) == 102) {
        if (!AddInstance(hero, defStat, over, true, &g, label)) {
            return;
        }
        p.guids.push_back(g);
        p.idHashes.push_back(Hash(kIdBadStat));
    } else {
        W("ACT", "label={} 不加 badstat：def={:X} stat={} safe={} {}", label, defStat, defStat ? Rd<std::int32_t>(defStat) : -1, safe ? 1 : 0, why);
    }
    if (defRule && Rd<std::int32_t>(defRule + 0x5C) == 55) {
        if (!AddInstance(hero, defRule, over, true, &g, label)) {
            return;
        }
        p.guids.push_back(g);
        p.idHashes.push_back(Hash(kIdBadRule));
    } else {
        W("ACT", "label={} 不加 badrule：def={:X} rule={}", label, defRule, defRule ? Rd<std::int32_t>(defRule + 0x5C) : -1);
    }
    if (p.guids.empty()) {
        return;
    }
    G_Stats(hero, p.afterAdd);
    L("ACT", "label={} hero={} {} guids={} {} spd {:g}->{:g}", label, Desc(hero), StateText(), p.guids.size(), why, p.before[6], p.afterAdd[6]);
    StartPending(std::move(p), hero, 90);
}

// 0A#12：来源 1（表 +0x48 为真）和来源 6（为假）的两条 speed +1
void TestTray(const char* label, std::uintptr_t hero)
{
    Pending p;
    p.label = label;
    p.haveStats = G_Stats(hero, p.before);
    const BuffSpec a = { kIdTray1, 2, "speed_rating", 1.0f, 3, 9, 1, 0, false, "", false };
    const BuffSpec b = { kIdTray6, 2, "speed_rating", 1.0f, 3, 9, 6, 0, false, "", false };
    std::uint32_t ga = 0, gb = 0;
    if (!AddInstance(hero, 0, a, false, &ga, label) || !AddInstance(hero, 0, b, false, &gb, label)) {
        return;
    }
    G_Stats(hero, p.afterAdd);
    p.guids = { ga, gb };
    p.idHashes = { Hash(kIdTray1), Hash(kIdTray6) };
    const auto flag = [](int src) { return Rd<std::uint8_t>(g_base + rva::kSourceTable + static_cast<std::uintptr_t>(src) * 0x4C + 0x48); };
    L("ACT", "label={} hero={} {} guid1={} guid6={} trayflag1={} trayflag6={} spd {:g}->{:g}", label, Desc(hero), StateText(), ga, gb, flag(1), flag(6),
        p.before[6], p.afterAdd[6]);
    StartPending(std::move(p), hero, 150);
}

// 0A#9：4CB4D0 施加原版 effect rakebuff_5（performer 和 target 都是这个英雄）
void TestEffect(const char* label, std::uintptr_t hero)
{
    const auto raid = RaidPtr();
    const auto modeName = Rd<std::uintptr_t>(raid + 0x52C0);
    if (modeName && RdStr(modeName, 16) == "arena_mp") {
        W("ACT", "label={} PvP 对局，跳过", label);
        g_done.corrEffect = true;
        return;
    }
    std::uintptr_t effect = 0;
    if (!G_EffectLookup("rakebuff_5", &effect)) {
        ActiveFailed(label);
        return;
    }
    if (!effect) {
        W("ACT", "label={} 找不到 effect rakebuff_5，跳过", label);
        return;
    }
    Pending p;
    p.label = label;
    p.haveStats = G_Stats(hero, p.before);
    const auto before = ReadTable(hero, 256);
    alignas(16) std::uint8_t res[0x20] = {};
    float roll = 0;
    bool ok = false;
    {
        OursScope ours;
        ok = G_ApplyEffect(effect, hero, res, &roll);
    }
    if (!ok) {
        ActiveFailed(label);
        return;
    }
    std::uintptr_t vec[2] = {};
    std::memcpy(vec, res + 8, sizeof(vec));
    std::vector<std::uint32_t> resGuids;
    if (vec[0] && vec[1] >= vec[0] && (vec[1] - vec[0]) / 4 < 64) {
        resGuids.resize((vec[1] - vec[0]) / 4);
        ReadBytes(vec[0], resGuids.data(), resGuids.size() * 4);
    }
    if (!G_FreeU32Vec(res + 8)) {
        ActiveFailed(label);
        return;
    }
    const auto after = ReadTable(hero, 256);
    std::string added;
    std::vector<std::uint32_t> newGuids;
    for (const auto& e : after) {
        bool old = false;
        for (const auto& b : before) {
            old = old || b.guid == e.guid;
        }
        if (!old) {
            newGuids.push_back(e.guid);
            added += " " + EntryText(e);
        }
    }
    std::string resText;
    for (auto g : resGuids) {
        resText += std::format("{}{}", resText.empty() ? "" : ",", g);
    }
    G_Stats(hero, p.afterAdd);
    L("ACT", "label={} hero={} {} effect=rakebuff_5 ptr={:X} name={} roll={:.4f} res_interrupt={} res_resisted={} res_cleared={} res_guids={} new:{}",
        label, Desc(hero), StateText(), effect, Q(RdStr(effect, 0x40)), roll, res[0], res[2], res[3], resText.empty() ? "-" : resText,
        added.empty() ? " -" : added);
    p.guids = newGuids;
    if (p.guids.empty()) {
        return;
    }
    StartPending(std::move(p), hero, 60);
}

// ------------------------------------------------------------------ CampaignLoad 时的项目

void CampaignInfo(const DDPhaseEvent& event)
{
    const auto sm = Rd<std::uintptr_t>(g_base + rva::kStorageManager);
    const auto dev = Rd<std::uintptr_t>(sm + 0x38);
    const auto vt = Rd<std::uintptr_t>(dev);
    const char* devKind = vt == g_base + rva::kSteamDevVtbl ? "SteamStorageDevice" : (vt == g_base + rva::kWinDevVtbl ? "WindowsStorageDevice" : "未知");
    std::string prefix;
    if (vt == g_base + rva::kWinDevVtbl) {
        wchar_t root[0x104] = {};
        ReadBytes(dev + 0xA8, root, sizeof(root) - 2);
        const int n = WideCharToMultiByte(CP_UTF8, 0, root, -1, nullptr, 0, nullptr, nullptr);
        prefix.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        if (n > 0) {
            WideCharToMultiByte(CP_UTF8, 0, root, -1, prefix.data(), n, nullptr, nullptr);
            prefix.resize(std::strlen(prefix.c_str()));
        }
    } else {
        prefix = RdStr(dev + 0xA8, 0x100);
    }
    const auto flushTid = Rd<std::uint32_t>(g_base + rva::kFlushMainThread);
    L("CAMPAIGN", "phase=load serial={} late={} {} app={} slot5_vtbl={:X} kind={} slot5_name={} slot5_prefix={} flush_tid={} main_tid={} same={} "
                  "backup_flag={} lib={:X} roster={} campaign={:X}",
        event.campaign_serial, event.late, StateText(), Q(CurrentProfileDevice()), vt ? vt - g_base : 0, devKind, Q(dev ? RdStr(dev + 0x20, 0x80) : std::string()),
        Q(prefix), flushTid, g_mainTid, flushTid == g_mainTid ? 1 : 0, Rd<std::uint8_t>(g_base + rva::kBackupFlag),
        Rd<std::uintptr_t>(g_base + rva::kBuffLibrary), RosterHeroes().size(), Rd<std::uintptr_t>(g_base + rva::kCampaignCurrent));
}

void T11Observe()
{
    const std::uint32_t hashMain = Hash(kIdMain);
    const std::uint32_t hashLoad = Hash(kIdLoad);
    const auto map = LibraryMap();
    const auto defMain = MapFind(map, hashMain);
    const auto defLoad = MapFind(map, hashLoad);
    L("T11DEF", "phase=before_insert def_main={:X} def_load={:X} def_load_fp={} def_load_amt={:g}", defMain, defLoad,
        defLoad ? Rd<std::uint8_t>(defLoad + 0x40A) : 0, defLoad ? Rd<float>(defLoad + 0x48) : 0.0f);
    int found = 0;
    for (auto hero : RosterHeroes()) {
        for (const auto& e : ReadTable(hero, 256)) {
            if (e.idHash != hashMain && e.idHash != hashLoad) {
                continue;
            }
            ++found;
            L("T11OBS", "hero={} entry={} fp={} fn={} amt={:g} stat={} guid_lt_next={} def_now={}", Desc(hero), EntryText(e), e.forcePos, e.forceNeg,
                e.amount, e.stat, e.guid < Rd<std::uint32_t>(hero + 0x478) ? 1 : 0, MapFind(map, e.idHash) ? 1 : 0);
            if (ActiveAllowed()) {
                bool removed = false;
                std::uint32_t hits = 0;
                bool ok = false;
                {
                    OursScope ours;
                    ok = G_RemoveByGuid(hero, e.guid, &removed, &hits);
                }
                if (!ok) {
                    ActiveFailed("t11_remove");
                    return;
                }
                L("T11DEL", "hero={} guid={} id={} slot14_ret={} pred_hits={}", Desc(hero), e.guid, e.id, removed ? 1 : 0, hits);
            }
        }
    }
    if (found) {
        g_t11Observed = true;
    }
    L("T11SCAN", "found={} added_this_run={} roster={}", found, g_t11Added ? 1 : 0, RosterHeroes().size());
}

void T11InsertAndAdd()
{
    const auto map = LibraryMap();
    std::uintptr_t def = 0;
    if (!G_MapIndex(map, Hash(kIdMain), &def) || !def || !FillInstance(reinterpret_cast<std::uint8_t*>(def), &kSpecMain)) {
        ActiveFailed("t11_insert");
        return;
    }
    L("T11DEF", "phase=inserted def_main={:X} found_again={:X} stat={} sub={} amt={:g} fp={}", def, MapFind(map, Hash(kIdMain)), Rd<std::int32_t>(def),
        Q(RdStr(def + 4, 0x40)), Rd<float>(def + 0x48), Rd<std::uint8_t>(def + 0x40A));
    if (g_t11Added || g_t11Observed) {
        L("T11ADD", "skip=1 原因={}", g_t11Added ? "本次运行已经加过" : "本次运行已经观察并移除过");
        return;
    }
    const auto hero = ChooseRosterHero();
    if (!hero) {
        W("T11ADD", "skip=1 原因=名册里没有可用的英雄");
        return;
    }
    const auto defLoad = MapFind(map, Hash(kIdLoad));
    float before[8]{}, after[8]{};
    G_Stats(hero, before);
    const BuffSpec over = { "", 0, "", 0, 1, 5, 1, 0, false, "", false };  // duration 1、idle_start_town_visit、来源 1
    std::uint32_t gMain = 0, gLoad = 0;
    if (!AddInstance(hero, def, over, true, &gMain, "t11_add_main")) {
        return;
    }
    if (defLoad && !AddInstance(hero, defLoad, over, true, &gLoad, "t11_add_load")) {
        return;
    }
    G_Stats(hero, after);
    g_t11Added = true;
    L("T11ADD", "hero={} guid_main={} guid_load={} def_load={:X} spd {:g}->{:g} next={}（等游戏自动存档，下次进存档时观察）", Desc(hero), gMain,
        defLoad ? std::to_string(gLoad) : std::string("-"), defLoad, before[6], after[6], Rd<std::uint32_t>(hero + 0x478));
}

void Fx8Lookup()
{
    for (const char* name : { "rakebuff_5", "ddprobe_p4_no_such_effect" }) {
        std::uintptr_t p = 0;
        if (!G_EffectLookup(name, &p)) {
            ActiveFailed("fx8");
            return;
        }
        L("FX8", "name={} ptr={:X} name_at0={}", Q(name), p, p ? Q(RdStr(p, 0x40)) : std::string("-"));
    }
}

bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(file);
    return ok;
}

std::string EffectFloats(std::uintptr_t effect)
{
    std::string out;
    for (int i = 0; i < 10; ++i) {
        out += std::format("{}{:g}", i ? "/" : "", Rd<float>(effect + 0x78 + static_cast<std::uintptr_t>(i) * 4));
    }
    return out;
}

// 0A#10：4F02D0 运行时解析 effect，绝对路径和相对游戏根目录的路径各一次，再同名重解析一次
void Fx10Parse()
{
    const std::string dirRel = "DDModLoader/probe_p4_tmp";
    const std::string dirAbs = g_gameRoot + "\\DDModLoader\\probe_p4_tmp";
    CreateDirectoryW(Wide(dirAbs).c_str(), nullptr);
    const auto mapSize = [] { return Rd<std::uint64_t>(g_base + rva::kEffectMap + 8); };
    const auto effectText = [](const char* name, int speed) {
        return std::format("effect: .name \"{}\" .target \"performer\" .chance 100% .combat_stat_buff 1 .speed_rating_add {} .on_hit true "
                           ".on_miss true .duration 1\r\n",
            name, speed);
    };
    std::uintptr_t prevAbs = 0;
    G_EffectLookup("ddprobe_p4_fx_abs", &prevAbs);
    L("FX10", "phase=start map_size={} 上个存档留下的 ddprobe_p4_fx_abs={:X}", mapSize(), prevAbs);
    struct Attempt {
        const char* name;
        std::string path;
        std::wstring file;
        std::uintptr_t ptr = 0;
    };
    Attempt attempts[2] = {
        { "ddprobe_p4_fx_abs", dirAbs + "\\ddprobe_p4_abs.effects.darkest", Wide(dirAbs + "\\ddprobe_p4_abs.effects.darkest") },
        { "ddprobe_p4_fx_rel", dirRel + "/ddprobe_p4_rel.effects.darkest", Wide(dirAbs + "\\ddprobe_p4_rel.effects.darkest") },
    };
    for (auto& a : attempts) {
        const bool written = WriteTextFile(a.file, effectText(a.name, 1));
        const auto size0 = mapSize();
        bool ok = false;
        {
            OursScope ours;
            ok = G_ParseEffects(a.path.c_str());
        }
        if (!ok) {
            ActiveFailed("fx10_parse");
            return;
        }
        G_EffectLookup(a.name, &a.ptr);
        L("FX10", "phase=parse name={} path={} written={} map_size {}->{} ptr={:X} target={} floats={}", a.name, Q(a.path), written ? 1 : 0, size0, mapSize(),
            a.ptr, a.ptr ? Rd<std::int32_t>(a.ptr + 0x44) : -1, a.ptr ? EffectFloats(a.ptr) : std::string("-"));
    }
    const Attempt* good = attempts[0].ptr ? &attempts[0] : (attempts[1].ptr ? &attempts[1] : nullptr);
    if (good) {
        WriteTextFile(good->file, effectText(good->name, 2));
        bool ok = false;
        {
            OursScope ours;
            ok = G_ParseEffects(good->path.c_str());
        }
        if (!ok) {
            ActiveFailed("fx10_reparse");
            return;
        }
        std::uintptr_t again = 0;
        G_EffectLookup(good->name, &again);
        L("FX10", "phase=reparse name={} ptr {:X}->{:X} same={} floats={}", good->name, good->ptr, again, again == good->ptr ? 1 : 0,
            again ? EffectFloats(again) : std::string("-"));
    }
    for (const auto& a : attempts) {
        DeleteFileW(a.file.c_str());
    }
}

// 0A#7：4AA770 解析带未知 stat_type、rule_type 的 buff 文件（validate=0），读回库里的枚举值
void Buff7Parse()
{
    const std::string dirAbs = g_gameRoot + "\\DDModLoader\\probe_p4_tmp";
    CreateDirectoryW(Wide(dirAbs).c_str(), nullptr);
    const std::string json =
        "{\r\n\"buffs\": [\r\n"
        "{\"id\": \"ddprobe_p4_badstat\", \"stat_type\": \"lua_stat\", \"stat_sub_type\": \"speed_rating\", \"amount\": 7, "
        "\"remove_if_not_active\": false, \"rule_type\": \"always\", \"is_false_rule\": false, \"rule_data\": {\"float\": 0, \"string\": \"\"}},\r\n"
        "{\"id\": \"ddprobe_p4_badrule\", \"stat_type\": \"combat_stat_add\", \"stat_sub_type\": \"speed_rating\", \"amount\": 6, "
        "\"remove_if_not_active\": false, \"rule_type\": \"lua_rule\", \"is_false_rule\": false, \"rule_data\": {\"float\": 0, \"string\": \"\"}}\r\n"
        "]\r\n}\r\n";
    const std::wstring file = Wide(dirAbs + "\\ddprobe_p4.buffs.json");
    const bool written = WriteTextFile(file, json);
    const std::string paths[2] = { dirAbs + "\\ddprobe_p4.buffs.json", "DDModLoader/probe_p4_tmp/ddprobe_p4.buffs.json" };
    const auto lib = Rd<std::uintptr_t>(g_base + rva::kBuffLibrary);
    for (const auto& path : paths) {
        bool ok = false;
        {
            OursScope ours;
            ok = G_ParseBuffs(lib, path.c_str());
        }
        if (!ok) {
            ActiveFailed("buff7_parse");
            break;
        }
        const auto map = LibraryMap();
        const auto ds = MapFind(map, Hash(kIdBadStat));
        const auto dr = MapFind(map, Hash(kIdBadRule));
        L("BUFF7", "path={} written={} badstat={:X} stat={} badrule={:X} rule={} rule_stat={}", Q(path), written ? 1 : 0, ds, ds ? Rd<std::int32_t>(ds) : -1, dr,
            dr ? Rd<std::int32_t>(dr + 0x5C) : -1, dr ? Rd<std::int32_t>(dr) : -1);
        if (ds && dr) {
            break;
        }
    }
    DeleteFileW(file.c_str());
}

// ------------------------------------------------------------------ 0B#12：Steam 导出和 SteamID

int g_steamTries = 0;
bool g_steamDone = false;

using SteamUserFn = void* (*)();
using GetSteamIdFn = std::uint64_t (*)(void*);

bool CallSteam(void* userFn, void* idFn, void** user, std::uint64_t* id)
{
    __try {
        *user = reinterpret_cast<SteamUserFn>(userFn)();
        *id = *user ? reinterpret_cast<GetSteamIdFn>(idFn)(*user) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SteamProbe()
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) {
        g_steamDone = true;
        L("STEAM", "steam_api64.dll 没有加载（离线版？）");
        return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(steam);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    std::vector<std::string> user, ids;
    std::string best;
    if (dir.VirtualAddress) {
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
            const std::string name = reinterpret_cast<const char*>(base + names[i]);
            if (name.rfind("SteamAPI_SteamUser_v", 0) == 0) {
                user.push_back(name);
                best = (std::max)(best, name);
            } else if (name == "SteamUser" || name.find("GetSteamID") != std::string::npos) {
                ids.push_back(name);
            }
        }
    }
    std::string userList, idList;
    for (const auto& n : user) {
        userList += (userList.empty() ? "" : ",") + n;
    }
    for (const auto& n : ids) {
        idList += (idList.empty() ? "" : ",") + n;
    }
    if (best.empty() && GetProcAddress(steam, "SteamUser")) {
        best = "SteamUser";
    }
    void* userFn = best.empty() ? nullptr : reinterpret_cast<void*>(GetProcAddress(steam, best.c_str()));
    void* idFn = reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID"));
    void* userPtr = nullptr;
    std::uint64_t id = 0;
    const bool called = userFn && idFn && CallSteam(userFn, idFn, &userPtr, &id);
    if (called && !userPtr && ++g_steamTries < 6) {
        L("STEAM", "accessor={} 返回空，稍后再试（第 {} 次）", best, g_steamTries);
        return;
    }
    g_steamDone = true;
    const auto id32 = static_cast<std::uint32_t>(id & 0xFFFFFFFF);
    // userdata 在 <Steam>\userdata：游戏根目录是 <Steam>\steamapps\common\<目录>
    std::string steamRoot = g_gameRoot;
    for (int i = 0; i < 3; ++i) {
        const auto pos = steamRoot.find_last_of('\\');
        steamRoot = pos == std::string::npos ? steamRoot : steamRoot.substr(0, pos);
    }
    std::string dirs;
    bool match = false, remote = false;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(Wide(steamRoot + "\\userdata\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                char name[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), nullptr, nullptr);
                dirs += (dirs.empty() ? "" : ",") + std::string(name);
                if (std::to_string(id32) == name) {
                    match = true;
                    remote = GetFileAttributesW(Wide(steamRoot + "\\userdata\\" + name + "\\262060\\remote").c_str()) != INVALID_FILE_ATTRIBUTES;
                }
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    L("STEAM", "exports_user={} exports_id={} accessor={} called={} user={:X} steamid={} id32={} userdata={} match={} remote_262060={}",
        userList.empty() ? "-" : userList, idList.empty() ? "-" : idList, best.empty() ? "-" : best, called ? 1 : 0,
        reinterpret_cast<std::uintptr_t>(userPtr), id, id32, dirs.empty() ? "-" : dirs, match ? 1 : 0, remote ? 1 : 0);
}

// ------------------------------------------------------------------ 每帧

int g_prevLast = -100, g_prevNext = -100, g_prevBattle = -100;
std::uintptr_t g_prevRaid = 1;

void TrackState()
{
    const int last = StateLast();
    const int next = StateNext();
    const auto raid = RaidPtr();
    const int battle = BattleState(raid);
    if (last != g_prevLast || next != g_prevNext || (raid != 0) != (g_prevRaid != 0) || battle != g_prevBattle) {
        g_prevLast = last;
        g_prevNext = next;
        g_prevRaid = raid;
        g_prevBattle = battle;
        L("STATE", "last={} next={} raid={} batt={} campaign={} loading={}", last, next, raid ? 1 : 0, battle,
            Rd<std::uintptr_t>(g_base + rva::kCampaignCurrent) ? 1 : 0, Loading() ? 1 : 0);
    }
}

void FlushSaveGaps()
{
    std::vector<SaveGap> ready;
    {
        std::lock_guard lock(g_gapMutex);
        if (g_gaps.empty()) {
            return;
        }
        ready.swap(g_gaps);
    }
    // 同一帧里的写入合成一行：写入那一刻到本帧 TICK（下一次 OnFrameBody）之间主线程上的游戏活动
    std::map<std::uint64_t, std::vector<const SaveGap*>> byFrame;
    for (const auto& g : ready) {
        byFrame[g.frame].push_back(&g);
    }
    for (const auto& [frame, list] : byFrame) {
        std::string files;
        for (const auto* g : list) {
            files += std::format("{}{}", files.empty() ? "" : ",", g->file);
        }
        const SaveGap* last = list.back();
        L("SAVEGAP", "wframe={} files={} d_add={} d_erase={} d_upkeep={} d_skill={} {}", frame, files, g_act.adds.load() - last->adds,
            g_act.erases.load() - last->erases, g_act.upkeeps.load() - last->upkeeps, g_act.skills.load() - last->skills, StateText());
    }
}

void RunActiveTests()
{
    if (!ActiveAllowed() || !g_inCampaign || Loading()) {
        g_townSince = g_corridorSince = 0;
        return;
    }
    const std::uint64_t frame = g_frame.load();
    if (g_pending) {
        if (frame >= g_pending->observeAt) {
            ObserveAndRemove("tick");
            g_nextTestFrame = frame + 30;
        }
        return;
    }
    if (frame < g_nextTestFrame) {
        return;
    }
    const int last = StateLast();
    const int next = StateNext();
    const auto raid = RaidPtr();
    const bool town = last == 19 && next == 19 && raid == 0 && Rd<std::uintptr_t>(g_base + rva::kCampaignCurrent);
    const bool corridor = last == 8 && next == 8 && raid && BattleState(raid) == 0 && BattleMonsters().empty() &&
        Rd<std::uintptr_t>(g_base + rva::kRaidDisplay) && !PartyHeroes().empty();
    g_townSince = town ? (g_townSince ? g_townSince : frame) : 0;
    g_corridorSince = corridor ? (g_corridorSince ? g_corridorSince : frame) : 0;
    if (town && frame - g_townSince >= 60) {
        const auto hero = ChooseRosterHero();
        if (!hero) {
            return;
        }
        if (!g_done.townAdd) {
            g_done.townAdd = true;
            TestAddCopy("town_add", hero);
        } else if (!g_done.townRule) {
            g_done.townRule = true;
            TestRule("town_rule42", hero);
        } else if (!g_done.townEnum) {
            g_done.townEnum = true;
            TestBadEnum("town_badenum", hero);
        }
    } else if (corridor && frame - g_corridorSince >= 60) {
        const auto hero = PartyHeroes().front();
        if (!g_done.corrAdd) {
            g_done.corrAdd = true;
            TestAddCopy("corridor_add", hero);
        } else if (!g_done.corrRule) {
            g_done.corrRule = true;
            TestRule("corridor_rule42", hero);
        } else if (!g_done.corrTray) {
            g_done.corrTray = true;
            TestTray("corridor_tray", hero);
        } else if (!g_done.corrEffect) {
            g_done.corrEffect = true;
            TestEffect("corridor_effect", hero);
        }
    }
}

void OnTick(const DDPhaseEvent&)
{
    g_frame.fetch_add(1, std::memory_order_relaxed);
    TrackState();
    FlushSaveGaps();
    if (!g_steamDone && g_frame.load() >= 120 && g_frame.load() % 120 == 0) {
        SteamProbe();
    }
    RunActiveTests();
}

void OnCampaignLoad(const DDPhaseEvent& event)
{
    if (event.campaign_serial == 1) {
        Summary("before_first_load");
    }
    ResetSession();
    g_session = event.campaign_serial;
    g_inCampaign = true;
    g_done = DoneFlags{};
    g_pending.reset();
    g_nextTestFrame = g_frame.load() + 60;
    CampaignInfo(event);
    T11Observe();
    if (!ActiveAllowed()) {
        L("ACTIVE", "主动测试关闭（active={} 出过异常={}），只做被动观察", g_cfg.active ? 1 : 0, g_activeDisabled.load() ? 1 : 0);
        return;
    }
    T11InsertAndAdd();
    if (ActiveAllowed()) {
        Fx8Lookup();
    }
    if (ActiveAllowed()) {
        Fx10Parse();
    }
    if (ActiveAllowed()) {
        Buff7Parse();
    }
}

void OnCampaignUnload(const DDPhaseEvent& event)
{
    if (g_pending && ActiveAllowed()) {
        ObserveAndRemove("campaign_unload");
    } else if (g_pending) {
        W("ACTLEFT", "label={} 退出存档时还没移除（主动测试已停）", g_pending->label);
        g_pending.reset();
    }
    g_inCampaign = false;
    Summary("campaign_unload");
    L("CAMPAIGN", "phase=unload serial={}", event.campaign_serial);
}

// ------------------------------------------------------------------ 配置和状态文件

void ReadConfig()
{
    const std::wstring path = Wide(g_gameRoot + "\\DDModLoader\\cfg\\probe_p4.cfg");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char buffer[4096] = {};
    DWORD got = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(file);
    g_cfgText = "cfg=probe_p4.cfg";
    std::string text(buffer, got);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto trim = [](std::string s) {
            const auto b = s.find_first_not_of(" \t\r");
            const auto e = s.find_last_not_of(" \t\r");
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        const bool on = value == "1" || value == "true";
        if (key == "active") {
            g_cfg.active = on;
        } else if (key == "load_inject") {
            g_cfg.loadInject = on;
        } else if (key == "exit_hook") {
            g_cfg.exitHook = on;
        }
    }
}

// 上次运行最后写入的哈希读进 g_prevWrites，然后重新开始记录本次运行
void OpenStateFile()
{
    const std::string dir = g_gameRoot + "\\DDModLoader\\probe_p4_state";
    CreateDirectoryW(Wide(dir).c_str(), nullptr);
    const std::wstring path = Wide(dir + "\\writes.tsv");
    HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        std::string text;
        char buffer[8192];
        DWORD got = 0;
        while (ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got) {
            text.append(buffer, got);
        }
        CloseHandle(in);
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            const auto t1 = line.find('\t');
            const auto t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
            if (t2 != std::string::npos) {
                g_prevWrites[line.substr(0, t1)] = std::strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 16);
            }
        }
    }
    g_stateFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void InstallHooks()
{
    int ok = 0, total = 0;
    auto count = [&](bool r) {
        ++total;
        ok += r ? 1 : 0;
    };
    count(InstallVtable("p4_slot13_hero", rva::kHeroVtbl, 13, rva::kSlot13Hero, reinterpret_cast<void*>(&Slot13HeroDetour),
        reinterpret_cast<void**>(&g_origSlot13Hero)));
    count(InstallVtable("p4_slot13_monster", rva::kMonsterVtbl, 13, rva::kSlot13Monster, reinterpret_cast<void*>(&Slot13MonsterDetour),
        reinterpret_cast<void**>(&g_origSlot13Monster)));
    count(InstallInline("p4_erase_5C4FF0", 0x5C4FF0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20 }, reinterpret_cast<void*>(&EraseDetour),
        reinterpret_cast<void**>(&g_origErase)));
    count(InstallInline("p4_upkeep_47AAD0", 0x47AAD0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 }, reinterpret_cast<void*>(&UpkeepDetour),
        reinterpret_cast<void**>(&g_origUpkeep)));
    count(InstallInline("p4_rule_479CD0", 0x479CD0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 }, reinterpret_cast<void*>(&RuleDetour),
        reinterpret_cast<void**>(&g_origRule)));
    count(InstallInline("p4_tray_700A00", 0x700A00, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57 }, reinterpret_cast<void*>(&TrayDetour),
        reinterpret_cast<void**>(&g_origTray)));
    count(InstallInline("p4_queuewrite_2487E0", 0x2487E0, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56 }, reinterpret_cast<void*>(&QueueWriteDetour),
        reinterpret_cast<void**>(&g_origQueueWrite)));
    count(InstallInline("p4_steamwrite_3875B0", 0x3875B0, { 0x40, 0x53, 0x55, 0x56, 0x41, 0x56 }, reinterpret_cast<void*>(&SteamWriteDetour),
        reinterpret_cast<void**>(&g_origSteamWrite)));
    count(InstallInline("p4_winwrite_376A00", 0x376A00, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56 }, reinterpret_cast<void*>(&WinWriteDetour),
        reinterpret_cast<void**>(&g_origWinWrite)));
    count(InstallInline("p4_flush_372840", 0x372840, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57 }, reinterpret_cast<void*>(&FlushDetour),
        reinterpret_cast<void**>(&g_origFlush)));
    count(InstallInline("p4_readfile_248D60", 0x248D60, { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&ReadFileDetour),
        reinterpret_cast<void**>(&g_origReadFile)));
    count(InstallInline("p4_backup_2923A0", 0x2923A0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&BackupDetour),
        reinterpret_cast<void**>(&g_origBackup)));
    count(InstallInline("p4_raidload_60EEE0", 0x60EEE0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&RaidLoadDetour),
        reinterpret_cast<void**>(&g_origRaidLoad)));
    count(InstallInline("p4_battleload_5AD160", 0x5AD160, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 }, reinterpret_cast<void*>(&BattleLoadDetour),
        reinterpret_cast<void**>(&g_origBattleLoad)));
    count(InstallInline("p4_battlesave_5AC440", 0x5AC440, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&BattleSaveDetour),
        reinterpret_cast<void**>(&g_origBattleSave)));
    count(InstallInline("p4_monsterload_600230", 0x600230, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57 }, reinterpret_cast<void*>(&MonsterLoadDetour),
        reinterpret_cast<void**>(&g_origMonsterLoad)));
    count(InstallInline("p4_instload_4A9FA0", 0x4A9FA0, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57 }, reinterpret_cast<void*>(&InstLoadDetour),
        reinterpret_cast<void**>(&g_origInstLoad)));
    count(InstallInline("p4_skill_5B9B50", 0x5B9B50, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&SkillDetour),
        reinterpret_cast<void**>(&g_origSkill)));
    count(InstallInline("p4_bufffile_4AA770", 0x4AA770, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18 }, reinterpret_cast<void*>(&BuffFileDetour),
        reinterpret_cast<void**>(&g_origBuffFile)));
    count(InstallInline("p4_effectfile_4F02D0", 0x4F02D0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&EffectFileDetour),
        reinterpret_cast<void**>(&g_origEffectFile)));
    if (g_cfg.active && g_cfg.loadInject) {
        count(InstallInline("p4_libloadall_4AA580", 0x4AA580, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20 },
            reinterpret_cast<void*>(&LibLoadAllDetour), reinterpret_cast<void**>(&g_origLibLoadAll)));
    }
    if (g_cfg.exitHook) {
        count(InstallInline("p4_exit_3D1CC0", 0x3D1CC0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 }, reinterpret_cast<void*>(&ExitDetour),
            reinterpret_cast<void**>(&g_origExit)));
    }
    L("HOOKS", "installed={} total={}", ok, total);
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "probe_p4";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.hooks().service()) {
        return false;
    }
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_mainTid = GetCurrentThreadId();  // Load 在主线程执行
    const char* sha1 = g_dd.game()->exe_sha1();
    g_enabled = sha1 && std::strcmp(sha1, kExeSha1) == 0;
    if (!g_enabled) {
        g_dd.log().warn("exe SHA1 {} 不是基准版本，探针不工作，什么都不装", sha1 ? sha1 : "");
        return true;
    }
    g_base = reinterpret_cast<std::uintptr_t>(g_dd.game()->image_base());
    g_imageEnd = g_base + static_cast<std::uintptr_t>(g_dd.game()->image_size());
    g_gameRoot = g_dd.game()->game_root();
    for (std::size_t i = 0; i < std::size(kTestIds); ++i) {
        g_testHashes[i] = Hash(kTestIds[i]);
    }
    ReadConfig();
    OpenStateFile();
    L("START", "exe=基准版 base={:X} main_tid={} {} active={} load_inject={} exit_hook={} prev_writes={}", g_base, g_mainTid, g_cfgText,
        g_cfg.active ? 1 : 0, g_cfg.loadInject ? 1 : 0, g_cfg.exitHook ? 1 : 0, g_prevWrites.size());

    g_dd.lifecycle().subscribe(DD_PHASE_APP_READY, [](const DDPhaseEvent&) {
        L("PHASE", "name=AppReady {}", StateText());
        InstallHooks();
    });
    g_dd.lifecycle().subscribe(DD_PHASE_ASSETS_READY, [](const DDPhaseEvent&) { L("PHASE", "name=AssetsReady {}", StateText()); });
    g_dd.lifecycle().subscribe(DD_PHASE_TICK, &OnTick);
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_LOAD, &OnCampaignLoad);
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_UNLOAD, &OnCampaignUnload);
    if (g_dd.events()) {
        // 0B#1：走廊步数（原生 PartyStepEvent 的 listener，不让 DDCore 装语义 hook），对照「走廊上是不是每步都存档」
        g_dd.events().on("raid.step", [](DDEvent*) {
            const auto n = g_steps.fetch_add(1) + 1;
            if (n <= 40) {
                L("STEP", "n={} {}", n, StateText());
            }
        });
        static int battles = 0;
        g_dd.events().on("battle.start", [](DDEvent*) {
            ++battles;
            L("BATTLE", "phase=start n={} monsters={} {}", battles, BattleMonsters().size(), StateText());
        });
        g_dd.events().on("battle.end", [](DDEvent*) {
            L("BATTLE", "phase=end n={} {}", battles, StateText());
            Summary("battle_end");
        });
    }
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}

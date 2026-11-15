#pragma once
// persist 服务里不依赖游戏的部分（P4c1）：内容哈希、旁路文件的格式、一致性判断、后台写盘（合并、原子替换、保留上一代）、
// backup 目录的同步、DDCore 标签表的序列化。DDCore 和单元测试（tests/unit）用同一份源码。
// 设计见 docs/adr/0031-persist-sidecar-content-hash.md 和 docs/02-architecture.md 的「persist 服务」。

#include <dd/dd_api.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ddc::persist {

// FNV-1a 64，游戏存档文件和旁路数据的内容哈希（名册最大约 0.5 MB，逐字节也只要零点几毫秒）
std::uint64_t Fnv1a64(const void* data, std::size_t size);

// 插件名和 key：字母数字和 _ . -，1..63 字节，不能以点开头（和插件名、Lua Mod id 的规则一致），可以直接当文件名
bool ValidKey(std::string_view key);
// 设备名（steam_profile_N、profile_N）：只认字母数字和 _ -，1..63 字节，用作目录名
bool ValidDevice(std::string_view device);
// ASCII 不区分大小写的比较（NTFS 上只差大小写的名字是同一个文件）
bool SameIgnoreCase(std::string_view a, std::string_view b);
// 插件名能不能当旁路的子目录：ValidKey，另外不能以点结尾（Windows 建目录时去掉结尾的点，a. 和 a 是同一个目录），
// 不能是 backup（选槽同步的目录）、DDCore（DDCore 自己的标签表），不区分大小写。不行时 why 写原因
bool ValidPluginDir(std::string_view name, std::string* why);

inline constexpr const char* kRosterFile = "persist.roster.json";
inline constexpr const char* kRaidFile = "persist.raid.json";
inline constexpr std::uint32_t kFormat = 1;
inline constexpr const char* kExtension = ".ddsave";

// ---- 观察 hook 的参数 ----
// 2487E0 的第 6 个参数（大小）、248D60 的第 5 个参数（容量）和返回值（读到的字节数）都是 32 位。栈上传的参数调用方只写
// 低 32 位（238290 的 2382F7 mov dword ptr [rsp+0x28], eax），高 32 位是栈上残留的值，常常是一个返回地址的高半部分。
// detour 按 64 位声明只为原样转发给原函数，取值一律经这里截成 32 位（审核 B1）
inline std::uint32_t ArgU32(std::uint64_t slot)
{
    return static_cast<std::uint32_t>(slot);
}
inline constexpr std::uint32_t kMaxObserved = 64u * 1024 * 1024;
// 观察到的一次读写要不要记：文件名（路径的最后一段）以 persist. 开头，大小不超过 64 MB
bool ShouldObserve(std::string_view name, std::uint32_t size);

struct FileHash {
    std::string name;
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
};

// 旁路文件头
struct Header {
    std::uint32_t format = kFormat;
    std::string plugin;
    std::string key;
    std::string version;     // 插件给的版本文字
    std::string device;
    std::string account;
    std::uint64_t writtenMs = 0;  // Unix 毫秒
    bool inRaid = false;
    bool inBattle = false;
    std::vector<FileHash> files;  // 快照时游戏文件的哈希：名册必有，在地牢里加 raid
    const FileHash* Find(std::string_view name) const;
};

// 文本头 + 空行 + 数据。头一行一项（「键 值」），数据带长度和校验和，写到一半的文件读回时能认出来：
//   DDSAVE 1 / plugin / key / version / device / account / written / in_raid / in_battle / file <名字> <哈希> <大小>（可多行）/
//   payload <大小> <哈希>
std::string EncodeFile(const Header& header, std::string_view payload);
bool DecodeFile(std::string_view bytes, Header* header, std::string* payload, std::string* why);

// ---- 一致性 ----
// 这次读档观察到的游戏状态（CAMPAIGN_LOAD 时）
struct Observed {
    std::string device;
    bool rosterKnown = false;
    FileHash roster;   // 这次读档读到的名册
    int inRaid = -1;   // 存档在地牢里 1、城镇 0、不知道 -1
    // 地牢里的存档：CAMPAIGN_LOAD 之前读到的 raid（状态 1 的 3DBC20 在主线程读一次，和之后 load_raid 读的相同，审核 S1）。
    // 知道时两代都按名册和 raid 判断；不知道时只按名册，raid 留到探索存档读完再核对
    bool raidKnown = false;
    FileHash raid;
    // 文件头里应有的插件名和 key（区分大小写）。为空时不比较
    std::string plugin;
    std::string key;
    // 新战役：这次读到的名册就是 scripts/starting_save 的初始名册（3DB3B0 原样复制过来的，审核 S6）
    bool newCampaign = false;
};

// 旁路的一代（当前或 .prev）
struct Candidate {
    bool exists = false;
    bool decoded = false;
    std::string error;  // 读不出、解析失败的原因
    Header header;
    std::string payload;
};

// 一代旁路和这次读档是否一致：文件头的插件名和 key、设备、名册的哈希和大小、城镇/地牢，地牢里知道 raid 时再比 raid。
// 不一致时 why 写原因
bool Matches(const Header& header, const Observed& observed, std::string* why);
// raid 的核对（探索存档读完之后的二次确认）
bool RaidMatches(const Header& header, const FileHash& raid, std::string* why);

struct Decision {
    std::uint32_t status = DD_PERSIST_UNAVAILABLE;  // DDPersistStatus
    int which = -1;      // 0 当前一代，1 上一代，-1 都不用
    std::string reason;
    // 第一次写之前要把旁路里现有的两代移开（归档）的标记："mismatch"（不一致）、"oldcampaign"（新战役复用旧存档位），
    // 空串表示不用
    std::string archive;
};
// 当前一代一致是 LOADED；否则上一代一致是 PREV；都不一致（含损坏）是 MISMATCH（archive = "mismatch"），但新战役是
// EMPTY（archive = "oldcampaign"）；都不存在是 EMPTY；没观察到名册的读取是 UNAVAILABLE
Decision Decide(const Candidate& current, const Candidate& prev, const Observed& observed);

// 文件内容是不是这个哈希和大小（读不出、不存在都是 false）。新战役的判断用：名册和 starting_save 里的比
bool SameContent(const std::wstring& path, const FileHash& expect);

// ---- 文件 ----
std::wstring Widen(std::string_view text);
std::string Narrow(std::wstring_view text);
// 加上 \\?\ 前缀（绝对路径），不受 MAX_PATH 和保留名的影响
std::wstring LongPath(const std::wstring& path);
bool FileExists(const std::wstring& path);
// 逐级创建目录
bool EnsureDir(const std::wstring& dir);
// 读整个文件。不存在时返回 true、*exists = false
bool ReadWhole(const std::wstring& path, std::string* out, bool* exists, std::string* why, std::size_t limit = 64u * 1024 * 1024);
// 读一代：解析失败时 decoded = false、error 写原因
Candidate ReadCandidate(const std::wstring& path);

// 测试用的失败注入，stage 返回 true 表示这一步失败："create"（建临时文件）、"tmp_write"（临时文件写到一半）、"flush"、
// "replace"（当作 ReplaceFileW 失败，走退回的两次改名）、"move_prev"（退回时把原文件改名为 .prev）、"move_new"（退回时把临时
// 文件改名为正式文件）
using FailHook = std::function<bool(const char* stage)>;
// 原子写入：先写 path.tmp 并 FlushFileBuffers，再 ReplaceFileW（原来的文件成为 path.prev），ReplaceFileW 失败时改用两次
// MoveFileExW；path 不存在时直接改名。写之前清掉 path、.tmp、.prev 的只读属性（只读的 .prev 会让这个 key 永远写不进去）。
// 失败时删掉临时文件，原来的文件和 .prev 保持不动；只有退回路径的第二次改名失败时原文件已经成了 .prev（why 里写明）。
// 父目录不存在时逐级创建
bool WriteAtomic(const std::wstring& path, std::string_view bytes, std::string* why, const FailHook& fail = {});

// 归档（审核 S3、S6）：把 path 和 path.prev 改名为 path.<tag>-<stamp>、path.prev.<tag>-<stamp>（不再参与读档判断，也不会被之后的
// 写入轮换掉），同一 path、同一 tag 只留最近 keep 份（按 stamp 排序，stamp 用可排序的时间文字）。两个文件都不存在时返回 true、
// moved 为空。改名失败返回 false（已经改名的不回退），why 写原因
bool ArchiveGenerations(const std::wstring& path, std::string_view tag, std::string_view stamp, std::size_t keep, std::vector<std::wstring>* moved,
    std::string* why);

// 后台写盘线程：Submit 在任意线程调用（DDCore 只在主线程调用），同一路径还没写的被新的替换（合并，只写最新的）。
// 写失败（杀毒软件、索引器正打开着 .prev、.tmp 之类）按退避重试，重试用完才放弃（审核 S2）
class Writer
{
public:
    using LogFn = std::function<void(int level, const std::string& text)>;
    explicit Writer(LogFn log);
    ~Writer();  // 写完队列再停线程（这时不再等重试）
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void Submit(std::wstring path, std::string bytes);
    // 等队列写完（包括等着重试的），超时返回 false。返回 true 只说明队列空了，写失败放弃的看 Failures()
    bool Flush(std::uint32_t timeoutMs);
    // 重试用完、最后放弃了的路径（之后同一路径写成功就去掉）
    std::vector<std::wstring> Failures() const;
    // 失败后第 k 次重试前等多久（毫秒）；默认 200、500、1000、2000，即最多写 5 次，前后约 4 秒
    void SetRetryDelays(std::vector<std::uint32_t> delaysMs);
    // 测试用：暂停写盘（提交照常），恢复后继续；失败注入
    void SetPaused(bool paused);
    void SetFailHook(FailHook hook);

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t merged = 0;   // 被同一路径后来的提交替换、没有写盘的
        std::uint64_t written = 0;
        std::uint64_t failed = 0;   // 重试用完放弃的（这份数据没有写进去）
        std::uint64_t retries = 0;  // 失败后安排了重试（或被同一路径更新的提交接替）的次数
    };
    Stats GetStats() const;
    std::size_t Pending() const;

private:
    struct Job {
        std::string bytes;
        std::uint32_t attempts = 0;                       // 已经失败的次数
        std::chrono::steady_clock::time_point due{};      // 重试的时间，第一次写是提交时
    };
    void Run();

    LogFn log_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<std::wstring> order_;
    std::map<std::wstring, Job> jobs_;
    std::map<std::wstring, std::uint64_t> failures_;  // 放弃了的路径 → 连续放弃的次数
    std::vector<std::uint32_t> delays_{ 200, 500, 1000, 2000 };
    FailHook fail_;
    Stats stats_;
    bool busy_ = false;
    bool paused_ = false;
    bool stop_ = false;
    std::thread thread_;
};

// backup：把 src 下每个插件目录里的 *.ddsave、*.ddsave.prev 复制到 dst 下同名目录，dst 里 src 没有的这类文件删掉
// （src 的子目录 backup 本身跳过）。copied 是复制的文件数
bool MirrorDir(const std::wstring& src, const std::wstring& dst, std::size_t* copied, std::string* why);

// ---- DDCore 标签表的序列化 ----
// 一条标签：actor 的标识（英雄 hero id + 名册；怪物 guid）、组 guid、条目的 buff id（恢复时核对）、插件名、标签
struct TagLine {
    std::uint32_t kind = 0;
    std::uint32_t id = 0;
    std::uint32_t roster = 0;
    std::uint32_t guid = 0;
    std::string buffId;
    std::string plugin;
    std::string tag;
};
// 一行一条，制表符分隔，% 制表符 回车 换行按 %XX 转义
std::string EncodeTags(const std::vector<TagLine>& tags);
bool DecodeTags(std::string_view text, std::vector<TagLine>* out, std::string* why);

}  // namespace ddc::persist

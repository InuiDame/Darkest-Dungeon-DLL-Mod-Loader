#include "persist.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace ddc::persist {

namespace {

// 槽 5（当前 profile）的设备在 StorageManager+0x38，设备名在设备对象 +0x20（char[0x80]），见 signatures.inc 的 StorageManager
constexpr std::uintptr_t kSmSlot5 = 0x38;
constexpr std::uintptr_t kDeviceName = 0x20;
constexpr std::size_t kDeviceNameCap = 0x80;
constexpr std::uint32_t kFlushMs = 3000;
// 退到桌面时多等一会儿：写盘失败要重试（最多约 4 秒），机械硬盘上几个 Mod 各自 FlushFileBuffers 也要时间（审核 N7）
constexpr std::uint32_t kExitFlushMs = 10000;
constexpr std::uint32_t kVerifyWaitFrames = 1200;  // CAMPAIGN_LOAD 之后等 as_raid_start 加载的帧数上限
constexpr std::size_t kArchiveKeep = 2;            // 每个 key、每种归档留几份
constexpr std::uint32_t kSaveLogFirst = 3;         // 每次进存档前几次 campaign.save 照常写日志，之后只在有旁路要写时写

// 参数宽度（反汇编核对过，见 store.h 的 ArgU32）：2487E0 第 5 个缓冲 qword、第 6 个大小 dword、第 7 个不用、第 8 个 byte、
// 第 9 个 qword；248D60 第 5 个容量 dword、第 6 个 byte，返回 eax（读到的字节数）。detour 按 64 位声明，原样转发
using QueueWriteFn = void* (*)(void*, void*, const char*, const char*, const void*, std::uint64_t, std::uint64_t, std::uint64_t, void*);
using ReadFileFn = std::uint64_t (*)(void*, const char*, const char*, void*, std::uint64_t, std::uint64_t);
using Void4Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

Persist::Impl* g_impl = nullptr;
QueueWriteFn g_origWrite = nullptr;
ReadFileFn g_origRead = nullptr;
Void4Fn g_origBackup = nullptr;
Void4Fn g_origShutdown = nullptr;

// ---- SEH 下读游戏内存（函数里不能有要析构的对象）----

bool SafeString(const char* src, char* dst, std::size_t cap)
{
    __try {
        std::size_t i = 0;
        for (; i + 1 < cap && src[i]; ++i) {
            dst[i] = src[i];
        }
        dst[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = 0;
        return false;
    }
}

bool SafeHash(const void* data, std::size_t size, std::uint64_t* out)
{
    __try {
        *out = Fnv1a64(data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafePointer(const void* address, std::uintptr_t* out)
{
    __try {
        *out = *static_cast<const std::uintptr_t volatile*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 当前 profile 的设备名：[[SM]+0x38]+0x20。读不到时为空串
void ReadProfileDevice(void* const volatile* sm, char* out, std::size_t cap)
{
    out[0] = 0;
    std::uintptr_t manager = 0, device = 0;
    if (!sm || !SafePointer(const_cast<void* const*>(sm), &manager) || !manager ||
        !SafePointer(reinterpret_cast<const void*>(manager + kSmSlot5), &device) || !device) {
        return;
    }
    SafeString(reinterpret_cast<const char*>(device + kDeviceName), out, (std::min)(cap, kDeviceNameCap));
}

const char* Basename(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

bool IsBackupPath(const char* path)
{
    return std::strncmp(path, "backup/", 7) == 0 || std::strncmp(path, "backup\\", 7) == 0 || std::strstr(path, "/backup/") != nullptr;
}

std::uint64_t NowUnixMs()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (t.QuadPart - 116444736000000000ull) / 10000ull;
}

std::string HashText(const FileHash& file)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%016llX/%u", static_cast<unsigned long long>(file.hash), file.size);
    return text;
}

// 归档文件名里的时间（本地时间，可排序）：20261114-153000-123
std::string Stamp()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return text;
}

using SteamUserFn = void* (*)();
using SteamIdFn = std::uint64_t (*)(void*);

bool CallSteam(void* userFn, void* idFn, std::uint64_t* id)
{
    __try {
        void* user = reinterpret_cast<SteamUserFn>(userFn)();
        *id = user ? reinterpret_cast<SteamIdFn>(idFn)(user) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* QueueWriteDetour(void* sm, void* handle, const char* dev, const char* file, const void* buffer, std::uint64_t size, std::uint64_t p7,
    std::uint64_t p8, void* p9);
std::uint64_t ReadFileDetour(void* sm, const char* dev, const char* path, void* buffer, std::uint64_t size, std::uint64_t decrypt);
std::uint64_t BackupDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d);
std::uint64_t ShutdownDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d);

}  // namespace

// ============================================================================

struct Persist::Impl {
    PersistDeps deps;

    struct Declared {
        std::uint32_t owner = 0;
        DDPersistSaveFn onSave = nullptr;
        DDPersistVerifyFn onVerify = nullptr;
        void* user = nullptr;
    };
    std::vector<Declared> declared;  // 按声明顺序（= 插件加载顺序）
    bool appReady = false;
    bool hooksOk = false;
    std::size_t hookCount = 0;
    std::string hookError;
    events::Topic* topic = nullptr;
    std::unique_ptr<Writer> writer;

    // ---- 观察 hook 记录（任何线程，mutex 保护）----
    struct Rec {
        std::uint64_t hash = 0;
        std::uint32_t size = 0;
        bool fromBackup = false;
        std::uint32_t epoch = 0;
    };
    mutable std::mutex mutex;
    std::map<std::string, Rec> latest;  // 「设备|文件名」→ 最后一次写入或读取的内容
    std::map<std::string, Rec> reads;   // 「设备|文件名」→ 最后一次读取
    struct Pending {
        // 当前 profile 上写了任何 persist.*.json（审核 S4：城镇里升级建筑、城镇事件只写庄园、城镇文件，也要派发，
        // 那之后改的 Lua 状态才进旁路；一致性仍只比较名册和 raid）
        bool trigger = false;
        std::vector<std::string> files; // 当前 profile 设备上写的 persist.*.json
        bool offMain = false;
    } pending;
    // 读取的分代：CAMPAIGN_UNLOAD 和 CAMPAIGN_LOAD 各加一。CAMPAIGN_LOAD 时只认本代（上次退出存档以来）读到的名册，
    // raid 只认 CAMPAIGN_LOAD 之后读到的
    std::atomic<std::uint32_t> epoch{ 1 };
    // 观察到的存档读写次数（诊断）；前几次逐条写日志，主菜单自检用它确认 hook 生效
    std::atomic<std::uint64_t> observedReads{ 0 };
    std::atomic<std::uint64_t> observedWrites{ 0 };
    static constexpr std::uint64_t kLogFirst = 6;
    // 每次 CAMPAIGN_LOAD 之后当前 profile 上的第一次名册写入也写一行（带大小），场景里能直接看到写入 hook 生效（审核 B1）
    std::atomic<bool> logNextRoster{ false };

    // ---- 主线程 ----
    std::string account;  // 取到后缓存
    struct Context {
        bool active = false;   // 在存档里
        bool usable = false;   // 能判断一致性
        std::string reason;    // 不能判断的原因
        std::string device;
        std::string account;
        std::wstring baseDir;  // <根>\DDModLoader\save\<账号>\<设备>
        std::wstring dir;      // 这次读档用的目录（读 backup 时是 baseDir\backup）
        bool fromBackup = false;
        FileHash roster;
        int inRaid = -1;
        bool raidKnown = false;    // 地牢里的存档：CAMPAIGN_LOAD 之前已经读到 raid（3DBC20），读档时就按它判断
        FileHash raid;
        bool newCampaign = false;  // 名册就是 starting_save 的初始名册
        std::uint32_t loadEpoch = 0;
    } ctx;
    struct Loaded {
        std::uint32_t status = DD_PERSIST_UNAVAILABLE;
        std::string reason;
        std::string baseReason;  // 不含「raid 待核对」的说明（核对通过后用它）
        Header header;
        std::string payload;
        bool raidPending = false;
        std::string archive;     // 第一次写之前把原来的两代归档（"mismatch"、"oldcampaign"），空串不用
        bool archived = false;
        // 这次进存档不再写这个 key：交出的数据后来被 raid 核对判成不一致（审核 S1，免得下一次存档按新哈希写回、
        // 把不一致「洗白」），或者归档失败。原来的两代保留
        bool blocked = false;
        bool blockedLogged = false;
    };
    std::map<std::pair<std::uint32_t, std::string>, Loaded> loaded;
    bool verifyPending = false;
    bool sawRaidLoading = false;
    std::uint32_t verifyWait = 0;
    int saveDepth = 0;
    struct SaveItem {
        std::string data;
        std::string version;
    };
    std::map<std::pair<std::uint32_t, std::string>, SaveItem> saveItems;
    std::uint32_t saveIndex = 0;   // 本次进存档
    std::uint32_t savesTotal = 0;  // 本次运行
    std::uint32_t savesSkipped = 0;
    bool tagsSeen = false;         // 本次进存档读到过或写过 DDCore 的标签文件
    std::vector<combat::TagRecord> monsterTags;  // 地牢存档里的怪物标签，raid 核对通过后恢复
    // Info() 的字符串
    std::string infoAccount, infoDevice, infoDir;

    void Log(int level, const std::string& text) const
    {
        if (deps.log) {
            deps.log(level, "persist：" + text);
        }
    }

    bool IsMain() const { return GetCurrentThreadId() == deps.mainThread; }
    std::string PluginName(std::uint32_t owner) const { return deps.pluginName ? deps.pluginName(owner) : "?"; }

    const Declared* FindDeclared(std::uint32_t owner) const
    {
        for (const auto& d : declared) {
            if (d.owner == owner) {
                return &d;
            }
        }
        return nullptr;
    }

    std::string ProfileDevice() const
    {
        char name[kDeviceNameCap] = {};
        ReadProfileDevice(deps.addr->storageManager, name, sizeof(name));
        return name;
    }

    bool RaidActive() const
    {
        std::uintptr_t raid = 0;
        return deps.addr->raidCurrent && SafePointer(const_cast<void**>(deps.addr->raidCurrent), &raid) && raid != 0;
    }

    bool BattleActive() const
    {
        if (!deps.addr->raidCurrent) {
            return false;
        }
        const BattleSample s = game::ReadBattleSample(deps.addr->raidCurrent);
        return s.raid && (s.state != 0 || s.monsters > 0);
    }

    // 账号：Steam 版是 SteamID32（steam_api64 的 SteamAPI_SteamUser_vNNN + SteamAPI_ISteamUser_GetSteamID，P4-0C 实测），
    // 离线版、没有 steam_api64.dll、取不到 SteamID（Steam 没运行、离线、导出缺失）时是 local。只在选存档槽、进存档、存档时调用，
    // 这时 Steam 早已初始化完（386660 在启动时），取不到就是取不到，按 local 缓存，WARN 只写一次（审核 N4：原来一直返回空串，
    // 每次存档一行 WARN、旁路从不写）
    const std::string& Account()
    {
        if (!account.empty()) {
            return account;
        }
        HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
        if (deps.buildLabel == "nosteam" || !steam) {
            account = "local";
            return account;
        }
        void* idFn = reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID"));
        void* userFn = nullptr;
        for (int v = 40; v >= 15 && !userFn; --v) {
            char name[48];
            std::snprintf(name, sizeof(name), "SteamAPI_SteamUser_v%03d", v);
            userFn = reinterpret_cast<void*>(GetProcAddress(steam, name));
        }
        std::uint64_t id = 0;
        if (!userFn || !idFn) {
            Log(DD_LOG_WARN, "steam_api64.dll 里没有 SteamAPI_SteamUser_vNNN / SteamAPI_ISteamUser_GetSteamID，账号按 local");
            account = "local";
        } else if (CallSteam(userFn, idFn, &id) && (id & 0xFFFFFFFFull) != 0) {
            account = std::to_string(static_cast<std::uint32_t>(id & 0xFFFFFFFFull));
        } else {
            Log(DD_LOG_WARN, "取不到 SteamID（Steam 没在运行或离线？），账号按 local");
            account = "local";
        }
        return account;
    }

    std::wstring DirFor(const std::string& acc, const std::string& device) const
    {
        return deps.gameRoot + L"\\DDModLoader\\save\\" + Widen(acc) + L"\\" + Widen(device);
    }

    std::wstring FileFor(const std::wstring& dir, std::uint32_t owner, const std::string& key) const
    {
        return dir + L"\\" + Widen(PluginName(owner)) + L"\\" + Widen(key) + Widen(kExtension);
    }

    static std::string RecKey(const std::string& device, const std::string& name) { return device + "|" + name; }

    // ---- 观察 hook（任何线程）----
    // size 是 2487E0 的第 6 个参数截成的 32 位（ArgU32）
    void ObserveWrite(const char* dev, const char* file, const void* buffer, std::uint32_t size)
    {
        char device[kDeviceNameCap], path[0x110];
        if (!SafeString(dev, device, sizeof(device)) || !SafeString(file, path, sizeof(path))) {
            return;
        }
        const char* name = Basename(path);
        if (!ShouldObserve(name, size)) {
            return;
        }
        std::uint64_t hash = 0;
        if (!SafeHash(buffer, size, &hash)) {
            return;
        }
        const std::string profile = ProfileDevice();
        const bool onProfile = !profile.empty() && profile == device;
        {
            std::lock_guard lock(mutex);
            latest[RecKey(device, name)] = Rec{ hash, size, false, epoch.load() };
            if (onProfile) {
                if (std::find(pending.files.begin(), pending.files.end(), name) == pending.files.end()) {
                    pending.files.emplace_back(name);
                }
                pending.trigger = true;
                pending.offMain = pending.offMain || !IsMain();
            }
        }
        const bool firstRoster = onProfile && std::strcmp(name, kRosterFile) == 0 && logNextRoster.exchange(false);
        if (observedWrites.fetch_add(1) < kLogFirst) {
            Log(DD_LOG_INFO, ObserveText("写", device, path, hash, size, "前 6 次逐条记录"));
        } else if (firstRoster) {
            Log(DD_LOG_INFO, ObserveText("写", device, path, hash, size, "进存档后第一次写名册"));
        }
    }

    std::string ObserveText(const char* what, const char* device, const char* path, std::uint64_t hash, std::uint32_t size, const char* why) const
    {
        char text[512];
        std::snprintf(text, sizeof(text), "观察到%s存档：设备 %s，%s，%u 字节，哈希 %016llX，%s线程（%s）", what, device, path, size,
            static_cast<unsigned long long>(hash), IsMain() ? "主" : "其他", why);
        return text;
    }

    // got 是 248D60 读到的字节数，已经截成 32 位、不超过容量
    void ObserveRead(const char* dev, const char* file, const void* buffer, std::uint32_t got)
    {
        char device[kDeviceNameCap], path[0x110];
        if (!SafeString(dev, device, sizeof(device)) || !SafeString(file, path, sizeof(path))) {
            return;
        }
        const char* name = Basename(path);
        if (!ShouldObserve(name, got)) {
            return;
        }
        std::uint64_t hash = 0;
        if (!SafeHash(buffer, got, &hash)) {
            return;
        }
        const Rec rec{ hash, got, IsBackupPath(path), epoch.load() };
        {
            std::lock_guard lock(mutex);
            latest[RecKey(device, name)] = rec;
            reads[RecKey(device, name)] = rec;
        }
        if (observedReads.fetch_add(1) < kLogFirst) {
            Log(DD_LOG_INFO, ObserveText("读", device, path, hash, got, "前 6 次逐条记录"));
        }
    }

    bool FindRec(const std::map<std::string, Rec>& map, const std::string& device, const char* name, Rec* out, std::uint32_t needEpoch = 0) const
    {
        std::lock_guard lock(mutex);
        const auto it = map.find(RecKey(device, name));
        if (it == map.end() || (needEpoch && it->second.epoch != needEpoch)) {
            return false;
        }
        *out = it->second;
        return true;
    }

    // ---- campaign.save ----
    void DispatchSave(const char* when)
    {
        Pending p;
        {
            std::lock_guard lock(mutex);
            p = std::move(pending);
            pending = Pending{};
        }
        if (!p.trigger) {
            return;
        }
        ++saveIndex;
        ++savesTotal;
        const std::string device = ProfileDevice();
        const bool inRaid = RaidActive();
        const bool inBattle = BattleActive();
        std::string files;
        for (const auto& f : p.files) {
            files += (files.empty() ? "" : ",") + f;
        }
        DDPersistSaveInfo info{};
        info.size = sizeof(info);
        info.files = files.c_str();
        info.in_raid = inRaid;
        info.in_battle = inBattle;
        info.main_thread = !p.offMain;
        info.campaign_serial = deps.campaignSerial ? deps.campaignSerial() : 0;
        info.index = saveIndex;

        // 快照时的游戏文件哈希（这组写完之后、Lua 状态变化之前）
        Header base;
        base.device = device;
        base.inRaid = inRaid;
        base.inBattle = inBattle;
        Rec roster, raid;
        const bool haveRoster = FindRec(latest, device, kRosterFile, &roster);
        const bool haveRaid = FindRec(latest, device, kRaidFile, &raid);
        if (haveRoster) {
            base.files.push_back({ kRosterFile, roster.hash, roster.size });
        }
        if (inRaid && haveRaid) {
            base.files.push_back({ kRaidFile, raid.hash, raid.size });
        }

        ++saveDepth;
        if (topic && deps.events && deps.events->hub().SubscriberCount(topic) > 0) {
            events::Slot slots[6]{};
            slots[0].v.s = files.c_str();
            slots[1].v.b = inRaid;
            slots[2].v.b = inBattle;
            slots[3].v.b = !p.offMain;
            slots[4].v.i = info.campaign_serial;
            slots[5].v.i = saveIndex;
            for (auto& slot : slots) {
                slot.set = true;
            }
            events::Payload payload;
            payload.slots = slots;
            payload.originThread = GetCurrentThreadId();
            deps.events->hub().Dispatch(topic, payload);
        }
        for (const auto& d : declared) {
            if (deps.invokeSave && !deps.invokeSave(d.onSave, &info, d.user, d.owner)) {
                Log(DD_LOG_ERROR, "插件 " + PluginName(d.owner) + " 的保存回调崩溃，这次没有交出的数据不写");
            }
        }
        SaveCoreTags();
        --saveDepth;

        std::string why;
        const std::string& acc = Account();
        if (!haveRoster) {
            why = "没有观察到名册的写入或读取";
        } else if (inRaid && !haveRaid) {
            why = "在地牢里，但没有观察到 raid 的写入或读取";
        } else if (acc.empty()) {
            why = "还没取到 Steam 账号";
        } else if (!ValidDevice(device)) {
            why = "设备名不合法：" + device;
        }
        std::size_t written = 0, held = 0;
        if (why.empty() && writer) {
            const std::wstring dir = DirFor(acc, device);
            base.account = acc;
            base.writtenMs = NowUnixMs();
            // 先逐个准备（判断、归档），再提交：准备时读旁路要先等后台写完，提交到一半再等就白等了
            std::vector<std::pair<std::pair<std::uint32_t, std::string>, SaveItem*>> ready;
            for (auto& [key, item] : saveItems) {
                if (PrepareWrite(key.first, key.second)) {
                    ready.emplace_back(key, &item);
                } else {
                    ++held;
                }
            }
            for (auto& [key, item] : ready) {
                Header h = base;
                h.plugin = PluginName(key.first);
                h.key = key.second;
                h.version = item->version;
                writer->Submit(FileFor(dir, key.first, key.second), EncodeFile(h, item->data));
                ++written;
            }
        } else if (!saveItems.empty()) {
            ++savesSkipped;
            Log(DD_LOG_WARN, "campaign.save #" + std::to_string(saveIndex) + " 的旁路不写：" + (why.empty() ? "后台写盘不可用" : why));
        }
        const bool hadItems = !saveItems.empty();
        saveItems.clear();
        // 没有 Lua Mod 的玩家每次存档也会派发（DDLua 总是声明），只有前几次和有旁路要写时写日志（审核 N8）
        if (written || held || hadItems || saveIndex <= kSaveLogFirst) {
            Log(DD_LOG_INFO, "campaign.save #" + std::to_string(saveIndex) + "（" + when + "）：文件 " + files + "；地牢 " + (inRaid ? "1" : "0") +
                "，战斗 " + (inBattle ? "1" : "0") + "，" + (p.offMain ? "有加载线程上的写入" : "都在主线程") + "；名册 " +
                (haveRoster ? HashText(base.files[0]) : "?") + (inRaid ? "，raid " + (haveRaid ? HashText({ kRaidFile, raid.hash, raid.size }) : "?") : "") +
                "；提交旁路文件 " + std::to_string(written) + " 个" + (held ? "，不写 " + std::to_string(held) + " 个（见上）" : ""));
        }
    }

    // 写之前：这次进存档第一次写某个 key 时先判断一次（插件没 load 过也判断，结果和 load 一样缓存）。判成不一致或新战役的，
    // 先把旁路里原来的两代移开（审核 S3：mismatch 不一定是 Mod 数据真的作废，云同步来的存档、旁路落后一次都会这样，按原来的
    // 轮换两次存档后就找不回了）；raid 核对不一致的不写。返回 false 表示这次不写这个 key
    bool PrepareWrite(std::uint32_t owner, const std::string& key)
    {
        if (!ctx.usable) {
            return true;  // 判断不了一致性（没观察到名册读取等）：照常写
        }
        Loaded& l = Evaluate(owner, key);
        const std::string label = PluginName(owner) + "/" + key;
        if (l.blocked) {
            if (!l.blockedLogged) {
                l.blockedLogged = true;
                Log(DD_LOG_INFO, label + "：这次进存档不写旁路（" + l.reason + "），原来的两代保留");
            }
            return false;
        }
        if (l.archive.empty() || l.archived) {
            return true;
        }
        l.archived = true;
        std::vector<std::wstring> moved;
        std::string why;
        if (!ArchiveGenerations(FileFor(ctx.baseDir, owner, key), l.archive, Stamp(), kArchiveKeep, &moved, &why)) {
            l.blocked = true;
            l.blockedLogged = true;
            Log(DD_LOG_WARN, label + "：第一次写之前归档原来的旁路失败（" + why + "），这次进存档不写它，原文件不动");
            return false;
        }
        if (!moved.empty()) {
            std::string names;
            for (const auto& path : moved) {
                const std::size_t slash = path.find_last_of(L'\\');
                names += (names.empty() ? "" : "、") + Narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
            }
            Log(DD_LOG_INFO, label + "：这次读档" + (l.archive == "oldcampaign" ? "是新战役" : "不一致") + "，第一次写之前把原来的旁路改名为 " + names +
                "（不再参与读档判断，要找回时把名字改回去；每种只留最近 " + std::to_string(kArchiveKeep) + " 份）");
        }
        return true;
    }

    void SaveCoreTags()
    {
        if (!deps.combat) {
            return;
        }
        const auto records = deps.combat->ExportTags();
        if (records.empty() && !tagsSeen) {
            return;
        }
        std::vector<TagLine> lines;
        for (const auto& r : records) {
            lines.push_back({ r.kind, r.id, r.roster, r.guid, r.buffId, r.plugin, r.tag });
        }
        saveItems[{ hook::kCoreOwner, "tags" }] = SaveItem{ EncodeTags(lines), "1" };
        tagsSeen = true;
    }

    // ---- 读档 ----
    Loaded& Evaluate(std::uint32_t owner, const std::string& key)
    {
        const auto mapKey = std::make_pair(owner, key);
        if (const auto it = loaded.find(mapKey); it != loaded.end()) {
            return it->second;
        }
        Loaded& l = loaded[mapKey];
        const std::string label = PluginName(owner) + "/" + key;
        if (!ctx.usable) {
            l.status = DD_PERSIST_UNAVAILABLE;
            l.reason = ctx.reason;
            Log(DD_LOG_WARN, label + "：" + StatusText(l.status) + "（" + l.reason + "）");
            return l;
        }
        if (writer) {
            writer->Flush(kFlushMs);
        }
        const std::wstring path = FileFor(ctx.dir, owner, key);
        const Candidate current = ReadCandidate(path);
        const Candidate prev = ReadCandidate(path + L".prev");
        Observed observed;
        observed.device = ctx.device;
        observed.rosterKnown = true;
        observed.roster = ctx.roster;
        observed.inRaid = ctx.inRaid;
        observed.raidKnown = ctx.raidKnown;
        observed.raid = ctx.raid;
        observed.plugin = PluginName(owner);
        observed.key = key;
        observed.newCampaign = ctx.newCampaign;
        const Decision d = Decide(current, prev, observed);
        l.status = d.status;
        l.reason = d.reason;
        l.baseReason = d.reason;
        l.archive = d.archive;
        if (d.which >= 0) {
            const Candidate& c = d.which == 0 ? current : prev;
            l.header = c.header;
            l.payload = c.payload;
            if (c.header.inRaid) {
                // 怪物的状态和标签要等战斗从探索存档恢复（as_raid_start 加载结束）才能核对、恢复，所以照样挂着 raid_pending
                l.raidPending = true;
                l.reason = (l.reason.empty() ? std::string() : l.reason + "；") +
                    (ctx.raidKnown ? "地牢里的存档，raid 已按读档前读到的探索存档核对，探索存档读完后再确认一次" : "地牢里的存档，raid 等探索存档读完再核对");
            }
        }
        if (owner == hook::kCoreOwner && (current.exists || prev.exists)) {
            tagsSeen = true;
        }
        const bool quiet = owner == hook::kCoreOwner && l.status == DD_PERSIST_EMPTY;
        if (!quiet) {
            std::string text = label + "：" + StatusText(l.status);
            if (d.which >= 0) {
                text += "（" + std::string(d.which == 0 ? "当前一代" : "上一代") + "，版本 " + l.header.version + "，写于 " + std::to_string(l.header.writtenMs) +
                    "，数据 " + std::to_string(l.payload.size()) + " 字节" + (ctx.fromBackup ? "，backup" : "") + "）";
            }
            if (!l.reason.empty()) {
                text += "：" + l.reason;
            }
            Log(l.status == DD_PERSIST_MISMATCH ? DD_LOG_WARN : DD_LOG_INFO, text);
        }
        return l;
    }

    void LoadCoreTags()
    {
        if (!deps.combat) {
            return;
        }
        Loaded& l = Evaluate(hook::kCoreOwner, "tags");
        if (l.status != DD_PERSIST_LOADED && l.status != DD_PERSIST_PREV) {
            return;
        }
        std::vector<TagLine> lines;
        std::string why;
        if (!DecodeTags(l.payload, &lines, &why)) {
            Log(DD_LOG_WARN, "标签表解析失败，不恢复：" + why);
            return;
        }
        std::vector<combat::TagRecord> heroes;
        monsterTags.clear();
        for (const auto& t : lines) {
            combat::TagRecord r{ t.kind, t.id, t.roster, t.guid, t.buffId, t.plugin, t.tag };
            (t.kind == DD_ACTOR_HERO ? heroes : monsterTags).push_back(std::move(r));
        }
        ImportTags(heroes, 1u << DD_ACTOR_HERO, "英雄");
        if (!l.raidPending && !monsterTags.empty()) {
            Log(DD_LOG_INFO, "标签表里有 " + std::to_string(monsterTags.size()) + " 条怪物的记录，但存档不在地牢里，丢弃");
            monsterTags.clear();
        }
    }

    void ImportTags(const std::vector<combat::TagRecord>& records, std::uint32_t kinds, const char* what)
    {
        if (records.empty()) {
            return;
        }
        std::vector<std::string> notes;
        const std::size_t restored = deps.combat->ImportTags(records, kinds, deps.ownerOf, &notes);
        Log(DD_LOG_INFO, std::string("标签表恢复") + what + "的 " + std::to_string(restored) + " 条，丢弃 " + std::to_string(notes.size()) + " 条");
        for (const auto& note : notes) {
            Log(DD_LOG_INFO, "  丢弃：" + note);
        }
    }

    void VerifyRaid(const char* failure)
    {
        verifyPending = false;
        Rec raid;
        const bool have = FindRec(reads, ctx.device, kRaidFile, &raid, ctx.loadEpoch);
        const FileHash raidHash{ kRaidFile, raid.hash, raid.size };
        bool coreOk = false;
        for (auto& [key, l] : loaded) {
            if (!l.raidPending) {
                continue;
            }
            l.raidPending = false;
            std::string why;
            bool ok = false;
            if (failure) {
                why = failure;
            } else if (!have) {
                why = "CAMPAIGN_LOAD 之后没有读到探索存档 persist.raid.json（游戏可能当作新探索）";
            } else {
                ok = RaidMatches(l.header, raidHash, &why);
            }
            const std::string label = PluginName(key.first) + "/" + key.second;
            if (ok) {
                l.reason = l.baseReason;
                Log(DD_LOG_INFO, label + "：raid 核对一致（" + HashText(raidHash) + "）");
            } else {
                // 数据已经交出去了（main.lua 之前），收不回来。这次进存档不再写这个 key：否则下一次 campaign.save 会把从不一致的
                // 一代来的数据连同新的哈希写进旁路，下次读档就成了 loaded（审核 S1）。原来的两代保留，下次读档照样判不一致
                l.status = DD_PERSIST_MISMATCH;
                l.reason = why;
                l.blocked = true;
                l.blockedLogged = true;
                Log(DD_LOG_WARN, label + "：raid 核对不一致：" + why + "；这次进存档不再写它的旁路，原来的两代保留");
            }
            if (key.first == hook::kCoreOwner) {
                coreOk = ok;
                continue;
            }
            const Declared* d = FindDeclared(key.first);
            if (d && d->onVerify && deps.invokeVerify) {
                DDPersistVerify v{};
                v.size = sizeof(v);
                v.key = key.second.c_str();
                v.status = l.status;
                v.reason = l.reason.c_str();
                if (!deps.invokeVerify(d->onVerify, &v, d->user, d->owner)) {
                    Log(DD_LOG_ERROR, "插件 " + PluginName(d->owner) + " 的核对回调崩溃");
                }
            }
        }
        if (coreOk) {
            ImportTags(monsterTags, 1u << DD_ACTOR_MONSTER, "怪物");
        } else if (!monsterTags.empty()) {
            Log(DD_LOG_INFO, "raid 核对没通过，丢弃标签表里 " + std::to_string(monsterTags.size()) + " 条怪物的记录");
        }
        monsterTags.clear();
    }

    // 等后台写完并如实报告（审核 S2）：「已写完」只在队列空了而且没有放弃的文件时写；没写完、有放弃的写 WARN 并列出路径
    void ReportFlush(const char* when, std::uint32_t timeoutMs, bool quietWhenClean = false)
    {
        if (!writer) {
            return;
        }
        const bool drained = writer->Flush(timeoutMs);
        const auto stats = writer->GetStats();
        const auto failures = writer->Failures();
        if (quietWhenClean && drained && failures.empty()) {
            return;
        }
        std::string text = std::string(when) + "：后台写盘";
        if (drained && failures.empty()) {
            text += "已写完";
        } else if (!drained) {
            text += "没有在 " + std::to_string(timeoutMs / 1000) + " 秒内写完（还有 " + std::to_string(writer->Pending()) + " 个）";
        } else {
            text += "写完了，但有 " + std::to_string(failures.size()) + " 个文件最后一次没写进去";
        }
        text += "（本次运行写盘 " + std::to_string(stats.written) + " 个，合并 " + std::to_string(stats.merged) + " 次，失败 " + std::to_string(stats.failed) +
            " 次，重试 " + std::to_string(stats.retries) + " 次）";
        for (std::size_t i = 0; i < failures.size() && i < 8; ++i) {
            text += (i ? "、" : "：") + Narrow(failures[i]);
        }
        Log(drained && failures.empty() ? DD_LOG_INFO : DD_LOG_WARN, text);
    }

    // ---- 安装 ----
    bool InstallOne(const char* name, const char* entryId, std::uint8_t* target, void* detour, void** orig, std::uint64_t* handle)
    {
        std::vector<std::uint8_t> expected;
        if (!target || !game::HookEntryBytes(entryId, expected)) {
            hookError = std::string("特征码 ") + entryId + " 缺失";
            return false;
        }
        hook::InlineRequest request{};
        request.name = name;
        request.target = target;
        request.expected = expected.data();
        request.expectedLength = static_cast<std::uint32_t>(expected.size());
        request.priority = 0;
        request.detour = detour;
        request.original = orig;
        request.chainForeignJump = true;
        const auto result = deps.hooks->InstallInline(hook::kCoreOwner, request);
        if (result.result != hook::Result::Ok) {
            hookError = std::string(name) + "：" + hook::ResultText(result.result) + "（" + result.detail + "）";
            return false;
        }
        *handle = result.handle;
        return true;
    }

    void RegisterTopic()
    {
        if (!deps.events) {
            return;
        }
        topic = deps.events->hub().AddTopic("campaign.save", events::TopicKind::App,
            { events::FieldDesc{ "files", DD_FIELD_STRING, events::kAccessR }, events::FieldDesc{ "in_raid", DD_FIELD_BOOL, events::kAccessR },
                events::FieldDesc{ "in_battle", DD_FIELD_BOOL, events::kAccessR }, events::FieldDesc{ "main_thread", DD_FIELD_BOOL, events::kAccessR },
                events::FieldDesc{ "serial", DD_FIELD_INT, events::kAccessR }, events::FieldDesc{ "index", DD_FIELD_INT, events::kAccessR } },
            false, false);
        if (topic) {
            deps.events->hub().SetAvailability(topic, false, "没有插件声明使用持久化（persist.declare），观察 hook 没有安装");
        }
    }
};

namespace {

// size 只有低 32 位是调用方写的（2382F7 mov dword ptr [rsp+0x28], eax），高 32 位是栈上残留的返回地址的高半部分；
// 原来按 64 位和 64 MB 比较，每次写入都被当成太大而跳过（审核 B1）
void* QueueWriteDetour(void* sm, void* handle, const char* dev, const char* file, const void* buffer, std::uint64_t size, std::uint64_t p7,
    std::uint64_t p8, void* p9)
{
    if (auto* impl = g_impl) {
        try {
            impl->ObserveWrite(dev, file, buffer, ArgU32(size));
        } catch (...) {
        }
    }
    return g_origWrite(sm, handle, dev, file, buffer, size, p7, p8, p9);
}

// size（容量）是第 5 个参数，248D86 mov r13d, dword ptr [rsp+0x170] 只读低 32 位；返回值是 eax
std::uint64_t ReadFileDetour(void* sm, const char* dev, const char* path, void* buffer, std::uint64_t size, std::uint64_t decrypt)
{
    const std::uint64_t result = g_origRead(sm, dev, path, buffer, size, decrypt);
    if (auto* impl = g_impl) {
        try {
            impl->ObserveRead(dev, path, buffer, (std::min)(ArgU32(result), ArgU32(size)));
        } catch (...) {
        }
    }
    return result;
}

std::uint64_t BackupDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    const std::uint64_t result = g_origBackup(a, b, c, d);
    if (auto* impl = g_impl) {
        try {
            if (impl->IsMain()) {
                // 游戏刚把根目录复制到 backup/：旁路同步到 <设备>\backup\（先等后台写完）
                if (impl->writer) {
                    impl->writer->Flush(kFlushMs);
                }
                const std::string device = impl->ProfileDevice();
                const std::string& acc = impl->Account();
                if (!ValidDevice(device) || acc.empty()) {
                    impl->Log(DD_LOG_INFO, "选存档槽：没取到设备名或账号（设备 " + device + "），旁路不同步 backup");
                } else {
                    const std::wstring dir = impl->DirFor(acc, device);
                    std::size_t copied = 0;
                    std::string why;
                    const bool ok = MirrorDir(dir, dir + L"\\backup", &copied, &why);
                    impl->Log(ok ? DD_LOG_INFO : DD_LOG_WARN, "选存档槽（" + device + "）：游戏复制了 backup/，旁路同步 " + std::to_string(copied) +
                        " 个文件到 " + Narrow(dir) + "\\backup" + (ok ? "" : "，有失败：" + why));
                }
            }
        } catch (...) {
        }
    }
    return result;
}

std::uint64_t ShutdownDetour(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d)
{
    if (auto* impl = g_impl) {
        try {
            if (impl->IsMain()) {
                bool trigger = false;
                {
                    std::lock_guard lock(impl->mutex);
                    trigger = impl->pending.trigger;
                }
                // 加载中（出征的加载画面里关窗口）不派发：load_raid 线程还在动游戏对象，快照也不是一个确定的时刻。
                // 旁路因此落后一次，下次读档判不一致，这是对的（审核 S7，和帧边界的条件一致）
                const bool loading = impl->deps.loadingNow && impl->deps.loadingNow();
                if (trigger && !loading && impl->deps.campaignActive && impl->deps.campaignActive()) {
                    impl->DispatchSave("退到桌面前");
                } else if (trigger && loading) {
                    impl->Log(DD_LOG_INFO, "退到桌面：加载中，最后一组存档不派发 campaign.save（旁路落后一次，下次读档会判不一致）");
                }
                impl->ReportFlush("退到桌面", kExitFlushMs);
            }
        } catch (...) {
        }
    }
    return g_origShutdown(a, b, c, d);
}

}  // namespace

// ============================================================================

Persist::Persist(PersistDeps deps) : impl_(std::make_unique<Impl>())
{
    impl_->deps = std::move(deps);
    g_impl = impl_.get();
    impl_->RegisterTopic();
}

Persist::~Persist()
{
    if (g_impl == impl_.get()) {
        g_impl = nullptr;
    }
}

const char* Persist::StatusText(std::uint32_t status)
{
    switch (status) {
    case DD_PERSIST_LOADED:
        return "loaded";
    case DD_PERSIST_PREV:
        return "prev";
    case DD_PERSIST_EMPTY:
        return "empty";
    case DD_PERSIST_MISMATCH:
        return "mismatch";
    case DD_PERSIST_UNAVAILABLE:
        return "unavailable";
    default:
        return "unknown";
    }
}

std::uint32_t Persist::Declare(std::uint32_t owner, DDPersistSaveFn onSave, DDPersistVerifyFn onVerify, void* user)
{
    auto& m = *impl_;
    if (!m.IsMain()) {
        return DD_PERSIST_E_THREAD;
    }
    if (!onSave) {
        return DD_PERSIST_E_INVALID;
    }
    if (m.appReady) {
        m.Log(DD_LOG_WARN, "插件 " + m.PluginName(owner) + " 在 AppReady 之后才声明，观察 hook 已经决定过，拒绝");
        return DD_PERSIST_E_TIMING;
    }
    // 插件名就是旁路的子目录名（审核 N3、S5）：.、..、以点结尾的建目录失败或和别的名字撞目录，backup、DDCore 和保留目录撞名，
    // 只差大小写的两个插件在 NTFS 上是同一个目录
    const std::string name = m.PluginName(owner);
    std::string why;
    if (!ValidPluginDir(name, &why)) {
        m.Log(DD_LOG_WARN, "插件 " + name + " 的名字不能当旁路目录（" + why + "），拒绝声明");
        return DD_PERSIST_E_INVALID;
    }
    for (auto& d : m.declared) {
        if (d.owner == owner) {
            d.onSave = onSave;
            d.onVerify = onVerify;
            d.user = user;
            return DD_PERSIST_OK;
        }
        if (SameIgnoreCase(m.PluginName(d.owner), name)) {
            m.Log(DD_LOG_WARN, "插件 " + name + " 和已经声明的 " + m.PluginName(d.owner) + " 只差大小写，旁路目录会是同一个，拒绝声明");
            return DD_PERSIST_E_INVALID;
        }
    }
    m.declared.push_back({ owner, onSave, onVerify, user });
    if (m.topic && m.deps.events) {
        m.deps.events->hub().SetAvailability(m.topic, true, "");  // 声明之后就能订阅 campaign.save（AppReady 装 hook 失败时再改回）
    }
    m.Log(DD_LOG_INFO, "插件 " + m.PluginName(owner) + " 声明使用存档旁路");
    return DD_PERSIST_OK;
}

std::uint32_t Persist::Load(std::uint32_t owner, const std::string& key, DDPersistData* out)
{
    auto& m = *impl_;
    if (!m.IsMain()) {
        return DD_PERSIST_E_THREAD;
    }
    if (!m.FindDeclared(owner)) {
        return DD_PERSIST_E_NOT_DECLARED;
    }
    if (!ValidKey(key) || !out || out->size < sizeof(DDPersistData)) {
        return DD_PERSIST_E_INVALID;
    }
    if (!m.hooksOk) {
        return DD_PERSIST_E_UNAVAILABLE;
    }
    if (!m.ctx.active) {
        return DD_PERSIST_E_NO_CAMPAIGN;
    }
    const Impl::Loaded& l = m.Evaluate(owner, key);
    const bool data = l.status == DD_PERSIST_LOADED || l.status == DD_PERSIST_PREV;
    out->status = l.status;
    out->reason = l.reason.c_str();
    out->data = data ? l.payload.data() : nullptr;
    out->data_size = data ? static_cast<std::uint32_t>(l.payload.size()) : 0;
    out->version = data ? l.header.version.c_str() : "";
    out->written_ms = data ? l.header.writtenMs : 0;
    out->in_raid = data && l.header.inRaid;
    out->raid_pending = data && l.raidPending;
    out->from_backup = m.ctx.fromBackup;
    return DD_PERSIST_OK;
}

std::uint32_t Persist::Save(std::uint32_t owner, const std::string& key, std::string data, std::string version)
{
    auto& m = *impl_;
    if (!m.IsMain()) {
        return DD_PERSIST_E_THREAD;
    }
    if (!m.FindDeclared(owner)) {
        return DD_PERSIST_E_NOT_DECLARED;
    }
    if (!ValidKey(key) || data.size() > DD_PERSIST_MAX_DATA || version.size() > 63) {
        return DD_PERSIST_E_INVALID;
    }
    if (m.saveDepth <= 0) {
        return DD_PERSIST_E_TIMING;
    }
    // 只差大小写的两个 key 在 NTFS 上是同一个文件（审核 S5）
    for (const auto& [k, item] : m.saveItems) {
        if (k.first == owner && k.second != key && SameIgnoreCase(k.second, key)) {
            m.Log(DD_LOG_WARN, "插件 " + m.PluginName(owner) + " 这次同时交了 " + k.second + " 和 " + key + "，只差大小写（是同一个文件），后一个拒绝");
            return DD_PERSIST_E_INVALID;
        }
    }
    // raid 核对不一致的 key 这次进存档不再写（见 VerifyRaid）
    if (const auto it = m.loaded.find({ owner, key }); it != m.loaded.end() && it->second.blocked) {
        return DD_PERSIST_E_INCONSISTENT;
    }
    m.saveItems[{ owner, key }] = Impl::SaveItem{ std::move(data), std::move(version) };
    return DD_PERSIST_OK;
}

bool Persist::Info(DDPersistInfo* out)
{
    auto& m = *impl_;
    if (!m.IsMain() || !out || out->size < sizeof(DDPersistInfo)) {
        return false;
    }
    m.infoAccount = m.account;
    m.infoDevice = m.ctx.active ? m.ctx.device : std::string();
    m.infoDir = m.ctx.active ? Narrow(m.ctx.dir) : std::string();
    out->hooks = m.hooksOk;
    out->hook_count = static_cast<std::uint32_t>(m.hookCount);
    out->account = m.infoAccount.c_str();
    out->device = m.infoDevice.c_str();
    out->directory = m.infoDir.c_str();
    out->saves = m.savesTotal;
    const Writer::Stats stats = m.writer ? m.writer->GetStats() : Writer::Stats{};
    out->written = stats.written;
    out->merged = stats.merged;
    out->failed = stats.failed;
    out->retries = stats.retries;
    return true;
}

void Persist::OnAppReady()
{
    auto& m = *impl_;
    m.appReady = true;
    if (m.declared.empty()) {
        m.Log(DD_LOG_INFO, "没有插件声明使用存档旁路，观察 hook 不安装");
        return;
    }
    std::string names;
    for (const auto& d : m.declared) {
        names += (names.empty() ? "" : "、") + m.PluginName(d.owner);
    }
    const auto* a = m.deps.addr;
    std::uint64_t handles[4] = {};
    bool ok = a->storageManager != nullptr;
    if (!ok) {
        m.hookError = "特征码 StorageManager 缺失";
    }
    ok = ok && m.InstallOne("persist.write(StorageQueueWrite)", "StorageQueueWrite", a->storageQueueWrite, reinterpret_cast<void*>(&QueueWriteDetour),
                  reinterpret_cast<void**>(&g_origWrite), &handles[0]);
    ok = ok && m.InstallOne("persist.read(StorageReadFile)", "StorageReadFile", a->storageReadFile, reinterpret_cast<void*>(&ReadFileDetour),
                  reinterpret_cast<void**>(&g_origRead), &handles[1]);
    ok = ok && m.InstallOne("persist.backup(SaveSlotBackup)", "SaveSlotBackup", a->saveSlotBackup, reinterpret_cast<void*>(&BackupDetour),
                  reinterpret_cast<void**>(&g_origBackup), &handles[2]);
    ok = ok && m.InstallOne("persist.exit(AppShutdown)", "AppShutdown", a->appShutdown, reinterpret_cast<void*>(&ShutdownDetour),
                  reinterpret_cast<void**>(&g_origShutdown), &handles[3]);
    if (!ok) {
        // 只装上一部分时全部拆掉：观察不完整就判断不了一致性，保持原版行为
        for (const auto handle : handles) {
            if (handle) {
                std::string detail;
                m.deps.hooks->Remove(hook::kCoreOwner, handle, &detail);
            }
        }
        m.Log(DD_LOG_WARN, "观察 hook 安装失败（" + m.hookError + "），存档旁路不可用（声明的插件：" + names + "）");
        if (m.topic && m.deps.events) {
            m.deps.events->hub().SetAvailability(m.topic, false, "观察 hook 安装失败：" + m.hookError);
        }
        return;
    }
    m.hooksOk = true;
    m.hookCount = 4;
    m.writer = std::make_unique<Writer>(m.deps.log);
    if (m.topic && m.deps.events) {
        m.deps.events->hub().SetAvailability(m.topic, true, "");
    }
    m.Log(DD_LOG_INFO, "插件 " + names + " 声明了存档旁路，观察 hook 已安装 4 个（排队写文件、读文件、选槽复制 backup、退到桌面），只读不改游戏行为");
}

void Persist::OnFrameBoundary()
{
    auto& m = *impl_;
    if (!m.hooksOk) {
        return;
    }
    const bool loading = m.deps.loadingNow && m.deps.loadingNow();
    if (m.verifyPending) {
        // 地牢存档：等 as_raid_start 的加载（load_raid 线程读 persist.raid.json 和其中的战斗）结束再核对 raid
        Impl::Rec raid;
        if (loading) {
            m.sawRaidLoading = m.sawRaidLoading || *m.deps.addr->stateLast == m.deps.addr->states.raidStart;
        } else if (m.sawRaidLoading || m.FindRec(m.reads, m.ctx.device, kRaidFile, &raid, m.ctx.loadEpoch)) {
            m.VerifyRaid(nullptr);
        } else if (++m.verifyWait > kVerifyWaitFrames) {
            m.VerifyRaid("CAMPAIGN_LOAD 之后很久没有观察到 as_raid_start 的加载");
        }
    }
    bool trigger = false;
    {
        std::lock_guard lock(m.mutex);
        trigger = m.pending.trigger;
    }
    // 加载线程（load_raid、load_town）写的一组，等加载结束再派发；Lua 在加载期间不动游戏状态
    if (trigger && !loading && m.deps.campaignActive && m.deps.campaignActive()) {
        m.DispatchSave("帧边界");
    }
}

void Persist::BeforeCampaignUnload(bool alive)
{
    auto& m = *impl_;
    if (!m.hooksOk) {
        return;
    }
    bool trigger = false;
    {
        std::lock_guard lock(m.mutex);
        trigger = m.pending.trigger;
    }
    if (trigger && alive) {
        m.DispatchSave("退出存档前");
    }
}

void Persist::OnCampaignLoad()
{
    auto& m = *impl_;
    m.ctx = Impl::Context{};
    m.loaded.clear();
    m.monsterTags.clear();
    m.verifyPending = false;
    m.saveIndex = 0;
    m.tagsSeen = false;
    {
        std::lock_guard lock(m.mutex);
        m.pending = Impl::Pending{};  // 读档过程中的写入（load_campaign 重写 persist.game.json）不算
    }
    if (!m.hooksOk) {
        return;
    }
    auto& c = m.ctx;
    c.active = true;
    c.device = m.ProfileDevice();
    c.account = m.Account();
    // 本代（上次退出存档以来）读到的名册
    const std::uint32_t epochNow = m.epoch.load();
    Impl::Rec roster;
    const bool haveRoster = m.FindRec(m.reads, c.device, kRosterFile, &roster, epochNow);
    c.roster = FileHash{ kRosterFile, roster.hash, roster.size };
    c.fromBackup = haveRoster && roster.fromBackup;
    const auto* a = m.deps.addr;
    c.inRaid = *a->stateNext == a->states.raidStart ? 1 : 0;
    // 地牢里的存档：状态 1 的 3DBC20 在 CAMPAIGN_LOAD 之前已经在主线程读过一次 raid（P4 探针场景两次读地牢存档都是这样，
    // 哈希和之后 load_raid 读的相同），读档时就拿它和名册一起判断两代（审核 S1）。路径要和名册一样来自根目录或 backup/
    Impl::Rec raid;
    if (c.inRaid == 1 && m.FindRec(m.reads, c.device, kRaidFile, &raid, epochNow) && raid.fromBackup == c.fromBackup) {
        c.raidKnown = true;
        c.raid = FileHash{ kRaidFile, raid.hash, raid.size };
    }
    // 新战役：3DB3B0 用 2493E0 把 scripts/starting_save/*.json 原样复制进存档位，load_campaign 读到的名册就是那个文件（审核 S6）
    c.newCampaign = haveRoster && !c.fromBackup && SameContent(m.deps.gameRoot + L"\\scripts\\starting_save\\" + Widen(kRosterFile), c.roster);
    if (!ValidDevice(c.device)) {
        c.reason = "取不到当前 profile 的设备名（" + c.device + "）";
    } else if (c.account.empty()) {
        c.reason = "取不到账号";
    } else if (!haveRoster) {
        c.reason = "这次读档没有观察到名册（persist.roster.json）的读取";
    }
    c.usable = c.reason.empty();
    if (c.usable) {
        c.baseDir = m.DirFor(c.account, c.device);
        c.dir = c.fromBackup ? c.baseDir + L"\\backup" : c.baseDir;
    }
    c.loadEpoch = ++m.epoch;
    if (c.usable && c.inRaid == 1) {
        m.verifyPending = true;
        m.sawRaidLoading = false;
        m.verifyWait = 0;
    }
    m.logNextRoster = true;
    std::string raidText;
    if (c.inRaid == 1) {
        raidText = c.raidKnown ? "，raid " + HashText(c.raid) + "（读档前读到）" : "，读档前没有读到 raid（等探索存档读完再核对）";
    }
    m.Log(c.usable ? DD_LOG_INFO : DD_LOG_WARN, "进存档：设备 " + c.device + "，账号 " + (c.account.empty() ? "?" : c.account) + "，名册 " +
        (haveRoster ? HashText(c.roster) : "?") + (c.fromBackup ? "（游戏读的是 backup/）" : "") + "，存档在" + (c.inRaid == 1 ? "地牢里" : "城镇") + raidText +
        (c.newCampaign ? "，新战役（名册是 starting_save 的初始名册）" : "") + (c.usable ? "；旁路目录 " + Narrow(c.dir) : "；不能判断一致性：" + c.reason));
    m.LoadCoreTags();
}

void Persist::OnCampaignUnload()
{
    auto& m = *impl_;
    // 没写完的留在后台继续写（下次读档前先等它）；没写完、有放弃的写 WARN 并给出路径，写完了不写日志
    m.ReportFlush("退出存档", kFlushMs, true);
    m.ctx = Impl::Context{};
    m.loaded.clear();
    m.monsterTags.clear();
    m.verifyPending = false;
    m.saveItems.clear();
    ++m.epoch;
}

void Persist::OnPluginRollback(std::uint32_t owner)
{
    auto& m = *impl_;
    std::erase_if(m.declared, [&](const Impl::Declared& d) { return d.owner == owner; });
}

bool Persist::InSaveCallback() const
{
    return impl_->saveDepth > 0;
}

std::size_t Persist::HookCount() const
{
    return impl_->hooksOk ? impl_->hookCount : 0;
}

std::string Persist::Summary() const
{
    const auto& m = *impl_;
    if (m.declared.empty()) {
        return "persist：观察 hook 0 个（没有插件声明使用存档旁路）";
    }
    const Writer::Stats stats = m.writer ? m.writer->GetStats() : Writer::Stats{};
    return "persist：观察 hook " + std::to_string(m.hooksOk ? m.hookCount : 0) + " 个（" + (m.hooksOk ? "已装" : "安装失败：" + m.hookError) +
        "），声明的插件 " + std::to_string(m.declared.size()) + " 个，观察到存档读 " + std::to_string(m.observedReads.load()) + " 次、写 " +
        std::to_string(m.observedWrites.load()) + " 次，campaign.save 派发 " + std::to_string(m.savesTotal) + " 次，旁路写盘 " +
        std::to_string(stats.written) + " 个（合并 " + std::to_string(stats.merged) + " 次，失败 " + std::to_string(stats.failed) + " 次，重试 " +
        std::to_string(stats.retries) + " 次）";
}

}  // namespace ddc::persist

// probe_p3：P3 开工前逆向结论的运行验证，只读，不改任何游戏数据。
// 核对：当前存档的启用 Mod 列表和目录解析、名册和英雄 id、战斗中的 Actor 分类和反查、
// estate.currency 和 query.TownVisitIdleStressBuffQuery 修正后的字段。
// 依据见 knowledge/dd1/data-and-mods.md、combat.md、events.md。地址是 Steam 版的 RVA，exe 不符时整体不工作。
// 日志：DDModLoader\log\probe_p3.log

#include <dd/dd.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

dd::Runtime g_dd;
std::uintptr_t g_base = 0;
bool g_enabled = false;

constexpr const char* kExeSha1 = "9db2d73b89aa034e2055df65a139403e18df641f";

constexpr std::uintptr_t kAppliedUgcVector = 0x2A6D8C0;  // App+0x748 的 UGCIdList+0x208
constexpr std::uintptr_t kUserServices = 0x118DFE8;
constexpr std::uintptr_t kSteamUserServicesVtbl = 0xE677F8;
constexpr std::uintptr_t kDefaultUserServicesVtbl = 0xE67298;
constexpr std::uintptr_t kCampaignCurrent = 0x118E138;
constexpr std::uintptr_t kAltRosterFlag = 0x2A6D4BB;
constexpr std::uintptr_t kRaidCurrent = 0x118E2C8;
constexpr std::uintptr_t kHeroVtbl = 0xE8A288;
constexpr std::uintptr_t kMonsterVtbl = 0xEA9480;

constexpr std::size_t kUgcIdSize = 0xC0;
constexpr std::size_t kUgcPath = 0x20;
constexpr std::size_t kUgcId = 0x620;
constexpr std::size_t kRosterHeroes = 0x20;
constexpr std::size_t kEntryHero = 0x08;
constexpr std::size_t kEntryHeroId = 0x1384;
constexpr std::size_t kMonsterGuid = 0x1320;
constexpr std::size_t kRaidBattle = 0x4898;
constexpr std::size_t kBattleMonsters = 0x38;

struct Vec {
    std::uintptr_t first = 0;
    std::uintptr_t last = 0;
    std::uintptr_t end = 0;
};

bool ReadBytes(std::uintptr_t src, void* dst, std::size_t size)
{
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Read(std::uintptr_t address, T& out)
{
    return address != 0 && ReadBytes(address, &out, sizeof(T));
}

std::string ReadString(std::uintptr_t address, std::size_t capacity)
{
    std::vector<char> buffer(capacity + 1, '\0');
    if (!ReadBytes(address, buffer.data(), capacity)) {
        return "<读取失败>";
    }
    return std::string(buffer.data(), strnlen(buffer.data(), capacity));
}

std::vector<std::uintptr_t> ReadPointerVector(std::uintptr_t address)
{
    std::vector<std::uintptr_t> out;
    Vec vec;
    if (!Read(address, vec) || vec.first == 0 || vec.last < vec.first || vec.last - vec.first > 8 * 4096) {
        return out;
    }
    out.resize((vec.last - vec.first) / 8);
    if (!out.empty() && !ReadBytes(vec.first, out.data(), out.size() * 8)) {
        out.clear();
    }
    return out;
}

// 虚函数调用放在 __try 里，指针不对时不拖垮游戏
bool CallVirtual(std::uintptr_t object, int slot, std::uintptr_t& out)
{
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        out = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtable[slot])(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t Hash(const char* text)
{
    std::uint32_t h = 0;
    for (; *text; ++text) {
        h = h * 53 + static_cast<unsigned char>(*text);
    }
    return h;
}

bool DirectoryExists(const std::string& path)
{
    for (UINT codepage : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
        const DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int length = MultiByteToWideChar(codepage, flags, path.c_str(), -1, nullptr, 0);
        if (length <= 0) {
            continue;
        }
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(codepage, flags, path.c_str(), -1, wide.data(), length);
        const DWORD attributes = GetFileAttributesW(wide.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }
    }
    return false;
}

const void* Ptr(std::uintptr_t value)
{
    return reinterpret_cast<const void*>(value);
}

// ---- 启用 Mod 列表 ----

struct InstalledUgc {
    std::string name;
    std::string source;
    std::string path;
};

std::vector<InstalledUgc> ReadInstalled(std::uintptr_t services, std::size_t offset)
{
    std::vector<InstalledUgc> out;
    for (auto ugc : ReadPointerVector(services + offset)) {
        out.push_back({ ReadString(ugc + kUgcId, 0x80), ReadString(ugc + kUgcId + 0x80, 0x40), ReadString(ugc + kUgcPath, 0x200) });
    }
    return out;
}

void CheckAppliedMods()
{
    Vec applied;
    if (!Read(g_base + kAppliedUgcVector, applied) || applied.last < applied.first) {
        g_dd.log().warn("启用列表：读取 vector 失败");
        return;
    }
    const std::size_t count = (applied.last - applied.first) / kUgcIdSize;

    std::uintptr_t services = 0;
    std::uintptr_t servicesVtbl = 0;
    Read(g_base + kUserServices, services);
    Read(services, servicesVtbl);
    const bool steam = servicesVtbl == g_base + kSteamUserServicesVtbl;
    const bool offline = servicesVtbl == g_base + kDefaultUserServicesVtbl;
    auto installed = ReadInstalled(services, 0x20);
    const std::size_t localCount = installed.size();
    if (steam) {
        auto workshop = ReadInstalled(services, 0x80);
        installed.insert(installed.end(), workshop.begin(), workshop.end());
    }
    g_dd.log().info("启用列表 {} 项；UserServices={} 类型={}，已安装本地 {} 个、Workshop {} 个", count, Ptr(services),
        steam ? "SteamUserServices" : (offline ? "DefaultUserServices" : "未知虚表"), localCount, installed.size() - localCount);

    const std::string root = g_dd.game()->game_root();
    std::size_t resolved = 0;
    std::size_t exists = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto entry = applied.first + i * kUgcIdSize;
        const auto name = ReadString(entry, 0x80);
        const auto source = ReadString(entry + 0x80, 0x40);
        const InstalledUgc* match = nullptr;
        for (const auto& ugc : installed) {
            if (ugc.name == name && ugc.source == source) {
                match = &ugc;
                break;
            }
        }
        std::string dir;
        bool dirExists = false;
        if (match) {
            ++resolved;
            dir = match->path;
            const bool relative = dir.size() < 2 || dir[1] != ':';
            dirExists = DirectoryExists(relative ? root + "\\" + dir : dir);
            exists += dirExists ? 1 : 0;
        }
        g_dd.log().info("  [{}] name='{}' source='{}' -> {}{}", i, name, source, match ? dir : std::string("未安装或未找到"),
            match ? (dirExists ? "（目录存在）" : "（目录不存在！）") : "");
    }
    g_dd.log().info("启用列表解析：{}/{} 项找到已安装条目，其中目录存在 {} 项", resolved, count, exists);
}

// ---- 名册 ----

std::unordered_map<std::uintptr_t, std::uint32_t> g_heroes;  // Hero* -> hero id
std::unordered_map<std::uint32_t, std::uintptr_t> g_heroById;

std::uintptr_t RosterSystem()
{
    std::uintptr_t campaign = 0;
    std::uint8_t alt = 0;
    Read(g_base + kCampaignCurrent, campaign);
    Read(g_base + kAltRosterFlag, alt);
    return campaign == 0 ? 0 : (alt ? campaign + 0x728 : campaign);
}

void ScanRoster(bool log)
{
    g_heroes.clear();
    g_heroById.clear();
    const auto roster = RosterSystem();
    const auto entries = ReadPointerVector(roster + kRosterHeroes);
    std::size_t vtblOk = 0;
    std::size_t idOk = 0;
    std::string sample;
    for (auto entry : entries) {
        const auto hero = entry + kEntryHero;
        std::uintptr_t vtbl = 0;
        std::uint32_t rawId = 0;
        std::uintptr_t virtualId = 0;
        Read(hero, vtbl);
        Read(entry + kEntryHeroId, rawId);
        const bool isHero = vtbl == g_base + kHeroVtbl;
        vtblOk += isHero ? 1 : 0;
        if (isHero && CallVirtual(hero, 5, virtualId) && static_cast<std::uint32_t>(virtualId) == rawId) {
            ++idOk;
        }
        g_heroes[hero] = rawId;
        g_heroById[rawId] = hero;
        if (sample.size() < 120) {
            sample += " " + std::to_string(rawId);
        }
    }
    if (log) {
        g_dd.log().info("名册：Roster::System={} 英雄 {} 个，虚表是 Hero 的 {} 个，vtbl[5] 和 Entry+0x1384 一致 {} 个；id:{}",
            Ptr(roster), entries.size(), vtblOk, idOk, sample);
    }
}

// ---- 战斗中的 Actor ----

struct Counters {
    std::uint64_t heroSeen = 0, heroInRoster = 0, heroIdMatch = 0;
    std::uint64_t monsterSeen = 0, monsterInBattle = 0, monsterGuidMatch = 0;
    std::uint64_t asHeroConsistent = 0, unknownVtbl = 0;
    std::uint64_t deathGuidFound = 0, deathGuidMissing = 0;
    std::uint64_t heroIdEvents = 0, heroIdFound = 0;
} g_count;

int g_actorLogs = 0;

std::vector<std::uintptr_t> BattleMonsters()
{
    std::uintptr_t raid = 0;
    Read(g_base + kRaidCurrent, raid);
    return raid ? ReadPointerVector(raid + kRaidBattle + kBattleMonsters) : std::vector<std::uintptr_t>{};
}

void CheckActor(const char* topic, const char* role, std::uintptr_t actor)
{
    if (actor == 0) {
        return;
    }
    std::uintptr_t vtbl = 0;
    Read(actor, vtbl);
    std::uintptr_t asHero = 0;
    std::uintptr_t id = 0;
    const bool callOk = CallVirtual(actor, 3, asHero) && CallVirtual(actor, 5, id);
    std::string verdict;
    if (vtbl == g_base + kHeroVtbl) {
        ++g_count.heroSeen;
        g_count.asHeroConsistent += (callOk && asHero == actor) ? 1 : 0;
        auto it = g_heroes.find(actor);
        if (it == g_heroes.end()) {
            ScanRoster(false);
            it = g_heroes.find(actor);
        }
        const bool inRoster = it != g_heroes.end();
        const bool idMatch = inRoster && static_cast<std::uint32_t>(id) == it->second;
        g_count.heroInRoster += inRoster ? 1 : 0;
        g_count.heroIdMatch += idMatch ? 1 : 0;
        verdict = std::string("英雄 id=") + std::to_string(static_cast<std::uint32_t>(id)) + (inRoster ? " 在名册中" : " 不在名册中！") +
                  (idMatch ? "，id 一致" : "");
    } else if (vtbl == g_base + kMonsterVtbl) {
        ++g_count.monsterSeen;
        g_count.asHeroConsistent += (callOk && asHero == 0) ? 1 : 0;
        bool inBattle = false;
        for (auto monster : BattleMonsters()) {
            inBattle = inBattle || monster == actor;
        }
        std::uint32_t rawGuid = 0;
        Read(actor + kMonsterGuid, rawGuid);
        const bool guidMatch = callOk && static_cast<std::uint32_t>(id) == rawGuid;
        g_count.monsterInBattle += inBattle ? 1 : 0;
        g_count.monsterGuidMatch += guidMatch ? 1 : 0;
        verdict = std::string("怪物 guid=") + std::to_string(rawGuid) + (inBattle ? " 在 Battle+0x38 中" : " 不在 Battle+0x38 中！") +
                  (guidMatch ? "，vtbl[5] 一致" : "，vtbl[5] 不一致！");
    } else {
        ++g_count.unknownVtbl;
        verdict = "虚表既不是 Hero 也不是 Monster！";
    }
    if (g_actorLogs < 40 || verdict.find("！") != std::string::npos) {
        ++g_actorLogs;
        g_dd.log().info("{} {}={} {}", topic, role, Ptr(actor), verdict);
    }
}

void OnActorEvent(DDEvent* ev, const char* first, const char* second)
{
    const auto& events = g_dd.events();
    const char* topic = events.topic(ev);
    CheckActor(topic, first, reinterpret_cast<std::uintptr_t>(events.get_pointer(ev, first)));
    CheckActor(topic, second, reinterpret_cast<std::uintptr_t>(events.get_pointer(ev, second)));
}

void OnBattleStart(DDEvent*)
{
    const auto monsters = BattleMonsters();
    std::string guids;
    std::size_t vtblOk = 0;
    for (auto monster : monsters) {
        std::uintptr_t vtbl = 0;
        std::uint32_t guid = 0;
        Read(monster, vtbl);
        Read(monster + kMonsterGuid, guid);
        vtblOk += vtbl == g_base + kMonsterVtbl ? 1 : 0;
        guids += " " + std::to_string(guid);
    }
    g_dd.log().info("battle.start：Battle+0x38 怪物 {} 个，虚表是 Monster 的 {} 个；guid:{}", monsters.size(), vtblOk, guids);
}

void OnDeath(DDEvent* ev)
{
    const auto& events = g_dd.events();
    if (events.get_bool(ev, "is_monster")) {
        const auto guid = static_cast<std::uint32_t>(events.get_int(ev, "monster_guid"));
        bool found = false;
        for (auto monster : BattleMonsters()) {
            std::uint32_t raw = 0;
            found = found || (Read(monster + kMonsterGuid, raw) && raw == guid);
        }
        (found ? g_count.deathGuidFound : g_count.deathGuidMissing) += 1;
        g_dd.log().info("actor.death 怪物 guid={} {}", guid, found ? "在 Battle+0x38 中找到" : "已不在 Battle+0x38 中");
    } else {
        const auto id = static_cast<std::uint32_t>(events.get_int(ev, "hero_id"));
        ++g_count.heroIdEvents;
        ScanRoster(false);
        const bool found = g_heroById.count(id) != 0;
        g_count.heroIdFound += found ? 1 : 0;
        g_dd.log().info("actor.death 英雄 hero_id={} {}", id, found ? "在名册中找到" : "名册中没有");
    }
}

void OnHeroIdEvent(DDEvent* ev)
{
    const auto id = static_cast<std::uint32_t>(g_dd.events().get_int(ev, "hero_id"));
    ++g_count.heroIdEvents;
    if (g_heroById.count(id) == 0) {
        ScanRoster(false);
    }
    const bool found = g_heroById.count(id) != 0;
    g_count.heroIdFound += found ? 1 : 0;
    if (g_count.heroIdEvents <= 20 || !found) {
        g_dd.log().info("{} hero_id={} {}", g_dd.events().topic(ev), id, found ? "在名册中找到" : "名册中没有！");
    }
}

// ---- 修正后的字段 ----

const char* FieldTypeText(const char* topic, const char* field)
{
    const auto* service = g_dd.events().service();
    DDEventFieldInfo info{};
    for (std::uint32_t i = 0; service && service->topic_field_at(topic, i, &info); ++i) {
        if (std::strcmp(info.name, field) == 0) {
            switch (info.type) {
            case DD_FIELD_INT:
                return "INT";
            case DD_FIELD_FLOAT:
                return "FLOAT";
            case DD_FIELD_BOOL:
                return "BOOL";
            case DD_FIELD_STRING:
                return "STRING";
            default:
                return "POINTER";
            }
        }
    }
    return "不存在";
}

int g_currencyLogs = 0;
std::uint64_t g_currencyKnown = 0, g_currencyUnknown = 0;

void OnCurrency(DDEvent* ev)
{
    static const char* kNames[] = { "gold", "bust", "portrait", "deed", "crest", "shard", "memory", "blueprint" };
    const auto& events = g_dd.events();
    const auto currency = static_cast<std::uint64_t>(events.get_int(ev, "currency"));
    const auto delta = events.get_int(ev, "delta");
    const char* name = nullptr;
    for (const char* candidate : kNames) {
        if (Hash(candidate) == currency) {
            name = candidate;
        }
    }
    (name ? g_currencyKnown : g_currencyUnknown) += 1;
    if (g_currencyLogs++ < 20) {
        g_dd.log().info("estate.currency currency={}（{}） delta={} 延迟={}", currency, name ? name : "未知哈希", delta,
            (events.flags(ev) & DD_EVENT_F_DEFERRED) ? "是" : "否");
    }
}

int g_stressLogs = 0;

void OnIdleStress(DDEvent* ev)
{
    if (g_stressLogs++ < 10) {
        g_dd.log().info("query.TownVisitIdleStressBuffQuery stress_relief={:.3f} flag={}", g_dd.events().get_float(ev, "stress_relief"),
            g_dd.events().get_bool(ev, "flag"));
    }
}

void Summary()
{
    const auto& c = g_count;
    g_dd.log().info("==== 汇总 ====");
    g_dd.log().info("英雄 {} 次：在名册中 {}，id 一致 {}；怪物 {} 次：在 Battle+0x38 中 {}，guid 一致 {}；vtbl[3] 一致 {}，未知虚表 {}",
        c.heroSeen, c.heroInRoster, c.heroIdMatch, c.monsterSeen, c.monsterInBattle, c.monsterGuidMatch, c.asHeroConsistent, c.unknownVtbl);
    g_dd.log().info("怪物死亡 guid 找到 {}，已不在表中 {}；hero_id 事件 {} 次，名册中找到 {}", c.deathGuidFound, c.deathGuidMissing,
        c.heroIdEvents, c.heroIdFound);
    g_dd.log().info("estate.currency 共 {} 次，已知货币 {}，未知哈希 {}；TownVisitIdleStressBuffQuery {} 次", g_currencyLogs,
        g_currencyKnown, g_currencyUnknown, g_stressLogs);
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "probe_p3";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.events()) {
        return false;
    }
    const char* sha1 = g_dd.game()->exe_sha1();
    g_enabled = sha1 && std::strcmp(sha1, kExeSha1) == 0;
    g_base = reinterpret_cast<std::uintptr_t>(g_dd.game()->image_base());
    if (!g_enabled) {
        g_dd.log().warn("exe SHA1 {} 不是基准版本，探针不工作", sha1 ? sha1 : "");
        return true;
    }
    g_dd.log().info("exe 是基准版本，映像基址 {}", Ptr(g_base));
    g_dd.log().info("字段类型：estate.currency.currency={} delta={}，query.TownVisitIdleStressBuffQuery.stress_relief={}",
        FieldTypeText("estate.currency", "currency"), FieldTypeText("estate.currency", "delta"),
        FieldTypeText("query.TownVisitIdleStressBuffQuery", "stress_relief"));

    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_LOAD, [](const DDPhaseEvent&) {
        CheckAppliedMods();
        ScanRoster(true);
    });
    g_dd.lifecycle().subscribe(DD_PHASE_CAMPAIGN_UNLOAD, [](const DDPhaseEvent&) { Summary(); });

    const auto& events = g_dd.events();
    events.on("battle.start", OnBattleStart);
    events.on("hit.pre", [](DDEvent* ev) { OnActorEvent(ev, "attacker", "target"); });
    events.on("damage.pre", [](DDEvent* ev) { OnActorEvent(ev, "target", "attacker"); });
    events.on("skill.pre", [](DDEvent* ev) { OnActorEvent(ev, "performer", "target"); });
    events.on("actor.death", OnDeath);
    events.on("hero.quirk", OnHeroIdEvent);
    events.on("hero.skill", OnHeroIdEvent);
    events.on("estate.currency", OnCurrency);
    events.on("query.TownVisitIdleStressBuffQuery", OnIdleStress);
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}

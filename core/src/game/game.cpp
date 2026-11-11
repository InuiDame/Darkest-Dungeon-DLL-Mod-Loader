#include "game.h"

#include "../../../common/pattern.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ddc::game {

namespace {

enum class Kind { Hook, Global, GlobalAny, Code };
enum class Req { Required, Loader, Optional };

struct Sig {
    const char* id;
    Kind kind;
    Req required;
    std::uint32_t steal;
    std::int32_t capture;
    const char* pattern;
};

constexpr Kind HOOK = Kind::Hook;
constexpr Kind GLOBAL = Kind::Global;
constexpr Kind GLOBAL_ANY = Kind::GlobalAny;
constexpr Kind CODE = Kind::Code;
constexpr Req REQUIRED = Req::Required;
constexpr Req LOADER = Req::Loader;
constexpr Req OPT = Req::Optional;

const Sig kSignatures[] = {
#define DD_SIG(id, kind, req, steal, capture, pattern) { #id, kind, req, steal, capture, pattern },
#define DD_KNOWN_BUILD(sha, label)
#include "signatures.inc"
#undef DD_SIG
#undef DD_KNOWN_BUILD
};

struct EntryBytes {
    const char* id;
    const char* bytes;
};

const EntryBytes kEntries[] = {
#define DD_SIG(id, kind, req, steal, capture, pattern)
#define DD_KNOWN_BUILD(sha, label)
#undef DD_ENTRY
#define DD_ENTRY(id, bytes) { #id, bytes },
#include "signatures.inc"
#undef DD_SIG
#undef DD_KNOWN_BUILD
#undef DD_ENTRY
};

struct KnownBuild {
    const char* sha1;
    const char* label;
};

const KnownBuild kKnownBuilds[] = {
#define DD_SIG(id, kind, req, steal, capture, pattern)
#define DD_KNOWN_BUILD(sha, label) { sha, label },
#define DD_ENTRY(id, bytes)
#include "signatures.inc"
#undef DD_SIG
#undef DD_KNOWN_BUILD
#undef DD_ENTRY
};

std::string CountText(std::size_t hits)
{
    return hits == 0 ? "未找到" : "命中 " + std::to_string(hits) + " 次，要求唯一";
}

void ScanAll(const std::uint8_t* text, std::size_t textSize, const std::vector<int>& pattern,
    std::vector<const std::uint8_t*>& hits)
{
    if (pattern.empty() || textSize < pattern.size()) {
        return;
    }
    const std::size_t last = textSize - pattern.size();
    for (std::size_t i = 0; i <= last; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && text[i + j] != static_cast<std::uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            hits.push_back(text + i);
        }
    }
}

}  // namespace

const char* BuildLabel(const std::string& sha1)
{
    for (const auto& build : kKnownBuilds) {
        if (sha1 == build.sha1) {
            return build.label;
        }
    }
    return nullptr;
}

bool HookEntryBytes(const char* id, std::vector<std::uint8_t>& out)
{
    for (const auto& entry : kEntries) {
        if (std::strcmp(entry.id, id) == 0) {
            std::vector<int> bytes;
            if (!dd::common::ParsePattern(entry.bytes, bytes)) {
                return false;
            }
            out.clear();
            for (int value : bytes) {
                if (value < 0) {
                    return false;
                }
                out.push_back(static_cast<std::uint8_t>(value));
            }
            return !out.empty();
        }
    }
    for (const auto& sig : kSignatures) {
        if (sig.kind != Kind::Hook || std::strcmp(sig.id, id) != 0) {
            continue;
        }
        std::vector<int> pattern;
        if (!dd::common::ParsePattern(sig.pattern, pattern) || pattern.size() < sig.steal) {
            return false;
        }
        out.clear();
        for (std::uint32_t i = 0; i < sig.steal; ++i) {
            if (pattern[i] < 0) {
                return false;
            }
            out.push_back(static_cast<std::uint8_t>(pattern[i]));
        }
        return true;
    }
    return false;
}

bool Resolve(HMODULE exe, Addresses& out, ResolveReport& report)
{
    const std::uint8_t* text = nullptr;
    std::size_t textSize = 0;
    if (!dd::common::ModuleTextSection(exe, &text, &textSize)) {
        report.missing.push_back("无法定位 Darkest.exe 的 .text 节");
        return false;
    }

    const auto* base = reinterpret_cast<const std::uint8_t*>(exe);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;

    bool allRequired = true;
    const auto noteMissing = [&](const Sig& sig, const std::string& what) {
        char line[192];
        std::snprintf(line, sizeof(line), "%s（%s）", sig.id, what.c_str());
        if (sig.required == Req::Required) {
            report.missing.push_back(line);
            allRequired = false;
        } else {
            report.notes.push_back(std::string("可选特征码 ") + line);
        }
    };

    std::vector<const std::uint8_t*> queryNameCandidates;

    for (const auto& sig : kSignatures) {
        // 加载器专用的特征码：这里入口已经被加载器打了补丁，重复扫描必然失败
        if (sig.required == Req::Loader) {
            continue;
        }
        std::vector<int> pattern;
        if (!dd::common::ParsePattern(sig.pattern, pattern)) {
            noteMissing(sig, "特征码格式错误");
            continue;
        }

        const std::uint8_t* hit = nullptr;
        std::vector<const std::uint8_t*> anyTargets;
        if (sig.kind == Kind::GlobalAny) {
            std::vector<const std::uint8_t*> hits;
            ScanAll(text, textSize, pattern, hits);
            for (const auto* item : hits) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, item + sig.capture, 4);
                const auto* global = item + sig.capture + 4 + displacement;
                if (global >= base && static_cast<std::size_t>(global - base) < imageSize &&
                    std::find(anyTargets.begin(), anyTargets.end(), global) == anyTargets.end()) {
                    anyTargets.push_back(global);
                }
            }
            if (anyTargets.empty()) {
                noteMissing(sig, "未找到（GLOBAL_ANY）");
                continue;
            }
            if (anyTargets.size() > 1 && std::strcmp(sig.id, "QueryClassNames") != 0) {
                noteMissing(sig, "多处命中解析出 " + std::to_string(anyTargets.size()) + " 个不同地址");
                continue;
            }
            hit = anyTargets[0];
        } else {
            const std::size_t count = dd::common::ScanPattern(text, textSize, pattern, &hit);
            if (count != 1) {
                noteMissing(sig, CountText(count));
                continue;
            }
        }

        void* resolved = nullptr;
        if (sig.kind == Kind::Global) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, hit + sig.capture, 4);
            const auto* global = hit + sig.capture + 4 + displacement;
            if (global < base || static_cast<std::size_t>(global - base) >= imageSize) {
                noteMissing(sig, "解析出的全局地址不在映像内");
                continue;
            }
            resolved = const_cast<std::uint8_t*>(global);
        } else {
            resolved = const_cast<std::uint8_t*>(hit);
        }

        const auto is = [&](const char* id) { return std::strcmp(sig.id, id) == 0; };
        if (is("CreateAppSystems")) {
            out.createAppSystems = static_cast<std::uint8_t*>(resolved);
        } else if (is("UpdateStateInternal")) {
            out.updateState = static_cast<std::uint8_t*>(resolved);
        } else if (is("StateLast")) {
            out.stateLast = static_cast<int*>(resolved);
        } else if (is("StateNext")) {
            out.stateNext = static_cast<int*>(resolved);
        } else if (is("CampaignCurrent")) {
            out.campaignCurrent = static_cast<void**>(resolved);
        } else if (is("BuffLibrary")) {
            out.buffLibrary = static_cast<void**>(resolved);
        } else if (is("StateNames")) {
            // 状态名表：一串指向 .rdata 字符串的指针，读到第一个无效指针为止
            auto* const* table = static_cast<const char* const*>(resolved);
            for (std::size_t i = 0; i < 64; ++i) {
                const char* name = table[i];
                if (!name || reinterpret_cast<const std::uint8_t*>(name) < base ||
                    static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(name) - base) >= imageSize) {
                    break;
                }
                if (std::strncmp(name, "as_", 3) != 0) {
                    break;
                }
                out.stateNames.push_back(name);
            }
        } else if (is("EventManager")) {
            out.eventManager = static_cast<void**>(resolved);
        } else if (is("EventTypeCount")) {
            out.eventTypeCount = static_cast<const int*>(resolved);
        } else if (is("EventClassNames")) {
            out.eventClassNames = static_cast<const char*>(resolved);
        } else if (is("ListenerAdd")) {
            out.listenerAdd = resolved;
        } else if (is("QueryDispatcher")) {
            out.queryDispatcher = static_cast<void**>(resolved);
        } else if (is("QueryClassNames")) {
            queryNameCandidates = anyTargets;
        } else if (is("QueryListenerAdd")) {
            out.queryListenerAdd = resolved;
        } else if (is("HeroVtable")) {
            out.heroVtable = static_cast<void* const*>(resolved);
        } else if (is("MonsterVtable")) {
            out.monsterVtable = static_cast<void* const*>(resolved);
        } else if (is("RaidCurrent")) {
            out.raidCurrent = static_cast<void**>(resolved);
        } else if (is("CombatStartTurn")) {
            out.combatStartTurn = static_cast<std::uint8_t*>(resolved);
        } else if (is("ApplyHit")) {
            out.applyHit = static_cast<std::uint8_t*>(resolved);
        } else if (is("ApplyMiss")) {
            out.applyMiss = static_cast<std::uint8_t*>(resolved);
        } else if (is("ActorDoAttackSkill")) {
            out.doAttackSkill = static_cast<std::uint8_t*>(resolved);
        } else if (is("ActorDoSkillFriendly")) {
            out.doSkillFriendly = static_cast<std::uint8_t*>(resolved);
        } else if (is("BattleAdvanceRound")) {
            out.advanceRound = static_cast<std::uint8_t*>(resolved);
        } else if (is("EffectApplyInternal")) {
            out.effectApply = static_cast<std::uint8_t*>(resolved);
        } else if (is("UserServices")) {
            out.userServices = static_cast<void* const*>(resolved);
        } else if (is("AppliedUgcVector")) {
            out.appliedUgcVector = resolved;
        } else if (is("ActorMaxStat")) {
            out.actorMaxStat = resolved;
        }
    }

    // Query 类名表：候选里选表项全部以 Query 结尾的那张，项数就是类型数
    for (const auto* candidate : queryNameCandidates) {
        const char* table = reinterpret_cast<const char*>(candidate);
        std::size_t count = 0;
        bool allQuery = true;
        for (std::size_t i = 0; i < 64; ++i) {
            const char* name = table + i * 0x80;
            const std::size_t length = strnlen(name, 0x80);
            if (length == 0) {
                break;
            }
            if (length < 5 || std::strcmp(name + length - 5, "Query") != 0) {
                allQuery = false;
                break;
            }
            ++count;
        }
        if (allQuery && count > 0) {
            out.queryClassNames = table;
            out.queryTypeCount = static_cast<int>(count);
            break;
        }
    }
    if (!queryNameCandidates.empty() && !out.queryClassNames) {
        report.notes.push_back("可选特征码 QueryClassNames（候选表都不是 Query 类名表，query.* 主题不可用）");
    }

    // 布局校验：状态名必须能找到需要的三个状态，序号由运行时读出，不硬编码
    const auto findState = [&](const char* name) -> int {
        for (std::size_t i = 0; i < out.stateNames.size(); ++i) {
            if (std::strcmp(out.stateNames[i], name) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    if (allRequired) {
        out.states.bootAssets = findState("as_load_boot_assets");
        out.states.loadCampaign = findState("as_load_campaign");
        out.states.unloadCampaign = findState("as_unload_campaign");
        out.states.raidStart = findState("as_raid_start");
        out.states.townVisitStart = findState("as_town_visit_start");
        if (out.states.bootAssets < 0 || out.states.loadCampaign < 0 || out.states.unloadCampaign < 0 ||
            out.states.raidStart < 0 || out.states.townVisitStart < 0) {
            report.layoutErrors.push_back(
                "状态名表中缺少 as_load_boot_assets / as_load_campaign / as_unload_campaign / as_raid_start / as_town_visit_start");
            return false;
        }
        const auto validState = [&](int value) {
            return value >= 0 && value < static_cast<int>(out.stateNames.size());
        };
        if (!validState(*out.stateLast) || !validState(*out.stateNext)) {
            report.layoutErrors.push_back("状态变量的当前值超出状态名表范围");
            return false;
        }
    }
    return allRequired;
}

}  // namespace ddc::game

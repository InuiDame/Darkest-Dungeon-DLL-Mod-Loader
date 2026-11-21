#include "game.h"

#include "layout.h"

#include "../../../common/pattern.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ddc::game {

namespace {

// 特征码表在 layout_check.cpp（check::Signatures()，和运行时核对、对照测试共用），这里只取入口字节和已知版本
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
    for (const auto& sig : check::Signatures()) {
        if (sig.kind != check::SigKind::Hook || std::strcmp(sig.id, id) != 0) {
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
    const auto* base = reinterpret_cast<const std::uint8_t*>(exe);
    LARGE_INTEGER frequency{}, begin{}, scanned{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);

    // 已加载的模块：虚表里的指针已经重定位，减实际基址得到 RVA
    check::ImageView view;
    std::string error;
    if (!check::DescribeImage(base, reinterpret_cast<std::uintptr_t>(exe), &view, &error)) {
        report.missing.push_back("无法解析 Darkest.exe 的 PE 头：" + error);
        return false;
    }
    const check::Scanner scanner(view);
    // 函数表建一次：HOOK 的入口检查（ResolveSignatures）和依据的 FUNC 范围（RunCheck）共用
    LARGE_INTEGER tableBegin{}, tableEnd{};
    QueryPerformanceCounter(&tableBegin);
    const check::FunctionTable table(view);
    QueryPerformanceCounter(&tableEnd);
    report.sigs = check::ResolveSignatures(view, scanner, false, &table);
    QueryPerformanceCounter(&scanned);
    report.resolveMicros = static_cast<double>(scanned.QuadPart - begin.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
    // 在装任何 hook 之前求值全部运行时依据（HOOK 的偷取字节写成了 ??，装了之后也能命中，但核对的是原始代码）
    report.check = check::RunCheck(view, scanner, report.sigs, &table);
    report.check.tableMicros =
        static_cast<double>(tableEnd.QuadPart - tableBegin.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);

    bool allRequired = true;
    std::vector<const std::uint8_t*> queryNameCandidates;
    const auto& defs = check::Signatures();
    for (std::size_t index = 0; index < defs.size(); ++index) {
        const check::SigDef& sig = defs[index];
        const check::SigResult& result = report.sigs[index];
        // 加载器专用的特征码：这里入口已经被加载器打了补丁，重复扫描必然失败
        if (sig.need == check::SigNeed::Loader) {
            continue;
        }
        std::string problem = result.problem;
        if (result.ok && std::find(report.check.misbound.begin(), report.check.misbound.end(), sig.id) != report.check.misbound.end()) {
            problem = "同一性依据不成立，可能绑到了别的函数";
        } else if (result.ok) {
            problem.clear();
        }
        if (!problem.empty()) {
            char line[256];
            std::snprintf(line, sizeof(line), "%s（%s）", sig.id, problem.c_str());
            if (sig.need == check::SigNeed::Required) {
                report.missing.push_back(line);
                allRequired = false;
            } else if (sig.need == check::SigNeed::Anchor) {
                report.notes.push_back(std::string("锚点特征码 ") + line + "，以它为锚点的布局依据没法求值");
            } else {
                report.notes.push_back(std::string("可选特征码 ") + line);
            }
            continue;
        }
        // 锚点只给布局依据定位函数，DDCore 不用它的地址
        if (sig.need == check::SigNeed::Anchor) {
            continue;
        }
        void* resolved = const_cast<std::uint8_t*>(base + result.resolved);

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
                    static_cast<std::size_t>(reinterpret_cast<const std::uint8_t*>(name) - base) >= view.size) {
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
            for (const std::uint32_t candidate : result.candidates) {
                queryNameCandidates.push_back(base + candidate);
            }
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
        } else if (is("BuffGroupAdd")) {
            out.buffGroupAdd = static_cast<std::uint8_t*>(resolved);
        } else if (is("BuffGroupErase")) {
            out.buffGroupErase = static_cast<std::uint8_t*>(resolved);
        } else if (is("BuffDurationUpkeep")) {
            out.buffDurationUpkeep = static_cast<std::uint8_t*>(resolved);
        } else if (is("BuffRuleActive")) {
            out.buffRuleActive = static_cast<std::uint8_t*>(resolved);
        } else if (is("ConsumeHealthDamageBlock")) {
            out.consumeBlock = static_cast<std::uint8_t*>(resolved);
        } else if (is("MonsterBlockReturn")) {
            const auto* hit = static_cast<const std::uint8_t*>(resolved);
            std::int32_t disp = 0;
            std::memcpy(&disp, hit + layout::kMonsterBlockImmuneLeaDisp, 4);  // lea rcx,[rip+disp]（48 8D 0D）的位移和下一条指令
            out.monsterBlockReturn = hit;
            out.monsterImmuneTable = hit + layout::kMonsterBlockImmuneLeaNext + disp;
        } else if (is("BuffInstanceCopy")) {
            out.buffInstanceCopy = resolved;
        } else if (is("VectorDtor64")) {
            out.vectorDtor64 = resolved;
        } else if (is("VectorDtor64b")) {
            out.vectorDtor64b = resolved;
        } else if (is("EhVectorDtor")) {
            out.ehVectorDtor = resolved;
        } else if (is("BuffLibraryIndex")) {
            out.buffLibraryIndex = resolved;
        } else if (is("EffectLookup")) {
            out.effectLookup = resolved;
        } else if (is("ApplyEffect")) {
            out.applyEffect = resolved;
        } else if (is("PrngFloat")) {
            out.prngFloat = resolved;
        } else if (is("FreeU32Vector")) {
            out.freeU32Vector = resolved;
        } else if (is("ParseEffectFile")) {
            out.parseEffectFile = resolved;
        } else if (is("GamePrng")) {
            out.gamePrng = resolved;
        } else if (is("EffectMap")) {
            out.effectMap = resolved;
        } else if (is("AltRosterFlag")) {
            out.altRosterFlag = static_cast<const std::uint8_t*>(resolved);
        } else if (is("UpdateBuffsInternal")) {
            out.updateBuffsInternal = resolved;
        } else if (is("OnStartTownVisit")) {
            out.onStartTownVisit = resolved;
        } else if (is("BuffBurnDecay")) {
            out.buffBurnDecay = resolved;
        } else if (is("BuffRemoveByGuid")) {
            out.buffRemoveByGuid = resolved;
        } else if (is("ActorResetStats")) {
            out.actorResetStats = resolved;
        } else if (is("InventoryBuffRemove")) {
            out.inventoryBuffRemove = resolved;
        } else if (is("SkillInstantBuffs")) {
            out.skillInstantBuffs = resolved;
        } else if (is("UpdateTrinketBuffs")) {
            out.updateTrinketBuffs = resolved;
        } else if (is("StorageQueueWrite")) {
            out.storageQueueWrite = static_cast<std::uint8_t*>(resolved);
        } else if (is("StorageReadFile")) {
            out.storageReadFile = static_cast<std::uint8_t*>(resolved);
        } else if (is("SaveSlotBackup")) {
            out.saveSlotBackup = static_cast<std::uint8_t*>(resolved);
        } else if (is("AppShutdown")) {
            out.appShutdown = static_cast<std::uint8_t*>(resolved);
        } else if (is("StorageManager")) {
            out.storageManager = static_cast<void* const volatile*>(resolved);
        }
    }

    // Query 类名表：候选里选表项全部以 Query 结尾的那张，项数就是类型数（和 check::ResolveSignatures 的选择同义，
    // 那边只给布局依据和对照测试用；这里只有一个候选时也核对内容）
    static_assert(layout::kQueryClassCapacity == 64 && layout::kClassNameStride == 0x80,
        "layout_check.cpp 的 SelectQueryClassNames 按 64 项、0x80 字节一项选表，改值时同步改那里");
    for (const auto* candidate : queryNameCandidates) {
        const char* table = reinterpret_cast<const char*>(candidate);
        std::size_t count = 0;
        bool allQuery = true;
        for (std::size_t i = 0; i < layout::kQueryClassCapacity; ++i) {
            const char* name = table + i * layout::kClassNameStride;
            const std::size_t length = strnlen(name, layout::kClassNameStride);
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

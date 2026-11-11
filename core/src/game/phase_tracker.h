#pragma once
// 从状态机取样，判定 AssetsReady / CampaignLoad / CampaignUnload。
//
// 依据见 knowledge/dd1/runtime.md 的「状态机」一节：
//   - 状态切换全部在 UpdateStateInternal 里进行，只有一个 next 和 last 变量；
//   - 读档线程（load_campaign）运行时状态一直是 as_load_campaign，
//     线程结束后才在主线程离开这个状态，此时 Buff/Effect 库和存档都已经就绪；
//   - as_unload_campaign 的处理器在 UpdateStateInternal 里同步销毁 Campaign。
// 取样点是 UpdateStateInternal 入口，所以能在处理器运行之前看到 next。
// 纯逻辑，不访问游戏内存，单元测试直接覆盖。

#include <cstdint>

namespace ddc {

struct StateIds {
    int bootAssets = -1;
    int loadCampaign = -1;
    int unloadCampaign = -1;
    int raidStart = -1;       // as_raid_start，load_raid 线程运行时停留在这里
    int townVisitStart = -1;  // as_town_visit_start，load_town 线程运行时停留在这里
};

struct GameSample {
    const void* campaign = nullptr;
    int nextState = -1;
    int lastState = -1;
};

// 当前战斗的取样（game::ReadBattleSample 读游戏内存，这里只做判定）
struct BattleSample {
    bool raid = false;              // Raid::Current 非空且读得到
    std::int32_t state = 0;         // Battle+0x288（bs_none = 0）
    std::uint32_t guidCounter = 0;  // Battle+0x2D4：本场已分配的怪物 guid 计数，Battle::Create 清零
    std::uint32_t monsters = 0;     // Battle+0x38 的元素个数（结构不合法时按 0）
};

struct PhaseEvents {
    bool assetsReady = false;
    bool campaignLoad = false;
    bool campaignUnload = false;
    bool loadWasFallback = false;    // 走了兜底路径（没有看到离开 as_load_campaign 的那一帧）
    bool unloadAfterDestroy = false; // 销毁之后才发现（比如非常规路径）
};

class PhaseTracker
{
public:
    explicit PhaseTracker(StateIds ids) : ids_(ids) {}

    PhaseEvents Step(const GameSample& sample);

    bool CampaignActive() const { return active_; }
    std::uint32_t CampaignSerial() const { return serial_; }
    // 探索序号：取样看到 last 新进入 as_raid_start 时加一（load_raid 线程在这个状态里新建 Raid 对象，
    // 主线程至少停留一帧，见 knowledge/dd1/combat.md 的「Actor 的标识和反查」）。怪物句柄用它区分探索
    std::uint32_t RaidSerial() const { return raidSerial_; }

    // 战斗序号（审核 B-B1）：怪物 guid 只在一场战斗内唯一，怪物句柄要靠它区分同一次探索里的两场战斗。
    // 每帧取样和每次 actor 服务调用时各观察一次：
    //   - 看到「没有战斗」（没有 Raid，或者状态是 bs_none 且怪物表是空的）之后又看到「有战斗」（状态非零，或者怪物表不空）：
    //     新战斗，加一。Battle::Create 先清零 guid 计数（5A40F7）、经 5F7B20 填怪物表，到 5A4722 才置 bs_create，
    //     再派发 BattleCreatedEvent。flashback 战斗在中间经 5A45B2 的循环派发 heal.pre/heal.post（47FBD0；普通战斗不执行
    //     这个循环，前提见 knowledge/dd1/combat.md），这些回调里的服务调用看到的是「状态 0、怪物表不空」，在这里就拿到
    //     新序号（第二轮审核 R2B-备注1），之后看到 bs_create 不再加一。
    //     战斗销毁（5A5720）先清空怪物表再置 bs_none，战斗之间至少隔几帧。撤退途中 5A93B3 先置 bs_none、怪物表还在，
    //     不算「没有战斗」，免得同一场战斗里加一。
    //   - guid 计数比上一次看到的小，不管状态都算新战斗（同一场战斗里计数只增加）。漏看了「没有战斗」时靠它兜底。
    // 盲区：销毁和下一次创建落在两次观察之间（同一帧里、中间没有服务调用），并且新战斗的 guid 计数在第一次观察时
    // 不小于上一场最后看到的值。静态上没有找到这样的路径（P3 第二、三轮审核评估过，见 docs/history/p3-review-findings.md）。
    // 已知的小缺口：Battle::Create 清零计数之后、填怪物表之前（5A40F7 到 5F7B20，期间没有找到事件派发）如果观察了两次，
    // 第二次会重新进入空闲，到 bs_create 再加一次；只是多加一，怪物句柄都还没创建，不影响正确性。
    // 返回这次观察是否加了一
    bool ObserveBattle(const BattleSample& sample);
    std::uint32_t BattleSerial() const { return battleSerial_; }

private:
    StateIds ids_;
    bool assetsDone_ = false;
    bool active_ = false;
    const void* tracked_ = nullptr;
    std::uint32_t serial_ = 0;
    std::uint32_t raidSerial_ = 0;
    int prevLast_ = -1;
    std::uint32_t battleSerial_ = 0;
    std::uint32_t battleRaid_ = 0;       // 上一次观察时的探索序号
    bool battleIdle_ = true;             // 上一次确认「没有战斗」之后还没看到新战斗
    std::uint32_t lastGuidCounter_ = 0;  // 上一次看到战斗时的 guid 计数
};

}  // namespace ddc

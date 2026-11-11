#include "phase_tracker.h"

namespace ddc {

PhaseEvents PhaseTracker::Step(const GameSample& s)
{
    PhaseEvents events;

    if (ids_.raidStart >= 0 && s.lastState == ids_.raidStart && prevLast_ != ids_.raidStart) {
        ++raidSerial_;
    }
    prevLast_ = s.lastState;

    if (!assetsDone_ && s.lastState == ids_.bootAssets && s.nextState != ids_.bootAssets) {
        assetsDone_ = true;
        events.assetsReady = true;
    }

    if (active_) {
        if (s.nextState == ids_.unloadCampaign) {
            // 待进入 as_unload_campaign，它的处理器还没运行，Campaign 仍然有效
            events.campaignUnload = true;
            active_ = false;
            tracked_ = nullptr;
        } else if (!s.campaign || s.campaign != tracked_) {
            // 非常规路径：指针已经变了。通知插件退出，但对象很可能已销毁
            events.campaignUnload = true;
            events.unloadAfterDestroy = true;
            active_ = false;
            tracked_ = nullptr;
        }
    }

    if (!active_ && s.campaign) {
        const bool leftLoad = s.lastState == ids_.loadCampaign && s.nextState != ids_.loadCampaign &&
            s.nextState != ids_.unloadCampaign;
        // 兜底：没有看到离开 as_load_campaign 的那一帧，但已明显处于存档内
        const bool pastLoad = s.lastState != ids_.loadCampaign && s.nextState != ids_.loadCampaign &&
            s.nextState != ids_.unloadCampaign && s.lastState != ids_.unloadCampaign;
        if (leftLoad || pastLoad) {
            active_ = true;
            tracked_ = s.campaign;
            ++serial_;
            events.campaignLoad = true;
            events.loadWasFallback = !leftLoad;
        }
    }

    return events;
}

bool PhaseTracker::ObserveBattle(const BattleSample& s)
{
    if (battleRaid_ != raidSerial_) {
        // 新的探索：新的 Raid 对象，上一个 Raid 的计数不再有意义
        battleRaid_ = raidSerial_;
        battleIdle_ = true;
        lastGuidCounter_ = 0;
    }
    if (!s.raid) {
        battleIdle_ = true;
        return false;
    }
    // 有战斗：状态非零，或者怪物表不空（Battle::Create 填好怪物表到 5A4722 置 bs_create 之间状态还是 0；
    // 撤退途中 5A93B3 先置 bs_none，怪物表还在）。两样都没有才是「没有战斗」
    const bool present = s.state != 0 || s.monsters != 0;
    // guid 计数只在 Battle::Create（5A40F7 清零）和战斗读档（5AD160 的 5AD1EA 从存档读入）时变小，同一场战斗里只增加
    // （5AD463 的 inc 是读档时给重复的怪物 guid 重新编号，只会变大，第三轮审核 R3B-备注3）：
    // 变小时不管状态都是新战斗（第二轮审核 R2B-备注1）
    const bool shrunk = s.guidCounter < lastGuidCounter_;
    const bool started = shrunk || (present && battleIdle_);
    // 加过一之后就不再空闲，之后看到 bs_create 不会再加一
    battleIdle_ = !present && !shrunk;
    lastGuidCounter_ = s.guidCounter;
    if (started) {
        ++battleSerial_;
    }
    return started;
}

}  // namespace ddc

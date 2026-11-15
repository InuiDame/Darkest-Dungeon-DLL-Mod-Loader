#pragma once
// 一个 Lua Mod 随存档保存的数据（P4c1）：dd.store 的 JSON 和这个 Mod 的脚本 Buff 状态，经 DDCore 的 persist 服务写进旁路
// （<游戏根目录>\DDModLoader\save\<账号>\<设备>\DDLua\<mod id>.ddsave）。分段带长度，JSON 原样存放（state_json 的规则），
// 不需要再解析一遍：
//   DDLUA-STORE 1
//   store <字节数>
//   <dd.store 的 JSON>
//   buff <kind> <actor id> <roster> <guid> <字节数> <buff id>
//   <状态的 JSON>
// kind 是 hero / monster；英雄按 hero id，怪物按战斗里的 guid（只在同一场战斗里有效，raid 核对通过后才恢复）。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddlua::store {

struct BuffState {
    std::string buffId;
    std::uint32_t kind = 0;    // DD_ACTOR_HERO / DD_ACTOR_MONSTER
    std::uint32_t id = 0;      // hero id 或怪物 guid
    std::uint32_t roster = 0;  // 英雄的名册（0 主名册）
    std::uint32_t guid = 0;    // 组 guid
    std::string json;
};

struct Payload {
    std::string store;  // dd.store 的 JSON（空串表示没有）
    std::vector<BuffState> buffs;
};

std::string Encode(const Payload& payload);
bool Decode(std::string_view bytes, Payload* out, std::string* why);

}  // namespace ddlua::store

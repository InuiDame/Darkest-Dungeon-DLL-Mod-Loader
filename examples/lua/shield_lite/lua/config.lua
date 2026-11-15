-- shield_lite 的配置。对应 DDShield 的 DDShield.ini [shield] 段、DDShield.effects.json 和英雄 info 里的 shield: 块。
-- 改完退回主菜单再进存档生效（每次进存档重新执行 main.lua）。
return {
  -- 没有职业配置时的护盾上限和初始值（DDShield 的 defaultMax、startFlat、startHpRatio、maxHpRatio）。
  -- 上限为 0 时，第一次获得护盾的量就是上限（DDShield 的规则）
  default_max = 0,
  start_flat = 0,
  start_hp_ratio = 0,
  max_hp_ratio = 0,

  -- 每回合开始时所有护盾固定增减（DDShield 的 regenPerRound、decayPerRound）
  regen_per_round = 0,
  decay_per_round = 0,

  -- 职业的护盾上限和初始值（英雄才有，键是职业 id）。DDShield 写在 heroes/<职业>/<职业>.info.darkest 的
  -- shield: .max 12 .start 0 里；Lua 版改成这张表，不读 info 文件
  classes = {
    crusader = { max = 12, start = 0 },
  },

  -- 名字里看不出规则的 effect（DDShield.effects.json 的写法）。名字能解析的（shield_gain_5 之类）不用写。
  -- op：add、addpct、max、maxpct、set、full、break、damage、damagepct、regen、decay、pierce
  specs = {
    shield_set_10 = { op = "set", value = 10 },
  },

  -- 有护盾时给角色加一条载体 Buff（tray 里能看到「有护盾」，护盾值随存档保存），护盾为 0 时删掉
  carrier = true,
  -- 每次获得、吸收、回合变化、清理都写一行 SHIELD 日志（JSON），检查脚本 tools/p4_scenario_check.py 逐次核对
  log = true,
  -- shielded 规则的判定日志：每个角色的结果变化时写一行，每场战斗最多这么多行
  rule_log_max = 40,
}

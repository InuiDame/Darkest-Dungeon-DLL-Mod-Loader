# ProtectionCapMod 版本记录

## 当前适配版本
- 游戏: Darkest Dungeon (Steam)
- 记录日期: 2026-10-24
- 游戏内显示的版本 / Steam build id: <填这里>
- Darkest.exe 大小: <字节数>
- Darkest.exe PE 时间戳: <0x????????>  ← 从 CFF Explorer / PEview 看

## 已验证有效的定位信息（v当前版本）
### 数据段 0.8f（4 处，全部替换为 cap）
- RVA 0x474304  (解析器内部)
- RVA 0x715ED8
- RVA 0xED7750  ← 主读取点，被 0x5BCFB9 / 0x5F90E9 引用
- RVA 0x1174CA0

### 数据段 80.0f（1 处）
- RVA 0xED78CC

### .text 立即数（防重置）
- RVA 0x26A3C0: `C7 05 ?? ?? ?? ?? CD CC 4C 3F`  (mov [rip+], 0.8f)

## 期望行为（回归验证基准）
- UI 悬停保护属性：显示 `cap * 100%`
- 30 点伤害怪打 95% PROT 英雄：飘字 `1`
- 10 点伤害怪打 95% PROT 英雄：飘字 `0`

## 已知非目标点（不要动）
- 0x4A1F6D 附近：这是 100.0f 的引用，与上限无关
- 0x5F90E9 函数：是"条件满足时三分量乘 0.8"的折扣逻辑，不是 PROT clamp
- 0x7D3719 附近：`str_stat_protection` 的 UI 引用，动这里会坏 tooltip

## 基线日志
- `baseline-inject.log`: 版本 <X> 下完整 DebugView 输出
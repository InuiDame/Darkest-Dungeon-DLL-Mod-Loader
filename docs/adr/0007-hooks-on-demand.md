# Hook 按需安装，优先虚表，内联补丁只在安全时机修改

`DDCore` 能用虚表 hook 就不碰函数体（如 `ApplyHealthDamageInternal` 的槽位 50）。语义 hook 按需安装：一个主题没有订阅者，对应的 hook 就不装，没有订阅时 hook 数为零（[0003](0003-thin-proxy-core-layering.md) 要求的原版行为）。

内联补丁的写入不是原子的，所以安装和拆除只在主线程进行（AppReady 或帧边界），并避开三条加载线程运行的状态（`as_load_campaign`、`as_raid_start`、`as_town_visit_start`）。插件通过 `hook` 服务改内联 hook 时受同样的限制，时机不对返回 `DD_HOOK_E_BAD_TIMING`。

目标入口已被别人改过、或入口指令无法安全搬移（rip 相对寻址、分支）时，拒绝安装并写日志。原加载器下两个 DLL hook 同一入口时，后来者的特征码匹配失败，功能静默丢失；这里绝不静默失败。

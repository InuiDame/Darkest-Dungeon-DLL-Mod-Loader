# 运行时定义：buff 只开放 POD 字段，effect 用游戏的解析器并先预检，每次进存档重新注册

- **buff 定义**经 Buff 库 map 的 `operator[]`（`4ACFA0`）原生插入，拿到默认构造的 Instance 后只写 POD 字段（stat、sub_stat、amount、duration、duration_type、source、sourcedata、规则类型、rule_data、is_false_rule、remove_if_not_active 等），不开放附加效果、`force_positive` 这类只在定义里的字段。读档时游戏查不到定义只丢这些字段（`4A9FA0`），英雄的读档又早于 Lua 注册定义，所以只开放 POD 字段时载体读档后不缺东西。`sub_stat` 按 stat 查游戏的名字表校验：认不出时游戏有的在显示提示时弹 assert，有的越界加到相邻的累加值上。
- **effect 定义**：游戏没有从内存解析的入口，DDCore 把文本写成 `DDModLoader\tmp\` 下的临时文件，把相对游戏根目录的路径交给 `4F02D0`，用完删除。交出去之前按游戏解析器的规则检查文本（`2901B0` 去注释、`2904D0` 分块、`36F020` 取字段）：块从「标识符:」开始，到下一个冒号前面的标识符为止，每块最多 1023 字节（`2904D0` 用 `strncpy_s(…, 0x400, 块, 块长)` 复制，块长到 0x400 时 CRT 直接终止进程）；块都必须是 `effect:` 块；`.name` 按游戏的取法（块里最后一次出现）；`.target` 必须是游戏认的 21 种之一（缺少时弹 assert）；列表项不能从块尾的引号开始（同样让 `strncpy_s` 终止进程）；零长度的块（解析器死循环）、0 字节、判断落在高位字节上的（结果依赖区域设置）都拒绝。
- **归属**：Buff 库和 effect map 每次进存档重建（effect map 在退出存档时由 `4F3A30` 清空），定义在每次进存档时重新注册，DDLua 的 `main.lua` 天然如此。`CampaignLoad` 时已经在表里的 id、名字都算游戏数据，不能覆盖；之后插件定义的按名字的哈希记归属，同一插件重复定义时更新，别的插件拒绝。DDLua 对 DDCore 是一个插件，它再按 Mod 记归属，哈希相同的不同 id 也算重复。
- 运行时定义的 effect 能被 buff 的附加效果、effect 的 `buff_ids`、Lua 使用，**不能被技能引用**：技能在职业加载时就把名字解析成 `Effect*`，早于 `CampaignLoad`。给技能用的 effect 写在 Mod 自己的 `effects/*.effects.darkest` 里由游戏加载（`examples/lua/shield_lite` 就是这样）。

## 考虑过的方案

- buff 定义写成临时 `.buffs.json` 交给 `4AA770`：能带定义专有字段，但读档时游戏照样查不到（Buff 库在 `CampaignLoad` 之前就加载完了），还多一次文件读写。
- 在 `load_campaign` 线程上注入（`4AA580`、`4F0100` 返回后，职业加载之前）：读档时能恢复定义专有字段，effect 也能被技能引用，探针证实 `4AA580` 出口插入的定义到 `CampaignLoad` 时还在。但那时 Lua 还没启动，只能跑原生代码，Lua Mod 用不上。
- 读档后由 DDCore 把定义专有字段补到已有实例上：只有开放这些字段时才需要。
- 只限制整段文本的长度（第一版是 64 KB）：一个块到 1024 字节就让游戏退出（P4 审核 A-B1）。
- 拒绝重新定义带列表字段的 effect：会挡住只改标量字段的正常用法。

## 后果

- Lua 拿不到附加效果、`force_positive` 等字段，要这些字段的 buff 写进数据层。
- 重新定义自己的 effect 时游戏复用原对象：标量字段被覆盖，列表字段（`.buff_ids`、`.kill_enemy_types`、`.summon_monsters`、`.set_monster_class_id(s)`）在后面追加、不清空，要改列表就换名字。
- 运行时定义没有个数上限，每条都在游戏里常驻到退出存档；按 Mod 的上限留到 P7 的 API 整理。
- 预检是按反汇编复刻的规则。第二轮审核用独立复刻的解析器做过约 300 万段文本的差分，原版和 Mod 的 26787 个真实块没有误拒；游戏改了解析器要重新核对。

2026-11-15 敲定（P4，经四轮审核）。

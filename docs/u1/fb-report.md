# U1 修复 FB 报告：Python 工具、自测、文档

阶段工作文件，用户验收后删除。修复简报见 [fix-brief.md](fix-brief.md)「FB」一节，审核报告见 [review-a.md](review-a.md)，FA 的结果见 [fa-report.md](fa-report.md)。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1fb\`（下文的文件名都相对这个目录），用到的一次性脚本存在 `scripts/`。审核的每条发现都先在 guest 上复核再改；没 commit，没碰真实安装和测试目录，没启动游戏。

## 1. 结论

| 审核编号 | 处理 | 证据 |
| --- | --- | --- |
| B1 位置没法对照按通过 | verify：依据成立、位置没法对照、同一项又没有别的「成立、位置对上」的依据时记为**无法确认**（新类别 `unconfirmed`），退出码 2；例外只有「已知版本用自己的基线」。未知版本放一份自己 SHA1 的基线进来不算自己的基线（否则同一性和位置核对都是自己和自己比）。repair：位置没法对照的依据不当作成立，重新定位要有指令对齐的确认，放宽的模式唯一命中但对齐有歧义也算「位置无法确认」；同一项没有位置确认过的依据佐证时报需要人工、原因进 `report.txt`；预演 verify 的无法确认也改成需要人工。accept：无法确认的项默认拒绝，只能在确认清单里逐项人工确认（理由见第 6 节）。指令对齐改成以全局序列对齐为准、分析插入删除的滑动歧义（`align.py`），但这只减少本来没有歧义的「没法对照」，不替代上面的规则 | `selftest_final.txt` 的「规则自检」：审核时的宽松依据配 `inst44.exe`，verify 退出码 2、`InstSubHash`、`InstAmount`、`InstSource`、`InstSourceData` 无法确认（原来判 OK 0x44），repair 四项需要人工、没有修错，accept 拒绝；变异样本 `inst44`、`inst_remove44`（第 3 节） |
| B2 `--allow-optional` 放行离线失效 | accept 重写（`repair.py` 的 `accept_gate`）：必需项失效、误绑定、只有 verify 做的特征码静态检查不过、离线依据不过一律拒绝，`--allow-optional` 和 `--confirm` 都不管用；`--allow-optional` 只放行运行时自己会发现的失效（可选特征码的失效原因全是 `resolve`/`section`/`entry`、可选 RUNTIME 项只有运行时依据不成立）；拒绝信息只在这种情况才提 `--allow-optional` | `replay/replay_accept.txt`：`enum.exe`、审核的 `inst44.exe`、`inst_remove44.exe` 带不带 `--allow-optional` 都拒绝（退出码 2，副本的 `signatures.inc` 没变、没生成基线）；规则自检里 `hook_entry.exe`（只有运行时会发现的失效）不带 `--allow-optional` 拒绝、带了以后要 `--confirm` |
| S4 组、升级、显式确认 | `layout.inc` 加只给 Python 用的 `DD_GROUP(组名, "说明", 成员...)`（C++ 展开成空），44 个组（结构 15 个、函数和原型 29 个）；基线存清单项的值（`items`）。verify 列出「同组有变化」（不改退出码）；repair 把同组的 NONE/OFFLINE 项改成需要人工；accept 要求全部 NONE 项、同组升级的项、无法确认的项在 `--confirm` 清单里逐项写 `method` 和 `evidence`（路径要存在，sha1 要对），`--confirm-template` 生成清单，接受后清单存进 `tools/baseline/confirm/<版本名>.json` | 往返：不带 `--confirm` 拒绝、副本不变；模板 172 项（NONE 全部在内），带同组原因的恰好是 Instance 组、GroupEntry 组和 `ProtoBuffGroupAdd` 的 7 项（断言是相等，不是包含）；填好后接受、确认记录写入。手册演练 `manual/manual_run.txt` |
| S5 对照测试的 static_only | `SigResult` 每条问题带类别；`cpp_compare.py` 只在失效原因**全部**是 `static` 时单列，原因里有运行时也做的检查而 C++ 判成立就算不一致；适配 FA 的 HOOK 入口检查（类别 `entry`，两边都做）；加了 `self_check()`（6 个构造的用例），selftest 的规则自检先跑它 | 故意改坏实验（第 4 节）：去掉 C++ 的 .data/.rdata 检查后 `statelast_text.exe` 报「不一致」退出码 1，正常构建退出码 0 |
| S6 GLOBAL_ANY 没有同一性 | 候选数和基线不同判误绑定；基线存每条 GLOBAL_ANY 命中函数的代表（`hit_reps`，按相似度贪心聚类），每个命中所在的函数都要像某个代表，否则误绑定；命中次数可以变（Steam 版 69、离线版 68），只记一笔 | verify 两个版本 `EventTypeCount` 等三条的最低相似度 1.000；`st_gate2.txt`：代表换成 `EventClassNames` 的判「都不像（最高 0.107）」，基线候选数改成 2 判「候选数不同」 |
| S7 样本迁就实现 | 样本重做：`struct_shift` 加新字段的复制和构造函数里的清零；`evidence_misbind` 加两个新字段的解析代码（新键名放进 `.rdata`）；吸收审核 B1 的两个样本（`inst44`、`inst_remove44`，构造函数也加了新字段的清零）；加 `twin_callers`（两份都有调用者）。函数原地变长用新写的 `pepatch.grow_in_place`（后面的函数依次后移，含没有 .pdata 的叶函数、链式 unwind 的主函数副本） | 第 3 节；13 个样本全部通过，修错 0 |
| 备注 2（Python 侧） | `sigs.py` 写明 `sig.required in ("REQUIRED", "LOADER")`，C++ 的注释跟着改；规则自检构造一条 LOADER 的 GLOBAL 指向 `.text`，判失效（类别 `section`） | 规则自检 |
| 备注 3 | `evidence.read_value` 读不满宽度时返回 None，和 C++ 一样判「超出映像」 | 代码；对照测试 25 个 exe 一致 |
| 备注 4（verify 侧） | `check_hook` 加「偷取不到 5 字节」（类别 `static`），清单检查也查 | 规则自检：偷取 3 字节时清单检查和 verify 都报 |
| 备注 5 | `effect_field_types` 改成按解析时的键名核对字段身份（`.chance`、`.dotPoison`……，读取函数按 float/int 分组）；`effect_misc_fields` 要求写入指令前面最近的键名是对应的键名；`skill_effects_offset` 要求偏移处是一个 vector 的 push_back（同一个基址寄存器的 cmp/add/lea）；`raid_battle_lea` 要求在读 `Raid::Current` 的函数里。`instance_copy_fields`、`hero_stress_slot` 仍是「存在」检查，写明弱点（它们挂的项另有运行时依据），没降成 NONE | Steam 版、离线版 verify 全部通过；`evidence_misbind` 里旧偏移 0x134 现在是 `.dotChill` 的解析，`EffectDotPoison` 判失效（原来会误判通过，审核 S7 的①） |
| 备注 6 | verify 在只要布局（`--only layout`）时也做依据位置核对，`check_actor_layout.py` 的封装因此包含它；`knowledge/README.md` 改成两个旧名字各对应哪一部分 | `check_actor_layout.txt`：依据位置 100 对上；两个封装退出码 0 |
| 备注 7 | `08-handoff.md` 按现在的停用开关重写：`hook_entry` 停 `damage.*` 的可写主题、`evidence_misbind` 停 `combat.effect_get` 和 `effect.*`、`vslot` 停 `game.actor_info` 和 `damage.*`/`heal.*`/`effect.*`/`damage.final` | `selftest_final/compare/compare.txt` |
| 备注 8、10 | 数据不足，在 ADR 0035「局限」和 08「仍未验证」如实写：阈值只有同源两个构建的数据，最近冒充者 3 条高于阈值，盲定位错误 0 不能证明真实更新上的区分力 | 文档 |

## 2. 4 处断言变化的核实

每处都先查了样本里这些字段的真值（样本的改法是确定的，`mutants.py` 的 truth 和 selftest 的不变式也逐项对过），再改断言；改的理由写进了 `mutants.py`、`mcheck.py` 的注释。

1. **`vslot` 多出 `VtSlotHeal`（optional）**：样本在第 38 槽插入新函数、后面整体后移一槽，`VtSlotHeal` 的真值是 52，清单里的 51 是错的。FA 加的依据 1（槽 51 的函数里有 `hp += min(治疗量, 最大生命 - hp)` 的指令串）在现在的槽 51（原来的伤害函数）里命中 0 次。这是真阳性。原来只有位置核对的误绑定（依据 0）能发现它，也报出来了，所以不是漏报；现在运行时也能发现。repair 照样按依据搜索到 52（修复项等于真值）。
2. **`evidence_misbind` 的 `EffectDotBurn` 从 misbind 变成 optional，退出码 2 → 1**：样本在 0x134 之前插入 8 字节，`EffectDotBurn` 真值 0x144，清单里的 0x13C 是错的。FA 收紧后依据带前后两个字段的字面量（0x1BC、0x140），两个都挪了，模式命中 0 次，判不成立。真阳性；原来靠位置核对判误绑定也报出来了，不是漏报。FA 跑的时候 repair 对 `EffectDotBurn`、`EffectDotStress` 报需要人工；现在新样本里新字段的解析带着新键名，指令对齐没有歧义，放宽模式按对齐选中，修到 0x144、0x1C0（等于真值），新模式保住原来前后两个字段的范围。
3. **`mcheck` 的 `EffectDotBurn` 从误绑定变成依据不成立**：这是清单变异（游戏没变，nosteam 上 `EffectDotBurn` 真值仍是 0x13C）。原来的改法把「下一个字段 0x140」改成 0x148，靠这一个字面量区分时模式会唯一命中别处；收紧后模式里另一个字面量（0x1BC）还在，命中 0 次。repair 报需要人工、值没改，没有修错，行为正确。原来判误绑定是对的（不是漏报），只是这个改法不再能造出误绑定。为了保留「改坏的清单上位置核对抓住误绑定」这一项，`MUST_MISBIND` 换成 `EffectDotStress`、两个字面量一起改（前一个改成 0x1B8、后一个改成 0x13C，模式唯一命中 `.clearDotStress` 的解析）：verify 判误绑定，repair 按对齐修回原模式，值不变。原来那处保留为「改坏的模式判不成立、修复不能修错」。
4. **`struct_shift` 多出 `InstRemoveInactive`、`InstRemoveBattle`（optional）**：插入点 0xB0，这两项的值 0xAC、0xAD 不变（真值不变）。FA 收紧的依据带界标（+0xB0 的 dword 复制）；新样本里新字段的复制（`mov rax,[rdx+0B0h]`）正好插在界标处，rule_float 的复制挪到 +0xB8，界标对不上，依据判不成立。这是**假阳性**（值没变却判失效），方向安全（停用、不会读错），是收紧的代价（ADR 0036「局限」）；原来判 OK 是对的，不是漏报。repair 按指令对齐找回原来的复制（新字段是 64 位复制，和相邻的 8/32 位复制形状不同，对齐没有歧义），值不变，新模式保住原来的范围、界标换成新字段的复制。

## 3. 变异样本

13 个，`selftest_final.txt` 全部通过，修错 0（不变式：修复项的新值等于真值，verify 判通过、repair 判 ok 的项也要等于真值，新版本里不存在的字段不能给出任何值）。新加和重做的：

| 样本 | 模拟的真实变化 | verify 的断言 | repair 的断言 |
| --- | --- | --- | --- |
| `struct_shift`（重做） | `Buff::Instance` 在 rule_float（0xB0）之前插入 8 字节的字段：复制构造多一对 64 位复制，构造函数多一条清零（S7）；其余同 U1d（访问后移 8、Instance 0x418、Entry 0x420、除法魔数、栈副本、map 节点） | 退出码 1；14 项 optional（原来 12 项加 `InstRemoveInactive`、`InstRemoveBattle`，见第 2 节第 4 条） | 退出码 1；14 项修好（值等于真值，`InstRemoveInactive`/`Battle` 值不变、模式重新生成）；`InstStat`、`InstScalarSize`、`InstFlagSize`、`GameTextCap`、`BuffEntryGuid`、`BuffGuidSize`、`ProtoBuffGroupAdd` 需要人工（S4） |
| `inst44`（审核 B1 反例 1） | sub_type 后面插入一个 int：[0x44, 0x152) 后移 4，尾部填充吸收，大小不变；复制构造的一串同形状 int 复制里多一对，构造函数多一条清零 | 退出码 1；`BuffEntryInstance` 和 16 个挪动的 Instance 字段 optional（收紧后的依据判不成立，原来判 OK） | 退出码 1；12 项修对（BuffRuleActive、构造、BuffDurationUpkeep 里只是位移变了的，和隔着别的形状复制的）；只靠一串 int 复制的 `InstSubHash`、`InstAmount`、`InstSource`、`InstSourceData` 需要人工（放宽模式唯一命中，但对齐有歧义，B1）；Instance 组 4 项需要人工 |
| `inst_remove44`（审核 B1 反例 2） | 删掉 sub_hash（0x44 的 int）：[0x48, 0x152) 前移 4；复制构造少一对 | 同 inst44 | 同 inst44；`InstSubHash` 的真值是「不存在」，报需要人工（放宽模式不命中、这条指令不在匹配块里） |
| `evidence_misbind`（重做） | Effect 在 0x134 之前插入两个 int：解析函数多两段同形状的解析（新键名 `.dotChill`、`.dotChillDuration` 在 `.rdata`），后面 14 个函数依次后移（含 `EffectLookup`，真值记了新位置） | 退出码 1；20 项 optional（`EffectDotBurn` 从 misbind 变成 optional，见第 2 节第 2 条） | 退出码 1；7 项修好；OFFLINE 项和 `EffectSwap` 需要人工；Effect 组 5 个 NONE 项需要人工 |
| `twin_callers`（新） | `EffectLookup` 多一份完全相同的副本，27 处调用按地址隔一处改调副本（14、13 处，10、11 个函数） | 退出码 1；`EffectLookup`、`SkillEffects` optional | 退出码 1；两项需要人工（指纹、调用的目标相同，不能挑一个） |

其余 8 个样本（`relocate`、`relocate_sched`、`frame`、`twin`、`decoy`、`enum`、`hook_entry`、`vslot`）的改法没动，repair 的预期按 S4 加了同组升级的项（`relocate_sched` 多 `ProtoQueryListenerAdd`，`hook_entry` 多 `ProtoConsumeHealthDamageBlock` 和虚表槽组的 6 个原型项，`vslot` 多这 6 个和 `ActorFloatSize`、`ActorIdSize`）。

样本做不到真实的地方（写在 `mutants.py` 的 unreal 里）：函数原地变长后，后面的函数不再 16 字节对齐（真实的重新链接会保持对齐、后面所有函数都挪）；C++ 异常表（FuncInfo）里的 IP 到状态映射没改（工具只读 .pdata 和 UNWIND_INFO，样本不运行）；只改了清单依据涉及的函数。

断言能在实现出错时失败：样本表写的是精确的集合（verify 的失效项和类别、repair 每项的状态、两个退出码），不是包含关系；往返里同组升级的项也是精确相等；修错由真值逐项核对。开发中实际抓到过的问题：确认模板的分组判断写宽了（把说明里带「组」字的 NONE 项也算成同组升级），往返的相等断言报出来了，已改。

## 4. S5 的故意改坏实验（复现）

```
python -B fbtmp/s5_prepare.py          # 现存 build/u1fb/scripts/s5_prepare.py：源码复制到 build/u1fb/s5tree，
                                       # layout_check.cpp 的「if (def.need == SigNeed::Required || def.need == SigNeed::Loader)」改成 if (false)
cd DD.ModLoader/build/u1fb/s5tree && xmake f -P . -p windows -a x64 -m releasedbg -y && xmake build -P . dd_layout_compare
python -B DD.ModLoader/tools/ddupdate/cpp_compare.py DD.ModLoader/build/u1review/statelast_text.exe \
    --bin DD.ModLoader/build/u1fb/s5tree/build/windows/x64/releasedbg/dd_layout_compare.exe --out DD.ModLoader/build/u1fb/s5_broken
  不一致 1 处：特征码 StateLast：Python 失效、C++ 成立，但失效原因里有运行时也做的检查：
    解析出的全局 3DCE85 不在 .data/.rdata（.text）（section）        → 退出码 1（s5_broken.txt）
python -B DD.ModLoader/tools/ddupdate/cpp_compare.py DD.ModLoader/build/u1review/statelast_text.exe --out DD.ModLoader/build/u1fb/s5_ok
  结论：一致 → 退出码 0（s5_ok.txt）
```

改坏的副本只在 `build/u1fb/s5tree`，仓库的 C++ 没动。分类规则本身在 selftest 里由 `cpp_compare.self_check()` 覆盖（不需要构建）。

## 5. 收尾证据

- `python -B DD.ModLoader/tools/dd_update.py selftest --out DD.ModLoader/build/u1fb/selftest_final`：退出码 0，6 步 2183 秒（`selftest_final.txt`、`summary.json`）。盲定位两个方向特征码 62 条正确 62、错误 0，函数级错误 0（需要人工 1、2）；变异样本 13 个通过、修错 0；清单变异冒烟修错 0、误绑定抓到 `EffectDotStress`；往返（apply 后 verify 0、副本编译 DDCore、accept 的拒绝和确认清单）通过；规则自检通过；C++/Python 对照 25 个 exe 全部一致。之后给规则自检加了 S6 的用例，单独重跑 `selftest --only gate`（`st_gate2.txt`）退出码 0。
- verify（`verify_final.txt`）：Steam 版、离线版退出码 0，依据位置 100 对上、0 对不上、0 没法对照。两个旧封装：`check_sigs.py`、`check_actor_layout.py` 退出码 0，后者也做了依据位置核对。SDK 的插件作者步骤（`templates/native/README.md` 的 verify `--sigs`、baseline `--sigs --out`、带 `--baseline` 的 verify）照常（`sdk/`）。
- 基线（`tools/baseline/steam.json`、`nosteam.json`）按新格式（加 `items`、`hit_reps`）重新生成，两次生成 sha256 相同（`steam.json` `df342d26…`、`nosteam.json` `3795225b…`，`baseline_run2/`）。
- 审核 B1、B2 的重放（`replay/`）：审核的 `inst44.exe`、`inst_remove44.exe` 上 verify 退出码 1（`InstSubHash` 等判 WARN，不再是 OK 0x44；原来退出码 0），accept 带不带 `--allow-optional` 都拒绝（`InstDuration`、`InstDurationType` 的离线检查不过）；`enum.exe` 带不带 `--allow-optional` 都拒绝。审核时的宽松依据（FA 收紧之前）配 `inst44.exe`：verify 无法确认、退出码 2，repair 四项需要人工、没有修错，accept 拒绝（规则自检）。
- 手册演练（`manual/manual_run.txt`，副本 `manual/tree`，样本 `inst44.exe`、宽松依据）：第 2 步 verify 出现 4 条 `UNKN`、退出码 2；第 3 步 repair 对这 4 项报「位置无法确认」，其余 12 项修对；第 4 步 `--apply` 后仍是 4 条 `UNKN`；按复制构造的反汇编（`manual/inst44_copyctor.txt`）人工改值和字面量后 verify 依据全部成立，位置仍是 `UNKN`（按指令对不齐，和手册写的一致）；第 7 步不带 `--confirm` 拒绝（176 项要确认：NONE 172、同组 4、无法确认 4），`--confirm-template` 生成清单，证据路径写错一项时拒绝，填好后接受，`DD_KNOWN_BUILD`、基线、确认记录都写进副本，再 verify 是已知版本、退出码 0。
- C++：只碰了 `layout.inc`（`DD_GROUP` 的默认空定义和组）、`layout.h`（`#undef DD_GROUP`、一行注释）、`layout_check.cpp`（备注 2 那段注释）。`xmake build -r DDCore`、`dd_core_tests`、`dd_layout_compare` 都 build ok、没有警告（`build_*.log`），`xmake run dd_core_tests` 1272 项失败 0（`core_tests.txt`）。**没有重跑测试目录自检**：C++ 的改动是展开成空的宏和注释，不改任何常量、依据表和停用逻辑；对照测试 25 个 exe 一致说明 C++ 的依据表和 Python 读的清单逐项相同；测试目录里还有一个不是我们放的 `SpineFix.dll`、guest 上可能有别人在并行工作，简报要求不动测试目录。

## 6. 设计上的取舍（请重新审核时看）

- **无法确认可以人工确认**：修复简报写的是「accept 在有这类项时拒绝」。默认确实拒绝；但我让确认清单可以覆盖它（每项要写核实方式和存在的证据路径）。原因：一串同形状字段里插入、删除时按指令本来就对不齐，人工把值和模式改对之后，位置核对拿旧基线比照样「没法对照」（手册演练就是这样）；新写的依据在旧基线里也没有参照。一律拒绝的话，这种最常见的结构变化永远 accept 不了。这和 NONE 项的处理一致：核对不了的靠人工核实、留下证据。如果主线认为必须一律拒绝，改 `accept_gate` 一处即可，但要给出别的出路。
- **同组升级不改 verify 的退出码**：apply 之后值本来就和基线不同，计入退出码的话手册第 4 步「verify 退出码 0」永远做不到；由 repair 报需要人工、accept 强制确认。
- **repair 的放宽模式唯一命中、对齐有歧义也算位置无法确认**：审核的反例里放宽模式恰好命中了对的字段，但在要读的字段之后插入时它会命中后一个字段，工具分不出这两种情况。
- **重新生成依据模式时保住原范围**：FA 跑的时候 repair 把收紧过的依据重新生成成最短模式（例如 `InstRemoveInactive` 只剩两对复制、丢了界标），下一次更新又会回到 B1 的形状。现在先按原模式的范围生成，保不住才退回最短模式并在报告里写明。
- **组的划分**：第一版把特征码放进结构的组（例如 listener 组里放了 `ListenerAdd`、`QueryListenerAdd`），`QueryListenerAdd` 搬迁时整个组的原型、RTTI 都升级成需要人工，范围太宽；改成结构的组只用 RUNTIME 项当触发条件，函数和原型单独成组。

## 7. 文档改动

- `docs/adr/0035-update-pipeline.md`：重写校验（无法确认、对齐的滑动分析、GLOBAL_ANY、问题类别、离线检查的位置约束、同组升级）、修复（位置无法确认、保住原范围、同组升级）、新增「确认新版本」一节（accept 的规则表和理由）、自测、考虑过的方案、局限（备注 8、10、5 的弱点、每次 accept 的确认成本）。
- `docs/adr/0036-runtime-layout-check.md`：停用规则表里「已知版本 → 照常」的前提改成新的 accept 规则（只改这一格，其余是 FA 的）。
- `docs/06-workflow.md`：工具表（verify 的退出码和无法确认、accept 的新参数、selftest 的步骤和耗时、对照测试的分类）；「游戏更新后」第 2 步（`BIND`/`UNKN`/`--`/`GRP` 怎么读）、第 3 步（「位置无法确认」「同组 … 变了」怎么处理）、第 4 步（只有人工核实过的 `UNKN` 可以留下）、第 7 步（生成、填写确认清单，accept，提交确认记录）。
- `docs/08-handoff.md`：当前状态（审核 A、FA、FB、要重新审核）、需要用户决定的事（未知版本的代价：150 个主题只剩 4 个；A 的两个例外；accept 的人工确认；原来的「persist 算只读」作废）、备注 7 的三个样本按实际开关重写、仍未验证（备注 8、10）、遗留整理项。
- `docs/04-roadmap.md`：U1 的状态和「没法确认不算通过」。
- `docs/02-architecture.md`：离线校验和修复那一条补上无法确认、同组升级和 accept 的规则。
- `knowledge/README.md`：两个旧脚本名各对应 verify 的哪一部分（备注 6）。

## 8. 改动文件

- `tools/ddupdate/verify.py`：无法确认（`mark_unconfirmed`）、`own_baselines`、GLOBAL_ANY 的同一性（`_global_any`）、同组升级（`group_flags`）、`--only layout` 也做位置核对、报告和 JSON 的新字段
- `tools/ddupdate/align.py`：对齐以全局序列对齐为准、插入删除的滑动歧义（`Aligner.ambiguous`）、匹配块够长时不要求唯一窗口
- `tools/ddupdate/layoutfix.py`：位置无法确认、佐证规则、重新定位要对齐确认、`to_json` 处理没生成新模式的项
- `tools/ddupdate/regen.py`：依据模式先保住原范围
- `tools/ddupdate/repair.py`：挑基线传已知版本、预演的无法确认改需要人工、同组升级（`group_upgrade`）、accept 重写（`accept_gate`、确认清单、模板、确认记录）
- `tools/ddupdate/sigs.py`：问题类别（`kinds`、`runtime_detectable`）、LOADER GLOBAL、偷取不少于 5
- `tools/ddupdate/manifest.py`：`DD_GROUP` 的解析和检查、HOOK 偷取不少于 5 的清单检查
- `tools/ddupdate/baseline.py`：`items`、`hit_reps`
- `tools/ddupdate/evidence.py`：读不满宽度时判超出映像（备注 3）
- `tools/ddupdate/offline.py`：备注 5 的位置约束
- `tools/ddupdate/cpp_compare.py`：按类别分类、`self_check`、`--self-check`
- `tools/ddupdate/pepatch.py`：`grow_in_place`（含空隙里的叶函数、链式 unwind 的主函数副本）、`assemble` 的 `translate`、`References.rel_sites_into`
- `tools/ddupdate/mutants.py`：`inst44`、`inst_remove44`、`twin_callers`，重做 `struct_shift`、`evidence_misbind`，样本表的预期和改动理由，`ABSENT`
- `tools/ddupdate/mcheck.py`：误绑定样本换成 `EffectDotStress`
- `tools/ddupdate/selftest.py`：真值 `ABSENT`、往返的 accept 和确认清单、新步骤 `gate`
- `tools/check_actor_layout.py`：说明
- `tools/baseline/steam.json`、`nosteam.json`：重新生成
- `core/src/game/layout.inc`：`DD_GROUP` 的格式说明、默认空定义、44 个组
- `core/src/game/layout.h`：`#undef DD_GROUP`、注释
- `core/src/game/layout_check.cpp`：备注 2 那段注释
- 文档：见第 7 节；`docs/u1/fb-report.md`（本文件）

## 9. 没做到的事、注意事项

- **S6 没有 exe 级的反例样本**：没有构造「别的模板族恰好匹配 `EventTypeCount` 的模式」的变异 exe，只在规则自检里换基线的代表、候选数验证了判定。
- **备注 5**：`instance_copy_fields`、`hero_stress_slot` 仍是存在检查（写明弱点，挂的项另有运行时依据）；`raid_battle_lea` 加了函数约束，结构里插入时旧偏移落到别的成员上仍可能通过，它挂的 `RaidBattle` 另有运行时依据和 `party_monster_targets`。
- **verify 的 B1 规则在正式清单的样本上触发不到**：FA 收紧后，结构插入时依据直接判不成立，不会到「成立但没法对照」这一步；所以 B1 的 verify 侧规则用审核时的宽松依据在规则自检和手册演练里验证。
- **每次 accept 的确认成本**：172 个 NONE 项每项都要写，确认清单可以引用同一份自检日志；Steam 版、离线版是确认清单之前 accept 的，`tools/baseline/confirm/` 现在是空的。
- verify 每个 exe 约 20 秒（只要布局时也算同一性）；selftest 全部约 36 分钟。
- 运行时（C++）的行为没改；未知版本停用范围（150 个主题只剩 4 个）是 FA 的实现结果，按简报只写进文档，策略由用户定。
- 测试目录、真实安装都没碰；测试目录里那个 `SpineFix.dll` 没动。
- 临时脚本（host 上的 `fbtmp/`）已删除，副本存在 guest 的 `build/u1fb/scripts/`。

## 10. 留给下一步

- 重新审核时请特别看第 6 节的几个取舍，尤其是「无法确认可以人工确认」。
- 用户待定事项见 `08-handoff.md` 的「U1 需要用户决定的事」第 1、2、7 条。
- 下一次真实更新 accept 时第一次会有确认记录，手册第 7 步写了怎么填。

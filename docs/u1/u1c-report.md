# U1c 报告：修复工具（repair、apply、accept、盲定位、RVA 对照表）

阶段工作文件，用户验收后删除。简报见 [brief.md](brief.md) 第 4 节，验收项 A4。接手 [u1a-report.md](u1a-report.md)。完整输出在 guest 的 `D:\workplace\DD.ModLoader\build\u1c\`（下文的文件名都相对这个目录）。

## 1. 结论

| 验收（A4 的各部分） | 状态 | 证据 |
| --- | --- | --- |
| `repair` 对失效项定位、按可信度给结论、候选要明显领先 | 完成 | 第 4 节；U1a 两个样本、清单变异冒烟（9 条特征码 + 7 项布局）全部修对，错误修复 0 |
| 重新生成特征码（保持种类和语义，rel32/rip 通配，扩展到唯一，有长度下限，自检） | 完成 | 第 5 节；盲定位两个方向 61/61 条重新生成并自检通过 |
| 布局项重新求值、新值、指令对齐、多依据一致、只离线 / 没有静态依据的处理 | 完成 | 第 6 节；`mcheck.txt` |
| 逐项报告（txt + json）、清单 diff、旧→新 RVA 对照表 | 完成 | `repair_mut*/`、`repair_m/`；RVA 表带旧 exe 时扩到 4473 个函数，两个方向互逆 4492/4492 |
| `--apply`（`--tree` 改副本）后自动 verify；`accept` 加 `DD_KNOWN_BUILD`、生成基线；`--apply` 不记已知版本 | 完成 | `repair_mut*_apply.txt`、`accept_mutA*.txt`、`accept_reject.txt` |
| 跨版本盲定位，两个方向，错误必须为 0 | 完成 | `blind.txt`、`blind.json`：两个方向都是正确 61、需要人工 0、错误 0；函数级正确 60、错误 0 |
| 盲定位可复用 | 完成 | `dd_update.py blind`、`repair.blind_compare` / `blind_both`（第 10 节） |

没有改 C++，不涉及构建。开工和收尾各跑一次 `verify`（`verify_start.txt`、`verify_end.txt`），都是退出码 0；收尾时基线已换成 format 2（第 7 节）。

## 2. 命令

```
python -B DD.ModLoader/tools/dd_update.py repair --exe <新 exe> [--baseline <基线>] [--tree <源码目录>] [--apply]
    [--sigs <特征码文件> [--layout <布局清单>]] [--out <目录>] [--min-confidence high|medium] [--old-exe <旧 exe>]
python -B DD.ModLoader/tools/dd_update.py accept --exe <新 exe> [--label <版本名>] [--replace] [--allow-optional] [--tree <源码目录>]
python -B DD.ModLoader/tools/dd_update.py blind [--baseline <基线> --exe <exe>] [--json <文件>]
```

- `repair` 默认只读：先对新 exe 跑完整 verify，失败的特征码（没命中、多处命中、hook 安全检查不过、`DD_ENTRY` 不符、误绑定）进入定位，全部运行时布局项重新评估；补丁写进临时目录的清单副本，在新 exe 上预演一遍 verify，预演不过的修复项改成「需要人工」、从补丁拿掉，再预演，直到稳定。输出到 `--out`（默认 `build/repair/<sha1 前 12 位>/`）：`report.txt`、`report.json`、`signatures.diff`、`layout.diff`、`rva_map.tsv`。
- `--apply` 把补丁写进清单（`--tree` 时写 `<tree>/core/src/game/` 下的副本），再跑一遍 verify 并打印。不改 `DD_KNOWN_BUILD`。
- 基线：默认读 `tools/baseline/*.json`（`--tree` 下有 `tools/baseline/` 时读那里），按 verify 同一性检查里各基线的指纹相似度总和挑最像的那个当「更新前」版本；`--baseline` 指定。
- 退出码：0 没有需要人工的项；1 只有可选项需要人工；2 有必需项需要人工；3 参数或清单有错。
- `--sigs` 给插件作者用：只读写那个文件，基线要用 `--baseline` 给（`dd_update.py baseline --sigs ... --out` 生成）。
- `accept` 要求 verify 退出码 0（`--allow-optional` 允许 1），未知版本要 `--label`；把 SHA1 加到最后一个 `DD_KNOWN_BUILD` 之后，生成 `<基线目录>/<版本名>.json`；`--replace` 删掉同名的旧版本行。

## 3. 模块

新增 `tools/ddupdate/` 下 7 个模块，对现有模块只做增量改动（第 11 节）：

| 模块 | 内容 |
| --- | --- |
| `index` | 全 exe 的函数指纹索引（异常目录的主函数 + 空隙里被 E8 调用的叶函数，约 4.9 万个），16 进程生成约 8 秒，按 SHA1 和代码哈希缓存到 `build/ddupdate-cache/`；反查表：调用者、字符串、全局、导入、立即数、函数指针、thunk 目标 |
| `align` | 解码 / 放宽模式；基线函数和新函数的指令对齐 |
| `locate` | 函数级匹配（第 4 节）和特征码的新位置 |
| `regen` | 重新生成特征码和依据模式（第 5 节） |
| `layoutfix` | 布局项修复（第 6 节） |
| `patch` | 按行号改写宏参数，unified diff |
| `repair` | 三个命令、预演 verify、报告、RVA 对照表、给 selftest 的函数接口 |

## 4. 定位手段和阈值

定位分两层：先把基线里的「节点」（特征码的函数、GLOBAL 指向 .text 的目标函数、thunk 的目标、依据所在的函数）在新 exe 里找出来，再推出每条特征码的命中位置和解析结果。

### 4.1 函数级匹配

候选来源，可信度从高到低：

| 手段 | 做法 | 阈值和理由 |
| --- | --- | --- |
| 放宽特征码（repair 模式） | 原模式每条完整指令的位移、立即数、rel 写成 `??`（操作码、ModRM、SIB、前缀保留），扫 .text；不命中时逐条去掉末尾指令再试，最少保留 4 条 | 命中超过 200 处不当候选来源：函数序言这一级的模式没有区分度 |
| 调用图 | 已确认的节点在基线里调用它 / 被它调用 / 把它的地址当函数指针 / 被它当函数指针 / 是它的 thunk 或 thunk 目标，或它引用了已确认的全局 | 约束同时用来打分：满足比例 < 0.5 不过门槛（和 verify 的关系一致度阈值一致）；单条约束的候选集超过 2000 只当检查 |
| 锚点 | 基线指纹里的字符串、导入函数在新 exe 里只被 ≤ 8 个函数引用；≥ 0x100 的立即数 / 位移只出现在 ≤ 4 个函数里 | 只是候选来源，不直接定结论；放宽就会把常见字符串、结构偏移带进来，候选池变大但不影响判定 |
| 指纹搜索 | 全部函数里 3-gram 个数之比 ≥ 0.6 的（Dice 上界 0.75），相似度 ≥ 0.6 的前 12 个 | 总是做，用来看「第二名」：只用其他手段的候选池会漏掉真正的冒充者 |

判定：

- 门槛：相似度 ≥ **0.85**（`SIM_GATE`，和 verify 的同一性阈值相同，修复结果才能通过 verify；U1a 的 simstats 和本阶段盲定位里，同一函数跨版本最低都是 0.929）；有调用图约束时满足比例 ≥ 0.5；基线里有直接调用者时候选也要有。
- 过门槛的按相似度排序，第一名 T。没有别的过门槛候选落在 [T − **0.10**, T]（`MARGIN`）里就选 T。
- 有相近的过门槛候选（模板孪生、相似函数）时，依次看调用图约束满足比例、字符串集合的 Jaccard：某一项上恰好一个候选领先其余 ≥ **0.5**（`DISCRIMINATE_GAP`）才选它，否则报「需要人工」并列出候选。
- 同一轮里所有节点按上一轮结束时的已确认集合判定，一起提交（与顺序无关）；两个旧函数选中同一个新函数，两个都改成需要人工；有新确认的就再来一轮。

可信度：「高」= 明显领先且有结构上的支持（放宽特征码命中、调用图约束全部满足、新 exe 里唯一引用某个字符串 / 导入 / 常量）；「中」= 只靠指纹，或者靠调用图 / 字符串区分了相近候选，或者有相似度相近但被调用者 / 调用图门槛挡掉的候选。

**MARGIN 的数据**（`threshold_stats.txt`，盲定位两个方向 123 个函数判定）：

| 分布 | n | 最小 | p05 | 中位 | p95 | 最大 |
| --- | --- | --- | --- | --- | --- | --- |
| 选中候选的相似度 | 123 | 0.929 | 0.981 | 1.000 | 1.000 | 1.000 |
| 最近冒充者（不论门槛） | 77 | 0.008 | 0.039 | 0.263 | 1.000 | 1.000 |
| 选中 − 最近冒充者 | 77 | 0.000 | 0.000 | 0.728 | 0.961 | 0.992 |

最近冒充者落在 [选中 − 0.10, 选中] 里的有 12 个，都是模板实例：`VectorDtor64`、`VectorDtor64b`、`FreeU32Vector`（1.000 对 1.000）靠调用图区分，`QueryClassNames` 所在的 Factory 注册函数（1.000 对 1.000）靠字符串区分（每个实例注册不同的类名），`BuffInstanceCopy` 的目标（1.000 对 0.924）、`BuffLibraryIndex`（1.000 对 0.941）被调用图门槛挡掉。其余的最近冒充者最高 0.859，离选中的最少 0.14。起初 MARGIN 取 0.05，`BuffLibraryIndex` 只靠指纹以 0.059 的差距通过；改成 0.10 后它要等 `BuffInstanceCtor` 确认、靠「调用了构造函数」这条约束定下来，盲定位结果不变（仍是 61/0/0），所以取更保守的 0.10。

### 4.2 特征码的新位置

- HOOK / 从入口开始的 CODE：函数入口。
- GLOBAL、函数中间的 CODE（`MonsterBlockReturn`）：基线（format 2）记了命中位置和 capture 在旧函数指令序列里的下标。放宽特征码在选中的函数里恰好命中一次时直接用（再和指令对齐比较，不一致报需要人工）；否则用指令对齐。capture 指令从新指令里按同一种字段（rip 位移 / call rel32）算出新地址。
- 指令对齐要两种方法一致：(1) 唯一窗口：旧指令周围最短的连续 token 窗口（≤ 24 条），在旧函数和新函数里都只出现一次；(2) `difflib.SequenceMatcher` 的匹配块。token 并上基线 refs：字符串、导入名原样，调用目标、全局、函数指针按已确认的新旧对应换算；不含位移和立即数的值（结构变化时正是它们在变）。
- GLOBAL 新地址的交叉检查：节和基线一致；指向 .text 时目标函数的相似度要过门槛，独立匹配过的要一致；指向数据时，基线里引用旧全局、已确认的其他函数要引用新地址（两个以上旁证时过半数不引用就报需要人工，只有一个旁证时不引用不算反证但不升到高）。对齐得到的地址可信度「中」，交叉检查全部吻合时升到「高」。
- GLOBAL_ANY：函数级如果是一组区分不了的模板实例，每个都对齐、算出地址；全部解析到同一个地址时定位（可信度中），这正是 GLOBAL_ANY 本身的规则；地址不一致时报需要人工（`DD_RUNTIME_SELECT` 的说明只有运行时能按内容选）。这不是「挑一个当结果」：结论是全局地址，所有候选给出同一个值才成立。

## 5. 重新生成特征码

- 保持种类和语义：HOOK 从入口开始，steal 取不小于 5 字节的第一个指令边界，偷取范围里不能有 rip 相对、分支、call、ret（有就报「hook 入口不安全」，需要人工），原模式把偷取字节写成 `??` 的照旧写 `??` 并更新 `DD_ENTRY`，原模式入口是字面量的（`UpdateStateInternal`）照旧；CODE 从命中位置开始，原模式开头有通配的照旧（不少于原长度的第一个指令边界）；GLOBAL 从命中位置对应的指令开始，capture 重算。
- 有依据用这条特征码的命中位置（`HIT`、`HIT_VALUE`）时不往前扩展；GLOBAL_ANY 的命中位置是最低的模板实例，新模式最低的命中换了实例时，要求原来成立的那些依据在新实例上也都成立。GLOBAL_ANY 要所有命中解析到同一个地址；`DD_RUNTIME_SELECT` 要包含定位到的地址，候选数不超过基线。
- rip 位移、分支 rel 一律 `??`；看起来是地址的位移和立即数也写 `??`：不小于 0x100000、落在映像某个节里的 32 位值（例如 `mov [rsi+rdi*8+2C21670h],rax` 这种以映像基址为基的寻址，清单里最大的结构偏移是 0x52C0），以及映像范围内的 64 位绝对地址。起初没有这一条，重新生成的 `QueryClassNames` 把这个 RVA 写成了字面量，只剩 1 个候选、下次更新必然失效；加上之后候选数和现有的一样是 7 个。
- 下限：至少 6 条指令、12 个非通配字节，最长 96 字节，不跨出分块（没有异常目录表项的 thunk 除外，原 `VectorDtor64b` 的模式就跨过了填充字节）。盲定位里原模式中位 23 字节（最短 11），重新生成的中位 27 字节（最短 13）。
- 自检：`sigs.resolve` + `check_hook`（指令边界、不安全指令、函数内跳进偷取范围的分支、`DD_ENTRY`、Steam 版上和 `hooks.md` 不重叠）在新 exe 上都要过，解析结果和定位一致；之后整份补丁再预演 verify（同一性也要过）。

## 6. 布局项

每条运行时依据给出一个「这一项应该等于多少」的提议：

1. 依据成立：提议当前值（高）。`read=NONE` 或 `mul=0` 的只要求成立。
2. 模式唯一命中、值变了：按 `EvidenceResult.implied` 提议新值。
3. 位置核对：唯一命中时，用基线里这条依据的 `insn_index` 和旧函数的 token 序列（format 2 的基线把依据所在、不是特征码函数的那些函数也存了）对齐到新函数，命中必须正好在对齐的位置。一致保持「高」；对齐不可用时「模式命中、值变了」降成「中」；不一致按「疑似依据误绑定」走第 4 步。
4. 模式不唯一命中（或第 3 步不一致）：放宽依据模式（位移、立即数都写 `??`），在范围里唯一命中就用；多处命中时选恰好在对齐位置上的那个；不命中时只用指令对齐。从同一种字段读值（宽度可以变，读取方式跟着变），提议新值（中，比原模式命中低一级），并在那条指令处重新生成依据模式（范围里唯一，至少 3 条指令、6 个非通配字节，TEXT 范围用特征码的下限），自检求值。
5. 起点依赖这一项自己的值的依据（`VSLOT_VALUE`、`HIT_VALUE`）不走第 4 步：在范围里搜索（槽号 0..原值+32，命中偏移 0..max(256, 2×原值)），所有这类依据都成立的值恰好一个才提议（中）。
6. `read=NONE` 的依据把值写在模式的字面量里（`MapNodeLeft` 的 `48 8B 00` 就是偏移 0，`ListDataSize` 的 `shl rdx,5` 就是 0x20），模式失效时不能自动重新生成，报需要人工（起点依赖值的除外，按第 5 步）。

一项的全部提议必须一致，否则需要人工；`rel` 是别的清单项时用它的新值，整个过程迭代到不再变化（最多 6 轮），每项的可信度取历轮最低。最后用新值和新模式把这一项的全部依据再求值、离线检查用新值重跑，全部通过才算修好。只有离线依据的项：检查通过就是 ok，不通过需要人工（离线检查不给新值）。没有静态依据的 118 项不修，列进「更新后要人工核实」。

## 7. 基线 format 2

`baseline.py` 的 FORMAT 改成 2，format 1 的字段全部保留，`load` 两种都认（`READABLE_FORMATS`）。新增：

- `sigs[id].anchor`：命中位置和 capture 在函数指令序列里的下标、capture 的字段种类。没有它就要拿模式的 token 序列在旧函数里认锚点指令，实测 `BuffLibrary`、`EventManager`、`QueryDispatcher`、`MonsterVtable`、`GamePrng` 都不唯一。
- 指纹的 `refs`：每条指令引用的字符串、导入、直接调用目标、全局、函数指针（`fingerprint.insn_refs`），存成紧凑字符串（字符串和导入记成在同一指纹 `strings` / `imports` 里的下标）。最初存成 JSON 列表，基线从 29 万字节涨到 63 万，改成字符串后是 37 万。
- `functions`：依据命中所在、但不是特征码函数的 10 个函数（虚表槽位、TEXT 依据所在的函数）的指纹。

只存规范化 token 和引用，不存机器码（仓库是公开的）。`tools/baseline/*.json` 已重新生成：两次生成字节一致（`baseline_sha256.txt`），verify 两个版本退出码 0。读到 format 1 基线时（例如 U1b 用旧代码重新生成），repair 仍能工作，GLOBAL 的锚点退回按模式认，认不出的报需要人工。

## 8. 冒烟证据

### 8.1 U1a 的手工破坏样本（`build/u1a/mut/`）

两个样本各 `repair --tree <副本> --apply`，副本里是清单和 format 2 基线（`repair_mutA_apply.txt`、`repair_mutB_apply.txt`）：

```
样本 B（Hero 槽 5 的位移 0x1380 + UpdateStateInternal 栈帧 0x3E8→0x3F0）
  基线 steam.json（format 2）
  FIX高 UpdateStateInternal      [REQUIRED] 修复前：0 次命中 []，要求唯一
        新位置：命中 3DCE10 解析 3DCE10；DD_SIG steal=5 capture=0 "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 18 FD FF FF"
  FIX高 HeroId                       [OPT] 0x137c → 0x1380
        依据 0：模式命中，值变了（读到 0x1380，期望 0x137c（按读到的值，常量应为 0x1380））
 预演 verify（补丁应用到临时副本）：OK（全部通过）（退出码 0）
 结论：需要人工 必需 0、可选 0（退出码 0）
已写入 ...tree_B\core\src\game\signatures.inc、...layout.inc，重新 verify： 结论：OK（全部通过）（退出码 0）
```

样本 A 只有 `HeroId` 一项（FIX高，0x137C→0x1380），apply 后 verify 退出码 0。

### 8.2 跨版本盲定位（`blind.txt`、`blind.json`）

`dd_update.py blind`：以一个版本的基线为准，关掉放宽特征码这一手段，不用现有特征码扫描，只靠调用图、锚点、指纹搜索在另一个 exe 里找全部特征码项，和现有特征码在那个 exe 上的解析结果逐项比较（GLOBAL 比全局地址，HOOK/CODE 比命中位置；`DD_RUNTIME_SELECT` 要求等于现有特征码离线解析出的那个、重新生成的模式的候选是现有候选的子集）。另外和现有特征码、依据给出的函数做函数级比较。模式只用来在基线的旧函数里认锚点指令，format 2 直接存了下标，用不到。

| 方向 | REQUIRED（含 LOADER） | OPT | ANCHOR | 合计 | 函数级 | 重新生成特征码 |
| --- | --- | --- | --- | --- | --- | --- |
| Steam 基线 → 离线版 | 10 / 0 / 0 | 49 / 0 / 0 | 2 / 0 / 0 | **61 / 0 / 0** | 正确 60、错误 0、没有参照 2、需要人工 1 | 61/61 |
| 离线版基线 → Steam 版 | 10 / 0 / 0 | 49 / 0 / 0 | 2 / 0 / 0 | **61 / 0 / 0** | 正确 60、错误 0、没有参照 1、需要人工 2 | 61/61 |

（正确 / 需要人工 / 错误。）每个方向约 44 秒（函数索引已缓存）。函数级的「需要人工」都是 GLOBAL_ANY 的模板实例（`EventClassNames`、`EventTypeCount` 的函数，十几个完全相同的实例），在特征码级按 4.2 的规则定位，没有参照的是 GLOBAL_ANY 的函数（任何一个实例都对）。特征码的可信度：高 43、中 79。盲定位没有调用图种子，第一轮几乎只能靠指纹，所以「中」多；repair 模式下大多数特征码校验通过、当种子，可信度会高得多（清单变异冒烟里 9 条有 8 条是高）。

第一次跑的结果是 58/3/0。三项需要人工分别是：`VectorDtor64b`（两个完全相同的 thunk，当时没有函数指针关系）、`EventTypeCount`（唯一的旁证函数不引用新地址，那是 Steam 版 `StateNames` 所在函数内联了 listener 注册），以及 `QueryClassNames` 的比较口径。随后加了函数指针 / thunk 关系，放宽了单旁证规则，统一了 `DD_RUNTIME_SELECT` 的口径。没有出现过错误定位。

### 8.3 清单变异冒烟（`mcheck.txt`、`repair_m/`）

Steam 版基线 + 离线版 exe，清单副本里故意改坏 16 处，repair `--apply` 后和原清单在离线版上的结果逐项比较（脚本 `build/u1c/mcheck.py`）：

| 改动 | 模拟的情况 | 结果 |
| --- | --- | --- |
| `UpdateStateInternal` 栈帧立即数、`RaidCurrent` 位移、`MonsterBlockReturn` 立即数、`BuffInstanceCopy` 位移、`BuffGroupErase` 栈帧 | 特征码里的常量变了 | 5 条 repaired-高，解析结果和原来一致 |
| `ListenerAdd`、`EventTypeCount` 操作码、`StorageManager`、`EffectLookup` 寄存器 | 放宽也找不回 | `EventTypeCount` 中（模板实例），其余高，全部一致 |
| `InstDuration` 依据模式里的 0x418、`EffectDotBurn` 依据模式里的 0x140 | 依据模式里别的常量变了 | 都是 repaired-中：值不变，生成了新模式 |
| `HeroClass`、`BattleMonsters`（`RaidBattle` 的 rel） | 清单值和 exe 不符 | repaired-高，值改回原值，`RaidBattle` 经迭代不受影响 |
| `VtSlotDamage` 槽号、`MonsterBlockImmuneLeaDisp` 命中偏移 | 起点依赖值 | 搜索到原值，repaired-中 |
| `MapNodeLeft` 的 NONE 依据 | 值在字面量里 | 需要人工 |

```
错误修复： 0
预演 verify： 只有可选项失效（MapNodeLeft）
```

**失败的尝试**：这个冒烟第一次跑出 2 个错误修复。(1) `EffectDotBurn` 的模式改成「下一个字段是 0x148」后在别处唯一命中，「模式命中、值变了」直接给出了 0x142。真实更新里字段整体后移时也会这样：模式靠邻居偏移区分，可能唯一命中邻居的解析代码，verify 照样报通过。(2) `MonsterBlockImmuneLeaDisp`（HIT_VALUE）走了指令对齐，把依据模式改成了在错误起点上成立的样子。修法是第 6 节的第 3、5、6 条（位置核对、起点依赖值的只搜索、NONE 不重新生成）。修完错误为 0，多出的那项 `MapNodeLeft` 正确地报了需要人工。

### 8.4 其余

- `--sigs`（`plugin/repair.txt`）：U1a 的插件文件改坏两条（hook 序言的栈帧、GLOBAL 的位移），用 format 2 的插件基线 repair `--apply`：`MyApplyMiss`、`MyRaid` 都是 repaired-高，`MyStale`（基线生成时就失效）需要人工，退出码 1；用 U1a 生成的 format 1 插件基线也能修 `MyApplyMiss`。
- `accept`（`accept_mutA.txt`）：样本 A 修复后的副本上 `accept --label mutA`，加了 `DD_KNOWN_BUILD("8da30d48…", "mutA")`、生成 `tree_A/tools/baseline/mutA.json`，再 verify 显示「已知版本 mutA」、退出码 0。样本 B 在没修的清单上 accept 被拒（verify 退出码 2），清单没被改（`accept_reject.txt`）。
- 没有失效项时（Steam 版本身）repair 退出码 0、两个 diff 都是空的（`repair_steam/`）。
- RVA 对照表（`rvamap_s2n/`、`rvamap_n2s/`）：带 `--old-exe` 时沿调用图扩展（对齐的直接调用目标、唯一的调用者，两边相似度 ≥ 0.85，冲突丢弃），两个方向各 4492 行（函数 4473、全局 19，冲突丢弃 5）；两张表逐行互逆 4492/4492，新地址没有重复，抽查的已知对（`4C3F20↔4B2090`、`431240↔41F370`、`3E9320↔3D8DF0`、`5C4F50↔5B2880`、`482480↔470650`）都对。不带旧 exe 时只有节点、全局和一对一的调用者（约 80–90 行）。

## 9. 需要人工的项和原因

盲定位和两个 U1a 样本里没有需要人工的项。下面是设计上一定报需要人工的情况，U1d 的变异样本会碰到：

| 情况 | 原因 |
| --- | --- |
| 模板孪生，调用图和字符串都区分不了 | 4.1；GLOBAL_ANY 例外（4.2） |
| 没有调用者的函数被复制（虚函数，例如 `MonsterBlockReturn` 所在的 `5FC3E0`） | 调用者门槛不起作用；虚表里的指针（.rdata 引用代码）没有建模，复制品和原函数区分不了 |
| hook 入口变得不安全 | 偷取范围里有分支 / rip 相对，换偷取方案要人定 |
| GLOBAL 的锚点或 capture 指令对齐失败、交叉检查有反证 | 4.2 |
| `read=NONE` 依据的模式失效（`MapNodeLeft`、`TargetActor` ×2、`ListDataSize`） | 第 6 节第 6 条 |
| 一项的多条依据结论不一致、离线检查用新值不过、只有离线依据的项检查不过 | 第 6 节 |
| 依据的锚点特征码没修好 | 依据 blocked |
| 基线里这条特征码本来就失效 | 没有可比的旧函数 |
| 预演 verify 不过 | 修复项改成需要人工，从补丁里拿掉 |
| `--min-confidence high` 时的「中」 | 按参数 |

## 10. 给 U1d 的接口

- 盲定位：`python -B DD.ModLoader/tools/dd_update.py blind [--json f]`，默认两个方向（`tools/baseline/steam.json` → 离线版、`nosteam.json` → Steam 版），特征码级或函数级有任何错误时退出码 2。Python：`from ddupdate import repair; results = repair.blind_both()`，每个结果的 `exit_code`、`total`（correct/manual/wrong）、`stats`（按 REQUIRED/OPT/ANCHOR）、`functions`、`rows[id]`；单个方向用 `repair.blind_compare(baseline_mod.load(path), exe, manifest)`。
- repair / apply：`repair.repair_exe(exe, tree=<副本>, apply=True, out=<目录>)` 返回 `(result, verify_report)`。`result["sigs"][id]["status"]`、`result["layout"][id]["status"]`（`ok` / `repaired-高` / `repaired-中` / `manual` / `unverified`）、`result["sigs"][id]["new"]`（pattern、steal、capture、entry、hit、resolved）、`result["layout"][id]["new"]`、`result["patch"]`、`result["rva_map"]`、`result["exit_code"]`；`verify_report["exit_code"]` 是 apply 之后的 verify。命令行同样可以：`repair --exe X --tree T --apply --out O`，结果在 `O/report.json`。
- `--tree` 要求 `<tree>/core/src/game/{signatures,layout}.inc`，可以有 `<tree>/tools/baseline/*.json`；往返测试可以直接复制整个 `DD.ModLoader` 当 tree，apply 后在副本里编译 DDCore。
- accept：`accept --exe X --tree T --label L`。
- 变异样本的断言建议：误绑定、多处命中、函数搬迁看 `sigs[id].status` 和 `new.resolved`；结构偏移整体变化看 `layout[id].new`；hook 入口不安全应当是 `manual`，原因含「hook 入口不安全」；模板孪生误绑定应当是 `manual` 或可信度「中」且备注含「靠调用者」。
- 函数索引用多进程生成，要从脚本文件运行（标准输入运行的脚本没法 spawn，会退回单进程，冷启动约 45 秒）；缓存按 SHA1，变异样本每个 exe 第一次约 8 秒。repair 一次 1–1.5 分钟，大头是三次 verify（预检、预演、apply 后）。
- `build/u1c/mcheck.py` 是 8.3 的清单变异冒烟，可以并进 selftest 当一类快的检查（不用造 exe）。

## 11. 改动文件

新增：
- `tools/ddupdate/index.py`、`align.py`、`locate.py`、`regen.py`、`layoutfix.py`、`patch.py`、`repair.py`
- `docs/u1/u1c-report.md`

修改（都是增量，没有改既有语义）：
- `tools/dd_update.py`：`repair`、`accept`、`blind` 子命令和说明
- `tools/ddupdate/__init__.py`：模块表
- `tools/ddupdate/fingerprint.py`：新增 `insn_refs`；`fingerprint` 加可选参数 `coderefs`（默认不变）
- `tools/ddupdate/baseline.py`：FORMAT 2（`anchor`、`refs`、`functions`），`load` 认 1 和 2
- `tools/baseline/steam.json`、`nosteam.json`：用 format 2 重新生成
- `knowledge/dd1/combat.md`：盲定位找到的更多模板孪生（`24A6B0`、`283C80`、thunk `24A610` 和离线版的对应地址），[静态]

没有动：`evidence.py`、`verify.py`、`sigs.py`、`manifest.py`、`core/`、`loader/`、`tests/`、各 `xmake.lua`、仓库里的 `signatures.inc` 和 `layout.inc`（所有 apply 都在 `build/u1c/tree_*` 副本里）、`cpp_compare.py`、`docs/adr/`。

## 12. 没做到的事、已知局限

- **verify 发现不了「依据误绑定」**（8.3 的 `EffectDotBurn`）：模式靠字面量里的邻居偏移区分时，字段整体后移可能让它唯一命中邻居的代码，verify 报通过、值却是错的。repair 的位置核对能发现（要求基线里有那个函数的指令序列，format 2 都有），所以手册里应当写成「更新后即使 verify 全过也跑一次 repair」。要不要把位置核对加进 verify 的同一性部分（只在 Python 侧做，不影响 C++ 的依据语义）需要主线决定，我没有改 verify。
- **字符串当孪生的区分依据**：简报写的是「需要人工或靠调用者区分」。我另外允许字符串 Jaccard 领先 ≥ 0.5 时区分（Factory 注册函数每个实例注册不同的类名，`QueryClassNames` 靠它），可信度「中」。如果主线认为不该这样，把 `locate.decide` 里的 `("strj", "字符串")` 去掉即可，代价是 `QueryClassNames` 的盲定位变成需要人工。
- 虚表对函数的引用（.rdata 里的指针）没有建模，没有调用者的函数被复制时区分不了（第 9 节）。
- 只有离线依据的项、`read=NONE` 依据失效的项不给新值。
- RVA 对照表不带旧 exe 时很小；带旧 exe 的扩展只沿直接调用和唯一调用者，覆盖 4.4 千个函数（约 9%），间接调用、只被虚表引用的函数不在内。
- `hooks.md` 的重叠检查只在标签是 `steam` 的 exe 上做（和 verify 一样），新版本 accept 后用新标签就不再检查。
- 用 Bash 工具跑带中文的长 heredoc 时出过一次截断，改成先把脚本写进文件；对产物没有影响。

## 13. 留给后面的事项

U1d：
- selftest 并入 `blind`（两个方向）、8.3 的清单变异和简报要求的 exe 变异样本，断言见第 10 节。
- `06-workflow.md` 的工具表和「游戏更新后」手册：`verify` → `repair --tree` 或 `repair --apply` → 人工看 `report.txt` 里的「中」和需要人工 → `accept --label` → 用 `rva_map.tsv` 更新知识库（有旧 exe 时加 `--old-exe`）→ `regen.sh --ghidra`。建议写明「verify 全过也跑一次 repair」（第 12 节第 1 条）。
- `06-workflow.md` 里 `baseline` 一行补一句：基线 format 2，改了 `baseline.py` 要重新生成。

主线：
- 第 12 节前两条（verify 加位置核对、字符串区分孪生）要不要采纳。
- U1b 如果重新生成 `tools/baseline/*.json`，用现在的 `baseline.py` 就是 format 2；用旧代码生成的 format 1 也能读，但 repair 对 GLOBAL 的定位会变弱。

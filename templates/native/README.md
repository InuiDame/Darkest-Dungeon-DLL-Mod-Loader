# native 插件模板

`src/main.cpp` 是最小的插件（`DDPlugin_Query`、`DDPlugin_Load`），ABI 见 `sdk/include/dd/dd_api.h`，示例见 `examples/hello_native`。复制本目录、改掉 `xmake.lua` 里的 target 名就能开始写。

## 校验和修复自己的特征码

插件经 hook 服务的 `scan(pattern)` 用自己的特征码时，游戏更新同样会让它们失效或绑错地方。DDModLoader 自己用的工具 `tools/dd_update.py` 也能校验和修复插件的特征码文件：

1. 把特征码写成和 `core/src/game/signatures.inc` 同格式的文件，例子是本目录的 [`sigs.example.inc`](sigs.example.inc)（一条带 `DD_ENTRY` 的 HOOK、两条 GLOBAL）。`DD_DEPS` 可以不写。插件代码里用的模式和文件里的保持一致。
2. 在 guest 的 `D:\workplace` 下校验（默认 Steam 版和离线版）：

   ```
   python -B DD.ModLoader/tools/dd_update.py verify --sigs DD.ModLoader/templates/native/sigs.example.inc
   ```

   查的是唯一命中、GLOBAL 的解析结果、HOOK 偷取范围的指令边界和安全性、命中位置是函数入口、`DD_ENTRY` 和入口字节一致、和 `knowledge/dd1/hooks.md` 里别的插件不重叠。退出码 0 全部通过，1 只有 `OPT` 的失效，2 有 `REQUIRED` 的失效或误绑定。
3. 在游戏还是现在这个版本时生成插件自己的基线，放进插件的仓库（修复工具在游戏更新后只能靠它，旧 exe 那时已经没了）：

   ```
   python -B DD.ModLoader/tools/dd_update.py baseline --sigs <你的特征码文件> --out <插件目录>/baseline
   ```

   每个已知版本一份（`steam.json`、`nosteam.json`，版本名取自 DDModLoader 的 `DD_KNOWN_BUILD`）。改了特征码要重新生成。
4. 游戏更新后，先校验，带上基线才会做同一性检查（命中的是不是原来那个函数）：

   ```
   python -B DD.ModLoader/tools/dd_update.py verify "<新 exe>" --sigs <你的特征码文件> --baseline <插件目录>/baseline/steam.json
   ```

5. 有失效的就修复。不带 `--apply` 只出报告和补丁（`report.txt`、`signatures.diff`、`rva_map.tsv`）；`--apply` 直接改你的特征码文件，然后再校验一遍：

   ```
   python -B DD.ModLoader/tools/dd_update.py repair --exe "<新 exe>" --sigs <你的特征码文件> --baseline <插件目录>/baseline/steam.json --out <目录> [--apply]
   ```

   报告里 `FIX高` 可以直接用，`FIX中` 要自己对照反汇编确认，`MANU` 是工具不给结论的（列出了原因和候选）。HOOK 的入口改成新的偷取字节时会同时改 `DD_ENTRY`，插件代码里装 hook 用的字节数要跟着改。退出码 0 没有需要人工的项，1 只有 `OPT` 的，2 有 `REQUIRED` 的。
6. 改完在新版本上重新生成基线（第 3 步），把新的模式抄回插件代码。

一次实际运行（U1d，用 selftest 的变异样本 `build/u1d/mut/frame.exe` 代替「新 exe」，那个样本把 `Campaign::Current` 所在函数的栈帧从 0x38 改成了 0x48）：第 2 步两个版本退出码 0；第 4 步 `MyCampaign` 0 次命中、退出码 1；第 5 步 `MyCampaign` 修成 `48 83 EC 48 …`（FIX高，放宽特征码直接命中、指令对齐一致），apply 后再校验退出码 0。完整输出在 guest 的 `DD.ModLoader/build/u1d/sdk/`。

# DD.ModLoader

《暗黑地牢》的模组运行时：在 DDModLoader 上增加 native 插件接口和 Lua 脚本接口。本目录同时是 GitHub 仓库 `InuiDame/Darkest-Dungeon-DLL-Mod-Loader` 的工作区。进度和验收标准见 `docs/04-roadmap.md`。

@../knowledge/README.md

## 文档

- 改架构、ABI、加载流程：`docs/02-architecture.md`
- 改 Lua API、事件、沙箱：`docs/03-lua-system.md`
- 开工或验收某阶段：`docs/04-roadmap.md`
- 查某个选型的结论：`docs/adr/`（索引在 `docs/README.md`）
- 构建、测试、派发或审核：`docs/06-workflow.md`
- 接手进行中的工作：`docs/08-handoff.md`

## 工作约定

- 在 guest 上构建和测试（`guest_run`），只认 guest 的 `Darkest.exe`。命令开头先 `source DD.ModLoader/tools/env.sh`，它补上 xmake 路径和用户环境变量，`dd_config` 负责配置平台。
- 用测试目录跑游戏（`tools/rig.py`），真实安装里的文件不动。
- 新的逆向事实写入 `../knowledge/` 并带可信度标记，这里只写设计。
- `DD Mod Lib` 里的 DLL 玩家关不掉，没有脚本和插件时要保持原版行为。
- 最终阶段验收前，设计以框架为主：不兼容现有 DD 插件，冲突的从测试目录卸载；不做新旧版本兼容，ABI 和结构可以直接改（ADR 0017、0018）。
- 集成进加载器后，更新 guest 的 `D:\Program Files (x86)\Inno Setup 6\Output\功能清单 Feature List.txt`（中英双语、末尾加更新记录）；面向 Mod 作者的功能同时更新 `D:\Program Files (x86)\Inno Setup 6\预制功能说明 Built-in Features.txt`。
- guest 装有 360，覆盖 `dbghelp.dll` 被拒时停下来问用户，用假的游戏目录继续测试。
- commit 信息只写主题和正文，不加 Claude 署名。

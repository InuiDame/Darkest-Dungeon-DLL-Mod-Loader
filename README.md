<p align="center">
  <a href="#zh"><kbd>中文</kbd></a>
  &nbsp;&nbsp;|&nbsp;&nbsp;
  <a href="#en"><kbd>English</kbd></a>
</p>

---

<a id="zh"></a>

# Darkest Dungeon DLL Mod Loader

一个专为《暗黑地牢》(Darkest Dungeon) 打造的非侵入式 DLL 类型 MOD 加载器与管理工具。  
本项目通过代理 `dbghelp.dll` 的底层技术，在不修改游戏本体任何文件的前提下，实现自定义 MOD DLL 的安全加载，并配备了直观的图形化配置面板。  

## 核心特性  
- **非侵入式加载**：不修改 `Darkest.exe`，不触碰游戏核心逻辑，兼容 Steam 正版与离线版。 
- **全渠道扫描**：自动识别并递归扫描本地 `mods` 文件夹与 Steam 创意工坊目录 (`262060`) 下的所有 `.dll` 文件。 
- **智能排除机制**：内置原生库排除名单（如 `SDL2.dll`, `fmod.dll` 等），自动跳过游戏自带的运行库，避免加载架构冲突。 
- **可视化配置面板**：附带独立 GUI 工具，自动解析 MOD 名称（读取 `project.xml` / `project.json`）与 DLL 名称，支持一键勾选控制加载。 
- **完善的日志系统**：自动记录加载过程至 `<游戏根目录>\DDModLoader\log\DDModLoader.log`，可配合 DebugView 实时排查 MOD 崩溃或依赖缺失问题。 
- **自动备份与快捷启动**：安装器自动备份原始文件，GUI 支持一键拉起 Steam 版或离线版游戏。 
- **预制功能**：`mods\DD Mod Lib` 中的 DLL 最先加载且始终启用，详见下方“预制功能”。 
- **同名 DLL 去重**：`mods\DD Mod Lib` 中的 DLL 加载成功后，其他目录的同名 DLL（不区分大小写）不再加载，兼容仍在使用独立版的创意工坊玩家；前置版本加载失败时仍会加载其他目录的版本。GUI 同样不列出这些副本。  

## 实现原理  
本项目利用 Windows 的 DLL 代理（Proxy / Side-loading）技术实现：  
1. 游戏 `Darkest.exe` 会静态导入系统级 `dbghelp.dll` 的 `MiniDumpWriteDump` 函数。 
2. 将编译好的 `DDModLoader.dll` 重命名为 `dbghelp.dll` 并放置在 `win64` 目录。游戏启动时，会优先加载该代理 DLL。 
3. 代理 DLL 内部将 `MiniDumpWriteDump` 原样转发给系统 `C:\Windows\System32\dbghelp.dll`，保证游戏基础功能正常。 
4. 随后，代理 DLL 开启独立线程，读取 `DDModLoader\cfg\DDModLoader.cfg` 配置，先加载 `mods\DD Mod Lib` 中的预制功能，再遍历其余 MOD 目录，并使用 `LoadLibraryW` 动态加载玩家勾选的 MOD DLL。  

> 注：本项目完全开源，没有任何网络请求。加载器本身只执行文件读取与 DLL 加载操作，安装包附带的预制功能会按各自用途 Hook 或修改游戏进程内存，不修改游戏文件。 

## 目录结构  
配置和日志统一放在 `<游戏根目录>\DDModLoader` 下：  

| 位置 | 内容 |
| --- | --- |
| `cfg\DDModLoader.cfg` | 每个 DLL 的启用状态 |
| `cfg\extra_mods.cfg` | 额外 MOD 目录，每行一个绝对路径 |
| `cfg\DeathBlowCap.cfg`、`cfg\ProtectionCap.cfg` | 预制功能的配置，重新安装时保留 |
| `ini\gui.ini` | 配置工具的游戏目录和视图设置，位于配置工具所在目录 |
| `log\DDModLoader.log` | 加载器日志，无法写入时退回 `%LOCALAPPDATA%\DDModLoader` 或系统临时目录 |
| `log\<插件名>.log` | 预制功能的日志 |

从旧版本升级时，安装程序会把原来位于游戏根目录和 `%LOCALAPPDATA%\DDModLoader` 的配置迁移过来。  

## 预制功能  
以下 DLL 随安装包释放到 `mods\DD Mod Lib`，不会出现在 GUI 列表中。MOD 制作者的用法见安装目录下的《预制功能说明 Built-in Features.txt》。  

| DLL | 功能 |
| --- | --- |
| `buff_icon.dll` | 在 buff 的 JSON 中添加 `buff_icon` 字段，在角色状态栏显示独立图标 |
| `FmodCrashFix.dll` | 修复返回主菜单时 FMOD Studio 异步线程读取空指针导致的闪退 |
| `DarkestCrashFix.dll` | 修复主程序的 4 类闪退：自定义祖产货币回城结算、无主 ActorDot、传送 ActorDot、BuffFX 叠加（贡献者：局师傅） |
| `DeathBlowCapMod.dll` | 修改死门抗性上限，默认保持原版 87% |
| `ProtectionCapMod.dll` | 修改保护上限，默认保持原版 80% |
| `MinHook.x64.dll` | MinHook 运行库，供依赖它的 DLL MOD 使用 |

## 快速使用  
1. 前往 Releases 页面下载最新的安装包。 
2. 运行安装包，安装程序会自动探测 Steam 游戏目录。确认路径后点击安装。 
3. 安装完成后，桌面会生成“DDModLoader 配置工具”快捷方式。 
4. 打开配置工具，列表会自动展示所有检测到的 MOD DLL。勾选你想加载的 MOD，取消勾选不想加载的，点击保存。 
5. 点击“从 Steam 启动”或“启动离线版”进入游戏即可生效。  

## 关于杀毒软件误报  
由于本程序采用了 DLL 代理技术，且使用 `LoadLibrary` 动态加载其他 DLL，这种行为特征与部分恶意软件极为相似。
同时，程序未购买数字签名证书，因此极易遭到 Windows Defender、360、火绒等杀毒软件的误报拦截。  
- 如果您不放心，欢迎审查本仓库的所有源代码，并使用 Visual Studio 自行编译。 
- 如果已被拦截，请将本程序添加到杀毒软件的信任区或白名单。  

## 本地编译指南  
- 环境：Visual Studio 2026 ，C++ 桌面开发工作负载。 
- 架构：x64 (必须)。 
- 工程结构：   
  - `DDModLoader`：核心代理 DLL 工程（编译后生成 `DDModLoader.dll`，部署时需改名为 `dbghelp.dll`）。   
  - `DDModLoaderGUI`：配置工具工程（编译后生成 `DDModLoaderGUI.exe`）。   
  - `Installer`：Inno Setup 打包脚本。 
 
## 开源协议  
本项目基于 MIT License 协议开源。你可以自由修改、分发，但请保留原作者信息。

---

<p align="right"><a href="#en"><kbd>English ↓</kbd></a></p>

---

<a id="en"></a>

# Darkest Dungeon DLL Mod Loader

A non-invasive DLL mod loader and manager built specifically for *Darkest Dungeon*.  
By proxying `dbghelp.dll` at a low level, this project safely loads custom mod DLLs without modifying any of the game's own files, and ships with an intuitive graphical configuration panel.

## Core Features
- **Non-invasive loading**: Does not modify `Darkest.exe` and does not touch the game's core logic. Compatible with both the Steam retail version and offline versions.
- **Full-channel scanning**: Automatically detects and recursively scans every `.dll` file in the local `mods` folder and the Steam Workshop directory (`262060`).
- **Smart exclusion mechanism**: A built-in native library exclusion list (e.g. `SDL2.dll`, `fmod.dll`) automatically skips the game's bundled runtime libraries to avoid loading architecture conflicts.
- **Visual configuration panel**: Comes with a standalone GUI tool that automatically parses mod names (by reading `project.xml` / `project.json`) and DLL names, and supports one-click checkbox control over loading.
- **Complete logging system**: Automatically records the loading process to `DDModLoader.log`; works with DebugView for real-time troubleshooting of mod crashes or missing dependencies.
- **Automatic backup and quick launch**: The installer automatically backs up the original files, and the GUI can launch either the Steam version or the offline version with a single click.

## How It Works
This project leverages Windows DLL proxying (Proxy / Side-loading):
1. The game `Darkest.exe` statically imports the `MiniDumpWriteDump` function from the system-level `dbghelp.dll`.
2. The compiled `DDModLoader.dll` is renamed to `dbghelp.dll` and placed in the `win64` directory. When the game starts, this proxy DLL is loaded first.
3. Inside the proxy DLL, `MiniDumpWriteDump` is forwarded verbatim to the system `C:\Windows\System32\dbghelp.dll`, so the game's basic functionality keeps working normally.
4. The proxy DLL then spawns a dedicated thread, reads the `DDModLoader.cfg` configuration, walks the mod directories, and uses `LoadLibraryW` to dynamically load the mod DLLs selected by the player.

> Note: This project is fully open source. It makes no network requests, performs no memory injection, and has no persistence behavior — it only reads files and loads DLLs.

## Quick Start
1. Go to the Releases page and download the latest installer.
2. Run the installer; it will automatically detect the Steam game directory. Confirm the path and click Install.
3. After installation, a "DDModLoader Configuration Tool" shortcut is created on the desktop.
4. Open the configuration tool — the list automatically shows every detected mod DLL. Check the mods you want to load, uncheck the ones you don't, and click Save.
5. Click "Launch from Steam" or "Launch Offline Version" to start the game, and the changes take effect.

## About Antivirus False Positives
Because this program uses DLL proxying and relies on `LoadLibrary` to dynamically load other DLLs, this behavior pattern closely resembles that of certain malware.
In addition, the program has not purchased a digital signature certificate, so it is very likely to be flagged and blocked as a false positive by Windows Defender, 360 Total Security, Huorong, and other antivirus software.
- If you have concerns, you are welcome to review all the source code in this repository and compile it yourself with Visual Studio.
- If it has already been blocked, please add this program to your antivirus software's trust zone or whitelist.

## Local Build Guide
- Environment: Visual Studio 2026, Desktop development with C++ workload.
- Architecture: x64 (required).
- Project structure:
  - `DDModLoader`: the core proxy DLL project (produces `DDModLoader.dll`, which must be renamed to `dbghelp.dll` when deployed).
  - `DDModLoaderGUI`: the configuration tool project (produces `DDModLoaderGUI.exe`).
  - `Installer`: the Inno Setup packaging script.

## License
This project is open source under the MIT License. You are free to modify and redistribute it, but please retain the original author's information.

---

<p align="right"><a href="#zh"><kbd>↑ 中文</kbd></a></p>

# Darkest Dungeon DLL Mod Loader

一个专为《暗黑地牢》(Darkest Dungeon) 打造的非侵入式 DLL 类型 MOD 加载器与管理工具。  
本项目通过代理 `dbghelp.dll` 的底层技术，在不修改游戏本体任何文件的前提下，实现自定义 MOD DLL 的安全加载，并配备了直观的图形化配置面板。  

## 核心特性  
- **非侵入式加载**：不修改 `Darkest.exe`，不触碰游戏核心逻辑，兼容 Steam 正版与离线版。 
- **全渠道扫描**：自动识别并递归扫描本地 `mods` 文件夹与 Steam 创意工坊目录 (`262060`) 下的所有 `.dll` 文件。 
- **智能排除机制**：内置原生库排除名单（如 `SDL2.dll`, `fmod.dll` 等），自动跳过游戏自带的运行库，避免加载架构冲突。 
- **可视化配置面板**：附带独立 GUI 工具，自动解析 MOD 名称（读取 `project.xml` / `project.json`）与 DLL 名称，支持一键勾选控制加载。 
- **完善的日志系统**：自动记录加载过程至 `DDModLoader.log`，可配合 DebugView 实时排查 MOD 崩溃或依赖缺失问题。 
- **自动备份与快捷启动**：安装器自动备份原始文件，GUI 支持一键拉起 Steam 版或离线版游戏。  

## 实现原理  
本项目利用 Windows 的 DLL 代理（Proxy / Side-loading）技术实现：  
1. 游戏 `Darkest.exe` 会静态导入系统级 `dbghelp.dll` 的 `MiniDumpWriteDump` 函数。 
2. 将编译好的 `DDModLoader.dll` 重命名为 `dbghelp.dll` 并放置在 `win64` 目录。游戏启动时，会优先加载该代理 DLL。 
3. 代理 DLL 内部将 `MiniDumpWriteDump` 原样转发给系统 `C:\Windows\System32\dbghelp.dll`，保证游戏基础功能正常。 
4. 随后，代理 DLL 开启独立线程，读取 `DDModLoader.cfg` 配置，遍历 MOD 目录，并使用 `LoadLibraryW` 动态加载玩家勾选的 MOD DLL。  

> 注：本项目完全开源，没有任何网络请求、内存注入或持久化行为，仅执行文件读取与 DLL 加载操作。 

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

# 本目录就是加载器仓库的工作区

`DD.ModLoader/` 是 GitHub 仓库 `InuiDame/Darkest-Dungeon-DLL-Mod-Loader` 的工作区，带完整历史，没有另开新仓库。新的运行时要改代理本身（早期 hook、启动 DDCore），并且和加载器一起发布，放在同一个仓库里只需维护一份源码。原来的 `DDModLoader/` 源码在 P0 迁到 `loader/`；`DDModLoaderGUI/`、`BaseDLLMod/`、`BeaconDLL/` 的 VS 工程保持原样，P7 再统一改成 xmake。

2026-11-09 敲定。

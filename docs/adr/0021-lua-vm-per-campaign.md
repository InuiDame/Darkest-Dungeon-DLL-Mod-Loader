# 每次进存档新建 Lua VM

DDLua 在 `CampaignLoad` 读当前存档的启用列表，有 Lua Mod 时才新建 VM，按依赖拓扑排序加载（同层按启用列表的顺序）。`CampaignUnload` 时按加载的逆序拆除每个 Mod 的订阅、定时器、协程，丢弃环境表，完整 GC 后关闭 VM，存档之间什么都不留。没有 Lua Mod 时不建 VM，DDLua 只订阅 lifecycle，hook 和原版一致。

Mod 由 `<Mod 目录>/lua/manifest.json` 识别，`id` 用于依赖、日志和重载。`main.lua` 出错的 Mod 视为加载失败，它已经注册的东西全部撤销，依赖它的 Mod 不加载，其余照常。VM 损坏（Lua 代码里出现原生异常）时，这个会话停止派发，拆除时不再碰 VM，直接丢弃，下次进存档再新建。

开发模式（`DDLua.cfg`，默认关）另有两个入口：`dev_global_dirs` 在 `AssetsReady` 加载指定目录的 Mod，用来在主菜单自检；`dd.reload` 和文件监视拆除一个 Mod 及依赖它的 Mod，再重新执行 `main.lua`，`dd.state` 跨重载保留。

## 考虑过的方案

整个进程只用一个 VM、退出存档时只拆除各 Mod：拆除的遗漏（被别的 Mod 引用的表、`package.loaded`、注册表里的引用）会跨存档累积，新建 VM 一次性排除了这类问题。

## 后果

切换存档时脚本状态全部丢失，要跨存档保留的数据等 P4 的持久化。直接退出游戏不保证有 `CampaignUnload`，进程退出时不碰 VM。

2026-11-11 敲定（P3）。

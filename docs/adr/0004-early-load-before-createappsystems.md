# 新协议插件在 CreateAppSystems 之前加载，旧式 DLL 不变

代理在 `DllMain` 里 hook `Darkest::CreateAppSystems`，主线程进入它之前加载 `DDCore` 和新协议插件，然后派发 `Early` 阶段。原加载器的做法是另开线程 `Sleep(1500)` 后再加载，它和主线程是竞争关系：DLL 不知道游戏初始化到了哪一步，还要在主线程运行时打补丁。新协议插件改成确定的时序。旧式 DLL 仍由原来的线程加载，已有 Mod 的行为不变。

## 后果

找不到 `CreateAppSystems` 的特征码时不装早期 hook，新协议整体停用，旧式 DLL 照常加载（见 [0006](0006-signature-addressing.md)）。

2026-11-09 敲定。

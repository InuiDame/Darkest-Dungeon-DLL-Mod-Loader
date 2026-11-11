# 代理保持精简，功能放在 DDCore，DDLua 只走 SDK

`dbghelp.dll` 代理只做三件事：装早期 hook、启动 `DDCore`、按原流程加载旧式 DLL。事件、hook 仲裁、调度等功能都在 `mods\DD Mod Lib\DDCore.dll`。代理是最难覆盖的文件（360 会拦截覆盖），改动越少越好，更新功能时只换 `DDCore.dll`。代理和 `DDCore` 之间只有 `DDCore_Boot`、`DDCore_AppSystemsCreated` 两个导出（`common/boot_abi.h`）。

`DDLua.dll` 也是一个普通的 native 插件，和第三方插件一样只通过 SDK 的 C ABI 调用 `DDCore`。这样 ABI 不够用的地方会在写 DDLua 时最先暴露出来，而不是等第三方作者撞上。

## 后果

`DDCore` 和 `DDLua` 预装在 `DD Mod Lib`，玩家关不掉。没有脚本和插件订阅时必须保持原版行为：hook 按需安装，默认不改任何数据（见 [0007](0007-hooks-on-demand.md)）。

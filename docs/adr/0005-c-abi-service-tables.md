# Native ABI 是纯 C 的服务表

插件导出 `DDPlugin_Query`、`DDPlugin_Load`（`DDPlugin_Unload` 可选），在 `Load` 里拿到 `DDApi`，再按名字和版本查询服务。每个服务是一张开头为 `{size, version}` 的函数指针表，没实现的服务返回空。这是 SKSE/F4SE 的模型，C++ 封装以 header-only 的形式放在 ABI 之上。

插件和 `DDCore` 各自静态链接 CRT（`/MT`），编译器版本也可能不同，所以接口上不能出现 C++ 类、STL 容器或跨 CRT 的内存所有权。字符串和缓冲区由调用方提供，或者由核心持有到回调结束。游戏对象以句柄（类型加 guid）暴露，每次访问时解析，失效返回空。

## 考虑过的方案

C++ 虚接口写起来更省事，但会把插件绑死在同一套编译器和 CRT 上，不采用。

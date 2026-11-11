# Lua 官方测试集

核对 `xmake/lua.lua` 的补丁有没有改坏 Lua 的语义（[ADR 0019](../../docs/adr/0019-lua-as-cpp-with-patches.md)）。改补丁或升级 Lua 之后重跑。P3 最后一轮（`dd_patch` 7、8）用的就是这套文件。

## 文件

| 文件 | 用途 |
| --- | --- |
| `standalone_xmake.lua` | 三个独立解释器：`lua_dd`（原版 `lua.c` + 仓库的补丁库 + `ddstubs.cpp`）、`lua_plain`（同一个补丁库，回调保持默认值）、`lua_vanilla`（原版 5.4.9 全部源码，同样按 C++ 编译，区分平台和配置造成的失败） |
| `ddstubs.cpp` | DD 补丁回调最简单的桩：只计数，检查线程切换和原子阶段的通知成对；`DDSTUB_GCLIMIT=<字节>` 时按 DDLua 的规则压低 GC 阈值；`DDSTUB_REPORT=<文件>` 时退出时写计数 |
| `runone.lua` | 单独跑一个测试文件，环境和 `all.lua` 的 `_U=true` 相同 |
| `runall.sh` | 依次跑 28 个文件，汇总成 `summary_<exe>.txt` |
| `mkvariant.lua` | 生成按设计差异调整过的副本 `lua-5.4.9-tests-dd`，每处修改对应一条补丁 |

## 目录和准备

都在 guest 上，工作目录 `D:\workplace\DD.ModLoader\build\luatests`（`build/` 不同步、不进 git）。guest 下载 lua.org 经常失败，可以在 host 下载后放进工作区同步过去：

| 压缩包 | sha256 |
| --- | --- |
| `https://www.lua.org/tests/lua-5.4.9-tests.tar.gz` | `7d971845f545ffc09fbb3128a86b2c6524161c70d0fdf0154a16e8c00c343fca` |
| `https://www.lua.org/ftp/lua-5.4.9.tar.gz` | `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`（和 `xmake/lua.lua` 相同） |

```
build/luatests/
├─ lua-5.4.9-tests/          测试集解包
├─ vanilla-src/lua-5.4.9/    源码解包
├─ standalone/               standalone_xmake.lua 复制成 xmake.lua，加 ddstubs.cpp，
│                            再从 vanilla-src/lua-5.4.9/src/ 复制 lua.c、lprefix.h
└─ scripts/                  runall.sh、runone.lua、mkvariant.lua
```

`lua.c` 要单独复制一份：直接编译源码目录里的 `lua.c` 会先找到旁边的原版头文件，而不是补丁库的头文件。

## 构建和运行

```bash
source DD.ModLoader/tools/env.sh
cd DD.ModLoader/build/luatests/standalone
xmake f -P . -m release -y </dev/null && xmake -P . -y </dev/null
cd ..
B=standalone/build/windows/x64/release
$B/lua_vanilla.exe scripts/mkvariant.lua                 # 生成 lua-5.4.9-tests-dd
bash scripts/runall.sh vanilla ../out lua-5.4.9-tests    # 对照：原版应当全过
bash scripts/runall.sh dd ../out lua-5.4.9-tests         # 补丁库跑原测试集
bash scripts/runall.sh dd ../out lua-5.4.9-tests-dd      # 补丁库跑调整过的副本
```

`runall.sh` 里的路径写死成 `/d/workplace/DD.ModLoader/build/luatests`。要测 GC 阈值的补丁，加环境变量 `DDSTUB_GCLIMIT=4194304` 再跑一遍。

## 预期结果

`dd_patch` 8 时：原版 28 个文件全过；补丁库跑原测试集 26 个通过，失败的两个都是按设计的差异：

- `gc.lua` 的 3 处断言：只有弱键的表按强引用处理；
- `errors.lua:384` 一带：`LUA_IDSIZE` 是 160。

补丁库跑 `lua-5.4.9-tests-dd` 时 28 个文件全过（`DDSTUB_GCLIMIT` 设 4 MB 时同样）。出现别的失败就是补丁引入的回归。新增按设计的差异时，在 `mkvariant.lua` 的 `EDITS` 里加一条，写明对应哪条补丁。

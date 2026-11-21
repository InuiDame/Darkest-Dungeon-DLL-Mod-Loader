// 插件自己的特征码文件示例：格式同 core/src/game/signatures.inc（开头有完整说明），DD_DEPS 可以不写。
// 插件运行时经 hook 服务的 scan(pattern) 用这些模式；离线用 dd_update.py 的 --sigs 校验和修复，见同目录 README.md。
//   DD_SIG(id, kind, required, steal, capture, "pattern")
//   kind      HOOK（steal 是入口偷取字节数）/ GLOBAL（capture 是 rip 相对位移在模式里的偏移）/ CODE（命中位置本身）
//   required  REQUIRED 或 OPT：只影响校验报告的退出码（必需项失效时退出码 2）

// 例 1：hook ApplyMiss（Steam 版 5BAE30）。入口 5 字节写成通配（可能已被别的 DLL hook），DD_ENTRY 给出真实入口字节
DD_SIG(MyApplyMiss, HOOK, OPT, 5, 0, "?? ?? ?? ?? ?? 55 56 57 41 54 41 56 48 81 EC E0 00")
DD_ENTRY(MyApplyMiss, "48 89 5C 24 08")

// 例 2：读 Raid::Current（全局指针），模式里 mov rax,[rip+disp32] 的位移在第 3 字节
DD_SIG(MyRaid, GLOBAL, REQUIRED, 0, 3, "48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8B 88 C0 52 00 00 48 85 C9 74 ?? 48 8D 15 ?? ?? ?? ?? E8")

// 例 3：读 Campaign::Current，模式从函数入口的 sub rsp,0x38 开始（栈帧一变就不命中，README 里的修复例子用的就是它）
DD_SIG(MyCampaign, GLOBAL, OPT, 0, 14, "48 83 EC 38 80 3D ?? ?? ?? ?? 00 48 8B 05 ?? ?? ?? ?? 74 ?? 48 85 C0 75")

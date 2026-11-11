#pragma once
// 函数入口指令的长度解码，只用于判断内联 hook 的偷取字节是否安全。
//
// 只认识常见的入口指令，覆盖不了的一律拒绝（宁可不 hook，也不搬坏指令）：
//   - rip 相对寻址搬走之后位移就错了，拒绝；
//   - call/jmp/jcc/ret 等控制流指令不能搬，拒绝。

#include <cstdint>

namespace ddc::hook {

// 解码 code 处的一条指令，返回其长度。失败返回 0 并把原因写入 reason。
std::uint32_t DecodeInstruction(const std::uint8_t* code, std::uint32_t available, const char** reason);

// 从 code 起累计解码，直到覆盖至少 minLength 字节，返回结束在指令边界上的长度。
// 任一指令不安全或超出 available 时返回 0 并写 reason。
std::uint32_t ComputeStealLength(const std::uint8_t* code, std::uint32_t available, std::uint32_t minLength,
    const char** reason);

}  // namespace ddc::hook

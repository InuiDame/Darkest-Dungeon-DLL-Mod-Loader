#pragma once
// 脚本 Buff 的状态表和 JSON 互转（P4b）。add_buff 时用它检查初始状态，P4c 的存档旁路用它导出、导入。
//
// 能保存的表：键全是字符串，或者是从 1 开始的连续整数（数组）；空表按对象 {} 处理。值是布尔、有限数（整数和浮点数
// 分开保存：1 和 1.0 读回来类型不变）、字符串（必须是合法的 UTF-8）、表。不允许函数、userdata、线程、nan/inf、
// 字符串和整数混用的键、有空洞的数组、循环引用。嵌套最多 32 层，元素最多 10 万个，结果最多 1 MB。
// 遍历用原始访问（lua_next、lua_rawget），不触发 Mod 的元方法；每个元素报告一次工作量，受执行预算约束。

#include <lua.h>

#include <string>
#include <string_view>

namespace ddlua::state_json {

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxItems = 100000;
constexpr std::size_t kMaxBytes = 1024 * 1024;

// index 处的表转成 JSON 文本。失败时返回 false，error 写原因和位置（如「state.items[3]：不能保存 function」）。
// root 是位置的开头（"state"）。在 Lua 的 C 函数里或保护模式内调用。
// maxBytes 是结果的上限（不超过 kMaxBytes），超过时编码到那里就停下（存档时按 store_max_kb 传，免得先编码完很大的表再丢掉，
// 审核 B-N6），tooLarge 写是否因为这个失败
bool Encode(lua_State* L, int index, const char* root, std::string& out, std::string* error, std::size_t maxBytes = kMaxBytes, bool* tooLarge = nullptr);

// JSON 文本（Encode 的结果）转成表压栈。失败时不压栈，返回 false，error 写原因。在保护模式内调用（分配计入 VM 的上限）
bool Decode(lua_State* L, std::string_view json, std::string* error);

}  // namespace ddlua::state_json

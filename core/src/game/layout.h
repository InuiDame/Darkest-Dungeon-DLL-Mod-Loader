#pragma once
// 游戏布局常量：layout::k<id>，唯一来源是同目录的 layout.inc（格式、依据的语义和依赖表的词汇见它的开头）。
// 这里只把常量和表展开成 constexpr；依据（DD_EVIDENCE、DD_OFFLINE、DD_UNVERIFIED）由 tools/dd_update.py verify
// 和运行时核对读取，组（DD_GROUP）只给 tools/ddupdate 用，C++ 展开成空。改值只改 layout.inc，再跑 verify 和 dd_core_tests（tests/unit/layout_golden.cpp 对照旧值）。

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ddc::game::layout {

// DD_VALUES 里的来源标志，等于 DD_SOURCE_F_*（combat/logic.cpp 里核对）
inline constexpr std::uint32_t SRC_S = 1;
inline constexpr std::uint32_t SRC_T = 2;
inline constexpr std::uint32_t SRC_Y = 4;

#define DD_LAYOUT_TYPE_OFFSET std::size_t
#define DD_LAYOUT_TYPE_SIZE std::size_t
#define DD_LAYOUT_TYPE_SLOT std::size_t
#define DD_LAYOUT_TYPE_COUNT std::size_t
#define DD_LAYOUT_TYPE_TABLE std::size_t
#define DD_LAYOUT_TYPE_ENUM std::int32_t
#define DD_LAYOUT_TYPE_CONST std::uint32_t
#define DD_LAYOUT(id, type, value, need, check, deps) inline constexpr DD_LAYOUT_TYPE_##type k##id = value;
#define DD_NAMES(id, ...) inline constexpr const char* const k##id##Names[] = { __VA_ARGS__ };
#define DD_VALUES(id, ...) inline constexpr std::uint32_t k##id##Values[] = { __VA_ARGS__ };
#include "layout.inc"
#undef DD_LAYOUT
#undef DD_NAMES
#undef DD_VALUES
#undef DD_EVIDENCE
#undef DD_OFFLINE
#undef DD_UNVERIFIED
#undef DD_GROUP
#undef DD_LAYOUT_TYPE_OFFSET
#undef DD_LAYOUT_TYPE_SIZE
#undef DD_LAYOUT_TYPE_SLOT
#undef DD_LAYOUT_TYPE_COUNT
#undef DD_LAYOUT_TYPE_TABLE
#undef DD_LAYOUT_TYPE_ENUM
#undef DD_LAYOUT_TYPE_CONST

// 名字表里按名字取下标（枚举数值），编译期求值，名字不在表里时编译失败
template <std::size_t N>
consteval std::int32_t IndexOf(const char* const (&names)[N], std::string_view name)
{
    for (std::size_t i = 0; i < N; ++i) {
        if (std::string_view(names[i]) == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    throw "名字不在 layout.inc 的表里";
}

// 函数指针类型的参数个数。layout.inc 的原型项（Proto*，COUNT，值是 C++ 声明并转发或传入的参数个数）用它和代码里的
// 函数指针类型绑定：static_assert(layout::kArgs<XxxFn> == layout::kProtoXxx)，改了一边没改另一边时编译失败。
// x64 上 __fastcall 被忽略，带不带它是同一个类型
template <class F>
struct FnArgs;
template <class R, class... A>
struct FnArgs<R (*)(A...)> {
    static constexpr std::size_t value = sizeof...(A);
};
template <class F>
inline constexpr std::size_t kArgs = FnArgs<F>::value;

}  // namespace ddc::game::layout

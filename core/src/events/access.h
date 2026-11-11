#pragma once
// events 服务里负载访问的实现（读写字段、取消、元信息），DDCore 的服务表和 DDLua 的离线测试共用，
// 保证测试里的假 events 服务和真实服务的语义一致。只依赖 hub.h，不依赖游戏。

#include "hub.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ddc::events::access {

// 写入值是否在字段的原始宽度之内（审核 B-Y1、B-Y2：超出 float 范围的有限值写回游戏会变成 ±inf，
// 超出 int32 的整数会被静默截断；native 插件和 Lua 都经过这里）
inline bool IntInRange(FieldRange range, std::int64_t value)
{
    switch (range) {
    case FieldRange::I32:
        return value >= (std::numeric_limits<std::int32_t>::min)() && value <= (std::numeric_limits<std::int32_t>::max)();
    case FieldRange::U32:
        return value >= 0 && value <= static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)());
    default:
        return true;
    }
}

inline bool FloatInRange(FieldRange range, double value)
{
    if (range == FieldRange::F32) {
        return std::fabs(value) <= static_cast<double>(FLT_MAX);  // nan 比较为假，也拒绝
    }
    return true;
}

inline Payload* ToPayload(DDEvent* event)
{
    return reinterpret_cast<Payload*>(event);
}

inline const Payload* ToPayload(const DDEvent* event)
{
    return reinterpret_cast<const Payload*>(event);
}

template <class T, class Get>
DDEventResult ReadField(const DDEvent* event, const char* field, DDEventFieldType type, T* out, Get get)
{
    const auto* payload = ToPayload(event);
    if (!payload || !payload->topic || !payload->slots || !out) {
        return DD_EV_E_INVALID;
    }
    std::uint32_t index = 0;
    const auto* desc = Hub::FindField(payload->topic, field, &index);
    if (!desc) {
        return DD_EV_E_NOT_FOUND;
    }
    if (desc->type != type) {
        return DD_EV_E_TYPE;
    }
    if (!payload->slots[index].set) {
        *out = T{};
        return DD_EV_OK;
    }
    *out = get(payload->slots[index]);
    return DD_EV_OK;
}

// inRange(desc) 为假时返回 DD_EV_E_RANGE，不写入
template <class InRange, class Set>
DDEventResult WriteField(DDEvent* event, const char* field, DDEventFieldType type, InRange inRange, Set set)
{
    auto* payload = ToPayload(event);
    if (!payload || !payload->topic || !payload->slots) {
        return DD_EV_E_INVALID;
    }
    std::uint32_t index = 0;
    const auto* desc = Hub::FindField(payload->topic, field, &index);
    if (!desc) {
        return DD_EV_E_NOT_FOUND;
    }
    if (desc->type != type) {
        return DD_EV_E_TYPE;
    }
    if (!payload->allowWrite || !(desc->access & DD_FIELD_WRITE)) {
        return DD_EV_E_READ_ONLY;
    }
    if (!inRange(*desc)) {
        return DD_EV_E_RANGE;
    }
    set(payload->slots[index]);
    payload->slots[index].set = true;
    return DD_EV_OK;
}

inline bool AnyValue(const FieldDesc&)
{
    return true;
}

inline DDEventResult GetInt(const DDEvent* event, const char* field, int64_t* out)
{
    return ReadField(event, field, DD_FIELD_INT, out, [](const Slot& s) { return s.v.i; });
}

inline DDEventResult GetFloat(const DDEvent* event, const char* field, double* out)
{
    return ReadField(event, field, DD_FIELD_FLOAT, out, [](const Slot& s) { return s.v.f; });
}

inline DDEventResult GetBool(const DDEvent* event, const char* field, bool* out)
{
    return ReadField(event, field, DD_FIELD_BOOL, out, [](const Slot& s) { return s.v.b; });
}

inline DDEventResult GetString(const DDEvent* event, const char* field, const char** out)
{
    return ReadField(event, field, DD_FIELD_STRING, out, [](const Slot& s) { return s.v.s ? s.v.s : ""; });
}

inline DDEventResult GetPointer(const DDEvent* event, const char* field, void** out)
{
    return ReadField(event, field, DD_FIELD_POINTER, out, [](const Slot& s) { return s.v.p; });
}

inline DDEventResult SetInt(DDEvent* event, const char* field, int64_t value)
{
    return WriteField(
        event, field, DD_FIELD_INT, [&](const FieldDesc& d) { return IntInRange(d.range, value); },
        [&](Slot& s) { s.v.i = value; });
}

inline DDEventResult SetFloat(DDEvent* event, const char* field, double value)
{
    return WriteField(
        event, field, DD_FIELD_FLOAT, [&](const FieldDesc& d) { return FloatInRange(d.range, value); },
        [&](Slot& s) { s.v.f = value; });
}

inline DDEventResult SetBool(DDEvent* event, const char* field, bool value)
{
    return WriteField(event, field, DD_FIELD_BOOL, AnyValue, [&](Slot& s) { s.v.b = value; });
}

inline DDEventResult SetString(DDEvent* event, const char* field, const char* value)
{
    return WriteField(event, field, DD_FIELD_STRING, AnyValue, [&](Slot& s) { s.v.s = value; });
}

inline DDEventResult SetPointer(DDEvent* event, const char* field, void* value)
{
    return WriteField(event, field, DD_FIELD_POINTER, AnyValue, [&](Slot& s) { s.v.p = value; });
}

inline DDEventResult Cancel(DDEvent* event)
{
    auto* payload = ToPayload(event);
    if (!payload || !payload->topic) {
        return DD_EV_E_INVALID;
    }
    if (!payload->topic->cancellable || !payload->allowWrite) {
        return DD_EV_E_NOT_CANCELLABLE;
    }
    payload->cancelled = true;
    payload->flags |= DD_EVENT_F_CANCELLED;
    return DD_EV_OK;
}

inline uint32_t Flags(const DDEvent* event)
{
    const auto* payload = ToPayload(event);
    return payload ? payload->flags : 0;
}

inline const char* TopicName(const DDEvent* event)
{
    const auto* payload = ToPayload(event);
    return payload && payload->topic ? payload->topic->name.c_str() : "";
}

inline uint32_t OriginThread(const DDEvent* event)
{
    const auto* payload = ToPayload(event);
    return payload ? payload->originThread : 0;
}

}  // namespace ddc::events::access

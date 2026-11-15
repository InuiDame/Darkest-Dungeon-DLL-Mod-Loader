#include "state_json.h"

#include <lauxlib.h>
#include <lddstep.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace ddlua::state_json {

namespace {

bool ValidUtf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) {
                return false;
            }
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // 过长编码、代理区、超出范围
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

void AppendString(std::string& out, std::string_view s)
{
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

bool IsIdentifier(std::string_view s)
{
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

std::string LimitText(std::size_t limit)
{
    if (limit == kMaxBytes) {
        return "1 MB";
    }
    return limit >= 1024 && limit % 1024 == 0 ? std::to_string(limit / 1024) + " KB" : std::to_string(limit) + " 字节";
}

struct Encoder {
    lua_State* L;
    std::string& out;
    std::string* error;
    std::size_t limit = kMaxBytes;
    bool tooLarge = false;
    std::size_t items = 0;

    bool Fail(const std::string& path, const std::string& why)
    {
        if (error) {
            *error = path + "：" + why;
        }
        return false;
    }

    bool TooLarge(const std::string& path, const char* what)
    {
        tooLarge = true;
        return Fail(path, std::string(what) + "超过 " + LimitText(limit));
    }

    std::string KeyPath(const std::string& path, int keyIndex)
    {
        if (lua_type(L, keyIndex) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, keyIndex, &length);
            const std::string_view key(text, length > 64 ? 64 : length);
            if (IsIdentifier(key) && length <= 64) {
                return path + "." + std::string(key);
            }
            std::string quoted;
            AppendString(quoted, key);
            return path + "[" + quoted + (length > 64 ? "…" : "") + "]";
        }
        if (lua_type(L, keyIndex) == LUA_TNUMBER && lua_isinteger(L, keyIndex)) {
            return path + "[" + std::to_string(lua_tointeger(L, keyIndex)) + "]";
        }
        return path + "[" + luaL_typename(L, keyIndex) + "]";
    }

    bool Value(int index, const std::string& path, int depth)
    {
        index = lua_absindex(L, index);
        if (++items > kMaxItems) {
            return Fail(path, "元素超过 " + std::to_string(kMaxItems) + " 个");
        }
        luai_ddstep(L, 1);
        if (out.size() > limit) {
            return TooLarge(path, "序列化结果");
        }
        switch (lua_type(L, index)) {
        case LUA_TBOOLEAN:
            out += lua_toboolean(L, index) ? "true" : "false";
            return true;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, index)) {
                out += std::to_string(static_cast<long long>(lua_tointeger(L, index)));
                return true;
            }
            const double v = static_cast<double>(lua_tonumber(L, index));
            if (!std::isfinite(v)) {
                return Fail(path, "不能保存 nan 或 inf");
            }
            char buffer[40];
            std::snprintf(buffer, sizeof(buffer), "%.17g", v);
            std::string text = buffer;
            if (text.find_first_of(".eEn") == std::string::npos) {
                text += ".0";  // 读回来仍是浮点数
            }
            out += text;
            return true;
        }
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            if (length > limit) {
                return TooLarge(path, "字符串");
            }
            luai_ddstep(L, static_cast<long>(length >> 4));
            if (!ValidUtf8(std::string_view(text, length))) {
                return Fail(path, "字符串不是合法的 UTF-8（JSON 存不了任意字节）");
            }
            AppendString(out, std::string_view(text, length));
            return true;
        }
        case LUA_TTABLE:
            return Table(index, path, depth + 1);
        default:
            return Fail(path, std::string("不能保存 ") + luaL_typename(L, index));
        }
    }

    bool Table(int index, const std::string& path, int depth)
    {
        if (depth > kMaxDepth) {
            return Fail(path, "嵌套超过 " + std::to_string(kMaxDepth) + " 层（或者有循环引用）");
        }
        luaL_checkstack(L, 4, "状态表嵌套太深");
        // 第一遍：判断是对象还是数组
        std::size_t count = 0;
        bool strings = false;
        bool integers = false;
        lua_Integer maxIndex = 0;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            luai_ddstep(L, 1);
            ++count;
            if (lua_type(L, -2) == LUA_TSTRING) {
                strings = true;
            } else if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1) {
                integers = true;
                maxIndex = (std::max)(maxIndex, lua_tointeger(L, -2));
            } else {
                const std::string where = KeyPath(path, lua_gettop(L) - 1);
                const std::string what = lua_type(L, -2) == LUA_TNUMBER ? "非正整数或浮点数" : luaL_typename(L, -2);
                lua_pop(L, 2);
                return Fail(where, "键只能是字符串或从 1 开始的整数，这里是 " + what);
            }
            lua_pop(L, 1);
            if (count > kMaxItems) {
                lua_pop(L, 1);
                return Fail(path, "元素超过 " + std::to_string(kMaxItems) + " 个");
            }
        }
        if (strings && integers) {
            return Fail(path, "键混用了字符串和整数（只能是全字符串的对象，或者从 1 开始的连续数组）");
        }
        if (integers) {
            if (static_cast<std::size_t>(maxIndex) != count) {
                return Fail(path, "数组有空洞（最大下标 " + std::to_string(maxIndex) + "，元素 " + std::to_string(count) + " 个）");
            }
            out += '[';
            for (lua_Integer i = 1; i <= maxIndex; ++i) {
                if (i > 1) {
                    out += ',';
                }
                lua_rawgeti(L, index, i);
                const bool ok = Value(-1, path + "[" + std::to_string(i) + "]", depth);
                lua_pop(L, 1);
                if (!ok) {
                    return false;
                }
            }
            out += ']';
            return true;
        }
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            if (!first) {
                out += ',';
            }
            first = false;
            std::size_t length = 0;
            const char* key = lua_tolstring(L, -2, &length);  // 键是字符串（上面检查过），不会改写成别的类型
            if (!ValidUtf8(std::string_view(key, length))) {
                const std::string where = KeyPath(path, lua_gettop(L) - 1);
                lua_pop(L, 2);
                return Fail(where, "键不是合法的 UTF-8");
            }
            AppendString(out, std::string_view(key, length));
            out += ':';
            const std::string where = KeyPath(path, lua_gettop(L) - 1);
            if (!Value(-1, where, depth)) {
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1);
        }
        out += '}';
        return true;
    }
};

bool PushJson(lua_State* L, const nlohmann::json& value, int depth, std::string* error)
{
    if (depth > kMaxDepth + 1) {
        if (error) {
            *error = "嵌套超过 " + std::to_string(kMaxDepth) + " 层";
        }
        return false;
    }
    luaL_checkstack(L, 3, "状态表嵌套太深");
    switch (value.type()) {
    case nlohmann::json::value_t::boolean:
        lua_pushboolean(L, value.get<bool>());
        return true;
    case nlohmann::json::value_t::number_integer:
        lua_pushinteger(L, static_cast<lua_Integer>(value.get<std::int64_t>()));
        return true;
    case nlohmann::json::value_t::number_unsigned: {
        const auto u = value.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(LUA_MAXINTEGER)) {
            lua_pushnumber(L, static_cast<lua_Number>(u));
        } else {
            lua_pushinteger(L, static_cast<lua_Integer>(u));
        }
        return true;
    }
    case nlohmann::json::value_t::number_float:
        lua_pushnumber(L, static_cast<lua_Number>(value.get<double>()));
        return true;
    case nlohmann::json::value_t::string: {
        const auto& s = value.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        return true;
    }
    case nlohmann::json::value_t::array: {
        lua_createtable(L, static_cast<int>((std::min)(value.size(), std::size_t{ 1u << 16 })), 0);
        lua_Integer i = 0;
        for (const auto& item : value) {
            ++i;
            if (item.is_null()) {
                continue;
            }
            if (!PushJson(L, item, depth + 1, error)) {
                lua_pop(L, 1);
                return false;
            }
            lua_rawseti(L, -2, i);
        }
        return true;
    }
    case nlohmann::json::value_t::object: {
        lua_createtable(L, 0, static_cast<int>((std::min)(value.size(), std::size_t{ 1u << 16 })));
        for (const auto& [key, item] : value.items()) {
            if (item.is_null()) {
                continue;
            }
            lua_pushlstring(L, key.data(), key.size());
            if (!PushJson(L, item, depth + 1, error)) {
                lua_pop(L, 2);
                return false;
            }
            lua_rawset(L, -3);
        }
        return true;
    }
    default:
        if (error) {
            *error = "不支持的 JSON 值";
        }
        return false;
    }
}

}  // namespace

bool Encode(lua_State* L, int index, const char* root, std::string& out, std::string* error, std::size_t maxBytes, bool* tooLarge)
{
    out.clear();
    if (tooLarge) {
        *tooLarge = false;
    }
    index = lua_absindex(L, index);
    if (lua_type(L, index) != LUA_TTABLE) {
        if (error) {
            *error = std::string(root) + "：必须是表，这里是 " + luaL_typename(L, index);
        }
        return false;
    }
    Encoder encoder{ L, out, error };
    encoder.limit = (std::min)(maxBytes, kMaxBytes);
    const bool ok = encoder.Table(index, root, 1) && (out.size() <= encoder.limit || encoder.TooLarge(root, "序列化结果"));
    if (tooLarge) {
        *tooLarge = encoder.tooLarge;
    }
    if (!ok) {
        out.clear();
        return false;
    }
    return true;
}

bool Decode(lua_State* L, std::string_view json, std::string* error)
{
    if (json.size() > kMaxBytes * 2) {
        if (error) {
            *error = "JSON 文本太长";
        }
        return false;
    }
    const nlohmann::json value = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (value.is_discarded()) {
        if (error) {
            *error = "JSON 解析失败";
        }
        return false;
    }
    if (!value.is_object() && !value.is_array()) {
        if (error) {
            *error = "状态必须是对象或数组";
        }
        return false;
    }
    return PushJson(L, value, 1, error);
}

}  // namespace ddlua::state_json

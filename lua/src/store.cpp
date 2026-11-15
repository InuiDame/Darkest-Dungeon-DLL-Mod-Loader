#include "store.h"

#include <dd/dd_api.h>

#include <cstdlib>

namespace ddlua::store {

namespace {

bool ParseU32(std::string_view text, std::uint32_t* out)
{
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

std::string Encode(const Payload& payload)
{
    std::string out = "DDLUA-STORE 1\n";
    out += "store " + std::to_string(payload.store.size()) + "\n" + payload.store + "\n";
    for (const auto& b : payload.buffs) {
        out += std::string("buff ") + (b.kind == DD_ACTOR_MONSTER ? "monster" : "hero") + " " + std::to_string(b.id) + " " + std::to_string(b.roster) + " " +
            std::to_string(b.guid) + " " + std::to_string(b.json.size()) + " " + b.buffId + "\n" + b.json + "\n";
    }
    return out;
}

bool Decode(std::string_view bytes, Payload* out, std::string* why)
{
    const auto fail = [&](std::string text) {
        if (why) {
            *why = std::move(text);
        }
        return false;
    };
    Payload p;
    std::size_t pos = 0;
    const auto line = [&](std::string_view* text) {
        const std::size_t end = bytes.find('\n', pos);
        if (end == std::string_view::npos) {
            return false;
        }
        *text = bytes.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    const auto body = [&](std::uint32_t size, std::string* text) {
        if (bytes.size() - pos < static_cast<std::size_t>(size) + 1 || bytes[pos + size] != '\n') {
            return false;
        }
        text->assign(bytes.substr(pos, size));
        pos += static_cast<std::size_t>(size) + 1;
        return true;
    };
    std::string_view head;
    if (!line(&head) || head != "DDLUA-STORE 1") {
        return fail("不是 DDLua 的旁路数据（第一行不对）");
    }
    bool haveStore = false;
    while (pos < bytes.size()) {
        std::string_view text;
        if (!line(&text)) {
            return fail("最后一段没有结束（数据被截断？）");
        }
        if (text.rfind("store ", 0) == 0) {
            std::uint32_t size = 0;
            if (haveStore || !ParseU32(text.substr(6), &size) || !body(size, &p.store)) {
                return fail("store 段不对");
            }
            haveStore = true;
        } else if (text.rfind("buff ", 0) == 0) {
            // buff <kind> <id> <roster> <guid> <字节数> <buff id>（buff id 是这一行剩下的部分）
            std::string_view rest = text.substr(5);
            std::string_view parts[5];
            for (auto& part : parts) {
                const std::size_t space = rest.find(' ');
                if (space == std::string_view::npos) {
                    return fail("buff 行的列数不对：" + std::string(text));
                }
                part = rest.substr(0, space);
                rest = rest.substr(space + 1);
            }
            BuffState b;
            std::uint32_t size = 0;
            if (parts[0] == "hero") {
                b.kind = DD_ACTOR_HERO;
            } else if (parts[0] == "monster") {
                b.kind = DD_ACTOR_MONSTER;
            } else {
                return fail("buff 行的 actor 种类不对：" + std::string(parts[0]));
            }
            if (!ParseU32(parts[1], &b.id) || !ParseU32(parts[2], &b.roster) || !ParseU32(parts[3], &b.guid) || !ParseU32(parts[4], &size) ||
                rest.empty()) {
                return fail("buff 行的数字不对：" + std::string(text));
            }
            b.buffId = rest;
            if (!body(size, &b.json)) {
                return fail("buff 段的长度不对（" + b.buffId + "）");
            }
            p.buffs.push_back(std::move(b));
        } else {
            return fail("不认识的段：" + std::string(text.substr(0, 40)));
        }
    }
    if (!haveStore) {
        return fail("没有 store 段");
    }
    if (out) {
        *out = std::move(p);
    }
    return true;
}

}  // namespace ddlua::store

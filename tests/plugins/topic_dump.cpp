// topic_dump：把 DDCore 的主题表（events 服务的 topic_* 查询）写成 JSON，tools/gen_lua_types.py 用它生成
// LuaLS 的主题注解（sdk/lua/dd_topics.lua）。只查询主题表：不订阅事件、不装 hook，只订阅 lifecycle AssetsReady。
// 输出：<游戏根目录>\DDModLoader\log\topics.json（AssetsReady 时写，那时原生主题的可用性已经确定）。
// 部署：rig.py deploy topics。

#include <dd/dd.hpp>

#include <windows.h>

#include <cstdio>
#include <string>

static dd::Runtime g_dd;

namespace {

std::string Escape(const char* text)
{
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
        switch (*p) {
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
            if (*p < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", *p);
                out += buffer;
            } else {
                out += static_cast<char>(*p);  // UTF-8 原样
            }
        }
    }
    return out;
}

const char* KindText(std::uint32_t kind)
{
    switch (kind) {
    case 0:
        return "native";
    case 1:
        return "query";
    case 2:
        return "semantic";
    case 3:
        return "custom";
    case 4:
        return "app";
    default:
        return "unknown";
    }
}

const char* TypeText(std::uint32_t type)
{
    switch (type) {
    case DD_FIELD_INT:
        return "int";
    case DD_FIELD_FLOAT:
        return "float";
    case DD_FIELD_BOOL:
        return "bool";
    case DD_FIELD_STRING:
        return "string";
    case DD_FIELD_POINTER:
        return "pointer";
    default:
        return "unknown";
    }
}

std::wstring Widen(const char* text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    }
    return out;
}

void Dump()
{
    const DDEventsService* events = g_dd.events().service();
    const std::uint32_t count = events->topic_count();
    std::string json = "{\n";
    json += "  \"exe_sha1\": \"" + Escape(g_dd.game()->exe_sha1()) + "\",\n";
    json += "  \"core_version\": \"" + Escape(g_dd.game()->core_version()) + "\",\n";
    json += "  \"topics\": [\n";
    std::uint32_t written = 0;
    std::uint32_t fields = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        DDEventTopicInfo info{};
        if (!events->topic_at(i, &info) || !info.name) {
            continue;
        }
        const char* reason = nullptr;
        const bool available = events->topic_available(info.name, &reason);
        json += std::string(written ? ",\n" : "") + "    {\"name\": \"" + Escape(info.name) + "\", \"kind\": \"" + KindText(info.kind) +
            "\", \"writable\": " + (info.writable ? "true" : "false") + ", \"cancellable\": " + (info.cancellable ? "true" : "false") +
            ", \"available\": " + (available ? "true" : "false") + ", \"reason\": \"" + Escape(available ? "" : reason) +
            "\",\n     \"fields\": [";
        std::uint32_t topicFields = 0;  // 本主题已写出的字段数：查询失败跳过的字段不能让下一个字段以逗号开头
        for (std::uint32_t f = 0; f < info.field_count; ++f) {
            DDEventFieldInfo field{};
            if (!events->topic_field_at(info.name, f, &field) || !field.name) {
                continue;
            }
            json += std::string(topicFields ? ", " : "") + "{\"name\": \"" + Escape(field.name) + "\", \"type\": \"" + TypeText(field.type) +
                "\", \"write\": " + ((field.access & DD_FIELD_WRITE) ? "true" : "false") + "}";
            ++topicFields;
            ++fields;
        }
        json += "]}";
        ++written;
    }
    json += "\n  ]\n}\n";

    const std::wstring path = Widen(g_dd.game()->game_root()) + L"\\DDModLoader\\log\\topics.json";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        g_dd.log().error("写不了 topics.json");
        return;
    }
    std::fwrite(json.data(), 1, json.size(), file);
    std::fclose(file);
    g_dd.log().info("主题表已写入 DDModLoader\\log\\topics.json：主题 {} 个，字段 {} 个", written, fields);
}

}  // namespace

DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out)
{
    out->abi_version = DD_ABI_VERSION;
    out->name = "topic_dump";
    out->version = DD_VERSION_MAKE(1, 0, 0);
    return true;
}

DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api)
{
    if (!g_dd.init(api) || !g_dd.events()) {
        return false;
    }
    g_dd.lifecycle().subscribe(DD_PHASE_ASSETS_READY, [](const DDPhaseEvent&) { Dump(); });
    return true;
}

DD_EXPORT void DD_CALL DDPlugin_Unload(void) {}

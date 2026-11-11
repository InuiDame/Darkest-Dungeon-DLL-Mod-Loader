#include "config.h"

#include <charconv>

namespace ddlua {

namespace {

std::string_view Trim(std::string_view text)
{
    const auto isSpace = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool ParseUint(std::string_view text, std::uint32_t minValue, std::uint32_t maxValue, std::uint32_t& out)
{
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minValue || value > maxValue) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

Config ParseConfig(std::string_view text, std::vector<std::string>* warnings)
{
    Config config;
    const auto warn = [&](std::string message) {
        if (warnings) {
            warnings->push_back(std::move(message));
        }
    };
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    int lineNo = 0;
    while (!text.empty()) {
        ++lineNo;
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            warn("第 " + std::to_string(lineNo) + " 行没有 '='，已忽略");
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        const auto bad = [&] { warn("第 " + std::to_string(lineNo) + " 行 " + std::string(key) + " 的值不合法：" + std::string(value) + "，用默认值"); };
        if (key == "memory_limit_mb") {
            if (!ParseUint(value, 16, 4096, config.memoryLimitMb)) {
                bad();
            }
        } else if (key == "budget_ms") {
            if (!ParseUint(value, 1, 60000, config.budgetMs)) {
                bad();
            }
        } else if (key == "load_budget_ms") {
            if (!ParseUint(value, 1, 600000, config.loadBudgetMs)) {
                bad();
            }
        } else if (key == "max_failures") {
            if (!ParseUint(value, 1, 1000, config.maxFailures)) {
                bad();
            }
        } else if (key == "max_per_mod") {
            if (!ParseUint(value, 1, 100000, config.maxPerMod)) {
                bad();
            }
        } else if (key == "frame_budget_ms") {
            if (!ParseUint(value, 1, 60000, config.frameBudgetMs)) {
                bad();
            }
        } else if (key == "log_lines_per_sec") {
            if (!ParseUint(value, 1, 100000, config.logLinesPerSec)) {
                bad();
            }
        } else if (key == "log_max_mb") {
            if (!ParseUint(value, 1, 4096, config.logMaxMb)) {
                bad();
            }
        } else if (key == "teardown_budget_ms") {
            if (!ParseUint(value, 1, 600000, config.teardownBudgetMs)) {
                bad();
            }
        } else if (key == "dev_mode") {
            std::uint32_t flag = 0;
            if (ParseUint(value, 0, 1, flag)) {
                config.devMode = flag != 0;
            } else {
                bad();
            }
        } else if (key == "dev_global_dirs") {
            config.devGlobalDirs.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto semi = rest.find(';');
                const std::string_view item = Trim(rest.substr(0, semi));
                rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
                if (!item.empty()) {
                    config.devGlobalDirs.emplace_back(item);
                }
            }
        } else {
            warn("第 " + std::to_string(lineNo) + " 行：未知的键 " + std::string(key) + "，已忽略");
        }
    }
    return config;
}

}  // namespace ddlua

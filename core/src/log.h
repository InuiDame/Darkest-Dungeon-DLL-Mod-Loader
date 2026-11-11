#pragma once
// DDCore 的日志：自身写 DDCore.log，每个插件写 <插件名>.log。UTF-8 带 BOM，每行落盘。

#include <cstdarg>
#include <memory>
#include <string>
#include <string_view>

namespace ddc {

const char* LevelText(int level);

class LogFile
{
public:
    LogFile();
    ~LogFile();
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    bool Open(const std::wstring& path);
    void Write(int level, std::string_view message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void CoreLogInit(const std::wstring& logDir);
void CoreLogV(int level, const char* format, va_list args);
void LogInfo(const char* format, ...);
void LogWarn(const char* format, ...);
void LogError(const char* format, ...);

}  // namespace ddc

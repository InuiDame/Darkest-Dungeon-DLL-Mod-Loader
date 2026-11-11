#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace ddc {

const char* LevelText(int level)
{
    switch (level) {
    case 0:
        return "TRACE";
    case 1:
        return "DEBUG";
    case 2:
        return "INFO";
    case 3:
        return "WARN";
    case 4:
        return "ERROR";
    default:
        return "?";
    }
}

struct LogFile::Impl {
    std::mutex mutex;
    HANDLE file = INVALID_HANDLE_VALUE;
};

LogFile::LogFile() : impl_(std::make_unique<Impl>()) {}

LogFile::~LogFile()
{
    if (impl_->file != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->file);
    }
}

bool LogFile::Open(const std::wstring& path)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->file != INVALID_HANDLE_VALUE) {
        return true;
    }
    impl_->file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) {
        return false;
    }
    static const unsigned char kBom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    WriteFile(impl_->file, kBom, sizeof(kBom), &written, nullptr);
    return true;
}

void LogFile::Write(int level, std::string_view message)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    const int n = std::snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03d] [%s] ", st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, LevelText(level));
    std::string line;
    line.reserve(static_cast<std::size_t>(n) + message.size() + 2);
    line.append(prefix, static_cast<std::size_t>(n));
    line.append(message);
    line.append("\r\n");

    std::lock_guard lock(impl_->mutex);
    if (impl_->file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(impl_->file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}

namespace {

LogFile& CoreFile()
{
    static auto* file = new LogFile();  // 不析构，进程退出时其他线程可能还在写
    return *file;
}

}  // namespace

void CoreLogInit(const std::wstring& logDir)
{
    CoreFile().Open(logDir + L"\\DDCore.log");
}

void CoreLogV(int level, const char* format, va_list args)
{
    char message[2048];
    int n = std::vsnprintf(message, sizeof(message), format, args);
    if (n < 0) {
        n = 0;
    }
    if (n >= static_cast<int>(sizeof(message))) {
        n = static_cast<int>(sizeof(message)) - 1;
    }
    message[n] = 0;
    CoreFile().Write(level, std::string_view(message, static_cast<std::size_t>(n)));
    OutputDebugStringA((std::string("[DDCore] ") + message + "\n").c_str());
}

#define DDC_DEFINE_LOG(name, level)                                  \
    void name(const char* format, ...)                               \
    {                                                                \
        va_list args;                                                \
        va_start(args, format);                                      \
        CoreLogV(level, format, args);                               \
        va_end(args);                                                \
    }

DDC_DEFINE_LOG(LogInfo, 2)
DDC_DEFINE_LOG(LogWarn, 3)
DDC_DEFINE_LOG(LogError, 4)

#undef DDC_DEFINE_LOG

}  // namespace ddc

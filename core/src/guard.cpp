#include "guard.h"

#include <malloc.h>

#include <cstdio>
#include <cstring>

namespace ddc {

namespace {

int RecordFault(EXCEPTION_POINTERS* info, Fault* fault)
{
    if (fault && info && info->ExceptionRecord) {
        fault->code = info->ExceptionRecord->ExceptionCode;
        fault->address = info->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

bool GuardedInvoke(void (*fn)(void*), void* arg, Fault* fault)
{
    __try {
        fn(arg);
        return true;
    } __except (RecordFault(GetExceptionInformation(), fault)) {
        if (fault && fault->code == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        return false;
    }
}

std::string DescribeAddress(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<const char*>(address), &module) ||
        !module) {
        return "未知模块";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    char buffer[MAX_PATH + 32];
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llX", slash ? slash + 1 : path,
        static_cast<unsigned long long>(
            reinterpret_cast<const std::uint8_t*>(address) - reinterpret_cast<const std::uint8_t*>(module)));
    return buffer;
}

std::string DescribeFault(const Fault& fault)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "异常码 %08lX，地址 %p", fault.code, fault.address);
    return std::string(buffer) + "（" + DescribeAddress(fault.address) + "）";
}

}  // namespace ddc

/* 用 C 编译器编译一次 SDK 头，确认它是纯 C。不被调用，只检查能否通过编译。 */
#include <dd/dd_api.h>

const DDServiceHeader* dd_core_tests_c_compat(const DDApi* api)
{
    const DDLogService* log = (const DDLogService*)api->query(DD_SERVICE_LOG, DD_SERVICE_LOG_VERSION);
    return log ? &log->header : NULL;
}

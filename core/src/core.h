#pragma once

#include "../../common/boot_abi.h"

namespace ddc {

// 加载器调用，都在主线程
int CoreBoot(const dd::common::CoreBootInfo* info);
void CoreAppSystemsCreated();

}  // namespace ddc

/**
 * @file esf_dump.hpp
 * @author qingyu
 * @brief 异常栈帧转储接口 — 架构层只声明，实现放在子树 drivers
 * @version 0.1
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <zephyr/kernel.h>

/**
 * @brief 转储异常栈帧的架构相关字段
 *
 * 架构层只声明接口；具体实现放在 zephyr_user/drivers/fatal/<vendor>/。
 * fatal.cpp 内提供 __weak 空实现，板卡未选实现时链接不报错。
 *
 * @param esf 异常栈帧（可能为 nullptr）
 */
void DumpEsf(const struct arch_esf* esf);

/**
 * @file esf_dump.hpp
 * @author qingyu
 * @brief 异常栈帧转储接口 — 架构层只声明，实现放在子树 drivers
 * @version 0.2
 * @date 2026-10-21
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

/**
 * @brief 致命上下文同步输出一行
 *
 * 逐字节轮询 UART 发送寄存器直发，不经 DMA、不入日志队列、不等中断回调，
 * 也不依赖 shell 线程调度。fatal handler 内中断已关且随后永久自旋，只有
 * 这条路径能保证字符真的发出。
 *
 * @param text 以 '\0' 结尾的字符串
 */
void FatalOut(const char* text);

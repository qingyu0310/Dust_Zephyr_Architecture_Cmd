/**
 * @file fatal.cpp
 * @author qingyu
 * @brief 致命错误处理 — 栈溢出/CPU 异常经 DUST_LOG 上报
 * @version 0.1
 * @date 2026-08-07
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal_types.h>
#include "log.hpp"

/**
 * @brief 致命错误原因字符串（K_ERR_*）
 * @param reason 致命错误原因
 * @return 原因描述
 */
static const char* FatalReasonStr(unsigned int reason)
{
    switch (reason)
    {
        case K_ERR_CPU_EXCEPTION:  return "CPU_EXCEPTION";
        case K_ERR_SPURIOUS_IRQ:   return "SPURIOUS_IRQ";
        case K_ERR_STACK_CHK_FAIL: return "STACK_CHK_FAIL";
        case K_ERR_KERNEL_OOPS:    return "KERNEL_OOPS";
        case K_ERR_KERNEL_PANIC:   return "KERNEL_PANIC";
        default:                   return "UNKNOWN";
    }
}

/**
 * @brief 覆盖 Zephyr 弱定义致命错误处理器，让栈溢出经 DUST_LOG 可见
 * @param reason 致命错误原因（K_ERR_*，栈溢出 = K_ERR_STACK_CHK_FAIL=2）
 * @param esf    异常栈帧（含 mepc/mstatus）
 *
 * Zephyr 默认实现走 LOG_ERR + printk，本项目 CONFIG_LOG/CONSOLE 关闭导致
 * 输出被吞。覆盖后用 DUST_LOG_ERR 上报原因 + 线程名 + mcause/mepc：
 * PMP 栈溢出触发 mcause=5/7（load/store access fault）。打印后停机等待
 * 复位，不重启以保留现场。
 */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    unsigned long mcause = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));

    const char* thread = k_thread_name_get(k_current_get());
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s mcause=%lx mepc=%lx mstatus=%lx",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?", mcause, esf->mepc, esf->mstatus);

    while (1) {}
}

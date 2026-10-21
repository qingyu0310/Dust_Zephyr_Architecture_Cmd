/**
 * @file fatal.cpp
 * @author qingyu
 * @brief 致命错误处理 — 冻结业务线程、保留 shell、上报现场
 * @version 0.5
 * @date 2026-10-21
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal_types.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/devicetree.h>
#include "esf_dump.hpp"

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
 * @brief 覆盖 Zephyr 弱定义致命错误处理器，直接串口输出现场
 * @param reason 致命错误原因（K_ERR_*，栈溢出 = K_ERR_STACK_CHK_FAIL=2）
 * @param esf    异常栈帧（架构相关，交给 DumpEsf 处理）
 *
 * Zephyr 默认实现走 LOG_ERR + printk，本项目 CONFIG_LOG/CONSOLE 关闭导致
 * 输出被吞。DUST_LOG_ERR 走异步队列依赖 shell 线程；Log::SendCommandLine
 * 在 DMA 忙时只入池，续发要靠 TX_DONE 中断唤醒 shell 线程，而本 handler
 * 内中断已关且随后自旋，第二帧起永远发不出去。因此改用 FatalOut 轮询直发。
 * 架构相关寄存器由 DumpEsf() 输出（实现在子树 drivers/fatal/<vendor>/）。
 * 打印后停机等待复位，不重启以保留现场。
 */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

    char line[128];
    snprintf(line, sizeof(line), "[fatal] %s (reason=%u) thread=%s", FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?");
    FatalOut(line);

    DumpEsf(esf);

    while (1) {}
}

/**
 * @brief 致命上下文同步输出一行（逐字节轮询 UART 发送寄存器）
 * @param text 以 '\0' 结尾的字符串
 *
 * 不走 DMA、不入日志队列、不等中断回调、不依赖 shell 线程：fatal handler
 * 内中断已由 z_fatal_error 关闭且随后永久自旋，只有轮询直发能保证发出。
 */
void FatalOut(const char* text)
{
    const struct device* dev = DEVICE_DT_GET(DT_ALIAS(shell_uart));

    for (const char* p = text; *p != '\0'; ++p) {
        uart_poll_out(dev, static_cast<unsigned char>(*p));
    }
    uart_poll_out(dev, '\r');
    uart_poll_out(dev, '\n');
}

/**
 * @brief 默认空实现：板卡没选 esf dump 实现时链接不报错
 * @param esf 异常栈帧（未使用）
 */
__weak void DumpEsf(const struct arch_esf* esf)
{
    ARG_UNUSED(esf);
    FatalOut("[fatal] esf dump not configured for this board");
}

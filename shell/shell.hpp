/**
 * @file shell.hpp
 * @author qingyu
 * @brief 自维护 UART 线程底座 — 接收循环 + 命令分发（var/log 子命令）
 * @version 0.2
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL

#include <cstdint>
#include <zephyr/kernel.h>
#include "uart.hpp"
#include "thread.hpp"

namespace debug
{

/**
 * @brief 调试控制台线程底座
 *
 * UART 接收循环（k_sem_take 阻塞）→ 逐行解析 → ProcessLine 分发：
 * var → Var::Process、log → Log::Process、h/? → 帮助。
 * var/log 子命令由各自的模块实现（var.hpp / log.hpp）。
 */
class Shell final
{
public:
    bool Init(UartDma &uart);
    void Start(ThreadPrio prio = ThreadPrio::Lowest)
    {
        thread_.Start(TaskEntry, prio, this, "shell");
    }

private:
    static constexpr size_t kLineBufSize = 128;

    UartDma           *uart_     = nullptr;
    uint8_t            line_buf_[kLineBufSize]{};
    uint32_t           line_pos_ = 0;
    Thread<2048>       thread_   {};

    void CmdHelp() const;
    void ProcessLine(uint8_t *line);
    void Task();

    static void TaskEntry(void *p1, void *p2, void *p3)
    {
        ARG_UNUSED(p2);
        ARG_UNUSED(p3);
        auto *self = static_cast<Shell*>(p1);
        self->Task();
    }
};

} // namespace debug

#endif

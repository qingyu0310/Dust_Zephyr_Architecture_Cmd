/**
 * @file shell.cpp
 * @author qingyu
 * @brief 自维护 UART 线程底座 — 命令表分派 + 初始化接入 DUST_LOG
 * @version 0.6
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "shell.hpp"
#include "shell_cmd.hpp"
#include "token.hpp"
#include "log.hpp"
#include "Init_entry.hpp"
#include <cstdio>
#include <cstring>
#include "uart.hpp"
#include "zephyr/kernel.h"

#pragma message "Compiling Cmd/Shell/"

namespace debug {

/**
 * @brief 绑定 UART DMA 实例，初始化完成
 * @param uart 外部创建的 UART DMA 实例引用
 * @return true
 */
bool Shell::Init(Stream &stream)
{
    stream_ = &stream;
    return true;
}

/**
 * @brief 输出帮助：遍历命令表，每族一行
 *
 * 命令族由各模块 REGISTER_SHELL_CMD 注册，帮助文本随注册项自动出现。
 */
void Shell::PrintHelp()
{
    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        char line[80];
        snprintf(line, sizeof(line), "%-8s %s", e->name, e->help);
        Log::SendCommandLine(line);
    }
}

/**
 * @brief 解析一行命令并按命令表分派
 *
 * 取第一个 token 作为命令族名，在 .shell_cmd 段内线性匹配，
 * 命中则把剩余参数串交给该族的 handler。
 *
 * @param line 行缓冲指针（可写，解析时会被修改）
 */
void Shell::ProcessLine(uint8_t *line)
{
    const char* cmd = nullptr;
    uint8_t* rest   = NextToken(line, cmd);

    if (*cmd == '\0') return;

    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        if (std::strcmp(e->name, cmd) == 0)
        {
            e->handler(rest);
            return;
        }
    }

    Log::SendCommandLine("unknown command");
}

/**
 * @brief 帮助命令入口（注册到命令表）
 * @param args 未使用
 */
static void HelpCmd(uint8_t* args)
{
    (void)args;
    Shell::PrintHelp();
}

REGISTER_SHELL_CMD("h", "help", HelpCmd, help);

/**
 * @brief 线程主循环，等待 UART 数据并逐行处理
 */
void Shell::Task()
{
    for (;;)
    {
        k_sem_take(&stream_->sem_, K_FOREVER);         // 通道事件：接收数据 OR 日志发送需要驱动

        Log::SendNextFrameIfIdle();                   				// 先驱动日志发送（DMA 空闲则续发）

        while (Log::FormatOnePendingRecord())         				// 出队参数快照请求 → 展开入帧池 → 泵
        {
            Log::SendNextFrameIfIdle();
        }

        uint8_t buf[32];
        uint16_t n = stream_->Read(buf, sizeof(buf));
        if (n == 0) continue;                         // 发送唤醒，无接收数据

        for (uint16_t i = 0; i < n; i++)
        {
            uint8_t ch = buf[i];

            if (ch == '\r' || ch == '\n')
            {
                line_buf_[line_pos_] = 0;
                if (line_pos_ > 0)
                {
                    ProcessLine(line_buf_);
                }
                line_pos_ = 0;
                continue;
            }

            if (ch == '\b' || ch == 0x7F)
            {
                if (line_pos_ > 0) line_pos_--;
                continue;
            }

            if (line_pos_ < kLineBufSize - 1)
            {
                line_buf_[line_pos_++] = ch;
            }
        }
    }
}

static Shell shell_;

/**
 * @brief 初始化 console UART 和 dbg 控制台
 *
 * UartDma 的 tx_cb 注册 Log::HandleTxDone（发送完成回调，仲裁器补发挂起帧），
 * 随后初始化 rx、接入 Shell、初始化并绑定 DUST_LOG 发送通道。
 *
 * @return true 初始化成功
 */
static bool thread_init()
{
    static UartDma rx {};

    UartDma::Config cfg;
    cfg.line_cfg.baudrate = 921600;
    cfg.base_cfg.tx_cb    = Log::HandleTxDone;

    if (!rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg)) return false;

    if (!shell_.Init(rx)) return false;

    Log::Init();
    Log::BindOutputStream(&rx);

    DUST_LOG_INF("shell init");

    return true;
}

/**
 * @brief 启动 dbg 线程
 * @return true 启动成功
 */
static bool thread_start()
{
    shell_.SetKeepAlive(true);      // fatal 冻结业务线程时保留 shell（日志消费者 + 观察窗口）
    shell_.Start();
    Log::MarkShellThreadReady();                  		// 首次运行即接管发送（重复置 true 无害）
    DUST_LOG_INF("shell send owner taken");        // 发送模式切换：调用点直发 → shell 线程驱动

    return true;
}

REGISTER_INIT  (thread_init,  PreInit, High, HaltOnFail, nullptr, "dbg_init");
REGISTER_THREAD(thread_start, PreThread, HaltOnFail, "dbg_start");

} // namespace debug

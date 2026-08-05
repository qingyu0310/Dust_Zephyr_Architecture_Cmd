/**
 * @file shell.cpp
 * @author qingyu
 * @brief 自维护 UART 线程底座 — 命令分发 + 初始化接入 DUST_LOG
 * @version 0.2
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "shell.hpp"
#include "var.hpp"
#include "log.hpp"
#include "Init_entry.hpp"
#include <cstring>

#pragma message "Compiling Cmd/Shell/Debug"

namespace debug {

/**
 * @brief 绑定 UART DMA 实例，初始化完成
 * @param uart 外部创建的 UART DMA 实例引用
 * @return true
 */
bool Shell::Init(UartDma &uart)
{
    uart_ = &uart;
    return true;
}

/**
 * @brief 输出命令帮助
 */
void Shell::CmdHelp() const
{
    Log::SendLine("var list                列出所有调试变量");
    Log::SendLine("var get <name>          查看变量");
    Log::SendLine("var set <name> <val>    修改变量");
    Log::SendLine("log list                列出所有日志条目（含选中状态）");
    Log::SendLine("log on <name>           选中某条日志流式打印（同一时间只打一条）");
    Log::SendLine("log off                 停止打印");
    Log::SendLine("h/?                     帮助");
}

/**
 * @brief 解析一行命令并分派到对应的命令族
 * @param line 行缓冲指针（可写，解析时会被修改）
 */
void Shell::ProcessLine(uint8_t *line)
{
    while (*line == ' ') line++;
    if (*line == '\0' || *line == '\r' || *line == '\n') return;

    // 解析第一个 token：命令族
    uint8_t *cmd = line;
    while (*line && *line != ' ') line++;
    if (*line == ' ') { *line = '\0'; line++; }
    while (*line == ' ') line++;

    if (std::strcmp(reinterpret_cast<const char*>(cmd), "var") == 0)
    {
        Var::Process(line);          // var list/get/set —— 实现在 var.cpp
    }
    else if (std::strcmp(reinterpret_cast<const char*>(cmd), "log") == 0)
    {
        Log::Process(line);          // log list/on/off —— 实现在 log.cpp
    }
    else if (std::strcmp(reinterpret_cast<const char*>(cmd), "h") == 0 || std::strcmp(reinterpret_cast<const char*>(cmd), "?") == 0)
    {
        CmdHelp();
    }
    else
    {
        Log::SendLine("?: var/log/h");
    }
}

/**
 * @brief 线程主循环，等待 UART 数据并逐行处理
 */
void Shell::Task()
{
    for (;;)
    {
        k_sem_take(&uart_->sem_, K_FOREVER);

        uint8_t buf[32];
        uint16_t n = uart_->Read(buf, sizeof(buf));
        if (n == 0) continue;

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
 * UartDma 的 tx_cb 注册 Log::OnTxDone（发送完成回调，仲裁器补发挂起帧），
 * 随后初始化 rx、接入 Shell、初始化并绑定 DUST_LOG 发送通道。
 *
 * @return true 初始化成功
 */
static bool thread_init()
{
    static UartDma rx {};

    UartDma::Config cfg;
    cfg.line_cfg.baudrate = 921600;
    cfg.base_cfg.tx_cb    = Log::OnTxDone;

    if (!rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg))
    {
        Log::Err("console uart init failed");
        return false;
    }

    if (!shell_.Init(rx)) return false;

    Log::Init();
    Log::BindUart(&rx);
    return true;
}

/**
 * @brief 启动 dbg 线程
 * @return true 启动成功
 */
static bool thread_start()
{
    shell_.Start();
    return true;
}

REGISTER_INIT  (thread_init,  PreInit,    High, "dbg_init");
REGISTER_THREAD(thread_start, LateThread, Low,  "dbg_start");

} // namespace debug

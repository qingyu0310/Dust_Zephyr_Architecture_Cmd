/**
 * @file shell.hpp
 * @author qingyu
 * @brief 自维护 UART 线程底座 — 接收循环 + 命令族分派
 * @version 0.5
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL

#include <cstdint>
#include <zephyr/kernel.h>
#include "stream.hpp"
#include "thread.hpp"

namespace debug
{

/**
 * @brief 调试控制台线程底座
 *
 * UART 接收循环（k_sem_take 阻塞）→ 逐行解析 → ProcessLine 按命令表分派。
 * 命令族由各自模块用 REGISTER_SHELL_CMD 注册到 .shell_cmd 段，本类不感知
 * 具体有哪些命令；帮助文本也由注册项自动生成。
 */
class Shell final
{
public:
    bool Init(Stream &stream);
    void Start(ThreadPrio prio = ThreadPrio::Lowest)
    {
        thread_.Start(TaskEntry, prio, this, "shell");
    }

    /**
     * @brief 暂停 / 恢复 / 查询暂停状态（转发给底层线程）
     */
    void Pause()  { thread_.Pause(); }
    void Resume() { thread_.Resume(); }
    bool Paused() const { return thread_.Paused(); }

    /**
     * @brief 设置「保留标记」：致命错误冻结业务线程时本线程继续运行
     *
     * shell 是日志的唯一消费者和唯一的现场观察窗口，不能被冻结。
     *
     * @param on true=保留
     */
    void SetKeepAlive(bool on) { thread_.SetKeepAlive(on); }

    /**
     * @brief 输出帮助：遍历命令表，每族一行
     */
    static void PrintHelp();

private:
    static constexpr size_t kLineBufSize = 128;

    Stream       *stream_   = nullptr;
    uint8_t       line_buf_[kLineBufSize]{};
    uint32_t      line_pos_ = 0;
    Thread<2048>  thread_   {};

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

/**
 * @file thread_cmd.cpp
 * @author qingyu
 * @brief thread 命令实现 — list/show/pause/resume/reset（输出走 DUST_LOG 命令响应）
 * @version 0.2
 * @date 2026-10-19
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "thread_cmd.hpp"
#include "thread_registry.hpp"
#include "log.hpp"
#include <cstdio>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Thread_cmd"

namespace debug {

/**
 * @brief 线程状态字符串
 * @param s 统计快照
 * @return 固定字符串
 */
static const char* StateName(const ThreadStats& s)
{
    if (!s.running) return "dead";
    if (s.paused)   return "paused";
    return "running";
}

/**
 * @brief thread list：遍历注册表，每个线程一行
 */
void ThreadCmd::CmdList()
{
    Log::SendCommandLine("THREAD          STATE     RUN      RETRY   FATAL   DROP    STACK");

    const uint8_t count = thread_registry::Count();
    for (uint8_t i = 0; i < count; ++i)
    {
        ThreadBase* t = thread_registry::At(i);
        if (t == nullptr) continue;

        ThreadStats s {};
        if (!t->GetStats(s)) continue;

        char line[128];
        snprintf(line, sizeof(line), "%-15s %-9s %-8u %-7u %-7u %-7u %u",
                 s.name, StateName(s),
                 (unsigned)s.run_count, (unsigned)s.retry_count,
                 (unsigned)s.fatal_count, (unsigned)s.drop_count,
                 (unsigned)s.stack_unused);
        Log::SendCommandLine(line);
    }

    char tail[32];
    snprintf(tail, sizeof(tail), "--- %u threads ---", (unsigned)count);
    Log::SendCommandLine(tail);
}

/**
 * @brief thread show：单线程详细统计
 * @param name 线程名
 */
void ThreadCmd::CmdShow(const char* name)
{
    ThreadBase* t = thread_registry::Find(name);
    if (t == nullptr) {
        Log::SendCommandLine("thread show: not found");
        return;
    }

    ThreadStats s {};
    if (!t->GetStats(s)) {
        Log::SendCommandLine("thread show: not started");
        return;
    }

    char line[128];

    snprintf(line, sizeof(line), "%s:", s.name);
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  state        %s", StateName(s));
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  run          %u", (unsigned)s.run_count);
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  retry/fatal  %u/%u", (unsigned)s.retry_count, (unsigned)s.fatal_count);
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  drop         %u", (unsigned)s.drop_count);
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  last         %u.%03u ms",
             (unsigned)(s.last_run_ns / 1000000ULL),
             (unsigned)((s.last_run_ns / 1000ULL) % 1000ULL));
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  max          %u.%03u ms",
             (unsigned)(s.max_run_ns / 1000000ULL),
             (unsigned)((s.max_run_ns / 1000ULL) % 1000ULL));
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  total        %u.%03u ms",
             (unsigned)(s.run_ns / 1000000ULL),
             (unsigned)((s.run_ns / 1000ULL) % 1000ULL));
    Log::SendCommandLine(line);

    snprintf(line, sizeof(line), "  stack        %u B unused", (unsigned)s.stack_unused);
    Log::SendCommandLine(line);
}

/**
 * @brief thread pause：暂停线程（保留全部现场）
 * @param name 线程名
 */
void ThreadCmd::CmdPause(const char* name)
{
    ThreadBase* t = thread_registry::Find(name);
    if (t == nullptr) {
        Log::SendCommandLine("thread pause: not found");
        return;
    }

    t->Pause();
    Log::SendCommandLine("thread pause: ok");
}

/**
 * @brief thread resume：恢复被暂停的线程
 * @param name 线程名
 */
void ThreadCmd::CmdResume(const char* name)
{
    ThreadBase* t = thread_registry::Find(name);
    if (t == nullptr) {
        Log::SendCommandLine("thread resume: not found");
        return;
    }

    t->Resume();
    Log::SendCommandLine("thread resume: ok");
}

/**
 * @brief thread reset：清零线程计数器
 * @param name 线程名
 */
void ThreadCmd::CmdReset(const char* name)
{
    ThreadBase* t = thread_registry::Find(name);
    if (t == nullptr) {
        Log::SendCommandLine("thread reset: not found");
        return;
    }

    t->ResetStats();
    Log::SendCommandLine("thread reset: ok");
}

/**
 * @brief thread 命令入口
 * @param line 子命令参数（不含 "thread" 前缀）
 */
void ThreadCmd::Process(uint8_t* line)
{
    while (*line == ' ') line++;
    uint8_t* sub = line;
    while (*line && *line != ' ') line++;
    if (*line == ' ') { *line = '\0'; line++; }
    while (*line == ' ') line++;

    const char* arg = reinterpret_cast<const char*>(line);

    if (std::strcmp(reinterpret_cast<const char*>(sub), "list") == 0)
    {
        CmdList();
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "show") == 0)
    {
        CmdShow(arg);
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "pause") == 0)
    {
        CmdPause(arg);
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "resume") == 0)
    {
        CmdResume(arg);
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "reset") == 0)
    {
        CmdReset(arg);
    }
    else
    {
        Log::SendCommandLine("?: thread list|show|pause|resume|reset <name>");
    }
}

} // namespace debug

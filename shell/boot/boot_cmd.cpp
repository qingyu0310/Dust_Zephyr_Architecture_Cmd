/**
 * @file boot_cmd.cpp
 * @author qingyu
 * @brief boot 命令实现 — 启动期耗时（输出走 DUST_LOG 命令响应）
 * @version 0.3
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "boot_cmd.hpp"
#include "boot_timing.hpp"
#include "shell_cmd.hpp"
#include "token.hpp"
#include "log.hpp"
#include <cstdio>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Boot_cmd"

namespace debug {

/**
 * @brief 输出一行耗时（毫秒，保留三位小数）
 * @param slot 耗时槽
 */
static void PrintTiming(const BootTimingSlot* slot)
{
    if (slot == nullptr) return;

    char line[96];

    const uint32_t us = slot->ns / 1000U;
    snprintf(line, sizeof(line), "  %-16s %6u.%03u ms   %s", slot->name, (unsigned)(us / 1000U), (unsigned)(us % 1000U), slot->ok ? "ok" : "FAIL");
    Log::SendCommandLine(line);
}

/**
 * @brief boot timing：按耗时降序列出启动项
 *
 * 逐次选出「剩余项里最大的」：O(n²)，n 为实际注册项数（通常 < 16）。
 * 不额外申请缓冲，只用一个位标记位图。
 */
void BootCmd::CmdTiming()
{
    const uint8_t count = BootTiming::Count();

    if (count == 0) {
        Log::SendCommandLine("(no boot timing recorded)");
        return;
    }

    char head[64];
    const uint32_t total_ms = static_cast<uint32_t>(BootTiming::TotalNs() / 1000000ULL);
    snprintf(head, sizeof(head), "BOOT TIMING (total %u ms, %u items)", (unsigned)total_ms, (unsigned)count);
    Log::SendCommandLine(head);

    uint32_t used = 0;   // 位图：已输出的槽

    for (uint8_t printed = 0; printed < count; ++printed)
    {
        uint8_t best    = 0;
        uint32_t best_ns = 0;

        for (uint8_t i = 0; i < count; ++i)
        {
            if (used & (1UL << i)) continue;

            const BootTimingSlot* slot = BootTiming::At(i);
            if (slot == nullptr) continue;

            if (best_ns == 0 || slot->ns > best_ns) {
                best    = i;
                best_ns = slot->ns;
            }
        }

        used |= (1UL << best);
        PrintTiming(BootTiming::At(best));
    }
}

/**
 * @brief boot 命令入口
 * @param line 子命令参数（不含 "boot" 前缀）
 */
void BootCmd::Process(uint8_t* line)
{
    const char* sub = nullptr;
    line = NextToken(line, sub);

    if (std::strcmp(sub, "timing") == 0) {
        CmdTiming();
    } else {
        Log::SendCommandLine("boot timing");
    }
}

REGISTER_SHELL_CMD("boot", "boot timing", BootCmd::Process, boot);

} // namespace debug

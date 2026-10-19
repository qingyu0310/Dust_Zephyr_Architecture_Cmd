/**
 * @file boot_cmd.hpp
 * @author qingyu
 * @brief boot 命令 — 启动期耗时查询
 * @version 0.1
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL

#include <cstdint>

namespace debug {

/**
 * @brief boot 命令（timing）
 */
class BootCmd final
{
public:
    static void Process(uint8_t* line);        // boot 命令入口

private:
    static void CmdTiming();                   // boot timing：按耗时降序列出启动项
};

} // namespace debug

#endif

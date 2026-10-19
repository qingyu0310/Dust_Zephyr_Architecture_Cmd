/**
 * @file shell_cmd.hpp
 * @author qingyu
 * @brief shell 命令族注册 — 链接段收集 + 分派遍历
 * @version 0.1
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <cstdint>

namespace debug {

/**
 * @brief shell 命令族注册项
 *
 * handler 收到的是「命令名之后」的原始参数串，参数怎么切由各命令族自己决定。
 */
struct ShellCmdEntry
{
    const char* name;                    // 命令族名（"var" / "log" / ...）
    void (*handler)(uint8_t* args);      // 族入口，收到参数原始串
    const char* help;                    // 帮助摘要（一行）
};

} // namespace debug

// 链接脚本导出的段边界（dust_init.ld，C 链接符号）
extern "C" {

extern const ::debug::ShellCmdEntry __shell_cmd_start[];
extern const ::debug::ShellCmdEntry __shell_cmd_end[];

}

namespace debug {

/**
 * @brief 命令表起始
 * @return 首项指针（表空时等于 End）
 */
inline const ShellCmdEntry* ShellCmdBegin()
{
    return reinterpret_cast<const ShellCmdEntry*>(__shell_cmd_start);
}

/**
 * @brief 命令表结束
 * @return 尾后指针
 */
inline const ShellCmdEntry* ShellCmdEnd()
{
    return reinterpret_cast<const ShellCmdEntry*>(__shell_cmd_end);
}

/**
 * @brief 注册命令族
 *
 * 与 REGISTER_INIT / REGISTER_SHELL_VAR 同构：编译期放入 .shell_cmd 段，
 * shell 线程遍历该段匹配命令名。新增命令族只在自己的 .cpp 里写一行，
 * 不需要修改 shell.cpp。
 *
 * @param name_    命令族名（字符串字面量）
 * @param help_    帮助摘要（字符串字面量，一行）
 * @param handler_ 族入口函数，签名 void(uint8_t* args)
 * @param ident_   注册项标识（唯一标识符，通常用命令族名）
 */
#define REGISTER_SHELL_CMD(name_, help_, handler_, ident_)                      \
    static const ::debug::ShellCmdEntry kShellCmd_##ident_                      \
        __attribute__((used, __section__(".shell_cmd"))) = {                    \
        name_, handler_, help_                                                  \
    }

} // namespace debug

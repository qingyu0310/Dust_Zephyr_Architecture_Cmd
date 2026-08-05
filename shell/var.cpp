/**
 * @file var.cpp
 * @author qingyu
 * @brief var 命令实现 — var list/get/set（输出走 DUST_LOG 命令响应）
 * @version 0.1
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "var.hpp"
#include "log.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Var"

extern const debug::Entry __shell_var_start[];
extern const debug::Entry __shell_var_end[];

namespace debug {

/**
 * @brief 遍历链接段查找匹配名称的注册条目
 * @param name 变量名称字符串
 * @return 匹配的条目指针，未匹配返回 nullptr
 */
const Entry *Var::Find(const char *name)
{
    size_t n = __shell_var_end - __shell_var_start;

    for (size_t i = 0; i < n; i++)
    {
        const Entry *e = &__shell_var_start[i];

        if (std::strcmp(e->name, name) == 0) return e;
    }

    return nullptr;
}

/**
 * @brief 输出变量名、类型和当前值
 * @param e 待输出的条目
 */
void Var::PrintVar(const Entry &e)
{
    char line[160];

    switch (e.type)
    {
        case VarType::Uint8:   snprintf(line, sizeof(line), "  %s (u8)    = %u",   e.name, *static_cast<const uint8_t*>(e.ptr));   break;
        case VarType::Int8:    snprintf(line, sizeof(line), "  %s (i8)    = %d",   e.name, *static_cast<const int8_t*>(e.ptr));    break;
        case VarType::Uint16:  snprintf(line, sizeof(line), "  %s (u16)   = %u",   e.name, *static_cast<const uint16_t*>(e.ptr));  break;
        case VarType::Int16:   snprintf(line, sizeof(line), "  %s (i16)   = %d",   e.name, *static_cast<const int16_t*>(e.ptr));   break;
        case VarType::Uint32:  snprintf(line, sizeof(line), "  %s (u32)   = %u",   e.name, *static_cast<const uint32_t*>(e.ptr));  break;
        case VarType::Int32:   snprintf(line, sizeof(line), "  %s (i32)   = %d",   e.name, *static_cast<const int32_t*>(e.ptr));   break;
        case VarType::Uint64:  snprintf(line, sizeof(line), "  %s (u64)   = %llu", e.name, *static_cast<const uint64_t*>(e.ptr));  break;
        case VarType::Int64:   snprintf(line, sizeof(line), "  %s (i64)   = %lld", e.name, *static_cast<const int64_t*>(e.ptr));   break;
        case VarType::Float:   snprintf(line, sizeof(line), "  %s (float) = %f",   e.name, static_cast<double>(*static_cast<const float*>(e.ptr)));   break;
        case VarType::Double:  snprintf(line, sizeof(line), "  %s (double)= %lf",  e.name, *static_cast<const double*>(e.ptr));    break;
        case VarType::Bool:    snprintf(line, sizeof(line), "  %s (bool)  = %s",   e.name, *static_cast<const bool*>(e.ptr) ? "true" : "false");      break;
    }

    Log::SendLine(line);
}

/**
 * @brief 输出变量名和值（单行）
 * @param e 待输出的条目
 */
void Var::PrintValueOnly(const Entry &e)
{
    char line[160];

    switch (e.type)
    {
        case VarType::Uint8:   snprintf(line, sizeof(line), "%s = %u",   e.name, *static_cast<const uint8_t*>(e.ptr));   break;
        case VarType::Int8:    snprintf(line, sizeof(line), "%s = %d",   e.name, *static_cast<const int8_t*>(e.ptr));    break;
        case VarType::Uint16:  snprintf(line, sizeof(line), "%s = %u",   e.name, *static_cast<const uint16_t*>(e.ptr));  break;
        case VarType::Int16:   snprintf(line, sizeof(line), "%s = %d",   e.name, *static_cast<const int16_t*>(e.ptr));   break;
        case VarType::Uint32:  snprintf(line, sizeof(line), "%s = %u",   e.name, *static_cast<const uint32_t*>(e.ptr));  break;
        case VarType::Int32:   snprintf(line, sizeof(line), "%s = %d",   e.name, *static_cast<const int32_t*>(e.ptr));   break;
        case VarType::Uint64:  snprintf(line, sizeof(line), "%s = %llu", e.name, *static_cast<const uint64_t*>(e.ptr));  break;
        case VarType::Int64:   snprintf(line, sizeof(line), "%s = %lld", e.name, *static_cast<const int64_t*>(e.ptr));   break;
        case VarType::Float:   snprintf(line, sizeof(line), "%s = %f",   e.name, static_cast<double>(*static_cast<const float*>(e.ptr)));   break;
        case VarType::Double:  snprintf(line, sizeof(line), "%s = %lf",  e.name, *static_cast<const double*>(e.ptr));    break;
        case VarType::Bool:    snprintf(line, sizeof(line), "%s = %s",   e.name, *static_cast<const bool*>(e.ptr) ? "true" : "false");      break;
    }

    Log::SendLine(line);
}

/**
 * @brief 遍历链接段，输出所有注册变量
 */
void Var::CmdList()
{
    size_t n = __shell_var_end - __shell_var_start;

    if (n == 0) { Log::SendLine("(none)"); return; }

    for (size_t i = 0; i < n; i++)
    {
        PrintVar(__shell_var_start[i]);
    }

    char line[160];
    snprintf(line, sizeof(line), "--- %zu variables ---", n);
    Log::SendLine(line);
}

/**
 * @brief 按名称查找并输出变量值
 * @param name 变量名称
 */
void Var::CmdGet(const char *name)
{
    const Entry *e = Find(name);

    if (e == nullptr)
    {
        char line[128];
        snprintf(line, sizeof(line), "not found: %s", name);
        Log::SendLine(line);
        return;
    }

    PrintValueOnly(*e);
}

/**
 * @brief 按类型解析值字符串并写入变量
 * @param name 变量名称
 * @param val  值字符串（整数/浮点数/true/false）
 */
void Var::CmdSet(const char *name, const char *val)
{
    const Entry *e = Find(name);

    if (e == nullptr)
    {
        char line[128];
        snprintf(line, sizeof(line), "not found: %s", name);
        Log::SendLine(line);
        return;
    }

    switch (e->type)
    {
        case VarType::Uint8:
        case VarType::Uint16:
        case VarType::Uint32:
        case VarType::Uint64:
        {
            char *end = nullptr;
            unsigned long long v = std::strtoull(val, &end, 0);
            if (*end != '\0') { Log::SendLine("bad value"); return; }
            switch (e->type)
            {
                case VarType::Uint8:   *static_cast<uint8_t*> (e->ptr) = static_cast<uint8_t> (v); break;
                case VarType::Uint16:  *static_cast<uint16_t*>(e->ptr) = static_cast<uint16_t>(v); break;
                case VarType::Uint32:  *static_cast<uint32_t*>(e->ptr) = static_cast<uint32_t>(v); break;
                case VarType::Uint64:  *static_cast<uint64_t*>(e->ptr) = v; break;
                default: break;
            }
            break;
        }
        case VarType::Int8:
        case VarType::Int16:
        case VarType::Int32:
        case VarType::Int64:
        {
            char *end = nullptr;
            long long v = std::strtoll(val, &end, 0);
            if (*end != '\0') { Log::SendLine("bad value"); return; }
            switch (e->type)
            {
                case VarType::Int8:   *static_cast<int8_t*> (e->ptr) = static_cast<int8_t> (v); break;
                case VarType::Int16:  *static_cast<int16_t*>(e->ptr) = static_cast<int16_t>(v); break;
                case VarType::Int32:  *static_cast<int32_t*>(e->ptr) = static_cast<int32_t>(v); break;
                case VarType::Int64:  *static_cast<int64_t*>(e->ptr) = v; break;
                default: break;
            }
            break;
        }
        case VarType::Float:
        {
            char *end = nullptr;
            float v = std::strtof(val, &end);
            if (*end != '\0') { Log::SendLine("bad value"); return; }
            *static_cast<float*>(e->ptr) = v;
            break;
        }
        case VarType::Double:
        {
            char *end = nullptr;
            double v = std::strtod(val, &end);
            if (*end != '\0') { Log::SendLine("bad value"); return; }
            *static_cast<double*>(e->ptr) = v;
            break;
        }
        case VarType::Bool:
        {
            bool v;
            if (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0)
                v = true;
            else if (std::strcmp(val, "false") == 0 || std::strcmp(val, "0") == 0)
                v = false;
            else { Log::SendLine("bad value"); return; }
            *static_cast<bool*>(e->ptr) = v;
            break;
        }
    }

    Log::SendLine("ok");
}

/**
 * @brief var 命令入口（var list/get/set）
 * @param line 子命令参数（不含 "var" 前缀）
 */
void Var::Process(uint8_t *line)
{
    while (*line == ' ') line++;
    uint8_t *sub = line;
    while (*line && *line != ' ') line++;
    if (*line == ' ') { *line = '\0'; line++; }
    while (*line == ' ') line++;

    if (std::strcmp(reinterpret_cast<const char*>(sub), "list") == 0)
    {
        CmdList();
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "get") == 0)
    {
        CmdGet(reinterpret_cast<const char*>(line));
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "set") == 0)
    {
        // 原 CmdSet 解析逻辑（name + value）
        uint8_t *name = line;
        uint8_t *val  = nullptr;
        while (*line && *line != ' ') line++;
        if (*line == ' ') { *line = '\0'; line++; while (*line == ' ') line++; val = line; }
        if (val == nullptr || *val == '\0') Log::SendLine("usage: var set <name> <value>");
        else CmdSet(reinterpret_cast<const char*>(name), reinterpret_cast<const char*>(val));
    }
    else Log::SendLine("?: var list|get <name>|set <name> <val>");
}

} // namespace debug

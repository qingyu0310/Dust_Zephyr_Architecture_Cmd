/**
 * @file shell.cpp
 * @author qingyu
 * @brief 自维护 UART 线程 — 调试变量 l/g/s 命令
 */

#include "shell.hpp"
#include "Init_entry.hpp"
#include <zephyr/logging/log.h>
#include <cstdlib>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Debug"

LOG_MODULE_REGISTER(shell_dbg, LOG_LEVEL_INF);

extern const debug::Entry __shell_var_start[];
extern const debug::Entry __shell_var_end[];

namespace debug {

/**
 * @brief 绑定 UART DMA 实例，初始化完成
 * @param uart 外部创建的 UART DMA 实例引用
 * @return true
 */
bool DbgConsole::Init(UartDma &uart)
{
    uart_ = &uart;
    return true;
}

/**
 * @brief 遍历链接段查找匹配名称的注册条目
 * @param name 变量名称字符串
 * @return 匹配的条目指针，未匹配返回 nullptr
 */
const debug::Entry *DbgConsole::FindVar(const char *name)
{
    size_t n = __shell_var_end - __shell_var_start;

    for (size_t i = 0; i < n; i++)
    {
        const debug::Entry *e = &__shell_var_start[i];

        if (std::strcmp(e->name, name) == 0) return e;
    }

    return nullptr;
}

/**
 * @brief 输出变量名、类型和当前值
 * @param e 待输出的条目
 */
void DbgConsole::PrintVar(const debug::Entry &e) const
{
    switch (e.type)
    {
        case debug::VarType::Uint8:   LOG_INF("  %s (u8)    = %u",   e.name, *static_cast<const uint8_t*>(e.ptr));   break;
        case debug::VarType::Int8:    LOG_INF("  %s (i8)    = %d",   e.name, *static_cast<const int8_t*>(e.ptr));    break;
        case debug::VarType::Uint16:  LOG_INF("  %s (u16)   = %u",   e.name, *static_cast<const uint16_t*>(e.ptr));  break;
        case debug::VarType::Int16:   LOG_INF("  %s (i16)   = %d",   e.name, *static_cast<const int16_t*>(e.ptr));   break;
        case debug::VarType::Uint32:  LOG_INF("  %s (u32)   = %u",   e.name, *static_cast<const uint32_t*>(e.ptr));  break;
        case debug::VarType::Int32:   LOG_INF("  %s (i32)   = %d",   e.name, *static_cast<const int32_t*>(e.ptr));   break;
        case debug::VarType::Uint64:  LOG_INF("  %s (u64)   = %llu", e.name, *static_cast<const uint64_t*>(e.ptr));  break;
        case debug::VarType::Int64:   LOG_INF("  %s (i64)   = %lld", e.name, *static_cast<const int64_t*>(e.ptr));   break;
        case debug::VarType::Float:   LOG_INF("  %s (float) = %f",   e.name, static_cast<double>(*static_cast<const float*>(e.ptr)));   break;
        case debug::VarType::Double:  LOG_INF("  %s (double)= %lf",  e.name, *static_cast<const double*>(e.ptr));    break;
        case debug::VarType::Bool:    LOG_INF("  %s (bool)  = %s",   e.name, *static_cast<const bool*>(e.ptr) ? "true" : "false");      break;
    }
}

/**
 * @brief 输出变量名和值（单行）
 * @param e 待输出的条目
 */
void DbgConsole::PrintValueOnly(const debug::Entry &e) const
{
    switch (e.type)
    {
        case debug::VarType::Uint8:   LOG_INF("%s = %u",   e.name, *static_cast<const uint8_t*>(e.ptr));   break;
        case debug::VarType::Int8:    LOG_INF("%s = %d",   e.name, *static_cast<const int8_t*>(e.ptr));    break;
        case debug::VarType::Uint16:  LOG_INF("%s = %u",   e.name, *static_cast<const uint16_t*>(e.ptr));  break;
        case debug::VarType::Int16:   LOG_INF("%s = %d",   e.name, *static_cast<const int16_t*>(e.ptr));   break;
        case debug::VarType::Uint32:  LOG_INF("%s = %u",   e.name, *static_cast<const uint32_t*>(e.ptr));  break;
        case debug::VarType::Int32:   LOG_INF("%s = %d",   e.name, *static_cast<const int32_t*>(e.ptr));   break;
        case debug::VarType::Uint64:  LOG_INF("%s = %llu", e.name, *static_cast<const uint64_t*>(e.ptr));  break;
        case debug::VarType::Int64:   LOG_INF("%s = %lld", e.name, *static_cast<const int64_t*>(e.ptr));   break;
        case debug::VarType::Float:   LOG_INF("%s = %f",   e.name, static_cast<double>(*static_cast<const float*>(e.ptr)));   break;
        case debug::VarType::Double:  LOG_INF("%s = %lf",  e.name, *static_cast<const double*>(e.ptr));    break;
        case debug::VarType::Bool:    LOG_INF("%s = %s",   e.name, *static_cast<const bool*>(e.ptr) ? "true" : "false");      break;
    }
}

/**
 * @brief 遍历链接段，输出所有注册变量
 */
void DbgConsole::CmdList() const
{
    size_t n = __shell_var_end - __shell_var_start;

    if (n == 0) { LOG_INF("(none)"); return; }

    for (size_t i = 0; i < n; i++)
    {
        PrintVar(__shell_var_start[i]);
    }

    LOG_INF("--- %zu variables ---", n);
}

/**
 * @brief 按名称查找并输出变量值
 * @param name 变量名称
 */
void DbgConsole::CmdGet(const char *name) const
{
    const debug::Entry *e = FindVar(name);

    if (e == nullptr)
    {
        LOG_ERR("not found: %s", name);
        return;
    }

    PrintValueOnly(*e);
}

/**
 * @brief 按类型解析值字符串并写入变量
 * @param name 变量名称
 * @param val  值字符串（整数/浮点数/true/false）
 */
void DbgConsole::CmdSet(const char *name, const char *val) const
{
    const debug::Entry *e = FindVar(name);

    if (e == nullptr)
    {
        LOG_ERR("not found: %s", name);
        return;
    }

    switch (e->type)
    {
        case debug::VarType::Uint8:
        case debug::VarType::Uint16:
        case debug::VarType::Uint32:
        case debug::VarType::Uint64:
        {
            char *end = nullptr;
            unsigned long long v = std::strtoull(val, &end, 0);
            if (*end != '\0') { LOG_ERR("bad value"); return; }
            switch (e->type)
            {
                case debug::VarType::Uint8:   *static_cast<uint8_t*> (e->ptr) = static_cast<uint8_t> (v); break;
                case debug::VarType::Uint16:  *static_cast<uint16_t*>(e->ptr) = static_cast<uint16_t>(v); break;
                case debug::VarType::Uint32:  *static_cast<uint32_t*>(e->ptr) = static_cast<uint32_t>(v); break;
                case debug::VarType::Uint64:  *static_cast<uint64_t*>(e->ptr) = v; break;
                default: break;
            }
            break;
        }
        case debug::VarType::Int8:
        case debug::VarType::Int16:
        case debug::VarType::Int32:
        case debug::VarType::Int64:
        {
            char *end = nullptr;
            long long v = std::strtoll(val, &end, 0);
            if (*end != '\0') { LOG_ERR("bad value"); return; }
            switch (e->type)
            {
                case debug::VarType::Int8:   *static_cast<int8_t*> (e->ptr) = static_cast<int8_t> (v); break;
                case debug::VarType::Int16:  *static_cast<int16_t*>(e->ptr) = static_cast<int16_t>(v); break;
                case debug::VarType::Int32:  *static_cast<int32_t*>(e->ptr) = static_cast<int32_t>(v); break;
                case debug::VarType::Int64:  *static_cast<int64_t*>(e->ptr) = v; break;
                default: break;
            }
            break;
        }
        case debug::VarType::Float:
        {
            char *end = nullptr;
            float v = std::strtof(val, &end);
            if (*end != '\0') { LOG_ERR("bad value"); return; }
            *static_cast<float*>(e->ptr) = v;
            break;
        }
        case debug::VarType::Double:
        {
            char *end = nullptr;
            double v = std::strtod(val, &end);
            if (*end != '\0') { LOG_ERR("bad value"); return; }
            *static_cast<double*>(e->ptr) = v;
            break;
        }
        case debug::VarType::Bool:
        {
            bool v;
            if (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0)
                v = true;
            else if (std::strcmp(val, "false") == 0 || std::strcmp(val, "0") == 0)
                v = false;
            else { LOG_ERR("bad value"); return; }
            *static_cast<bool*>(e->ptr) = v;
            break;
        }
    }

    LOG_INF("ok");
}

/**
 * @brief 解析一行命令并分派到对应的处理函数
 * @param line 行缓冲指针（可写，解析时会被修改）
 */
void DbgConsole::ProcessLine(uint8_t *line)
{
    while (*line == ' ') line++;
    if (*line == '\0' || *line == '\r' || *line == '\n') return;

    uint8_t cmd = *line++;
    while (*line == ' ') line++;

    switch (cmd)
    {
        case 'l': case 'L': CmdList(); break;
        case 'g': case 'G': CmdGet(reinterpret_cast<const char*>(line)); break;
        case 's': case 'S':
        {
            uint8_t *name = line;
            uint8_t *val  = nullptr;

            while (*line && *line != ' ') line++;

            if (*line == ' ')
            {
                *line = '\0';
                line++;
                while (*line == ' ') line++;
                val = line;
            }

            if (val == nullptr || *val == '\0')
                LOG_ERR("usage: s <name> <value>");
            else
                CmdSet(reinterpret_cast<const char*>(name),
                       reinterpret_cast<const char*>(val));

            break;
        }
        case '?': case 'H': case 'h':
            LOG_INF("l           list");
            LOG_INF("g <name>    get");
            LOG_INF("s <n> <v>   set");
            break;
        default:
            LOG_ERR("?: l/g/s/?");
            break;
    }
}

/**
 * @brief 线程主循环，等待 UART 数据并逐行处理
 */
void DbgConsole::Task()
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

static DbgConsole dbg_console;

/**
 * @brief 初始化 console UART 和 dbg 控制台
 * @return true 初始化成功
 */
static bool thread_init()
{
    static UartDma rx {};

    UartDma::Config cfg;
    cfg.line_cfg.baudrate = 921600;

    if (!rx.Init(DEVICE_DT_GET(DT_CHOSEN(zephyr_console)), cfg))
    {
        LOG_ERR("console uart init failed");
        return false;
    }

    return dbg_console.Init(rx);
}

/**
 * @brief 启动 dbg 线程
 * @return true 启动成功
 */
static bool thread_start()
{
    dbg_console.Start();
    return true;
}

REGISTER_INIT(thread_init,  PreInit,   High, "dbg_init");
REGISTER_INIT(thread_start, AppThread, Low,  "dbg_start");

} // namespace debug



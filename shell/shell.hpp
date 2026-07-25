/**
 * @file shell.hpp
 * @author qingyu
 * @brief 调试变量注册 — 链接段收集 + 自维护 UART 线程
 * @version 0.1
 * @date 2026-07-25
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>
#include <zephyr/kernel.h>
#include "uart.hpp"
#include "thread.hpp"

namespace debug
{

enum class VarType : uint8_t
{
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int32,
    Uint64,
    Int64,
    Float,
    Double,
    Bool,
};

union VarValue
{
    uint8_t   u8;
    int8_t    i8;
    uint16_t  u16;
    int16_t   i16;
    uint32_t  u32;
    int32_t   i32;
    uint64_t  u64;
    int64_t   i64;
    float     f;
    double    d;
    bool      b;
};

/**
 * @brief shell 链接段条目
 *
 * 每个 REGISTER_SHELL_VAR 生成一个 Entry，编译期放入 .shell_var 段。
 * shell 线程通过 __shell_var_start/end 遍历所有条目。
 */
struct Entry
{
    const char   *name;     // 变量名称（l/g/s 命令用）
    VarType       type;     // 变量类型（TypeMap 自动推导）
    void         *ptr;      // 变量指针（读写目标）
};

class DbgConsole final
{
public:
    bool Init(UartDma &uart);
    void Start(ThreadPrio prio = ThreadPrio::Lowest)
    {
        thread_.Start(TaskEntry, prio, this);
    }

private:
    static constexpr size_t kLineBufSize = 128;

    UartDma           *uart_     = nullptr;
    uint8_t            line_buf_[kLineBufSize]{};
    uint32_t           line_pos_ = 0;
    Thread<2048>       thread_   {};

    static const debug::Entry *FindVar(const char *name);

    void PrintVar(const debug::Entry &e) const;
    void CmdList() const;
    void CmdGet(const char *name) const;
    void CmdSet(const char *name, const char *val) const;
    void PrintValueOnly(const debug::Entry &e) const;
    void ProcessLine(uint8_t *line);
    void Task();

    static void TaskEntry(void *p1, void *p2, void *p3)
    {
        ARG_UNUSED(p2);
        ARG_UNUSED(p3);
        auto *self = static_cast<DbgConsole*>(p1);
        self->Task();
    }
};

template<typename T>
struct TypeMap
{
    static constexpr VarType type = VarType::Uint32;
};


template<> struct TypeMap<uint8_t>  { static constexpr auto type = VarType::Uint8;  };
template<> struct TypeMap<int8_t>   { static constexpr auto type = VarType::Int8;   };
template<> struct TypeMap<uint16_t> { static constexpr auto type = VarType::Uint16; };
template<> struct TypeMap<int16_t>  { static constexpr auto type = VarType::Int16;  };
template<> struct TypeMap<uint32_t> { static constexpr auto type = VarType::Uint32; };
template<> struct TypeMap<int32_t>  { static constexpr auto type = VarType::Int32;  };
template<> struct TypeMap<uint64_t> { static constexpr auto type = VarType::Uint64; };
template<> struct TypeMap<int64_t>  { static constexpr auto type = VarType::Int64;  };
template<> struct TypeMap<float>    { static constexpr auto type = VarType::Float;  };
template<> struct TypeMap<double>   { static constexpr auto type = VarType::Double; };
template<> struct TypeMap<bool>     { static constexpr auto type = VarType::Bool;   };

/**
 * @brief 注册调试变量到 shell 链接段
 *
 * 编译期在 .shell_var 段生成一个 Entry，shell 线程通过链接段
 * 边界指针遍历所有注册变量，实现 l/g/s 命令。
 *
 * @param name_ 变量名称字符串（供 l/g/s 命令使用）
 * @param var_  被注册的变量名（通过 decltype 自动推导类型）
 *
 * 示例：
 * @code{cpp}
 * static float my_value;
 * REGISTER_SHELL_VAR("my_value", my_value);
 * @endcode
 */
#define PP_CONCAT(a, b)  PP_CAT(a, b)
#define PP_CAT(a, b) a##b

#define REGISTER_SHELL_VAR(name_, var_)                                                   \
    static ::debug::Entry PP_CONCAT(s_shell_var_, __COUNTER__)                                       \
        __attribute__((used, section(".shell_var"), aligned(sizeof(void*)))) = {          \
        .name = (name_),                                                                  \
        .type = ::debug::TypeMap<decltype(var_)>::type,                                   \
        .ptr  = static_cast<void*>(&(var_)),                                              \
    };                                                                                    \
    static_assert(sizeof(var_) <= sizeof(::debug::VarValue),                              \
                  "shell var \"" name_ "\" is too large for VarValue")

} // namespace debug

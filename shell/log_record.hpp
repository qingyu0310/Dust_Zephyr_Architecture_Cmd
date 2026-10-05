/**
 * @file log_record.hpp
 * @author qingyu
 * @brief DUST_LOG 记录快照与格式化 — 参数快照/文本展开/行包装
 * @version 0.3
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "log_policy.hpp"

namespace debug {

constexpr uint16_t kLogBufSize    = 256;		// 格式化缓冲：可变参数展开目标（超 255 截断）
constexpr uint16_t kColorOutExtra = 16;			// 颜色输出缓冲额外预留（ANSI 转义 + \r\n）
constexpr uint16_t kLineOutExtra  = 4;			// 行输出缓冲额外预留（\r\n）
constexpr uint8_t  kMaxLogArgs    = 4;			// 参数快照槽上限（%f 占 2 槽）

/**
 * @brief 日志原始请求（未格式化，异步段生产队列条目）
 *
 * 异步段调用点只快照 fmt+参数入队，格式化由 shell 线程 FormatRecordToText 完成。
 * args 槽位约定：%f 占 2 槽（double 位模式），%s/%p 指针 1 槽，整型 1 槽。
 */
struct LogRecord
{
    const char* fmt;               				// 格式串（静态字面量，禁止临时栈串）
    LogChannel  channel;           				// 日志通道（通道策略表索引）
    LogColor    color;             				// 颜色（由通道策略填入）
    TxPriority  prio;              				// 展开后的发送优先级
    uint16_t    epoch;             				// DBG 选择代数（出队比对，不一致丢弃）
    bool        ansi;              				// 是否上 ANSI 颜色（DBG 按条目模式，其余取通道策略）
    uint32_t    args[kMaxLogArgs]; 				// 参数快照
    uint8_t     nargs;             				// 参数槽数（0~kMaxLogArgs）
    uint8_t     next;              				// 记录链表（数组索引，255=nullptr）
};

/**
 * @brief 调用点参数快照：按 fmt 从 va_list 取定长槽（va_end 前调用）
 *
 * 逐转换符 va_arg 一次，类型严格匹配防失步；超 kMaxLogArgs 停止写槽但继续消费参数。
 * %f 以 double 位模式存 2 槽（little-endian：args[n]=低32位，args[n+1]=高32位）。
 *
 * @param fmt   格式串（静态字面量）
 * @param ap    可变参数（调用点 va_list）
 * @param args  输出：参数槽
 * @param nargs 输出：槽数
 */
inline void BuildRecordFromVaList(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs)
{
    uint8_t n = 0;

    while (*fmt && n <= kMaxLogArgs)   // n==kMaxLogArgs 时仍扫（消费参数防失步），不写槽
    {
        if (*fmt != '%') { fmt++; continue; }

        fmt++;                                    // 跳过 '%'
        if (*fmt == '%') { fmt++; continue; }     // "%%" 转义：无参数
        if (*fmt == '\0') break;

        // flags（-+ #0）
        while (*fmt && (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0')) fmt++;
        // width
        while (*fmt && *fmt >= '0' && *fmt <= '9') fmt++;
        // .prec
        if (*fmt == '.') { fmt++; while (*fmt && *fmt >= '0' && *fmt <= '9') fmt++; }
        // length（h/hh/l/ll）
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }

        char spec = *fmt;
        if (spec == '\0') break;

        switch (spec)
        {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
                if (n < kMaxLogArgs) args[n++] = static_cast<uint32_t>(va_arg(ap, int));
                else                 (void)va_arg(ap, int);       // 超限：仍消费
                break;
            case 'f': case 'F':
            {
                double d = va_arg(ap, double);
                if (n + 1 < kMaxLogArgs)
                {
                    uint32_t lo, hi;
                    std::memcpy(&lo, &d, sizeof(lo));
                    std::memcpy(&hi, reinterpret_cast<const uint8_t*>(&d) + sizeof(lo), sizeof(hi));
                    args[n++] = lo;
                    args[n++] = hi;
                }
                break;
            }
            case 's': case 'p':
                if (n < kMaxLogArgs)
                    args[n++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(va_arg(ap, void*)));
                else (void)va_arg(ap, void*);
                break;
            default:
                break;                                            // 未知转换符：格式串受约束，不消费
        }

        if (*fmt) fmt++;
    }

    *nargs = (n <= kMaxLogArgs) ? n : kMaxLogArgs;
}

/**
 * @brief 记录展开为纯文本（shell 线程，不含前缀/颜色/行尾）
 *
 * 逐转换符从 args 取槽，用 picolibc snprintf 展开；%f 从 2 槽拼回 double。
 * 不依赖 libc va_list。
 *
 * @param r         日志请求记录
 * @param text      输出：展开文本
 * @param text_size 输出缓冲大小
 * @return 文本长度（不含结尾 '\0'）
 */
inline size_t FormatRecordToText(const LogRecord& r, char* text, size_t text_size)
{
    size_t pos = 0;
    uint8_t k  = 0;                                    // args 槽游标

    const char* fmt = r.fmt;
    while (*fmt && pos < text_size - 1)
    {
        if (*fmt != '%') { text[pos++] = *fmt++; continue; }

        fmt++;                                        // 跳过 '%'
        if (*fmt == '%') { text[pos++] = '%'; fmt++; continue; }

        // 收集完整 %段 到 seg（flags/width/prec/length/spec），格式串逐字符保留
        char  seg[24];
        size_t segn = 0;
        seg[segn++] = '%';
        while (*fmt && (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '0')) seg[segn++] = *fmt++;
        while (*fmt && *fmt >= '0' && *fmt <= '9')      seg[segn++] = *fmt++;
        if (*fmt == '.') { seg[segn++] = *fmt++; while (*fmt && *fmt >= '0' && *fmt <= '9') seg[segn++] = *fmt++; }
        if (*fmt == 'l') { seg[segn++] = *fmt++; if (*fmt == 'l') seg[segn++] = *fmt++; }
        else if (*fmt == 'h') { seg[segn++] = *fmt++; if (*fmt == 'h') seg[segn++] = *fmt++; }
        char spec = *fmt;
        if (spec != '\0') seg[segn++] = spec;
        seg[segn] = '\0';
        if (spec != '\0') fmt++;

        size_t room = text_size - pos;
        if (room < 8) break;                          // 剩太少，截断

        switch (spec)
        {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
            {
                int v = (k < r.nargs) ? static_cast<int>(r.args[k++]) : 0;
                int m = snprintf(text + pos, room, seg, v);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 'f': case 'F':
            {
                double d = 0.0;
                if (k + 1 < r.nargs)
                {
                    std::memcpy(&d, &r.args[k], sizeof(d));   // 2 槽拼回（little-endian）
                    k += 2;
                }
                else { k = r.nargs; }
                int m = snprintf(text + pos, room, seg, d);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 's':
            {
                const char* s = (k < r.nargs) ? reinterpret_cast<const char*>(static_cast<uintptr_t>(r.args[k++])) : "(null)";
                if (s == nullptr) s = "(null)";
                int m = snprintf(text + pos, room, seg, s);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 'p':
            {
                void* p = (k < r.nargs) ? reinterpret_cast<void*>(static_cast<uintptr_t>(r.args[k++])) : nullptr;
                int m = snprintf(text + pos, room, seg, p);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            default:
                text[pos++] = '%';                     // 未知转换符：按字面 '%' 输出（格式串受约束）
                break;
        }
    }
    text[pos] = '\0';
    return pos;
}

/**
 * @brief 文本包装为待发内容：前缀 + ANSI 颜色 + 行尾（\r\n）
 *
 * ansi_color=false 时纯文本（DBG/命令响应）——兼容 VOFA+ FireWater；
 * ANSI 转义串 \x1b[38;2;R;G;Bm 里的数字会被 VOFA+ 误解析成通道数据。
 *
 * @param prefix     行首前缀
 * @param ansi_color 是否上 ANSI 颜色（DBG 按条目模式，其余取通道策略）
 * @param color      ANSI 颜色（ansi_color=true 时使用）
 * @param text       已展开文本
 * @param out        输出：待发内容
 * @param out_size   输出缓冲大小
 * @return 待发内容长度；<=0 表示失败
 */
inline int WrapOutputFrame(const char* prefix, bool ansi_color, LogColor color, const char* text, char* out, size_t out_size)
{
    if (!ansi_color) return snprintf(out, out_size, "%s%s\r\n", prefix, text);

    const uint32_t rgb = static_cast<uint32_t>(color);
    return snprintf(out, out_size, "\x1b[38;2;%d;%d;%dm%s%s\x1b[0m\r\n", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, prefix, text);
}

} // namespace debug

#endif

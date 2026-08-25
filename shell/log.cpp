/**
 * @file log.cpp
 * @author qingyu
 * @brief DUST_LOG 自研日志系统实现 — 帧池三档优先级仲裁发送
 * @version 0.1
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "log.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Log"

namespace debug {

// 日志行格式化/输出缓冲（vsnprintf/snprintf 目标）
constexpr uint16_t kLogBufSize    = 256;		// 格式化缓冲：可变参数展开目标（超 255 截断）
constexpr uint16_t kColorOutExtra = 16;			// 颜色输出缓冲额外预留（ANSI 转义 + \r\n）
constexpr uint16_t kLineOutExtra  = 4;			// 行输出缓冲额外预留（\r\n）

/**
 * @brief 初始化日志系统
 *
 * 清空选中状态/条目计数，重建空闲帧链表（0→1→…→255），
 * 发送队列置空。DBG 条目由 FindOrCreate 运行时创建，无链接段遍历。
 *
 * @return true 初始化成功
 */
bool Log::Init()
{
    active_  = nullptr;
    count_   = 0;
    sending_ = false;
    txq_.Reset();
    recq_.Reset();
    return true;
}

/**
 * @brief 按名字查 DBG 条目，首见创建（同名复用同一条目）
 * @param name DBG 名字（字符串字面量）
 * @return 条目指针；池满无法创建返回 nullptr
 */
LogEntry* Log::FindOrCreate(const char* name)
{
    for (uint8_t i = 0; i < count_; ++i)
    {
        if (std::strcmp(entries_[i].name, name) == 0) return &entries_[i];
    }

    if (count_ >= kMaxLogEntries) return nullptr;   // 池满：无法创建，返回 nullptr

    LogEntry* e = &entries_[count_++];
    e->name = name;
    return e;
}

/**
 * @brief 选中 DBG 条目：静默 → 流式打印
 *
 * 只选中已存在的条目（代码里 DUST_LOG_DBG 注册过的名字），
 * 不存在返回 false（log on 回 not found），不创建幽灵条目。
 * 直接改 active_ 指针，旧选中自动被顶替（同一时间只打一条）；
 * 队列中残留的旧 Dbg 档帧标记作废（及时顶替）。
 *
 * @param name DBG 名字
 * @return true 选中成功；false 条目不存在
 */
bool Log::Select(const char* name)
{
    for (uint8_t i = 0; i < count_; ++i)
    {
        if (std::strcmp(entries_[i].name, name) == 0)
        {
            active_ = &entries_[i];
            MarkStaleDbg();       // 及时顶替：队列中残留的旧 Dbg 档帧作废（Dequeue 跳过回收）
            return true;
        }
    }
    return false;                 // 不存在：log on 回 not found
}

/**
 * @brief DBG 流式打印（仅当 e 是当前选中条目才发，白色，低优先级）
 *
 * 分时段：同步段调用点 vsnprintf + 直发；异步段参数快照入队，shell 线程 FormatRecord。
 *
 * @param e   DBG 条目（FindOrCreate 返回值）
 * @param fmt 格式化串
 */
void Log::Dbgl(LogEntry* e, const char* fmt, ...)
{
	if (!shell_own_) return;                // boot 早期无 DBG（未选中不可用），直接返回
	
    if (e == nullptr) return;               // 池满（FindOrCreate 返回 nullptr）：静默丢弃
    if (e != active_) return;               // 未选中，静默

    va_list ap;
    va_start(ap, fmt);

    // 异步段（shell 接管后）：参数快照（DBG 白色）
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    SnapshotArgs(fmt, ap, args, &nargs);
    va_end(ap);

    TryEnqueueRecord(fmt, LogColor::White, TxPriority::Dbg, args, nargs);

    unsigned key = irq_lock();
    if (!sending_ && stream_ != nullptr) k_sem_give(&stream_->sem_);
    irq_unlock(key);
}

/**
 * @brief 一次性日志，黑色
 * @param fmt 格式化串
 */
void Log::Inf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColor(LogColor::Black, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，红色
 * @param fmt 格式化串
 */
void Log::Err(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColor(LogColor::Red, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，绿色（状态正常）
 * @param fmt 格式化串
 */
void Log::Ok(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColor(LogColor::Green, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，橘色
 * @param fmt 格式化串
 */
void Log::Wrn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColor(LogColor::Orange, fmt, ap);
    va_end(ap);
}

/**
 * @brief 通用：格式化 + ANSI 上色 + 仲裁发送（事件档）
 *
 * 分时段：boot 早期（!shell_own_）调用点 vsnprintf + 直发（无实时要求，现状）；
 * shell 接管后（shell_own_）参数快照入队，格式化由 shell 线程 FormatRecord 完成。
 *
 * @param c   颜色
 * @param fmt 格式化串
 * @param ap  可变参数
 */
void Log::PrintColor(LogColor c, const char* fmt, va_list ap)
{
    if (!shell_own_)                          									// 同步段（boot 早期）：调用点直发，现状不变
    {
        char buf[kLogBufSize];
        vsnprintf(buf, sizeof(buf), fmt, ap);   								// 格式化（picolibc 支持 %f，prj.conf 已开 PICOLIBC_IO_FLOAT）

        char out[kLogBufSize + kColorOutExtra];
        const uint32_t rgb = static_cast<uint32_t>(c);
        int n = snprintf(out, sizeof(out), "\x1b[38;2;%d;%d;%dm%s\x1b[0m\r\n", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, buf);
        if (n > 0) PublishDirect(out, n, TxPriority::Event);   // boot 早期普通日志：调用点直发
        return;
    }

    // 异步段（shell 接管后）：参数快照入队，调用点不格式化
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    SnapshotArgs(fmt, ap, args, &nargs);      // 在 va_end 前快照

    TryEnqueueRecord(fmt, c, TxPriority::Event, args, nargs);    // 入原始请求队列

    unsigned key = irq_lock();
    if (!sending_ && stream_ != nullptr) k_sem_give(&stream_->sem_);   		// 唤醒 shell 去 FormatRecord
    irq_unlock(key);
}

/**
 * @brief 命令响应直发（不经过 log 过滤，带 \r\n，中档）
 * @param text 响应文本
 */
void Log::SendLine(const char* text)
{
    char out[kLogBufSize + kLineOutExtra];
    int n = snprintf(out, sizeof(out), "%s\r\n", text);
    if (n > 0) PublishDirect(out, n, TxPriority::Cmd);   // 命令响应：调用点直发（帧池 8 缓冲突发）
}

/**
 * @brief 快照 fmt 的参数值到定长槽（异步段调用点使用，va_end 前调用）
 *
 * 逐转换符 va_arg 一次，类型严格匹配防失步；超 kMaxLogArgs 停止写槽但继续消费参数。
 * %f 以 double 位模式存 2 槽（little-endian：args[n]=低32位，args[n+1]=高32位）。
 *
 * @param fmt   格式串（静态字面量）
 * @param ap    可变参数（调用点 va_list）
 * @param args  输出：参数槽
 * @param nargs 输出：槽数
 */
void Log::SnapshotArgs(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs)
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
 * @brief 入队日志原始请求（异步段调用点，任务或 ISR 上下文）
 *
 * 短临界区：池满直接丢（不遍历挤，ISR 安全）。FIFO 尾插保证事件日志不乱序。
 *
 * @param fmt   格式串（静态字面量）
 * @param color 颜色
 * @param prio  展开后的发送优先级
 * @param args  参数槽
 * @param nargs 槽数
 */
void Log::TryEnqueueRecord(const char* fmt, LogColor color, TxPriority prio, const uint32_t* args, uint8_t nargs)
{
    unsigned key = irq_lock();                // 并发保护（任务或 ISR 上下文）
    (void)recq_.PushLocked(fmt, color, prio, args, nargs);
    irq_unlock(key);
}

/**
 * @brief 处理一条日志原始请求（shell 线程）
 *
 * 记录在格式化完成后再归还空闲链，避免出队后立即被 ISR 复用。
 * @return true 处理了一条记录；false 队列为空
 */
bool Log::ProcessOneRecord()
{
    unsigned key = irq_lock();
    LogRecord* r = recq_.PopLocked();
    irq_unlock(key);

    if (r == nullptr) return false;

    FormatRecord(r);

    key = irq_lock();
    recq_.ReleaseLocked(r);
    irq_unlock(key);
    return true;
}

/**
 * @brief 展开日志请求为格式化字符串并入 TxFrame 帧池（shell 线程）
 *
 * 逐转换符从 args 取槽，用 picolibc snprintf 展开；%f 从 2 槽拼回 double。
 * 展开结果复用 PrintColor 的 ANSI 组装方式。不依赖 libc va_list。
 *
 * @param r 日志请求记录
 */
void Log::FormatRecord(LogRecord* r)
{
    char   buf[kLogBufSize];
    size_t pos = 0;
    uint8_t k  = 0;                                    // args 槽游标

    const char* fmt = r->fmt;
    while (*fmt && pos < kLogBufSize - 1)
    {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }

        fmt++;                                        // 跳过 '%'
        if (*fmt == '%') { buf[pos++] = '%'; fmt++; continue; }

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

        size_t room = kLogBufSize - pos;
        if (room < 8) break;                          // 剩太少，截断

        switch (spec)
        {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
            {
                int v = (k < r->nargs) ? static_cast<int>(r->args[k++]) : 0;
                int m = snprintf(buf + pos, room, seg, v);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 'f': case 'F':
            {
                double d = 0.0;
                if (k + 1 < r->nargs)
                {
                    std::memcpy(&d, &r->args[k], sizeof(d));   // 2 槽拼回（little-endian）
                    k += 2;
                }
                else { k = r->nargs; }
                int m = snprintf(buf + pos, room, seg, d);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 's':
            {
                const char* s = (k < r->nargs)
                    ? reinterpret_cast<const char*>(static_cast<uintptr_t>(r->args[k++]))
                    : "(null)";
                if (s == nullptr) s = "(null)";
                int m = snprintf(buf + pos, room, seg, s);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            case 'p':
            {
                void* p = (k < r->nargs)
                    ? reinterpret_cast<void*>(static_cast<uintptr_t>(r->args[k++]))
                    : nullptr;
                int m = snprintf(buf + pos, room, seg, p);
                if (m > 0) pos += (static_cast<size_t>(m) < room) ? static_cast<size_t>(m) : room - 1;
                break;
            }
            default:
                buf[pos++] = '%';                     // 未知转换符：按字面 '%' 输出（格式串受约束）
                break;
        }
    }
    buf[pos] = '\0';

    // ANSI 上色 + \r\n（复用 PrintColor 组装）
    char out[kLogBufSize + kColorOutExtra];
    const uint32_t rgb = static_cast<uint32_t>(r->color);
    int n = snprintf(out, sizeof(out), "\x1b[38;2;%d;%d;%dm%s\x1b[0m\r\n", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, buf);
	
    if (n > 0) PublishQueued(out, n, r->prio);             // 线程展开后按原始请求档位入队
}

/**
 * @brief 重置发送帧池和发送队列
 */
void Log::TxFrameQueue::Reset()
{
    head = kNullIndex;
    tail = kNullIndex;
    free_head = 0;
    for (uint8_t i = 0; i < kTxPoolCount; ++i)
    {
        frames[i].len = 0;
        frames[i].is_stale = false;
        frames[i].next = (i + 1 < kTxPoolCount) ? static_cast<uint8_t>(i + 1) : kNullIndex;
    }
}

uint8_t Log::TxFrameQueue::IndexOf(const TxFrame* f) const
{
    return static_cast<uint8_t>(f - frames);
}

TxFrame* Log::TxFrameQueue::AllocLocked()
{
    if (free_head == kNullIndex) return nullptr;

    uint8_t i = free_head;
    free_head = frames[i].next;
    frames[i].next = kNullIndex;
    return &frames[i];
}

void Log::TxFrameQueue::ReleaseLocked(TxFrame* f)
{
    uint8_t i = IndexOf(f);
    f->is_stale = false;
    f->next = free_head;
    free_head = i;
}

void Log::TxFrameQueue::PushByPrioLocked(TxFrame* f)
{
    f->next = kNullIndex;
    if (head == kNullIndex) { head = tail = IndexOf(f); return; }

    uint8_t* pp = &head;              			// 找插入点：第一个 prio > f->prio 的帧之前
    while (*pp != kNullIndex && frames[*pp].prio <= f->prio)
        pp = &frames[*pp].next;

    uint8_t idx = IndexOf(f);
    f->next = *pp;
    *pp = idx;
    if (f->next == kNullIndex) tail = idx;
}

/**
 * @brief 池满挤帧：从队尾往前找第一个非事件帧（优先挤 DBG，其次命令响应）
 * @return 被挤出的帧指针；队列全是事件帧（极端）返回 nullptr
 */
TxFrame* Log::TxFrameQueue::EvictLowestLocked()
{
    if (head == kNullIndex) return nullptr;

    uint8_t prev = kNullIndex, cur = head, victim = kNullIndex, victim_prev = kNullIndex;
    while (cur != kNullIndex)
    {
        if (frames[cur].prio != TxPriority::Event)   // 找最低优先（最大 prio 值）的帧
        {
            if (victim == kNullIndex || frames[cur].prio > frames[victim].prio)
            { victim = cur; victim_prev = prev; }
        }
        prev = cur;
        cur = frames[cur].next;
    }
    if (victim == kNullIndex) return nullptr;     // 全是事件帧（极端）→ 无可挤

    if (victim_prev == kNullIndex) {
		head = frames[victim].next;
	} else {
		frames[victim_prev].next = frames[victim].next;
	}                           
	 	
    if (frames[victim].next == kNullIndex) tail = victim_prev;

    frames[victim].next = kNullIndex;
    frames[victim].is_stale = false;
    return &frames[victim];
}

/**
 * @brief 队列弹头：is_stale 作废帧跳过并回收（DBG 切换及时顶替）
 * @return 待发送帧指针；队列空返回 nullptr
 */
TxFrame* Log::TxFrameQueue::PopLocked()
{
    while (head != kNullIndex)
    {
        uint8_t i = head;
        head = frames[i].next;
        if (head == kNullIndex) tail = kNullIndex;

        frames[i].next = kNullIndex;
        if (frames[i].is_stale)
        {
            ReleaseLocked(&frames[i]);   // 作废帧：回收不发送
            continue;
        }
        return &frames[i];
    }
    return nullptr;
}

/**
 * @brief 切换/关闭时：队列中所有 Dbg 档帧标记作废（Event/Cmd 不动，及时顶替）
 */
void Log::TxFrameQueue::MarkStaleDbgLocked()
{
    uint8_t cur = head;
    while (cur != kNullIndex)
    {
        if (frames[cur].prio == TxPriority::Dbg) frames[cur].is_stale = true;
        cur = frames[cur].next;
    }
}

/**
 * @brief 锁内入队仲裁：三档优先级（事件 > 命令响应 > DBG）
 *
 * 超长截断到 kTxMaxLen；池满挤最低优先级帧（DBG 先、命令次、事件永不挤）；
 * 并发保护：调用者已持有 irq_lock。
 *
 * @param data 发送内容（已格式化，可能含 ANSI 颜色）
 * @param len  数据长度
 * @param prio 优先级档位
 */
bool Log::TxFrameQueue::PushLocked(const char* data, int len, TxPriority prio, bool allow_evict)
{
    if (len > kTxMaxLen) len = kTxMaxLen; // 超长截断

    TxFrame* f = AllocLocked();            // 从空闲池取帧
    if (f == nullptr)
    {
        if (!allow_evict) return false;  // ISR：池满直接丢，不 Evict 遍历链表
        f = EvictLowestLocked();         // 线程上下文池满：挤最低优先级帧（DBG 先、命令次、事件永不挤）
        if (f == nullptr) return false;   // 池满且全是事件帧（极端）→ 丢弃
    }

    std::memcpy(f->data, data, static_cast<size_t>(len));
    f->len = static_cast<uint16_t>(len);
    f->prio = prio;
    f->is_stale = false;

    PushByPrioLocked(f);                  // 按档位插队
    return true;
}

void Log::LogRecordQueue::Reset()
{
    head = kNullIndex;
    tail = kNullIndex;
    free_head = 0;
    for (uint8_t i = 0; i < kMaxLogRecords; ++i)
    {
        records[i].is_stale = false;
        records[i].next = (i + 1 < kMaxLogRecords) ? static_cast<uint8_t>(i + 1) : kNullIndex;
    }
}

uint8_t Log::LogRecordQueue::IndexOf(const LogRecord* r) const
{
    return static_cast<uint8_t>(r - records);
}

bool Log::LogRecordQueue::PushLocked(const char* fmt, LogColor color, TxPriority prio, const uint32_t* args, uint8_t nargs)
{
    if (free_head == kNullIndex) return false;
    if (nargs > kMaxLogArgs) nargs = kMaxLogArgs;

    uint8_t i = free_head;
    free_head = records[i].next;

    records[i].fmt = fmt;
    records[i].color = color;
    records[i].prio = prio;
    records[i].is_stale = false;
    records[i].nargs = nargs;
    for (uint8_t k = 0; k < nargs; ++k) records[i].args[k] = args[k];
    records[i].next = kNullIndex;

    if (head == kNullIndex)
    {
        head = tail = i;
    }
    else
    {
        records[tail].next = i;
        tail = i;
    }
    return true;
}

LogRecord* Log::LogRecordQueue::PopLocked()
{
    while (head != kNullIndex)
    {
        uint8_t i = head;
        head = records[i].next;
        if (head == kNullIndex) tail = kNullIndex;

        records[i].next = kNullIndex;
        if (records[i].is_stale)
        {
            ReleaseLocked(&records[i]);
            continue;
        }
        return &records[i];
    }
    return nullptr;
}

void Log::LogRecordQueue::ReleaseLocked(LogRecord* r)
{
    uint8_t i = IndexOf(r);
    r->is_stale = false;
    r->next = free_head;
    free_head = i;
}

void Log::LogRecordQueue::MarkStaleDbgLocked()
{
    uint8_t cur = head;
    while (cur != kNullIndex)
    {
        if (records[cur].prio == TxPriority::Dbg) records[cur].is_stale = true;
        cur = records[cur].next;
    }
}

TxFrame* Log::Dequeue()
{
    return txq_.PopLocked();
}

void Log::MarkStaleDbg()
{
    unsigned key = irq_lock();            // 并发保护（任务上下文）
    txq_.MarkStaleDbgLocked();
    recq_.MarkStaleDbgLocked();
    irq_unlock(key);
}

bool Log::EnqueueFrame(const char* data, int len, TxPriority prio)
{
    return txq_.PushLocked(data, len, prio, !k_is_in_isr());
}

/**
 * @brief 直发原语：入帧池 + 立即 SendFrame（调用点直发）
 *
 * 入队成功后若 DMA 空闲，调用点直接弹帧提交发送（帧即取即还），
 * 不依赖 shell 线程泵。用于命令响应 / boot 早期同步段直发路径。
 *
 * @param data 发送内容（已格式化，可能含 ANSI 颜色）
 * @param len  数据长度
 * @param prio 优先级档位
 */
void Log::PublishDirect(const char* data, int len, TxPriority prio)
{
    unsigned key = irq_lock();            // 并发保护

    if (EnqueueFrame(data, len, prio))
    {
        if (!sending_)                    // DMA 空闲 → 调用点直发（帧即取即还）
        {
            TxFrame* next = Dequeue();
            if (next != nullptr) SendFrame(next);
        }
    }

    irq_unlock(key);
}

/**
 * @brief 异步原语：入帧池 + give（shell 线程泵发送）
 *
 * 入队成功后若 DMA 空闲，give 唤醒 shell 线程去 PumpSend 续发。
 * 用于异步段格式化后入队（FormatRecord 结果），DMA 提交交给 shell 线程。
 *
 * @param data 发送内容（已格式化，可能含 ANSI 颜色）
 * @param len  数据长度
 * @param prio 优先级档位
 */
void Log::PublishQueued(const char* data, int len, TxPriority prio)
{
    unsigned key = irq_lock();            // 并发保护

    if (EnqueueFrame(data, len, prio))
    {
        if (!sending_ && stream_ != nullptr) k_sem_give(&stream_->sem_);   // shell 泵
    }

    irq_unlock(key);
}

/**
 * @brief log 命令入口（log list/on/off）
 * @param line 子命令参数（不含 "log" 前缀）
 */
void Log::Process(uint8_t* line)
{
    while (*line == ' ') line++;
    uint8_t* sub = line;
    while (*line && *line != ' ') line++;
    if (*line == ' ') { *line = '\0'; line++; }
    while (*line == ' ') line++;

    if (std::strcmp(reinterpret_cast<const char*>(sub), "list") == 0)
    {
        CmdLogList();
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "on") == 0)
    {
        if (Select(reinterpret_cast<const char*>(line))) SendLine("log on: ok");
        else SendLine("log on: not found");
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "off") == 0)
    {
        Deselect();
        SendLine("log off: ok");
    }
    else SendLine("?: log list|on <name>|off");
}

/**
 * @brief log list：遍历所有已注册 DBG 条目输出（名字 + 选中状态）
 */
void Log::CmdLogList()
{
    const LogEntry* active = Active();
    for (const LogEntry* e = First(); e != nullptr; e = Next(e))
    {
        char line[160];
        snprintf(line, sizeof(line), "%s %s", e->name,
                 (e == active) ? "[ON]" : "[off]");
        SendLine(line);
    }
}

} // namespace debug

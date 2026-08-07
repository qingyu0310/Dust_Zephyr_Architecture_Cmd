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
    tx_head_ = kNullIndex;
    tx_tail_ = kNullIndex;

    free_head_ = 0;
    for (uint8_t i = 0; i < kTxPoolCount; ++i)
    {
        tx_pool_[i].next = (i + 1 < kTxPoolCount) ? static_cast<uint8_t>(i + 1) : kNullIndex;
    }
    return true;
}

/**
 * @brief 绑定发送通道
 * @param uart 发送通道指针（shell thread_init 传入 rx）
 */
void Log::BindUart(UartDma* uart)
{
    uart_ = uart;
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
 * @brief 停止打印：active_ 清空，队列中残留 Dbg 档帧作废
 */
void Log::Deselect()
{
    active_ = nullptr;
    MarkStaleDbg();               // log off：队列中残留的 Dbg 档帧作废
}

/**
 * @brief 当前选中条目（log list 显示 [ON]）
 * @return 选中条目指针；无选中返回 nullptr
 */
const LogEntry* Log::Active()
{
    return active_;
}

/**
 * @brief 遍历起点：数组首个条目
 * @return 首个条目指针；无条目返回 nullptr
 */
LogEntry* Log::First()
{
    return (count_ > 0) ? &entries_[0] : nullptr;
}

/**
 * @brief 遍历下一个：数组下一个条目
 * @param e 当前条目指针
 * @return 下一个条目指针；到尾部返回 nullptr
 */
LogEntry* Log::Next(const LogEntry* e)
{
    ptrdiff_t idx = e - entries_;        // 数组索引 = 指针差
    return (idx + 1 < count_) ? &entries_[idx + 1] : nullptr;
}

/**
 * @brief DBG 流式打印（仅当 e 是当前选中条目才发，白色，低优先级）
 * @param e   DBG 条目（FindOrCreate 返回值）
 * @param fmt 格式化串
 */
void Log::Dbgl(LogEntry* e, const char* fmt, ...)
{
    if (e == nullptr) return;               // 池满（FindOrCreate 返回 nullptr）：静默丢弃
    if (e != active_) return;               // 未选中，静默

    char buf[kLogBufSize];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char out[kLogBufSize + kColorOutExtra];
    int n = snprintf(out, sizeof(out), "%s\r\n", buf);
    if (n > 0) TrySend(out, n, TxPriority::Dbg);   // DBG = 最低档（让位事件/命令）
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
 * @param c   颜色
 * @param fmt 格式化串
 * @param ap  可变参数
 */
void Log::PrintColor(LogColor c, const char* fmt, va_list ap)
{
    char buf[kLogBufSize];
    vsnprintf(buf, sizeof(buf), fmt, ap);   // 格式化（picolibc 支持 %f，prj.conf 已开 PICOLIBC_IO_FLOAT）

    char out[kLogBufSize + kColorOutExtra];
    const uint32_t rgb = static_cast<uint32_t>(c);
    int n = snprintf(out, sizeof(out), "\x1b[38;2;%d;%d;%dm%s\x1b[0m\r\n", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, buf);
    if (n > 0) TrySend(out, n, TxPriority::Event);   // 四色直发 = 事件档（最高）
}

/**
 * @brief 命令响应直发（不经过 log 过滤，带 \r\n，中档）
 * @param text 响应文本
 */
void Log::SendLine(const char* text)
{
    char out[kLogBufSize + kLineOutExtra];
    int n = snprintf(out, sizeof(out), "%s\r\n", text);
    if (n > 0) TrySend(out, n, TxPriority::Cmd);   // 命令响应 = 中档（事件后、DBG 前）
}

/**
 * @brief TX_DONE 回调（UartDma tx_cb，ISR 上下文）
 *
 * 清发送标志后取队头下一帧继续驱动式发送；作废帧由 Dequeue 跳过回收。
 */
void Log::OnTxDone()
{
    unsigned key = irq_lock();            // 并发保护（ISR 上下文）
    sending_ = false;
    TxFrame* f = Dequeue();               // 取队头下一帧（作废帧已跳过）
    if (f != nullptr) SendFrame(f);       // 继续驱动式发送
    irq_unlock(key);
}

/**
 * @brief 发送一帧：DMA 发送，无论成败帧立即归还空闲池
 *
 * Send 内部已把帧内容拷贝进 UartDma 发送缓冲（uart.cpp:196 memcpy），
 * DMA 搬运的是 UartDma 缓冲而非 TxFrame::data，Send 返回后帧即可复用。
 *
 * @param f 待发送帧
 */
void Log::SendFrame(TxFrame* f)
{
    sending_ = true;
    if (uart_ == nullptr || !uart_->Send(reinterpret_cast<const uint8_t*>(f->data), f->len))
    {
        sending_ = false;                 // 发送失败：不置发送中
    }
    RecycleFrame(f);                      // 归还空闲池：帧内容已移交 UartDma，立即复用
}

/**
 * @brief 帧指针 → 数组索引
 * @param f 帧指针
 * @return 数组索引
 */
uint8_t Log::IndexOf(TxFrame* f)
{
    return static_cast<uint8_t>(f - tx_pool_);
}

/**
 * @brief 从空闲池取帧
 * @return 帧指针；池空返回 nullptr
 */
TxFrame* Log::AllocFrame()
{
    if (free_head_ == kNullIndex) return nullptr;

    uint8_t i = free_head_;
    free_head_ = tx_pool_[i].next;
    tx_pool_[i].next = kNullIndex;
    return &tx_pool_[i];
}

/**
 * @brief 帧归还空闲池
 * @param f 帧指针
 */
void Log::RecycleFrame(TxFrame* f)
{
    uint8_t i = IndexOf(f);
    f->next = free_head_;
    free_head_ = i;
}

/**
 * @brief 按优先级入队：Event 插队头，Cmd 插事件后/DBG 前，Dbg 排队尾
 *
 * 同档帧保持先来后到（prio 相同的帧插在后面）。
 * @param f 待入队帧
 */
void Log::EnqueueByPrio(TxFrame* f)
{
    f->next = kNullIndex;
    if (tx_head_ == kNullIndex) { tx_head_ = tx_tail_ = IndexOf(f); return; }

    uint8_t* pp = &tx_head_;              // 找插入点：第一个 prio > f->prio 的帧之前
    while (*pp != kNullIndex && tx_pool_[*pp].prio <= f->prio)
        pp = &tx_pool_[*pp].next;

    uint8_t idx = IndexOf(f);
    f->next = *pp;
    *pp = idx;
    if (f->next == kNullIndex) tx_tail_ = idx;
}

/**
 * @brief 池满挤帧：从队尾往前找第一个非事件帧（优先挤 DBG，其次命令响应）
 * @return 被挤出的帧指针；队列全是事件帧（极端）返回 nullptr
 */
TxFrame* Log::EvictLowest()
{
    if (tx_head_ == kNullIndex) return nullptr;

    uint8_t prev = kNullIndex, cur = tx_head_, victim = kNullIndex, victim_prev = kNullIndex;
    while (cur != kNullIndex)
    {
        if (tx_pool_[cur].prio != TxPriority::Event)   // 找最低优先（最大 prio 值）的帧
        {
            if (victim == kNullIndex || tx_pool_[cur].prio > tx_pool_[victim].prio)
            { victim = cur; victim_prev = prev; }
        }
        prev = cur;
        cur = tx_pool_[cur].next;
    }
    if (victim == kNullIndex) return nullptr;     // 全是事件帧（极端）→ 无可挤

    if (victim_prev == kNullIndex) tx_head_ = tx_pool_[victim].next;
    else                           tx_pool_[victim_prev].next = tx_pool_[victim].next;
    if (tx_pool_[victim].next == kNullIndex) tx_tail_ = victim_prev;

    tx_pool_[victim].next = kNullIndex;
    return &tx_pool_[victim];
}

/**
 * @brief 队列弹头：is_stale 作废帧跳过并回收（DBG 切换及时顶替）
 * @return 待发送帧指针；队列空返回 nullptr
 */
TxFrame* Log::Dequeue()
{
    while (tx_head_ != kNullIndex)
    {
        uint8_t i = tx_head_;
        tx_head_ = tx_pool_[i].next;
        if (tx_head_ == kNullIndex) tx_tail_ = kNullIndex;

        tx_pool_[i].next = kNullIndex;
        if (tx_pool_[i].is_stale)
        {
            RecycleFrame(&tx_pool_[i]);   // 作废帧：回收不发送
            continue;
        }
        return &tx_pool_[i];
    }
    return nullptr;
}

/**
 * @brief 切换/关闭时：队列中所有 Dbg 档帧标记作废（Event/Cmd 不动，及时顶替）
 */
void Log::MarkStaleDbg()
{
    unsigned key = irq_lock();            // 并发保护（任务上下文）
    uint8_t cur = tx_head_;
    while (cur != kNullIndex)
    {
        if (tx_pool_[cur].prio == TxPriority::Dbg) tx_pool_[cur].is_stale = true;
        cur = tx_pool_[cur].next;
    }
    irq_unlock(key);
}

/**
 * @brief 入队仲裁：三档优先级（事件 > 命令响应 > DBG）
 *
 * 超长截断到 kTxMaxLen；池满挤最低优先级帧（DBG 先、命令次、事件永不挤）；
 * DMA 空闲时立即启动发送链。并发保护：本函数在 irq_lock 内完成全部队列操作。
 *
 * @param data 发送内容（已格式化，可能含 ANSI 颜色）
 * @param len  数据长度
 * @param prio 优先级档位
 */
void Log::TrySend(const char* data, int len, TxPriority prio)
{
    if (len > kTxMaxLen) len = kTxMaxLen; // 超长截断（发送最长字节长度规定）

    unsigned key = irq_lock();            // 并发保护（任务上下文）

    TxFrame* f = AllocFrame();            // 从空闲池取帧
    if (f == nullptr)
    {
        f = EvictLowest();                // 池满：挤掉最低优先级帧（DBG 先、命令次、事件永不挤）
        if (f == nullptr) { irq_unlock(key); return; }  // 池满且全是事件帧（极端）→ 丢弃
    }

    memcpy(f->data, data, static_cast<size_t>(len));
    f->len = static_cast<uint16_t>(len);
    f->prio = prio;
    f->is_stale = false;

    EnqueueByPrio(f);                     // 按档位插队

    if (!sending_) {                      // DMA 空闲 → 立即启动发送链
        TxFrame* next = Dequeue();
        if (next != nullptr) SendFrame(next);
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

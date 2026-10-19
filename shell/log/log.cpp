/**
 * @file log.cpp
 * @author qingyu
 * @brief DUST_LOG 自研日志系统实现 — 帧池三档优先级仲裁发送
 * @version 1.0
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "log.hpp"
#include <cstdio>
#include <cstring>

#pragma message "Compiling Cmd/Shell/Log"

namespace debug {

/**
 * @brief 初始化日志系统
 *
 * 清空选中状态/条目计数，重建空闲帧链表（0→1→…→255），
 * 发送队列置空。DBG 条目由 FindOrCreateDebugEntry 运行时创建，无链接段遍历。
 *
 * @return true 初始化成功
 */
bool Log::Init()
{
    dbg_.Reset();
    pump_.Reset();
    txq_.ResetFrameQueue();
    recq_.ResetRecordQueue();
    return true;
}

/**
 * @brief 选中 DBG 条目：静默 → 流式打印
 *
 * 只选中已存在的条目（代码里 DUST_LOG_DBG 注册过的名字），
 * 不存在返回 false（log on 回 not found），不创建幽灵条目。
 * 由注册表改选中（旧选中自动被顶替，同一时间只打一条），选择代数递增，
 * 队列中残留的旧 Dbg 档记录/帧在出队时按代数不一致作废。
 *
 * @param name DBG 名字
 * @return true 选中成功；false 条目不存在
 */
bool Log::SelectDebugEntry(const char* name)
{
    return dbg_.Select(name);                      // 代数递增：在途旧 DBG 出队时作废
}

/**
 * @brief DBG 流式打印（仅当 e 是当前选中条目才发；形态按条目标志：默认上色，vofa 模式纯文本，低优先级）
 *
 * 分时段：同步段调用点 vsnprintf + 直发；异步段参数快照入队，shell 线程 FormatRecordToText。
 *
 * @param e   DBG 条目（FindOrCreateDebugEntry 返回值）
 * @param fmt 格式化串
 */
void Log::PrintSelectedDebug(LogEntry* e, const char* fmt, ...)
{
	if (!shell_own_) return;                		// boot 早期无 DBG（未选中不可用），直接返回

    if (e == nullptr) return;               		// 池满（FindOrCreateDebugEntry 返回 nullptr）：静默丢弃
    if (e != dbg_.Active()) return;         		// 未选中，静默

    va_list ap;
    va_start(ap, fmt);

    // 异步段（shell 接管后）：参数快照（DBG 纯文本）
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    BuildRecordFromVaList(fmt, ap, args, &nargs);
    va_end(ap);

    QueueLogRecord(fmt, LogChannel::Debug, dbg_.ActiveEpoch(), !e->vofa, args, nargs);   // 模式取条目标志

    unsigned key = irq_lock();
    if (!pump_.Sending() && pump_.Channel() != nullptr) k_sem_give(&pump_.Channel()->sem_);
    irq_unlock(key);
}

/**
 * @brief 一次性日志，黑色
 * @param fmt 格式化串
 */
void Log::PrintInfo(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColoredLog(LogChannel::Info, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，红色
 * @param fmt 格式化串
 */
void Log::PrintError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColoredLog(LogChannel::Error, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，绿色（状态正常）
 * @param fmt 格式化串
 */
void Log::PrintOk(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColoredLog(LogChannel::Ok, fmt, ap);
    va_end(ap);
}

/**
 * @brief 一次性日志，橘色
 * @param fmt 格式化串
 */
void Log::PrintWarning(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    PrintColoredLog(LogChannel::Warning, fmt, ap);
    va_end(ap);
}

/**
 * @brief 通用：按通道策略格式化 + ANSI 上色 + 仲裁发送
 *
 * 分时段：boot 早期（!shell_own_）调用点 vsnprintf + 直发（无实时要求，现状）；
 * shell 接管后（shell_own_）参数快照入队，格式化由 shell 线程 FormatRecordToText 完成。
 *
 * @param channel 日志通道（取策略：颜色/前缀/ANSI/优先级）
 * @param fmt     格式化串
 * @param ap      可变参数
 */
void Log::PrintColoredLog(LogChannel channel, const char* fmt, va_list ap)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(channel);

    if (!shell_own_)                          													// 同步段（boot 早期）：调用点直发，现状不变
    {
        char buf[kLogBufSize];
        vsnprintf(buf, sizeof(buf), fmt, ap);   												// 格式化（picolibc 支持 %f，prj.conf 已开 PICOLIBC_IO_FLOAT）

        char out[kLogBufSize + kColorOutExtra];
        int n = WrapOutputFrame(policy.prefix, policy.ansi_color, policy.color, buf, out, sizeof(out));
        if (n > 0) QueueFrameAndSendNow(out, n, policy.priority, 0);   	  // boot 早期普通日志：调用点直发
        return;
    }

    // 异步段（shell 接管后）：参数快照入队，调用点不格式化
    uint32_t args[kMaxLogArgs];
    uint8_t  nargs = 0;
    BuildRecordFromVaList(fmt, ap, args, &nargs);      										// 在 va_end 前快照

    QueueLogRecord(fmt, channel, 0, policy.ansi_color, args, nargs);    					// 入原始请求队列

    unsigned key = irq_lock();
    if (!pump_.Sending() && pump_.Channel() != nullptr) k_sem_give(&pump_.Channel()->sem_);   	// 唤醒 shell 去 FormatRecordToText
    irq_unlock(key);
}

/**
 * @brief 命令响应直发（不经过 log 过滤，带 \r\n，中档）
 * @param text 响应文本
 */
void Log::SendCommandLine(const char* text)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(LogChannel::Command);
    char out[kLogBufSize + kLineOutExtra];
    int n = WrapOutputFrame(policy.prefix, policy.ansi_color, policy.color, text, out, sizeof(out));
    if (n > 0) QueueFrameAndSendNow(out, n, policy.priority, 0);   			 // 命令响应：调用点直发（帧池 8 缓冲突发）
}

/**
 * @brief 入队日志原始请求（异步段调用点，任务或 ISR 上下文）
 *
 * 短临界区：池满直接丢（不遍历挤，ISR 安全）。FIFO 尾插保证事件日志不乱序。
 *
 * @param fmt     格式串（静态字面量）
 * @param channel 日志通道（取策略填 color/prio）
 * @param epoch   DBG 选择代数（非 DBG 传 0）
 * @param ansi    是否上 ANSI 颜色
 * @param args    参数槽
 * @param nargs   槽数
 */
void Log::QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(channel);
    unsigned key = irq_lock();                														// 并发保护（任务或 ISR 上下文）
    (void)recq_.PushLogRecordLocked(fmt, channel, policy.color, policy.priority, epoch, ansi, args, nargs);
    irq_unlock(key);
}

/**
 * @brief 处理一条日志原始请求（shell 线程）
 *
 * 记录在格式化完成后再归还空闲链，避免出队后立即被 ISR 复用。
 * @return true 处理了一条记录；false 队列为空
 */
bool Log::FormatOnePendingRecord()
{
    unsigned key = irq_lock();
    LogRecord* r = recq_.PopNextRecordLocked(dbg_.ActiveEpoch());
    irq_unlock(key);

    if (r == nullptr) return false;

    char text[kLogBufSize];
    FormatRecordToText(*r, text, sizeof(text));           								// 记录展开为纯文本

    const LogChannelPolicy& policy = GetLogChannelPolicy(r->channel);
    char out[kLogBufSize + kColorOutExtra];
    int n = WrapOutputFrame(policy.prefix, r->ansi, r->color, text, out, sizeof(out));
    if (n > 0) QueueFrameForShellSend(out, n, r->prio, r->epoch);  		 	   // 线程展开后按原始请求档位入队

    key = irq_lock();
    recq_.ReleaseRecordLocked(r);
    irq_unlock(key);
    return true;
}

/**
 * @brief 直发原语：入帧池 + 立即提交发送（调用点直发）
 *
 * 入队成功后若 DMA 空闲，调用点直接弹帧提交发送（帧即取即还），
 * 不依赖 shell 线程泵。用于命令响应 / boot 早期同步段直发路径。
 *
 * @param data  发送内容（已格式化，可能含 ANSI 颜色）
 * @param len   数据长度
 * @param prio  优先级档位
 * @param epoch DBG 选择代数（非 DBG 传 0）
 */
void Log::QueueFrameAndSendNow(const char* data, int len, TxPriority prio, uint16_t epoch)
{
    unsigned key = irq_lock();            // 并发保护

    if (txq_.PushFrameLocked(data, len, prio, epoch, !k_is_in_isr()))   	 // 入帧池：截断 + 取帧 + 按档插队（ISR 池满直接丢）
    {
        pump_.PumpTxQueue(txq_, dbg_.ActiveEpoch());   				// DMA 空闲 → 调用点直发（帧即取即还）
    }

    irq_unlock(key);
}

/**
 * @brief 异步原语：入帧池 + give（shell 线程泵发送）
 *
 * 入队成功后若 DMA 空闲，give 唤醒 shell 线程去 SendNextFrameIfIdle 续发。
 * 用于异步段格式化后入队（FormatRecordToText 结果），DMA 提交交给 shell 线程。
 *
 * @param data  发送内容（已格式化，可能含 ANSI 颜色）
 * @param len   数据长度
 * @param prio  优先级档位
 * @param epoch DBG 选择代数（非 DBG 传 0）
 */
void Log::QueueFrameForShellSend(const char* data, int len, TxPriority prio, uint16_t epoch)
{
    unsigned key = irq_lock();            // 并发保护

    if (txq_.PushFrameLocked(data, len, prio, epoch, !k_is_in_isr()))   // 入帧池：截断 + 取帧 + 按档插队（ISR 池满直接丢）
    {
        if (!pump_.Sending() && pump_.Channel() != nullptr) k_sem_give(&pump_.Channel()->sem_);   // shell 泵
    }

    irq_unlock(key);
}

/**
 * @brief log 命令入口（log list/on/off/mode）
 * @param line 子命令参数（不含 "log" 前缀）
 */
void Log::ProcessLogCommand(uint8_t* line)
{
    while (*line == ' ') line++;
    uint8_t* sub = line;
    while (*line && *line != ' ') line++;
    if (*line == ' ') { *line = '\0'; line++; }
    while (*line == ' ') line++;

    if (std::strcmp(reinterpret_cast<const char*>(sub), "list") == 0)
    {
        PrintLogList();
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "on") == 0)
    {
        if (SelectDebugEntry(reinterpret_cast<const char*>(line))) SendCommandLine("log on: ok");
        else SendCommandLine("log on: not found");
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "off") == 0)
    {
        DeselectDebugEntry();
        SendCommandLine("log off: ok");
    }
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "mode") == 0)
    {
        // line 形如 "<name> on|off"（on=VOFA+ 纯文本，off=上色；vofa/color 为同义别名）
        uint8_t* name = line;
        while (*line && *line != ' ') line++;
        if (*line == ' ') { *line = '\0'; line++; }
        while (*line == ' ') line++;

        if (std::strcmp(reinterpret_cast<const char*>(line), "on") == 0 ||
            std::strcmp(reinterpret_cast<const char*>(line), "vofa") == 0)
        {
            if (SetDebugVofa(reinterpret_cast<const char*>(name), true)) SendCommandLine("log mode: vofa");
            else SendCommandLine("log mode: not found");
        }
        else if (std::strcmp(reinterpret_cast<const char*>(line), "off") == 0 ||
                 std::strcmp(reinterpret_cast<const char*>(line), "color") == 0)
        {
            if (SetDebugVofa(reinterpret_cast<const char*>(name), false)) SendCommandLine("log mode: color");
            else SendCommandLine("log mode: not found");
        }
        else SendCommandLine("?: log mode <name> on|off");
    }
    else SendCommandLine("?: log list|on <name>|off|mode <name> on|off");
}

/**
 * @brief log list：遍历所有已注册 DBG 条目输出（名字 + 选中状态 + 输出模式）
 */
void Log::PrintLogList()
{
    const LogEntry* active = ActiveDebugEntry();
    for (const LogEntry* e = FirstDebugEntry(); e != nullptr; e = NextDebugEntry(e))
    {
        char line[160];
        snprintf(line, sizeof(line), "%s %s %s", e->name, (e == active) ? "[ON]" : "[off]", e->vofa ? "[vofa]" : "[color]");
        SendCommandLine(line);
    }
}

} // namespace debug

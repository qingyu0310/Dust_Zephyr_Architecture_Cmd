/**
 * @file log.hpp
 * @author qingyu
 * @brief DUST_LOG 自研日志系统 — 四色一次性日志 + DBG 可选择流式日志
 * @version 1.0
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <cstdarg>
#include <cstdint>
#include <zephyr/kernel.h>
#include "stream.hpp"
#include "log_policy.hpp"
#include "log_debug.hpp"
#include "log_queue.hpp"
#include "log_transport.hpp"

namespace debug {

/**
 * @brief DUST_LOG 日志系统
 *
 * 一次性四色日志（INF/ERR/OK/WRN）调用即 DMA 打印，无名字无选择；
 * DBG 流式日志带名字、默认静默，log on <name> 选中后打印（同一时间只打一条）。
 * 所有发送统一走三档优先级帧队列（事件 > 命令响应 > DBG），帧池 8×128B。
 *
 * 容量（实测 2026-08-06）：当前 8 帧池 → 同一时间最多连发 9 条（第 10 条起丢）；
 * 稳态平均速率 < 92160 B/s（921600 波特）时无限闭环。
 */
class Log
{
public:
    static bool      Init();                             											// 初始化：清选中/条目计数/队列池
    static void      BindOutputStream(Stream* s) { pump_.Bind(s); }									// 绑定发送通道（shell thread_init 调用）
    static LogEntry* FindOrCreateDebugEntry(const char* name) { return dbg_.FindOrCreate(name); }	// 按名字查 DBG 条目，首见创建（返回 nullptr=池满）
    static bool      SelectDebugEntry(const char* name);           									// 选中：注册表指向该条目（同一时间只保留一条）
    static void      DeselectDebugEntry() { dbg_.Deselect(); }                  					// 停止打印：清选中（代数递增作废在途）
    static bool      SetDebugVofa(const char* name, bool on) { return dbg_.SetVofa(name, on); }	// 设置 DBG 输出模式（on=VOFA+ 纯文本，off=上色）

    static void PrintSelectedDebug(LogEntry* e, const char* fmt, ...);								// DBG 流式打印（仅 e==当前选中条目才发；形态按条目标志：默认上色，vofa 模式纯文本）
    static void PrintInfo(const char* fmt, ...);													// 一次性，黑色
    static void PrintError(const char* fmt, ...);													// 一次性，红色
    static void PrintOk(const char* fmt, ...);														// 一次性，绿色
    static void PrintWarning(const char* fmt, ...);													// 一次性，橘色

    static void SendCommandLine(const char* text);													// 命令响应直发（不经过 log 过滤，带 \r\n）
    static void MarkShellThreadReady() { shell_own_ = true; }										// shell 线程接管发送（Task 首行调用）
    static void ProcessLogCommand(uint8_t* line);													// log 命令入口（list/on/off）
    static void PrintLogList();																		// log list：遍历数组输出
    static bool FormatOnePendingRecord();															// 处理一条原始请求：出队、展开、归还

    static void SendNextFrameIfIdle()																// DMA 空闲则续发下一帧（shell 线程驱动）
    {
        unsigned key = irq_lock();
        pump_.PumpTxQueue(txq_, dbg_.ActiveEpoch());   						   // 弹队头（作废帧跳过回收）→ 提交发送
        irq_unlock(key);
    }

	static void HandleTxDone() { pump_.HandleTxDone(); }											// TX_DONE 回调（UartDma tx_cb）

	static const LogEntry* ActiveDebugEntry()														// 当前选中条目（log list 显示 [ON]；返回 nullptr=无）
    {
        return dbg_.Active();
    }
	static LogEntry* FirstDebugEntry()																// log list 遍历：数组首个条目（返回 nullptr=空）
    {
		return dbg_.First();
	}
    static LogEntry* NextDebugEntry(const LogEntry* e)												// log list 遍历：数组下一个条目（返回 nullptr=尾）
    {
		return dbg_.Next(e);
	}

private:
    static inline DebugSourceRegistry dbg_ {};           											// DBG 源注册表（名字/选中/遍历）
    static inline LogTransport      pump_ {};            											// 发送泵（通道/发送中标志/续发）
    static inline bool      shell_own_  = false;         											// shell 线程已接管发送（Task 首行置 true，boot 早期为 false 直发）

    static inline TxFrameQueue   txq_   {};             											// 发送帧池 + 三档优先级队列
    static inline LogRecordQueue recq_  {};             											// 原始请求池 + FIFO 队列

    static void PrintColoredLog(LogChannel channel, const char* fmt, va_list ap);												// 通用：按通道策略上色 + 仲裁发送
    static void QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs);	// 入队原始请求（异步段调用点）
	static void QueueFrameAndSendNow(const char* data, int len, TxPriority prio, uint16_t epoch);								// 直发原语：入帧池+立即发（命令响应/boot 日志）
	static void QueueFrameForShellSend(const char* data, int len, TxPriority prio, uint16_t epoch);								// 异步原语：入帧池+give（shell 线程 FormatRecordToText 后）
};

} // namespace debug

// ========== 开：真实实现 ==========

// DBG：流式调试日志（默认静默，log on <name> 选中后打印）
// 名字是字符串字面量，运行时 FindOrCreateDebugEntry 创建/复用条目，无需 DEFINE/链接段
#define DUST_LOG_DBG(name_, ...) ::debug::Log::PrintSelectedDebug(::debug::Log::FindOrCreateDebugEntry(name_), ##__VA_ARGS__)

// 一次性四色（调用即打，无名字，用法与 LOG_INF 一致；[等级] 前缀由通道策略补）
// 每帧字节开销（TrueColor 38;2;R;G;B）：[inf]前缀(5) + 颜色(最长18 \x1b[38;2;246;167;83m) + 复位 \x1b[0m(4) + \r\n(2) = 最长29B；
// 帧上限 127B（kTxMaxLen 截断）→ 内容建议 ≤98B，超出截尾
#define DUST_LOG_INF(...) ::debug::Log::PrintInfo(__VA_ARGS__)
#define DUST_LOG_ERR(...) ::debug::Log::PrintError(__VA_ARGS__)
#define DUST_LOG_OK(...)  ::debug::Log::PrintOk(__VA_ARGS__)
#define DUST_LOG_WRN(...) ::debug::Log::PrintWarning(__VA_ARGS__)

#else

// ========== 关：空宏（调用点零开销，编译不报错）==========

#define DUST_LOG_DBG(...)
#define DUST_LOG_INF(...)
#define DUST_LOG_ERR(...)
#define DUST_LOG_OK(...)
#define DUST_LOG_WRN(...)

#endif

/**
 * @file log.hpp
 * @author qingyu
 * @brief DUST_LOG 自研日志系统 — 四色一次性日志 + DBG 可选择流式日志
 * @version 0.1
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <cstdarg>
#include <cstdint>
#include <zephyr/kernel.h>
#include "uart.hpp"

namespace debug {

/**
 * @brief 日志颜色（ANSI 前景色编码）
 */
enum class LogColor : uint32_t
{
    Black  = 0x000000,  // 黑（INF）
    Red    = 0xF50002,  // 亮红（ERR）
    Green  = 0x00F700,  // 亮绿（OK）
    Orange = 0xF6A753,  // 亮橙（WRN）
    White  = 0xFFFFFF,  // 白（DBG）
};

/**
 * @brief 发送优先级（数值越小越靠前发）
 */
enum class TxPriority : uint8_t
{
    Event = 0,   								// INF/ERR/OK/WRN：最高，插队头，永不挤
    Cmd   = 1,   								// 命令响应（var/log 输出）：中，插事件后、DBG 前
    Dbg   = 2,   								// DBG 流式：最低，排队尾，先被挤
};

constexpr uint16_t kTxFrameSize   = 128;		// 发送帧数据区大小（含 \0 保险）
constexpr uint16_t kTxMaxLen      = 127;		// 发送最长字节长度：超了截断
// 帧数：4 × 128B = 512B 内存池
// 容量语义（实测 2026-08-06）：K 帧池 → 同一时间最多连发 K+1 条（1 DMA 中 + K 排队），
// 第 K+2 条起丢（事件永不挤）；稳态平均速率 < 波特率/10（921600 → 92160 B/s）且
// 帧池 ≥ 峰值突发条数时，可无限闭环（永不枯竭）
constexpr uint8_t  kTxPoolCount   = 4;
constexpr uint8_t  kMaxLogEntries = 64;			// DBG 条目数上限
constexpr uint8_t  kNullIndex     = 255;		// 帧索引链表空值（255 = 无下一帧）

/**
 * @brief 发送帧（三档共用）
 */
struct TxFrame
{
    char       data[kTxFrameSize];   // 格式化后内容（含 ANSI 颜色），超 kTxMaxLen 截断
    uint16_t   len;                  // 实际有效长度（≤127）
    TxPriority prio;                 // 优先级档位（§5.1 优先级表）
    bool       is_stale;             // DBG 切换时标记作废（及时顶替用）
    uint8_t    next;                 // 帧索引链表（255=nullptr）
};

/**
 * @brief DBG 日志条目（只服务 DBG）
 */
struct LogEntry
{
    const char* name;   // DBG 名字（log on 用，运行时 FindOrCreate 创建）
};

/**
 * @brief DUST_LOG 日志系统
 *
 * 一次性四色日志（INF/ERR/OK/WRN）调用即 DMA 打印，无名字无选择；
 * DBG 流式日志带名字、默认静默，log on <name> 选中后打印（同一时间只打一条）。
 * 所有发送统一走三档优先级帧队列（事件 > 命令响应 > DBG），帧池 4×128B。
 *
 * 容量（实测 2026-08-06）：当前 4 帧池 → 同一时间最多连发 5 条（第 6 条起丢）；
 * 稳态平均速率 < 92160 B/s（921600 波特）时无限闭环。
 */
class Log
{
public:
    static bool Init();                                  					// 初始化：清 active_/计数/空闲帧链
    static void BindUart(UartDma* uart);                 					// 绑定发送通道（shell thread_init 调用）
    static LogEntry* FindOrCreate(const char* name);     					// 按名字查 DBG 条目，首见创建（返回 nullptr=池满）
    static bool Select(const char* name);                					// 选中：active_ 指向该条目（同一时间只保留一条）
    static void Deselect();                              					// 停止打印：active_ = nullptr
    static const LogEntry* Active();                     					// 当前选中条目（log list 显示 [ON]；返回 nullptr=无）
    static LogEntry* First();                            					// log list 遍历：数组首个条目（返回 nullptr=空）
    static LogEntry* Next(const LogEntry* e);            					// log list 遍历：数组下一个条目（返回 nullptr=尾）
    static void Dbgl(LogEntry* e, const char* fmt, ...); 					// DBG 流式打印（仅 e==active_ 才发，白色）
    static void Inf(const char* fmt, ...);               					// 一次性，黑色
    static void Err(const char* fmt, ...);               					// 一次性，红色
    static void Ok(const char* fmt, ...);                					// 一次性，绿色
    static void Wrn(const char* fmt, ...);               					// 一次性，橘色
    static void SendLine(const char* text);              					// 命令响应直发（不经过 log 过滤，带 \r\n）
    static void OnTxDone();                              					// TX_DONE 回调（UartDma tx_cb，驱动续发；shell thread_init 注册）
    static void Process(uint8_t* line);                  					// log 命令入口（list/on/off）
    static void CmdLogList();                            					// log list：遍历数组输出

private:
    static inline LogEntry  entries_[kMaxLogEntries] {}; 					// 64 条静态池（DBG，运行时注册）
    static inline LogEntry* active_     = nullptr;       					// 当前选中条目（log on 指向、log off 置空；同一时间只打一条）
    static inline uint8_t   count_      = 0;             					// 已注册条目数
    static inline UartDma*  uart_       = nullptr;       					// 发送通道（BindUart 绑定）
    static inline TxFrame   tx_pool_[kTxPoolCount] {};   					// 帧池 4×128B=512B
    static inline uint8_t   free_head_  = 0;             					// 空闲帧索引链表头
    static inline uint8_t   tx_head_    = kNullIndex;    					// 发送帧队列头（三档共用）
    static inline uint8_t   tx_tail_    = kNullIndex;    					// 发送帧队列尾（三档共用）
    static inline bool      sending_    = false;         					// 当前是否有帧在 DMA 中

    static uint8_t   IndexOf(TxFrame* f);                					// 帧指针 → 数组索引
    static TxFrame*  AllocFrame();                       					// 空闲池取帧（无则 nullptr）
    static TxFrame*  EvictLowest();                      					// 池满：挤最低优先级帧（DBG 先、命令次、事件永不挤）
    static void      RecycleFrame(TxFrame* f);           					// 帧归还空闲池
    static void      EnqueueByPrio(TxFrame* f);          					// 按三档优先级插队（同档先来后到）
    static TxFrame*  Dequeue();                          					// 取队头帧（作废帧跳过回收）
    static void      MarkStaleDbg();                     					// 切换/关闭时：队列中 Dbg 档帧标记作废（及时顶替）
    static void      SendFrame(TxFrame* f);              					// DMA 发送一帧
    static void      TrySend(const char* data, int len, TxPriority prio);  	// 发送仲裁（三档）
    static void      PrintColor(LogColor c, const char* fmt, va_list ap);  	// 通用：上色 + 仲裁发送
};

} // namespace debug

// ========== 开：真实实现 ==========

// DBG：流式调试日志（默认静默，log on <name> 选中后打印）
// 名字是字符串字面量，运行时 FindOrCreate 创建/复用条目，无需 DEFINE/链接段
#define DUST_LOG_DBG(name_, ...) \
    ::debug::Log::Dbgl(::debug::Log::FindOrCreate(name_), ##__VA_ARGS__)

// 一次性四色（调用即打，无名字，用法与 LOG_INF 一致；输出前带 [等级] 前缀）
// 每帧字节开销（TrueColor 38;2;R;G;B）：[inf]前缀(5) + 颜色(最长18 \x1b[38;2;246;167;83m) + 复位 \x1b[0m(4) + \r\n(2) = 最长29B；
// 帧上限 127B（kTxMaxLen 截断）→ 内容建议 ≤98B，超出截尾
#define DUST_LOG_INF(...) ::debug::Log::Inf("[inf] " __VA_ARGS__)
#define DUST_LOG_ERR(...) ::debug::Log::Err("[err] " __VA_ARGS__)
#define DUST_LOG_OK(...)  ::debug::Log::Ok("[ok] " __VA_ARGS__)
#define DUST_LOG_WRN(...) ::debug::Log::Wrn("[wrn] " __VA_ARGS__) 

#else

// ========== 关：空宏（调用点零开销，编译不报错）==========

#define DUST_LOG_DBG(...)
#define DUST_LOG_INF(...)
#define DUST_LOG_ERR(...)
#define DUST_LOG_OK(...)
#define DUST_LOG_WRN(...)

#endif

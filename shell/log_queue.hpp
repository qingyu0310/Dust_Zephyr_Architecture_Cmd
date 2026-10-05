/**
 * @file log_queue.hpp
 * @author qingyu
 * @brief DUST_LOG 发送队列 — 原始请求池 FIFO + 发送帧池三档优先级
 * @version 0.3
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <cstdint>
#include <cstring>
#include "log_policy.hpp"
#include "log_record.hpp"

namespace debug {

constexpr uint8_t  kNullIndex     = 255;		// 索引链表空值（255 = 无下一项）
constexpr uint16_t kTxFrameSize   = 128;		// 发送帧数据区大小（含 \0 保险）
constexpr uint16_t kTxMaxLen      = 127;		// 发送最长字节长度：超了截断
// 帧数：8 × 128B = 1KB 内存池（2026-08-10 由 4 扩到 8：命令响应连发突发缓冲，见 doc/log命令响应回退规划.md）
// 容量语义（实测 2026-08-06）：K 帧池 → 同一时间最多连发 K+1 条（1 DMA 中 + K 排队），
// 第 K+2 条起丢（事件永不挤）；稳态平均速率 < 波特率/10（921600 → 92160 B/s）且
// 帧池 ≥ 峰值突发条数时，可无限闭环（永不枯竭）
constexpr uint8_t  kTxPoolCount   = 8;
constexpr uint8_t  kMaxLogRecords = 8;			// LogRecord 原始请求队列深度（8×26B=208B）

/**
 * @brief 发送帧（三档共用）
 */
struct TxFrame
{
    char       data[kTxFrameSize];   			// 格式化后内容（含 ANSI 颜色），超 kTxMaxLen 截断
    uint16_t   len;                  			// 实际有效长度（≤127）
    TxPriority prio;                 			// 优先级档位
    uint16_t   epoch;                			// DBG 选择代数（出队比对，不一致丢弃）
    uint8_t    next;                 			// 帧索引链表（255=nullptr）
};

/**
 * @brief 日志原始请求池 + FIFO 队列
 *
 * 静态池 + uint8_t 索引链表，无堆无 STL。池满直接丢（不挤，ISR 安全）。
 */
class LogRecordQueue
{
public:
    /**
     * @brief 重置请求池和 FIFO 队列
     *
     * 清队头/队尾，重建空闲记录链（0→1→…→255）。
     */
    void ResetRecordQueue()
    {
        head = kNullIndex;
        tail = kNullIndex;
        free_head = 0;
        for (uint8_t i = 0; i < kMaxLogRecords; ++i)
        {
            records[i].next = (i + 1 < kMaxLogRecords) ? static_cast<uint8_t>(i + 1) : kNullIndex;
        }
    }

    /**
     * @brief 入队一条原始请求（FIFO 尾插）
     *
     * 池满直接返回 false（不挤帧，ISR 安全）。调用者须持有 irq_lock。
     *
     * @param fmt     格式串（静态字面量）
     * @param channel 日志通道（通道策略表索引）
     * @param color   颜色
     * @param prio    展开后的发送优先级
     * @param epoch   DBG 选择代数（非 DBG 传 0）
     * @param ansi    是否上 ANSI 颜色
     * @param args    参数槽
     * @param nargs   槽数
     * @return true 入队成功；false 池满丢弃
     */
    bool PushLogRecordLocked(const char* fmt, LogChannel channel, LogColor color, TxPriority prio, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs)
    {
        if (free_head == kNullIndex) return false;
        if (nargs > kMaxLogArgs) nargs = kMaxLogArgs;

        uint8_t i = free_head;
        free_head = records[i].next;

        records[i].fmt 		= fmt;
        records[i].channel 	= channel;
        records[i].color 	= color;
        records[i].prio 	= prio;
        records[i].epoch 	= epoch;
        records[i].ansi 	= ansi;
        records[i].nargs 	= nargs;
        for (uint8_t k = 0; k < nargs; ++k) records[i].args[k] = args[k];
        records[i].next = kNullIndex;

        if (head == kNullIndex) {
            head = tail = i;
        } else {
            records[tail].next = i;
            tail = i;
        }
        return true;
    }

    /**
     * @brief 队列弹头：Dbg 档记录选择代数不一致则作废回收（选择已切换）
     * @param active_epoch 当前 DBG 选择代数
     * @return 待处理记录指针；队列空返回 nullptr
     */
    LogRecord* PopNextRecordLocked(uint16_t active_epoch)
    {
        while (head != kNullIndex)
        {
            uint8_t i = head;
            head = records[i].next;
            if (head == kNullIndex) tail = kNullIndex;

            records[i].next = kNullIndex;
            if (records[i].prio == TxPriority::Dbg && records[i].epoch != active_epoch)
            {
                ReleaseRecordLocked(&records[i]);      // 作废记录：回收不展开
                continue;
            }
            return &records[i];
        }
        return nullptr;
    }

    /**
     * @brief 归还记录到空闲链（头插）
     *
     * 必须在展开完成后调用，避免出队后记录立即被 ISR 复用。
     *
     * @param r 待归还的记录
     */
    void ReleaseRecordLocked(LogRecord* r)
    {
        uint8_t i = static_cast<uint8_t>(r - records);   // 记录指针 → 记录池索引
        r->next = free_head;
        free_head = i;
    }

private:
    LogRecord records[kMaxLogRecords];
    uint8_t   free_head;
    uint8_t   head;
    uint8_t   tail;
};

/**
 * @brief 发送帧池 + 三档优先级队列
 *
 * 静态池 + uint8_t 索引链表，无堆无 STL。按优先级插队，同档保 FIFO。
 */
class TxFrameQueue
{
public:
    /**
     * @brief 重置发送帧池和发送队列
     */
    void ResetFrameQueue()
    {
        head = kNullIndex;
        tail = kNullIndex;
        free_head = 0;
        for (uint8_t i = 0; i < kTxPoolCount; ++i)
        {
            frames[i].len = 0;
            frames[i].next = (i + 1 < kTxPoolCount) ? static_cast<uint8_t>(i + 1) : kNullIndex;
        }
    }

    /**
     * @brief 锁内入队仲裁：按优先级插队
     *
     * 超长截断到 kTxMaxLen；池满按优先级策略挤最低档帧（Event 永不挤）；
     * 并发保护：调用者已持有 irq_lock。
     *
     * @param data       发送内容（已格式化，可能含 ANSI 颜色）
     * @param len        数据长度
     * @param prio       优先级档位
     * @param epoch      DBG 选择代数（非 DBG 传 0）
     * @param allow_evict 是否允许挤帧（线程上下文 true；ISR false，池满直接丢）
     * @return true 入队成功；false 池满丢弃
     */
    bool PushFrameLocked(const char* data, int len, TxPriority prio, uint16_t epoch, bool allow_evict)
    {
        if (len > kTxMaxLen) len = kTxMaxLen; 				// 超长截断

        TxFrame* f = AllocateFreeFrameLocked();            	// 从空闲池取帧
        if (f == nullptr)
        {
            if (!allow_evict) return false;  				// ISR：池满直接丢，不 Evict 遍历链表
            f = EvictLowestPriorityFrameLocked();         	// 线程上下文池满：挤最低优先级帧
            if (f == nullptr) return false;   				// 池满且无可挤帧（极端）→ 丢弃
        }

        std::memcpy(f->data, data, static_cast<size_t>(len));
        f->len = static_cast<uint16_t>(len);
        f->prio = prio;
        f->epoch = epoch;

        InsertFrameByPriorityLocked(f);                  	// 按档位插队
        return true;
    }

    /**
     * @brief 队列弹头：Dbg 档帧选择代数不一致则作废回收（选择已切换）
     * @param active_epoch 当前 DBG 选择代数
     * @return 待发送帧指针；队列空返回 nullptr
     */
    TxFrame* PopNextFrameLocked(uint16_t active_epoch)
    {
        while (head != kNullIndex)
        {
            uint8_t i = head;
            head = frames[i].next;
            if (head == kNullIndex) tail = kNullIndex;

            frames[i].next = kNullIndex;
            if (frames[i].prio == TxPriority::Dbg && frames[i].epoch != active_epoch)
            {
                ReleaseFrameLocked(&frames[i]);   		// 作废帧：回收不发送
                continue;
            }
            return &frames[i];
        }
        return nullptr;
    }

    /**
     * @brief 归还帧到空闲链（头插）
     * @param f 待归还的帧
     */
    void ReleaseFrameLocked(TxFrame* f)
    {
        uint8_t i = static_cast<uint8_t>(f - frames);   	// 帧指针 → 帧池索引
        f->next = free_head;
        free_head = i;
    }

private:
	TxFrame frames[kTxPoolCount];
    uint8_t free_head;
    uint8_t head;
    uint8_t tail;

    /**
     * @brief 从空闲帧链取一帧（摘链，next 置空）
     * @return 空闲帧指针；空闲链空返回 nullptr
     */
    TxFrame* AllocateFreeFrameLocked()
    {
        if (free_head == kNullIndex) return nullptr;

        uint8_t i = free_head;
        free_head = frames[i].next;
        frames[i].next = kNullIndex;
        return &frames[i];
    }

    /**
     * @brief 按优先级插队入队（同档保 FIFO：插在同档末尾）
     * @param f 待入队的帧（data/len/prio 已填好）
     */
    void InsertFrameByPriorityLocked(TxFrame* f)
    {
        const uint8_t idx = static_cast<uint8_t>(f - frames);   	// 帧指针 → 帧池索引

        f->next = kNullIndex;
        if (head == kNullIndex) { head = tail = idx; return; }

        uint8_t* pp = &head;              							// 找插入点：第一个 prio > f->prio 的帧之前
        while (*pp != kNullIndex && frames[*pp].prio <= f->prio)
            pp = &frames[*pp].next;

        f->next = *pp;
        *pp = idx;
        if (f->next == kNullIndex) tail = idx;
    }

    /**
     * @brief 池满挤帧：按优先级策略取可被挤出的最低档帧（Event 永不挤）
     * @return 被挤出的帧指针；无可挤帧（极端）返回 nullptr
     */
    TxFrame* EvictLowestPriorityFrameLocked()
    {
        if (head == kNullIndex) return nullptr;

        uint8_t prev = kNullIndex, cur = head, victim = kNullIndex, victim_prev = kNullIndex;
        while (cur != kNullIndex)
        {
            if (GetTxPriorityPolicy(frames[cur].prio).may_be_evicted)   // 只考虑策略允许被挤出的帧
            {
                if (victim == kNullIndex || frames[cur].prio > frames[victim].prio)
                { victim = cur; victim_prev = prev; }
            }
            prev = cur;
            cur = frames[cur].next;
        }
        if (victim == kNullIndex) return nullptr;     // 无帧可挤（极端）→ 丢弃

        if (victim_prev == kNullIndex) {
            head = frames[victim].next;
        } else {
            frames[victim_prev].next = frames[victim].next;
        }

        if (frames[victim].next == kNullIndex) tail = victim_prev;

        frames[victim].next = kNullIndex;
        return &frames[victim];
    }
};

} // namespace debug

#endif

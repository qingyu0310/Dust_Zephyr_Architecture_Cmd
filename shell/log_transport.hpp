/**
 * @file log_transport.hpp
 * @author qingyu
 * @brief DUST_LOG 发送泵 — Stream 绑定 / DMA 空闲续发 / 发送完成回调
 * @version 0.1
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <zephyr/kernel.h>
#include "stream.hpp"
#include "log_queue.hpp"

namespace debug {

/**
 * @brief 发送泵
 *
 * 持有发送通道与「帧在 DMA 中」标志，负责提交帧、发送完成后清标志并唤醒 shell 续发。
 * 只消费已格式化好的 TxFrame，不感知优先级仲裁、参数快照、格式化、DBG 选择。
 */
class LogTransport
{
public:
    void    Reset() { sending_ = false; }              	// 清「发送中」标志（初始化）
    void    Bind(Stream* s) { stream_ = s; }           	// 绑定发送通道
    bool    Sending() const { return sending_; }       	// 当前是否有帧在 DMA 中
    Stream* Channel() const { return stream_; }        	// 发送通道（唤醒 shell 泵用）

    /**
     * @brief DMA 空闲则从发送队列取一帧提交发送
     *
     * 调用者须持有 irq_lock。帧内容已移交 UartDma，提交后立即归还空闲池。
     *
     * @param queue        发送帧队列
     * @param active_epoch 当前 DBG 选择代数（作废帧跳过回收）
     * @return true 提交了一帧；false DMA 忙或队列无可发帧
     */
    bool PumpTxQueue(TxFrameQueue& queue, uint16_t active_epoch)
    {
        if (sending_) return false;

        TxFrame* f = queue.PopNextFrameLocked(active_epoch);
        if (f == nullptr) return false;

        sending_ = true;
        if (stream_ == nullptr || !stream_->Send(reinterpret_cast<const uint8_t*>(f->data), f->len))
        {
            sending_ = false;                           // 发送失败：不置发送中
        }
        queue.ReleaseFrameLocked(f);                    // 归还空闲池：帧内容已移交 UartDma，立即复用
        return true;
    }

    /**
     * @brief TX_DONE 回调（UartDma tx_cb）：清标志并唤醒 shell 线程续发
     */
    void HandleTxDone()
    {
        unsigned key = irq_lock();
        sending_ = false;
        irq_unlock(key);
        if (stream_ != nullptr) k_sem_give(&stream_->sem_);
    }

private:
    Stream* stream_  = nullptr;  	// 发送通道（Bind 绑定）
    bool    sending_ = false;    	// 当前是否有帧在 DMA 中
};

} // namespace debug

#endif

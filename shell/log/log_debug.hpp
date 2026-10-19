/**
 * @file log_debug.hpp
 * @author qingyu
 * @brief DUST_LOG DBG 源注册表 — 名字注册/选中/遍历
 * @version 0.4
 * @date 2026-10-05
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_DUST_CMD_SHELL_LOG

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace debug {

constexpr uint8_t kMaxLogEntries = 64;			// DBG 条目数上限

/**
 * @brief DBG 日志条目（只服务 DBG）
 */
struct LogEntry
{
    const char* name;   						// DBG 名字（log on 用，运行时 FindOrCreate 创建）
    bool        vofa;   						// 输出模式：false=上色（默认）/ true=VOFA+ 纯文本
};

/**
 * @brief DBG 源注册表
 *
 * 只管名字注册与选中状态（同一时间只选中一条）、提供遍历；
 * 不感知发送队列、优先级、格式化。
 */
class DebugSourceRegistry
{
public:
    /**
     * @brief 清空选中状态、条目计数与选择代数
     */
    void Reset()
    {
        active_ = nullptr;
        count_  = 0;
        epoch_  = 0;
    }

    /**
     * @brief 按名字查 DBG 条目，首见创建（同名复用同一条目）
     * @param name DBG 名字（字符串字面量）
     * @return 条目指针；池满无法创建返回 nullptr
     */
    LogEntry* FindOrCreate(const char* name)
    {
        for (uint8_t i = 0; i < count_; ++i)
        {
            if (std::strcmp(entries_[i].name, name) == 0) return &entries_[i];
        }

        if (count_ >= kMaxLogEntries) return nullptr;   // 池满：无法创建，返回 nullptr

        LogEntry* e = &entries_[count_++];
        e->name = name;
        e->vofa = false;                                // 新条目默认上色
        return e;
    }

    /**
     * @brief 选中 DBG 条目
     *
     * 只选中已存在的条目（代码里 DUST_LOG_DBG 注册过的名字），
     * 不存在返回 false，不创建幽灵条目。
     *
     * @param name DBG 名字
     * @return true 选中成功；false 条目不存在
     */
    bool Select(const char* name)
    {
        for (uint8_t i = 0; i < count_; ++i)
        {
            if (std::strcmp(entries_[i].name, name) == 0)
            {
                active_ = &entries_[i];
                ++epoch_;                                 // 选择代数递增：作废在途旧 DBG 记录/帧
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 设置某 DBG 条目的输出模式
     *
     * 只设置已存在的条目（代码里 DUST_LOG_DBG 注册过的名字），
     * 不存在返回 false，不创建幽灵条目。
     *
     * @param name DBG 名字
     * @param vofa true=VOFA+ 纯文本；false=上色
     * @return true 设置成功；false 条目不存在
     */
    bool SetVofa(const char* name, bool vofa)
    {
        for (uint8_t i = 0; i < count_; ++i)
        {
            if (std::strcmp(entries_[i].name, name) == 0)
            {
                entries_[i].vofa = vofa;
                return true;
            }
        }
        return false;                          // 不存在：不创建幽灵条目
    }

    void Deselect()                { active_ = nullptr; ++epoch_; }   // 取消选中（代数递增作废在途）
    const LogEntry* Active() const { return active_; }  	// 当前选中条目（nullptr=无）
    uint16_t ActiveEpoch() const   { return epoch_; }   	// 当前选择代数（DBG 记录/帧携带比对）

    /**
     * @brief 遍历：数组首个条目
     * @return 首个条目指针；空表返回 nullptr
     */
    LogEntry* First()
    {
        return (count_ > 0) ? &entries_[0] : nullptr;
    }

    /**
     * @brief 遍历：数组下一个条目
     * @param e 当前条目
     * @return 下一条目指针；已到尾部返回 nullptr
     */
    LogEntry* Next(const LogEntry* e)
    {
        ptrdiff_t idx = e - entries_;
        return (idx + 1 < count_) ? &entries_[idx + 1] : nullptr;
    }

private:
    LogEntry  entries_[kMaxLogEntries] {};    	// DBG 条目静态池
    LogEntry* active_ = nullptr;              	// 当前选中条目（同一时间只打一条）
    uint8_t   count_  = 0;                    	// 已注册条目数
    uint16_t  epoch_  = 0;                    	// 选择代数（Select/Deselect 递增，作废在途旧 DBG）
};

} // namespace debug

#endif

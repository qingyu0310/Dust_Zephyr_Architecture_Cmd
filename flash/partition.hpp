/**
 * @file partition.hpp
 * @author qingyu
 * @brief 分区布局 — 各模块专用存储区定义
 * @version 0.1
 * @date 2026-07-26
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <cstdint>

namespace flash {

/**
 * @brief 分区描述
 */
struct Partition
{
    const char *id;
    uint32_t    offset;
    uint32_t    size;
};

/**
 * @brief 4KB — IMU 校准参数（gyro/accel offset/scale）
 */
inline constexpr Partition kPartCalib {
    "ImuCalib",
    0x000000,
    0x001000
};

/**
 * @brief 1MB — 日志记录
 */
inline constexpr Partition kPartLog {
    "DebugLog",
    0x001000,
    0x100000
};

/**
 * @brief ~15MB — 保留区
 */
inline constexpr Partition kPartReserve {
    "Reserve",
    0x101000,
    0xEFF000
};

inline constexpr Partition kPartitions[] {
    kPartCalib,
    kPartLog,
    kPartReserve,
};

inline constexpr uint32_t kPartitionCount = sizeof(kPartitions) / sizeof(kPartitions[0]);

} // namespace flash

/**
 * @file w25q128.cpp
 * @author qingyu
 * @brief W25Q128 SPI NOR Flash — 同步阻塞驱动
 * @version 0.1
 * @date 2026-07-26
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "w25q128.hpp"
#include "Init_entry.hpp"
#include <cstdlib>
#include <cstring>
#include <zephyr/kernel.h>

#pragma message "Compiling Cmd/W25Q128"

namespace w25q128 {

// 命令常量
namespace {

constexpr uint8_t kCmdWriteEnable         = 0x06;
constexpr uint8_t kCmdWriteDisable        = 0x04;
constexpr uint8_t kCmdReadStatus          = 0x05;
constexpr uint8_t kCmdReadStatus2         = 0x35;
constexpr uint8_t kCmdReadStatus3         = 0x15;
constexpr uint8_t kCmdReadData            = 0x03;
constexpr uint8_t kCmdFastRead            = 0x0B;
constexpr uint8_t kCmdPageProgram         = 0x02;
constexpr uint8_t kCmdSectorErase         = 0x20;
constexpr uint8_t kCmdBlockErase32K       = 0x52;
constexpr uint8_t kCmdBlockErase64K       = 0xD8;
constexpr uint8_t kCmdChipErase           = 0xC7;
constexpr uint8_t kCmdPowerDown           = 0xB9;
constexpr uint8_t kCmdReleasePowerDown    = 0xAB;
constexpr uint8_t kCmdJedecId             = 0x9F;
constexpr uint8_t kCmdReadUniqueId        = 0x4B;
constexpr uint8_t kCmdEnableReset         = 0x66;
constexpr uint8_t kCmdResetDevice         = 0x99;

constexpr uint8_t kStatusBusy             = 0x01;

} // anonymous namespace

/**
 * @brief 绑定 SPI 实例
 *
 * 从设备树 alias（flash-spi）读取 SPI 总线配置，
 * 调用 Spi::Init 完成底层初始化。
 *
 * @return true 初始化成功
 */
bool W25Q128::Init()
{
    static const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_ALIAS(flash_spi), SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
    return spi_.Init(spec);
}

/**
 * @brief 读取状态寄存器 1
 *
 * @return 状态寄存器 1 的值，失败返回 0xFF
 */
uint8_t W25Q128::ReadStatus()
{
    uint8_t tx[2] = { kCmdReadStatus, 0x00 };
    uint8_t rx[2] = { 0 };

    if (!spi_.Transceive(tx, rx, 2)) return 0xFF;

    return rx[1];
}

/**
 * @brief 读取状态寄存器 2
 *
 * @return 状态寄存器 2 的值，失败返回 0xFF
 */
uint8_t W25Q128::ReadStatus2()
{
    uint8_t tx[2] = { kCmdReadStatus2, 0x00 };
    uint8_t rx[2] = { 0 };

    if (!spi_.Transceive(tx, rx, 2)) return 0xFF;

    return rx[1];
}

/**
 * @brief 读取状态寄存器 3
 *
 * @return 状态寄存器 3 的值，失败返回 0xFF
 */
uint8_t W25Q128::ReadStatus3()
{
    uint8_t tx[2] = { kCmdReadStatus3, 0x00 };
    uint8_t rx[2] = { 0 };

    if (!spi_.Transceive(tx, rx, 2)) return 0xFF;

    return rx[1];
}

/**
 * @brief 等待 BUSY 位清除或超时
 *
 * @param timeout_ms 超时毫秒
 * @return true 操作完成, false 超时
 */
bool W25Q128::WaitWhileBusy(uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0; elapsed < timeout_ms; elapsed++)
    {
        if ((ReadStatus() & kStatusBusy) == 0) return true;

        k_msleep(1);
    }

    return false;
}

/**
 * @brief 发送写使能命令（WEL 置位）
 *
 * 页写和擦除前必须先调用此命令。
 */
bool W25Q128::WriteEnable()
{
    return WriteCmd(kCmdWriteEnable);
}

/**
 * @brief 发送写禁用命令
 */
bool W25Q128::WriteDisable()
{
    return WriteCmd(kCmdWriteDisable);
}

/**
 * @brief 发送单字节命令（无地址无数据）
 *
 * 用于写使能、片擦除等单字节指令。
 *
 * @param cmd 命令字节
 * @return true 发送成功
 */
bool W25Q128::WriteCmd(uint8_t cmd)
{
    return spi_.Send(&cmd, 1);
}

/**
 * @brief 发送命令 + 3 字节地址
 *
 * 用于页写、扇区擦除等需要地址的命令。
 *
 * @param cmd  命令字节
 * @param addr 24 位地址
 * @return true 发送成功
 */
bool W25Q128::WriteCmdAddr(uint8_t cmd, uint32_t addr)
{
    uint8_t buf[4] =
    {
        cmd,
        static_cast<uint8_t>((addr >> 16) & 0xFF),
        static_cast<uint8_t>((addr >>  8) & 0xFF),
        static_cast<uint8_t>( addr        & 0xFF),
    };

    return spi_.Send(buf, 4);
}

/**
 * @brief 读取 JEDEC ID
 *
 * @return 24 位 JEDEC ID（高位 = 制造商, 中间 = 类型, 低位 = 容量），
 *         失败返回 0xFFFFFF
 */
uint32_t W25Q128::ReadJedecId()
{
    uint8_t tx[4] = { kCmdJedecId, 0x00, 0x00, 0x00 };
    uint8_t rx[4] = { 0 };

    if (!spi_.Transceive(tx, rx, 4)) return 0xFFFFFF;

    return (static_cast<uint32_t>(rx[1]) << 16)
         | (static_cast<uint32_t>(rx[2]) <<  8)
         | (static_cast<uint32_t>(rx[3]));
}

/**
 * @brief 读取芯片唯一 ID（64 位）
 *
 * @param[out] id 8 字节 ID 缓冲区
 * @return true 读取成功
 */
bool W25Q128::ReadUniqueId(uint8_t id[8])
{
    // 命令 + 4 字节虚拟填充后读 8 字节
    uint8_t tx[13] = { kCmdReadUniqueId, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00 };
    uint8_t rx[13] = { 0 };

    if (!spi_.Transceive(tx, rx, 13)) return false;

    std::memcpy(id, rx + 5, 8);

    return true;
}

/**
 * @brief 从指定地址读取数据
 *
 * @param addr 起始地址（24 位，0 ~ 16M-1）
 * @param data 接收缓冲区
 * @param len  读取字节数
 * @return true 读取成功
 */
bool W25Q128::Read(uint32_t addr, void *data, uint32_t len)
{
    if (data == nullptr || len == 0) return false;

    // 单次 Transceive，heap 分配，精确匹配长度
    const uint32_t total = 4 + len;

    auto tx = (uint8_t*)malloc(total);
    auto rx = (uint8_t*)malloc(total);
    if (tx == nullptr || rx == nullptr) {
        free(tx);
        free(rx);
        return false;
    }

    tx[0] = kCmdReadData;
    tx[1] = static_cast<uint8_t>((addr >> 16) & 0xFF);
    tx[2] = static_cast<uint8_t>((addr >>  8) & 0xFF);
    tx[3] = static_cast<uint8_t>( addr        & 0xFF);
    memset(tx + 4, 0xFF, total - 4);

    bool ok = spi_.Transceive(tx, rx, total);
    if (ok) memcpy(data, rx + 4, len);

    free(tx);
    free(rx);
    return ok;
}

/**
 * @brief 快速读（带虚拟字节，适合较高 SPI 频率）
 *
 * @param addr 起始地址
 * @param data 接收缓冲区
 * @param len  读取字节数
 * @return true 读取成功
 */
bool W25Q128::FastRead(uint32_t addr, void *data, uint32_t len)
{
    if (data == nullptr || len == 0) return false;

    const uint32_t total = 5 + len;

    auto tx = (uint8_t*)malloc(total);
    auto rx = (uint8_t*)malloc(total);
    if (tx == nullptr || rx == nullptr) {
        free(tx);
        free(rx);
        return false;
    }

    tx[0] = kCmdFastRead;
    tx[1] = static_cast<uint8_t>((addr >> 16) & 0xFF);
    tx[2] = static_cast<uint8_t>((addr >>  8) & 0xFF);
    tx[3] = static_cast<uint8_t>( addr        & 0xFF);
    tx[4] = 0xFF;  // dummy
    memset(tx + 5, 0xFF, total - 5);

    bool ok = spi_.Transceive(tx, rx, total);
    if (ok) memcpy(data, rx + 5, len);

    free(tx);
    free(rx);
    return ok;
}

/**
 * @brief 页写（最大 256 字节）
 *
 * 写入目标地址所在的页。如果数据跨越页边界，行为由 W25Q128 内部
 * 决定（折回页首）。跨页写入请使用 Write() 或分多次调用。
 *
 * @param addr 页内起始地址
 * @param data 待写入数据
 * @param len  写入字节数（≤ 256）
 * @return true 写入成功
 */
bool W25Q128::PageWrite(uint32_t addr, const void *data, uint32_t len)
{
    if (data == nullptr || len == 0 || len > kPageSize) return false;

    if (!WriteEnable()) return false;

    uint8_t hdr[4];

    hdr[0] = kCmdPageProgram;
    hdr[1] = static_cast<uint8_t>((addr >> 16) & 0xFF);
    hdr[2] = static_cast<uint8_t>((addr >>  8) & 0xFF);
    hdr[3] = static_cast<uint8_t>( addr        & 0xFF);

    // 合并发送包头 + 数据
    uint8_t tmp[260];

    std::memcpy(tmp, hdr, 4);
    std::memcpy(tmp + 4, data, len);

    if (!spi_.Send(tmp, 4 + len)) return false;

    return WaitWhileBusy();
}

/**
 * @brief 写数据（自动处理跨页和扇区擦除）
 *
 * 按页边界自动拆分写入。调用方需确保目标扇区已被擦除。
 * 整片擦除或扇区擦除一次后，可连续多次调用 Write 写入。
 *
 * @param addr 起始地址
 * @param data 待写入数据
 * @param len  写入字节数
 * @return true 写入成功
 */
bool W25Q128::Write(uint32_t addr, const void *data, uint32_t len)
{
    auto *bytes = static_cast<const uint8_t*>(data);

    while (len > 0)
    {
        // 当前页内剩余空间
        uint32_t page_remain = kPageSize - (addr % kPageSize);
        uint32_t chunk = len;

        if (chunk > page_remain) chunk = page_remain;

        if (!PageWrite(addr, bytes, chunk)) return false;

        addr  += chunk;
        bytes += chunk;
        len   -= chunk;
    }

    return true;
}

/**
 * @brief 扇区擦除（4KB）
 *
 * @param addr 目标扇区内任意地址
 * @return true 擦除成功
 */
bool W25Q128::SectorErase(uint32_t addr)
{
    if (!WriteEnable()) return false;
    if (!WriteCmdAddr(kCmdSectorErase, addr)) return false;

    return WaitWhileBusy(5000);
}

/**
 * @brief 块擦除（32KB）
 *
 * @param addr 目标块内任意地址
 * @return true 擦除成功
 */
bool W25Q128::BlockErase32K(uint32_t addr)
{
    if (!WriteEnable()) return false;
    if (!WriteCmdAddr(kCmdBlockErase32K, addr)) return false;

    return WaitWhileBusy(5000);
}

/**
 * @brief 块擦除（64KB）
 *
 * @param addr 目标块内任意地址
 * @return true 擦除成功
 */
bool W25Q128::BlockErase64K(uint32_t addr)
{
    if (!WriteEnable()) return false;
    if (!WriteCmdAddr(kCmdBlockErase64K, addr)) return false;

    return WaitWhileBusy(60000);
}

/**
 * @brief 整片擦除（全片置 0xFF）
 *
 * @return true 擦除成功
 */
bool W25Q128::ChipErase()
{
    if (!WriteEnable()) return false;
    if (!WriteCmd(kCmdChipErase)) return false;

    return WaitWhileBusy(120000);
}

/**
 * @brief 进入掉电模式
 *
 * 后续需要 ReleasePowerDown 唤醒后才能操作。
 */
bool W25Q128::PowerDown()
{
    return WriteCmd(kCmdPowerDown);
}

/**
 * @brief 退出掉电模式
 */
bool W25Q128::ReleasePowerDown()
{
    // 发送释放命令后需要等待 tRES1（~30us）
    if (!WriteCmd(kCmdReleasePowerDown)) return false;

    k_busy_wait(50);

    return true;
}

/**
 * @brief 复位器件
 *
 * 先使能复位，再执行复位。
 */
bool W25Q128::ResetDevice()
{
    if (!WriteCmd(kCmdEnableReset)) return false;

    return WriteCmd(kCmdResetDevice);
}

/**
 * @brief 等待 BUSY 清除或超时
 */
static bool flash_init()
{
    return w25q128::Instance().Init();
}

REGISTER_INIT(flash_init, PreInit, Mid, HaltOnFail, "w25q128");

} // namespace w25q128

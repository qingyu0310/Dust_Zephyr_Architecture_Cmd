/**
 * @file w25q128.hpp
 * @author qingyu
 * @brief W25Q128 SPI NOR Flash 驱动 — 读/写/擦除
 * @version 0.1
 * @date 2026-07-26
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#ifdef CONFIG_CMD_W25Q128

#include <cstdint>
#include "spi.hpp"

namespace w25q128 {

/**
 * @brief W25Q128 SPI NOR Flash 控制器
 *
 * 封装 W25Q128 的基本操作：读、页写、跨页写、扇区/块/片擦除、
 * JEDEC ID 读取、掉电/唤醒、复位等。
 * 所有操作同步阻塞。
 */
class W25Q128 final
{
public:
    W25Q128() = default;

    bool Init();

    uint32_t ReadJedecId();
    bool     ReadUniqueId(uint8_t id[8]);

    bool     Read(uint32_t addr, void *data, uint32_t len);
    bool     FastRead(uint32_t addr, void *data, uint32_t len);
    bool     PageWrite(uint32_t addr, const void *data, uint32_t len);
    bool     Write(uint32_t addr, const void *data, uint32_t len);

    bool     SectorErase(uint32_t addr);
    bool     BlockErase32K(uint32_t addr);
    bool     BlockErase64K(uint32_t addr);
    bool     ChipErase();

    bool     PowerDown();
    bool     ReleasePowerDown();
    bool     ResetDevice();

    uint8_t  ReadStatus();
    uint8_t  ReadStatus2();
    uint8_t  ReadStatus3();

private:
    static constexpr uint32_t kPageSize   = 256;
    static constexpr uint32_t kSectorSize = 4096;

    bool    WriteEnable();
    bool    WriteDisable();
    bool    WriteCmd(uint8_t cmd);
    bool    WriteCmdAddr(uint8_t cmd, uint32_t addr);
    bool    WaitWhileBusy(uint32_t timeout_ms = 5000);

    Spi     spi_ {};
};

inline W25Q128& Instance()
{
    static W25Q128 flash;
    return flash;
}

} // namespace w25q128

#define EXEC_FLASH_READ(addr, data, len)        w25q128::Instance().Read(addr, data, len)
#define EXEC_FLASH_WRITE(addr, data, len)       (w25q128::Instance().SectorErase(addr) && w25q128::Instance().Write(addr, data, len))
#define EXEC_FLASH_SECTOR_ERASE(addr)           w25q128::Instance().SectorErase(addr)
#define EXEC_FLASH_CHIP_ERASE()                 w25q128::Instance().ChipErase()
#define EXEC_FLASH_POWER_DOWN()                 w25q128::Instance().PowerDown()
#define EXEC_FLASH_WAKEUP()                     w25q128::Instance().ReleasePowerDown()
#else

#define EXEC_FLASH_READ(addr, data, len)        ((void)(addr), (void)(data), (void)(len), false)
#define EXEC_FLASH_WRITE(addr, data, len)       ((void)(addr), (void)(data), (void)(len), false)
#define EXEC_FLASH_SECTOR_ERASE(addr)           ((void)(addr), false)
#define EXEC_FLASH_CHIP_ERASE()                 false
#define EXEC_FLASH_POWER_DOWN()                 false
#define EXEC_FLASH_WAKEUP()                     false

#endif

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 项目初始化项链接段 — 收集 REGISTER_INIT 声明的 InitEntry
 * CAN 接收分发链接段 — 收集 CAN_RX_HANDLER 声明的 CanRxEntry
 */

SECTION_PROLOGUE(.user_init,,)
{
    __user_init_start = .;
    KEEP(*(.user_init))
    __user_init_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

SECTION_PROLOGUE(.can_rx1,,)
{
    __can_rx1_start = .;
    KEEP(*(.can_rx1))
    __can_rx1_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

SECTION_PROLOGUE(.can_rx2,,)
{
    __can_rx2_start = .;
    KEEP(*(.can_rx2))
    __can_rx2_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

SECTION_PROLOGUE(.can_rx3,,)
{
    __can_rx3_start = .;
    KEEP(*(.can_rx3))
    __can_rx3_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

SECTION_PROLOGUE(.remote,,)
{
    __remote_start = .;
    KEEP(*(.remote))
    __remote_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

SECTION_PROLOGUE(.imu,,)
{
    __imu_start = .;
    KEEP(*(.imu))
    __imu_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 启动期耗时槽 — 收集 REGISTER_INIT / REGISTER_THREAD 为每项生成的 BootTimingSlot
 *
 * 槽在运行期由启动器写入（ns / ok），因此放在 RAMABLE_REGION，
 * 初值从 ROMABLE_REGION 拷贝，与普通 .data 同机制。
 * 槽数量由链接器按实际注册项决定，无需固定容量。
 */

SECTION_DATA_PROLOGUE(.boot_timing,,)
{
    __boot_timing_start = .;
    KEEP(*(.boot_timing))
    __boot_timing_end = .;
} GROUP_DATA_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)

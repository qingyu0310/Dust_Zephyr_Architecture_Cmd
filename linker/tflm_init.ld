/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 项目初始化项链接段 — 收集 REGISTER_INIT 声明的 InitEntry
 */

SECTION_PROLOGUE(.user_init,,)
{
    __user_init_start = .;
    KEEP(*(.user_init))
    __user_init_end = .;
} GROUP_LINK_IN(ROMABLE_REGION)

/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Host stand-in for the SDK's "pico/bootrom.h": the real bootrom constants, plus
// rom_get_partition_table_info(), implemented by flash_sim.c

#ifndef _HOST_STUB_PICO_BOOTROM_H
#define _HOST_STUB_PICO_BOOTROM_H

#include "pico.h"

#define NO_PICO_PLATFORM 1

// The real headers: picobin.h also provides _u(), bootrom_constants.h PT_INFO_* and FFS_DATA_PARTITION_ID
#include "boot/picobin.h"
#include "boot/bootrom_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

int rom_get_partition_table_info(uint32_t *out_buffer, uint32_t out_buffer_word_size, uint32_t partition_and_flags);

#ifdef __cplusplus
}
#endif

#endif

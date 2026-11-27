/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Host stand-in for the SDK's "hardware/flash.h", implemented by flash_sim.c

#ifndef _HOST_STUB_HARDWARE_FLASH_H
#define _HOST_STUB_HARDWARE_FLASH_H

#include "pico.h"

#ifdef __cplusplus
extern "C" {
#endif

// W25Q16JV (and all Pico boards): 256 byte program page, 4 KB erase sector
#define FLASH_PAGE_SIZE   (1u << 8)
#define FLASH_SECTOR_SIZE (1u << 12)

void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Host stand-in for the SDK's "pico.h": just enough for ffs.c and ffs_internal.c
// to build unmodified for the pico_ffs power-fail harness

#ifndef _HOST_STUB_PICO_H
#define _HOST_STUB_PICO_H

// The real SDK error codes
#include "pico/error.h"

// static_assert, used by boot/bootrom_constants.h
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ffs_internal.h uses this to tell an SDK build from an ffsgen build
#define PICO_SDK_VERSION_MAJOR 2

typedef unsigned int uint;

#ifndef count_of
#define count_of(a) (sizeof(a)/sizeof((a)[0]))
#endif

// W25Q16JV: 16 Mbit = 2 MB
#define PICO_FLASH_SIZE_BYTES  (2u * 1024u * 1024u)

// Real RP2xxx memory map values (hardware/regs/addressmap.h)
#define XIP_BASE                                0x10000000u
#define XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE    0x14000000u

// The harness sets the ffs area at run time; on RP2040 the build points
// FFS_RP2040_FLASH_{START,END}_OFFSET at these variables
extern uint32_t sim_ffs_start_offset;
extern uint32_t sim_ffs_end_offset;

#ifdef __cplusplus
}
#endif

#endif

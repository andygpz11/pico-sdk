/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Host stand-in for the SDK's "pico/flash.h", implemented by flash_sim.c

#ifndef _HOST_STUB_PICO_FLASH_H
#define _HOST_STUB_PICO_FLASH_H

#include "pico.h"

#ifdef __cplusplus
extern "C" {
#endif

int flash_safe_execute(void (*func)(void *), void *param, uint32_t enter_exit_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

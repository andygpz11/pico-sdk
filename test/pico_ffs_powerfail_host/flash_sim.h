/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Simulated NOR flash, mapped read-only where ffs.c reads it, with power-fail and
// flash-error injection at a chosen op (see README.md)

#ifndef _FLASH_SIM_H
#define _FLASH_SIM_H

#include "pico.h"
#include "hardware/flash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_FLASH_SIZE      PICO_FLASH_SIZE_BYTES

#ifdef PICO_RP2040
#define SIM_FLASH_READ_BASE XIP_BASE
#define SIM_PORT_NAME       "RP2040"
#else
#define SIM_FLASH_READ_BASE XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#define SIM_PORT_NAME       "RP2350"
#endif

#define SIM_MAX_OPS         20000
#define SIM_BT_DEPTH        20
#define SIM_NO_FAIL         UINT32_MAX
#define SIM_MAX_WEAK        64

// Child process exit codes
#define SIM_EXIT_OK         0
#define SIM_EXIT_POWERFAIL  42
#define SIM_EXIT_VIOLATION  43

typedef enum {
    OP_PROGRAM = 1,
    OP_ERASE   = 2,
} sim_op_type_t;

// PF_*: power fails during the op. FE_*: flash_safe_execute() reports an error and the code keeps running.
// SF_*: no cut and no error, but the program silently goes wrong.
typedef enum {
    PF_CLEAN       = 0, ///< the op does not happen at all
    PF_TORN_PREFIX = 1, ///< program: first `param` changing bytes land; erase: first param/8 of sector erased
    PF_TORN_RANDOM = 2, ///< a random subset (seed `param`) of the bit changes land
    PF_MARGINAL    = 3, ///< the op completes, but 1-4 of its bit changes (seed `param`) are left weak
    FE_BEFORE      = 4, ///< safe zone entry fails: the op does not happen
    FE_AFTER       = 5, ///< safe zone exit fails: the op has completed
    FE_TWICE       = 6, ///< as FE_BEFORE, then again at the `param`th flash call after
    SF_MISSED_BIT  = 7, ///< no power cut or error: the program reports success, but one bit it should
                        ///< clear (seed `param`) stays 1
    SF_EXTRA_ZERO  = 8, ///< no power cut or error: the program reports success, but one bit it should
                        ///< leave 1 (seed `param`) reads 0 until the sector is erased
} pf_mode_t;

#define IS_SILENT_MODE(_m) ((_m) == SF_MISSED_BIT || (_m) == SF_EXTRA_ZERO)

#define IS_FLASH_ERROR_MODE(_m) ((_m) >= FE_BEFORE && (_m) <= FE_TWICE)

// What the simulated flash_safe_execute() returns when an error is injected
#define SIM_FLASH_ERROR     PICO_ERROR_TIMEOUT

typedef struct {
    uint8_t  type;          ///< sim_op_type_t
    uint8_t  boot;
    uint16_t step;          ///< workload step in progress (0 = ffs_initialise)
    uint32_t offset;
    uint32_t changed;       ///< program: bytes that change; erase: bytes not already 0xff
    bool     first_in_call; ///< first op of its flash_safe_execute() call
    uint32_t n_bt;
    void    *bt[SIM_BT_DEPTH];
} sim_op_t;

// Lives in shared memory so it survives the (simulated power-off) exit of a child
typedef struct {
    // Set by the parent before each boot
    uint32_t fail_at;           ///< op index within this boot that the power cut, flash error or silent fault hits
    uint8_t  fail_mode;         ///< pf_mode_t
    uint32_t fail_param;
    bool     log_ops;

    // Maintained by the child
    uint8_t  boot;
    uint16_t step;
    uint32_t op_count;
    uint32_t n_logged;
    bool     power_failed;
    uint32_t call_start_op;     ///< op_count at the start of the current flash_safe_execute()
    uint32_t errors_injected;   ///< flash errors injected in this boot
    uint32_t silent_faults;     ///< SF_MISSED_BIT or SF_EXTRA_ZERO faults injected in this boot
    uint32_t second_error_in;   ///< FE_TWICE: flash calls until the second error (0 = none due)
    uint32_t overwrite_count;   ///< attempts to program a 0 bit back to 1
    char     overwrite_msg[512];
    char     violation[2048];
    uint32_t viol_nbt;
    void    *viol_bt[SIM_BT_DEPTH];

    // Maintained by the child, reset per scenario
    uint32_t wrong_code_count;  ///< failed calls that did not pass on SIM_FLASH_ERROR
    char     wrong_code_msg[256];

    // Weak cells left by PF_MARGINAL, as a power cut late in a real erase or program leaves them. Each
    // weak bit reads as intended, or (1 in 4) not, afresh at every flash op and boot; reset per scenario.
    uint32_t n_weak;
    struct {
        uint32_t offset;
        uint8_t  mask;          ///< the weak bits of the byte
        uint8_t  want;          ///< what the interrupted op meant them to be
    } weak[SIM_MAX_WEAK];
    uint32_t weak_rng;

    // The bit SF_EXTRA_ZERO cleared: programming it again is the fault, not an ffs bug; reset per scenario
    uint32_t stuck0_offset;
    uint8_t  stuck0_mask;

    sim_op_t ops[SIM_MAX_OPS];
} sim_ctl_t;

extern sim_ctl_t *sim;
extern uint8_t *sim_flash_rw;           ///< writable alias (harness only)
extern const uint8_t *sim_flash_ro;     ///< == SIM_FLASH_READ_BASE (what ffs sees)

void  sim_setup(uint32_t ffs_start, uint32_t ffs_end);
void *sim_shared_alloc(size_t size);
void  sim_violation(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void  sim_weak_clear(uint32_t seed);
void  sim_weak_reroll(void);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Simulated NOR flash with power-fail and flash-error injection: see flash_sim.h

#include "flash_sim.h"
#include "pico/flash.h"
#include "pico/bootrom.h"

#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

sim_ctl_t     *sim;
uint8_t       *sim_flash_rw;
const uint8_t *sim_flash_ro = (const uint8_t *)(uintptr_t)SIM_FLASH_READ_BASE;

uint32_t sim_ffs_start_offset;
uint32_t sim_ffs_end_offset;

static uint32_t rng_state;

// xorshift32
static uint32_t rng(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void rng_seed(uint32_t seed) {
    rng_state = seed * 2654435761u + 0x9e3779b9u;
    if (!rng_state) rng_state = 1;
    for (int i = 0; i < 8; i++) {
        rng();
    }
}

static void log_op(sim_op_type_t type, uint32_t offset, uint32_t changed) {
    if (!sim->log_ops || sim->n_logged >= SIM_MAX_OPS) {
        return;
    }
    sim_op_t *op = &sim->ops[sim->n_logged++];
    op->type    = (uint8_t)type;
    op->boot    = sim->boot;
    op->step    = sim->step;
    op->offset  = offset;
    op->changed = changed;
    op->first_in_call = (sim->op_count == sim->call_start_op);
    op->n_bt    = (uint32_t)backtrace(op->bt, SIM_BT_DEPTH);
}

static void power_off(void) {
    sim->power_failed = true;
    _exit(SIM_EXIT_POWERFAIL);
}

static void check_in_ffs_area(const char *fn, uint32_t offs, size_t count) {
    if (offs < sim_ffs_start_offset || offs + count > sim_ffs_end_offset) {
        sim_violation("[flash-op-outside-ffs-area] %s(0x%06x, %zu) is outside the ffs area "
                      "0x%06x-0x%06x - on a real device this corrupts firmware/other data",
                      fn, offs, count, sim_ffs_start_offset, sim_ffs_end_offset);
    }
}

// The weak bits of the byte at flash offset offs
static uint8_t weak_mask_at(uint32_t offs) {
    for (uint32_t i = 0; i < sim->n_weak; i++) {
        if (sim->weak[i].offset == offs) {
            return sim->weak[i].mask;
        }
    }
    return 0;
}

// Weak bits in [offs, offs + len) that a program just cleared (src bit 0) or an erase (src NULL) are solid again
static void weak_settle(uint32_t offs, uint32_t len, const uint8_t *src) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < sim->n_weak; i++) {
        if (sim->weak[i].offset - offs < len) {
            sim->weak[i].mask &= src ? src[sim->weak[i].offset - offs] : 0;
        }
        if (sim->weak[i].mask) {
            sim->weak[kept++] = sim->weak[i];
        }
    }
    sim->n_weak = kept;
}

// PF_MARGINAL: after the op has completed, leave 1-4 of the bits it changed weak. before[] is the
// region as it was; want = the bit value the op meant (0 for a program, 1 for an erase).
static void weak_leave_some(uint32_t offs, const uint8_t *before, const uint8_t *src, uint32_t len) {
    uint32_t candidates = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t changing = src ? (uint8_t)(before[i] & ~src[i]) : (uint8_t)~before[i];
        candidates += (uint32_t)__builtin_popcount(changing);
    }
    rng_seed(sim->fail_param ^ (offs << 8));
    uint32_t k = 1 + sim->fail_param % 4;
    for (uint32_t n = 0; n < k && candidates && sim->n_weak < SIM_MAX_WEAK; n++) {
        uint32_t pick = rng() % candidates;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t changing = src ? (uint8_t)(before[i] & ~src[i]) : (uint8_t)~before[i];
            uint32_t c = (uint32_t)__builtin_popcount(changing);
            if (pick >= c) {
                pick -= c;
                continue;
            }
            while (pick--) {
                changing &= (uint8_t)(changing - 1);
            }
            uint8_t bit = changing & (uint8_t)-changing;
            uint8_t old = weak_mask_at(offs + i);
            if (old) {
                for (uint32_t w = 0; w < sim->n_weak; w++) {
                    if (sim->weak[w].offset == offs + i) {
                        sim->weak[w].mask |= bit;
                        sim->weak[w].want = (uint8_t)((sim->weak[w].want & ~bit) | (src ? 0 : bit));
                    }
                }
            } else {
                sim->weak[sim->n_weak++] = (typeof(sim->weak[0])){ offs + i, bit, (uint8_t)(src ? 0 : bit) };
            }
            break;
        }
    }
}

#ifndef PICO_RP2040
static uint32_t pt_location(uint32_t start, uint32_t end) {
    return ((start / FLASH_SECTOR_SIZE) << PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) |
           (((end / FLASH_SECTOR_SIZE) - 1) << PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB);
}
#endif

//----------------------------------------------------------------------------
// Harness API

void *sim_shared_alloc(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap shared");
        exit(2);
    }
    return p;
}

void sim_setup(uint32_t ffs_start, uint32_t ffs_end) {
    sim_ffs_start_offset = ffs_start;
    sim_ffs_end_offset   = ffs_end;

    int fd = memfd_create("sim_nor_flash", 0);
    if (fd < 0) {
        perror("memfd_create");
        exit(2);
    }
    if (ftruncate(fd, SIM_FLASH_SIZE) != 0) {
        perror("ftruncate of the simulated flash");
        exit(2);
    }

    // Read-only view at the address ffs.c reads from, so a direct store by ffs SEGVs and is reported
    void *ro = mmap((void *)(uintptr_t)SIM_FLASH_READ_BASE, SIM_FLASH_SIZE, PROT_READ,
                    MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (ro != (void *)(uintptr_t)SIM_FLASH_READ_BASE) {
        fprintf(stderr, "error - could not map simulated flash at 0x%08x\n", SIM_FLASH_READ_BASE);
        exit(2);
    }

    // As in the real 16 MB XIP window, reads past the end of a 2 MB device wrap to its start
    for (uint32_t alias = SIM_FLASH_SIZE; alias < 16u * 1024 * 1024; alias += SIM_FLASH_SIZE) {
        void *a = (void *)(uintptr_t)(SIM_FLASH_READ_BASE + alias);
        if (mmap(a, SIM_FLASH_SIZE, PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) != a) {
            fprintf(stderr, "error - could not map flash alias at %p\n", a);
            exit(2);
        }
    }

    sim_flash_rw = mmap(NULL, SIM_FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sim_flash_rw == MAP_FAILED) {
        perror("mmap rw");
        exit(2);
    }

    sim = sim_shared_alloc(sizeof(sim_ctl_t));
    sim->fail_at = SIM_NO_FAIL;
}

void sim_weak_clear(uint32_t seed) {
    sim->n_weak = 0;
    sim->weak_rng = seed * 2654435761u | 1;
    sim->stuck0_mask = 0;
}

void sim_weak_reroll(void) {
    for (uint32_t i = 0; i < sim->n_weak; i++) {
        uint32_t x = sim->weak_rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        sim->weak_rng = x;
        uint8_t m = sim->weak[i].mask;
        uint8_t wrong = m & (uint8_t)x & (uint8_t)(x >> 8);
        uint8_t *b = sim_flash_rw + sim->weak[i].offset;
        *b = (uint8_t)((*b & ~m) | ((sim->weak[i].want ^ wrong) & m));
    }
}

void sim_violation(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sim->violation, sizeof(sim->violation), fmt, ap);
    va_end(ap);
    sim->viol_nbt = (uint32_t)backtrace(sim->viol_bt, SIM_BT_DEPTH);
    _exit(SIM_EXIT_VIOLATION);
}

//----------------------------------------------------------------------------
// SDK / bootrom API replacements

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) {
    if ((flash_offs % FLASH_PAGE_SIZE) || (count % FLASH_PAGE_SIZE)) {
        sim_violation("[bad-alignment] flash_range_program(0x%06x, %zu)", flash_offs, count);
    }
    check_in_ffs_area("flash_range_program", flash_offs, count);
    sim_weak_reroll();

    for (size_t page = 0; page < count; page += FLASH_PAGE_SIZE) {
        uint32_t offs = flash_offs + (uint32_t)page;
        uint8_t *dst = sim_flash_rw + offs;
        const uint8_t *src = data + page;
        uint32_t changed = 0;

        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
            // 0xff leaves a byte as it is, so ffs may program it anywhere
            uint8_t stuck0 = offs + i == sim->stuck0_offset ? sim->stuck0_mask : 0;
            if (src[i] != 0xff && (src[i] & ~dst[i] & ~weak_mask_at(offs + i) & ~stuck0)) {
                // NOR can't do this - ffs wanted a value that flash can't hold
                if (sim->overwrite_count++ == 0) {
                    snprintf(sim->overwrite_msg, sizeof(sim->overwrite_msg),
                             "boot %u step %u: programming 0x%02x over already-programmed 0x%02x at "
                             "flash offset 0x%06x (NOR can only clear bits - result is 0x%02x)",
                             sim->boot, sim->step, src[i], dst[i], offs + i, src[i] & dst[i]);
                }
            }
            if ((dst[i] & src[i]) != dst[i]) {
                changed++;
            }
        }

        log_op(OP_PROGRAM, offs, changed);

        if (sim->op_count == sim->fail_at && sim->fail_mode == SF_MISSED_BIT && changed) {
            // Pick one bit the program should clear, and leave it set
            uint32_t bits = 0;
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                bits += (uint32_t)__builtin_popcount((uint8_t)(dst[i] & ~src[i]));
            }
            rng_seed(sim->fail_param ^ (offs << 8));
            uint32_t pick = rng() % bits;
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                uint8_t clearing = (uint8_t)(dst[i] & ~src[i]);
                dst[i] &= src[i];
                uint32_t c = (uint32_t)__builtin_popcount(clearing);
                if (pick < c) {
                    while (pick--) {
                        clearing &= (uint8_t)(clearing - 1);
                    }
                    dst[i] |= clearing & (uint8_t)-clearing;
                    pick = UINT32_MAX;
                } else if (pick != UINT32_MAX) {
                    pick -= c;
                }
            }
            weak_settle(offs, FLASH_PAGE_SIZE, src);
            sim->silent_faults++;
            sim->op_count++;
            continue;
        }

        if (sim->op_count == sim->fail_at && sim->fail_mode == SF_EXTRA_ZERO && changed) {
            // Pick one bit the program should leave 1, in a byte it programs, and clear it
            uint32_t bits = 0;
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                if (src[i] != 0xff) {
                    bits += (uint32_t)__builtin_popcount((uint8_t)(dst[i] & src[i]));
                }
            }
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                dst[i] &= src[i];
            }
            weak_settle(offs, FLASH_PAGE_SIZE, src);
            if (bits > 0) {
                rng_seed(sim->fail_param ^ (offs << 8));
                uint32_t pick = rng() % bits;
                for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                    uint8_t keeping = src[i] != 0xff ? dst[i] : 0;
                    uint32_t c = (uint32_t)__builtin_popcount(keeping);
                    if (pick < c) {
                        while (pick--) {
                            keeping &= (uint8_t)(keeping - 1);
                        }
                        sim->stuck0_offset = offs + i;
                        sim->stuck0_mask = keeping & (uint8_t)-keeping;
                        dst[i] &= (uint8_t)~sim->stuck0_mask;
                        sim->silent_faults++;
                        break;
                    }
                    pick -= c;
                }
            }
            sim->op_count++;
            continue;
        }

        if (sim->op_count == sim->fail_at && sim->fail_mode == PF_MARGINAL) {
            uint8_t before[FLASH_PAGE_SIZE];
            memcpy(before, dst, FLASH_PAGE_SIZE);
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                dst[i] &= src[i];
            }
            weak_settle(offs, FLASH_PAGE_SIZE, src);
            weak_leave_some(offs, before, src, FLASH_PAGE_SIZE);
            sim_weak_reroll();
            power_off();
        }

        if (sim->op_count == sim->fail_at && !IS_FLASH_ERROR_MODE(sim->fail_mode)) {
            if (sim->fail_mode == PF_TORN_PREFIX) {
                uint32_t landed = 0;
                for (uint32_t i = 0; i < FLASH_PAGE_SIZE && landed < sim->fail_param; i++) {
                    if ((dst[i] & src[i]) != dst[i]) {
                        dst[i] &= src[i];
                        landed++;
                    }
                }
            } else if (sim->fail_mode == PF_TORN_RANDOM) {
                rng_seed(sim->fail_param ^ (offs << 8));
                for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
                    uint8_t to_clear = dst[i] & ~src[i];
                    dst[i] &= (uint8_t)~(to_clear & rng());
                }
            }
            power_off();
        }

        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
            dst[i] &= src[i];
        }
        weak_settle(offs, FLASH_PAGE_SIZE, src);
        sim->op_count++;
    }
}

void flash_range_erase(uint32_t flash_offs, size_t count) {
    if ((flash_offs % FLASH_SECTOR_SIZE) || (count % FLASH_SECTOR_SIZE)) {
        sim_violation("[bad-alignment] flash_range_erase(0x%06x, %zu)", flash_offs, count);
    }
    check_in_ffs_area("flash_range_erase", flash_offs, count);
    sim_weak_reroll();

    for (size_t sect = 0; sect < count; sect += FLASH_SECTOR_SIZE) {
        uint32_t offs = flash_offs + (uint32_t)sect;
        uint8_t *dst = sim_flash_rw + offs;
        uint32_t changed = 0;
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
            changed += (dst[i] != 0xff);
        }

        log_op(OP_ERASE, offs, changed);
        if (sim->stuck0_offset - offs < FLASH_SECTOR_SIZE) {
            sim->stuck0_mask = 0;
        }

        if (sim->op_count == sim->fail_at && sim->fail_mode == PF_MARGINAL) {
            static uint8_t before[FLASH_SECTOR_SIZE];
            memcpy(before, dst, FLASH_SECTOR_SIZE);
            memset(dst, 0xff, FLASH_SECTOR_SIZE);
            weak_settle(offs, FLASH_SECTOR_SIZE, NULL);
            weak_leave_some(offs, before, NULL, FLASH_SECTOR_SIZE);
            sim_weak_reroll();
            power_off();
        }

        if (sim->op_count == sim->fail_at && !IS_FLASH_ERROR_MODE(sim->fail_mode)) {
            if (sim->fail_mode == PF_TORN_PREFIX) {
                memset(dst, 0xff, (FLASH_SECTOR_SIZE / 8) * sim->fail_param);
            } else if (sim->fail_mode == PF_TORN_RANDOM) {
                rng_seed(sim->fail_param ^ (offs << 8));
                for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) {
                    dst[i] |= (uint8_t)rng();
                }
            }
            power_off();
        }

        memset(dst, 0xff, FLASH_SECTOR_SIZE);
        weak_settle(offs, FLASH_SECTOR_SIZE, NULL);
        sim->op_count++;
    }
}

// Like the real one: an error on entry skips func, an error on exit follows it
int flash_safe_execute(void (*func)(void *), void *param, uint32_t enter_exit_timeout_ms) {
    (void)enter_exit_timeout_ms;

    // FE_TWICE's second error, counted in calls from the first
    bool inject = sim->second_error_in && --sim->second_error_in == 0;

    if (IS_FLASH_ERROR_MODE(sim->fail_mode) && sim->op_count == sim->fail_at) {
        // Once only
        sim->fail_at = SIM_NO_FAIL;
        inject = true;
        if (sim->fail_mode == FE_TWICE) {
            sim->second_error_in = sim->fail_param;
        }
    }

    if (inject) {
        sim->errors_injected++;
        if (sim->fail_mode != FE_AFTER) {
            return SIM_FLASH_ERROR;
        }
    }

    sim->call_start_op = sim->op_count;
    func(param);

    return inject ? SIM_FLASH_ERROR : PICO_OK;
}

#ifndef PICO_RP2040
// Minimal RP2350 partition table: partition 0 is firmware with no ID, partition 1 is the ffs area
int rom_get_partition_table_info(uint32_t *out_buffer, uint32_t out_buffer_word_size, uint32_t partition_and_flags) {
    const uint32_t want_all = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID;
    const uint32_t want_one = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION;
    uint64_t id = FFS_DATA_PARTITION_ID;

    if (partition_and_flags == want_all && out_buffer_word_size >= 7) {
        out_buffer[0] = partition_and_flags;
        // Partition 0, with no ID
        out_buffer[1] = pt_location(0, sim_ffs_start_offset);
        out_buffer[2] = 0;
        // Partition 1, the ffs area
        out_buffer[3] = pt_location(sim_ffs_start_offset, sim_ffs_end_offset);
        out_buffer[4] = PICOBIN_PARTITION_FLAGS_HAS_ID_BITS;
        out_buffer[5] = (uint32_t)id;
        out_buffer[6] = (uint32_t)(id >> 32);
        return 7;
    }

    if ((partition_and_flags & 0x00ffffffu) == want_one && out_buffer_word_size >= 3) {
        uint32_t p = partition_and_flags >> 24;
        out_buffer[0] = want_one;
        out_buffer[1] = p ? pt_location(sim_ffs_start_offset, sim_ffs_end_offset)
                          : pt_location(0, sim_ffs_start_offset);
        out_buffer[2] = p ? PICOBIN_PARTITION_FLAGS_HAS_ID_BITS : 0;
        return 3;
    }

    return PICO_ERROR_INVALID_ARG;
}
#endif

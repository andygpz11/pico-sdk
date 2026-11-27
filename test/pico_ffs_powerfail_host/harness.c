/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// pico_ffs power-fail and flash-error test harness (host build): runs the unmodified ffs
// sources against a simulated NOR flash and checks every file after each fault (see README.md)

#include "flash_sim.h"
#include "pico/ffs.h"
#include "ffs_internal.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NUM_IDS         (FFS_MAX_VALID_FILE_ID + 1)
#define MAX_FILE_LEN    16384
#define MAX_STEPS       512
#define MAX_BOOTS       5
#define HDR_SIZE        ((uint32_t)sizeof(ffs_file_hdr_t))
#define STEP_NOTE_LEN   16
// A boot that runs longer than this is reported as a hang
#define BOOT_TIMEOUT_S  20
// Torn erases leave the first 1/8 .. 7/8 of the sector erased
#define ERASE_PARTS     8
// FE_TWICE: the second error comes 1 .. this many flash calls after the first
#define FE_TWICE_MAX_GAP 8

//----------------------------------------------------------------------------
// Workload description

// ST_STRING_SAME: ffs_update_string() with the string the file already holds
typedef enum {
    ST_WRITE,
    ST_DELETE,
    ST_STRING,
    ST_STRING_SAME,
} step_kind_t;

typedef enum {
    PAT_RANDOM,
    PAT_FF_HEAVY,
    PAT_TEXT,
} pattern_t;

typedef struct {
    uint8_t  kind;
    uint8_t  id;
    uint8_t  pattern;
    uint16_t len;
} step_t;

static step_t pre_steps[MAX_STEPS], post_steps[MAX_STEPS], fill_steps[MAX_STEPS];
static int    n_pre, n_post, n_fill;

//----------------------------------------------------------------------------
// Oracle - what ffs is expected to contain. Shared with the children.

typedef struct {
    bool     exists;
    uint16_t len;
    uint8_t  boot;
    uint16_t step;
    uint8_t  data[MAX_FILE_LEN];
} file_state_t;

enum {
    IF_NONE,
    IF_WRITE,
    IF_DELETE,
};

typedef struct {
    file_state_t committed[NUM_IDS];
    uint8_t      inflight_kind;
    uint8_t      inflight_id;
    uint8_t      inflight_boot;
    uint16_t     inflight_step;
    bool         inflight_was_create;
    file_state_t inflight_new;

    // A call that returned a flash error leaves its file either old or new (G2) until the
    // recovery that the next ffs call starts with settles it
    bool         unc_active;
    uint8_t      unc_kind;
    uint8_t      unc_id;
    bool         unc_was_create;
    bool         unc_must_be_old;   ///< R8: the old copy was still valid in flash when the write failed
    file_state_t unc_new;

    uint32_t     n_insufficient;    ///< writes refused for lack of space
    char         step_note[MAX_BOOTS][MAX_STEPS][STEP_NOTE_LEN];
} oracle_t;

// The oracle, shared with the children
static oracle_t *oracle;
// Private: the oracle and the flash image at the start of each scenario
static oracle_t *baseline;
static uint8_t  *pristine;

static uint32_t ffs_start, ffs_end, chunk_size;

//----------------------------------------------------------------------------
// Options

static struct {
    const char *preload;
    uint32_t chunk_sectors;
    bool mode_clean, mode_prefix, mode_random, mode_marginal, mode_silent;
    bool mode_err_before, mode_err_after, mode_err_twice;
    uint32_t seeds;
    uint32_t prefix_samples;
    bool nested;
    int  max_examples;
    bool verbose;
    long only_op;
} opt = {
        .preload = NULL,
        .chunk_sectors = 1,
        .mode_clean = true,
        .mode_prefix = true,
        .mode_random = true,
        .mode_marginal = false,
        .mode_silent = false,
        .mode_err_before = true,
        .mode_err_after = true,
        .mode_err_twice = true,
        .seeds = 4,
        .prefix_samples = 24,
        .nested = false,
        .max_examples = 1000,
        .verbose = false,
        .only_op = -1,
};

//----------------------------------------------------------------------------
// Helpers

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static void gen_content(uint8_t *buf, uint boot, uint step, uint id, pattern_t pat, uint len) {
    uint32_t s = hash32(boot * 1000003u ^ step * 7919u ^ id * 104729u);
    for (uint i = 0; i < len; i++) {
        s = hash32(s + i);
        switch (pat) {
        case PAT_RANDOM:   buf[i] = (uint8_t)s; break;
        // Mostly 0xff - realistic for binary config blobs, and lots of
        // word-aligned 0xffffffff that looks like virgin flash
        case PAT_FF_HEAVY: buf[i] = (i % 13) ? 0xff : (uint8_t)s; break;
        case PAT_TEXT:     buf[i] = (uint8_t)('a' + s % 26); break;
        }
    }
    if (pat == PAT_TEXT && len) {
        int n = snprintf((char *)buf, len, "b%u-s%u-id%u-", boot, step, id);
        // Undo snprintf's NUL
        if (n < (int)len) buf[n] = 'x';
        buf[len - 1] = 0;
    }
}

static uint32_t rec_size(uint len) {
    return HDR_SIZE + ROUND_UP_TO_NEXT_32B_WORD(len);
}

static void add_step(step_t *steps, int *n, step_kind_t kind, uint id, uint len, pattern_t pat) {
    if (*n >= MAX_STEPS) {
        fprintf(stderr, "too many workload steps\n");
        exit(2);
    }
    steps[(*n)++] = (step_t){ (uint8_t)kind, (uint8_t)id, (uint8_t)pat, (uint16_t)len };
}

static void build_workloads(void) {
    // Boot 1, during which the power is cut: first init, new, zero-length and page-spanning files,
    // updates, delete and re-create, strings, and chunk swaps from both updates and new files
    step_t *s = pre_steps;
    add_step(s, &n_pre, ST_WRITE,  1,  40, PAT_RANDOM);
    add_step(s, &n_pre, ST_WRITE,  2, 300, PAT_RANDOM);
    add_step(s, &n_pre, ST_WRITE,  3,   0, PAT_RANDOM);
    add_step(s, &n_pre, ST_WRITE,  1,  60, PAT_RANDOM);
    add_step(s, &n_pre, ST_WRITE,  4, 200, PAT_FF_HEAVY);
    add_step(s, &n_pre, ST_DELETE, 2,   0, PAT_RANDOM);
    add_step(s, &n_pre, ST_WRITE,  2, 500, PAT_RANDOM);
    add_step(s, &n_pre, ST_STRING, FFS_WIFI_SSID_FILE_ID, 24, PAT_TEXT);
    add_step(s, &n_pre, ST_STRING_SAME, FFS_WIFI_SSID_FILE_ID, 0, PAT_TEXT);

    uint32_t written = 0;
    for (uint r = 0; written < 3 * chunk_size; r++) {
        add_step(s, &n_pre, ST_WRITE, 1, 600, PAT_RANDOM);
        // A new file
        add_step(s, &n_pre, ST_WRITE, 10 + r % 40, 120, PAT_RANDOM);
        written += rec_size(600) + rec_size(120);
        if (r) {
            add_step(s, &n_pre, ST_DELETE, 10 + (r - 1) % 40, 0, PAT_RANDOM);
        }
        if (r % 2) {
            add_step(s, &n_pre, ST_WRITE, 4, 200, PAT_FF_HEAVY);
            written += rec_size(200);
        }
    }

    // Boots after the power failure: prove ffs is still fully usable
    s = post_steps;
    written = 0;
    for (uint r = 0; written < 2.5 * chunk_size; r++) {
        add_step(s, &n_post, ST_WRITE,  1, 333 + r, PAT_RANDOM);
        add_step(s, &n_post, ST_WRITE,  60 + r % 30, 77, PAT_RANDOM);
        add_step(s, &n_post, ST_WRITE,  2, 450, PAT_RANDOM);
        add_step(s, &n_post, ST_DELETE, 60 + r % 30, 0, PAT_RANDOM);
        add_step(s, &n_post, ST_WRITE,  3, (r % 2) ? 0 : 10, PAT_RANDOM);
        add_step(s, &n_post, ST_WRITE,  4, 200, PAT_FF_HEAVY);
        add_step(s, &n_post, ST_STRING, FFS_WIFI_SSID_FILE_ID, 30, PAT_TEXT);
        add_step(s, &n_post, ST_STRING_SAME, FFS_WIFI_SSID_FILE_ID, 0, PAT_TEXT);
        written += rec_size(333 + r) + rec_size(77) + rec_size(450) + rec_size(10) + rec_size(200) + rec_size(30);
    }

    // Filling ffs: new files until writes are refused, then an update, a delete and a new file when nearly full
    s = fill_steps;
    uint n_files = chunk_size / rec_size(400) + 2;
    for (uint i = 0; i < n_files; i++) {
        add_step(s, &n_fill, ST_WRITE, 110 + i, 400, PAT_RANDOM);
    }
    add_step(s, &n_fill, ST_WRITE,  110, 400, PAT_RANDOM);
    add_step(s, &n_fill, ST_DELETE, 111, 0, PAT_RANDOM);
    add_step(s, &n_fill, ST_WRITE,  110 + n_files, 300, PAT_RANDOM);
}

static const char *kind_name(uint8_t kind) {
    return kind == ST_WRITE ? "ffs_write" : kind == ST_DELETE ? "ffs_delete" : "ffs_update_string";
}

static void describe_step(char *buf, size_t len, uint boot, uint step, const step_t *steps, int n) {
    if (step == 0) {
        snprintf(buf, len, "ffs_initialise()");
    } else if ((int)step <= n) {
        const step_t *st = &steps[step - 1];
        // Recorded in the fault-free run
        const char *note = baseline->step_note[boot][step];
        if (st->kind == ST_DELETE) {
            snprintf(buf, len, "step %u: ffs_delete(id=%u)", step, st->id);
        } else {
            snprintf(buf, len, "step %u: %s(id=%u, len=%u)%s%s", step, kind_name(st->kind),
                     st->id, st->len, *note ? " - " : "", note);
        }
    } else {
        snprintf(buf, len, "step %u", step);
    }
}

//----------------------------------------------------------------------------
// Child side: one "boot" of the device

static const char *hdr_state(uint32_t offs, uint16_t *len_out, uint8_t *id_out);

// Whether the chunk holds a valid record of the file, with exactly this content unless fs is NULL
// (read raw, not through ffs)
static bool valid_copy_in_chunk(uint id, const file_state_t *fs, uint32_t chunk) {
    uint32_t o = chunk;
    while (o + HDR_SIZE <= chunk + chunk_size) {
        uint16_t dl = 0;
        uint8_t rid = 0;
        const char *st = hdr_state(o, &dl, &rid);
        if (!st || strcmp(st, "virgin") == 0 || strncmp(st, "BAD-CRC0", 8) == 0) {
            o += 4;
            continue;
        }
        if (rid == id && strcmp(st, "valid") == 0 &&
            (fs == NULL || (dl == fs->len && memcmp(sim_flash_ro + o + HDR_SIZE, fs->data, dl) == 0))) {
            return true;
        }
        o += ROUND_UP_TO_NEXT_32B_WORD(HDR_SIZE + dl);
    }
    return false;
}

// C4: a deprecated chunk's records stay current unless the other chunk is valid and holds the file
static bool valid_copy_in_flash(uint id, const file_state_t *fs) {
    for (uint32_t c = ffs_start; c < ffs_end; c += chunk_size) {
        const char *hs = hdr_state(c, NULL, NULL);
        if (hs && strcmp(hs, "deprecated") == 0) {
            uint32_t other = c == ffs_start ? ffs_start + chunk_size : ffs_start;
            const char *os = hdr_state(other, NULL, NULL);
            if (os && strcmp(os, "valid") == 0 && valid_copy_in_chunk(id, NULL, other)) {
                continue;
            }
        }
        if (valid_copy_in_chunk(id, fs, c)) {
            return true;
        }
    }
    return false;
}

// G3: a call whose opening recovery hits a flash error must fail; the next call retries it
static int list_retrying(uint boot, const char *where, uint step, file_info_t *info, uint max) {
    for (int tries = 0; ; tries++) {
        uint32_t before = sim->errors_injected;
        int n = ffs_list(info, max);
        if (sim->errors_injected == before || tries == 3) {
            return n;
        }
        if (n >= 0) {
            sim_violation("[flash-error-not-reported] boot %u %s %u: ffs_list() returned %d although a flash "
                          "operation in its recovery failed", boot, where, step, n);
        }
    }
}

static int read_retrying(uint boot, const char *where, uint step, uint id, const char **p) {
    for (int tries = 0; ; tries++) {
        uint32_t before = sim->errors_injected;
        int r = ffs_read(id, p);
        if (sim->errors_injected == before || tries == 3) {
            return r;
        }
        if (r >= 0) {
            sim_violation("[flash-error-not-reported] boot %u %s %u: ffs_read(%u) returned %d although a flash "
                          "operation in its recovery failed", boot, where, step, id, r);
        }
    }
}

static bool state_matches(const file_state_t *fs, int r, const char *p) {
    if (!fs->exists) {
        return r == PICO_ERROR_NOT_FOUND;
    }
    return r == (int)fs->len && (r == 0 || memcmp(p, fs->data, r) == 0);
}

static void describe_read(char *buf, size_t len, int r) {
    if (r == PICO_ERROR_NOT_FOUND) {
        snprintf(buf, len, "NOT_FOUND");
    } else if (r < 0) {
        snprintf(buf, len, "error %d", r);
    } else {
        snprintf(buf, len, "%d bytes (content wrong)", r);
    }
}

static void describe_state(char *buf, size_t len, const file_state_t *fs) {
    if (!fs->exists) {
        snprintf(buf, len, "absent");
    } else {
        snprintf(buf, len, "%u bytes, written at boot %u step %u", fs->len, fs->boot, fs->step);
    }
}

// Check a file whose last call was interrupted or failed: it must be old (*c) or new (*nw).
// must_be_old (R8): a write failed while the old copy was still valid, so the old one must remain.
static void check_old_or_new(uint boot, const char *where, uint id, file_state_t *c, const file_state_t *nw,
                             uint8_t kind, bool was_create, bool must_be_old, const char *why, int r,
                             const char *p) {
    if (state_matches(nw, r, p)) {
        if (must_be_old && !state_matches(c, r, p)) {
            sim_violation("[failed-write-visible] boot %u %s: a write of file %u failed while its previous "
                          "state was still valid in flash, so that state must remain (R8), but ffs_read() "
                          "gives the new data", boot, where, id);
        }
        *c = *nw;
        return;
    }
    if (state_matches(c, r, p)) {
        return;
    }

    char got[64], want[96], want_new[96];
    describe_read(got, sizeof(got), r);
    describe_state(want, sizeof(want), c);
    describe_state(want_new, sizeof(want_new), nw);
    if (c->exists && !was_create && r < 0) {
        sim_violation("[previous-version-lost] boot %u %s: file %u was being rewritten when %s; ffs_read() "
                      "gives %s but must give either the previous version (%s) or the new one (%s)",
                      boot, where, id, why, got, want, want_new);
    }
    sim_violation("[interrupted-file-corrupt] boot %u %s: file %u was being %s when %s; ffs_read() gives %s "
                  "but must give either the previous state (%s) or the new one (%s)", boot, where, id,
                  kind == IF_DELETE ? "deleted" : was_create ? "created" : "rewritten",
                  why, got, want, want_new);
}

static void verify_all(uint boot, uint step, bool resolve_inflight, bool full) {
    const char *where = resolve_inflight ? "after ffs_initialise()" : "after step";

    // ffs_list() first, so that even a partial check covers everything it reports. After a
    // flash error it also runs the recovery, which settles the failed call's file.
    file_info_t info[NUM_IDS + 16];
    int n = list_retrying(boot, where, step, info, count_of(info));
    if (n < 0) {
        sim_violation("[list-failed] boot %u %s %u: ffs_list() returned %d", boot, where, step, n);
    }
    bool listed[256] = { 0 };
    for (int i = 0; i < n; i++) {
        uint id = info[i].file_id;
        if (id > 255 || listed[id]) {
            sim_violation("[list-duplicate] boot %u %s %u: ffs_list() reports file %u more than once",
                          boot, where, step, id);
        }
        listed[id] = true;
    }

    uint n_exist = 0;
    for (uint id = 0; id < NUM_IDS; id++) {
        file_state_t *c = &oracle->committed[id];
        bool is_inflight = resolve_inflight && oracle->inflight_kind != IF_NONE && id == oracle->inflight_id;
        bool is_unc = oracle->unc_active && id == oracle->unc_id;

        // Reading all 255 ids is slow (ffs CRCs every record on every read), so
        // between steps only check ids that ffs or the oracle know about.
        if (!full && !c->exists && !listed[id] && !is_inflight && !is_unc) {
            continue;
        }

        const char *p = NULL;
        int r = read_retrying(boot, where, step, id, &p);
        bool ok_old = state_matches(c, r, p);
        char got[64], want[96];
        describe_read(got, sizeof(got), r);
        describe_state(want, sizeof(want), c);

        if (is_unc) {
            file_state_t *nw = &oracle->unc_new;
            nw->exists = (oracle->unc_kind == IF_WRITE);
            check_old_or_new(boot, where, id, c, nw, oracle->unc_kind, oracle->unc_was_create,
                             oracle->unc_must_be_old, "its call returned a flash error", r, p);
        } else if (is_inflight) {
            file_state_t *nw = &oracle->inflight_new;
            nw->exists = (oracle->inflight_kind == IF_WRITE);
            check_old_or_new(boot, where, id, c, nw, oracle->inflight_kind, oracle->inflight_was_create,
                             false, "power failed", r, p);
        } else if (!ok_old) {
            if (c->exists) {
                sim_violation("[%s] boot %u %s %u: file %u should be %s but ffs_read() gives %s",
                              r < 0 ? "committed-file-lost" : "committed-file-corrupt",
                              boot, where, step, id, want, got);
            }
            sim_violation("[phantom-file] boot %u %s %u: file %u should be absent but ffs_read() gives %d",
                          boot, where, step, id, r);
        }
        n_exist += c->exists;
    }

    if (resolve_inflight) {
        oracle->inflight_kind = IF_NONE;
    }
    oracle->unc_active = false;

    for (int i = 0; i < n; i++) {
        uint id = info[i].file_id;
        if (id >= NUM_IDS || !oracle->committed[id].exists || info[i].data_len != oracle->committed[id].len) {
            sim_violation("[list-mismatch] boot %u %s %u: ffs_list() reports file %u len %u which ffs_read() "
                          "does not agree with", boot, where, step, id, info[i].data_len);
        }
    }
    if ((uint)n != n_exist) {
        sim_violation("[list-mismatch] boot %u %s %u: ffs_list() returns %d files but ffs_read() finds %u",
                      boot, where, step, n, n_exist);
    }
}

static bool write_should_fit(uint id, uint len) {
    // Start with the chunk header
    uint32_t total = HDR_SIZE;
    for (uint i = 0; i < NUM_IDS; i++) {
        if (i != id && oracle->committed[i].exists) {
            total += rec_size(oracle->committed[i].len);
        }
    }
    return total + rec_size(len) + 3 <= chunk_size;
}

// G3: a call that hit a flash error must return an error, ideally the flash error itself
static void check_error_reported(uint boot, uint step, const char *call, int rc) {
    if (rc == PICO_OK) {
        sim_violation("[flash-error-not-reported] boot %u step %u: %s returned PICO_OK although a flash "
                      "operation in it failed", boot, step, call);
    }
    if (rc != SIM_FLASH_ERROR && sim->wrong_code_count++ == 0) {
        snprintf(sim->wrong_code_msg, sizeof(sim->wrong_code_msg),
                 "boot %u step %u: %s returned %d, not the flash error %d", boot, step, call, rc, SIM_FLASH_ERROR);
    }
}

// The call returned a flash error: its file is old or new until settled
static void mark_uncertain(uint8_t kind, uint id, bool was_create, bool must_be_old, const file_state_t *nw) {
    oracle->unc_active = true;
    oracle->unc_kind = kind;
    oracle->unc_id = (uint8_t)id;
    oracle->unc_was_create = was_create;
    oracle->unc_must_be_old = must_be_old;
    if (nw) {
        oracle->unc_new = *nw;
    }
    oracle->inflight_kind = IF_NONE;
}

static const char *error_category(const char *usual) {
    return sim->errors_injected ? "call-failed-after-flash-error" : usual;
}

static void do_step(uint boot, uint step, const step_t *st) {
    file_state_t *c = &oracle->committed[st->id];
    char *note = oracle->step_note[boot][step];
    uint32_t errors_before = sim->errors_injected;

    if (st->kind == ST_STRING_SAME) {
        // Only a file that holds a NUL-terminated string, as ffs_update_string() writes, is unchanged by it
        bool is_string = c->exists && c->len > 0 && c->data[c->len - 1] == 0 &&
                         strlen((const char *)c->data) == (size_t)c->len - 1;
        if (!is_string || (oracle->unc_active && oracle->unc_id == st->id)) {
            snprintf(note, STEP_NOTE_LEN, "skipped");
            return;
        }
        snprintf(note, STEP_NOTE_LEN, "same string");
        uint32_t ops_before = sim->op_count;
        int rc = ffs_update_string(st->id, (const char *)c->data);
        if (sim->errors_injected != errors_before) {
            // The data is unchanged whatever happened, so only the error needs checking
            check_error_reported(boot, step, "ffs_update_string(unchanged)", rc);
            return;
        }
        if (rc != PICO_OK) {
            sim_violation("[%s] boot %u step %u: ffs_update_string(id=%u) with an unchanged string returned %d",
                          error_category("write-error"), boot, step, st->id, rc);
        }
        // Recovery after an earlier flash error may legitimately program flash
        if (sim->errors_injected == 0 && sim->op_count != ops_before) {
            sim_violation("[unchanged-string-rewritten] boot %u step %u: ffs_update_string(id=%u) did %u flash "
                          "op(s) although the string was unchanged", boot, step, st->id, sim->op_count - ops_before);
        }
        return;
    }

    if (st->kind == ST_DELETE) {
        bool existed = c->exists;
        snprintf(note, STEP_NOTE_LEN, existed ? "delete" : "delete-absent");
        oracle->inflight_id = st->id;
        oracle->inflight_boot = (uint8_t)boot;
        oracle->inflight_step = (uint16_t)step;
        oracle->inflight_was_create = false;
        oracle->inflight_kind = IF_DELETE;

        int rc = ffs_delete(st->id);
        if (sim->errors_injected != errors_before) {
            check_error_reported(boot, step, "ffs_delete()", rc);
            mark_uncertain(IF_DELETE, st->id, false, false, NULL);
            return;
        }
        bool unc = oracle->unc_active && oracle->unc_id == st->id;
        bool ok = unc ? (rc == PICO_OK || rc == PICO_ERROR_NOT_FOUND)
                      : existed ? (rc == PICO_OK) : (rc == PICO_ERROR_NOT_FOUND);
        if (!ok) {
            sim_violation("[%s] boot %u step %u: ffs_delete(%u) returned %d", error_category("delete-error"),
                          boot, step, st->id, rc);
        }
        c->exists = false;
        if (oracle->unc_active && oracle->unc_id == st->id) {
            oracle->unc_active = false;
        }
        oracle->inflight_kind = IF_NONE;
        return;
    }

    file_state_t *nw = &oracle->inflight_new;
    gen_content(nw->data, boot, step, st->id, (pattern_t)st->pattern, st->len);
    nw->len = st->len;
    nw->boot = (uint8_t)boot;
    nw->step = (uint16_t)step;
    nw->exists = true;
    bool fits = write_should_fit(st->id, st->len);
    snprintf(note, STEP_NOTE_LEN, c->exists ? "update" : "new file");

    oracle->inflight_id = st->id;
    oracle->inflight_boot = (uint8_t)boot;
    oracle->inflight_step = (uint16_t)step;
    oracle->inflight_was_create = !c->exists;
    oracle->inflight_kind = IF_WRITE;

    int rc;
    if (st->kind == ST_STRING) {
        bool same = c->exists && c->len == nw->len && memcmp(c->data, nw->data, nw->len) == 0;
        if (same) {
            snprintf(note, STEP_NOTE_LEN, "unchanged");
        }
        rc = ffs_update_string(st->id, (const char *)nw->data);
    } else {
        rc = ffs_write(st->id, (const char *)nw->data, st->len);
    }

    if (sim->errors_injected != errors_before) {
        char call[48];
        snprintf(call, sizeof(call), "%s(id=%u)", kind_name(st->kind), st->id);
        check_error_reported(boot, step, call, rc);
        // R8: with one error, if the old copy is still valid the failed write must not show
        bool must_be_old = sim->fail_mode != FE_TWICE && c->exists && valid_copy_in_flash(st->id, c);
        mark_uncertain(IF_WRITE, st->id, !c->exists, must_be_old, nw);
        return;
    }

    if (rc != PICO_OK) {
        if (rc == PICO_ERROR_INSUFFICIENT_RESOURCES && !fits) {
            oracle->n_insufficient++;
            oracle->inflight_kind = IF_NONE;
            return;
        }
        sim_violation("[%s] boot %u step %u: %s(id=%u, len=%u) returned %d%s", error_category("write-error"),
                      boot, step, kind_name(st->kind), st->id, st->len, rc, fits ? " although the data fits" : "");
    }
    *c = *nw;
    if (oracle->unc_active && oracle->unc_id == st->id) {
        oracle->unc_active = false;
    }
    oracle->inflight_kind = IF_NONE;
}

static void boot_main(uint boot, const step_t *steps, int n) {
    sim->boot = (uint8_t)boot;
    sim->step = 0;
    sim->op_count = 0;

    int rc = ffs_initialise();
    if (sim->errors_injected) {
        // G3: initialisation hit a flash error, so it must fail and ffs is unusable this boot
        check_error_reported(boot, 0, "ffs_initialise()", rc);
        return;
    }
    if (rc != PICO_OK) {
        sim_violation("[init-failed] boot %u: ffs_initialise() returned %d", boot, rc);
    }
    // A plain reboot follows a completed boot, so there is nothing to repair
    if (n == 0 && sim->op_count != 0) {
        sim_violation("[repair-without-cause] boot %u: ffs_initialise() did %u flash op(s) after a clean shutdown",
                      boot, sim->op_count);
    }
    verify_all(boot, 0, true, true);

    for (int i = 0; i < n; i++) {
        sim->step = (uint16_t)(i + 1);
        do_step(boot, i + 1, &steps[i]);
        verify_all(boot, i + 1, false, i + 1 == n);
    }
}

static void baseline_main(void) {
    sim->boot = 0;
    int rc = ffs_initialise();
    if (rc != PICO_OK) {
        sim_violation("[init-failed] baseline: ffs_initialise() returned %d", rc);
    }
    for (uint id = 0; id < NUM_IDS; id++) {
        const char *p;
        int r = ffs_read(id, &p);
        file_state_t *c = &oracle->committed[id];
        if (r < 0 && r != PICO_ERROR_NOT_FOUND) {
            sim_violation("[read-failed] baseline: ffs_read(%u) returned %d", id, r);
        }
        c->exists = r >= 0;
        if (r >= 0) {
            c->len = (uint16_t)r;
            memcpy(c->data, p, r);
        }
    }
}

//----------------------------------------------------------------------------
// Parent side: process control

typedef enum {
    CH_OK,
    CH_POWERFAIL,
    CH_VIOLATION,
    CH_CRASH,
    CH_TIMEOUT,
} child_result_t;

static child_result_t spawn_boot(uint boot, const step_t *steps, int n,
                                 uint32_t fail_at, pf_mode_t mode, uint32_t param, bool log) {
    sim->fail_at = fail_at;
    sim->fail_mode = (uint8_t)mode;
    sim->fail_param = param;
    sim->log_ops = log;
    sim->n_logged = 0;
    sim->power_failed = false;
    sim->errors_injected = 0;
    sim->silent_faults = 0;
    sim->second_error_in = 0;
    sim->call_start_op = 0;
    sim->violation[0] = 0;
    sim->viol_nbt = 0;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        alarm(BOOT_TIMEOUT_S);
        // Weak cells read afresh at power-up
        sim_weak_reroll();
        if (boot == 0) {
            baseline_main();
        } else {
            boot_main(boot, steps, n);
        }
        _exit(SIM_EXIT_OK);
    }

    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            exit(2);
        }
    }

    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        snprintf(sim->violation, sizeof(sim->violation), "[%s] boot %u step %u: process killed by %s",
                 sig == SIGALRM ? "hang" : "crash", boot, sim->step, strsignal(sig));
        return sig == SIGALRM ? CH_TIMEOUT : CH_CRASH;
    }
    switch (WEXITSTATUS(st)) {
    case SIM_EXIT_OK:        return CH_OK;
    case SIM_EXIT_POWERFAIL: return CH_POWERFAIL;
    case SIM_EXIT_VIOLATION: return CH_VIOLATION;
    }
    snprintf(sim->violation, sizeof(sim->violation), "[crash] boot %u: exit status %d", boot, WEXITSTATUS(st));
    return CH_CRASH;
}

static void restore_pristine(void) {
    memcpy(sim_flash_rw, pristine, SIM_FLASH_SIZE);
    memcpy(oracle, baseline, sizeof(oracle_t));
    sim->overwrite_count = 0;
    sim->wrong_code_count = 0;
    sim_weak_clear(1);
}

//----------------------------------------------------------------------------
// Symbolisation of backtraces via addr2line (the binary is built -no-pie)

typedef struct {
    uintptr_t addr;
    char func[64];
    char file[48];
    int  line;
} sym_t;

static sym_t *syms;
static int n_syms, cap_syms;
static char exe_path[1024];

// Symbolisation only aids reports, so a failure is a warning, given once
static void sym_warn(const char *why) {
    static bool warned;
    if (!warned) {
        warned = true;
        fprintf(stderr, "warning: cannot symbolise backtraces, so ffs call chains show '?': %s\n", why);
    }
}

static sym_t *sym_lookup(uintptr_t a) {
    for (int i = 0; i < n_syms; i++) {
        if (syms[i].addr == a) return &syms[i];
    }
    return NULL;
}

static void sym_resolve(void *const *addrs, int n) {
    uintptr_t todo[256];
    int nt = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)addrs[i];
        bool dup = sym_lookup(a) != NULL;
        for (int j = 0; j < nt && !dup; j++) {
            dup = todo[j] == a;
        }
        if (!dup && nt < (int)count_of(todo)) todo[nt++] = a;
    }
    if (!nt) return;

    FILE *fp = NULL;
    if (exe_path[0]) {
        char cmd[8192];
        int len = snprintf(cmd, sizeof(cmd), "addr2line -f -s -e '%s'", exe_path);
        for (int i = 0; i < nt; i++) {
            len += snprintf(cmd + len, sizeof(cmd) - len, " 0x%lx", (unsigned long)(todo[i] - 1));
        }
        fp = popen(cmd, "r");
        if (!fp) {
            sym_warn("popen() of addr2line failed");
        }
    } else {
        sym_warn("the path of this executable is unknown");
    }
    for (int i = 0; i < nt; i++) {
        if (n_syms == cap_syms) {
            cap_syms = cap_syms ? cap_syms * 2 : 256;
            syms = realloc(syms, cap_syms * sizeof(sym_t));
            if (!syms) {
                perror("realloc symbols");
                exit(2);
            }
        }
        sym_t *s = &syms[n_syms++];
        memset(s, 0, sizeof(*s));
        s->addr = todo[i];
        char fn[256] = "?", fl[512] = "?";
        if (fp && fgets(fn, sizeof(fn), fp) && fgets(fl, sizeof(fl), fp)) {
            fn[strcspn(fn, "\r\n")] = 0;
            char *colon = strrchr(fl, ':');
            if (colon) {
                *colon = 0;
                s->line = atoi(colon + 1);
            }
        } else if (fp) {
            sym_warn("addr2line gave too little output");
        }
        snprintf(s->func, sizeof(s->func), "%s", fn);
        snprintf(s->file, sizeof(s->file), "%s", fl);
    }
    if (fp && pclose(fp) != 0) {
        sym_warn("addr2line failed or is not installed");
    }
}

static bool is_low_level(const char *fn) {
    static const char *const low[] = {
        "call_flash_range_program", "call_flash_range_erase", "flash_program_a_page",
        "flash_erase_chunk_sectors", "flash_write_cache_flush", "flash_write_via_cache",
        "flash_write_and_flush_cache",
    };
    for (uint i = 0; i < count_of(low); i++) {
        if (strcmp(fn, low[i]) == 0) return true;
    }
    return false;
}

// Outermost-first chain of ffs.c frames, e.g. "ffs_write:760 > mark_file_as_deprecated:266"
static void chain_str(char *out, size_t len, void *const *bt, uint32_t n, bool site_only) {
    sym_resolve(bt, (int)n);
    out[0] = 0;
    size_t used = 0;
    for (int i = (int)n - 1; i >= 0; i--) {
        sym_t *s = sym_lookup((uintptr_t)bt[i]);
        if (!s || strcmp(s->file, "ffs.c") != 0 || (site_only && is_low_level(s->func))) continue;
        used += snprintf(out + used, used < len ? len - used : 0, "%s%s:%d", used ? " > " : "", s->func, s->line);
    }
    if (!out[0]) snprintf(out, len, "(not in ffs.c)");
}

//----------------------------------------------------------------------------
// Raw flash decoding, independent of ffs.c: for reports and the R8 check

static const char *hdr_state(uint32_t offs, uint16_t *len_out, uint8_t *id_out) {
    ffs_file_hdr_t h;
    memcpy(&h, sim_flash_ro + offs, sizeof(h));
    if (h.magic[0] == VIRGIN_FLASH_WORD_VALUE) return "virgin";
    if (memcmp(h.magic, file_magic, sizeof(file_magic)) != 0) return NULL;
    if (offs + HDR_SIZE + h.data_len > SIM_FLASH_SIZE) return NULL;
    if (len_out) *len_out = h.data_len;
    if (id_out)  *id_out = h.file_id;
    uint32_t crc0 = file_hdr_calc_crc0(&h, (const char *)sim_flash_ro + offs + HDR_SIZE, h.data_len);
    if (crc32_block_of_bytes(crc0, &h.crc0, sizeof(h.crc0))) return "BAD-CRC0";
    // W8: crc1 checked as if the status were valid, and the status started on its way to valid
    ffs_file_hdr_t as_valid = h;
    as_valid.status = FILE_STATUS_VALID;
    uint32_t crc1 = file_hdr_calc_crc1(&as_valid);
    if (crc32_block_of_bytes(crc1, &h.crc1, sizeof(h.crc1)) == 0 && h.status != FILE_STATUS_NASCENT &&
        (h.status & FILE_STATUS_VALID) == FILE_STATUS_VALID) {
        return "valid";
    }
    if (h.status == FILE_STATUS_NASCENT)    return "nascent";
    if (h.status == FILE_STATUS_DEPRECATED) return "deprecated";
    static char buf[32];
    snprintf(buf, sizeof(buf), "BAD-STATUS-0x%02x", h.status);
    return buf;
}

static void dump_ffs_area(char *out, size_t len) {
    size_t used = 0;
    for (uint c = 0; c < 2; c++) {
        uint32_t base = ffs_start + c * chunk_size;
        used += snprintf(out + used, len - used, "      chunk %u @0x%06x:", c, base);
        uint32_t o = base, junk_from = 0;
        bool in_junk = false;
        int items = 0;
        while (o < base + chunk_size && used < len - 200) {
            uint16_t dl = 0;
            uint8_t id = 0;
            const char *st = hdr_state(o, &dl, &id);
            bool good = st && strcmp(st, "virgin") != 0 && strncmp(st, "BAD", 3) != 0;
            // Show torn headers too
            if (!good && st && strncmp(st, "BAD", 3) == 0) good = true;
            if (in_junk && (good || (st && strcmp(st, "virgin") == 0))) {
                used += snprintf(out + used, len - used, " [junk 0x%x bytes]", o - junk_from);
                in_junk = false;
            }
            if (st && strcmp(st, "virgin") == 0) {
                // Report virgin span until next non-virgin word
                uint32_t v = o;
                while (v < base + chunk_size && *(uint32_t *)(sim_flash_ro + v) == VIRGIN_FLASH_WORD_VALUE) {
                    v += 4;
                }
                if (v >= base + chunk_size) {
                    used += snprintf(out + used, len - used, " [erased to end]");
                    break;
                }
                used += snprintf(out + used, len - used, " [erased 0x%x bytes]", v - o);
                o = v;
                continue;
            }
            if (good) {
                if (items++ % 4 == 0) used += snprintf(out + used, len - used, "\n        ");
                if (id == FILE_ID_CHUNK_HEADER) {
                    used += snprintf(out + used, len - used, " CHUNK-HDR:%s", st);
                } else {
                    used += snprintf(out + used, len - used, " id%u/len%u:%s", id, dl, st);
                }
                o += (strncmp(st, "BAD-CRC0", 8) != 0 ? ROUND_UP_TO_NEXT_32B_WORD(HDR_SIZE + dl) : 4);
                continue;
            }
            if (!in_junk) {
                in_junk = true;
                junk_from = o;
            }
            o += 4;
        }
        if (in_junk) used += snprintf(out + used, len - used, " [junk 0x%x bytes]", o - junk_from);
        used += snprintf(out + used, len - used, "\n");
    }
}

//----------------------------------------------------------------------------
// Findings

typedef struct {
    char sig[600];
    char category[64];
    char cls[24];
    char site[400];
    uint count;
    char example[8192];
} finding_t;

static finding_t *findings;
static int n_findings, cap_findings;

static sim_op_t *dry_ops;
static uint32_t n_dry_ops;

static const char *mode_class(const sim_op_t *op, pf_mode_t mode) {
    if (mode == PF_CLEAN) return "clean";
    if (mode == FE_BEFORE) return "flash-error-before";
    if (mode == FE_AFTER) return "flash-error-after";
    if (mode == FE_TWICE) return "flash-error-twice";
    if (mode == PF_MARGINAL) return op->type == OP_PROGRAM ? "weak-program" : "weak-erase";
    if (mode == SF_MISSED_BIT) return "silent-missed-bit";
    if (mode == SF_EXTRA_ZERO) return "silent-extra-zero";
    return op->type == OP_PROGRAM ? "torn-program" : "torn-erase";
}

static void get_category(char *out, size_t len, const char *msg) {
    const char *a = strchr(msg, '['), *b = a ? strchr(a, ']') : NULL;
    if (a && b) {
        snprintf(out, len, "%.*s", (int)(b - a - 1), a + 1);
    } else {
        snprintf(out, len, "unknown");
    }
}

static const char *mode_text(pf_mode_t mode, uint32_t param, const sim_op_t *op, char *buf, size_t len) {
    if (mode == PF_CLEAN) {
        snprintf(buf, len, "clean (op not started)");
    } else if (mode == FE_BEFORE) {
        snprintf(buf, len, "flash_safe_execute() returned %d without doing the op", SIM_FLASH_ERROR);
    } else if (mode == FE_AFTER) {
        snprintf(buf, len, "flash_safe_execute() did the op, then returned %d", SIM_FLASH_ERROR);
    } else if (mode == FE_TWICE) {
        snprintf(buf, len, "flash_safe_execute() returned %d without doing the op, and again %u flash call(s) later",
                 SIM_FLASH_ERROR, param);
    } else if (mode == PF_TORN_PREFIX) {
        if (op->type == OP_PROGRAM) {
            snprintf(buf, len, "torn program (only first %u of %u changing bytes landed)", param, op->changed);
        } else {
            snprintf(buf, len, "torn erase (only first %u/%u of sector erased)", param, ERASE_PARTS);
        }
    } else if (mode == SF_MISSED_BIT) {
        snprintf(buf, len, "program reported success, but one bit it should clear stayed 1 (seed %u)", param);
    } else if (mode == SF_EXTRA_ZERO) {
        snprintf(buf, len, "program reported success, but one bit it should leave 1 read 0 (seed %u)", param);
    } else if (mode == PF_MARGINAL) {
        snprintf(buf, len, "%s completed, but left %u weak bit(s) that read wrong 1 time in 4 (seed %u)",
                 op->type == OP_PROGRAM ? "program" : "erase", 1 + param % 4, param);
    } else {
        snprintf(buf, len, "torn %s (random subset of bit changes landed, seed %u)",
                 op->type == OP_PROGRAM ? "program" : "erase", param);
    }
    return buf;
}

static void record_finding(uint32_t op_idx, pf_mode_t mode, uint32_t param, long nested_op,
                           const char *msg, uint32_t viol_nbt, void *const *viol_bt,
                           const char *after_fail_dump, const char *detected_in) {
    const sim_op_t *op = &dry_ops[op_idx];
    char cat[64], site[400], sig[600];
    get_category(cat, sizeof(cat), msg);
    chain_str(site, sizeof(site), op->bt, op->n_bt, true);
    const char *cls = mode_class(op, mode);
    snprintf(sig, sizeof(sig), "%s|%s|%s%s", cls, cat, site, nested_op >= 0 ? "|nested" : "");

    for (int i = 0; i < n_findings; i++) {
        if (strcmp(findings[i].sig, sig) == 0) {
            findings[i].count++;
            return;
        }
    }
    if (n_findings == cap_findings) {
        cap_findings = cap_findings ? cap_findings * 2 : 32;
        findings = realloc(findings, cap_findings * sizeof(finding_t));
        if (!findings) {
            perror("realloc findings");
            exit(2);
        }
    }
    finding_t *f = &findings[n_findings++];
    memset(f, 0, sizeof(*f));
    snprintf(f->sig, sizeof(f->sig), "%s", sig);
    snprintf(f->category, sizeof(f->category), "%s", cat);
    snprintf(f->cls, sizeof(f->cls), "%s%s", cls, nested_op >= 0 ? "+nested" : "");
    snprintf(f->site, sizeof(f->site), "%s", site);
    f->count = 1;

    char full[600], step_desc[160], mt[160], vchain[600] = "", nested[96] = "";
    chain_str(full, sizeof(full), op->bt, op->n_bt, false);
    describe_step(step_desc, sizeof(step_desc), op->boot, op->step, pre_steps, n_pre);
    mode_text(mode, param, op, mt, sizeof(mt));
    if (viol_nbt) {
        chain_str(vchain, sizeof(vchain), viol_bt, viol_nbt, false);
    }
    if (nested_op >= 0) {
        snprintf(nested, sizeof(nested), "    Second power failure during recovery at boot-2 flash op #%ld\n",
                 nested_op);
    }
    bool fe = IS_FLASH_ERROR_MODE(mode);
    snprintf(f->example, sizeof(f->example),
             "    %s at boot-1 flash op #%u: %s 0x%06x, %s\n"
             "    API call in progress: %s\n"
             "    ffs call chain at the failing op:\n      %s\n"
             "%s"
             "    ffs area %s:\n%s"
             "    Detected in %s:\n      %s\n%s%s%s",
             fe ? "Flash error" : IS_SILENT_MODE(mode) ? "Silent program fault" : "Power failed", op_idx,
             op->type == OP_PROGRAM ? "PROGRAM page" : "ERASE sector", op->offset, mt,
             step_desc, full, nested, fe ? "at the end of boot 1" : "just after the power failure",
             after_fail_dump, detected_in, msg,
             vchain[0] ? "    ffs call chain at the offending flash op:\n      " : "", vchain, vchain[0] ? "\n" : "");
}

//----------------------------------------------------------------------------
// Hand-built flash images: states that older firmware or a double fault could leave

typedef enum {
    REC_NASCENT,
    REC_VALID,
    REC_DEPRECATED,
    REC_STATUS_VALID_CRC1_ERASED,
} rec_state_t;

// Write a record of the file straight into the simulated flash; returns the offset after it
static uint32_t craft_record(uint32_t offs, uint id, uint version, uint len, rec_state_t state) {
    static uint8_t data[MAX_FILE_LEN];
    gen_content(data, 0, version, id, PAT_RANDOM, len);
    ffs_file_hdr_t h;
    file_header_generate(&h, id, (const char *)data, len);
    if (state == REC_VALID || state == REC_DEPRECATED) {
        file_header_validate(&h);
    }
    if (state == REC_DEPRECATED) {
        h.status = FILE_STATUS_DEPRECATED;
    }
    if (state == REC_STATUS_VALID_CRC1_ERASED) {
        h.status = FILE_STATUS_VALID;
    }
    memcpy(sim_flash_rw + offs, &h, HDR_SIZE);
    memcpy(sim_flash_rw + offs + HDR_SIZE, data, len);
    return ROUND_UP_TO_NEXT_32B_WORD(offs + HDR_SIZE + len);
}

// Erased ffs area with a valid first chunk and a nascent second; returns where records go
static uint32_t craft_start(void) {
    memcpy(sim_flash_rw, pristine, ffs_start);
    memset(sim_flash_rw + ffs_start, 0xff, ffs_end - ffs_start);
    memset(oracle, 0, sizeof(*oracle));
    sim->overwrite_count = 0;
    sim->wrong_code_count = 0;
    sim_weak_clear(1);
    craft_record(ffs_start + chunk_size, FILE_ID_CHUNK_HEADER, 0, 0, REC_NASCENT);
    return craft_record(ffs_start, FILE_ID_CHUNK_HEADER, 0, 0, REC_VALID);
}

static void expect_file(uint id, uint version, uint len) {
    file_state_t *c = &oracle->committed[id];
    c->exists = true;
    c->len = (uint16_t)len;
    c->step = (uint16_t)version;
    gen_content(c->data, 0, version, id, PAT_RANDOM, len);
}

// Boot the image and run the steps, then reboot, checking every file throughout
static bool run_crafted(const char *name, const char *rule, const step_t *steps, int n) {
    const char *stage = "boot 1 (ffs_initialise() of the image, then the workload)";
    child_result_t r = spawn_boot(1, steps, n, SIM_NO_FAIL, PF_CLEAN, 0, false);
    if (r == CH_OK) {
        stage = "boot 2 (plain reboot)";
        r = spawn_boot(2, NULL, 0, SIM_NO_FAIL, PF_CLEAN, 0, false);
    }
    if (r == CH_OK && sim->overwrite_count) {
        snprintf(sim->violation, sizeof(sim->violation), "[overwrite-programmed-flash] %s", sim->overwrite_msg);
        r = CH_VIOLATION;
    }
    if (r == CH_OK) {
        printf("  passed  %-56s %s\n", name, rule);
        return true;
    }
    printf("  FAILED  %-56s %s\n          in %s: %s\n", name, rule, stage, sim->violation);
    return false;
}

static uint run_crafted_tests(void) {
    uint failed = 0;
    uint32_t o;
    printf("hand-built flash images:\n");

    o = craft_start();
    o = craft_record(o, 100, 1, 50, REC_NASCENT);
    craft_record(o, 100, 2, 60, REC_VALID);
    expect_file(100, 2, 60);
    failed += !run_crafted("stale nascent record, then a valid copy", "R4", post_steps, n_post);

    o = craft_start();
    o = craft_record(o, 101, 1, 50, REC_NASCENT);
    craft_record(o, 101, 2, 60, REC_DEPRECATED);
    failed += !run_crafted("stale nascent record, then a deleted copy", "R4", post_steps, n_post);

    o = craft_start();
    o = craft_record(o, 102, 1, 50, REC_VALID);
    craft_record(o, 102, 2, 60, REC_NASCENT);
    expect_file(102, 2, 60);
    failed += !run_crafted("valid copy, then the newest record nascent", "R3, R4", post_steps, n_post);

    o = craft_start();
    o = craft_record(o, 103, 1, 50, REC_STATUS_VALID_CRC1_ERASED);
    craft_record(o, 103, 2, 60, REC_VALID);
    expect_file(103, 2, 60);
    failed += !run_crafted("old firmware's valid status with no crc1, then a copy", "R4, W8", post_steps, n_post);

    o = craft_start();
    o = craft_record(o, 104, 1, 50, REC_NASCENT);
    craft_record(o, 104, 2, 60, REC_NASCENT);
    expect_file(104, 2, 60);
    failed += !run_crafted("two nascent records of one file", "R4", post_steps, n_post);

    o = craft_start();
    o = craft_record(o, 105, 1, 100, REC_VALID);
    // Damaged data, so initialisation swaps chunks at once
    memset(sim_flash_rw + o, 0x00, ffs_start + chunk_size - o);
    expect_file(105, 1, 100);
    failed += !run_crafted("current chunk programmed to its very end", "R7, W5", post_steps, n_post);

    craft_start();
    bool ok = run_crafted("ffs filled until writes are refused", "space check", fill_steps, n_fill);
    if (ok && oracle->n_insufficient == 0) {
        printf("  FAILED  ...no write was refused, so ffs was never full: fix the test\n");
        ok = false;
    }
    failed += !ok;

    return failed;
}

//----------------------------------------------------------------------------
// One scenario: power-fail at boot-1 op `op_idx`, reboot, recover, verify

typedef enum {
    SC_PASS,
    SC_FAIL,
    SC_NOT_REACHED,
} sc_result_t;

static sc_result_t run_scenario(uint32_t op_idx, pf_mode_t mode, uint32_t param, long nested_op) {
    restore_pristine();

    bool flash_error = IS_FLASH_ERROR_MODE(mode);
    bool silent = IS_SILENT_MODE(mode);
    child_result_t r = spawn_boot(1, pre_steps, n_pre, op_idx, mode, param, false);
    if (r == CH_OK && silent && sim->silent_faults == 0) return SC_NOT_REACHED;
    if (r == CH_OK && !silent && (!flash_error || sim->errors_injected == 0)) return SC_NOT_REACHED;

    char dump[4096] = "";
    dump_ffs_area(dump, sizeof(dump));

    const char *stage = flash_error ? "boot 1 (with the flash error)"
                      : silent      ? "boot 1 (with the silent program fault)" : "boot 1 (before the power failure)";
    if (r == CH_POWERFAIL || ((flash_error || silent) && r == CH_OK)) {
        uint boot = 2;
        r = CH_OK;
        if (nested_op >= 0) {
            r = spawn_boot(boot++, post_steps, n_post, (uint32_t)nested_op, PF_CLEAN, 0, false);
            stage = "boot 2 (recovery, interrupted by a 2nd power failure)";
            if (r == CH_OK || r == CH_POWERFAIL) {
                r = CH_OK;
            }
        }
        if (r == CH_OK) {
            stage = nested_op >= 0 ? "boot 3 (2nd recovery + continued use)" : "boot 2 (recovery + continued use)";
            r = spawn_boot(boot++, post_steps, n_post, SIM_NO_FAIL, PF_CLEAN, 0, false);
        }
        if (r == CH_OK) {
            stage = nested_op >= 0 ? "boot 4 (plain reboot)" : "boot 3 (plain reboot)";
            r = spawn_boot(boot, NULL, 0, SIM_NO_FAIL, PF_CLEAN, 0, false);
        }
    }

    if (r == CH_OK && sim->overwrite_count) {
        snprintf(sim->violation, sizeof(sim->violation),
                 "[overwrite-programmed-flash] %u attempt(s); first: %s", sim->overwrite_count, sim->overwrite_msg);
        stage = "(any boot)";
        sim->viol_nbt = 0;
        r = CH_VIOLATION;
    }

    // Reported last so that it never hides a data integrity failure
    if (r == CH_OK && sim->wrong_code_count) {
        snprintf(sim->violation, sizeof(sim->violation), "[flash-error-code-not-passed-on] %s",
                 sim->wrong_code_msg);
        stage = "boot 1 (with the flash error)";
        sim->viol_nbt = 0;
        r = CH_VIOLATION;
    }

    if (r == CH_OK) return SC_PASS;

    if (sim->overwrite_count && r == CH_VIOLATION && !strstr(sim->violation, "overwrite")) {
        size_t l = strlen(sim->violation);
        snprintf(sim->violation + l, sizeof(sim->violation) - l,
                 "\n      (also: ffs tried to overwrite programmed flash: %s)", sim->overwrite_msg);
    }
    record_finding(op_idx, mode, param, nested_op, sim->violation, sim->viol_nbt, sim->viol_bt, dump, stage);
    return SC_FAIL;
}

typedef struct {
    pf_mode_t mode;
    uint32_t  param;
} variant_t;

static void add_variant(variant_t *v, uint *nv, uint cap, pf_mode_t mode, uint32_t param) {
    if (*nv >= cap) {
        fprintf(stderr, "internal error: more than %u fault variants for one flash op\n", cap);
        exit(2);
    }
    v[(*nv)++] = (variant_t){ mode, param };
}

static void tally(sc_result_t r, uint *passed, uint *failed, uint *not_reached) {
    switch (r) {
    case SC_PASS:        (*passed)++; break;
    case SC_FAIL:        (*failed)++; break;
    case SC_NOT_REACHED: (*not_reached)++; break;
    }
}

//----------------------------------------------------------------------------
// Command line

static void usage(const char *prog) {
    printf("usage: %s [options]\n"
           "  --preload FILE        program FILE (e.g. ffsgen output) at the start of the ffs area first\n"
           "  --chunk-sectors N     flash sectors per ffs chunk (default 1, i.e. 8 KB ffs area)\n"
           "  --modes LIST          comma list of: clean,prefix,random,marginal,silent,err-before,err-after,\n"
           "                        err-twice (default all but marginal and silent). silent: a program reports\n"
           "                        success but leaves one bit set, or clears one it should not. marginal:\n"
           "                        the op completes but leaves 1-4 weak bits that read wrong now and then,\n"
           "                        as a late power cut leaves real flash. err-before/after:\n"
           "                        flash_safe_execute() returns an error before/after the op; err-twice:\n"
           "                        and again 1 to 8 flash calls later\n"
           "  --seeds N             random torn-op, weak-bit and silent-fault variants per op (default 4)\n"
           "  --prefix-samples N    max torn-prefix variants per program op (default 24)\n"
           "  --nested              also cut power a 2nd time at every op of the recovery boot (clean only)\n"
           "  --examples N          max findings to describe in detail (default all)\n"
           "  --only-op N           only test faults (of every mode) at boot-1 op N\n"
           "  --verbose             list every flash op of the fault-free run\n", prog);
}

// A whole non-negative number no bigger than max, or exit with an error
static unsigned long parse_number(const char *name, const char *arg, unsigned long max) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(arg, &end, 0);
    if (!isdigit((unsigned char)arg[0]) || *end != 0 || errno != 0 || v > max) {
        fprintf(stderr, "--%s needs a whole number from 0 to %lu, not '%s'\n", name, max, arg);
        exit(2);
    }
    return v;
}

static void parse_modes(const char *list) {
    static const struct {
        const char *name;
        bool       *flag;
    } modes[] = {
        { "clean", &opt.mode_clean },
        { "prefix", &opt.mode_prefix },
        { "random", &opt.mode_random },
        { "marginal", &opt.mode_marginal },
        { "silent", &opt.mode_silent },
        { "err-before", &opt.mode_err_before },
        { "err-after", &opt.mode_err_after },
        { "err-twice", &opt.mode_err_twice },
    };
    for (uint i = 0; i < count_of(modes); i++) {
        *modes[i].flag = false;
    }
    const char *p = list;
    for (;;) {
        size_t len = strcspn(p, ",");
        uint i = 0;
        while (i < count_of(modes) && (strlen(modes[i].name) != len || strncmp(p, modes[i].name, len) != 0)) {
            i++;
        }
        if (i == count_of(modes)) {
            fprintf(stderr, "--modes: unknown mode '%.*s'; valid modes: clean, prefix, random, marginal, silent, "
                    "err-before, err-after, err-twice\n", (int)len, p);
            exit(2);
        }
        *modes[i].flag = true;
        if (p[len] == 0) {
            break;
        }
        p += len + 1;
    }
}

static void parse_args(int argc, char **argv) {
    static struct option lo[] = {
        { "preload", required_argument, 0, 'p' },
        { "chunk-sectors", required_argument, 0, 'c' },
        { "modes", required_argument, 0, 'm' },
        { "seeds", required_argument, 0, 's' },
        { "prefix-samples", required_argument, 0, 'x' },
        { "nested", no_argument, 0, 'n' },
        { "examples", required_argument, 0, 'e' },
        { "only-op", required_argument, 0, 'o' },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 'p': opt.preload = optarg; break;
        case 'c': opt.chunk_sectors = (uint32_t)parse_number("chunk-sectors", optarg, UINT32_MAX); break;
        case 'm': parse_modes(optarg); break;
        case 's': opt.seeds = (uint32_t)parse_number("seeds", optarg, UINT32_MAX); break;
        case 'x': opt.prefix_samples = (uint32_t)parse_number("prefix-samples", optarg, UINT32_MAX); break;
        case 'n': opt.nested = true; break;
        case 'e': opt.max_examples = (int)parse_number("examples", optarg, INT_MAX); break;
        case 'o': opt.only_op = (long)parse_number("only-op", optarg, LONG_MAX); break;
        case 'v': opt.verbose = true; break;
        default: usage(argv[0]); exit(c == 'h' ? 0 : 2);
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
        exit(2);
    }
    if (opt.chunk_sectors < 1 || opt.chunk_sectors > 4) {
        fprintf(stderr, "--chunk-sectors must be 1..4\n");
        exit(2);
    }
}

static void fatal_functional(const char *what) {
    char chain[600] = "";
    if (sim->viol_nbt) chain_str(chain, sizeof(chain), sim->viol_bt, sim->viol_nbt, false);
    printf("\nFAIL: %s WITHOUT any power failure:\n  %s\n  %s\n", what, sim->violation, chain);
    exit(1);
}

//----------------------------------------------------------------------------
int main(int argc, char **argv) {
    parse_args(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);

    ssize_t el = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (el < 0) {
        el = 0;
    }
    exe_path[el] = 0;

    chunk_size = opt.chunk_sectors * FLASH_SECTOR_SIZE;
    ffs_end    = SIM_FLASH_SIZE;
    ffs_start  = ffs_end - 2 * chunk_size;
    sim_setup(ffs_start, ffs_end);

    oracle   = sim_shared_alloc(sizeof(oracle_t));
    baseline = calloc(1, sizeof(oracle_t));
    pristine = malloc(SIM_FLASH_SIZE);
    dry_ops  = calloc(SIM_MAX_OPS, sizeof(sim_op_t));
    if (!baseline || !pristine || !dry_ops) {
        perror("allocating the baseline, pristine image and op log");
        exit(2);
    }

    printf("pico_ffs power-fail harness - %s port, simulated 2 MB NOR flash (W25Q16JV geometry)\n", SIM_PORT_NAME);
    printf("ffs area 0x%06x-0x%06x: 2 chunks of %u bytes; flash read via 0x%08x\n",
           ffs_start, ffs_end, chunk_size, SIM_FLASH_READ_BASE);

    // Pristine image: "firmware" (random, non-erased bytes) below the ffs area
    uint32_t s = 0x12345678;
    for (uint32_t i = 0; i < ffs_start; i++) {
        s = hash32(s + i);
        pristine[i] = (uint8_t)s;
    }
    memset(pristine + ffs_start, 0xff, ffs_end - ffs_start);

    if (opt.preload) {
        FILE *fp = fopen(opt.preload, "rb");
        if (!fp) {
            fprintf(stderr, "cannot open preload file %s: %s\n", opt.preload, strerror(errno));
            return 2;
        }
        size_t n = fread(pristine + ffs_start, 1, chunk_size + 1, fp);
        bool read_failed = ferror(fp) != 0;
        fclose(fp);
        if (read_failed) {
            fprintf(stderr, "error reading preload file %s\n", opt.preload);
            return 2;
        }
        if (n == 0) {
            fprintf(stderr, "preload file %s is empty\n", opt.preload);
            return 2;
        }
        if (n > chunk_size) {
            fprintf(stderr, "preload file is bigger than one chunk (%u bytes)\n", chunk_size);
            return 2;
        }
        printf("preloaded %zu bytes from %s at the start of the ffs area\n", n, opt.preload);
    }

    build_workloads();

    // Baseline: what a clean boot of the pristine image contains
    memcpy(sim_flash_rw, pristine, SIM_FLASH_SIZE);
    memset(oracle, 0, sizeof(*oracle));
    if (spawn_boot(0, NULL, 0, SIM_NO_FAIL, PF_CLEAN, 0, false) != CH_OK) {
        fatal_functional("baseline boot failed");
    }
    memcpy(baseline, oracle, sizeof(oracle_t));
    uint n_base = 0;
    for (uint i = 0; i < NUM_IDS; i++) {
        n_base += baseline->committed[i].exists;
    }
    printf("baseline: %u file(s) present before the workload\n", n_base);

    // Fault-free run, logging every flash op (deterministic, so op N here is op N in every scenario)
    restore_pristine();
    if (spawn_boot(1, pre_steps, n_pre, SIM_NO_FAIL, PF_CLEAN, 0, true) != CH_OK) {
        fatal_functional("boot 1 workload failed");
    }
    n_dry_ops = sim->n_logged;
    memcpy(dry_ops, sim->ops, n_dry_ops * sizeof(sim_op_t));
    memcpy(baseline->step_note, oracle->step_note, sizeof(oracle->step_note));
    if (spawn_boot(2, post_steps, n_post, SIM_NO_FAIL, PF_CLEAN, 0, false) != CH_OK ||
        spawn_boot(3, NULL, 0, SIM_NO_FAIL, PF_CLEAN, 0, false) != CH_OK) {
        fatal_functional("continued use workload failed");
    }
    if (sim->overwrite_count) {
        snprintf(sim->violation, sizeof(sim->violation), "[overwrite-programmed-flash] %u attempt(s); first: %s",
                 sim->overwrite_count, sim->overwrite_msg);
        sim->viol_nbt = 0;
        fatal_functional("ffs overwrote programmed flash");
    }

    uint n_prog = 0, n_erase = 0;
    for (uint32_t i = 0; i < n_dry_ops; i++) {
        if (dry_ops[i].type == OP_PROGRAM) {
            n_prog++;
        } else {
            n_erase++;
        }
    }
    printf("fault-free run OK: %d boot-1 steps, %u flash ops (%u page programs, %u sector erases); "
           "%d continued-use steps\n", n_pre, n_dry_ops, n_prog, n_erase, n_post);

    // Show where chunk swaps happen in the workload
    printf("chunk swaps in the boot-1 workload:\n");
    int last_swap_step = -1;
    for (uint32_t i = 0; i < n_dry_ops; i++) {
        char chain[600], desc[160];
        chain_str(chain, sizeof(chain), dry_ops[i].bt, dry_ops[i].n_bt, true);
        if (opt.verbose) {
            describe_step(desc, sizeof(desc), 1, dry_ops[i].step, pre_steps, n_pre);
            printf("  op %4u %s 0x%06x chg %3u  %-44s %s\n", i, dry_ops[i].type == OP_PROGRAM ? "PROG " : "ERASE",
                   dry_ops[i].offset, dry_ops[i].changed, desc, chain);
        }
        bool swap = strstr(chain, "relocate_files_once") || strstr(chain, "move_existing_files_to_latest_chunk");
        if (swap && dry_ops[i].step != last_swap_step) {
            last_swap_step = dry_ops[i].step;
            describe_step(desc, sizeof(desc), 1, dry_ops[i].step, pre_steps, n_pre);
            printf("  - %s\n", desc);
        }
    }

    uint crafted_failed = run_crafted_tests();

    // Build the scenario list and run it
    uint total = 0, passed = 0, failed = 0, not_reached = 0;
    uint32_t first = 0, last = n_dry_ops;
    if (opt.only_op >= 0) {
        if (opt.only_op >= (long)n_dry_ops) {
            fprintf(stderr, "--only-op %ld is past the last boot-1 flash op (%u)\n", opt.only_op, n_dry_ops - 1);
            return 2;
        }
        first = (uint32_t)opt.only_op;
        last = first + 1;
    }

    // Clean, torn prefixes, then per seed torn random, 2 silent and marginal, then flash errors
    uint max_prefixes = opt.prefix_samples > ERASE_PARTS - 1 ? opt.prefix_samples : ERASE_PARTS - 1;
    uint64_t max_variants = 1 + (uint64_t)max_prefixes + 4 * (uint64_t)opt.seeds + 2 + FE_TWICE_MAX_GAP;
    if (max_variants > UINT_MAX / sizeof(variant_t)) {
        fprintf(stderr, "--seeds or --prefix-samples is too big\n");
        return 2;
    }
    variant_t *v = malloc((size_t)max_variants * sizeof(variant_t));
    if (!v) {
        perror("allocating fault variants");
        return 2;
    }

    printf("running power-fail and flash-error scenarios%s...\n", opt.nested ? " (with nested 2nd failure)" : "");
    for (uint32_t i = first; i < last && i < n_dry_ops; i++) {
        const sim_op_t *op = &dry_ops[i];
        uint nv = 0, cap = (uint)max_variants;

        if (opt.mode_clean) {
            add_variant(v, &nv, cap, PF_CLEAN, 0);
        }
        if (opt.mode_prefix && op->changed > 1) {
            if (op->type == OP_PROGRAM) {
                uint32_t k_max = op->changed - 1;
                uint32_t n = k_max < opt.prefix_samples ? k_max : opt.prefix_samples;
                for (uint32_t j = 1; j <= n; j++) {
                    add_variant(v, &nv, cap, PF_TORN_PREFIX, (uint32_t)((uint64_t)j * k_max / n));
                }
            } else {
                for (uint32_t k = 1; k < ERASE_PARTS; k++) {
                    add_variant(v, &nv, cap, PF_TORN_PREFIX, k);
                }
            }
        }
        if (opt.mode_random && op->changed) {
            for (uint32_t sd = 1; sd <= opt.seeds; sd++) {
                add_variant(v, &nv, cap, PF_TORN_RANDOM, sd);
            }
        }
        if (opt.mode_silent && op->type == OP_PROGRAM && op->changed) {
            for (uint32_t sd = 1; sd <= opt.seeds; sd++) {
                add_variant(v, &nv, cap, SF_MISSED_BIT, sd);
            }
            for (uint32_t sd = 1; sd <= opt.seeds; sd++) {
                add_variant(v, &nv, cap, SF_EXTRA_ZERO, sd);
            }
        }
        if (opt.mode_marginal && op->changed) {
            for (uint32_t sd = 1; sd <= opt.seeds; sd++) {
                add_variant(v, &nv, cap, PF_MARGINAL, sd);
            }
        }
        if (op->first_in_call) {
            if (opt.mode_err_before) {
                add_variant(v, &nv, cap, FE_BEFORE, 0);
            }
            if (opt.mode_err_after) {
                add_variant(v, &nv, cap, FE_AFTER, 0);
            }
            for (uint32_t k = 1; opt.mode_err_twice && k <= FE_TWICE_MAX_GAP; k++) {
                add_variant(v, &nv, cap, FE_TWICE, k);
            }
        }

        for (uint j = 0; j < nv; j++) {
            sc_result_t r = run_scenario(i, v[j].mode, v[j].param, -1);
            total++;
            tally(r, &passed, &failed, &not_reached);
        }

        if (opt.nested && opt.mode_clean) {
            // Find how many ops the recovery boot does for this failure point, then fail at each
            restore_pristine();
            if (spawn_boot(1, pre_steps, n_pre, i, PF_CLEAN, 0, false) == CH_POWERFAIL) {
                // Its op count means nothing unless it completed, so a failure counts as a failed scenario
                if (spawn_boot(2, post_steps, n_post, SIM_NO_FAIL, PF_CLEAN, 0, false) == CH_OK) {
                    uint32_t rec_ops = sim->op_count;
                    for (uint32_t m = 0; m < rec_ops; m++) {
                        sc_result_t r = run_scenario(i, PF_CLEAN, 0, (long)m);
                        total++;
                        tally(r, &passed, &failed, &not_reached);
                    }
                } else {
                    printf("\nFAIL: the recovery boot after a power failure at op %u did not complete, so no "
                           "nested scenarios were run for it: %s\n", i, sim->violation);
                    total++;
                    failed++;
                }
            }
        }

        if (!opt.verbose && (i % 50 == 49 || i + 1 == last)) {
            fprintf(stderr, "\r  op %u/%u, %u scenarios, %u failing   ", i + 1, n_dry_ops, total, failed);
        }
    }
    fprintf(stderr, "\n");
    free(v);

    // Report
    printf("\n==== RESULTS (%s) ====\n", SIM_PORT_NAME);
    printf("%u scenarios: %u passed, %u FAILED%s\n", total, passed, failed,
           not_reached ? " (some not reached)" : "");
    printf("hand-built flash images: %u FAILED\n", crafted_failed);

    if (n_findings) {
        printf("\n%d distinct failure signatures (power-fail type | what went wrong | where ffs was):\n", n_findings);
        for (int i = 0; i < n_findings; i++) {
            printf("  #%-2d %5u x  %-20s %-30s at %s\n", i + 1, findings[i].count, findings[i].cls,
                   findings[i].category, findings[i].site);
        }
        int shown = n_findings < opt.max_examples ? n_findings : opt.max_examples;
        for (int i = 0; i < shown; i++) {
            printf("\n---- #%d: %s / %s (%u scenarios) - first example ----\n%s", i + 1, findings[i].cls,
                   findings[i].category, findings[i].count, findings[i].example);
        }
    }

    if (total == 0) {
        printf("FAIL: no scenario was run (check --modes and --only-op)\n");
        return 1;
    }
    return (failed || crafted_failed) ? 1 : 0;
}

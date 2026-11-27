/**
 * Copyright (c) 2025 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "ffs_internal.h"
#include "pico/ffs.h"

#define NUMBER_OF_CHUNKS                2

#define PT_ID_LOCATION_AND_FLAGS \
    (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID)
#define PT_SINGLE_LOCATION_AND_FLAGS \
    (PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION)

// Used to mark flash offset addresses as 'not in use'
#define NOT_A_VALID_FLASH_OFFSET        0xffffffff

// The programming API uses *offsets* from the flash start.
// Flash reading (obviously) requires the actual memory address.
#ifdef PICO_RP2040
#define FL_OFFSET_TO_READ_ADDR(_o)      ((void *)(XIP_BASE + (uint32_t)(_o)))
#else // RP2350 may be using address mapping, we need to avoid remapped addresses
#define FL_OFFSET_TO_READ_ADDR(_o)      \
    ((void *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + (uint32_t)(_o)))
#endif

// Convert an unaligned flash offset into its page start offset
#define ALIGN_TO_START(_o, _a)          ((_o) & ~((_a)-1))
#define FL_OFFSET_TO_PAGE_START(_o)     ALIGN_TO_START((_o), FLASH_PAGE_SIZE)

// Word index of a record within its chunk (records are word aligned); 0, a chunk header, means none
typedef uint16_t chunk_word_t;

// The largest chunk that a chunk_word_t can index
#define MAX_CHUNK_SIZE                  (((uint32_t)1 << (8 * sizeof(chunk_word_t))) * (uint32_t)sizeof(uint32_t))

// The largest value a header field can hold
#define HDR_FIELD_MAX(_field)           ((1ull << (8 * sizeof(((ffs_file_hdr_t *)0)->_field))) - 1)

static_assert(FFS_API_MAX_WRITE_LENGTH <= HDR_FIELD_MAX(data_len), "FFS_API_MAX_WRITE_LENGTH must fit data_len");
static_assert(FILE_ID_CHUNK_HEADER <= HDR_FIELD_MAX(file_id), "every file ID must fit file_id");
static_assert(FILE_ID_CHUNK_HEADER == FFS_MAX_VALID_FILE_ID + 1, "per-file tables are sized by file ID");
static_assert(2 * sizeof(ffs_file_hdr_t) + FFS_API_MAX_WRITE_LENGTH <= MAX_CHUNK_SIZE,
              "a chunk_word_t must be able to index a chunk holding the longest file");

typedef enum {
    FILE_FOUND_VALID   = 0
  , FILE_FOUND_NASCENT = 1 // means finalisation did not complete
  , FILE_FOUND_DEPRECATED = 2
  , VIRGIN_FLASH       = 3
  , UNEXPECTED_DATA    = 4
} file_found_e;

typedef struct {
    uint32_t increment;
    uint32_t check_offset;
    uint32_t end_offset;
    file_found_e found_filter;
    file_found_e file_found;
} flash_iterator_t;

typedef struct {
    uint8_t  cached_flash_page[FLASH_PAGE_SIZE];
    uint32_t wr_page_cache_offset;
    uint32_t start_offset;
    uint32_t end_offset;
    uint32_t chunk_size;
    uint32_t current_chunk;
    uint32_t next_write;
    bool initialised;
    bool needs_recovery;    ///< a flash error may have left memory and flash out of step
} ffs_data_t;

static ffs_data_t ffs_data = { 0 };

#ifndef PICO_RP2040
// This code copied from: pico-sdk/src/rp2_common/pico_cyw43_driver/cyw43_driver.c
static int get_ffs_partition_info(void) {

    // maximum of 16 partitions, each with maximum of 4 words returned, plus 1
    uint32_t buffer[(16 * 4) + 1] = { };

    int num_p = rom_get_partition_table_info(buffer, count_of(buffer), PT_ID_LOCATION_AND_FLAGS);
    if (num_p <= 0 || buffer[0] != PT_ID_LOCATION_AND_FLAGS) {
        return PICO_ERROR_GENERIC;
    }

    int picked_p = -1;
    int p = 0;
    int i = 1;

    while (i < num_p) {
        i++;
        uint32_t flags_and_permissions = buffer[i++];
        bool has_id = (flags_and_permissions & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS);
        if (has_id) {
            uint64_t id = 0;
            id |= buffer[i++];
            id |= ((uint64_t)(buffer[i++]) << 32ull);
            if (id == FFS_DATA_PARTITION_ID) {
                picked_p = p;
                break;
            }
        }
        p++;
    }

    if (picked_p < 0) {
        return PICO_ERROR_GENERIC;
    }

    int ret = rom_get_partition_table_info(buffer, count_of(buffer), PT_SINGLE_LOCATION_AND_FLAGS | (picked_p << 24));
    if (ret != 3 || buffer[0] != PT_SINGLE_LOCATION_AND_FLAGS) {
        return PICO_ERROR_NOT_FOUND;
    }

    uint32_t loc_and_perms = buffer[1];
    ffs_data.start_offset = ((loc_and_perms >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu) * FLASH_SECTOR_SIZE;
    ffs_data.end_offset = (((loc_and_perms >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB) & 0x1fffu) + 1) * FLASH_SECTOR_SIZE;

    return PICO_OK;
}
#endif // !PICO_RP2040


static uint32_t other_chunk(uint32_t chunk_start) {

    if (chunk_start == ffs_data.start_offset) {
        return ffs_data.start_offset + ffs_data.chunk_size;
    }

    return ffs_data.start_offset;
}


// The end offset of the chunk that holds offset
static uint32_t chunk_end_of(uint32_t offset) {

    uint32_t chunk_index = (offset - ffs_data.start_offset) / ffs_data.chunk_size;
    return ffs_data.start_offset + (chunk_index + 1) * ffs_data.chunk_size;
}


// G4: whether [offset, offset + len) lies wholly within the ffs area (overflow safe)
static bool in_ffs_area(uint32_t offset, uint32_t len) {

    uint32_t ffs_size = ffs_data.end_offset - ffs_data.start_offset;

    return offset >= ffs_data.start_offset &&
        len <= ffs_size &&
        offset - ffs_data.start_offset <= ffs_size - len;
}


static void call_flash_range_erase(void *param) {

    uint32_t offset = ((uintptr_t*)param)[0];
    uint32_t length = ((uintptr_t*)param)[1];

    flash_range_erase(offset, length);
}


static void call_flash_range_program(void *param) {

    uint32_t offset = ((uintptr_t*)param)[0];
    const uint8_t *data = (const uint8_t *)((uintptr_t*)param)[1];

    flash_range_program(offset, data, FLASH_PAGE_SIZE);
}


// Returns PICO_OK, PICO_ERROR_INVALID_ADDRESS outside the ffs area (G4),
// or the error from flash_safe_execute() unchanged (G3)
static int flash_erase_chunk_sectors(uint32_t offset) {

    if (!in_ffs_area(offset, ffs_data.chunk_size)) {
        return PICO_ERROR_INVALID_ADDRESS;
    }

    uintptr_t params[] = { offset, ffs_data.chunk_size };
    return flash_safe_execute(call_flash_range_erase, params, UINT32_MAX);
}


// Returns PICO_OK, PICO_ERROR_INVALID_ADDRESS outside the ffs area (G4),
// or the error from flash_safe_execute() unchanged (G3)
static int flash_program_a_page(uint32_t offset, const void *page) {

    if (!in_ffs_area(offset, FLASH_PAGE_SIZE)) {
        return PICO_ERROR_INVALID_ADDRESS;
    }

    uintptr_t params[] = { offset, (uintptr_t)page };
    return flash_safe_execute(call_flash_range_program, params, UINT32_MAX);
}


// W1: unwritten bytes are 0xff, which leaves flash as it is. Reading the page back instead could
// copy a weak bit, left by an interrupted program, into flash for good.
static inline void flash_clear_cache(void) {
    memset(ffs_data.cached_flash_page, 0xff, FLASH_PAGE_SIZE);
}


// Programs the cached page, if any, and always leaves the cache empty
static int flash_write_cache_flush(void) {

    int rc = PICO_OK;

    if (ffs_data.wr_page_cache_offset != NOT_A_VALID_FLASH_OFFSET) {
        rc = flash_program_a_page(ffs_data.wr_page_cache_offset, ffs_data.cached_flash_page);
        ffs_data.wr_page_cache_offset = NOT_A_VALID_FLASH_OFFSET;
    }

    return rc;
}


// Advances *ptr_to_wr_offset as it writes, and may leave data cached until flash_write_cache_flush().
// On an error it stops at once and leaves the cache empty.
static int flash_write_via_cache(uint32_t *ptr_to_wr_offset,
                                 const void *data,
                                 uint16_t length) {

    const uint8_t *rd_ptr = (const uint8_t *)data;

    while (length != 0) {

        uint32_t flash_page_start = FL_OFFSET_TO_PAGE_START(*ptr_to_wr_offset);

        // No page cached yet: start caching this one
        if (ffs_data.wr_page_cache_offset == NOT_A_VALID_FLASH_OFFSET) {
            flash_clear_cache();
            ffs_data.wr_page_cache_offset = flash_page_start;
        }

        // The offset is in another page: flush the cached one first
        else if (flash_page_start != ffs_data.wr_page_cache_offset) {
            int rc = flash_write_cache_flush();
            if (rc != PICO_OK) {
                return rc;
            }
            continue;
        }

        // copy 1 character of data into the flash page holding buffer
        ffs_data.cached_flash_page[*ptr_to_wr_offset % FLASH_PAGE_SIZE] = *rd_ptr++;
        (*ptr_to_wr_offset)++;
        length--;
    }

    return PICO_OK;
}


// NOTE: This function automatically advances the write address used as
// data is written so that callers can easily store consecutive items.
// This function will program data into flash leaving the cache clear.
static int flash_write_and_flush_cache(uint32_t *ptr_to_wr_offset,
                                       const void *data,
                                       uint16_t length) {

    int rc = flash_write_via_cache(ptr_to_wr_offset, data, length);
    if (rc != PICO_OK) {
        return rc;
    }

    return flash_write_cache_flush();
}


static file_found_e record_state(uint32_t offset);


// R2: mark a file header valid, crc1 first, then status, so an interrupted crc1 program leaves it
// nascent and an interrupted status program leaves it valid (W8). The status is never set if crc1 failed.
static int commit_file_header(uint32_t header_offset, ffs_file_hdr_t *phdr) {

    file_header_validate(phdr);

    // W9: a program can fail without reporting it, so check the record is now valid; try once more
    for (int attempt = 0; attempt < 2; attempt++) {

        uint32_t crc1_offset = header_offset + OFFSET_OF_HDR_CRC1_WORD();
        int rc = flash_write_and_flush_cache(&crc1_offset, &phdr->crc1, sizeof(phdr->crc1));
        if (rc != PICO_OK) {
            return rc;
        }

        uint32_t status_offset = header_offset + OFFSET_OF_HDR_STATUS_BYTE();
        rc = flash_write_and_flush_cache(&status_offset, &phdr->status, sizeof(phdr->status));
        if (rc != PICO_OK) {
            return rc;
        }

        if (record_state(header_offset) == FILE_FOUND_VALID) {
            return PICO_OK;
        }
    }

    return PICO_ERROR_IO;
}


static int mark_file_as_deprecated(uint32_t file_offset) {

    uint32_t status_offset = file_offset + OFFSET_OF_HDR_STATUS_BYTE();
    const uint8_t status = FILE_STATUS_DEPRECATED;

    // W9: check the status reads back as deprecated; try once more
    for (int attempt = 0; attempt < 2; attempt++) {

        uint32_t wr_offset = status_offset;
        int rc = flash_write_and_flush_cache(&wr_offset, &status, sizeof(status));
        if (rc != PICO_OK) {
            return rc;
        }

        if (*(const volatile uint8_t *)FL_OFFSET_TO_READ_ADDR(status_offset) == FILE_STATUS_DEPRECATED) {
            return PICO_OK;
        }
    }

    return PICO_ERROR_IO;
}


static file_found_e check_for_file_at_offset(uint32_t offset) {

    ffs_file_hdr_t *pfile_hdr = (ffs_file_hdr_t *)FL_OFFSET_TO_READ_ADDR(offset);
    if (pfile_hdr->magic[0] == VIRGIN_FLASH_WORD_VALUE) {
        return VIRGIN_FLASH;
    }

    // R1: a record never crosses the end of its chunk, so nor may anything read as one
    uint32_t chunk_end = chunk_end_of(offset);
    if (chunk_end - offset < (uint32_t)sizeof(ffs_file_hdr_t)) {
        return UNEXPECTED_DATA;
    }

    if (memcmp(&pfile_hdr->magic, file_magic, sizeof(file_magic)) != 0) {
        return UNEXPECTED_DATA;
    }

    // A damaged length must not take the crc0 check past the end of the chunk
    uint32_t data_len = pfile_hdr->data_len;
    if (data_len > chunk_end - offset - (uint32_t)sizeof(ffs_file_hdr_t)) {
        return UNEXPECTED_DATA;
    }

    // Calculate the crc0 value which comprises the header up
    // until the status field and also the actual file data.
    void *data = (void *)((int)pfile_hdr + sizeof(ffs_file_hdr_t));
    uint32_t crc0 = file_hdr_calc_crc0(pfile_hdr, data, data_len);

    // check crc0, the "residue" should be 0 with an valid header & data, ignoring file status
    if (crc32_block_of_bytes(crc0, &pfile_hdr->crc0, sizeof(pfile_hdr->crc0)) != 0) {
        return UNEXPECTED_DATA;
    }

    // W8: crc1, computed with a valid status, is the commit mark. Once any status bit is cleared crc1
    // was complete, so a status part-way to valid counts as valid and weak status bits can't matter.
    uint8_t status = pfile_hdr->status;
    ffs_file_hdr_t as_valid;
    memcpy(&as_valid, pfile_hdr, sizeof(as_valid));
    as_valid.status = FILE_STATUS_VALID;
    uint32_t crc1 = file_hdr_calc_crc1(&as_valid);

    if (crc32_block_of_bytes(crc1, &pfile_hdr->crc1, sizeof(pfile_hdr->crc1)) == 0 &&
        status != FILE_STATUS_NASCENT && (status & FILE_STATUS_VALID) == FILE_STATUS_VALID) {
        return FILE_FOUND_VALID;
    }

    // Good crc0 but not valid: a partly programmed status still shows its transition.
    // All VALID bits set: nascent, or validation incomplete.
    if ((status & FILE_STATUS_VALID) == FILE_STATUS_VALID) {
        return FILE_FOUND_NASCENT;
    }

    // Only VALID bits set: deprecated, or deprecation incomplete
    if ((status & (uint8_t)~FILE_STATUS_VALID) == 0) {
        return FILE_FOUND_DEPRECATED;
    }

    return UNEXPECTED_DATA;
}


static file_found_e record_state(uint32_t offset) {
    return check_for_file_at_offset(offset);
}


static void *flash_iterator_get_next_file(flash_iterator_t *iterator) {

    while (1) {

        iterator->check_offset += iterator->increment;  // Note: must be 0 first time
        iterator->check_offset = ROUND_UP_TO_NEXT_32B_WORD(iterator->check_offset);

        if (iterator->check_offset >= iterator->end_offset) {
            return NULL;
        }

        ffs_file_hdr_t *pfile_hdr = (ffs_file_hdr_t *)FL_OFFSET_TO_READ_ADDR(iterator->check_offset);
        iterator->file_found = check_for_file_at_offset(iterator->check_offset);

        // Vital: Set increment value for next loop round *before* the (possible) return of file ptr
        if (iterator->file_found == FILE_FOUND_NASCENT ||
            iterator->file_found == FILE_FOUND_VALID ||
            iterator->file_found == FILE_FOUND_DEPRECATED) {
            // file checked well enough to trust the data length field
            iterator->increment = sizeof(ffs_file_hdr_t) + pfile_hdr->data_len;
        }
        else {
            iterator->increment = 4;
        }

        // If the caller is sufficiently interested, return them the header pointer
        if (iterator->found_filter >= iterator->file_found) {
            return pfile_hdr;
        }
    }
}


// The last (newest) valid record of the file in the chunk, or NOT_A_VALID_FLASH_OFFSET
static uint32_t newest_valid_record_in(uint32_t chunk_start, uint file_id) {

    flash_iterator_t iterator;
    iterator.check_offset = chunk_start;
    iterator.end_offset   = chunk_start + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_VALID;
    ffs_file_hdr_t *pfile_hdr;
    uint32_t newest = NOT_A_VALID_FLASH_OFFSET;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {
        if (pfile_hdr->file_id == file_id) {
            newest = iterator.check_offset;
        }
    }

    return newest;
}


// W3: the newest valid record of the file, in the current chunk or else the other one
static uint32_t search_for_specified_valid_file(uint file_id) {

    uint32_t offset = newest_valid_record_in(ffs_data.current_chunk, file_id);
    if (offset == NOT_A_VALID_FLASH_OFFSET) {
        offset = newest_valid_record_in(other_chunk(ffs_data.current_chunk), file_id);
    }

    return offset;
}


// R5: deprecate every valid and nascent record of the file.
// Returns PICO_ERROR_NOT_FOUND if there was no valid one.
static int deprecate_all_records_of(uint file_id) {

    flash_iterator_t iterator;
    iterator.check_offset = ffs_data.start_offset; // All Flash
    iterator.end_offset   = ffs_data.end_offset;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_NASCENT;    // valid and nascent
    ffs_file_hdr_t *pfile_hdr;
    bool found_valid = false;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {

        if (pfile_hdr->file_id != file_id) {
            continue;
        }

        if (iterator.file_found == FILE_FOUND_VALID) {
            found_valid = true;
        }

        int rc = mark_file_as_deprecated(iterator.check_offset);
        if (rc != PICO_OK) {
            return rc;
        }
    }

    return found_valid ? PICO_OK : PICO_ERROR_NOT_FOUND;
}


// R8: after a failed write, deprecate the new record unless the old one has already gone.
// A failure here is not reported: the caller returns the first error, and G2 still holds.
static void abandon_new_record(uint32_t new_offset, uint32_t old_offset) {

    // R4: with the old copy gone, keep the new record so that initialisation validates it
    if (old_offset != NOT_A_VALID_FLASH_OFFSET && record_state(old_offset) != FILE_FOUND_VALID) {
        return;
    }

    if (record_state(new_offset) == FILE_FOUND_NASCENT) {
        (void)mark_file_as_deprecated(new_offset);
    }
}


static int write_new_file_to_flash(uint file_id, const char *wrdata, uint data_len) {

    ffs_file_hdr_t hdr_buff;
    file_header_generate(&hdr_buff, file_id, wrdata, data_len);
    uint32_t old_offset = search_for_specified_valid_file(file_id);
    uint32_t header_offset;
    int rc;

    for (int attempt = 0; ; attempt++) {

        // The file header structure contains words, it must start on a word boundary
        ffs_data.next_write = ROUND_UP_TO_NEXT_32B_WORD(ffs_data.next_write);

        // R1: the whole record must lie within the current chunk
        header_offset = ffs_data.next_write;
        uint32_t record_end = header_offset + (uint32_t)sizeof(ffs_file_hdr_t) + data_len;
        if (header_offset < ffs_data.current_chunk ||
            record_end > ffs_data.current_chunk + ffs_data.chunk_size) {
            return PICO_ERROR_INSUFFICIENT_RESOURCES;
        }

        // W2: the magic alone first. If a power cut leaves no trace of it, the next record programs the
        // same bits here again; any other interruption leaves a visible record for start-up to repair.
        uint32_t magic_offset = header_offset;
        rc = flash_write_and_flush_cache(&magic_offset, &hdr_buff.magic, sizeof(hdr_buff.magic));

        // Step 1: Write file header to flash including crc0 with a 'nascent' status, then the data
        if (rc == PICO_OK) {
            rc = flash_write_via_cache(&ffs_data.next_write, &hdr_buff, sizeof(ffs_file_hdr_t));
        }
        if (rc == PICO_OK) {
            rc = flash_write_and_flush_cache(&ffs_data.next_write, wrdata, data_len);
        }
        if (rc != PICO_OK || record_state(header_offset) == FILE_FOUND_NASCENT) {
            break;
        }

        // W9: the record does not read back intact. Leave it as damaged data and write it once more after it.
        rc = PICO_ERROR_IO;
        if (attempt == 1) {
            return rc;
        }
    }

    // Step 2 (R3): deprecate the previous version before the new one is made valid
    if (rc == PICO_OK && old_offset != NOT_A_VALID_FLASH_OFFSET) {
        rc = mark_file_as_deprecated(old_offset);
    }

    // Step 3: Finalise new file by setting crc1 / marking it as 'valid' status
    if (rc == PICO_OK) {
        rc = commit_file_header(header_offset, &hdr_buff);
    }

    if (rc != PICO_OK) {
        abandon_new_record(header_offset, old_offset);
    }

    return rc;
}


static int chunk_erase_and_set_nascent_header(uint32_t offset) {

    ffs_file_hdr_t hdr_buff;
    file_header_generate(&hdr_buff, FILE_ID_CHUNK_HEADER, NULL, 0);

    // W9: check the header reads back intact; erase and try once more, as a bit may be stuck at 0
    for (int attempt = 0; attempt < 2; attempt++) {
        int rc = flash_erase_chunk_sectors(offset);
        if (rc != PICO_OK) {
            return rc;
        }
        uint32_t wr_offset = offset;
        rc = flash_write_and_flush_cache(&wr_offset, &hdr_buff, sizeof(ffs_file_hdr_t));
        if (rc != PICO_OK || record_state(offset) == FILE_FOUND_NASCENT) {
            return rc;
        }
    }

    return PICO_ERROR_IO;
}


// W6: programs the whole header again before validating it, so that any bit
// left weak by an interrupted header write becomes solid
static int chunk_set_header_as_valid(uint32_t offset) {

    ffs_file_hdr_t hdr_buff;
    file_header_generate(&hdr_buff, FILE_ID_CHUNK_HEADER, NULL, 0);

    uint32_t wr_offset = offset;
    int rc = flash_write_and_flush_cache(&wr_offset, &hdr_buff, sizeof(ffs_file_hdr_t));
    if (rc != PICO_OK) {
        return rc;
    }

    return commit_file_header(offset, &hdr_buff);
}


// Whether the chunk holds any intact record besides its header
static bool chunk_has_records(uint32_t chunk_start) {

    flash_iterator_t iterator;
    iterator.check_offset = chunk_start;
    iterator.end_offset   = chunk_start + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_DEPRECATED;
    ffs_file_hdr_t *pfile_hdr;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {
        if (pfile_hdr->file_id != FILE_ID_CHUNK_HEADER) {
            return true;
        }
    }

    return false;
}


// C4: find the current chunk, erasing damaged chunk headers and validating the new chunk of an
// interrupted swap. Sets *complete_file_move if the caller must copy files from the old chunk.
static int find_or_make_a_valid_chunk(uint32_t *valid_chunk, bool *complete_file_move) {

    *complete_file_move = false;

    uint32_t chunk[NUMBER_OF_CHUNKS] = { ffs_data.start_offset, ffs_data.start_offset + ffs_data.chunk_size };
    file_found_e found[NUMBER_OF_CHUNKS];

    for (int i = 0; i < NUMBER_OF_CHUNKS; i++) {

        found[i] = record_state(chunk[i]);

        if (found[i] != FILE_FOUND_DEPRECATED &&
            found[i] != FILE_FOUND_NASCENT &&
            found[i] != FILE_FOUND_VALID) {
            int rc = chunk_erase_and_set_nascent_header(chunk[i]);
            if (rc != PICO_OK) {
                return rc;
            }
            found[i] = FILE_FOUND_NASCENT;
        }
    }

    // Empty ffs: make the first chunk valid. W7: unless only the second one holds records, as when
    // the current chunk's header was left weak and reads nascent.
    if (found[0] == FILE_FOUND_NASCENT && found[1] == FILE_FOUND_NASCENT) {
        int pick = (!chunk_has_records(chunk[0]) && chunk_has_records(chunk[1])) ? 1 : 0;
        *valid_chunk = chunk[pick];
        return chunk_set_header_as_valid(chunk[pick]);
    }

    for (int i = 0; i < NUMBER_OF_CHUNKS; i++) {

        file_found_e other = found[i ^ 1];

        // Valid & Nascent: normal
        if (found[i] == FILE_FOUND_VALID && other == FILE_FOUND_NASCENT) {
            *valid_chunk = chunk[i];
            return PICO_OK;
        }

        // Valid & Deprecated: swap interrupted after the new chunk was validated (W6: perhaps weakly)
        if (found[i] == FILE_FOUND_VALID && other == FILE_FOUND_DEPRECATED) {
            *valid_chunk = chunk[i];
            *complete_file_move = true;
            return chunk_set_header_as_valid(chunk[i]);
        }

        // Nascent & Deprecated: swap interrupted before the new chunk was validated, or R8 erased it
        if (found[i] == FILE_FOUND_NASCENT && other == FILE_FOUND_DEPRECATED) {
            *valid_chunk = chunk[i];
            *complete_file_move = true;
            return chunk_set_header_as_valid(chunk[i]);
        }
    }

    // Valid & Valid or Deprecated & Deprecated: an invariant is broken, so erase nothing
    return PICO_ERROR_INVALID_DATA;
}


// Returns the offset just beyond the last programmed (not 0xffffffff) word in the chunk
static uint32_t find_end_of_programmed_flash(uint32_t chunk_start) {

    const uint32_t *chunk_words = (const uint32_t *)FL_OFFSET_TO_READ_ADDR(chunk_start);
    uint32_t num_words = ffs_data.chunk_size / sizeof(uint32_t);

    while (num_words != 0 && chunk_words[num_words - 1] == VIRGIN_FLASH_WORD_VALUE) {
        num_words--;
    }

    return chunk_start + num_words * (uint32_t)sizeof(uint32_t);
}


// R7: the next record goes after the last programmed byte in the chunk, never into
// an erased gap within damaged data. The result is at most the end of the chunk.
static uint32_t get_next_flash_write_offset(uint32_t chunk_start) {

    flash_iterator_t iterator;
    iterator.check_offset = chunk_start;
    iterator.end_offset   = chunk_start + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = VIRGIN_FLASH;
    uint32_t programmed_end = find_end_of_programmed_flash(chunk_start);

    while (flash_iterator_get_next_file(&iterator) != NULL) {

        if (iterator.file_found == VIRGIN_FLASH && iterator.check_offset >= programmed_end) {
            return iterator.check_offset;
        }
    }

    return iterator.end_offset;
}


// W4: copies the record at src into the chunk at *wr_offset with its original crc0, checks the
// copy against that crc0 (a weak source bit may have read wrong), then validates it
static int copy_record_checked(uint32_t src, uint32_t *wr_offset, uint32_t chunk_end) {

    const char *data = (const char *)FL_OFFSET_TO_READ_ADDR(src + (uint32_t)sizeof(ffs_file_hdr_t));

    for (int attempt = 0; attempt < 3; attempt++) {

        // A header snapshot that the source, read again, still agrees with
        ffs_file_hdr_t hdr_buff;
        memcpy(&hdr_buff, FL_OFFSET_TO_READ_ADDR(src), sizeof(ffs_file_hdr_t));
        file_found_e state = record_state(src);
        if ((state != FILE_FOUND_VALID && state != FILE_FOUND_NASCENT) ||
            memcmp(&hdr_buff, FL_OFFSET_TO_READ_ADDR(src), OFFSET_OF_HDR_STATUS_BYTE()) != 0) {
            continue;
        }
        hdr_buff.status = FILE_STATUS_NASCENT;
        hdr_buff.crc1 = VIRGIN_FLASH_WORD_VALUE;

        uint32_t copy = ROUND_UP_TO_NEXT_32B_WORD(*wr_offset);
        if (copy + (uint32_t)sizeof(ffs_file_hdr_t) + hdr_buff.data_len > chunk_end) {
            return PICO_ERROR_INSUFFICIENT_RESOURCES;
        }

        *wr_offset = copy;
        int rc = flash_write_via_cache(wr_offset, &hdr_buff, sizeof(ffs_file_hdr_t));
        if (rc == PICO_OK) {
            rc = flash_write_and_flush_cache(wr_offset, data, hdr_buff.data_len);
        }
        if (rc != PICO_OK) {
            return rc;
        }

        if (record_state(copy) == FILE_FOUND_NASCENT) {
            return commit_file_header(copy, &hdr_buff);
        }
        // The copy does not match its crc0: abandon it and read the source again
        rc = mark_file_as_deprecated(copy);
        if (rc != PICO_OK) {
            return rc;
        }
    }

    return PICO_ERROR_INVALID_DATA;
}


// Writes a new record from RAM into the chunk at *wr_offset, checks it, then validates it
static int write_record_checked(uint file_id, const char *wrdata, uint data_len,
                                uint32_t *wr_offset, uint32_t chunk_end) {

    ffs_file_hdr_t hdr_buff;
    file_header_generate(&hdr_buff, file_id, wrdata, data_len);

    // W9: if the record does not read back intact, write it once more after it
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t header_offset = ROUND_UP_TO_NEXT_32B_WORD(*wr_offset);
        if (header_offset + (uint32_t)sizeof(ffs_file_hdr_t) + data_len > chunk_end) {
            return PICO_ERROR_INSUFFICIENT_RESOURCES;
        }
        *wr_offset = header_offset;
        int rc = flash_write_via_cache(wr_offset, &hdr_buff, sizeof(ffs_file_hdr_t));
        if (rc == PICO_OK) {
            rc = flash_write_and_flush_cache(wr_offset, wrdata, data_len);
        }
        if (rc != PICO_OK) {
            return rc;
        }
        if (record_state(header_offset) == FILE_FOUND_NASCENT) {
            return commit_file_header(header_offset, &hdr_buff);
        }
    }

    return PICO_ERROR_IO;
}


// File IDs per window of a chunk swap: more takes more stack, but fewer scans when IDs are spread
#define SURVIVOR_WINDOW_IDS             32
static_assert(SURVIVOR_WINDOW_IDS % 32 == 0, "windows are made of whole bitmap words");

// The surviving record of each file in a window of file IDs, by R3, R4 and R5: its last intact
// record. If that is not valid and cannot be copied, copy_survivor() takes the newest valid one (R8).
typedef struct {
    uint first_id;
    chunk_word_t word[SURVIVOR_WINDOW_IDS];
} survivors_t;

static bool id_in_window(const survivors_t *s, uint id) {
    return id >= s->first_id && id - s->first_id < SURVIVOR_WINDOW_IDS && id <= FFS_MAX_VALID_FILE_ID;
}


// Chooses the survivors in s's window; if present is not NULL, also marks every file ID in the chunk
static void choose_surviving_records(uint32_t chunk_start, survivors_t *s, uint32_t *present) {

    memset(s->word, 0, sizeof(s->word));

    flash_iterator_t iterator;
    iterator.check_offset = chunk_start;
    iterator.end_offset   = chunk_start + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_DEPRECATED;
    ffs_file_hdr_t *pfile_hdr;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {
        uint id = pfile_hdr->file_id;
        if (present != NULL && id <= FFS_MAX_VALID_FILE_ID) {
            present[id / 32] |= 1u << (id % 32);
        }
        if (id_in_window(s, id)) {
            s->word[id - s->first_id] = (chunk_word_t)((iterator.check_offset - chunk_start) / sizeof(uint32_t));
        }
    }
}


// Whether any file ID in the window starting at first_id is marked in present
static bool window_has_files(const uint32_t *present, uint first_id) {

    for (uint w = first_id / 32; w < (first_id + SURVIVOR_WINDOW_IDS) / 32 && w * 32 <= FFS_MAX_VALID_FILE_ID; w++) {
        if (present[w] != 0) {
            return true;
        }
    }

    return false;
}


// The flash offset of a word index in chunk_start, or NOT_A_VALID_FLASH_OFFSET for none
static uint32_t survivor_of(uint32_t chunk_start, chunk_word_t word) {
    return word == 0 ? NOT_A_VALID_FLASH_OFFSET : chunk_start + word * (uint32_t)sizeof(uint32_t);
}


// W4, R8: copies a file's surviving record from chunk. A deprecated record cannot be copied, nor
// can an unreadable nascent one, which was never committed: then the newest valid one is, if any.
static int copy_survivor(uint32_t chunk, uint file_id, uint32_t chosen, uint32_t *wr_offset, uint32_t chunk_end) {

    bool not_valid = record_state(chosen) != FILE_FOUND_VALID;
    int rc = copy_record_checked(chosen, wr_offset, chunk_end);
    if (rc == PICO_ERROR_INVALID_DATA && not_valid) {
        uint32_t valid = newest_valid_record_in(chunk, file_id);
        rc = valid == NOT_A_VALID_FLASH_OFFSET ? PICO_OK : copy_record_checked(valid, wr_offset, chunk_end);
    }

    return rc;
}


// C4: complete a move-after swap: copy each file's surviving record (R4) from the old chunk unless
// the current chunk holds a newer valid or nascent one, then erase the old chunk only if all copied (C3)
static int move_existing_files_to_latest_chunk(void) {

    uint32_t old_chunk = other_chunk(ffs_data.current_chunk);
    uint32_t chunk_end = ffs_data.current_chunk + ffs_data.chunk_size;

    // Files with a valid or nascent record in the current chunk, which is newer than the old one
    uint32_t in_current[(FFS_MAX_VALID_FILE_ID + 32) / 32];
    memset(in_current, 0, sizeof(in_current));

    flash_iterator_t iterator;
    iterator.check_offset = ffs_data.current_chunk;
    iterator.end_offset   = chunk_end;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_NASCENT;
    ffs_file_hdr_t *pfile_hdr;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {
        uint id = pfile_hdr->file_id;
        if (id <= FFS_MAX_VALID_FILE_ID) {
            in_current[id / 32] |= 1u << (id % 32);
        }
    }

    uint32_t present[(FFS_MAX_VALID_FILE_ID + 32) / 32];
    memset(present, 0, sizeof(present));
    survivors_t survivors;

    for (uint first = 0; first <= FFS_MAX_VALID_FILE_ID; first += SURVIVOR_WINDOW_IDS) {
        if (first != 0 && !window_has_files(present, first)) {
            continue;
        }
        survivors.first_id = first;
        choose_surviving_records(old_chunk, &survivors, first == 0 ? present : NULL);

        for (uint i = 0; i < SURVIVOR_WINDOW_IDS; i++) {
            uint id = first + i;
            uint32_t chosen = survivor_of(old_chunk, survivors.word[i]);
            if (chosen == NOT_A_VALID_FLASH_OFFSET || (in_current[id / 32] & (1u << (id % 32)))) {
                continue;
            }
            int rc = copy_survivor(old_chunk, id, chosen, &ffs_data.next_write, chunk_end);
            if (rc != PICO_OK) {
                return rc;
            }
        }
    }

    ffs_data.next_write = ROUND_UP_TO_NEXT_32B_WORD(ffs_data.next_write);

    return chunk_erase_and_set_nascent_header(old_chunk);
}


// W4: move the surviving files, and optionally a new one, from the current chunk into the other,
// copy first and switch after, so that an interruption never leaves the current chunk short
static int relocate_files_once(bool with_new, uint new_id, const char *new_data, uint new_len) {

    uint32_t source = ffs_data.current_chunk;
    uint32_t target = other_chunk(source);
    uint32_t target_end = target + ffs_data.chunk_size;

    // The target was erased at the end of the last swap; erase it again only if anything was written since
    int rc = PICO_OK;
    if (record_state(target) != FILE_FOUND_NASCENT ||
        find_end_of_programmed_flash(target) > target + (uint32_t)sizeof(ffs_file_hdr_t)) {
        rc = chunk_erase_and_set_nascent_header(target);
    }
    uint32_t wr_offset = target + (uint32_t)sizeof(ffs_file_hdr_t);

    // One window of file IDs at a time; the first window's scan also finds which windows have files
    uint32_t present[(FFS_MAX_VALID_FILE_ID + 32) / 32];
    memset(present, 0, sizeof(present));
    survivors_t survivors;

    for (uint first = 0; rc == PICO_OK && first <= FFS_MAX_VALID_FILE_ID; first += SURVIVOR_WINDOW_IDS) {
        if (first != 0 && !window_has_files(present, first)) {
            continue;
        }
        survivors.first_id = first;
        choose_surviving_records(source, &survivors, first == 0 ? present : NULL);
        if (with_new && id_in_window(&survivors, new_id)) {
            survivors.word[new_id - first] = 0;
        }

        for (uint i = 0; rc == PICO_OK && i < SURVIVOR_WINDOW_IDS; i++) {
            uint32_t chosen = survivor_of(source, survivors.word[i]);
            if (chosen != NOT_A_VALID_FLASH_OFFSET) {
                rc = copy_survivor(source, first + i, chosen, &wr_offset, target_end);
            }
        }
    }
    if (rc == PICO_OK && with_new) {
        rc = write_record_checked(new_id, new_data, new_len, &wr_offset, target_end);
    }

    // Switch: C2 steps 3 and 4, then the old chunk is erased (C3)
    if (rc == PICO_OK) {
        rc = mark_file_as_deprecated(source);
    }
    if (rc == PICO_OK) {
        rc = chunk_set_header_as_valid(target);
    }
    if (rc != PICO_OK) {
        // R8: a failed write must not appear, so damage the target's header; recovery then erases it
        // and rebuilds from the source, whose records W4 never changes. A failure here is not reported.
        if (with_new) {
            const uint32_t zeros[2] = { 0, 0 };
            uint32_t header_offset = target;
            (void)flash_write_and_flush_cache(&header_offset, zeros, sizeof(zeros));
        }
        return rc;
    }

    ffs_data.current_chunk = target;
    ffs_data.next_write = ROUND_UP_TO_NEXT_32B_WORD(wr_offset);

    return chunk_erase_and_set_nascent_header(source);
}


// W9: a swap that fails a check before the switch (perhaps a bit stuck at 0 in the target) is tried
// once more into the target erased again; W4 has not changed the source
static int relocate_files(bool with_new, uint new_id, const char *new_data, uint new_len) {

    uint32_t source = ffs_data.current_chunk;
    int rc = relocate_files_once(with_new, new_id, new_data, new_len);
    if (rc == PICO_ERROR_IO && ffs_data.current_chunk == source) {
        rc = chunk_erase_and_set_nascent_header(other_chunk(source));
        if (rc == PICO_OK) {
            rc = relocate_files_once(with_new, new_id, new_data, new_len);
        }
    }

    return rc;
}


// W5: whether an interrupted program may have left weak bits that ffs relies on: a nascent record,
// junk, or two valid copies of a file in the current chunk, or anything written in the spare chunk
static bool interrupted_program_evident(void) {

    uint32_t chunk_start = ffs_data.current_chunk;
    uint32_t programmed_end = find_end_of_programmed_flash(chunk_start);

    // Files with a valid record already seen, so that one pass finds any second valid copy
    uint32_t seen_valid[(FFS_MAX_VALID_FILE_ID + 32) / 32];
    memset(seen_valid, 0, sizeof(seen_valid));

    flash_iterator_t iterator;
    iterator.check_offset = chunk_start;
    iterator.end_offset   = programmed_end;
    iterator.increment    = 0;
    iterator.found_filter = UNEXPECTED_DATA;
    ffs_file_hdr_t *pfile_hdr;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {
        if (iterator.file_found == UNEXPECTED_DATA || iterator.file_found == VIRGIN_FLASH) {
            return true;
        }
        uint id = pfile_hdr->file_id;
        if (id == FILE_ID_CHUNK_HEADER) {
            continue;
        }
        if (iterator.file_found == FILE_FOUND_NASCENT) {
            return true;
        }
        if (iterator.file_found == FILE_FOUND_VALID) {
            uint32_t bit = 1u << (id % 32);
            if (seen_valid[id / 32] & bit) {
                return true;
            }
            seen_valid[id / 32] |= bit;
        }
    }

    uint32_t spare = other_chunk(chunk_start);
    return find_end_of_programmed_flash(spare) > spare + (uint32_t)sizeof(ffs_file_hdr_t);
}


// R6: fully program any partly programmed deprecated status, as marginal bits may later read as valid
static int complete_interrupted_deprecations(void) {

    flash_iterator_t iterator;
    iterator.check_offset = ffs_data.start_offset; // All Flash
    iterator.end_offset   = ffs_data.end_offset;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_DEPRECATED;
    ffs_file_hdr_t *pfile_hdr;

    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {

        if (iterator.file_found == FILE_FOUND_DEPRECATED &&
            pfile_hdr->status != FILE_STATUS_DEPRECATED) {
            int rc = mark_file_as_deprecated(iterator.check_offset);
            if (rc != PICO_OK) {
                return rc;
            }
        }
    }

    return PICO_OK;
}


// Bring flash to a state where every invariant holds, and set up the
// in-memory state from it. Used by ffs_initialise() and after a flash error.
static int recover_flash_state(void) {

    ffs_data.needs_recovery = true;
    ffs_data.wr_page_cache_offset = NOT_A_VALID_FLASH_OFFSET;

    int rc = complete_interrupted_deprecations();
    if (rc != PICO_OK) {
        return rc;
    }

    bool complete_file_move;
    uint32_t valid_chunk = NOT_A_VALID_FLASH_OFFSET;
    rc = find_or_make_a_valid_chunk(&valid_chunk, &complete_file_move);
    // W9: a chunk header that cannot be validated in place: move the chunk's records to the other chunk
    if (rc == PICO_ERROR_IO && !complete_file_move && valid_chunk != NOT_A_VALID_FLASH_OFFSET) {
        ffs_data.current_chunk = valid_chunk;
        rc = relocate_files(false, 0, NULL, 0);
        valid_chunk = ffs_data.current_chunk;
    }
    if (rc != PICO_OK) {
        return rc;
    }

    ffs_data.current_chunk = valid_chunk;
    ffs_data.next_write = get_next_flash_write_offset(valid_chunk);

    // A move-after swap left part-way, or a failed swap's new chunk erased after R8 (C4)
    if (complete_file_move) {
        rc = move_existing_files_to_latest_chunk();
        if (rc != PICO_OK) {
            return rc;
        }
    }

    // W5: repair only when there is evidence of an interrupted program
    if (interrupted_program_evident()) {
        rc = relocate_files(false, 0, NULL, 0);
        if (rc != PICO_OK) {
            return rc;
        }
    }

    ffs_data.needs_recovery = false;
    return PICO_OK;
}


// After a flash error, the next call first recovers from flash
static int recover_if_needed(void) {

    if (!ffs_data.needs_recovery) {
        return PICO_OK;
    }

    return recover_flash_state();
}


static uint list_files(file_info_t file_info[], uint max_infos) {

    flash_iterator_t iterator;
    // Only search in the current, active chunk
    iterator.check_offset = ffs_data.current_chunk;
    iterator.end_offset   = iterator.check_offset + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_VALID; // ONLY valid files

    uint files_found = 0;
    while (files_found < max_infos) {

        ffs_file_hdr_t *pfile_hdr = flash_iterator_get_next_file(&iterator);
        if (pfile_hdr == NULL) {
            break; // no more files!
        }

        // Ignore the chunk header record  - it's not a user accessible r/w file.
        if (pfile_hdr->file_id == FILE_ID_CHUNK_HEADER) {
            continue;
        }

        // Add file details to the list, bump counter.
        file_info[files_found].file_id  = (uint)pfile_hdr->file_id;
        file_info[files_found].data_len = (uint)pfile_hdr->data_len;
        files_found++;
    }

    return files_found;
}


static uint32_t calc_total_size_of_files_except(uint ignore_file_id) {

    flash_iterator_t iterator;
    // Only search in the current, active chunk
    iterator.check_offset = ffs_data.current_chunk;
    iterator.end_offset   = iterator.check_offset + ffs_data.chunk_size;
    iterator.increment    = 0;
    iterator.found_filter = FILE_FOUND_VALID; // ONLY valid files
    ffs_file_hdr_t *pfile_hdr;

    uint32_t file_size_less_one_file = 0;

    // Sum the "in flash" sizes of all files except the specified one
    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {

        if (ignore_file_id != (uint) pfile_hdr->file_id) {
            file_size_less_one_file += (uint32_t)sizeof(ffs_file_hdr_t);
            file_size_less_one_file += ROUND_UP_TO_NEXT_32B_WORD(pfile_hdr->data_len);
        }
    }

    return file_size_less_one_file;
}

//----------------------------------------------------------------------------

int ffs_initialise(void) {

    ffs_data.initialised = false;
    ffs_data.wr_page_cache_offset = NOT_A_VALID_FLASH_OFFSET;

#ifndef PICO_RP2040
    int status = get_ffs_partition_info();
    if (status != PICO_OK) {
        return status;
    }
#else
    // For now, there are no partitions on RP2040, ffs area is harded coded
    ffs_data.start_offset = FFS_RP2040_FLASH_START_OFFSET;
    ffs_data.end_offset = FFS_RP2040_FLASH_END_OFFSET;
#endif

    // VITAL maths!
    uint32_t ffs_size = ffs_data.end_offset - ffs_data.start_offset;
    ffs_data.chunk_size = ffs_size / 2;

    if (ffs_data.chunk_size < FLASH_SECTOR_SIZE) {
        return PICO_ERROR_BUFFER_TOO_SMALL;
    }

    if ((ffs_data.chunk_size % FLASH_SECTOR_SIZE) ||
        (ffs_data.start_offset % FLASH_SECTOR_SIZE)) {
        return PICO_ERROR_BAD_ALIGNMENT;
    }

    // Records within a chunk are indexed by a chunk_word_t
    if (ffs_data.chunk_size > MAX_CHUNK_SIZE) {
        return PICO_ERROR_INVALID_ARG;
    }

    int rc = recover_flash_state();
    if (rc != PICO_OK) {
        return rc;
    }

    ffs_data.initialised = true;

    return PICO_OK;
}


int ffs_read(uint file_id, const char **filedata) {

    if (filedata == NULL) {
        return PICO_ERROR_INVALID_ARG;
    }

    // So that a caller ignoring an error never uses a stale pointer
    *filedata = NULL;

    if (!ffs_data.initialised) {
        return PICO_ERROR_INVALID_STATE;
    }

    if (file_id > FFS_MAX_VALID_FILE_ID) {
        return PICO_ERROR_INVALID_ARG;
    }

    int rc = recover_if_needed();
    if (rc != PICO_OK) {
        return rc;
    }

    uint32_t file_offset = search_for_specified_valid_file(file_id);
    if (file_offset == NOT_A_VALID_FLASH_OFFSET) {
        return PICO_ERROR_NOT_FOUND;
    }

    ffs_file_hdr_t *pfile_hdr = (ffs_file_hdr_t *)FL_OFFSET_TO_READ_ADDR(file_offset);

    // Data, if any, starts immediately after the file header structure
    *filedata = (const char *)((uint32_t)pfile_hdr + (uint32_t)sizeof(ffs_file_hdr_t));

    return pfile_hdr->data_len; // Note: could be 0
}


int ffs_delete(uint file_id) {

    if (!ffs_data.initialised) {
        return PICO_ERROR_INVALID_STATE;
    }

    if (file_id > FFS_MAX_VALID_FILE_ID) {
        return PICO_ERROR_INVALID_ARG;
    }

    int rc = recover_if_needed();
    if (rc != PICO_OK) {
        return rc;
    }

    rc = deprecate_all_records_of(file_id);
    if (rc != PICO_OK && rc != PICO_ERROR_NOT_FOUND) {
        ffs_data.needs_recovery = true;
    }

    return rc;
}


int ffs_write(uint file_id, const char *wrdata, uint data_len) {

    if (!ffs_data.initialised) {
        return PICO_ERROR_INVALID_STATE;
    }

    if (wrdata == NULL) {
        return PICO_ERROR_INVALID_ARG;
    }

    if (file_id > FFS_MAX_VALID_FILE_ID) {
        return PICO_ERROR_INVALID_ARG;
    }

    if (data_len > FFS_API_MAX_WRITE_LENGTH) {
        return PICO_ERROR_INVALID_ARG;
    }

    int rc = recover_if_needed();
    if (rc != PICO_OK) {
        return rc;
    }

    uint32_t new_filesize = (uint32_t)sizeof(ffs_file_hdr_t) + ROUND_UP_TO_NEXT_32B_WORD(data_len);

    if (ffs_data.next_write + new_filesize > ffs_data.current_chunk + ffs_data.chunk_size) {
        // New chunk: First check we can keep all existing (valid) files.
        // The extra +3 is vital to avoid round-up causing an unhandled wrap.
        uint32_t current_size = calc_total_size_of_files_except(file_id);
        if (ffs_data.chunk_size < current_size + new_filesize + 3) {
            return PICO_ERROR_INSUFFICIENT_RESOURCES;
        }

        // W4: copy the other files and the new one into the other chunk, then switch to it
        rc = relocate_files(true, file_id, wrdata, data_len);
    } else {
        rc = write_new_file_to_flash(file_id, wrdata, data_len);
        // W9: no room to write a bad record again, or a program that cannot be fixed in place
        // (a bit already 0), so write it into the other chunk
        if ((rc == PICO_ERROR_INSUFFICIENT_RESOURCES || rc == PICO_ERROR_IO) &&
            ffs_data.chunk_size >= calc_total_size_of_files_except(file_id) + new_filesize + 3) {
            rc = relocate_files(true, file_id, wrdata, data_len);
        }
    }

    if (rc != PICO_OK) {
        ffs_data.needs_recovery = true;
    }

    return rc;
}


int ffs_list(file_info_t file_info[], uint max_infos) {

    if (!ffs_data.initialised) {
        return PICO_ERROR_INVALID_STATE;
    }

    if (file_info == NULL) {
        return PICO_ERROR_INVALID_ARG;
    }

    int rc = recover_if_needed();
    if (rc != PICO_OK) {
        return rc;
    }

    return (int) list_files(file_info, max_infos);
}


char *ffs_get_string(uint8_t file_id) {
    const char *data;
    int rc = ffs_read(file_id, &data);
    if (rc >= 0 && data) {
        return strndup(data, rc);
    }
    return NULL;
}


// Updates the FFS file data if the new value is different from the existing value.
// If the file does not exist, it is created.
int ffs_update_string(uint8_t file_id, const char *data) {
    int data_len = strlen(data);
    const char *existing_data;
    int rc = ffs_read(file_id, &existing_data);
    if (rc == data_len + 1 && memcmp(existing_data, data, data_len + 1) == 0) {
        return PICO_OK;
    }

    rc = ffs_write(file_id, data, data_len + 1); // Include the null terminator
    return rc;
}

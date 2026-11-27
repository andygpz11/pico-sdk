/*
 * Copyright (c) 2025 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_FFS_H
#define _PICO_FFS_H

#include "pico.h"
#include "pico/ffs_file_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file pico/ffs.h
 *  \defgroup pico_ffs pico_ffs
 *
 * \brief femto flash filing system API
 *  Please see pico_ffs/README_ffs.md for design details.
 */

#if PICO_RP2040
// On RP2040 there's no flash partitioning, the ffs flash area needs to be hardcoded.
// The defaults make it minimum size and right at the end of the flash memory.
// PICO_CONFIG: FFS_RP2040_FLASH_END_OFFSET, End of ffs flash area + 1, type=int, default=Offset to end of flash + 1, group=pico_ffs
#ifndef FFS_RP2040_FLASH_END_OFFSET
#define FFS_RP2040_FLASH_END_OFFSET  PICO_FLASH_SIZE_BYTES
#endif
// PICO_CONFIG: FFS_RP2040_FLASH_START_OFFSET, Start offset of ffs flash area, type=int, default=Two flash sectors less than end of flash + 1, group=pico_ffs
#ifndef FFS_RP2040_FLASH_START_OFFSET
// Note: This defines the minimum ffs flash area - two sectors
#define FFS_RP2040_FLASH_START_OFFSET (PICO_FLASH_SIZE_BYTES - (2 * FLASH_SECTOR_SIZE))
#endif
#endif

// Used by the API functions: File IDs MUST be less than or equal to this value.
// NOTE: Do NOT change this on a whim, ffs uses an 8-bit type internally!
#define FFS_MAX_VALID_FILE_ID           254

// Maximum length of an individual file that can be specified *on the API*
// The veracity of this is protected by a static_assert() within ffs_internal.c
#define FFS_API_MAX_WRITE_LENGTH        UINT16_MAX

// Structure used by ffs_list() to return stored file information
typedef struct {
    uint file_id;
    uint data_len;
} file_info_t;


/*! \brief Perform 'femto filing system' start of day initialisation
 *  \ingroup pico_ffs
 *
 * Initialises ffs module variables.
 * RP2350: Reads ffs partition location in flash, RP2040 uses hard coded dimensions.
 * Scans ffs flash area to establish start and end of any file records in flash.
 * Repairs what an interrupted write, delete or chunk swap left; with nothing to repair, it
 * programs and erases nothing. If it fails, ffs stays uninitialised and every other call
 * returns PICO_ERROR_INVALID_STATE (after any argument checks).
 *
 * \return PICO_OK on success, or:
 *         PICO_ERROR_INVALID_DATA if the flash contents are inconsistent and cannot be recovered;
 *         the error from flash_safe_execute() if a flash operation failed;
 *         PICO_ERROR_IO if a program or erase did not read back correctly, even when retried;
 *         or another PICO_ERROR_xxx code, e.g. for a bad ffs area definition.
 */
int ffs_initialise(void);


/*! \brief Writes a file to flash.
 *  \ingroup pico_ffs
 *
 * Note: The data length is checked against the maximum size the ffs's internal
 * data type can accommodate, and all files including the new one must fit in one
 * ffs chunk. When the current chunk is full, the files are moved to the other chunk.
 * To ensure integrity, any previous version of the same file is deprecated once
 * the new data has been programmed and read back, just before it is made valid.
 *
 * If the write fails after starting to program flash, the file holds either its
 * previous or its new contents, and the next ffs call first recovers the ffs state
 * from flash.
 *
 * \param file_id  The file ID to write. This must be <= FFS_MAX_VALID_FILE_ID
 * \param wrdata   Pointer to char data to be written to flash (not NULL, even for 0 bytes)
 * \param data_len Length of data to be written, at most FFS_API_MAX_WRITE_LENGTH
 * \return         PICO_OK on success, or:
 *                 PICO_ERROR_INVALID_STATE if ffs is not initialised;
 *                 PICO_ERROR_INVALID_ARG for a bad argument;
 *                 PICO_ERROR_INSUFFICIENT_RESOURCES if the files would not fit in one chunk;
 *                 the error from flash_safe_execute() if a flash operation failed;
 *                 PICO_ERROR_IO if a program did not read back correctly, even when retried;
 *                 or an error from the recovery that the call ran first
 */
int ffs_write(uint file_id, const char *wrdata, uint data_len);


/*! \brief Locate file for reading; set pointer to the data start, return length
 *  \ingroup pico_ffs
 *
 * If the requested file is found in flash, the supplied pointer will be set
 * to the start address of the specified file's data.  The data length, which
 * can be 0, is the return value.  The caller can de-reference the pointer
 * to read the data, up-to the returned length.  If the file is *not* found, or on any
 * other error, the pointer is set to NULL and a suitable PICO_ERROR_xxx code returned.
 * After a failed write or delete, this call first recovers the ffs state from flash,
 * which may program and erase flash and return that recovery's error. A pointer
 * returned earlier stays valid only until the next ffs call that writes, deletes or recovers.
 *
 * \param file_id  The file ID to read. This must be <= FFS_MAX_VALID_FILE_ID
 * \param filedata  A pointer to a pointer to hold the file's data start address.
 * \return          If >= 0, the data length of the file in flash.
 *                  If < 0, one of the PICO_ERROR_xxx codes.
 */
int ffs_read(uint file_id, const char **filedata);


/*! \brief Deletes a file, specified by its file ID.
 *  \ingroup pico_ffs
 *
 * Every valid and nascent record of the file is deprecated, so that an interrupted
 * earlier write cannot bring it back.
 *
 * \param file_id  The file ID to delete. This must be <= FFS_MAX_VALID_FILE_ID
 * \return PICO_OK on success, PICO_ERROR_NOT_FOUND if there was no such file,
 *         PICO_ERROR_INVALID_STATE or PICO_ERROR_INVALID_ARG,
 *         the error from flash_safe_execute() if a flash operation failed,
 *         PICO_ERROR_IO if a status did not read back correctly, even when retried,
 *         or an error from the recovery that the call ran first
 */
int ffs_delete(uint file_id);


/*! \brief Returns a list of file_info structures up to the maximum number specified.
 *  \ingroup pico_ffs
 *
 * Lists the valid files, stopping silently at max_infos. After a failed write or
 * delete, this call first recovers the ffs state from flash, and may return its error.
 *
 * \param file_info Pointer to array of file_info_t data to receive the result
 * \param max_infos Maximum number of file_info_t that can be returned
 * \return          Number of file info records returned on success or PICO_ERROR_xxx
 */
int ffs_list(file_info_t file_info[], uint max_infos);


/*! \brief Returns a malloc'd copy of a NUL-terminated string stored in FFS
 *  \ingroup pico_ffs
 *
 * \param file_id   The file ID to read the string from. This must be <= FFS_MAX_VALID_FILE_ID
 * \return          Pointer to the copy, or NULL if there was an error
 */
char *ffs_get_string(uint8_t file_id);


/*! \brief Updates the FFS file data if the new value is different from the existing value
 *  \ingroup pico_ffs
 *
 * \param file_id   The file ID to update the string in. If the file does not exist, it is created. This must be <= FFS_MAX_VALID_FILE_ID
 * \param data      The string to write to the file
 * \return          PICO_OK on success or PICO_ERROR_xxx
 */
int ffs_update_string(uint8_t file_id, const char *data);

#ifdef __cplusplus
}
#endif

#endif

#!/usr/bin/env python3
#
# Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Deliberate bugs ("mutants") in pico_ffs's ffs.c, each breaking one invariant from README_ffs.md.
# Usage: mutants.py list | mutants.py expected NAME | mutants.py apply NAME SRC DST
import sys

# name: (invariant broken, whether the tests are expected to catch it, text in ffs.c, replacement)
# The ones not expected to be caught are backstops that no test can reach (see README.md).
MUTANTS = {
    'R2-status-before-crc1': ('R2', False,
        '        uint32_t crc1_offset = header_offset + OFFSET_OF_HDR_CRC1_WORD();\n'
        '        int rc = flash_write_and_flush_cache(&crc1_offset, &phdr->crc1, sizeof(phdr->crc1));\n'
        '        if (rc != PICO_OK) {\n            return rc;\n        }\n\n'
        '        uint32_t status_offset = header_offset + OFFSET_OF_HDR_STATUS_BYTE();\n'
        '        rc = flash_write_and_flush_cache(&status_offset, &phdr->status, sizeof(phdr->status));\n',
        '        uint32_t status_offset = header_offset + OFFSET_OF_HDR_STATUS_BYTE();\n'
        '        int rc = flash_write_and_flush_cache(&status_offset, &phdr->status, sizeof(phdr->status));\n'
        '        if (rc != PICO_OK) {\n            return rc;\n        }\n\n'
        '        uint32_t crc1_offset = header_offset + OFFSET_OF_HDR_CRC1_WORD();\n'
        '        rc = flash_write_and_flush_cache(&crc1_offset, &phdr->crc1, sizeof(phdr->crc1));\n'),
    'R2-status-after-failed-crc1': ('R2', True,
        '        if (rc != PICO_OK) {\n            return rc;\n        }\n\n        uint32_t status_offset',
        '        (void)rc;\n\n        uint32_t status_offset'),
    'R3-ignore-failed-deprecation': ('R3', True,
        '        rc = mark_file_as_deprecated(old_offset);\n    }\n\n    // Step 3',
        '        (void)mark_file_as_deprecated(old_offset);\n    }\n\n    // Step 3'),
    'R3-skip-deprecation': ('R3', True,
        '    if (rc == PICO_OK && old_offset != NOT_A_VALID_FLASH_OFFSET) {',
        '    if (false) {'),
    'R4-nascent-never-survives': ('R4', True,
        '        if (id_in_window(s, id)) {\n            s->word[id - s->first_id] = ',
        '        if (id_in_window(s, id) && iterator.file_found != FILE_FOUND_NASCENT) {\n'
        '            s->word[id - s->first_id] = '),
    'R4-later-deprecated-ignored': ('R4', True,
        '    iterator.found_filter = FILE_FOUND_DEPRECATED;\n    ffs_file_hdr_t *pfile_hdr;\n\n'
        '    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {\n'
        '        uint id = pfile_hdr->file_id;\n        if (present != NULL',
        '    iterator.found_filter = FILE_FOUND_NASCENT;\n    ffs_file_hdr_t *pfile_hdr;\n\n'
        '    while ((pfile_hdr = flash_iterator_get_next_file(&iterator)) != NULL) {\n'
        '        uint id = pfile_hdr->file_id;\n        if (present != NULL'),
    'R8-no-newest-valid-fallback': ('R8', True,
        '    bool not_valid = record_state(chosen) != FILE_FOUND_VALID;',
        '    bool not_valid = record_state(chosen) == FILE_FOUND_NASCENT;'),
    'R5-delete-skips-nascent': ('R5', False,
        '        if (iterator.file_found == FILE_FOUND_VALID) {\n            found_valid = true;\n        }\n',
        '        if (iterator.file_found != FILE_FOUND_VALID) {\n            continue;\n        }\n'
        '        found_valid = true;\n'),
    'R6-no-deprecation-completion': ('R6', False,
        '    int rc = complete_interrupted_deprecations();',
        '    int rc = PICO_OK;'),
    'R7-write-into-erased-gap': ('R7', True,
        'if (iterator.file_found == VIRGIN_FLASH && iterator.check_offset >= programmed_end) {',
        'if (iterator.file_found == VIRGIN_FLASH) {'),
    'R8-never-abandon': ('R8', True,
        '    if (rc != PICO_OK) {\n        abandon_new_record(header_offset, old_offset);\n    }',
        '    (void)old_offset;'),
    'R8-abandon-even-if-old-gone': ('R8', True,
        '    if (old_offset != NOT_A_VALID_FLASH_OFFSET && record_state(old_offset) != FILE_FOUND_VALID) {',
        '    if (false) {'),
    'C2-swap-continues-after-error': ('C2', True,
        '    if (rc == PICO_OK) {\n        rc = mark_file_as_deprecated(source);\n    }',
        '    if (rc == PICO_OK) {\n        (void)mark_file_as_deprecated(source);\n    }'),
    'C3-erase-after-failed-copy': ('C3', True,
        '            int rc = copy_survivor(old_chunk, id, chosen, &ffs_data.next_write, chunk_end);\n'
        '            if (rc != PICO_OK) {\n                return rc;\n            }',
        '            (void)copy_survivor(old_chunk, id, chosen, &ffs_data.next_write, chunk_end);'),
    'C4-nascent-chunk-not-validated': ('C4', False,
        '        if (found[i] == FILE_FOUND_NASCENT && other == FILE_FOUND_DEPRECATED) {\n'
        '            *valid_chunk = chunk[i];\n            *complete_file_move = true;\n'
        '            return chunk_set_header_as_valid(chunk[i]);',
        '        if (found[i] == FILE_FOUND_NASCENT && other == FILE_FOUND_DEPRECATED) {\n'
        '            *valid_chunk = chunk[i];\n            *complete_file_move = true;\n'
        '            return PICO_OK;'),
    'G3-no-recovery-after-write-error': ('G3', True,
        '    if (rc != PICO_OK) {\n        ffs_data.needs_recovery = true;\n    }\n\n    return rc;\n}',
        '    return rc;\n}'),
    'G3-flush-error-ignored': ('G3', True,
        '        rc = flash_program_a_page(ffs_data.wr_page_cache_offset, ffs_data.cached_flash_page);',
        '        (void)flash_program_a_page(ffs_data.wr_page_cache_offset, ffs_data.cached_flash_page);'),
    'G3-error-code-collapsed': ('G3', True,
        '    return flash_safe_execute(call_flash_range_program, params, UINT32_MAX);',
        '    return flash_safe_execute(call_flash_range_program, params, UINT32_MAX) ? PICO_ERROR_GENERIC : PICO_OK;'),
    'G4-no-area-guard': ('G4', False,
        '    return offset >= ffs_data.start_offset &&\n        len <= ffs_size &&\n'
        '        offset - ffs_data.start_offset <= ffs_size - len;',
        '    (void)offset;\n    (void)len;\n    (void)ffs_size;\n    return true;'),
    'R1-no-chunk-end-check': ('R1', False,
        '        record_end > ffs_data.current_chunk + ffs_data.chunk_size) {',
        '        false) {'),
}


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if len(sys.argv) > 2 and sys.argv[2] not in MUTANTS:
        sys.exit(f"unknown mutant {sys.argv[2]}; see '{sys.argv[0]} list'")
    if cmd == 'list':
        print('\n'.join(MUTANTS))
    elif cmd == 'expected' and len(sys.argv) == 3:
        rule, caught = MUTANTS[sys.argv[2]][:2]
        print(f"{rule} {'caught' if caught else 'backstop'}")
    elif cmd == 'apply' and len(sys.argv) == 5:
        name, src, dst = sys.argv[2:5]
        old, new = MUTANTS[name][2:]
        text = open(src).read()
        n = text.count(old)
        if n != 1:
            sys.exit(f"{name}: expected the text to change once in {src}, found it {n} times")
        open(dst, 'w').write(text.replace(old, new))
    else:
        sys.exit(f"usage: {sys.argv[0]} list | expected NAME | apply NAME SRC DST")


if __name__ == '__main__':
    main()

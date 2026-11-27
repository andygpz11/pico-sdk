# Host (Linux) build of the pico_ffs power-fail test harness: a standalone project, not part
# of the on-device SDK test build (see README.md for how to build and run it)
cmake_minimum_required(VERSION 3.13)
project(pico_ffs_powerfail_host C)

set(SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(FFS_SRC_DIR ${SDK_DIR}/src/rp2_common/pico_ffs)
set(FFSGEN_DIR ${SDK_DIR}/tools/ffsgen)

foreach(PORT rp2040 rp2350)
    set(T ffs_powerfail_${PORT})
    add_executable(${T}
            harness.c
            flash_sim.c
            ${FFS_SRC_DIR}/ffs.c            # the real, unmodified ffs sources
            ${FFS_SRC_DIR}/ffs_internal.c
    )
    # stubs/ must come first so its pico.h etc. shadow the SDK's
    target_include_directories(${T} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/stubs
            ${CMAKE_CURRENT_LIST_DIR}
            ${FFS_SRC_DIR}
            ${FFS_SRC_DIR}/include
            ${SDK_DIR}/src/common/pico_base_headers/include
            ${SDK_DIR}/src/common/boot_picobin_headers/include
            ${SDK_DIR}/src/rp2_common/boot_bootrom_headers/include
    )
    target_compile_definitions(${T} PRIVATE _GNU_SOURCE)
    if (PORT STREQUAL rp2040)
        target_compile_definitions(${T} PRIVATE
                PICO_RP2040=1
                FFS_RP2040_FLASH_START_OFFSET=sim_ffs_start_offset
                FFS_RP2040_FLASH_END_OFFSET=sim_ffs_end_offset)
    endif()
    # No inlining or sibling calls keeps ffs's statics in backtraces, -no-pie lets addr2line resolve them;
    # the pointer cast warnings are safe as the simulated flash sits at the real 32-bit XIP addresses
    target_compile_options(${T} PRIVATE -O1 -g -fno-inline -fno-inline-small-functions
            -fno-optimize-sibling-calls -fno-omit-frame-pointer
            -Wall -Wno-format-truncation -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
    target_link_options(${T} PRIVATE -no-pie)
endforeach()

# Build ffsgen from its sources so a test can start from an ffsgen generated image
add_executable(ffsgen ${FFSGEN_DIR}/ffsgen.c ${FFS_SRC_DIR}/ffs_internal.c)
target_include_directories(ffsgen PRIVATE ${FFS_SRC_DIR})

set(PRELOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/preload)
file(MAKE_DIRECTORY ${PRELOAD_DIR})
file(WRITE ${PRELOAD_DIR}/ssid.txt "FactoryNet")
file(WRITE ${PRELOAD_DIR}/password.txt "correct-horse-battery-staple")
file(WRITE ${PRELOAD_DIR}/deployment_uri.txt "https://example.invalid/deploy/0123456789abcdef")
file(WRITE ${PRELOAD_DIR}/ffsgen.cfg
"# ffsgen config used by the power-fail harness
0x10 ssid.txt
0x11 password.txt
0x03 deployment_uri.txt
")
foreach(CHUNK 4096 8192)
    add_custom_command(OUTPUT ${PRELOAD_DIR}/ffs_preload_${CHUNK}.bin
            COMMAND ffsgen ffsgen.cfg ffs_preload_${CHUNK}.bin ${CHUNK}
            WORKING_DIRECTORY ${PRELOAD_DIR}
            DEPENDS ffsgen ${PRELOAD_DIR}/ffsgen.cfg)
    list(APPEND PRELOADS ${PRELOAD_DIR}/ffs_preload_${CHUNK}.bin)
endforeach()
add_custom_target(ffs_preload_images ALL DEPENDS ${PRELOADS})

enable_testing()
foreach(PORT rp2040 rp2350)
    add_test(NAME ${PORT}_virgin_flash COMMAND ffs_powerfail_${PORT} --examples 0)
    add_test(NAME ${PORT}_ffsgen_preload
            COMMAND ffs_powerfail_${PORT} --examples 0 --preload ${PRELOAD_DIR}/ffs_preload_4096.bin)
    add_test(NAME ${PORT}_two_sector_chunks
            COMMAND ffs_powerfail_${PORT} --examples 0 --chunk-sectors 2
            --preload ${PRELOAD_DIR}/ffs_preload_8192.bin)
endforeach()

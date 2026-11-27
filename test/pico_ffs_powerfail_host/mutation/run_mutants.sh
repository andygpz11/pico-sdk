#!/bin/sh
#
# Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Plants each ffs.c mutant in turn, runs the power-fail harness against it, and reports which the tests
# catch (see README.md). Usage: run_mutants.sh [-j JOBS] [-w WORKDIR] [-p POWERCUT_TEST_DIR] [MUTANT...]

set -eu
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(cd "$here/../../.." && pwd)

# One mutant, run by the parallel jobs below: prints "name|result|detail"
run_one() {
    m=$1
    w=$work/trees/$m
    if ! mkdir -p "$work/trees"; then
        echo "$m|ERROR|cannot create $work/trees"
        return
    fi
    if ! { rm -rf "$w" &&
           mkdir -p "$w/src/common" "$w/src/rp2_common" "$w/tools" "$w/test" &&
           cp -r "$sdk/src/common/pico_base_headers" "$sdk/src/common/boot_picobin_headers" "$w/src/common/" &&
           cp -r "$sdk/src/rp2_common/boot_bootrom_headers" "$sdk/src/rp2_common/pico_ffs" "$w/src/rp2_common/" &&
           cp -r "$sdk/tools/ffsgen" "$w/tools/" &&
           cp -r "$sdk/test/pico_ffs_powerfail_host" "$w/test/" &&
           rm -rf "$w/test/pico_ffs_powerfail_host/build"; } 2>"$w.setup.log"; then
        echo "$m|ERROR|setting up the source tree failed: $(cat "$w.setup.log")"
        return
    fi
    s=$w/src/rp2_common/pico_ffs

    if [ "$m" != "none" ]; then
        if ! python3 -I "$here/mutants.py" apply "$m" "$sdk/src/rp2_common/pico_ffs/ffs.c" "$s/ffs.c" 2>"$w/apply.log"; then
            echo "$m|APPLY-FAILED|$(cat "$w/apply.log")"
            return
        fi
    fi

    if ! { cmake -S "$w/test/pico_ffs_powerfail_host" -B "$w/build" && cmake --build "$w/build" -j 2; } >"$w/build.log" 2>&1; then
        echo "$m|BUILD-FAILED|see $w/build.log"
        return
    fi

    h=$w/build/ffs_powerfail_rp2040
    for t in "quick:--modes clean,err-before,err-after,err-twice" \
             "2-sector:--modes clean,err-before,err-after --chunk-sectors 2 --preload $w/build/preload/ffs_preload_8192.bin" \
             "torn:--modes prefix,random --seeds 1 --prefix-samples 4"; do
        name=${t%%:*}
        # Exit status 1 means the tests failed; anything else is a usage error or a crash
        status=0
        # shellcheck disable=SC2086
        "$h" ${t#*:} --examples 1 >"$w/$name.txt" 2>&1 || status=$?
        if [ "$status" -eq 1 ]; then
            cat=$(grep -m1 -o -E '#1 +[0-9]+ x +[a-z0-9+-]+ +[a-z0-9-]+' "$w/$name.txt" | awk '{print $4 ", " $5}')
            [ -n "$cat" ] || cat=$(grep -m1 -o '\[[a-z-]*\]' "$w/$name.txt" || echo "see $w/$name.txt")
            echo "$m|CAUGHT|harness $name: $cat"
            return
        fi
        if [ "$status" -ne 0 ]; then
            echo "$m|ERROR|harness $name exited with status $status: see $w/$name.txt"
            return
        fi
    done

    if [ -n "$powercut" ]; then
        start=$((0x300000))
        if ! cc -std=gnu11 -O2 -Wall -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
                -I"$powercut/stubs" -I"$s/include" -I"$s" \
                -DFFS_RP2040_FLASH_START_OFFSET=$start -DFFS_RP2040_FLASH_END_OFFSET=$((start + 8 * 1024)) \
                -o "$w/powercut" "$powercut/ffs_powercut.c" "$s/ffs_internal.c" >"$w/powercut-build.log" 2>&1; then
            echo "$m|BUILD-FAILED|see $w/powercut-build.log"
            return
        fi
        if ! "$w/powercut" -n 300 >"$w/powercut.txt" 2>&1; then
            echo "$m|CAUGHT|power-cut test: $(grep -m1 '^FAIL:' "$w/powercut.txt" || echo "see $w/powercut.txt")"
            return
        fi
    fi

    echo "$m|SURVIVED|"
}

if [ -n "${FFS_MUTATION_ONE:-}" ]; then
    work=$FFS_MUTATION_WORK
    powercut=$FFS_MUTATION_POWERCUT
    run_one "$FFS_MUTATION_ONE"
    exit 0
fi

jobs=$(( $(nproc) / 2 ))
[ "$jobs" -ge 1 ] || jobs=1
work=$sdk/build-ffs-mutation
powercut=
while getopts "j:w:p:h" opt; do
    case $opt in
    j) jobs=$OPTARG ;;
    w) work=$OPTARG ;;
    p) powercut=$(cd "$OPTARG" 2>/dev/null && pwd) || { echo "error: -p $OPTARG is not a directory" >&2; exit 2; } ;;
    *) sed -n '7,8p' "$0" | sed 's/^# //'; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

if [ -n "$powercut" ] && [ ! -f "$powercut/ffs_powercut.c" ]; then
    echo "error: -p $powercut does not hold ffs_powercut.c" >&2
    exit 2
fi

mutants=${*:-$(python3 -I "$here/mutants.py" list)}
for m in $mutants; do
    python3 -I "$here/mutants.py" expected "$m" >/dev/null || { echo "error: unknown mutant $m" >&2; exit 2; }
done

mkdir -p "$work"
results=$work/results.txt
echo "running $(echo "$mutants" | wc -w) mutants and the unmutated sources, $jobs at a time, in $work"
started=$(date +%s)
# shellcheck disable=SC2086
if ! printf '%s\n' none $mutants | FFS_MUTATION_WORK=$work FFS_MUTATION_POWERCUT=$powercut \
        xargs -P "$jobs" -I{} env FFS_MUTATION_ONE={} sh "$0" > "$results"; then
    echo "error: a mutant run stopped without reporting a result (results so far in $results)" >&2
    exit 2
fi

if ! base=$(grep '^none|' "$results"); then
    echo "error: no result for the unmutated sources in $results" >&2
    exit 2
fi
if [ "${base#none|}" != "SURVIVED|" ]; then
    echo "FAIL: the tests fail on the unmutated sources, so the mutant results mean nothing:" >&2
    echo "  ${base#none|}" >&2
    exit 1
fi

unexpected=0
printf '%-38s %-8s %-9s %s\n' MUTANT BREAKS RESULT DETAIL
for m in $mutants; do
    if ! line=$(grep "^$m|" "$results"); then
        echo "error: no result for mutant $m in $results" >&2
        exit 2
    fi
    result=$(echo "$line" | cut -d'|' -f2)
    detail=$(echo "$line" | cut -d'|' -f3-)
    set -- $(python3 -I "$here/mutants.py" expected "$m")
    rule=$1
    expect=$2
    note=
    if [ "$result" = "CAUGHT" ] && [ "$expect" = "backstop" ]; then
        note="  (backstop, now caught: update mutants.py)"
    elif [ "$result" != "CAUGHT" ] && { [ "$result" != "SURVIVED" ] || [ "$expect" != "backstop" ]; }; then
        note="  <-- UNEXPECTED"
        unexpected=$((unexpected + 1))
    fi
    printf '%-38s %-8s %-9s %s%s\n' "$m" "$rule" "$result" "$detail" "$note"
done
echo "took $(( $(date +%s) - started ))s"

if [ "$unexpected" -ne 0 ]; then
    echo "FAIL: $unexpected mutant(s) not caught by the tests, not built, or with errors" >&2
    exit 1
fi
echo "passed: every mutant caught, except the expected backstops"

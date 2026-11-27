# pico_ffs power-fail test harness (host build)

This harness checks that `pico_ffs` keeps the "fail safe" behaviour that `README_ffs.md` promises. It compiles the real, unmodified `ffs.c` and `ffs_internal.c` for a Linux host and runs them against a simulated NOR flash. It cuts the power at every flash operation of a scripted workload, reboots, and checks the result. It also makes each flash operation report an error in turn, and checks that ffs reports it and loses no data.

It is a standalone CMake project. It is not part of the on-device SDK test build.

```bash
cmake -S test/pico_ffs_powerfail_host -B build-ffs-powerfail
cmake --build build-ffs-powerfail
ctest --test-dir build-ffs-powerfail --output-on-failure        # summary only
build-ffs-powerfail/ffs_powerfail_rp2040                        # full report
build-ffs-powerfail/ffs_powerfail_rp2350 --preload build-ffs-powerfail/preload/ffs_preload_4096.bin
```

## How it works

### Two port builds
There are two executables, `ffs_powerfail_rp2040` and `ffs_powerfail_rp2350`.
- **RP2040:** the simulated flash is mapped at `XIP_BASE` (0x10000000). The ffs area comes from `FFS_RP2040_FLASH_{START,END}_OFFSET`.
- **RP2350:** the simulated flash is mapped at the un-translated alias (0x14000000). The ffs area comes from a stubbed `rom_get_partition_table_info()`.

Both builds use the real `FL_OFFSET_TO_READ_ADDR()`. The flash mapping is read-only, so any stray direct write would crash and be reported.

### Simulated flash
The simulated flash is a 2 MB device with W25Q16JV geometry: 256 B pages and 4 KB sectors. It follows NOR rules:
- A program can only clear bits.
- An erase sets a whole sector to 0xff.

Everything below the ffs area is filled with random "firmware" bytes.

### Power failure
Every page program and every sector erase counts as one "op". The fault-free run is deterministic, so op N is the same op in every run. For each op, the harness re-runs the workload from a pristine image and cuts the power at that op in one of these ways:

| mode     | effect |
|----------|--------|
| `clean`  | The op never starts. This covers every point between flash ops, i.e. a power cut anywhere in ffs code. |
| `prefix` | A torn program, where only the first *k* changing bytes land. For an erase, only the first *k*/8 of the sector is erased. |
| `random` | A torn op, where a random subset of the bit changes land. |
| `silent` | Not run by default. No power cut: a page program reports success, but one bit it should clear stays set, or one bit it should leave set reads 0 until the sector is erased. Both were seen on a Pico 2. The workload carries on. |
| `marginal` | Not run by default. The op completes, but leaves 1–4 of its bit changes weak, as a power cut late in a real erase or program does. A weak bit reads as intended, or (1 time in 4) not, afresh at every later flash op and boot, until a program clears it or an erase sets it. |
| `--nested` | Also cuts power a second time at every op of the recovery boot. |

### Flash errors
The real `flash_safe_execute()` can fail in two ways, and the harness injects both, at the first op of each call in turn:

| mode         | effect |
|--------------|--------|
| `err-before` | Entering the safe zone fails: the op is not done, and `PICO_ERROR_TIMEOUT` is returned. |
| `err-after`  | The op is done, then leaving the safe zone fails and `PICO_ERROR_TIMEOUT` is returned. ffs is told the op failed although it reached the flash. |
| `err-twice`  | As `err-before`, then a second error 1 to 8 flash calls later. The second error lands in ffs's own clean-up of the first (R8), in the recovery the next call starts with, including its move-after copies (C4), or later in a chunk swap. |

The boot then carries on with its workload, followed by the same reboots as after a power failure. These checks apply, from the invariants in `README_ffs.md`:
- The call that hit the error must return an error (G3). If initialisation hits it, `ffs_initialise()` must fail and that boot does nothing more. A call whose opening recovery hits the error must fail too: the harness retries an `ffs_read()` or `ffs_list()`, and treats a write or delete as one that may or may not have happened.
- The next ffs call recovers, after which the failed call's file must hold its previous state or the new one (G2).
- With a single error, if the file's previous version was still valid in flash when the write failed, that version must remain (R8). The harness reads the flash directly to decide this.
- Every other file must stay intact throughout.
- The error code returned should be the one `flash_safe_execute()` returned (G3). This is checked last, so that it never hides a data integrity failure.

### Hand-built flash images
Some states can't be reached from a single fault in the workload: older firmware could have left them, or two faults in a row. The harness writes these straight into the flash, then runs `ffs_initialise()`, the continued-use workload (the last image runs its own fill workload) and a reboot, checking every file throughout:

| Image | Checks |
|---|---|
| A stale nascent record, then a valid copy of the same file | R4: the valid copy is kept |
| A stale nascent record, then a deleted (deprecated) copy | R4: the file stays deleted |
| A valid copy, then the newest record still nascent | R4, W5: the newest record is kept |
| A valid status with an erased crc1 (left by older firmware), then a valid copy | R4, W8: the later copy is kept |
| Two nascent records of one file | R4: the later one is kept |
| The current chunk programmed with junk to its very end | R7, W5: initialisation moves the files to the other chunk, and later writes work |
| New files written until ffs is full, then an update, a delete and another new file | Writes are refused with `PICO_ERROR_INSUFFICIENT_RESOURCES` only when they can't fit, and at least once |

### Reboots
Each boot is a forked process, so a power failure is a real `_exit()` and the next boot starts from fresh static state. After the power failure the harness runs these boots:
1. **Recovery boot:** `ffs_initialise()`, then a full check, then a continued-use workload that does several more chunk swaps and checks after every step.
2. **Plain reboot:** a final check.

### Checks
The harness keeps its own record of what each file should hold: the "oracle". It lives in shared memory, so it survives a simulated power failure. After every step and reboot, the harness reads each file through ffs and compares it with the oracle. A scenario fails if any of these happens:
- `ffs_initialise()` fails.
- A file whose write or delete had returned is lost or changed.
- The file being written when power failed is neither its old version nor its new one.
- `ffs_list()` disagrees with `ffs_read()`.
- A later API call fails.
- ffs programs or erases flash outside its own area.
- ffs tries to program a 0 bit back to 1 (NOR can't do that, so data is silently corrupted). Programming more 0s over programmed bytes is allowed.
- `ffs_update_string()` with an unchanged string does any flash operation.
- ffs crashes or hangs.
- A call that hit a flash error returns `PICO_OK`, or returns a different error code from the one the flash reported.
- A write that failed while the file's previous version was still valid becomes visible (R8).
- A plain reboot after a clean shutdown does any flash operation (W5).
- A hand-built flash image (see above) isn't recovered as expected.

### Reporting
Failures are grouped by signature: power-fail type, what went wrong, and the ffs call chain at the failing op. The chain comes from `backtrace()` and `addr2line`. For each group the harness prints one example, including a decoded map of both chunks just after the power failure.

### Workload
- **Boot 1** (power is cut during this boot) covers:
  - first-ever initialisation;
  - new files, zero-length files and records that span pages;
  - updates, delete and re-create, and `ffs_update_string()`, including with an unchanged string;
  - a file that is mostly 0xff;
  - four chunk swaps (three with `--chunk-sectors 2`), triggered by both updates and new-file creation.
- **The recovery boot** does another 72 steps (136 with `--chunk-sectors 2`).
- **ctest** runs both ports three ways: virgin flash, an image made by `ffsgen` (built from `tools/ffsgen`), and 2-sector chunks.

## Options
Run with `--help`. Useful ones:
- `--modes clean`: fast, and the most important power-fail mode.
- `--modes err-before,err-after,err-twice`: only the flash error modes; about 7 minutes.
- `--modes marginal`: weak cells left by late power cuts; about 2 minutes. Not in the default set.
- `--modes silent`: programs that fail without reporting it; about 5 minutes. Not in the default set.
- `--only-op N`: run a single scenario.
- `--verbose`: list every op of the fault-free run, with its ffs call chain.
- `--nested`: also cut the power at every op of the recovery boot. This is slow; see Limitations.
- `--examples N`: describe only the first N failure signatures in full.

## Example commands
Build once, and again after any change to ffs or the harness:
```bash
cmake -S test/pico_ffs_powerfail_host -B build-ffs-powerfail
cmake --build build-ffs-powerfail -j8
```

The times below were measured on a 20-core PC under WSL2, unless marked "est." (an estimate). The `ffs_powerfail_rp2350` runs take about as long as the RP2040 ones.

| Purpose | Command | Time |
|---|---|---|
| Flash errors only | `build-ffs-powerfail/ffs_powerfail_rp2040 --modes err-before,err-after` | 1 min |
| Weak cells left by late power cuts | `build-ffs-powerfail/ffs_powerfail_rp2040 --modes marginal` | 2 min |
| Quick check after a change | `build-ffs-powerfail/ffs_powerfail_rp2040 --modes clean,err-before,err-after,err-twice` | 2–4 min (est.) |
| Every mode, one port | `build-ffs-powerfail/ffs_powerfail_rp2040` | 8 min |
| Start from an `ffsgen` image | `build-ffs-powerfail/ffs_powerfail_rp2040 --preload build-ffs-powerfail/preload/ffs_preload_4096.bin` | 8 min |
| 2-sector chunks, quick modes | `build-ffs-powerfail/ffs_powerfail_rp2040 --chunk-sectors 2 --preload build-ffs-powerfail/preload/ffs_preload_8192.bin --modes clean,err-before,err-after,err-twice` | 4–8 min (est.) |
| 2-sector chunks, every mode | The same, without `--modes` | 28 min |
| Full suite, 6 tests in parallel | `ctest --test-dir build-ffs-powerfail -j6 --output-on-failure` | 28 min |
| Full suite, one test at a time | `ctest --test-dir build-ffs-powerfail --output-on-failure` | 75 min (est.) |
| Second power cut during every recovery | `build-ffs-powerfail/ffs_powerfail_rp2040 --nested` | 4 h (est.) |
| Re-run one scenario | `build-ffs-powerfail/ffs_powerfail_rp2040 --only-op 174 --modes err-before` | Seconds |
| List the ops of the fault-free run | `build-ffs-powerfail/ffs_powerfail_rp2040 --verbose --modes clean --only-op 0` | Seconds |

`ctest` is CMake's test runner. It runs the six tests registered in `CMakeLists.txt` (each port: virgin flash, `ffsgen` preload, 2-sector chunks) and reports each as passed or failed. A test passes when its executable exits with 0. Useful options:
- `-j6`: run the six tests at once, so the suite takes as long as its slowest test.
- `--output-on-failure`: print a test's full report only if it fails.
- `-R <pattern>`: run only the tests whose names match, for example `-R rp2350`.

Every run exits with 1 if any scenario fails, so these commands also work in scripts and CI.

## Mutation check
`mutation/run_mutants.sh` plants deliberate bugs in `ffs.c`, at least one for most invariants, and checks that these tests catch each
one (about 35 minutes). Run it after changing the harness or ffs, to show the tests still guard every invariant. See
`mutation/README.md`.

## Limitations
A clean pass shows that ffs survives every power cut this harness can produce. It does not prove that ffs survives every power cut on real hardware:
- **The flash is a model.** The simulator follows NOR rules (a program only clears bits, an erase sets a sector to 0xff) and models torn programs and erases. `marginal` models weak cells only roughly: a few bits per interrupted op, re-read at each flash op and boot rather than at every read. It does not model the W25Q16JV's real internal behaviour in detail, or a power cut during the bootrom's own flash command sequence. A power-cycling soak test on real hardware is a useful complement.
- **The workload is fixed.** It covers new files, updates, deletes, zero-length files, records that span pages and four chunk swaps. It does not cover every pattern, for example a nearly full chunk holding many small files.
- **Nested runs use clean cuts only.** `--nested` adds a second clean cut during the recovery boot. A torn op followed by a second cut is not tested.
- **The full nested run is slow.** It takes about 4 hours per port. After a change to ffs, use the quick check (see Example commands), run the ctest suite (about 55 minutes) before committing, and run `--nested` before a release.

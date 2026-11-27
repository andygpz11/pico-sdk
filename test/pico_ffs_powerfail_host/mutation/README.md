# Mutation check for the pico_ffs tests

Passing tests show that the cases someone thought of work. This check shows whether the tests would notice if an
invariant from `src/rp2_common/pico_ffs/README_ffs.md` stopped being kept. It plants one deliberate bug (a
"mutant") at a time in `ffs.c` and runs the power-fail harness against it. A mutant the tests don't catch is a gap
in the tests.

```bash
test/pico_ffs_powerfail_host/mutation/run_mutants.sh                 # all mutants, about 35 min
test/pico_ffs_powerfail_host/mutation/run_mutants.sh R4-nascent-never-survives R8-never-abandon
```

Options:
- `-j JOBS`: mutants built and run at once (default: half the CPUs).
- `-w WORKDIR`: where each mutant's copy of the sources is built (default `build-ffs-mutation` at the SDK root).
- `-p DIR`: also run an external power-cut test against each mutant: a directory holding `ffs_powercut.c` and
  its `stubs/`, built against the mutant's `ffs.c` and run with `-n 300`.

Each mutant runs the harness's quick modes (`clean,err-before,err-after,err-twice`), then 2-sector chunks, then a
sample of torn power cuts, stopping at the first that fails. The same tests also run on the unmutated sources:
if those fail, the script stops, because the mutant results would then mean nothing.

The script exits with 0 only if every mutant is caught, except those `mutants.py` marks as backstops. It also
says when a backstop has started to be caught, so that `mutants.py` can be updated.

## Mutants

`mutants.py list` lists them; each breaks one invariant. These six are backstops, which no test can reach:

| Mutant | Why no test catches it |
|---|---|
| `R1-no-chunk-end-check` | `ffs_write()` already swaps chunks before a record could overrun one. |
| `G4-no-area-guard` | Nothing else ever addresses flash outside the ffs area. |
| `R5-delete-skips-nascent` | A nascent record never survives to an `ffs_delete()`: start-up moves it to the other chunk (W5), a write whose record fails its check writes it again or swaps (W9), and after a flash error the next call recovers first. |
| `R6-no-deprecation-completion` | Guards against weak flash bits; the simulated flash models them (`--modes marginal`), but this script doesn't run that mode. |
| `C4-nascent-chunk-not-validated` | The next start-up finds two nascent headers and validates the chunk holding records (W7), with the same result. |
| `R2-status-before-crc1` | A valid status with an erased crc1 already reads as nascent (W8), so the order of the two writes doesn't change the outcome. |

## Adding a mutant

Add an entry to `MUTANTS` in `mutants.py`: the invariant it breaks, whether the tests should catch it, the exact
text in `ffs.c` and its replacement. The text must occur exactly once in `ffs.c`, or `apply` fails. When `ffs.c`
changes, `run_mutants.sh` reports any mutant whose text no longer matches as `APPLY-FAILED`.

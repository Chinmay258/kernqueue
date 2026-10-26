# KernQueue: project status

As of 2026-10-27, after the final engineering audit
([FINAL_AUDIT.md](FINAL_AUDIT.md)), on top of commit `6f3cdb2`. The full context
is in [PROJECT_HANDOFF.md](PROJECT_HANDOFF.md); the run-by-run record is in
[implementation_status.md](implementation_status.md).

All nine planned milestones are done:
1. environment
2. driver skeleton
3. ring buffer and read/write
4. synchronisation, blocking and poll
5. ioctl
6. client and functional tests
7. stress tests
8. benchmark
9. documentation and review

Validation under lockdep + KASAN and under strict KCSAN, and a final audit,
are also done. The audit found **no driver defect** and made **no driver
change**. It did correct one benchmark claim.

Labels: **[AUDIT]** run and observed during the audit (2026-10-26);
**[HISTORICAL]** recorded earlier in `docs/evidence/`, not re-run.

## Completed (verified on device)
- **Character device:** `/dev/kernqueue` with clean init/exit and `goto`
  unwinding; `capacity` module parameter (4 KiB–16 MiB, range-checked).
  [AUDIT]
- **Byte ring:** head/tail/count, invariant check after every transfer.
- **read/write:** `copy_*_user` with exact partial-fault semantics; atomic
  writes ≤ 4096 B, short writes above.
- **Blocking and non-blocking:** blocking I/O, `O_NONBLOCK`/`EAGAIN`,
  `-ERESTARTSYS` (EINTR or restart), and `SIGKILL` while blocked (new audit
  tests).
- **poll:** `EPOLLOUT` only at ≥ 4096 bytes free; the exact threshold
  transition is now tested.
- **ioctls:** GET_STATS / RESET / CLEAR_STATS, with `ENOTTY`/`EBADF`/`EFAULT`;
  `compat_ptr_ioctl` tested from a 32-bit process.
- **Wake-up strategy:** `exclusive_wake` parameter. Default Y: need-aware
  exclusive wake-ups with hand-off. N: wake-all, as in v1.
- **Tools and tests:**
  - `kqctl` CLI
  - tests: functional ×33 (27 + 6 added by the audit), stress, ring unit,
    compat32, harness self-test, kqctl smoke
  - benchmark plus an interleaved A/B script
  - debug-kernel, KCSAN-mutation and (new) wake-mutation scripts

**Audit device run [AUDIT]** (stock kernel, `docs/evidence/audit-*`):
- functional 4 configurations × 33/33 PASS
- stress 16/16 PASS
- kqctl 13/13, compat32 PASS
- bad parameters 3/3 rejected
- kernel log clean for the device-cycle phase
- wake mutation check PASS: each of 3 mutants caught 3/3; control passes
- min-capacity A/B and the benchmark matrix rerun

**Build and user-space checks [AUDIT]**, from a fresh clone:
- `make user`: 0 warnings, `-Werror`
- `make module`: W=1, 0 warnings
- `make sparse`: clean
- checkpatch: driver and headers 0/0
- `test_ring` 5/5
- harness 4 passes + 7/7 faults detected
- ASan+UBSan clean
- `git status` clean after the builds; `make clean` complete

## Partially validated
- **Sanitizer coverage of the new tests.** lockdep, KASAN,
  DEBUG_ATOMIC_SLEEP and strict KCSAN ran the **27 original tests**
  ([HISTORICAL], `debug-run2`, `kcsan-run2`, KCSAN mutation check in
  `kcsan-mutant`). The **6 new tests have run only on the stock kernel.**
- **Kernel log during the audit's wake-mutant and A/B phases:** not captured
  (the scripts don't scan `dmesg`, and the VM restarted before collection).
- **Performance claims.** These reproduced in every session that measured
  them:
  - 16×1, 4 KiB: writer sleeps ~1.8× lower; writer `write()` p99 1.8–2.6×
    lower
  - 16×1 integrity stress 4.3–6.0× faster (3 stock-kernel sessions)
  - ≈5× slower when the ring holds one record (2 interleaved A/Bs)
  - 1–11% slower for one producer and one consumer

  **Not reproduced:** the 2.7× throughput at 16×1 and 1×16. The audit rerun
  measured 1.05× (ranges overlap) and 0.86×. It is no longer claimed.
- **Interrupted-after-exclusive-wake hand-off:** covered by code review only;
  forcing it needs privileged `userfaultfd`.
- **EPOLLEXCLUSIVE interaction (F4) and the copy-under-mutex stall (F1):**
  analysed, with no reproducer.

## Outstanding
- The 6 new tests under the KASAN/lockdep and KCSAN kernels
- A kernel-log scan in `wake_mutant_check.sh` and `ab_wake.sh`
- Interleaved A/Bs for 1×1 and 16×1 before quoting any throughput ratio
- Soak run; `kq_bench --rate` run
- Other kernels, architectures, bare metal
- Runtime capacity change; multiple device instances; a udev rule
- Transition-only or `wq_has_sleeper` wake optimisation; bounce-buffered
  copies
- Profiling (no `perf` on WSL)
- Removing the pre-redaction paths from Git history (needs a history rewrite
  and force-push; not done)

## Known issues
- **KCSAN `data_race` self-test never reports** (2 attempts; cause unknown).
  It's informational; the KCSAN mutation check is the required proof.
- **KCSAN run 1's device-cycle verdict was FAIL** because of the first,
  coarse attribution rule. That rule was replaced; on replay, 0 races were
  attributed to kernqueue.
- **KASAN debug run 1's `lock_order` self-test was missed:** lockdep had
  already turned itself off. The order was fixed, and run 2 passed.
- **Copies run under the mutex (F1):** a slow or `userfaultfd`-backed buffer
  can stall other clients.
- **Spurious `poll` wake-ups (F3):** cost not measured. The audit test shows
  they never make `poll()` return early.
- **Fairness:** large atomic writers can be starved by small writers (F5).
- **Benchmark ordering (F11):** the matrix runs the two wake modes in
  separate blocks, minutes apart.

## Environment limitations
- Every device test needs a human to type the WSL sudo password.
- Booting a debug kernel needs `%UserProfile%\.wslconfig` + `wsl --shutdown`.
  It's currently **reverted**: no `.wslconfig`, stock kernel running.
- WSL VM boot timeout (the heavy KCSAN config never booted), and one hang
  after a KCSAN session.
- No `perf`. Single VM; the same benchmark configuration varied by up to ~2×
  between sessions.
- No kernel headers ship for WSL; the module builds against
  `~/kq-kbuild/linux`.

## Git
- **Branch:** `main`, in sync with `origin/main` (private remote) at
  `6f3cdb2`.
- **Uncommitted audit work:**
  - modified: `tests/test_functional.c`, 16 redacted evidence logs, and the
    docs listed in FINAL_AUDIT.md §3
  - new: `docs/FINAL_AUDIT.md`, `scripts/wake_mutant_check.sh` (to be
    committed as executable), `docs/evidence/audit-*`,
    `bench/results/20261026-200144/`

## Resume development
From WSL Ubuntu, in the repository:
```bash
make user && make module && make test-unit && bash scripts/run_device_tests.sh
```
The last command needs sudo and loads the module.
`bash scripts/wake_mutant_check.sh` repeats the wake mutation check.

## Next three actions
1. **Run the 6 new tests under the debug kernels**
   (`scripts/run_debug_checks.sh` with the KASAN and KCSAN builds).
2. **Fix the benchmark method:** interleave the wake modes in the matrix (or
   use `ab_wake.sh` per configuration) and add a `dmesg` scan to the mutant
   and A/B scripts. Then re-measure before quoting any throughput ratio.
3. **Run on a second platform** (native Linux or QEMU, another kernel
   version, ideally arm64) with a soak run (`KQ_SOAK_SECONDS=1800`).

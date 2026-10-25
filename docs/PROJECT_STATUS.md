# KernQueue: project status

As of 2026-10-25, commit `5ec6551`. The full context is in
[PROJECT_HANDOFF.md](PROJECT_HANDOFF.md); the run-by-run record is in
[implementation_status.md](implementation_status.md).

## Current milestone
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

Post-milestone validation under lockdep + KASAN and under strict KCSAN is
also done. **Next phase: performance follow-ups** (see below).

## Implemented (verified on device)
- **Character device:** `/dev/kernqueue` with clean init/exit and `goto`
  unwinding; `capacity` module parameter (4 KiB–16 MiB, range-checked).
- **Byte ring:** head/tail/count, invariant check after every transfer.
- **read/write:** `copy_*_user` with exact partial-fault semantics; atomic
  writes ≤ 4096 B, short writes above.
- **Blocking and non-blocking:** blocking I/O, `O_NONBLOCK`/`EAGAIN`,
  `-ERESTARTSYS` (EINTR or restart).
- **poll:** `EPOLLOUT` only at ≥ 4096 bytes free.
- **ioctls:** GET_STATS / RESET / CLEAR_STATS, with `ENOTTY`/`EBADF`/`EFAULT`;
  `compat_ptr_ioctl` tested from a 32-bit process.
- **Wake-up strategy:** `exclusive_wake` parameter. Default Y: need-aware
  exclusive wake-ups with hand-off. N: wake-all, as in v1.
- **Tools:** `kqctl` CLI; tests (functional ×27, stress, ring unit, compat32,
  harness self-test, kqctl smoke); benchmark plus A/B script; debug-kernel
  and KCSAN mutation scripts.

## Missing / not done
- Runtime capacity change
- Multiple device instances
- A udev rule (node permissions are set by `load_module.sh`)
- Transition-only or `wq_has_sleeper` wake optimisation
- Bounce-buffered copies
- Profiling (no `perf` on WSL)
- Soak run
- `kq_bench --rate` run
- Other kernels, architectures, bare metal
- A deterministic test of the interrupted-after-wake hand-off

## Last successful build
2026-10-25, re-run during this inspection: `make user` (0 warnings,
`-Werror`), `make module` (W=1, 0 warnings, vermagic
`6.6.87.1-microsoft-standard-WSL2`), and `make sparse` (clean). checkpatch:
the driver and headers are 0/0; `tests/debugcheck/kq_debugcheck.c` has 1
intentional `volatile` warning.

## Last successful tests
- **2026-10-25, no driver needed:** `make test-unit` 5/5; `make test-harness`
  4 passes + 7/7 faults detected; `make sanitize` (ASan+UBSan) clean.
- **Stock kernel, run 3** (`docs/evidence/run3-v2`): functional 4 × 27/27,
  kqctl 13/13, compat32 PASS, stress 16/16, bad parameters 3/3 rejected,
  clean log.
- **KASAN debug kernel** (`debug-run2`): full cycle PASS, `debug_locks=1`,
  3/3 checker self-tests detected.
- **Strict-KCSAN kernel** (`kcsan-run2`): full cycle PASS; 0 of 168 KCSAN
  reports with a racing access in kernqueue. KCSAN mutation check: 15
  attributed races (`kcsan-mutant`).

## Failed tests and known issues
- **KCSAN `data_race` self-test never reports** (2 attempts; cause unknown).
  It's informational; the mutation check is the required proof.
- **KCSAN run 1's device-cycle verdict was FAIL** because of the first,
  coarse attribution rule. That rule was replaced; on replay, 0 races were
  attributed to kernqueue.
- **KASAN debug run 1's `lock_order` self-test was missed:** lockdep had
  already turned itself off. The order was fixed, and run 2 passed.
- **Performance trade-offs (measured):**
  - exclusive wake-ups are ≈5× slower when capacity equals the record size
  - 8–11% slower for a single producer and consumer
  - 2.7× faster under 16×1 or 1×16 contention
- **Suspected, unmeasured:** spurious `poll` wake-ups from the wake and
  hand-off calls.
- **No confirmed driver correctness defect.**

## Environment limitations
- Every device test needs a human to type the WSL sudo password.
- Booting a debug kernel needs `%UserProfile%\.wslconfig` + `wsl --shutdown`.
  It's currently **reverted**: no `.wslconfig`, stock kernel running.
- WSL VM boot timeout (the heavy KCSAN config never booted), and one hang
  after a KCSAN session.
- No `perf`. Single VM, so cross-session timing varies by up to 2×.
- No kernel headers ship for WSL; the module builds against
  `~/kq-kbuild/linux`.

## Git
- **Branch:** `main`, in sync with `origin/main` (private remote), at
  `5ec6551`.
- **Uncommitted:** only the two new files `docs/PROJECT_HANDOFF.md` and
  `docs/PROJECT_STATUS.md` (untracked).

## Resume development
From WSL Ubuntu, in the repository:
```bash
make user && make module && make test-unit && bash scripts/run_device_tests.sh
```
The last command needs sudo and loads the module.

## Next three actions
1. **Cut the uncontended wake cost:** wake only on transitions, or skip
   wakes with no sleepers (`wq_has_sleeper`). Then same-session
   `scripts/ab_wake.sh` plus a benchmark, the full device cycle, and a
   debug-kernel cycle.
2. **Profile the minimum-capacity regression** with `perf` on bare metal or
   in QEMU, and consider an adaptive wake strategy.
3. **Run on a second platform** (native Linux or QEMU, another kernel
   version, ideally arm64) with a soak run (`KQ_SOAK_SECONDS=1800`).

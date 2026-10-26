# KernQueue final engineering audit

The audit was performed 2026-10-25 to 2026-10-27 against commit `6f3cdb2`
(branch `main`). It covers the environment, a baseline from a fresh clone, a
code-level correctness and concurrency review, test coverage, repository
hygiene, benchmark validity and documentation.

**Labels used throughout:**
- **[AUDIT]**: run and observed during this audit.
- **[HISTORICAL]**: recorded in `docs/evidence/` or `bench/results/` before
  the audit, and not re-run here.
- **[NOT RUN]**: not executed; the reason is given.

---

## 1. Environment and baseline

| Item | Value [AUDIT] |
|---|---|
| Platform | WSL2 (Hyper-V VM) on Windows 11, AMD Ryzen 7 5800H, 16 vCPUs, 6.7 GiB |
| Distribution | Ubuntu 24.04.4 LTS |
| Kernel | `6.6.87.1-microsoft-standard-WSL2`, x86_64 (the only kernel the project targets and was tested on) |
| Kernel headers | `/lib/modules/$(uname -r)/build` **absent**; the module builds against `~/kq-kbuild/linux` (the `linux-msft-wsl-6.6.87.1` tag built by `scripts/prepare_wsl_kdir.sh`, with `Module.symvers` present) |
| Compilers | gcc 13.3.0 (user space), gcc-11 11.5.0 (module, matching the kernel's gcc 11) |
| Tools | GNU Make 4.3, git 2.43.0, pahole 1.25, Python 3.12.3, sparse 0.6.4 (user-local), checkpatch.pl from the tree |
| Privilege | `sudo` requires a password (`sudo -n` fails). Device tests need a human at a terminal |
| Module state at start | not loaded; stock kernel; no `.wslconfig` |

**Driver provenance.** `git log -- driver/ include/` shows a single commit,
`e1f68c3`. The driver and uapi sources have not changed since the first
commit, so the historical device and sanitizer evidence was produced with the
exact driver source audited here.

### Baseline: fresh clone into ext4 (`/tmp/kq-audit-clone`) [AUDIT]

A clone on WSL's native filesystem reproduces what a new user gets: real
file modes, and no `/mnt/c` behaviour that makes every file look executable.

| Command | Result |
|---|---|
| `git ls-files -s` modes | all 16 shell/Python scripts are `100755`; no file with a shebang lacks the exec bit |
| `make user` | exit 0, **0 warnings** (`-Wall -Wextra -Wpedantic ... -Werror`) |
| `make test-unit` | `test_ring: 5 tests, 0 failed` |
| `make test-harness` | 4 expected passes, **7/7 injected faults detected** |
| `make sanitize` (ASan+UBSan build of the two above) | exit 0, no sanitizer report |
| `make module` (path without spaces, so an in-tree Kbuild build) | exit 0, 0 warnings at `W=1`, vermagic `6.6.87.1-microsoft-standard-WSL2 SMP preempt mod_unload modversions` |
| `make sparse` (`C=2`) | exit 0, no warnings |
| `checkpatch.pl --no-tree` | `kernqueue.c`, `kq_ring.h`, `kernqueue.h`: 0 errors / 0 warnings. `tests/debugcheck/kq_debugcheck.c`: 1 warning (the intentional `volatile` in the KASAN self-test) |
| `git status` after all builds | clean: every artifact is ignored |
| `make clean` | removes `build/` and every in-tree Kbuild artifact (no ignored files remain) |

**Historical reports reproducible here without privileges:** build, unit test,
harness self-test, ASan/UBSan, sparse, checkpatch (all re-run above).

**Reports that need module loading:** functional, stress, kqctl, compat32,
benchmark, A/B. See §5 for what was re-run.

**Reports that need a debug kernel:** lockdep/KASAN and KCSAN runs. These
were **[NOT RUN]** in this audit: they need a `.wslconfig` kernel swap
(~1 h), and the driver source they validated is unchanged. They remain
**[HISTORICAL]**.

---

## 2. Correctness and concurrency review

Every operation was traced from the system call through the VFS into
`driver/kernqueue.c` and back. Line numbers refer to the audited source.

### 2.1 Ring buffer (`driver/kq_ring.h`)
- **Representation.** `head`, `tail`, `count`, `cap`, with invariants I1–I4
  checked by `kq_ring_valid()` after every transfer, under the lock
  (`kq_check_ring`, `kernqueue.c:202`). Empty and full both have
  `head == tail` and are told apart by `count` (`test_full_vs_empty`).
- **Wrap-around.** `kq_ring_split()` (`:91`) gives `len[0] = min(cap - start,
  n)` ≥ 1 for every `n ≥ 1`, because `start < cap`. So there is never an empty
  first span followed by a non-empty second one. `kq_ring_advance()` (`:78`)
  can't overflow for `cap ≤ SIZE_MAX/2`, and capacity is capped at 16 MiB.
- **Record sizes.** Writes ≤ `KQ_ATOMIC_MAX` (4096) need `count` free bytes,
  larger writes need 1 (`kq_write_need`, `:87`). `KQ_MIN_CAPACITY ==
  KQ_ATOMIC_MAX` guarantees every atomic write fits an empty ring, so no
  atomic write can wait forever for space that can never exist. Writes larger
  than capacity become short writes.
- **Invalid sizes.** `count == 0` returns 0 without touching memory. A user
  `count` is clamped with `min()` before arithmetic (`:333`, `:380–382`). The
  VFS already limits `count` to `MAX_RW_COUNT`.
- **Partial transfers.**
  - read: consumes exactly the copied bytes; `-EFAULT` only when 0 bytes were
    copied
  - atomic write: all or nothing (`:293–294`)
  - non-atomic write: the copied prefix is published

  State (`head`/`tail`/`count`) changes only *after* the user copy, under the
  lock.
- **Semantics vs documentation.** `README.md`, `docs/design.md` §1 and
  `include/kernqueue.h` match the code.

### 2.2 Synchronisation
- **Locking.** Every read or write of `head`/`tail`/`buf`/`stats`/
  `open_files` happens with `kq.lock` held. `count` is written only under the
  lock (`KQ_WRITE_ONCE`) and read locklessly only through `READ_ONCE` as a
  hint (`:95`, `:100`, `:411`).
- **Return paths.**
  - `kq_read`: 3 unlock sites (`:315`, `:319`, `:338`); the bail-out at
    `:326` returns with the lock *not* held, because the failed
    `mutex_lock_interruptible` didn't take it.
  - `kq_write`: same structure.
  - `kq_get_stats`, `RESET`, `CLEAR_STATS`: lock/unlock around a straight-line
    section.

  No path returns with the mutex held, and none unlocks twice.
- **Waking.** Waiters queue *before* testing the condition. Wakers call
  `wake_up_*` unconditionally, after the state change and after unlocking.
  This ordering through the wait-queue spinlock prevents lost wake-ups: the
  waker's `count` store is ordered before its wait-queue unlock, and a waiter
  that queues later synchronises with that unlock.
- **Re-checking.** Conditions are re-checked under the mutex after every wake
  (`while` loops at `:312` and `:362`).
- **Signals.**
  - Interruptible waits and interruptible mutex acquisition return
    `-ERESTARTSYS`, which becomes `EINTR` or a restart.
  - `kq_release` uses `mutex_lock` (it can't fail).
  - A fatal signal (`SIGKILL`) is delivered through the same interruptible
    waits. Covered by the new tests in §4.
- **RESET and stats.** Both run under the mutex, so they are serialised
  against copies in progress. `RESET` wakes writers (space freed) and not
  readers (no data). Stats are snapshotted under the lock and copied out
  after unlocking.

### 2.3 Exclusive wake-up and hand-off (`exclusive_wake=1`)
- **Who is woken.**
  - Readers: `wait_event_interruptible_exclusive` (`:140`); any exclusive
    reader can use any data.
  - Writers: an open-coded exclusive wait (`:176–199`) whose wake function
    `kq_writer_wake` (`:159`) returns 0 unless `cap - count >= need` for
    *that* writer. In 6.6 `__wake_up_common()` counts an exclusive entry
    against the wake-one budget only when its wake function returns
    non-zero, so the wake-up goes to the first waiter that can proceed.

  A wake-up therefore can't be spent on a writer that can't progress *at
  wake time*. The writer still re-checks under the lock, because another task
  may take the space first.
- **Variable record sizes.** Covered by the need check above. Test
  `wake_skips_unfit_writer`.
- **Signals.** All bail-out paths after a wait (`:326–330`, `:371–376`) call
  `kq_pass_on_*`. Inside `kq_wait_for_space` the condition is tested before
  the signal (`:189–194`), matching `___wait_event`'s "an exclusive waiter
  chosen by a wake-up must not fail" rule.
- **Stranding.**
  - Every event that adds data (write) or space (read, `RESET`) is followed
    by a wake evaluation.
  - Every task that leaves data or space behind hands the wake-up on: after a
    read with data left (`:342`); after any write with ≥ 1 byte free
    (`:391`).
  - A woken task that finds the resource gone doesn't hand on, and doesn't
    need to: the task that took the resource does.
- **Starvation and chains.** A task that can't proceed never wakes anyone,
  so there's no ping-pong. Every hand-off follows a state change or a
  bail-out. A large atomic writer can be starved by a stream of small
  writers. This is a fairness property, not a liveness bug, and it holds in
  **both** wake modes.
- **Shared waiter state.** `struct kq_writer_wait` lives on the writer's
  stack and is only read by wakers under the wait-queue lock while the entry
  is queued. `finish_wait()` removes it before the frame dies.
- **Equivalence of the two modes.** Functionally equivalent: the same
  conditions and the same re-checks. With `exclusive_wake=0` the hand-offs are
  no-ops and every waiter is woken. They differ only in which waiter runs
  first, and in performance.

### 2.4 User/kernel boundary, poll, ioctl, lifecycle
- **User copies.** Only `copy_to_user`/`copy_from_user` touch user memory.
  Their return values are used exactly (`done += len - left`). No queue state
  is modified before a copy that might fail.
- **ioctl.**
  - An unknown type or number gives `-ENOTTY` (`:451`, `:484`).
  - `GET_STATS` copies a 96-byte, all-`__u64` snapshot (no padding).
  - `RESET` and `CLEAR_STATS` need `FMODE_WRITE`, otherwise `-EBADF`.
  - `compat_ptr_ioctl` is correct because the only argument is a pointer to
    a layout-identical struct (`_Static_assert` in `tests/test_compat32.c`).
- **poll.**
  - `EPOLLIN` on a readable fd when `count > 0`.
  - `EPOLLOUT` on a writable fd when `cap - count ≥ 4096`.
  - It registers on both queues before sampling.
  - It never reports `EPOLLHUP`/`EPOLLERR`: the queue has no notion of
    producers or consumers being present. That's documented here as API
    behaviour, unlike pipes.
- **Lifecycle.**
  - Init unwinds in reverse with `cdev_add` last (`:506–573`).
  - Exit is safe because `fops.owner` pins the module while any file is
    open.
  - epoll entries are removed in `eventpoll_release()` before `->release()`,
    so no waiter can still be on `readq`/`writeq` at `kq_exit`.

### 2.5 Findings

No critical or high-severity defect was found.

| # | Severity | Finding | Status |
|---|---|---|---|
| F1 | Medium (design risk) | User copies run with `kq.lock` held, so a `userfaultfd`- or slow-mmap-backed buffer can stall every other client (DoS by a local user with device access) | Documented, not fixed: changing it needs a bounce buffer and measurement. Pipes share the property |
| F2 | Medium (claim validity) | The documented headline "`exclusive_wake=1` is 2.7× faster at 16×1 and 1×16" **did not reproduce** in the audit rerun: 16×1 was 1.05× (ranges overlap) and 1×16 was 0.86× (exclusive slower). What did reproduce: fewer writer sleeps and lower writer p99 latency at 16×1, faster 16×1 integrity-stress runs, ~5× slower throughput when the ring holds one record, and a small single-producer cost (§5, §6) | Claims corrected in all docs; the historical numbers are kept and labelled. No driver change: the rerun shows a claim problem, not a defect |
| F3 | Low (suspected inefficiency) | After every read (`kq_wake_writers`) and every write (`kq_pass_on_writers`), all `poll` waiters on the queue are woken even when `EPOLLOUT` still doesn't hold; they re-sample and sleep again | Not visible to user space. The new test `poll_out_threshold` checks that such wake-ups never make `poll()` return early. Cost not measured |
| F4 | Low (API interaction) | With `exclusive_wake=1`, an epoll waiter registered with `EPOLLEXCLUSIVE` sits in the same exclusive section of the wait queue and can absorb the single wake-up meant for a blocked reader or writer; if that process doesn't act, the blocked task waits for the next event. Linux pipes behave the same | Documented; reasoning only, no reproducer |
| F5 | Low (fairness) | Large atomic writers can be starved by small writers (both modes) | Documented |
| F6 | Low (test strength) | `wake_skips_unfit_writer` relies on the big writer being first in the wait queue; whether the three exclusive-wake tests can fail had never been shown | Addressed by the new mutation check (§3, §5) |
| F7 | Low (coverage) | Untested: a reader blocked across `RESET`; `RESET` waking a `POLLOUT` poller; the exact `POLLOUT` threshold transition; a process killed while blocked; `RESET` racing with active I/O | Tests added (§3) |
| F8 | Medium (hygiene) | 16 committed evidence logs contained machine-specific absolute paths and the account username | Fixed (§3); Git history still contains them |
| F9 | Info (open) | The "woken exclusively, then interrupted before taking the mutex" hand-off can't be forced deterministically without privileged `userfaultfd` | Open; covered by reasoning only |
| F10 | Info (tool) | The synthetic KCSAN `data_race` self-test never reported under WSL ([HISTORICAL]) | Open; the KCSAN liveness proof is the mutation check ([HISTORICAL]) |
| F11 | Low (benchmark method) | The `--bench` matrix runs all kq-excl configurations (plus FIFO) first and kq-all about 5 minutes later, so a kq-all vs kq-excl comparison from the matrix is exposed to drift between the two blocks. Only `scripts/ab_wake.sh` interleaves the modes | Documented (§6). A plausible contributor to F2, not a demonstrated cause |
| F12 | Low (test infrastructure) | `wake_mutant_check.sh` and `ab_wake.sh` don't capture or scan the kernel log; only `run_device_tests.sh` does. The VM had restarted before collection, so the kernel log for those two phases of the audit run is lost | Documented; recommended follow-up (§8) |

---

## 3. Changes made

There are **no driver changes**: no defect justified one.

| File | Change | Why |
|---|---|---|
| `tests/test_functional.c` | +6 tests: `reset_keeps_reader_blocked`, `reset_wakes_poll_writer`, `poll_out_threshold`, `killed_blocked_reader`, `killed_blocked_writer`, `reset_under_load` (27 → 33 tests); includes `<sched.h>`, `<sys/prctl.h>`, `<sys/wait.h>` | Coverage gaps F7 and F3 |
| `scripts/wake_mutant_check.sh` | New script: builds 3 wake-logic mutants (need filter removed; reader hand-off removed; writer hand-off removed) and requires each targeted test to fail 3/3, with the real driver as control | F6: prove the regression tests can fail |
| `docs/evidence/**` (16 existing files) | Literal substitutions only: the checkout path → `<repo>`, `/home/<user>` → `<home>`, `root <user-group> ` → `root <group> `. Verified byte-identical to the committed file with only these three substitutions applied | F8 |
| `docs/evidence/audit-device-cycle/` (17 files), `audit-wake-mutant/` (19), `audit-ab-min-capacity/` (4), `audit-device-run.log` | New: this audit's raw device logs, with the same three substitutions. Verified identical to the originals in `build/logs/` apart from them | Evidence for §5 |
| `bench/results/20261026-200144/` | New: the audit's benchmark matrix (`results.csv`, `summary.md`, `environment.txt`; contained no paths) | Evidence for §5.4 |
| `docs/FINAL_AUDIT.md` | New (this file) | Deliverable |
| `docs/PROJECT_STATUS.md`, `docs/PROJECT_HANDOFF.md` | Updated to the post-audit state | Deliverables B, C |
| `docs/benchmark_results.md` | Both sessions side by side, the correction notice, "what holds across sessions", the ordering caveat | F2, F11 |
| `docs/interview_prep.md` | Résumé bullets and answers revised: no 2.7× claim; 33 tests; 54M records / 133 runs; the wake mutation check; the KCSAN-mutant wording fixed to what was run (stress, not the functional suite) | F2 |
| `docs/testing.md` | 33 tests; a new row for `wake_mutant_check.sh`; notes that neither it nor `ab_wake.sh` scans `dmesg` | F6, F12 |
| `docs/implementation_status.md` | Audit run section, new totals, checks #22–#23; the account name in the run 1 node listing replaced with `<group>` | F8 and the record |
| `README.md` | Limitations (audit tests are stock-kernel-only; not production or security-audited) and links to the audit, status and handoff docs | Accuracy |

`driver/`, `include/`, `user/`, the `bench/` sources, the `Makefile` and every
other test and script are unchanged.

---

## 4. Tests executed in this audit

User space and build, all **[AUDIT]**: see the §1 table (fresh clone) and §5.

**New tests built** with the strict flags and with ASan/UBSan
(`make SANITIZE=address,undefined build/san/test_functional`): 0 warnings.
Run without the module, the suite fails (`open(/dev/kernqueue): No such file
or directory`, exit 1), so it never skips. Each mutation `sed` was checked to
change exactly one line of the real source.

Device-level tests were run once, as one chained command in a terminal where
the user typed the sudo password, on the stock kernel listed in §1:

```
scripts/run_device_tests.sh --bench && scripts/wake_mutant_check.sh && scripts/ab_wake.sh 4096 4 4 4096 3
```

All three steps exited 0. The results are in §5. The full transcript is
`docs/evidence/audit-device-run.log`.

---

## 5. Device-level results [AUDIT]

2026-10-26, 20:00–20:11 UTC, stock `6.6.87.1-microsoft-standard-WSL2`. The
module was built by `scripts/build_module.sh` with gcc-11 at `W=1`: 0
warnings, module BTF regenerated against the running kernel.

### 5.1 Device test cycle (`docs/evidence/audit-device-cycle/`)

| Step | Observed result |
|---|---|
| functional, capacity 65536, `exclusive_wake=Y` | **33/33 PASS** |
| functional, capacity 65536, `exclusive_wake=N` | **33/33 PASS** |
| functional, capacity 4096 (minimum), `exclusive_wake=Y` | **33/33 PASS** |
| functional, capacity 16 MiB (maximum), `exclusive_wake=Y` | **33/33 PASS** |
| the 6 new tests, in each of the 4 configurations | PASS (`reset_under_load` 1.00–1.08 s, `poll_out_threshold` 0.20–0.22 s, the rest ≤ 0.12 s) |
| `test_kqctl` | 13/13 ok, PASS |
| `test_compat32` (32-bit ioctl ABI) | PASS |
| `test_stress`, 7 configurations × 2 wake modes | **14/14 PASS**, every record verified |
| `test_stress` at capacity 4096: 4×4 4 KiB blocking, 4×4 1000 B non-blocking | **2/2 PASS** |
| invalid `capacity=100`, `4095`, `16777217` | **3/3 rejected** by `insmod` with the expected message |
| benchmark matrix, both wake modes plus FIFO | completed, exit 0 (§5.4) |
| kernel-log scan from the run's marker | **no unexpected lines**. `dmesg-filtered.log` holds the 17 lines it inspected: load/unload messages, the three rejections, the marker, and one unrelated `systemd-journald` time-jump line |

**Verdict printed by the script:** `DEVICE TEST CYCLE: PASS`. That makes 132
functional-test executions and 16 integrity-stress runs, with no failure.

### 5.2 Wake mutation check (`docs/evidence/audit-wake-mutant/`)

| Build | Targeted test | Required | Observed |
|---|---|---|---|
| control (real source) | all three | pass | 3/3 pass (`wake_skips_unfit_writer` 54.0 ms) |
| M1: need filter in `kq_writer_wake` replaced by `if (0)` | `wake_skips_unfit_writer` | fail 3/3 | **failed 3/3** (thread did not finish within 2000 ms) |
| M2: reader hand-off in `kq_pass_on_readers` disabled | `wake_hands_on_to_next_reader` | fail 3/3 | **failed 3/3** (same) |
| M3: writer hand-off in `kq_pass_on_writers` disabled | `wake_hands_on_to_next_writer` | fail 3/3 | **failed 3/3** (same) |

**Verdict:** `WAKE MUTANT CHECK: PASS`. Each `M*.diff` shows a one-line change.
The M1 build prints `-Wunused-variable` for `w`, which is expected: the mutation
removes `w`'s only use. It also prints a make clock-skew warning, because the
staged copy's timestamp is slightly ahead of the VM clock. Both concern the
mutant build only.

### 5.3 Same-session A/B at minimum capacity (`docs/evidence/audit-ab-min-capacity/`)

`ab_wake.sh 4096 4 4 4096 3`: capacity 4096 (one 4 KiB record), 4 writers × 4
readers. The modes alternate within each repetition.

| | kq-all (`exclusive_wake=0`) | kq-excl (`exclusive_wake=1`) |
|---|---|---|
| stress, 200,000 × 4 KiB records | 3.42, 3.28, 3.28 s, all PASS | 6.28, 6.53, 8.27 s, all PASS |
| benchmark read MB/s, median [min–max] | **441.8** [433.1–442.6] | **85.5** [76.6–86.7] |
| write() p50 / p99 µs | 13.6 / 261.1 | 191.5 / 352.3 |
| [HISTORICAL] 2026-10-25, same script | 482.1 [481.0–484.5] | 97.3 [97.3–97.5] |

Exclusive mode was **5.2× slower** here, against 5.0× historically, and the
ranges don't overlap. **This trade-off reproduced.**

### 5.4 Benchmark matrix rerun (`bench/results/20261026-200144/`)

Same script, parameters and binary as the historical run
(`bench/results/20261025-010601/`): capacity 65536, 1 s warm-up, 5 s
measured, 3 repetitions, median [min–max] read MB/s.

| W×R, size | [HISTORICAL] kq-all | [HISTORICAL] kq-excl | ratio | [AUDIT] kq-all | [AUDIT] kq-excl | ratio |
|---|---|---|---|---|---|---|
| **16×1, 4096** | 150.8 [135.8–184.7] | 401.1 [400.4–404.5] | **2.66×** | 312.4 [294.9–319.3] | 327.4 [288.2–369.6] | **1.05×, ranges overlap** |
| **1×16, 64** | 2.2 [2.1–2.4] | 5.9 [5.8–5.9] | **2.68×** | 5.7 [5.0–5.7] | 4.9 [4.9–5.2] | **0.86×** |
| 1×1, 64 | 80.8 | 73.5 | 0.91× | 69.6 | 68.9 | 0.99× |
| 1×1, 64, non-blocking | 81.1 | 74.0 | 0.91× | 73.1 | 67.7 | 0.93× |
| 1×1, 512 | 636.6 | 583.4 | 0.92× | 557.3 | 524.1 | 0.94× |
| 1×1, 4096 | 4720.3 | 4220.3 | 0.89× | 4114.1 | 3876.2 | 0.94× |
| 4×4, 64 | 99.5 | 96.1 | 0.97× | 89.9 | 89.4 | 0.99× |
| 4×4, 4096 | 4705.5 | 4571.5 | 0.97× | 3544.3 [3410.9–3779.6] | 3764.2 [3685.5–3805.6] | 1.06×, overlap |
| 8×8, 512 | 541.1 | 551.8 | 1.02× | 396.1 [372.7–414.6] | 469.7 [389.4–474.3] | 1.19×, overlap |

Wake-up and latency metrics at 16×1, 4096 B:

| Metric | [HISTORICAL] all → excl | [AUDIT] all → excl |
|---|---|---|
| writer sleeps | 587,600 → 333,951 (**1.76× fewer**) | 535,960 → 290,550 (**1.84× fewer**) |
| write() p99 µs, median [min–max] | 2974.5 [2665–2978] → 1133.0 [1102–1172] (**2.6×**) | 2266.8 [2232–2273] → 1259.9 [1161–1463] (**1.8×**) |
| end-to-end p99 µs | 3595.6 → 1285.1 | 2702.4 → 1462.9 |
| write() p50 µs | 171.4 → 131.4 | 7.6 → 147.9 |

The p99 ranges don't overlap in either session. The p50 moved in opposite
directions between sessions.

At 1×16, reader sleeps fell 478,325 → 386,676 (historical) and
464,429 → 326,121 (audit), but throughput didn't follow.

### 5.5 Integrity-stress timings across sessions

`test_stress -w 16 -r 1 -n 20000 -s 4096` (320,000 records, 1.25 GiB), one run
per mode per session. All runs PASS. The audit row is [AUDIT]; the rest are
[HISTORICAL], made with the same driver source.

| Session | Kernel | excl s | all s | all ÷ excl | writer sleeps excl / all |
|---|---|---|---|---|---|
| run 2 | stock | 4.84 | 29.04 | 6.0× | 167,313 / 1,271,020 |
| run 3 | stock | 4.91 | 26.71 | 5.4× | 165,369 / 1,133,832 |
| **audit** | stock | **5.05** | **21.79** | **4.3×** | **168,235 / 901,511** |
| debug 1 | KASAN + lockdep | 9.98 | 35.56 | 3.6× | 176,808 / 985,546 |
| debug 2 | KASAN + lockdep | 9.50 | 39.53 | 4.2× | 175,157 / 1,228,007 |
| KCSAN 1 | KCSAN | 7.18 | 31.96 | 4.5× | 178,069 / 1,392,975 |
| KCSAN 2 | KCSAN | 7.31 | 34.01 | 4.7× | 180,095 / 1,483,913 |

`test_stress -w 1 -r 16 -n 200000 -s 32` gave no consistent direction on the
stock kernel: excl/all = 2.24/2.33 s, 2.29/4.73 s, and **2.54/1.14 s** in the
audit (exclusive slower).

---

## 6. Benchmark validity

**Code review of `bench/kq_bench.c` and `bench/run_bench.sh`** [AUDIT]:
- **Equivalent work.** kq-all and kq-excl use the same binary, the same
  parameters, and a freshly loaded module (only `exclusive_wake` differs),
  and the device is `RESET` at the start of every run. The FIFO baseline runs
  the same binary on a named pipe. Semantics are comparable (atomic writes ≤
  4096, blocking, poll), but a pipe stores 4 KiB writes in page-sized slots,
  so its capacity in records differs for small and large records.
- **Parameters.** 9 configurations, 64/512/4096 B records, 1 s warm-up, 5 s
  measured, 3 repetitions, unchanged since the historical run.
- **Metrics.**
  - Throughput is reader bytes completed during the measured window divided
    by its length.
  - Operations that straddle a phase boundary are mis-attributed by at most
    one per thread, which is negligible against 10⁵–10⁷ operations.
  - Syscall latency is timed around each `read`/`write`.
  - End-to-end latency uses a timestamp taken just before `write()`, so it
    includes writer blocking and queueing. Unthrottled, it measures queue
    depth (≈ capacity ÷ throughput), not driver overhead.
- **Percentiles.** From per-thread reservoir samples (200,000 per metric),
  merged across threads. They are biased when threads complete very
  different numbers of operations. Max is exact.
- **Errors.** Any failed or short transfer exits the benchmark with status 1,
  and `run_bench.sh` runs under `set -e`, so no partial metrics are recorded.
- **Stability.** Within one session, repetition ranges were mostly a few
  percent. Across sessions the same configuration varied by up to ~2× (the
  historical stress timings show this). **Only same-session comparisons are
  meaningful.** No statistical-significance testing was done; 3 repetitions
  are too few for that.
- **Ordering (F11).** `run_device_tests.sh --bench` loads `exclusive_wake=1`
  and runs every kq-excl configuration plus FIFO, then reloads with
  `exclusive_wake=0` and runs every kq-all configuration. A kq-all vs kq-excl
  ratio from the matrix therefore compares two blocks about 5 minutes apart.
  `ab_wake.sh` alternates the modes within each repetition and is the more
  trustworthy A/B, but it was run only for the minimum-capacity case.

### 6.1 What the rerun shows

- **Session drift is large.** Between the two sessions, FIFO 8×8 512 B fell
  642.8 → 357.5 MB/s and kq-all 16×1 rose 150.8 → 312.4 MB/s, with the same
  code and parameters. The cause wasn't determined: VM placement, host load,
  thermal state, and the block ordering above are all candidates, and none
  was isolated.
- **The 2.7× throughput claim doesn't reproduce** (F2). In the historical
  session kq-all was unusually slow at 16×1 and 1×16; in the audit session it
  wasn't. The ratio is therefore a property of one session, not of the
  design. It has been removed from every claim. The historical rows stay in
  `docs/benchmark_results.md`, labelled.
- **What reproduced in every session that measured it:**
  1. At 16×1, 4 KiB, exclusive mode cut writer sleeps (1.76×, 1.84×) and
     writer `write()` p99 latency (2.6×, 1.8×), with non-overlapping
     repetition ranges in both benchmark sessions.
  2. The 16×1 integrity-stress run finished 4.3–6.0× faster on the stock
     kernel in all three sessions (one run each), and 3.6–4.7× faster on the
     four debug-kernel sessions, with 5.4–7.6× fewer writer sleeps on the
     stock kernel.
  3. With the ring holding one record (capacity 4096, 4×4, 4 KiB),
     exclusive mode was 5.0× and 5.2× slower, measured with the interleaved
     A/B script in both sessions.
  4. Exclusive mode was slower in every 1×1 row of both sessions (1–11%). The
     block ordering can't be excluded as a cause, because no interleaved 1×1
     A/B was run.
- **Not consistent, so not claimed:** throughput at 16×1, 1×16, 4×4 4 KiB and
  8×8 512 B, and the 1×16 integrity-stress time.
- **Against pipes**, in both sessions: the FIFO was faster at 16×1 (2.7× over the
  best kernqueue mode in both) and at 1×16 (3.2×, 3.4×). kernqueue was faster
  at 4×4 4 KiB (2.0× in both). kernqueue is not claimed to be faster than pipes in
  general.

### 6.2 Defensible benchmark claims

On one machine (WSL2, Ryzen 7 5800H, kernel 6.6.87.1), with the same driver
source:

- "Exclusive, need-aware wake-ups cut writer sleeps ~1.8× and writer p99
  latency 1.8–2.6× under 16-writer/1-reader contention with 4 KiB records, in
  two benchmark sessions."
- "A 16-writer integrity stress run (320K 4 KiB records) completed 4.3–6.0×
  faster than with wake-all, across three sessions."
- "The cost: ~5× lower throughput when the ring holds a single record, and a
  1–11% single-producer slowdown."

None of these generalise beyond this machine and kernel, and none is a
statistically tested result.

---

## 7. Tests not executed [NOT RUN]

| Test | Why not | Consequence |
|---|---|---|
| lockdep + DEBUG_ATOMIC_SLEEP + KASAN, and strict-KCSAN device cycles | Each needs a `.wslconfig` kernel swap and restart (~1 h). The driver source they validated is unchanged | The historical runs stand for the 27 original tests. **The 6 new tests, including `killed_blocked_*` and `reset_under_load`, have never run under a debug kernel** |
| Kernel-log scan during the wake-mutant and A/B phases | Those scripts don't capture `dmesg` (F12), and the VM restarted before collection | No evidence either way for those phases. The device-cycle phase was scanned and clean |
| Soak run (`KQ_SOAK_SECONDS`) | Not requested in any session, historical or audit | No long-duration evidence. The longest runs are about 40 s |
| Rate-limited latency benchmark (`kq_bench --rate`) | Not part of the matrix | The end-to-end latencies measure queue depth, not driver overhead (§6) |
| Interleaved A/B for 1×1, 16×1 and 1×16 | Only the minimum-capacity A/B was run | Matrix ratios for those cases carry the block-ordering caveat (F11) |
| Other kernels, distributions, architectures, bare metal | Only WSL2 6.6.87.1 x86_64 is available | Portability is unverified. `class_create(name)` requires 6.4 or later |
| "Woken exclusively, then interrupted before the mutex" path (F9) | Needs privileged `userfaultfd` to force it | Reasoning only |
| `userfaultfd` stall under the mutex (F1), and `EPOLLEXCLUSIVE` absorbing a wake-up (F4) | No reproducers written | Reasoning only |
| Fuzzing (syzkaller), smatch, Coccinelle | Not set up | Beyond sparse, checkpatch, the sanitizers and the tests, no static or fuzz coverage |
| Statistical significance testing | 3 repetitions are too few | Benchmark claims are stated as per-session observations |

---

## 8. Remaining risks and limitations

- **One environment.** Every result comes from one WSL2 VM on one laptop, with
  one kernel build. Nothing is known about other kernels or real hardware.
- **Not production-ready, and not security-audited.** F1 (copies under the
  mutex) lets one local client with device access stall the others. Access
  control is only the node's mode: `load_module.sh` sets `0660` with the
  user's group, and a plain `insmod` leaves `root:root 0600`. No threat model
  was written.
- **Concurrency correctness rests on review plus testing.** That means
  lockdep, KASAN and KCSAN on the 27 original tests ([HISTORICAL]), and stress
  and mutation testing ([AUDIT] and [HISTORICAL]). That isn't a proof of race
  freedom: KCSAN is sampling-based, and the stress runs are short.
- **Benchmarks are noisy across sessions** (§6.1). Only same-session,
  preferably interleaved, comparisons are meaningful.
- **Fairness.** Large atomic writers can be starved by small ones (F5).
- **Git history** still contains the unredacted paths from before F8. Removing
  them needs a history rewrite and a force-push, which this audit didn't do.

**Recommended follow-ups**, in priority order:
1. Run the 6 new tests under the KASAN/lockdep and KCSAN kernels.
2. Add a kernel-log capture and scan to `wake_mutant_check.sh` and
   `ab_wake.sh` (F12).
3. Run interleaved A/Bs for 1×1 and 16×1 before quoting any throughput ratio
   (F11).
4. Run a soak (`KQ_SOAK_SECONDS=1800`) on the stock kernel.

---

## 9. Readiness verdict

**Ready for résumé review**, with the corrected claims. The driver is small,
reviewed line by line, and passes every test that was run, with
fault-injection and mutation evidence that the tests can fail. The documents
now distinguish audit results from historical ones and drop the
non-reproducible 2.7× throughput figure. It is a well-tested learning
project, not production software.

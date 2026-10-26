# Implementation status

The truthful record of what exists, what was run, and what passed. "Verified"
means a test or tool was actually run on the environment below and passed.
Last updated: 2026-10-27 (final audit; see [FINAL_AUDIT.md](FINAL_AUDIT.md)).

## Environment

| Item | Value |
|---|---|
| Host | Windows 11 (10.0.26200), AMD Ryzen 7 5800H, 16 logical CPUs |
| Linux | WSL2 (Hyper-V VM), Ubuntu 24.04.4 LTS, 6.7 GiB RAM visible |
| Kernel | `6.6.87.1-microsoft-standard-WSL2` (SMP, `PREEMPT_DYNAMIC`, `MODVERSIONS`, `DEBUG_INFO_BTF`; no lockdep/KASAN/KCSAN) |
| Kernel tree | `microsoft/WSL2-Linux-Kernel` tag `linux-msft-wsl-6.6.87.1`, `/proc/config.gz`, built with gcc-11 11.5.0 |
| User-space compiler | gcc 13.3.0 |

Config differences between the built tree and the running kernel were
reviewed. All are toolchain version strings, capability probes, or ABI-neutral
options (`GCC_PLUGINS` with no plugin enabled, `GCC_ASM_GOTO_OUTPUT_WORKAROUND`).
See `scripts/prepare_wsl_kdir.sh`.

## Driver versions

| Version | Wake-up strategy | Status |
|---|---|---|
| v1 | every state change wakes every waiter | verified on device (run 1 below) |
| v2 (current) | `exclusive_wake` parameter: Y (default) wakes one *suitable* waiter and hands on; N is exactly v1 | verified on device in both modes (runs 2 and 3 below) |

## Features

| Feature | Implemented | Verified by (run 1 = v1 on device) |
|---|---|---|
| cdev registration, class/device node, goto unwinding, clean unload | yes | run 1: 7 load/unload cycles, node created as `crw-rw---- root:<group> 240:0` |
| module parameter `capacity` with range check | yes | run 1: 4096, 65536, 16777216 accepted; 100, 4095, 16777217 rejected with a log message |
| ring buffer invariants and wraparound | yes | `test_ring`; run 1: `wraparound_stream`, `partial_read_fault` (wrapped) at 3 capacities |
| `read`/`write`, safe user copies, partial transfers, EFAULT policy | yes | run 1: `bad_user_pointers`, `partial_read_fault`, `atomic_write_fault`, `large_write_partial_fault` |
| atomic writes ≤ 4096 B, short writes above | yes | run 1: `atomic_write_semantics`, `fill_to_capacity`; stress runs (no split record in 3.82M records) |
| blocking I/O, `O_NONBLOCK`/`EAGAIN`, fcntl toggling | yes | run 1: `blocking_*`, `nonblock_*` |
| signals (`-ERESTARTSYS` → EINTR / SA_RESTART) | yes | run 1: `signal_interrupts_read`, `signal_restarts_read` |
| `poll` readiness and wake-ups | yes | run 1: `poll_states`, `poll_wakeups`; non-blocking stress runs |
| ioctls GET_STATS / RESET / CLEAR_STATS, ENOTTY, EBADF, EFAULT | yes | run 1: `ioctl_invalid`, `reset_*` |
| 32-bit compat ioctl (`compat_ptr_ioctl`) | yes | run 3: `test_compat32` (freestanding i386 process) PASS: read/write, GET_STATS, ENOTTY, EFAULT via `compat_sys_ioctl()` |
| non-seekable stream (`ESPIPE`) | yes | run 1: `not_seekable` |
| exclusive, need-aware wake-ups with hand-off (v2) | yes | run 2: 3 targeted tests (`wake_*`) pass; stress in both modes; 16×1 4.84 s vs 29.04 s |
| `kqctl` client | yes | run 3: `scripts/test_kqctl.sh` **13/13** checks pass |
| benchmark with latency percentiles | yes | run 2: 81 runs (9 configs × 3 reps × 3 targets), see benchmark_results.md |

## Checks executed

| # | Check | Result |
|---|---|---|
| 1 | `test_ring` (5 tests, ~1.8M random ops, 9 capacities) | **PASS** |
| 2 | `test_ring` under ASan + UBSan | **PASS** |
| 3 | Mutation check: 4 hand-made `kq_ring.h` mutants | **4/4 detected** |
| 4 | User-space build, gcc 13.3, strict warnings + `-Werror` | **PASS** (0 warnings) |
| 5 | Harness self-test on a FIFO: 4 normal runs | **PASS** |
| 6 | Harness self-test: 7 injected faults (dup ×2, skip ×2, corrupt ×2, split) | **7/7 detected** |
| 7 | #5–#6 under ASan + UBSan | **PASS** |
| 8 | `test_stress` under TSan on a FIFO (blocking and non-blocking) | **PASS**, no races reported |
| 9 | Driver compile, gcc-11, Kbuild `W=1` (v1 and v2) | **PASS** (0 warnings) |
| 10 | `checkpatch.pl --no-tree` on driver + headers (v1 and v2) | **0 errors, 0 warnings** |
| 11 | Kernel tree build; `kernelrelease` equals `uname -r`; `Module.symvers` produced | **PASS** (after fixes, below) |
| 12 | `make module`: vermagic equals the running kernel | **PASS** |
| 13 | Imported-symbol CRCs vs. modules shipped with the running kernel | v1 **36/36**, v2 **39/39** match |
| 14 | `sparse` 0.6.4 over the driver (`make sparse`, Kbuild `C=2`) | **clean** (0 warnings) |
| 15 | sparse mutation check: `copy_to_user()` → `memcpy()` into a `__user` buffer | **detected** ("different address spaces") |
| 16 | `test_compat32` builds as an i386 binary; without the module it fails at check 1 (no false pass) | **as expected** |
| 17 | Device cycle under a lockdep + DEBUG_ATOMIC_SLEEP + KASAN kernel (2 boots) | **PASS** both times, no report; lockdep stayed on and tracked kernqueue's locks |
| 18 | Deliberate-bug self-tests under that kernel: lock order, sleep in atomic, out-of-bounds | **3/3 detected** (debug run 2) |
| 19 | Device cycle under a strict-KCSAN kernel (2 boots) | **PASS**; 0 of 351 printed KCSAN reports had a racing access in kernqueue |
| 20 | KCSAN mutation check: ring.count markings removed | **15 races in kernqueue reported**; stress still passed |
| 21 | `data_race` self-test (kthreads racing on a module static) under KCSAN | **not reported** in 2 attempts; unexplained, see above |
| 22 | Final audit, fresh clone: user build, module build, sparse, checkpatch, `test_ring`, harness, ASan/UBSan, `git status` clean, `make clean` complete | **PASS** (FINAL_AUDIT.md §1) |
| 23 | Wake-logic mutation check: 3 mutants, each targeted test must fail 3/3 | **3/3 mutants detected** (audit) |

### Device run 1: v1 driver (logs: [evidence/run1-v1](evidence/run1-v1/))

| Step | Result |
|---|---|
| functional, capacity 65536 | **24/24 PASS** |
| stress suite, capacity 65536 (7 configurations, below) | **7/7 PASS** |
| functional, capacity 4096 | **24/24 PASS** |
| stress, capacity 4096: 4×4 × 100k × 4096 B blocking; 4×4 × 100k × 1000 B non-blocking | **2/2 PASS** |
| functional, capacity 16 MiB | **24/24 PASS** |
| invalid `capacity` values rejected (100, 4095, 16777217) | **3/3** |
| kernel log scan (WARNING/BUG/Oops/lockdep/Call Trace) | **clean**, but see the BTF note below |

Stress configurations in run 1 (all with full integrity checks):

| W×R | records/writer | size | mode | time | driver sleeps r/w |
|---|---|---|---|---|---|
| 1×1 | 500,000 | 64 | blocking | 0.37 s | 1 / 0 |
| 4×4 | 200,000 | 64 | blocking | 0.52 s | 6,702 / 14,100 |
| 8×8 | 50,000 | 512 | blocking | 0.36 s | 4,699 / 23,850 |
| 16×1 | 20,000 | 4096 | blocking | **27.77 s** | 71 / **1,268,318** |
| 1×16 | 200,000 | 32 | blocking | **4.30 s** | **439,621** / 0 |
| 4×4 | 100,000 | 100 | non-blocking + poll | 0.48 s | 0 / 0 (EAGAIN 5,354 / 1,598) |
| 8×8 | 50,000 | 4096 | non-blocking + poll | 0.71 s | 0 / 0 (EAGAIN 62,964 / 5,062) |

The 16×1 and 1×16 rows are the **thundering herd** predicted in design.md: one
event wakes every sleeper, but only one can use it (~4 writer sleeps per
record in 16×1). This motivated v2.

### Device run 2: v2 driver, both wake modes (logs: [evidence/run2-v2](evidence/run2-v2/))

| Step | Result |
|---|---|
| functional, capacity 65536, `exclusive_wake=1` | **27/27 PASS** |
| stress suite, `exclusive_wake=1` (7 configurations) | **7/7 PASS** |
| functional, capacity 65536, `exclusive_wake=0` | **27/27 PASS** |
| stress suite, `exclusive_wake=0` (7 configurations) | **7/7 PASS** |
| functional, capacity 4096 (default mode) | **27/27 PASS** |
| stress, capacity 4096: blocking 4 KiB; non-blocking 1000 B | **2/2 PASS** |
| functional, capacity 16 MiB | **27/27 PASS** |
| invalid `capacity` values rejected | **3/3** |
| benchmark: both modes + FIFO, 9 configurations × 3 reps | completed, see benchmark_results.md |
| kernel log scan (now also `error`/`invalid`/`mismatch`) | **clean**; 0 BTF lines (BTF fix verified) |

Same-session wake-mode comparison from the stress suites:

| W×R, size | v2 `exclusive_wake=1` | `exclusive_wake=0` (v1 behaviour) |
|---|---|---|
| 16×1, 4096 B | **4.84 s**, writer sleeps 167,313 | 29.04 s, writer sleeps 1,271,020 |
| 1×16, 32 B | 2.24 s, reader sleeps 180,836 | 2.33 s, reader sleeps 242,910 |
| 8×8, 4096 B non-blocking | 0.69 s | 1.60 s |
| others | within ±0.05 s | |

### Minimum-capacity wake-mode A/B (logs: [evidence/ab-min-capacity](evidence/ab-min-capacity/))

6 integrity-checked stress runs (3 per mode, interleaved): **6/6 PASS**.
Exclusive wake-ups were slower in this configuration: 5.33–5.52 s vs
3.46–3.66 s, and 97 vs 482 MB/s in the benchmark. Analysis is in
benchmark_results.md. This is a documented performance trade-off, not a
correctness issue.

### Device run 3: v2 driver plus new tests (logs: [evidence/run3-v2](evidence/run3-v2/))

Same cycle as run 2 (without the benchmark), plus the two tests added after
it. The driver source is unchanged since run 2.

| Step | Result |
|---|---|
| functional, 4 configurations (default, wake-all, 4 KiB, 16 MiB) | **4 × 27/27 PASS** |
| `scripts/test_kqctl.sh` | **13/13** |
| `test_compat32` (32-bit process, compat ioctl path) | **PASS** |
| stress: both wake modes (7 + 7) and minimum capacity (2) | **16/16 PASS** |
| invalid `capacity` values rejected | **3/3** |
| kernel log scan | **clean** |

The herd fix replicated: 16×1 × 4 KiB took 4.91 s with `exclusive_wake=1`
and 26.71 s with `exclusive_wake=0` in this session (writer sleeps 165,369 vs
1,133,832).

### Debug-kernel run 1: lockdep + DEBUG_ATOMIC_SLEEP + KASAN (logs: [evidence/debug-run1-selftest-order](evidence/debug-run1-selftest-order/))

Kernel `6.6.87.1-microsoft-standard-WSL2-kqdebug` (`scripts/prepare_debug_kdir.sh`:
`PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`, `KASAN` generic + `KASAN_VMALLOC`,
`DEBUG_LIST`), booted through `.wslconfig`. Driver unchanged since run 2.

| Step | Result |
|---|---|
| lockdep enabled before the tests (`debug_locks=1`) | yes |
| full device cycle (functional ×4 configurations, kqctl, compat32, 16 stress runs, parameters, log scan) | **PASS**, no lockdep, KASAN, sleep-in-atomic or list-corruption report |
| lockdep still enabled after the cycle | **yes** (`debug_locks=1`), so it reported nothing about kernqueue |
| lockdep tracked kernqueue's locks | **yes**: `&kq.lock` (187 forward dependencies), `&kq.readq`, `&kq.writeq` |
| self-test `atomic_sleep` | **detected** by DEBUG_ATOMIC_SLEEP, and by lockdep as "Invalid wait context" |
| self-test `lock_order` | **not reported**: lockdep had already switched itself off after the `atomic_sleep` report |
| self-test `oob` | **detected**: `BUG: KASAN: slab-out-of-bounds in kqd_oob` |

**Failure analysis.** The `lock_order` miss was a test-ordering bug, not a
lockdep gap. Lockdep disables itself after its first report, and the
`atomic_sleep` self-test produces one ("Invalid wait context", because
`might_fault()` takes `mmap_lock` under the spinlock). **Fix:**
`run_debug_checks.sh` now runs `lock_order` first and asserts `debug_locks=1`
just before it. Lockdep's validation was demonstrably live in this run anyway:
it produced the invalid-wait-context report and tracked kernqueue's locks.

### Debug-kernel run 2: same kernel, corrected self-test order (logs: [evidence/debug-run2](evidence/debug-run2/))

Fresh boot of the same debug kernel.

| Step | Result |
|---|---|
| lockdep enabled before the tests | yes (`debug_locks=1`) |
| full device cycle: functional 4 × 27/27, kqctl 13/13, compat32, 16/16 stress, 3/3 parameters, log scan | **PASS**; no lockdep, KASAN, sleep-in-atomic or list report |
| lockdep after the cycle | still on (`debug_locks=1`); tracks `&kq.lock` (183 forward deps), `&kq.readq`, `&kq.writeq` |
| lockdep on immediately before `lock_order` | yes |
| self-test `lock_order` | **detected**: `WARNING: possible circular locking dependency detected` |
| self-test `atomic_sleep` | **detected**: `BUG: sleeping function called from invalid context` |
| self-test `oob` | **detected**: `BUG: KASAN: slab-out-of-bounds in kqd_oob` |
| overall | **DEBUG KERNEL CHECKS: PASS** |

Afterwards `.wslconfig` was removed, WSL restarted on the stock kernel
(`uname -r` = `6.6.87.1-microsoft-standard-WSL2`), and `build/kernqueue.ko`
was rebuilt for it (vermagic and symbol CRCs re-verified).

Under KASAN everything is slower (e.g. 16×1 × 4 KiB: 9.50 s exclusive vs
39.53 s wake-all, against 4.9 s / 26.7 s on the stock kernel), but the
relative effect of the wake-up strategy is the same.

### KCSAN runs: strict KCSAN + DEBUG_ATOMIC_SLEEP (logs: [evidence/kcsan-run1](evidence/kcsan-run1/), [evidence/kcsan-run2](evidence/kcsan-run2/))

Kernel `...-kqkcsan` (`scripts/prepare_debug_kdir.sh kcsan`: `KCSAN`,
`KCSAN_STRICT` (implies `KCSAN_WEAK_MEMORY`), `DEBUG_ATOMIC_SLEEP`). KCSAN is
switched on through debugfs before the tests. Driver unchanged since run 2.

| Step | Run 1 | Run 2 (fresh boot) |
|---|---|---|
| functional ×4 configs, kqctl, compat32, 16 stress runs, parameters | all pass | all pass |
| KCSAN activity during the cycle | 3.63M watchpoints, 90,024 races detected kernel-wide, 183 printed | 3.59M watchpoints, 85,156 detected, 168 printed |
| printed reports with a kernqueue frame anywhere on a stack | 125 | 127 |
| printed reports where kernqueue performed a racing access | **0** | **0** |
| device-cycle verdict | FAIL under the first (coarse) rule | **PASS** under the per-access rule |
| self-test `atomic_sleep` | detected | detected |
| self-test `data_race` (two kthreads racing on a module static) | **not reported** | **not reported** (threads pinned to 2 CPUs, 2 s; `data_races` +1, unrelated) |

**What the 125–127 "kernqueue" reports actually are.** For every report,
`scripts/kcsan_attribute.py` takes the first frame of each racing access,
i.e. the function that made it. All of them are in core-kernel code that
kernqueue calls or that interrupted it:

* `osq_lock`/`osq_unlock`: the mutex's optimistic-spinning queue, under
  contention on `kq.lock`
* `pollwake`/`poll_schedule_timeout` in `fs/select.c`
* the scheduler's `_find_next_and_bit`, hrtimer, tick and cgroup accounting

The first log-scan rule failed the run if a `[kernqueue]` frame appeared
anywhere in a report, which blamed kernqueue for races inside `osq_lock()`.
It now fails only when kernqueue code performs one of the racing accesses. The
change, and its replay on run 1's log (183 reports, 0 attributed), is
documented in testing.md.

**The `data_race` self-test does not trigger on this setup.** Its loop is
instrumented (`__tsan_read4`/`__tsan_write4` on every iteration; checked by
disassembly). During the 2 s window KCSAN set up 44,230 watchpoints, yet
`data_races` and `races_unknown_origin` didn't move. When a watchpoint
expires, KCSAN re-reads the watched value, and that value never changed. So
the two racers never wrote while the other was stalled on a watchpoint, even
though both 2 s loops overlapped in wall-clock time (total 2.08 s). The cause
is not determined. One hypothesis: how Hyper-V schedules vCPUs that spin in
`udelay()`. This self-test therefore cannot show that KCSAN sees module code,
so a mutation check on kernqueue itself was run instead (below).

### KCSAN mutation check (logs: [evidence/kcsan-mutant](evidence/kcsan-mutant/))

`scripts/kcsan_mutant_check.sh` builds a copy of the driver with the
markings removed from every lockless `ring.count` access: `READ_ONCE` →
plain read in `kq_has_data`, `kq_has_space` and `kq_poll`; `WRITE_ONCE` →
plain store in `kq_ring.h` (8 changed lines, `mutation.diff`). It then runs
three stress configurations on it under the KCSAN kernel.

| Result | |
|---|---|
| stress runs on the mutant | **3/3 PASS**: the bug is invisible to integrity testing (plain aligned 8-byte accesses happen not to tear on x86) |
| KCSAN reports | 22, of which **15 have a racing access inside kernqueue** |
| race pairs | `kq_write`/`kq_read` ×8, `kq_read`/`kq_read` ×4, `kq_poll`/`kq_write`, `kq_poll`/`kq_read`, `kq_write`/`kq_write` |
| example | `read to 0xffffffffc059b6d0 of 8 bytes ... kq_poll` vs `write ... kq_write`: `kq.ring.count` |
| verdict | **KCSAN MUTANT CHECK: PASS** |

So KCSAN does see kernqueue's accesses to its static state in the real
syscall paths, and the real driver's **0 attributed races** is a meaningful
result. It also shows why the markings matter even though the tests pass
without them.

After the mutation check, WSL stopped responding on the KCSAN kernel
(`HCS_E_CONNECTION_TIMEOUT`). Its logs were already on the Windows
filesystem. WSL was shut down, `.wslconfig` removed, the stock kernel booted,
and `build/kernqueue.ko` rebuilt for it (vermagic and CRCs re-verified).

### Final audit device run (logs: [evidence/audit-device-cycle](evidence/audit-device-cycle/), [evidence/audit-wake-mutant](evidence/audit-wake-mutant/), [evidence/audit-ab-min-capacity](evidence/audit-ab-min-capacity/))

Run on 2026-10-26 on the stock kernel. The driver source is unchanged since
run 2. The functional suite now has 33 tests: 6 were added by the audit (see
[FINAL_AUDIT.md](FINAL_AUDIT.md) §3).

| Step | Result |
|---|---|
| functional, 4 configurations (default, wake-all, 4 KiB, 16 MiB) | **4 × 33/33 PASS** |
| `scripts/test_kqctl.sh`; `test_compat32` | **13/13**; **PASS** |
| stress: both wake modes (7 + 7) and minimum capacity (2) | **16/16 PASS** |
| invalid `capacity` values rejected | **3/3** |
| benchmark: both modes + FIFO, 9 configurations × 3 reps | completed: `bench/results/20261026-200144/` |
| kernel log scan (device-cycle phase) | **clean** |
| `scripts/wake_mutant_check.sh` (new) | **PASS**: control passes; mutants M1 (need filter), M2 (reader hand-off) and M3 (writer hand-off) each make their targeted test fail **3/3** |
| `scripts/ab_wake.sh 4096 4 4 4096 3` | **6/6 stress PASS**. Exclusive mode was again slower: 85.5 vs 441.8 MB/s, 6.28–8.27 s vs 3.28–3.42 s |

The 16×1 herd fix replicated in the stress suite: 5.05 s vs 21.79 s, with
writer sleeps 168,235 vs 901,511. The benchmark's 16×1 and 1×16
*throughput* gains **did not replicate** (1.05× and 0.86×; see
benchmark_results.md). The kernel log during the mutant and A/B phases was
not captured.

### Totals

Across stock runs 1–3, the two KASAN debug runs and the two KCSAN runs
(historical), plus the audit run:
- **852 functional test executions** (720 historical, 132 in the audit)
- **121 stress runs**, all passing, plus **12 A/B stress runs** (6 historical,
  6 audit)

**54.10 million records** were integrity-checked: 46.06M historical and
8.04M in the audit. Of these, 13.68M ran under lockdep + KASAN and 13.68M
under strict KCSAN (both historical). No record
was lost, duplicated, reordered, corrupted or split. Runs on deliberately
broken drivers (the KCSAN mutant's 3 stress runs and the wake mutants) are
not counted.

### Problems found and fixed

| Problem | Cause | Fix |
|---|---|---|
| `prepare_wsl_kdir.sh` stopped on config differences | Its safety check flagged `GCC_PLUGINS`, `GCC_ASM_GOTO_OUTPUT_WORKAROUND` and toolchain capability probes | Reviewed each (all ABI-neutral; no plugin enabled, `RANDSTRUCT_NONE`) and encoded the review in the script, keeping the check strict for everything else |
| Kernel build failed: `'cpio' could not be found` | `CONFIG_IKHEADERS` packs headers with cpio; missing from the dependency list | Added cpio to the prerequisites; the rerun resumes instead of rebuilding |
| Tree reported `6.6.87.1-microsoft-standard-WSL2+` | `setlocalversion` appends `+` when the checkout is not at an annotated tag; vermagic would mismatch | Build with `LOCALVERSION=` (set, empty); the script verifies the release string |
| Every load logged `BPF: Invalid name_offset` / `Kernel module BTF mismatch detected` (run 1) | Module BTF is split BTF on top of vmlinux BTF; Kbuild used the self-built vmlinux, whose BTF (pahole 1.25) differs from the running kernel's (pahole 1.21). The module works; BPF tools could not read its types | `build_module.sh` regenerates the module's BTF against `/sys/kernel/btf/vmlinux`. The log scan also missed it (it only matched WARNING/BUG/…), so it now also flags `error`, `invalid` and `mismatch`. Fix verification: run 2 |
| Thundering herd (run 1, table above) | Non-exclusive waits | v2 exclusive, need-aware wake-ups (design.md §4); v1 behaviour kept as `exclusive_wake=0` for comparison |
| Debug run 1: `lock_order` self-test not reported | Test ordering: lockdep turns itself off after its first report, and the preceding `atomic_sleep` self-test also triggers one ("Invalid wait context") | Run `lock_order` first, and assert `debug_locks=1` just before it. Verified in debug run 2 |
| During debug run 2's wait, my revert (`wsl --shutdown`) killed a run that had just started | I batched "stop the tab" with the revert; the stop was refused because the user had typed in the tab, but the revert still ran | Restored the debug kernel and reran; reverted only after the run had exited. Process note: never batch a destructive step with the check that guards it |
| First KCSAN kernel never booted (`HCS_E_CONNECTION_TIMEOUT`, twice) | Lockdep + strict KCSAN + boot-time selftest + KCSAN enabled from early boot was too slow for WSL's VM start timeout | KCSAN variant without lockdep and the boot selftest, booting with KCSAN off; enabled through debugfs before the tests (booted in 8 s) |
| KCSAN run 1 "failed" with 125 reports involving kernqueue | My first attribution rule counted any `[kernqueue]` frame anywhere in a report; all 125 racing accesses were in core code (`osq_lock`, `pollwake`, ...) | `kcsan_attribute.py` attributes by the functions that performed the racing accesses; replayed on run 1 (0 attributed) and used in run 2 |
| Scan could pass by scanning nothing | If the kernel log ring buffer wrapped past the start marker, `sed` returned nothing | The scan fails when the marker is missing; KCSAN boots get `log_buf_len=16M` |
| `data_race` self-test not reported | Undetermined (see the KCSAN section) | Replaced as the KCSAN liveness proof by the mutation check, which fired; `data_race` kept as informational |

## Not done / environment-dependent

* Why the synthetic `data_race` self-test doesn't trigger KCSAN under WSL.
  The mutation check shows KCSAN works on kernqueue itself.
* The KCSAN kernel has no lockdep (it couldn't boot with both under WSL), so
  lockdep coverage comes from the KASAN kernel only.
* The "woken, then interrupted by a signal" hand-off path in v2 is reasoned
  about (design.md) but has no deterministic test.
* The 6 tests added by the final audit have run only on the stock kernel,
  not under the lockdep/KASAN or KCSAN kernels.
* Other kernel versions, architectures and native (non-VM) hardware.

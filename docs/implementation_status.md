# Implementation status

The truthful record of what exists, what was run, and what passed. "Verified"
means a test or tool was actually run on the environment below and passed.
Last updated: 2026-10-25.

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

### Totals

Across stock runs 1–3 and debug runs 1–2: **504 functional test executions**
and **73 stress runs**, all passing, plus the 6 A/B stress runs. **32.38
million records** were integrity-checked (13.68M of them under lockdep +
KASAN), with no lost, duplicated, reordered, corrupted or split record.

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

## Not done / environment-dependent

* KCSAN (data-race detector): not run. It's mutually exclusive with KASAN,
  so it needs a second debug kernel (`-e KCSAN` instead of KASAN in
  `prepare_debug_kdir.sh`).
* The "woken, then interrupted by a signal" hand-off path in v2 is reasoned
  about (design.md) but has no deterministic test.
* Other kernel versions, architectures and native (non-VM) hardware.

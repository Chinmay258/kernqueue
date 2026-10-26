# Benchmark results

Every number here was measured on the machine below. Nothing is estimated.
The matrix was run in two sessions with the same driver source, script and
parameters:

| Session | Date | Raw data |
|---|---|---|
| 1 (historical) | 2026-10-25 | [`bench/results/20261025-010601/`](../bench/results/20261025-010601/) |
| 2 (final audit rerun) | 2026-10-26 | [`bench/results/20261026-200144/`](../bench/results/20261026-200144/) |

Each directory holds `results.csv`, `environment.txt` and `summary.md`.

> **Correction (final audit).** Session 1 measured exclusive wake-ups 2.7×
> faster at 16×1 and 1×16. **Session 2 did not reproduce that:** 1.05× (ranges
> overlap) and 0.86×. The 2.7× figure is a single-session result and is no
> longer claimed. What reproduced is listed under
> [What holds across sessions](#what-holds-across-sessions). The audit's
> validity review is in [FINAL_AUDIT.md](FINAL_AUDIT.md) §6.

## Setup

| Item | Value |
|---|---|
| Machine | AMD Ryzen 7 5800H (8 cores / 16 threads), Windows 11 host |
| Linux | WSL2 (Hyper-V VM), Ubuntu 24.04.4, kernel `6.6.87.1-microsoft-standard-WSL2`, 16 vCPUs, 6.7 GiB |
| Driver | v2, capacity 65536 B, loaded with `exclusive_wake=1` (**kq-excl**) and `exclusive_wake=0` (**kq-all**, identical to v1) |
| Baseline | a named pipe (FIFO), default 64 KiB capacity (**fifo**) |
| Binary | `build/kq_bench`, gcc 13.3 `-O2` |

## Methodology

* **Workload.** W writer threads and R reader threads, each with its own fd.
  Every `write()`/`read()` moves one fixed-size record (64, 512 or 4096
  bytes). Writers are unthrottled, so the queue runs near full.
* **Phases.** 1 s warm-up (not measured), then 5 s measured. Each
  configuration was run 3 times; tables show the **median**, with the
  min–max range where it matters.
* **Order.** `run_device_tests.sh --bench` runs every kq-excl configuration
  and the FIFO first, then reloads the module and runs every kq-all
  configuration. kq-all vs kq-excl therefore compares two blocks a few minutes
  apart, not interleaved runs. Only `scripts/ab_wake.sh` (minimum-capacity
  section) interleaves the modes.
* **Throughput** is bytes (and records) completed by readers during the
  measured window, divided by its length.
* **Syscall latency** is `CLOCK_MONOTONIC` around each `write()`/`read()`,
  including time blocked in the driver.
* **End-to-end latency** is the reader's receive time minus the timestamp the
  writer stored in the record. In these unthrottled runs it is dominated by
  queueing delay: 1×1 at 64 B shows ~0.7 ms p50, which matches 64 KiB ÷
  ~81 MB/s ≈ 0.8 ms. So it says little about the driver itself.
* **Percentiles** come from per-thread reservoir samples (200,000 per thread
  per metric), merged across threads. Max is exact.
* **Driver sleep counts** (`write_waits`/`read_waits`) are the deltas over the
  measured window.

## Results (read MB/s, medians of 3 runs)

| W×R | size | S1 fifo | S1 kq-all | S1 kq-excl | S1 excl/all | S2 fifo | S2 kq-all | S2 kq-excl | S2 excl/all |
|---|---|---|---|---|---|---|---|---|---|
| 1×1 | 64 | 82.3 | 80.8 [79.6–82.9] | 73.5 [73.0–74.1] | 0.91 | 74.9 | 69.6 [69.2–69.6] | 68.9 [68.6–69.1] | 0.99 |
| 1×1 (non-blocking) | 64 | 77.4 | 81.1 | 74.0 | 0.91 | 64.3 | 73.1 | 67.7 | 0.93 |
| 1×1 | 512 | 677.9 | 636.6 | 583.4 | 0.92 | 607.7 | 557.3 | 524.1 | 0.94 |
| 1×1 | 4096 | 4432.0 | 4720.3 | 4220.3 | 0.89 | 3743.8 | 4114.1 | 3876.2 | 0.94 |
| 4×4 | 64 | 134.7 | 99.5 [98.8–100.8] | 96.1 [95.4–96.3] | 0.97 | 110.0 | 89.9 | 89.4 | 0.99 |
| 4×4 | 4096 | 2402.8 | 4705.5 [4530–4786] | 4571.5 [4501–4637] | ≈ (overlap) | 1863.1 | 3544.3 [3411–3780] | 3764.2 [3686–3806] | ≈ (overlap) |
| 8×8 | 512 | 642.8 | 541.1 [495–556] | 551.8 [542–552] | ≈ (overlap) | 357.5 | 396.1 [373–415] | 469.7 [389–474] | ≈ (overlap) |
| **16×1** | 4096 | 1100.3 | **150.8** [136–185] | **401.1** [400–405] | **2.66** | 901.3 | **312.4** [295–319] | **327.4** [288–370] | **1.05 (overlap)** |
| **1×16** | 64 | 19.1 | **2.2** [2.1–2.4] | **5.9** [5.8–5.9] | **2.68** | 19.6 | **5.7** [5.0–5.7] | **4.9** [4.9–5.2] | **0.86** |

Latency and sleeps for the contended cases (S1 / S2):

| W×R | size | variant | write() p99 µs | read() p99 µs | driver sleeps (writer / reader) |
|---|---|---|---|---|---|
| 16×1 | 4096 | kq-all | 2975 / 2267 | 139 / 149 | 587,600 / 21; 535,960 / 93 |
| 16×1 | 4096 | kq-excl | **1133 / 1260** | **23 / 36** | 333,951 / 0; 290,550 / 0 |
| 16×1 | 4096 | fifo | 771 / 879 | 19 / 23 | n/a |
| 1×16 | 64 | kq-all | 141 / 147 | 3210 / 2194 | 0 / 478,325; 0 / 464,429 |
| 1×16 | 64 | kq-excl | **26 / 35** | **1155 / 1377** | 0 / 386,676; 0 / 326,121 |
| 1×16 | 64 | fifo | 15 / 21 | 1019 / 1006 | n/a |

The 16×1 writer p99 ranges don't overlap in either session: S1 kq-all
2665–2978 vs kq-excl 1102–1172 µs; S2 2232–2273 vs 1161–1463 µs.

Integrity-checked stress runs (16×1 × 20,000 × 4 KiB records, one run per
mode per session) favour v2 in every session: **4.84 / 4.91 / 5.05 s** with
v2 against **29.04 / 26.71 / 21.79 s** with `exclusive_wake=0` (device runs 2,
3 and the audit). See [implementation_status.md](implementation_status.md).

## What holds across sessions

Reproduced in every session that measured it:
1. **16×1, 4 KiB: fewer writer sleeps and a lower writer tail.** Writer sleeps
   fell 1.76× and 1.84×; writer `write()` p99 fell 2.6× and 1.8×.
2. **16×1 integrity stress: 4.3–6.0× faster** with v2 on the stock kernel
   (3 sessions), and 3.6–4.7× on the debug kernels (4 sessions).
3. **Minimum capacity: v2 ≈5× slower** (interleaved A/B, 2 sessions; below).
4. **One producer, one consumer: v2 slower in every row, by 1–11%.** The
   block ordering above can't be excluded as a contributor.

Not reproduced, so not claimed:
- the 16×1 and 1×16 throughput gains
- any throughput difference at 4×4 and 8×8

Between sessions the same configuration moved by up to ~2×: kq-all 16×1 went
150.8 → 312.4 MB/s, and FIFO 8×8 512 B went 642.8 → 357.5 MB/s. The cause
wasn't isolated.

## Analysis

1. **The thundering herd is real; its throughput cost depends on the
   session.** With wake-all, one freed record wakes all 16 writers. Fifteen
   of them take the mutex, find no room and sleep again. Need-aware
   exclusive wake-ups remove most of those sleeps, and in both sessions that
   cut writer p99 latency (2.6×, 1.8×). In session 1 it also raised 16×1
   throughput 2.7×. In session 2 the wake-all case was itself twice as fast,
   and the throughput gain disappeared. The integrity-stress runs, which
   transfer a fixed 1.25 GiB, were 4.3–6.0× faster in all three stock-kernel
   sessions. Why the stress runs and the 5 s benchmark windows disagree in
   session 2 wasn't determined.
2. **v2 costs 1–11% on the uncontended single-producer path.** In session 1
   the ranges don't overlap. Every read and write now makes an extra hand-off
   wake call: a spinlock round trip on the wait queue, even when nobody is
   waiting. That's the likely cause, but it hasn't been profiled.
3. **Pipes are 2.7–3.4× faster in the 16×1 and 1×16 cases, and 22–35% faster
   at 4×4 with 64 B records** (both sessions). Reading `fs/pipe.c` in the same
   6.6 tree shows the pipe already uses kernqueue v2's core idea:
   `wait_event_interruptible_exclusive` plus a `wake_next_reader`/
   `wake_next_writer` hand-off. That's independent confirmation that v2's
   design is the standard one. Pipes add two things kernqueue doesn't do:
   - **Transition-only wake-ups.** Writers are woken only `if (was_full)` and
     readers only `if (was_empty)`, so most operations make no wake call at
     all. That's also exactly what would remove v2's single-producer cost.
   - **`wake_up_interruptible_sync_poll`.** It hints that the waker is about
     to sleep, so the scheduler can avoid migrating the woken task to another
     CPU.

   Pipe writes can also fill a whole page buffer, while kernqueue copies into
   one byte ring under one mutex.
4. **Where kernqueue beats the pipe: 4×4 at 4096 B**, about 2× in both
   sessions (S1 4.6–4.7 vs 2.4 GB/s; S2 3.5–3.8 vs 1.9 GB/s). A plausible
   cause is that a 4 KiB pipe write takes a whole page-sized slot, so the
   16-slot pipe holds at most 16 records, while kernqueue's byte ring packs
   them densely. This is not verified with a profiler.

**Next steps the data supports (not implemented):**
- interleave the two wake modes in the matrix before quoting any throughput
  ratio
- transition-only wake-ups, or skipping wake calls when the queue has no
  sleepers (`wq_has_sleeper()`, which needs a memory barrier)
- the `_sync` wake variants
- profiling with `perf` (not available in WSL) to confirm where the
  remaining contended-case time goes

## Minimum capacity (4096 B): same-session A/B, where v2 loses

Command: `scripts/ab_wake.sh 4096 4 4 4096 3` (4 writers, 4 readers, 4 KiB
records, modes alternated within each repetition). Raw data:
[`docs/evidence/ab-min-capacity/`](evidence/ab-min-capacity/) (session 1) and
[`docs/evidence/audit-ab-min-capacity/`](evidence/audit-ab-min-capacity/)
(session 2).

| Session | Variant | stress run (200k records, integrity-checked), 3 reps | bench read MB/s (median of 3) | write() p50 / p99 | driver sleeps w/r (5 s) |
|---|---|---|---|---|---|
| 1 | kq-all (`exclusive_wake=0`) | 3.48 s, 3.46 s, 3.66 s | **482.1** | 15.9 / 229.7 µs | 520,486 / 519,979 |
| 1 | kq-excl (`exclusive_wake=1`) | 5.33 s, 5.48 s, 5.52 s | **97.3** | 166.4 / 276.5 µs | 117,415 / 117,407 |
| 2 | kq-all | 3.42 s, 3.28 s, 3.28 s | **441.8** | 13.6 / 261.1 µs | 433,614 / 417,341 |
| 2 | kq-excl | 6.28 s, 6.53 s, 8.27 s | **85.5** | 191.5 / 352.3 µs | 102,150 / 102,115 |

**Exclusive wake-ups are ~5× slower here (5.0× and 5.2×)** even though they
cause about 4× fewer sleeps. All twelve stress runs passed their integrity
checks.

**Interpretation (a hypothesis consistent with the data, not profiled; `perf`
is unavailable in WSL).** When the ring holds a single record there's no
pipelining. Every transfer must wait for exactly one sleeping task to run. In
exclusive mode that's one wake-up per hand-off, and the other vCPUs go idle
meanwhile, so each hand-off pays the VM's latency for waking an idle vCPU. The
p50 write latency of ~166–192 µs fits that. With wake-all, several tasks are
woken on each event and stay runnable, so a task is usually already running
when the next state change happens. At the default 64 KiB capacity the ring
holds 16 such records, wake-up latency overlaps with useful work, and the
herd's cost dominates instead.

**Conclusion.** Neither strategy dominates.
- **Exclusive wake-ups, at the default capacity under 16×1 contention:**
  remove most wasted writer sleeps, cut writer tail latency, and finish the
  integrity stress several times faster. Their benchmark throughput
  advantage, though, isn't reproducible.
- **Their cost:** 1–11% for a single producer, and a large loss (~5×) when
  the queue holds only one record.

The default stays `exclusive_wake=1` for the latency and herd behaviour at the
default capacity, and the parameter lets a deployment pick. Things not
implemented that could combine the strengths: transition-only wake-ups (as
pipes do), the `_sync` wake variants, or briefly spinning before sleeping when
the queue is nearly empty or full.

## Caveats

* WSL2 is a VM on a Windows host running other software. Threads are not
  pinned, and CPU frequency isn't controlled (not exposed in WSL).
  Within-session spreads were mostly a few percent, but across sessions the
  same configuration varied by up to ~2×. Compare only numbers from the same
  session, and prefer interleaved runs.
* Three repetitions per configuration; no statistical-significance testing.
* One machine, one kernel. These are measurements of this setup, not general
  claims about either design.

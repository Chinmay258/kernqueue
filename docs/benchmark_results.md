# Benchmark results

Every number here was measured on the machine below. Nothing is estimated.
Raw data: [`bench/results/20261025-010601/`](../bench/results/20261025-010601/)
(`results.csv`, `environment.txt`, `summary.md`).

## Setup

| Item | Value |
|---|---|
| Machine | AMD Ryzen 7 5800H (8 cores / 16 threads), Windows 11 host |
| Linux | WSL2 (Hyper-V VM), Ubuntu 24.04.4, kernel `6.6.87.1-microsoft-standard-WSL2`, 16 vCPUs, 6.7 GiB |
| Driver | v2, capacity 65536 B, loaded with `exclusive_wake=1` (**kq-excl**) and `exclusive_wake=0` (**kq-all**, identical to v1) |
| Baseline | a named pipe (FIFO), default 64 KiB capacity (**fifo**) |
| Binary | `build/kq_bench`, gcc 13.3 `-O2` |
| Date | 2026-10-25 |

## Methodology

* **Workload.** W writer threads and R reader threads, each with its own fd.
  Every `write()`/`read()` moves one fixed-size record (64, 512 or 4096
  bytes). Writers are unthrottled, so the queue runs near full.
* **Phases.** 1 s warm-up (not measured), then 5 s measured. Each
  configuration was run 3 times; tables show the **median**, with the
  min–max range where it matters.
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

## Results (medians of 3 runs)

| W×R | size | fifo MB/s | kq-all MB/s (v1) | kq-excl MB/s (v2) | v2 vs v1 |
|---|---|---|---|---|---|
| 1×1 | 64 | 82.3 | 80.8 [79.6–82.9] | 73.5 [73.0–74.1] | **−9%** |
| 1×1 (non-blocking) | 64 | 77.4 | 81.1 | 74.0 | −9% |
| 1×1 | 512 | 677.9 | 636.6 | 583.4 | −8% |
| 1×1 | 4096 | 4432.0 | 4720.3 | 4220.3 | −11% |
| 4×4 | 64 | 134.7 | 99.5 [98.8–100.8] | 96.1 [95.4–96.3] | −3% |
| 4×4 | 4096 | 2402.8 | 4705.5 [4530–4786] | 4571.5 [4501–4637] | ≈ (ranges overlap) |
| 8×8 | 512 | 642.8 | 541.1 [495–556] | 551.8 [542–552] | ≈ (ranges overlap) |
| **16×1** | 4096 | 1100.3 | **150.8** [136–185] | **401.1** [400–405] | **+166% (2.7×)** |
| **1×16** | 64 | 19.1 | **2.2** (33.7k rec/s) | **5.9** (91.6k rec/s) | **+170% (2.7×)** |

Latency and sleeps for the contended cases:

| W×R | size | variant | write() p99 | read() p99 | driver sleeps (writer / reader) |
|---|---|---|---|---|---|
| 16×1 | 4096 | kq-all | 2975 µs | 139 µs | 587,600 / 21 |
| 16×1 | 4096 | kq-excl | **1133 µs** | **23 µs** | 333,951 / 0 |
| 16×1 | 4096 | fifo | 771 µs | 19 µs | n/a |
| 1×16 | 64 | kq-all | 141 µs | 3210 µs | 0 / 478,325 |
| 1×16 | 64 | kq-excl | **26 µs** | **1155 µs** | 0 / 386,676 |
| 1×16 | 64 | fifo | 15 µs | 1019 µs | n/a |

Integrity-checked stress runs agree. In device run 2, 16×1 × 20,000 × 4 KiB
records took **4.84 s** with v2 and **29.04 s** with `exclusive_wake=0` in the
same session (writer sleeps 167,313 vs 1,271,020). See
[implementation_status.md](implementation_status.md).

## Analysis

1. **The thundering herd was the dominant cost under contention.** With
   wake-all, one freed record wakes all 16 writers. Fifteen of them take the
   mutex, find no room and sleep again, so every record costs about a dozen
   context switches. Need-aware exclusive wake-ups raised 16×1 throughput
   2.7× and cut writer p99 latency 2.6×. 1×16 improves by the same factor on
   the reader side.
2. **v2 costs about 8–11% on the uncontended single-producer path.** The
   ranges don't overlap, so this is real. Every read and write now makes an
   extra hand-off wake call: a spinlock round trip on the wait queue, even
   when nobody is waiting. The 4×4 and 8×8 cases are within noise.
3. **Pipes are still 2.7–3.2× faster in the 16×1 and 1×16 cases, and 35%
   faster at 4×4 with 64 B records.** Reading `fs/pipe.c` in the same 6.6 tree
   shows the pipe already uses kernqueue v2's core idea:
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
4. **Where kernqueue beats the pipe: 4×4 at 4096 B** (4.6–4.7 GB/s vs
   2.4 GB/s). A plausible cause is that a 4 KiB pipe write takes a whole
   page-sized slot, so the 16-slot pipe holds at most 16 records, while
   kernqueue's byte ring packs them densely. This is not verified with a
   profiler.

**Next steps the data supports (not implemented):** transition-only wake-ups,
or skipping wake calls when the queue has no sleepers (`wq_has_sleeper()`,
which needs a memory barrier); the `_sync` wake variants; and profiling with
`perf` (not available in WSL) to confirm where the remaining contended-case
time goes.

## Minimum capacity (4096 B): same-session A/B, where v2 loses

Command: `scripts/ab_wake.sh 4096 4 4 4096 3` (4 writers, 4 readers, 4 KiB
records, modes alternated within each repetition). Raw data:
[`docs/evidence/ab-min-capacity/`](evidence/ab-min-capacity/).

| Variant | stress run (200k records, integrity-checked), 3 reps | bench read MB/s (median of 3) | write() p50 / p99 | driver sleeps w/r (5 s) |
|---|---|---|---|---|
| kq-all (`exclusive_wake=0`) | 3.48 s, 3.46 s, 3.66 s | **482.1** | 15.9 / 229.7 µs | 520,486 / 519,979 |
| kq-excl (`exclusive_wake=1`) | 5.33 s, 5.48 s, 5.52 s | **97.3** | 166.4 / 276.5 µs | 117,415 / 117,407 |

**Exclusive wake-ups are ~5× slower here** even though they cause about 4×
fewer sleeps. All six stress runs passed their integrity checks.

**Interpretation (a hypothesis consistent with the data, not profiled; `perf`
is unavailable in WSL).** When the ring holds a single record there's no
pipelining. Every transfer must wait for exactly one sleeping task to run. In
exclusive mode that's one wake-up per hand-off, and the other vCPUs go idle
meanwhile, so each hand-off pays the VM's latency for waking an idle vCPU. The
p50 write latency of ~166 µs fits that. With wake-all, several tasks are woken
on each event and stay runnable, so a task is usually already running when
the next state change happens. At the default 64 KiB capacity the ring holds
16 such records, wake-up latency overlaps with useful work, and the herd's
cost dominates instead.

**Conclusion.** Neither strategy dominates. Exclusive wake-ups win under heavy
contention with some queue depth (2.7× at 16×1), cost ~10% for a single
producer, and lose badly when the queue holds only one record. The default
stays `exclusive_wake=1` because it wins at the default capacity, and the
parameter lets a deployment pick. Things not implemented that could combine
the strengths: transition-only wake-ups (as pipes do), the `_sync` wake
variants, or briefly spinning before sleeping when the queue is nearly
empty/full.

## Caveats

* WSL2 is a VM on a Windows host running other software. Threads are not
  pinned, and CPU frequency isn't controlled (not exposed in WSL).
  Within-session spreads were mostly a few percent, but across sessions the
  same configuration varied by up to ~2×. Compare only numbers from the same
  session.
* One machine, one kernel. These are measurements of this setup, not general
  claims about either design.

# Interview preparation

Questions an interviewer is likely to ask about **this** implementation, with
the points a strong answer covers. Practise answering out loud before reading
the notes. Each answer is backed by a specific line of code or a test.

## Driver structure and lifecycle

1. **Walk me through what happens from `insmod` to the first `read()`.**
   Parameter validation, mutex and wait-queue init, `kvzalloc`, dynamic major,
   `class_create`, `device_create` (which creates the node via devtmpfs), and
   `cdev_add` *last*. Then `open` goes through `chrdev_open` → `fops_get` (module
   ref) → `kq_open` (`private_data`, `stream_open`), and `read` goes through
   `vfs_read` → `kq_read`.
2. **Why is `cdev_add` the last step of init?** It makes the device live.
   Everything it depends on must exist first, and no later step can fail and
   force a teardown under live users.
3. **How do you guarantee the buffer isn't freed while someone is blocked in
   `read()`?** `fops.owner = THIS_MODULE` → each open file pins the module →
   `rmmod` gets `EBUSY`. When `kq_exit` runs, no file is open, so nothing can be
   inside the driver.
4. **What is the difference between `struct file`, `struct inode` and a file
   descriptor? When does `release` run?** `release` runs once per open file
   description, on the last close. The `open_modes` test checks this with
   `dup`.
5. **Why `alloc_chrdev_region` instead of a fixed major? Why not
   `misc_register`?** Dynamic avoids collisions. Misc would be simpler; cdev
   shows the mechanism.

## Concurrency

6. **Why a mutex, not a spinlock?** `copy_to_user`/`copy_from_user` can fault
   and sleep, and sleeping with a spinlock held is illegal. There is no IRQ
   context use, so the mutex costs nothing.
7. **Show me how a blocked reader avoids a lost wake-up.** It drops the mutex,
   then `wait_event_interruptible` enqueues and sets the task state *before*
   testing the condition. The writer updates `count` under the lock, then calls
   `wake_up` (which takes the wait-queue lock). Explain both orderings.
8. **Why re-check the condition after re-acquiring the mutex?** Multiple
   waiters wake, another reader may consume the data, and wake-ups can be
   spurious.
9. **`count` is read without the lock: isn't that a data race?** It is
   intentionally lockless and marked with `READ_ONCE`/`WRITE_ONCE`. It is used
   only as a wake-up hint and poll snapshot; decisions are re-made under the
   lock. KCSAN would accept it.
10. **What is the thundering-herd problem here, and how did you fix it?** In
    v1 every event woke every sleeper. The stress test measured 1.27M writer
    sleeps for 320K records with 16 writers and 1 reader (27.8 s). v2 uses
    exclusive waiters, a need-aware writer wake function (returning 0 makes
    `__wake_up_common` skip the entry without using the wake-one budget), and
    hand-off when data or space is left over or a woken task bails out. The
    same run went to 4.8 s and 167K sleeps. The old behaviour stays available
    as `exclusive_wake=0` for same-session A/B runs. Be ready to explain why
    plain wake-one would starve a small writer behind a big one (the
    `wake_skips_unfit_writer` test), and where exclusive wake-ups do *not*
    help. With the ring holding one record, they were ~5× slower in a
    same-session A/B (97 vs 482 MB/s). Likely cause: no pipelining, so each
    hand-off pays an idle-vCPU wake-up. Wake-all keeps more tasks runnable.
    That's a hypothesis, not profiled.
10b. **How did you make sure your before/after numbers were trustworthy?**
    Kept the old behaviour behind a parameter so both variants run in the same
    session. Interleaved the variants in each repetition. Reported medians
    with min–max ranges. Called a difference real only when ranges didn't
    overlap. Noted that cross-session variance in the VM was up to ~2× for the
    same configuration.
11. **What happens if a user passes a `userfaultfd`-backed buffer to `read()`?**
    The copy blocks while holding the mutex and stalls every other client. The
    fix is a bounce buffer (copy outside the lock) at the cost of an extra copy.
    Pipes have the same behaviour.
12. **Could two readers ever get the same bytes? Could a reader see half of an
    atomic write?** No. Copy plus index update is one critical section, and
    writes publish `tail`/`count` only after the full copy succeeded.
13. **How would you make it faster under contention?** Separate reader and
    writer locks with an atomic `count` (one reader and one writer copy in
    parallel), a lock-free SPSC fast path, exclusive wake-ups, or batching.
    Each adds complexity; the benchmark shows where time goes.

## Blocking, signals and poll

14. **What does the driver return when a signal arrives during a blocking
    read, and what does the user see?** `-ERESTARTSYS`. The user sees a
    transparent restart with `SA_RESTART`, otherwise `EINTR`. Both cases are
    tested.
15. **How is `O_NONBLOCK` handled, and why check `f_flags` on every call?**
    `fcntl(F_SETFL)` can change it at any time (the `nonblock_toggle` test).
16. **Explain your `poll` implementation. Does it sleep?** No. `poll_wait`
    only registers on both queues; the VFS sleeps and re-invokes it after a
    wake-up.
17. **Why does `POLLOUT` require 4096 free bytes?** So a writable report
    guarantees an atomic write won't get `EAGAIN`. Otherwise a poll loop spins.
    It's the same contract pipes give for `PIPE_BUF`.

## Kernel/user boundary

18. **Why can't the driver just `memcpy` from the user pointer?** The pointer
    might be unmapped, paged out, or point into the kernel. `copy_from_user`
    checks `access_ok` and handles faults through exception tables.
19. **What does `copy_to_user` return, and how do you use it?** The number of
    bytes *not* copied. The driver consumes exactly `len - left` bytes, so a
    partially faulting read returns a short count and keeps the rest queued
    (`partial_read_fault` test with a guard page).
20. **What happens on a fault in the middle of an atomic write? Why not return
    a partial count?** `EFAULT`, nothing published. A torn record would break
    the atomicity contract that multi-writer users rely on.
21. **How did you prevent integer overflow and unbounded allocation?** Clamp
    `count` with `min()` before arithmetic. The only allocation is at load time
    from a range-checked parameter. Index advance can't overflow for
    `cap <= SIZE_MAX/2`.
22. **Why does `GET_STATS` copy to user space after dropping the mutex?** A
    faulting pointer can't stall other users, and the snapshot is already
    consistent.

## ioctl and ABI

23. **Why must unknown ioctls return `ENOTTY`?** Convention; `isatty`
    depends on it (tested).
24. **What is encoded in `_IOR(0xBA, 1, struct kq_stats)`, and what does that
    buy you?** Direction, size, type and number. A size mismatch becomes a
    different command, so old binaries get `ENOTTY`, not corruption.
25. **Why all-`__u64` fields? What is `compat_ptr_ioctl`?** No padding, so no
    kernel stack leak, and the same layout for 32 and 64 bit, so the compat
    handler only converts the pointer.

## Testing and debugging

26. **How do you test blocking behaviour without flaky sleeps?** Poll the
    driver's `read_waits`/`write_waits` counters until the thread is provably
    asleep in the driver, then act.
27. **How do you know your tests can fail?** Mutants of `kq_ring.h` were
    killed. `test_stress --inject dup|skip|corrupt|split` must be detected.
    Both were demonstrated.
28. **How does the stress test detect lost, duplicated, reordered or corrupted
    data?** Sequence numbers, checksums, regenerated payloads, per-reader
    monotonicity, an atomic seen-bitmap, a final sweep, and cross-checks
    against the driver's counters.
29. **What would you run next to increase confidence?** A lockdep +
    `DEBUG_ATOMIC_SLEEP` kernel, KASAN, KCSAN, longer soaks, and other kernel
    versions/architectures. sparse is already clean, and a planted `__user`
    misuse was caught.
29b. **How did you test the 32-bit compat path without a 32-bit libc?** With a
    freestanding `-m32 -nostdlib` program that makes raw `int $0x80` syscalls,
    so its `ioctl()` goes through `compat_sys_ioctl()` → `compat_ptr_ioctl()`.
    It works because `struct kq_stats` is all `__u64` (96 bytes in both ABIs),
    so the command number is the same and only the pointer needs converting.
30. **Why did you build a whole kernel just to compile one module?** No WSL
    headers exist, and modversions needs `Module.symvers` CRCs. The config must
    match exactly (e.g. BTF changes `struct module`). The script refuses to
    continue on unexplained diffs.

## Connecting to GPU / Tegra system software

31. **How does this relate to a GPU driver?** Same skeleton: char device +
    ioctls as the user-mode driver's control path; command/completion rings
    (head/tail/occupancy, wraparound); wait queues for fences and completion
    waits; `poll` for event fds; strict `copy_from_user` validation of
    user-supplied descriptors. What a real GPU driver adds: MMIO doorbells, IRQ
    handlers (which can't take mutexes, hence spinlocks), DMA mappings and
    pinned memory, and mmap of buffers instead of copies.
32. **If an interrupt handler produced data into this ring, what would change?**
    The producer side can't sleep or take a mutex. You'd use a spinlock with
    `spin_lock_irqsave` on both sides, move user copies outside the lock
    (bounce buffer or a per-reader copy after reserving space), and wake
    readers from the IRQ (`wake_up` is IRQ-safe).

## Résumé description

Every claim below is backed by a logged run (see implementation_status.md and
benchmark_results.md). If you change the code, re-run before reusing the
numbers.

**KernQueue: Linux character device driver and test framework** (C, Linux 6.6, kernel modules)

* Built a loadable Linux character driver exposing a bounded, blocking
  ring-buffer FIFO through `read`/`write`/`poll`/`ioctl`. It handles user
  copies safely (partial faults, `EFAULT`), `O_NONBLOCK`, signal-restartable
  waits, PIPE_BUF-style atomic writes and an ABI-stable statistics ioctl.
* Found a thundering-herd bottleneck through instrumented stress tests, and
  fixed it with exclusive, need-aware wake-ups (custom wait-queue wake
  function plus hand-off). That raised 16-writer/1-reader throughput 2.7×
  (151 → 401 MB/s) and cut writer p99 latency 2.6×. Benchmarked against Linux
  pipes and documented where the change is slower (~10% single-producer; ~5×
  when the ring holds a single record) via same-session A/B runs.
* Wrote a C test framework: 27 functional tests with fork-per-test timeouts
  and guard-page fault injection, multi-producer/multi-consumer integrity
  tests (11.9M records verified across 31 runs), sanitizer runs
  (ASan/UBSan/TSan) and fault-injection checks that prove the tests can fail.
* Reproduced a matching vendor kernel build tree for WSL2, with config-diff
  checks and symbol-CRC verification against shipped modules, to build and
  load the module safely.

Shorter one-liner: *Linux char driver (ring-buffer FIFO, blocking/poll/ioctl)
with a C stress/benchmark suite; fixed a thundering herd with need-aware
exclusive wake-ups (2.7× throughput under 16-way contention).*

Measured on a WSL2 VM (Ryzen 7 5800H). Say so if asked; don't present these
as bare-metal numbers.

# KernQueue design

KernQueue is a software-only Linux character device (`/dev/kernqueue`) that
exposes one bounded, blocking, thread-safe byte FIFO to user space. It has no
hardware, interrupts, DMA or MMIO. The interesting parts are the
kernel/user boundary, concurrency and blocking semantics.

Source: [`driver/kernqueue.c`](../driver/kernqueue.c),
[`driver/kq_ring.h`](../driver/kq_ring.h),
[`include/kernqueue.h`](../include/kernqueue.h).

## 1. User-visible semantics

Semantics are deliberately modelled on pipes, so they are familiar and every
choice has a precedent.

| Operation | Behaviour |
|---|---|
| `open` | Any access mode. All opens share one queue. The file is a stream (`stream_open`), so `lseek`/`pread`/`pwrite` fail with `ESPIPE`. |
| `read(n)`, n = 0 | Returns 0 immediately. Never blocks, never touches the buffer. |
| `read(n)`, queue empty | Blocks until data arrives. With `O_NONBLOCK`: `-1/EAGAIN`. |
| `read(n)`, data queued | Copies `min(n, used)` bytes and returns that count (a short read is normal). |
| `write(n)`, n = 0 | Returns 0 immediately. |
| `write(n)`, n ≤ 4096 (`KQ_ATOMIC_MAX`) | **Atomic.** Waits until `n` bytes are free, then queues all `n` contiguously in FIFO order. With `O_NONBLOCK` and fewer than `n` free bytes: `EAGAIN`, nothing queued. |
| `write(n)`, n > 4096 | **Not atomic.** Waits for at least one free byte, queues `min(n, free)` and returns that count. The caller loops. |
| bad user pointer | `EFAULT` if nothing was transferred; see section 5 for partial faults. |
| `poll` | `EPOLLIN` if ≥ 1 byte is queued (readable fds only). `EPOLLOUT` if ≥ 4096 bytes are free (writable fds only). |
| signal while blocked | `-ERESTARTSYS`: the syscall is restarted (`SA_RESTART`) or fails with `EINTR`. |
| `close` | Data stays queued for the next reader. |

**Why atomic writes up to 4096 bytes (like `PIPE_BUF`)?** With several writers,
records must not interleave. If every writer sends fixed-size records of at
most 4096 bytes, every record lands contiguously. Because all transfers are
whole records, `used` is always a multiple of the record size, so a reader
asking for one record always gets exactly one. The stress test depends on this.

**Why must capacity be at least 4096?** An atomic write waits until *all* of it
fits. If capacity were smaller than `KQ_ATOMIC_MAX`, a 4096-byte write could
never fit and would block forever. `KQ_MIN_CAPACITY == KQ_ATOMIC_MAX` rules
that out.

**Why does `EPOLLOUT` need 4096 free bytes, not 1?** Suppose `EPOLLOUT` meant
"at least one byte free". A writer sending 100-byte records with 50 bytes free
would see `poll` report "writable", get `EAGAIN` from `write`, call `poll`
again, get "writable" again, and spin at 100% CPU. Requiring 4096 free bytes
means `EPOLLOUT` guarantees that *any* write of up to 4096 bytes will not block.
That's the same promise pipes make. The trade-off: a writer of tiny records may
sleep in `poll` slightly longer than strictly necessary.

## 2. ioctl interface

| Command | Direction | Argument | Effect | Errors |
|---|---|---|---|---|
| `KQ_IOC_GET_STATS` | `_IOR(0xBA, 1, struct kq_stats)` | `struct kq_stats *` | Consistent snapshot of state and counters | `EFAULT` |
| `KQ_IOC_RESET` | `_IO(0xBA, 2)` | none | Discard queued bytes, wake blocked writers | `EBADF` if the fd is not writable |
| `KQ_IOC_CLEAR_STATS` | `_IO(0xBA, 3)` | none | Zero cumulative counters | `EBADF` if the fd is not writable |
| anything else | | | | `ENOTTY` |

* **`ENOTTY` for unknown commands** is the kernel convention. `isatty()` relies
  on it: it issues `TCGETS` and treats `ENOTTY` as "not a terminal". The tests
  check this directly.
* **The `_IOR`/`_IO` encodings put direction and size into the command number.**
  A caller compiled against a different `struct kq_stats` size sends a
  different number and gets `ENOTTY` instead of memory corruption.
* **`struct kq_stats` is all `__u64`.** It has no padding, which means no
  uninitialised kernel stack bytes can leak through `copy_to_user`. Its layout
  is also the same for 32-bit and 64-bit processes, which is why
  `compat_ptr_ioctl` is enough.
* **`RESET` and `CLEAR_STATS` need `FMODE_WRITE`.** A read-only fd shouldn't be
  able to destroy other users' data. Querying works on any fd.
* **The stats snapshot is copied to user space after the mutex is dropped.** A
  faulting user pointer then can't stall other users of the queue.

## 3. Ring buffer and invariants

```
 buf: [ . . . D D D D D . . . ]      cap = 11, head = 3, count = 5, tail = 8
              ^head     ^tail        bytes are queued at indices 3..7
 wrapped:
 buf: [ D D . . . . . . D D D ]      head = 8, count = 5, tail = 2
          ^tail         ^head        queued at 8,9,10,0,1
```

State: `head` (next byte to read), `tail` (next byte to write), `count`
(bytes queued), `cap`. Note that `<linux/circ_buf.h>` uses the opposite names:
its `head` is the producer index.

Invariants, which hold whenever the mutex is free:

* **I1** `cap > 0`
* **I2** `head < cap` and `tail < cap`
* **I3** `count <= cap`
* **I4** `tail == (head + count) mod cap`
* **I5** the queued bytes, oldest first, are at `head .. head+count-1` (mod `cap`)

**Empty vs full.** `head == tail` holds both when the queue is empty and when
it is full; `count` tells them apart. The alternatives are:

| Design | Pros | Cons |
|---|---|---|
| explicit `count` (chosen) | any capacity, uses every byte, trivially clear | one extra word to maintain |
| waste one slot | no count | loses a byte; "full" is `tail+1 == head` |
| free-running indices, power-of-2 size (kfifo) | no division, lock-free SPSC is possible | capacity must be 2^k; unsigned wrap tricks are subtle |

**Wraparound.** A transfer of `n` bytes from index `i` touches at most two
regions: `[i, min(cap, i+n))`, then `[0, rest)`. `kq_ring_split()` computes
them, and the driver loops over the two spans calling
`copy_to_user`/`copy_from_user`. Indices advance with a conditional subtraction
(`idx += n; if (idx >= cap) idx -= cap;`), valid because `n <= cap`. That avoids
a division and can't overflow because `cap <= 16 MiB`.

`kq_ring.h` is pure index arithmetic with no kernel dependencies, so the exact
same code is unit-tested in user space (`tests/test_ring.c`) against a
reference model. The model shares none of its arithmetic, and mutation testing
showed the test kills off-by-one mutants.

## 4. Locking and blocking

### Shared state and what protects it

| State | Protection |
|---|---|
| `ring.head`, `ring.tail`, buffer contents, `stats`, `open_files` | `dev->lock` (mutex) |
| `ring.count` | written under `dev->lock` with `WRITE_ONCE`; also read **without** the lock via `READ_ONCE` in wait conditions and `poll` |
| `ring.cap`, `buf` pointer | written once in `kq_init` before the device goes live; read-only afterwards |
| `readq`, `writeq` | wait queues have their own internal spinlock |

### Why a mutex and not a spinlock

The critical section copies to and from user memory. `copy_to_user` and
`copy_from_user` can page-fault, and handling a fault can sleep (swap-in,
page cache read). **Sleeping while holding a spinlock is forbidden.** It can
deadlock (another CPU spins waiting for a lock held by a task that is
sleeping), and with `CONFIG_DEBUG_ATOMIC_SLEEP` it produces "BUG: sleeping
function called from invalid context". A mutex lets its holder sleep. We never
touch the queue from interrupt context, so a mutex's inability to be taken in
IRQ context costs nothing.

The alternatives considered:

* **Spinlock + bounce buffer.** Copy from user into a kernel temporary first,
  then lock and `memcpy`. This works but costs an extra copy and an allocation
  per call (or a fixed per-call cap).
* **Lock-free SPSC ring (kfifo-style).** This is fast, but the device allows
  any number of readers and writers, so it would need locks on both sides
  anyway.
* **Two locks (reader/writer side).** These allow one reader and one writer to
  copy concurrently. The count update then needs atomics, and the reasoning
  gets much harder. Possible future work, motivated by the benchmark.

### The blocking pattern

```c
mutex_lock_interruptible(&lock);
while (count == 0) {                     // condition checked under the lock
	if (O_NONBLOCK) { unlock; return -EAGAIN; }
	unlock;                              // never sleep holding the mutex
	if (wait_event_interruptible(readq, READ_ONCE(count) > 0))
		return -ERESTARTSYS;             // signal
	mutex_lock_interruptible(&lock);     // re-take and RE-CHECK
}
copy out; update head/count; unlock;
wake_up_interruptible_poll(&writeq, EPOLLOUT);
```

* **Why a loop that re-checks?** Several readers can be woken by one write
  (`wake_up` wakes all non-exclusive waiters), and only some will find data.
  Wake-ups can also be spurious. The decision is always re-made under the lock.
* **Why there are no lost wake-ups.** `wait_event_interruptible` adds the task
  to the wait queue and sets `TASK_INTERRUPTIBLE` *before* evaluating the
  condition. A writer updates `count` and then calls `wake_up`, which takes the
  wait queue's spinlock. Either the reader is already queued, so it gets woken,
  or it queues afterwards and then sees the new `count`. Both sides go through
  that spinlock, so the ordering is guaranteed.
* **Why wake after unlocking?** A woken task's first action is
  `mutex_lock`. Waking while still holding the mutex would just make it sleep
  again on the mutex.
* **Why the `_poll` variant with a key?** The key lets `epoll` entries that
  only care about the other direction skip the wake-up. Plain
  `wait_event` sleepers ignore the key.
* **Why interruptible waits everywhere a user task can sleep?** So `Ctrl-C`
  and `kill` work. An uninterruptible sleep would leave an unkillable "D"
  state task if something went wrong. `mutex_lock_interruptible` is used for
  the same reason. Only `release()` uses plain `mutex_lock`, because `close`
  can't fail.

### Deadlock and race analysis

| Risk | Why it can't happen here |
|---|---|
| Sleeping on a wait queue with the mutex held | Every waiter unlocks before `wait_event_interruptible`. |
| Lock-order inversion | There is only one driver lock. The mutex is never taken with a wait-queue lock held (the wait-queue lock is internal to `wait_event`/`wake_up`). |
| Self-deadlock through a page fault while holding the mutex | A fault on the user buffer could only re-enter the driver if the buffer were backed by this device, and KernQueue has no `mmap`. |
| Lost wake-up | See above: queue-then-check in `wait_event`, unconditional `wake_up` after every state change, and `RESET` wakes writers too. |
| Reader and reader racing for the same bytes | The copy and the `head`/`count` update happen in one critical section. |
| Torn atomic write seen by a reader | Bytes are copied into the free region first. `tail`/`count` are published only after the whole copy succeeded, all under the mutex. |
| Unload while in use | `fops.owner = THIS_MODULE`: every open file holds a module reference, so `rmmod` fails with `EBUSY` until all fds are closed. With no open files, no task can be inside a file operation or asleep on our wait queues, so `kq_exit` can free the buffer. |
| Open racing with `init` failure | `cdev_add` is the **last** step of `kq_init`. Nothing after it can fail, so the device never has to be torn down under a live user. Opening the node before `cdev_add` just gets `ENXIO`. |
| Lockless reads of `count` | `READ_ONCE`/`WRITE_ONCE` make them proper marked accesses (no compiler tearing or caching, and no KCSAN data-race report). They are only hints. |

### Wake-up strategy: from wake-all (v1) to exclusive, need-aware wake-ups (v2)

**v1 (`exclusive_wake=0`).** Every state change calls `wake_up`, which wakes
*every* sleeper. Device run 1 measured the cost. With 16 writers and 1 reader
moving 4 KiB records, each read frees room for exactly one record but wakes all
16 writers. Fifteen of them take the mutex, find no room and sleep again. That
was 1,268,318 writer sleeps for 320,000 records, and 27.8 s for a run that
moves the same bytes as other configurations in well under a second. The
1-writer/16-reader case shows the mirror image on the reader side.

**v2 (`exclusive_wake=1`, the default).** Three rules:

1. **Exclusive waiters.** Blocked readers and writers queue with
   `WQ_FLAG_EXCLUSIVE`. `wake_up_*` wakes every non-exclusive entry (all `poll`
   waiters) and **one** exclusive entry.
2. **Need-aware writer wake function.** Writers don't all want the same thing:
   one may need 4096 free bytes, another 10. If the 4096-byte writer is first
   in the queue and only 100 bytes are freed, plain wake-one would wake it. It
   can't proceed and goes back to sleep, while the 10-byte writer, which could
   proceed, is never woken. So each writer's wait entry uses `kq_writer_wake()`,
   which returns 0 ("didn't take it") unless that writer's need now fits.
   `__wake_up_common()` only counts an exclusive entry against the wake-one
   budget when its wake function returns non-zero (verified in the 6.6 source),
   so the wake-up goes to the first writer that can use it. Readers all need
   just one byte, so they use the default wake function.
3. **Hand-off.** With wake-one, whoever leaves something behind must pass it
   on. A reader that leaves data in the queue wakes the next reader. A writer
   that leaves free space wakes the next fitting writer. A task that was woken
   but then bails out (signal, `mutex_lock_interruptible` failure, `EFAULT`)
   hands its wake-up on as well.

**Why v2 cannot leave a task asleep while it could make progress.** Every
change that creates data or space (write, read, reset) is followed by a wake
call made *after* the change, and that call evaluates waiters against a value
at least as new as the change. A waiter that queued itself after the change
sees the change in its own post-enqueue check, by the same queue-then-check
argument as before. A woken task that doesn't use the resource hands the
wake-up on. Livelock (two waiters waking each other forever) can't happen,
because a writer that can't proceed never wakes anyone, and the wake function
skips writers that don't fit.

**Regression tests** that hang (and so report `TIMEOUT`) if a rule is broken:
`wake_skips_unfit_writer` (rule 2), `wake_hands_on_to_next_reader` and
`wake_hands_on_to_next_writer` (rule 3). They pass trivially under v1.

**Measured trade-off.** v2 is not uniformly faster: +170% at 16×1, about −10%
for a single producer and consumer (the extra hand-off wake call), and about
5× slower when the ring holds only one record (4 KiB capacity, 4 KiB records).
Details and the likely cause are in benchmark_results.md. The default stays
exclusive because it wins at the default capacity.

**Not covered by a deterministic test:** "woken, then interrupted before taking
the mutex". It's handled by the hand-off on the bail-out path and reasoned
about above, but it's timing-dependent and hard to force from user space.

### Known weaknesses (accepted, documented)

* **User copies happen under the mutex.** A process can hand the driver a
  buffer backed by `userfaultfd` (or slow NFS-backed `mmap`) and hold the mutex
  for a long time, stalling every other user of the queue. Pipes have the same
  property. A bounce buffer would fix it.
* **No fairness guarantee.** A large atomic writer waiting for 4096 free bytes
  can be starved by small writers that keep taking the space as it frees up.
  The need-aware wake function makes this more likely, not less: it prefers
  writers that fit *now*.
* **The wake function walks the queue.** With many sleeping writers that don't
  fit, each wake call checks them all under the wait-queue spinlock. That's
  O(waiters) per event, but without context switches.

## 5. Kernel/user boundary

* **The driver never dereferences a `__user` pointer.** All access goes through
  `copy_to_user`/`copy_from_user`. These run `access_ok()` (which rejects
  kernel addresses) and handle faults through the exception table, returning
  the number of bytes *not* copied.
* **A read that faults part-way returns the bytes that arrived and consumes
  only those.** The rest stay queued. If even the first byte faults, the result
  is `EFAULT` and nothing is consumed.
* **An atomic write that faults part-way returns `EFAULT` and publishes
  nothing.** The partially copied bytes sit beyond `tail`, invisible to
  readers, and get overwritten later. A larger (non-atomic) write keeps the
  prefix that was copied and returns its length, like pipes.
* **`count` from user space is clamped before any arithmetic:**
  `min(count, used)` or `min(count, free)`. The VFS already limits a single
  read or write to `MAX_RW_COUNT`.
* **The only allocation is the ring itself**, made once at load time from a
  validated module parameter. Its range is
  `[KQ_MIN_CAPACITY, KQ_MAX_CAPACITY]` = `[4096, 16 MiB]`, and an out-of-range
  value makes `insmod` fail with `EINVAL`. `kvzalloc` falls back to `vmalloc`,
  so a 16 MiB ring doesn't need physically contiguous memory. That's fine
  because the buffer is only ever touched from process context.

## 6. Module lifecycle

`kq_init`:
1. validate the parameter
2. init the mutex and wait queues
3. `kvzalloc` the buffer
4. `alloc_chrdev_region` (dynamic major)
5. `class_create`
6. `device_create`, which creates `/dev/kernqueue` via devtmpfs/udev
7. `cdev_init` + `cdev_add`, the device goes live; this is last on purpose

Every failure unwinds the earlier steps in reverse through `goto` labels.

`kq_exit` reverses it: `cdev_del`, `device_destroy`, `class_destroy`,
`unregister_chrdev_region`, `kvfree`.

Alternatives: `misc_register()` would replace steps 4–7 with one call (dynamic
minor under major 10). The explicit `cdev` path was chosen to show the
underlying mechanism. `cdev_device_add()` combines the last two steps but needs
a caller-initialised `struct device`.

Kernel API notes (tested on 6.6 only):
* `class_create()` takes only a name since Linux 6.4. Older kernels need
  `class_create(THIS_MODULE, name)`.
* `stream_open()` exists since 5.3. With it, `.llseek` can be left `NULL`,
  because `vfs_llseek` returns `-ESPIPE` when `FMODE_LSEEK` is clear (verified
  in the 6.6 source).
* `wake_up_interruptible_poll`, `kvzalloc`, `compat_ptr_ioctl` (5.5+).

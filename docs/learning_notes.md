# KernQueue learning notes

These notes explain every concept the code actually uses, in the order you
meet them when reading `driver/kernqueue.c`. Each section ends with
comprehension exercises; answers are folded underneath. Try to answer before
opening them. If you can answer all of them without notes, you can defend the
project in an interview.

Read alongside: [design.md](design.md) (the "why" of each decision) and the
source.

---

## Milestone 1: environment and kernel build mechanics

### Kernel modules and the build tree

A loadable module is an ELF relocatable object (`.ko`) that the kernel links
into itself at `insmod` time. It runs in kernel mode with full privileges, so
a NULL dereference in a module is a kernel oops, not a segfault.

Modules must be compiled against **the exact configuration and headers of the
running kernel**:

* **`vermagic`** is a string embedded in every `.ko`: kernel release, SMP,
  preemption model and similar. The loader rejects a mismatch.
* **`CONFIG_MODVERSIONS`** (on in WSL) makes every exported symbol carry a CRC
  of its type signature. The CRCs come from `Module.symvers`, which is only
  produced by building the kernel. That's why `prepare_wsl_kdir.sh` does a full
  build rather than `make modules_prepare`.
* **Config options can change struct layouts.** Example from this project:
  `CONFIG_DEBUG_INFO_BTF_MODULES` adds `btf_data_size` and `btf_data` to
  `struct module` (see `include/linux/module.h`). A module built with that
  option off would lay out its `__this_module` differently from what the
  kernel expects, and the kernel would read garbage. The script diffs the
  generated `.config` against `/proc/config.gz` and refuses to continue on
  unexplained differences.
* **Kbuild** is the kernel's build system. An out-of-tree module is built with
  `make -C <kernel tree> M=<module dir> modules`. Kbuild reads `driver/Kbuild`
  (`obj-m := kernqueue.o`). `M=` cannot contain spaces, hence the staging step
  in `build_module.sh`.

### WSL2 specifics

WSL2 runs a real Linux kernel (Microsoft's fork) inside a Hyper-V VM. Modules
load into that kernel, and a crash takes down the WSL VM (all distros), not
Windows. Microsoft doesn't ship headers, so we rebuild the tree from the tag
matching `uname -r`.

#### Exercises

1. Why can't you just `apt install linux-headers-$(uname -r)` on WSL2?
2. What breaks if you build the module with `CONFIG_DEBUG_INFO_BTF_MODULES=n`
   but load it into a kernel that has it set to `y`?
3. Why does the module need `Module.symvers`, and why does
   `make modules_prepare` alone not give you a usable one?

<details><summary>Answers</summary>

1. Ubuntu publishes headers for Ubuntu kernels. WSL runs Microsoft's kernel
   build (`6.6.87.1-microsoft-standard-WSL2`), and no package matches that
   release.
2. `struct module` has two extra fields in the running kernel. Fields after
   them sit at different offsets in the module's `__this_module`, so the
   kernel misreads the module's metadata (init/exit pointers, etc.). That
   means a crash or undefined behaviour. With modversions the `module_layout`
   CRC check would normally catch it first.
3. With `CONFIG_MODVERSIONS=y`, every imported symbol's CRC is recorded in the
   module and checked at load time. The CRCs come from `Module.symvers`, which
   is generated when vmlinux (and modules) are built and modpost runs.
   `modules_prepare` builds only the host tools and generated headers, not
   vmlinux, so there are no CRCs.

</details>

---

## Milestone 2: character device registration and lifecycle

### Device numbers, cdev, class and device

* **`dev_t`** packs a *major* number (which driver) and a *minor* number
  (which instance). `alloc_chrdev_region(&devt, 0, 1, "kernqueue")` asks the
  kernel for a free major dynamically. It shows up in `/proc/devices`.
* **`struct cdev`** connects a `dev_t` to a `struct file_operations`.
  `cdev_add()` makes the device **live**: from that instant, `open()` on a
  node with that `dev_t` reaches our code. That's why it is the last step of
  `kq_init`.
* **`class_create` + `device_create`** create a sysfs entry
  (`/sys/class/kernqueue/kernqueue`) and emit a uevent. **devtmpfs** creates
  `/dev/kernqueue` (root:root 0600), and udev may then apply rules. Without
  these calls you would have to `mknod` the node by hand.
* **Error unwinding with `goto`.** Each step that succeeded is undone in
  reverse order if a later step fails. That's the standard kernel idiom: one
  exit path per acquired resource, no nested `if`s.

### `file_operations` and the three "file" objects

| Object | What it is | Lifetime |
|---|---|---|
| file descriptor (int) | per-process index into the fd table | `open` to `close` in that process |
| `struct file` ("open file description") | one per `open()`; holds `f_flags`, `f_mode`, `private_data` | until the **last** fd referring to it is closed (`dup`, `fork` share it) |
| `struct inode` | the device node itself; `i_cdev` points at our cdev | as long as the node exists |

`release()` runs once per `struct file`, when its last reference is dropped,
not on every `close()`. The `open_modes` test proves it: `dup()` doesn't call
`open`, and closing the dup doesn't call `release`.

`container_of(inode->i_cdev, struct kq_dev, cdev)` recovers our device struct
from the embedded `cdev`. We store it in `filp->private_data` so the other
callbacks don't need the inode.

### Unload safety: module reference counting

`.owner = THIS_MODULE` in `file_operations`: when `open()` goes through
`chrdev_open()`, the VFS calls `fops_get()`, which calls `try_module_get()`.
Every open file therefore holds a reference to our module, and
`rmmod kernqueue` fails with `EBUSY` while any fd is open. So in `kq_exit()`
no task can be inside our code or asleep on our wait queues, and freeing the
buffer is safe.

#### Exercises

1. Why must `cdev_add()` come after the buffer allocation and mutex
   initialisation?
2. A process opens `/dev/kernqueue`, forks, and both processes close their fd.
   How many times are `kq_open` and `kq_release` called?
3. What would happen if `.owner` were left `NULL` and you ran `rmmod` while
   a reader was blocked in `read()`?
4. Why does the device node exist before `cdev_add()` in our init order, and
   why is that harmless?

<details><summary>Answers</summary>

1. `cdev_add()` publishes the device. A concurrent `open()`/`read()` could
   otherwise run against an uninitialised mutex or a NULL buffer.
2. `kq_open` once, `kq_release` once. `fork` duplicates the fd but shares the
   same `struct file`; release runs when the last reference is closed.
3. The module could unload, freeing the code and the buffer the reader is
   sleeping on. When the reader woke, it would execute freed memory: a
   use-after-free and a kernel crash.
4. `device_create` (creating the node) comes before `cdev_add`. An `open()` in
   that window finds no cdev registered for the `dev_t` and fails with
   `ENXIO`. Nothing can reach half-initialised state, and no step after
   `cdev_add` can fail, so we never tear down a live device during init.

</details>

---

## Milestone 3: ring buffer and the kernel/user boundary

### Ring invariants

See [design.md §3](design.md#3-ring-buffer-and-invariants) for the five
invariants and the empty/full discussion. Things to be able to explain:

* why `count` is needed (`head == tail` is ambiguous)
* why at most two `memcpy`/`copy_*_user` calls are needed per transfer
* why `idx += n; if (idx >= cap) idx -= cap;` is correct for `n <= cap`
* why the ring logic is in a header with no kernel dependencies (so it can be
  unit-tested in user space against a model)

### `copy_to_user` / `copy_from_user`

* **A `__user` pointer is an address in the calling process's address space.**
  The kernel must never dereference it directly. It might be unmapped, point
  into kernel memory (an attack), or be paged out. The `__user` annotation
  lets the `sparse` checker (`make C=1`) flag direct dereferences.
* **`copy_to_user(to, from, n)` does two things.** First, `access_ok()`
  rejects ranges that aren't user addresses, so a kernel address like
  `0xffff888000000000` gets `EFAULT`; the `bad_user_pointers` test checks
  this. Second, it copies with fault handling: if a page fault can't be
  resolved, the exception table redirects to fix-up code instead of oopsing.
* **They return the number of bytes NOT copied.** `0` means success. This is
  what makes partial transfers precise:

  ```c
  left = copy_to_user(ubuf + done, buf + off, len);
  done += len - left;
  if (left) break;          // stop at the first fault
  kq_ring_consume(&ring, done);   // dequeue exactly what arrived
  ```
* **They may sleep** (to page memory in). That's the root reason the driver
  uses a mutex.

### Partial transfers and EFAULT policy

| Situation | Result |
|---|---|
| read faults after k > 0 bytes | returns k; k bytes consumed; rest stays queued |
| read faults on the first byte | `EFAULT`; nothing consumed |
| atomic write (≤ 4096) faults anywhere | `EFAULT`; **nothing published** (tail not advanced) |
| non-atomic write faults after k > 0 bytes | returns k; k bytes queued |

The tests build these cases with `mmap` + `mprotect(PROT_NONE)`: a buffer
whose last 100 bytes are valid, followed by an inaccessible guard page.

#### Exercises

1. `copy_to_user` returned 3 for a 10-byte copy. How many bytes reached user
   space?
2. Why does an atomic write that faults not need to "roll back" the bytes it
   already copied into the ring?
3. Which invariant would break if `kq_copy_in` called `kq_ring_produce(n)`
   instead of `kq_ring_produce(done)`?
4. Why is it safe for the read path to call `copy_to_user` twice for one
   `read()` (wrapped data) while holding the mutex?

<details><summary>Answers</summary>

1. 7 (it returns the number *not* copied).
2. They were written into the free region beyond `tail`. Readers only see
   `[head, head+count)`. Since `tail`/`count` were not advanced, those bytes
   are invisible and will be overwritten by the next write.
3. I5: `count` would claim `n` valid bytes, but only `done` of them are real
   data. Readers would get stale or zeroed bytes. That breaks the "queued bytes
   are exactly what was written" property, and the atomicity guarantee.
4. The mutex makes the whole read one critical section. No other reader can
   move `head` between the two spans, and no writer can overwrite the bytes
   being copied.

</details>

---

## Milestone 4: synchronisation, blocking, non-blocking and poll

### Execution context and the sleeping rule

Our file operations run in **process context**: on behalf of a user task,
which may sleep. Code in **atomic context** must not sleep: holding a
spinlock, in an interrupt handler, or with preemption disabled. Sleeping there
can deadlock the CPU or corrupt scheduler state. `copy_*_user`, `kmalloc
(GFP_KERNEL)` and `mutex_lock` may all sleep.

### Mutex vs spinlock

| | spinlock | mutex |
|---|---|---|
| waiting | busy-waits | sleeps |
| holder may sleep | **no** | yes |
| usable in IRQ context | yes (`spin_lock_irqsave`) | no |
| best for | very short sections, IRQ-shared data | sections that may sleep |

Our critical section calls `copy_*_user`, so it **must** be a mutex.
`mutex_lock_interruptible` returns non-zero if a signal arrives while waiting,
so a user can always `Ctrl-C` out. `lockdep_assert_held(&dev->lock)` in the
copy helpers documents, and with lockdep enabled checks, that the caller holds
the lock.

### Wait queues

`wait_event_interruptible(wq, cond)` is roughly:

```c
for (;;) {
	prepare_to_wait(&wq, &entry, TASK_INTERRUPTIBLE); // enqueue + set state
	if (cond) break;                                  // check AFTER enqueueing
	if (signal_pending(current)) { ret = -ERESTARTSYS; break; }
	schedule();                                       // actually sleep
}
finish_wait(&wq, &entry);
```

**Lost wake-up**: if you checked `cond` *before* enqueueing, a writer could
update the state and call `wake_up` in between. Your wake-up would be lost and
you'd sleep forever. Enqueue-then-check closes that window. Both sides go
through the wait queue's internal spinlock, so either the waker sees us on the
queue, or we see its update.

**The driver's loop**: check under the mutex → unlock → `wait_event` (lockless
`READ_ONCE` hint) → relock → **check again**. The re-check is necessary because
`wake_up` wakes *all* non-exclusive waiters and another reader may take the
data first.

**Thundering herd**: N sleeping readers, one 64-byte write → all N wake and
contend for the mutex, and N−1 go back to sleep. Exclusive waits avoid that.
It's not done here; see design.md "Known weaknesses".

### Signals: `-ERESTARTSYS`

A driver never returns `EINTR` itself. It returns `-ERESTARTSYS`, and on the
way back to user space the signal code decides:
* if the handler has `SA_RESTART`, the syscall is re-issued transparently. The
  `signal_restarts_read` test sees `read_waits` go from 1 to 2.
* otherwise the call fails with `EINTR` (`signal_interrupts_read`).

### Non-blocking I/O

`O_NONBLOCK` lives in `filp->f_flags` and can be changed by `fcntl(F_SETFL)` at
any time, so the driver tests it on every call, at the moment it would
otherwise sleep. It returns `-EAGAIN` (== `EWOULDBLOCK` on Linux).

### poll

`kq_poll` **never sleeps**:
1. `poll_wait(filp, &readq, wait)` and `poll_wait(filp, &writeq, wait)`
   *register* the caller's poll entry on our wait queues (no sleeping here).
2. It returns the current readiness mask.

The VFS (`do_poll`) sleeps if no fd is ready, and our `wake_up_*` calls wake
it. It then calls `kq_poll` again to get the new mask. Registering before
sampling is what prevents lost wake-ups, by the same argument as above.

`EPOLLOUT` requires `KQ_ATOMIC_MAX` free bytes. Reporting writable when a
write would still get `EAGAIN` makes a poll loop spin. Pipes promise the same
thing for `PIPE_BUF`.

### READ_ONCE / WRITE_ONCE

`count` is read without the lock (in the wait condition and in poll). A plain
C access there is a **data race**: the compiler may tear it, cache it in a
register across loop iterations, or re-read it. `READ_ONCE` forces exactly one
untorn load, and the writer pairs it with `WRITE_ONCE`. KCSAN treats marked
accesses as intentional. The value is only a *hint*; all decisions are
re-made under the mutex.

#### Exercises

1. Why can't `kq_read` simply call `wait_event_interruptible` while still
   holding `dev->lock`?
2. Two readers sleep on an empty queue; a writer queues 4 bytes. Walk through
   what happens with the driver's loop.
3. Why is the wake-up issued after `mutex_unlock` rather than before?
4. A user installs a `SIGUSR1` handler without `SA_RESTART`, and a thread
   blocked in `read()` receives it. What does `read()` return, and which line
   in the driver produced that?
5. Could `kq_poll` take `dev->lock`? What would you gain or lose?
6. Why is `stream_open()` called in `kq_open`?

<details><summary>Answers</summary>

1. Writers need the mutex to add data. A reader sleeping with it held would
   block every writer forever: a deadlock.
2. Both wake (non-exclusive). Each calls `mutex_lock_interruptible`. The first
   gets the lock, sees `count == 4`, copies `min(n, 4)` bytes and unlocks. The
   second gets the lock. If `count == 0` it goes round the loop and sleeps
   again; otherwise it takes what is left. No data is duplicated or lost.
3. A woken writer's first action is `mutex_lock`. If we still held the mutex
   it would immediately sleep again. Correctness doesn't depend on the order,
   only efficiency.
4. `-1` with `errno == EINTR`. The driver returned `-ERESTARTSYS` from
   `if (wait_event_interruptible(...)) return -ERESTARTSYS;`, and the kernel's
   signal delivery converted it to `EINTR` because the handler lacked
   `SA_RESTART`.
5. It could. A mutex may be taken in `->poll`, which runs in process context.
   The gain would be an exactly consistent snapshot. The loss: `poll` could
   sleep on a busy mutex, and it adds contention with the hot read/write
   paths. The snapshot is inherently stale the moment we return anyway, and
   correctness comes from registering on the wait queues first, so
   `READ_ONCE` is enough.
6. A FIFO has no file position. `stream_open` clears `FMODE_LSEEK`,
   `FMODE_PREAD` and `FMODE_PWRITE` (so `lseek`/`pread`/`pwrite` fail with
   `ESPIPE`) and sets `FMODE_STREAM`, so the VFS doesn't maintain or lock
   `f_pos` for us.

</details>

### Exclusive wake-ups and custom wake functions (v2)

A wait-queue entry has a `func` and `flags`. `__wake_up_common()` walks the
queue and calls each entry's `func`:

```c
ret = curr->func(curr, mode, wake_flags, key);
if (ret < 0) break;
if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive) break;
```

* Non-exclusive entries (`poll`, `wait_event_*`) are all woken.
* `wake_up*()` passes `nr_exclusive = 1`, so after the first exclusive entry
  that *accepts* the wake-up (returns non-zero), the walk stops.
* An entry whose `func` returns 0 is skipped **without** using up the budget.
  That's the hook `kq_writer_wake()` uses to say "my request doesn't fit yet,
  wake someone else".
* `autoremove_wake_function` wakes the task and removes the entry from the
  queue. The waiter re-adds itself (at the tail) if it has to sleep again.

Wake-one needs **hand-off**: whoever leaves data or space behind, or bails out
after being woken, must wake the next waiter. Otherwise a sleeper can be left
behind with nothing left to wake it.

Measured effect (run 1, v1 wake-all): 16 writers / 1 reader, 4 KiB records,
1.27M writer sleeps for 320K records. See benchmark_results.md for v2.

#### Exercises (wake-ups)

1. Why does plain wake-one break when writers need different amounts of space?
2. What does returning 0 from a wake function mean to `__wake_up_common()`?
3. Two readers sleep exclusively; a writer queues 2 bytes; each reader asks
   for 1. Trace what happens with and without the reader hand-off.
4. Why can two large writers never wake each other in an endless loop in v2?

<details><summary>Answers</summary>

1. Wake-one may pick a writer whose request doesn't fit. It re-sleeps, and a
   writer whose request would fit is never woken until some later event.
2. "This entry didn't take the wake-up." It isn't counted against
   `nr_exclusive`, so the walk continues to the next entry.
3. Without hand-off, the write wakes one reader, which takes 1 byte. The other
   byte stays queued and the second reader sleeps forever if no more writes
   come. With hand-off, the first reader sees data left after its read and
   wakes the second one. This is test `wake_hands_on_to_next_reader`.
4. A writer that can't proceed doesn't wake anybody. It just goes back to
   sleep. And the wake function never wakes a writer that doesn't fit. Wake-ups
   only originate from real state changes or from tasks that consumed or
   skipped a resource they had been given.

</details>

---

## Milestone 5: ioctl interface

* **Encoding.** `_IOR(type, nr, T)` packs direction (read from the driver's
  perspective = data goes to user), `sizeof(T)`, a magic "type" byte (`0xBA`)
  and a command number into 32 bits. Changing the struct size changes the
  command number, so mismatched binaries get `ENOTTY` instead of corrupt data.
* **`ENOTTY`** is the required answer for unknown commands. `isatty()` uses it.
* **ABI design.** Only `__u64` fields: no padding, so no stack bytes can leak,
  and the same layout on 32 and 64 bit. That's why `.compat_ioctl =
  compat_ptr_ioctl` (which only converts the pointer argument) is sufficient.
* **Permissions.** Destructive commands check `filp->f_mode & FMODE_WRITE`.
* **Locking discipline.** Snapshot under the mutex, `copy_to_user` after
  unlocking.

#### Exercises

1. What does `ioctl(fd, _IOR(0xBA, 1, uint32_t), &x)` return, and why?
2. Why might returning `-EINVAL` for unknown ioctls cause bugs in unrelated
   programs?
3. What could leak if `struct kq_stats` had a `__u32` field followed by a
   `__u64`?
4. Why does `KQ_IOC_RESET` wake `writeq` but not `readq`?

<details><summary>Answers</summary>

1. `-1/ENOTTY`. The size field differs from `sizeof(struct kq_stats)`, so it
   is a different command number and hits `default:`.
2. Library code probes ioctls (e.g. `isatty` → `TCGETS`) and interprets
   `ENOTTY` specifically as "not supported". `EINVAL` means "supported, bad
   argument", which misleads the caller.
3. 4 bytes of padding between the two fields. If the struct were built on the
   stack without zeroing, those bytes would contain old kernel stack data and
   be copied to user space: an information leak.
4. After a reset the queue is empty, so writers have space (they can make
   progress) and readers still have nothing to read.

</details>

---

## Milestones 6–8: testing and benchmarking techniques

* **Fork per test + parent-side timeout** (`tests/kq_test.c`). A hung
  blocking call becomes `TIMEOUT`, not a frozen run. If even `SIGKILL` can't
  reap the child, it's stuck in uninterruptible sleep (state D), which points
  at the kernel. The runner reports `STUCK` and stops.
* **Using the driver's counters to synchronise tests.** "Is the reader asleep
  yet?" is answered by polling `stats.read_waits` instead of `sleep(1)`. That
  makes blocking tests deterministic and fast.
* **Exact expectations.** The device is reset before each test and used by one
  thread, so every return value can be predicted (`t_wraparound_stream`
  predicts each call from a model).
* **Guard pages** (`mmap` + `mprotect(PROT_NONE)`) create precise partial-fault
  scenarios.
* **Mutation testing / fault injection** check that tests *can fail*:
  hand-made mutants of `kq_ring.h` and `test_stress --inject` must be caught.
* **Integrity design** (`test_stress`): per-writer sequence numbers + checksums
  + payload regeneration + an atomic "seen" bitmap. FIFO implies per-writer
  order is preserved for each reader. Poison records shut down readers cleanly
  because FIFO puts them behind all real data.
* **Reservoir sampling** (`kq_bench`) keeps a uniform sample of latencies in
  fixed memory. Percentiles are computed from merged samples.
* **Sanitizers.** ASan (memory errors), UBSan (undefined behaviour) and TSan
  (data races) instrument user-space code only. The kernel equivalents are
  KASAN, UBSAN, KCSAN and lockdep, which need a kernel built with them.

#### Exercises

1. Why does `test_stress` require every `read()` to return exactly one record,
   and why is that a valid expectation for this driver but not for a TCP
   socket?
2. With 4 readers, why can't the test require each writer's sequence numbers
   to be consecutive per reader? What weaker property does it check?
3. Why are poison records guaranteed to be read only after every real record?
4. Why does unthrottled end-to-end latency say more about queue depth than
   about the driver?

<details><summary>Answers</summary>

1. Every write is a whole record of ≤ 4096 bytes and is atomic, so `used` is
   always a multiple of the record size and a read of one record's size
   returns exactly one. TCP is a byte stream with no atomic-write guarantee or
   record boundaries, so reads can return any split.
2. Records from one writer are spread across readers, so each reader sees a
   subsequence with gaps. FIFO order still implies the subsequence is
   **strictly increasing**. With exactly one reader, it must be consecutive.
3. They are written after all writer threads have been joined, so every real
   record is already queued. The queue is FIFO, so every real record is
   dequeued before the first poison record.
4. Writers keep the queue full, so a record waits behind ~capacity bytes:
   latency ≈ capacity / throughput, which is mostly queueing time. Use
   `--rate` to measure the driver path on a lightly loaded queue.

</details>

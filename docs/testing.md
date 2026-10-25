# Testing and debugging guide

The current results are in [implementation_status.md](implementation_status.md).
This page describes **what each test layer proves**, how to run it, and which
debugging tools apply. Tools that were actually used on this project are
marked **(used)**; the rest are recommendations.

## Test layers

| Layer | Command | Needs driver? | What it proves |
|---|---|---|---|
| Ring unit test | `make test-unit` | no | `kq_ring.h` index arithmetic matches a reference model over ~1.8M random operations and 9 capacities; boundary cases; overflow edge at `SIZE_MAX/2`. |
| Harness self-test | `make test-harness` | no | `test_stress` passes on a correct queue (a named pipe), and detects injected duplicates, losses, corruption and split records. Validates the *checker*, not the driver. |
| Sanitizers | `make sanitize` | no | The two layers above under ASan + UBSan. |
| Functional | `make test` | yes | 27 single-purpose tests of the read/write/poll/ioctl contract, including blocking, signals, partial faults, bad pointers, and three tests that hang if exclusive wake-ups lose a wake-up. Exact expected values. |
| Stress | `make stress` | yes | MPMC integrity under contention: 7 configurations (1–16 writers × 1–16 readers, 32–4096-byte records, blocking and poll-driven). |
| Device cycle | `make check-device` (or `scripts/run_device_tests.sh --bench`) | loads it | Functional + stress in both wake-up modes, the `kqctl` smoke test and the 32-bit compat test, functional at min/max capacity, parameter rejection, optional benchmark, and a kernel-log scan for warnings, errors, BTF mismatches and lockdep reports. Needs sudo (asks once). |
| Wake-mode A/B | `scripts/ab_wake.sh [cap] [W] [R] [size] [reps]` | loads it | Interleaved same-session comparison of `exclusive_wake=1` vs `0` for one workload (integrity run + benchmark per step). |
| Benchmark | `make bench` | yes | Throughput and latency percentiles vs a FIFO baseline. Not a correctness test. |

All test programs exit nonzero on failure, and the device targets fail when
`/dev/kernqueue` is missing rather than skipping.

### Running one functional test

```bash
build/test_functional --list
build/test_functional poll          # every test whose name contains "poll"
```

### Stress options

```bash
build/test_stress -w 8 -r 8 -n 100000 -s 512          # count-based
build/test_stress -w 8 -r 8 -t 60 -s 256              # 60 s soak
build/test_stress -w 4 -r 4 -n 100000 -s 100 --nonblock
KQ_SOAK_SECONDS=600 make stress                       # add a 10 min soak
```

## Debugging tools

### Kernel log (used)
```bash
dmesg -w | grep kernqueue        # dmesg_restrict=0 on WSL; else use sudo
```
`kq_check_ring()` runs `WARN_ON_ONCE(!kq_ring_valid())` after every transfer,
so an invariant violation prints a stack trace here. `run_device_tests.sh`
fails if any `WARNING`, `BUG`, `Oops` or lockdep report appears.

### Dynamic debug (available, `CONFIG_DYNAMIC_DEBUG=y`)
The driver has `pr_debug()` in open, release and reset:
```bash
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 'module kernqueue +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

### ftrace (available, `CONFIG_FUNCTION_TRACER=y`)
```bash
cd /sys/kernel/tracing
echo 'kq_*' | sudo tee set_ftrace_filter
echo function | sudo tee current_tracer
sudo cat trace_pipe
```

### strace (installed)
```bash
strace -e trace=read,write,ioctl,poll build/kqctl -n read 10   # see EAGAIN etc.
```

### GDB (installed)
User-space: `gdb --args build/test_stress -w 2 -r 2`. Kernel-side, the module
can be inspected with `gdb vmlinux` plus `/proc/kcore` (read-only, root), or
properly with a VM that exposes a gdbstub (QEMU `-s`). WSL doesn't offer a
kernel gdbstub.

### Sanitizers for user space (used)
```bash
make sanitize                                    # ASan + UBSan
make BUILD=build/tsan SANITIZE=thread build/tsan/test_stress
setarch "$(uname -m)" -R build/tsan/test_stress -d /path/to/fifo   # TSan on WSL
```
`setarch -R` is needed because WSL's mmap randomisation breaks TSan's shadow
memory layout ("unexpected memory mapping").

### sparse (used)
```bash
make sparse        # Kbuild C=2: runs sparse on every driver source file
```
sparse understands the kernel's `__user` / `__iomem` address-space
annotations and lock-context annotations. The driver is sparse-clean. To show
the check is live, a mutant that replaced one `copy_to_user()` with
`memcpy()` into the `__user` buffer was flagged ("incorrect type in argument
1 (different address spaces)"). On WSL, sparse 0.6.4 was installed
user-locally from the Ubuntu archive (SHA-256 checked against apt's index)
into `~/kq-kbuild/tools`; `build_module.sh` falls back to that location.

### 32-bit compat path (used)
`build/test_compat32` is a freestanding i386 binary (`-m32 -nostdlib`, raw
`int $0x80` system calls, because no 32-bit libc is installed). It opens the
device, writes, reads, and issues `GET_STATS`, unknown and bad-pointer ioctls
through `compat_sys_ioctl()` → `compat_ptr_ioctl()`. It's part of the device
cycle on x86-64 hosts.

### Kernel sanitizers: lockdep, DEBUG_ATOMIC_SLEEP, KASAN (used); KCSAN (not run)
None of these are enabled in the stock WSL kernel, so a debug variant was
built and booted:

| Option | Catches |
|---|---|
| `CONFIG_PROVE_LOCKING` (lockdep, plus `DEBUG_MUTEXES`, `DEBUG_SPINLOCK`, `DEBUG_LOCK_ALLOC`) | lock-order inversions, invalid wait contexts, and makes `lockdep_assert_held()` live |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | anything that may sleep (e.g. `copy_to_user`) in atomic context |
| `CONFIG_KASAN` (generic, outline) + `KASAN_VMALLOC` | out-of-bounds and use-after-free, including on the vmalloc'd 16 MiB ring |
| `CONFIG_DEBUG_LIST` | corrupted list operations (wait queues are lists) |
| `CONFIG_KCSAN` | data races. **Not run**: mutually exclusive with KASAN, so it needs a second kernel |

Steps (they change system configuration):
1. `scripts/prepare_debug_kdir.sh`: git worktree of the stock tree, the
   options above, release `...-kqdebug`, builds `bzImage` (~20 min).
2. Copy `arch/x86/boot/bzImage` to Windows and create
   `%UserProfile%\.wslconfig` with `[wsl2]` / `kernel=C:\\path\\to\\bzImage`.
   Then `wsl --shutdown`.
3. `scripts/run_debug_checks.sh`: builds kernqueue against the debug tree,
   runs the full device cycle, checks lockdep stayed on and tracked
   kernqueue's locks, then loads `tests/debugcheck/kq_debugcheck.ko` three
   times to trigger one deliberate bug per checker and require each report.
4. Delete `.wslconfig` and `wsl --shutdown` to return to the stock kernel.

**Result (debug run 2, docs/evidence/debug-run2):** the whole device cycle
passed with no lockdep, KASAN, sleep-in-atomic or list report. Lockdep stayed
on and listed `&kq.lock`, `&kq.readq` and `&kq.writeq`. All three
deliberate bugs were reported (circular locking, sleeping in invalid context,
`slab-out-of-bounds`).

**Self-test ordering matters.** Any lockdep report switches lockdep off for
the rest of the boot. The `atomic_sleep` self-test produces one too: lockdep
reports "Invalid wait context" because `might_fault()` takes `mmap_lock`, a
sleeping lock, under the spinlock. Run in that order first, it silenced the
`lock_order` test (debug run 1). The script now runs `lock_order` first and
asserts `debug_locks=1` just before it.

The debug kernel also printed boot-time lockdep assertion warnings from
**Xwayland** (WSLg: `remap_pfn_range` without the mmap write lock). That is
unrelated to kernqueue and logged before the test marker. These were plain
`WARN`s, which leave lockdep on (`debug_locks` stayed 1).

A disposable QEMU VM with a debug kernel is the cleaner alternative to
swapping the WSL kernel.

## When a test fails

1. Rerun only that test: `build/test_functional <name>`.
2. Check `dmesg` for a `WARN_ON_ONCE` from `kq_check_ring` or an oops.
3. A `TIMEOUT` with no oops usually means a missing wake-up. Look at which wait
   queue the blocked thread sleeps on; `/proc/<pid>/stack` (as root) shows
   where it is.
4. Fix the driver, rebuild (`make module`), reload, and rerun the failing test
   and then the full cycle (`make check-device`).
5. Record the failure, cause and fix in implementation_status.md.

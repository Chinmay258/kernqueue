# KernQueue

A Linux character device driver (`/dev/kernqueue`) that exposes a bounded,
blocking, thread-safe byte FIFO to user space, plus a C test and benchmark
framework for it.

It's a software-only driver (no hardware, IRQs or DMA). The focus is the parts
every driver needs to get right:
- device registration and module lifecycle
- safe copying across the kernel/user boundary, including partial faults
- locking that respects the kernel's sleeping rules
- wait-queue blocking, `O_NONBLOCK` and `poll`
- a small, ABI-stable ioctl interface

> **Warning.** An experimental kernel module runs with full kernel privileges.
> A bug can crash or hang the whole machine and lose unsaved work. Use a
> disposable VM. Under WSL2, a crash restarts the WSL VM (all distros,
> including Docker Desktop's), not Windows.

Status, test results and known gaps: [docs/implementation_status.md](docs/implementation_status.md).
Nothing here has been security-audited, and it is not production code.

## Repository layout

```
driver/      kernqueue.c (module), kq_ring.h (ring index logic), Kbuild, Makefile
include/     kernqueue.h: user-space API shared by the driver and programs (ioctls, stats)
user/        kqctl (CLI client), kq_common.{c,h} (shared helpers, record format)
tests/       test_ring (unit, no driver), test_functional, test_stress, kq_test runner
bench/       kq_bench (throughput/latency), run_bench.sh, summarize.py, results/
scripts/     kernel-tree preparation, module build/load/unload, test cycles
docs/        design, learning notes, testing guide, status, interview prep
docs/evidence/  raw logs of the device test runs and the wake-mode A/B
```

## Semantics in one table

| Call | Behaviour |
|---|---|
| `read(n)` | Blocks while empty (or `EAGAIN` with `O_NONBLOCK`), then returns 1..n bytes. |
| `write(n)`, n ≤ 4096 | Atomic: waits for n free bytes, queues all of them (or `EAGAIN`). |
| `write(n)`, n > 4096 | Waits for 1 free byte, queues as much as fits, returns the count. |
| `poll` | `POLLIN`: ≥ 1 byte queued. `POLLOUT`: ≥ 4096 bytes free. |
| `KQ_IOC_GET_STATS` | Snapshot of capacity, usage and counters. |
| `KQ_IOC_RESET` | Discard queued data (writable fd only). |
| `KQ_IOC_CLEAR_STATS` | Zero the counters (writable fd only). |

Design rationale and the locking argument: [docs/design.md](docs/design.md).

## Prerequisites

* Linux x86-64 with `CONFIG_MODULES=y`. Tested only on WSL2 kernel
  `6.6.87.1-microsoft-standard-WSL2`. Kernel API differences are noted in
  design.md §6.
* gcc, make, git, and Python 3 (for the benchmark summary).
* A kernel build tree matching the running kernel:
  * **Regular distro:** `sudo apt install build-essential linux-headers-$(uname -r)`
    (Debian/Ubuntu). The build uses `/lib/modules/$(uname -r)/build`.
  * **WSL2:** no headers are published, so build the matching tree yourself:
    ```bash
    sudo apt install flex bison libelf-dev libssl-dev dwarves bc cpio gcc-11
    scripts/prepare_wsl_kdir.sh     # clone + configure + full build into ~/kq-kbuild (~30 min)
    ```
    The script uses `/proc/config.gz`, refuses to continue on unexplained
    config differences, and records the compiler to use for modules.
* Secure Boot or kernel lockdown setups that enforce module signing will
  reject the unsigned module. WSL2 doesn't enforce signing.

## Build

```bash
make              # user-space programs into build/ (strict warnings, -Werror)
make module       # build/kernqueue.ko (KDIR=... to override the kernel tree)
make sparse       # same, with the sparse static checker on the driver
```

Kbuild can't handle spaces in paths. If the repository path contains spaces,
`scripts/build_module.sh` stages the driver sources in
`~/.cache/kernqueue-build` automatically.

## Load, use, unload

```bash
scripts/load_module.sh                    # sudo insmod; node gets your group, mode 0660
scripts/load_module.sh capacity=4096      # ring size in bytes, 4096..16777216
scripts/load_module.sh exclusive_wake=0   # v1 behaviour: wake every waiter (for A/B tests)
build/kqctl write "hello"
build/kqctl stats
build/kqctl read 100                      # prints "hello"
build/kqctl -n read 1                     # O_NONBLOCK on an empty queue: EAGAIN
scripts/unload_module.sh                  # sudo rmmod; EBUSY while any fd is open
```

The node `/dev/kernqueue` is created automatically by devtmpfs/udev. Without
devtmpfs, `mknod /dev/kernqueue c <major> 0` with the major from
`/proc/devices`.

## Test

```bash
make test-unit      # ring logic vs a reference model (no driver needed)
make test-harness   # stress-checker self-test on a FIFO, with fault injection (no driver)
make sanitize       # both of the above under ASan + UBSan
make test           # functional tests (module loaded, device otherwise unused)
make stress         # concurrent integrity suite
make check-device   # full cycle: load at 3 capacities, test, scan dmesg, unload (sudo)
scripts/run_debug_checks.sh  # same under a lockdep+KASAN kernel (see docs/testing.md)
make bench          # benchmark matrix vs a FIFO baseline -> bench/results/<time>/
```

Details, debugging tools and what each layer proves: [docs/testing.md](docs/testing.md).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `make module`: "not a prepared kernel tree" | No headers for the running kernel. Install `linux-headers-$(uname -r)`, or on WSL2 run `scripts/prepare_wsl_kdir.sh`. |
| `insmod: Invalid module format` | Built against a different kernel/config. Check `modinfo -F vermagic build/kernqueue.ko` against `uname -r`; see `dmesg`. |
| `insmod: Invalid parameters` | `capacity` is outside 4096..16777216. `dmesg` says which. |
| `open: Permission denied` | The node is 0600 root after a manual insmod. Use `scripts/load_module.sh`, or `sudo chmod 0660 /dev/kernqueue`. |
| `rmmod: Module kernqueue is in use` | Some process has the device open: `sudo lsof /dev/kernqueue`. |
| Tests fail with unexpected counters | Another process is using the device; tests need it exclusively. |
| `prepare_wsl_kdir.sh` stops on config differences | Review the listed options; rerun with `KQ_ALLOW_CONFIG_DIFF=1` only if they can't affect the ABI. |
| Kbuild errors mentioning a path fragment | A space in the path when running `driver/Makefile` directly; use `make module`. |
| TSan: "unexpected memory mapping" | Run under `setarch $(uname -m) -R`. |

## Limitations

* One device instance. All opens share one queue.
* User copies happen while holding the mutex, so a client with a slow-faulting
  buffer (e.g. `userfaultfd`) can stall other clients. Same as pipes.
* No fairness guarantee between large and small writers. The need-aware
  wake-up deliberately prefers writers whose request fits now.
* The default exclusive wake-ups remove the thundering herd measured in v1,
  but they aren't uniformly faster; see docs/benchmark_results.md.
* Verified on one kernel (6.6, WSL2, x86-64) only, plus a debug build of it
  with lockdep, DEBUG_ATOMIC_SLEEP and KASAN, under which the full test cycle
  was clean. KCSAN (data races) has not been run; see docs/testing.md.

## Documentation

* [docs/design.md](docs/design.md): semantics, invariants, locking, lifecycle
* [docs/learning_notes.md](docs/learning_notes.md): concepts with exercises
* [docs/testing.md](docs/testing.md): test layers, debugging tools
* [docs/implementation_status.md](docs/implementation_status.md): what is verified
* [docs/benchmark_results.md](docs/benchmark_results.md): measured results, methodology and trade-offs
* [docs/evidence/](docs/evidence/): raw logs behind every reported result
* [docs/interview_prep.md](docs/interview_prep.md): likely questions and résumé text

## License

GPL-2.0 (the module uses GPL-only kernel symbols such as `device_create`).

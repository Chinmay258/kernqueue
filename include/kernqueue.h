/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * kernqueue.h - user-space API of the KernQueue character device.
 *
 * This header is included by both the kernel module and the user-space
 * programs, so it only uses <linux/ioctl.h> and the fixed-width __u64 types
 * from <linux/types.h>. Every field of struct kq_stats is a __u64, so the
 * structure has no padding and the same size and layout for 32-bit and 64-bit
 * processes. That is what allows the driver to use compat_ptr_ioctl() instead
 * of a hand-written 32-bit compat handler.
 */
#ifndef KERNQUEUE_UAPI_H
#define KERNQUEUE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KQ_DEVICE_NAME "kernqueue"
#define KQ_DEVICE_PATH "/dev/kernqueue"

/*
 * A write of at most KQ_ATOMIC_MAX bytes is atomic: either all of it is
 * queued contiguously or none of it is. This mirrors POSIX PIPE_BUF and is
 * what keeps fixed-size records intact when several writers share the queue.
 * poll() reports EPOLLOUT only when at least this much space is free.
 */
#define KQ_ATOMIC_MAX 4096u

/*
 * Ring capacity bounds in bytes, enforced on the "capacity" module parameter.
 * The minimum must be at least KQ_ATOMIC_MAX: an atomic write waits until all
 * of it fits, so a smaller ring would let such a write block forever.
 */
#define KQ_MIN_CAPACITY KQ_ATOMIC_MAX
#define KQ_MAX_CAPACITY (16u << 20)
#define KQ_DEFAULT_CAPACITY (64u << 10)

/*
 * Snapshot returned by KQ_IOC_GET_STATS. "capacity", "used" and "open_files"
 * describe current state; the other fields are cumulative since module load
 * or the last KQ_IOC_CLEAR_STATS.
 */
struct kq_stats {
	__u64 capacity;      /* ring size in bytes */
	__u64 used;          /* bytes currently queued */
	__u64 open_files;    /* open file descriptions of the device */
	__u64 bytes_written; /* bytes accepted by write() */
	__u64 bytes_read;    /* bytes returned by read() */
	__u64 write_calls;   /* write() calls that queued at least one byte */
	__u64 read_calls;    /* read() calls that returned at least one byte */
	__u64 write_waits;   /* times a writer went to sleep waiting for space */
	__u64 read_waits;    /* times a reader went to sleep waiting for data */
	__u64 write_eagain;  /* O_NONBLOCK writes refused with EAGAIN */
	__u64 read_eagain;   /* O_NONBLOCK reads refused with EAGAIN */
	__u64 resets;        /* successful KQ_IOC_RESET calls */
};

/*
 * 0xBA is not assigned in Documentation/userspace-api/ioctl/ioctl-number.rst
 * (checked against the 6.6 tree). It is not registered either: this is an
 * out-of-tree teaching driver.
 */
#define KQ_IOC_MAGIC 0xBA

/* Copy a struct kq_stats snapshot to user space. */
#define KQ_IOC_GET_STATS _IOR(KQ_IOC_MAGIC, 0x01, struct kq_stats)
/* Discard all queued bytes and wake blocked writers. Needs a writable fd. */
#define KQ_IOC_RESET _IO(KQ_IOC_MAGIC, 0x02)
/* Zero the cumulative counters. Needs a writable fd. */
#define KQ_IOC_CLEAR_STATS _IO(KQ_IOC_MAGIC, 0x03)

#endif /* KERNQUEUE_UAPI_H */

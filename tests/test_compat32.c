// SPDX-License-Identifier: GPL-2.0
/*
 * test_compat32.c - exercise /dev/kernqueue from a 32-bit (i386) process.
 *
 * A 32-bit process's ioctl() enters the kernel through compat_sys_ioctl()
 * and reaches the driver's .compat_ioctl (compat_ptr_ioctl), not
 * .unlocked_ioctl directly. Without a compat handler every kernqueue ioctl
 * would fail with ENOTTY for 32-bit callers. This checks that the handler is
 * wired up and that struct kq_stats has the same layout in both ABIs.
 *
 * Ubuntu does not ship a 32-bit C library by default, so this program is
 * built with -m32 -nostdlib and makes raw "int $0x80" system calls. It always
 * uses /dev/kernqueue. Exit status 0 means pass; otherwise it is the number of
 * the first failed check.
 */
#include <kernqueue.h>

/* i386 system call numbers (arch/x86/entry/syscalls/syscall_32.tbl). */
#define NR_exit 1
#define NR_read 3
#define NR_write 4
#define NR_open 5
#define NR_close 6
#define NR_ioctl 54

#define O_RDWR 2
#define EFAULT 14
#define ENOTTY 25

/* The same command number in both ABIs relies on this. */
_Static_assert(sizeof(struct kq_stats) == 12 * 8, "kq_stats must be 96 bytes");

/* Returns the raw kernel result: >= 0, or -errno. */
static long sys3(long nr, long a, long b, long c)
{
	long ret;

	__asm__ volatile("int $0x80"
			 : "=a"(ret)
			 : "a"(nr), "b"(a), "c"(b), "d"(c)
			 : "memory");
	return ret;
}

static void say(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sys3(NR_write, 1, (long)s, n);
}

static void __attribute__((noreturn)) finish(int failed_check)
{
	say(failed_check ? "test_compat32: FAIL\n" : "test_compat32: PASS\n");
	sys3(NR_exit, failed_check, 0, 0);
	for (;;)
		;
}

#define CHECK(n, cond)                 \
	do {                           \
		if (!(cond))           \
			finish(n);     \
	} while (0)

void _start(void);

__attribute__((force_align_arg_pointer)) void _start(void)
{
	static const char msg[8] = "compat32";
	static struct kq_stats st;
	static char buf[64];
	long fd, i;

	fd = sys3(NR_open, (long)KQ_DEVICE_PATH, O_RDWR, 0);
	CHECK(1, fd >= 0);
	CHECK(2, sys3(NR_ioctl, fd, KQ_IOC_RESET, 0) == 0);
	CHECK(3, sys3(NR_ioctl, fd, KQ_IOC_CLEAR_STATS, 0) == 0);
	CHECK(4, sys3(NR_write, fd, (long)msg, sizeof(msg)) == sizeof(msg));

	/* The compat path: a 32-bit pointer to a struct with 64-bit fields. */
	CHECK(5, sys3(NR_ioctl, fd, KQ_IOC_GET_STATS, (long)&st) == 0);
	CHECK(6, st.used == sizeof(msg));
	CHECK(7, st.bytes_written == sizeof(msg) && st.write_calls == 1);
	CHECK(8, st.capacity >= KQ_MIN_CAPACITY && st.capacity <= KQ_MAX_CAPACITY);

	CHECK(9, sys3(NR_read, fd, (long)buf, sizeof(buf)) == sizeof(msg));
	for (i = 0; i < (long)sizeof(msg); i++)
		CHECK(10, buf[i] == msg[i]);

	CHECK(11, sys3(NR_ioctl, fd, _IO(KQ_IOC_MAGIC, 0x7f), 0) == -ENOTTY);
	CHECK(12, sys3(NR_ioctl, fd, KQ_IOC_GET_STATS, 1) == -EFAULT);
	CHECK(13, sys3(NR_close, fd, 0, 0) == 0);
	finish(0);
}

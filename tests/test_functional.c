// SPDX-License-Identifier: GPL-2.0
/*
 * test_functional.c - functional tests for /dev/kernqueue.
 *
 * Requires the module to be loaded and the device to be otherwise unused:
 * every test starts by resetting the queue and zeroing the statistics, then
 * computes exact expected results. Tests that involve blocking use the
 * driver's own counters (read_waits/write_waits) to prove that a thread is
 * really asleep in the driver before acting, instead of guessing with sleeps.
 */
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kq_common.h"
#include "kq_test.h"

#define POLL_MASK (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)

static size_t g_cap; /* ring capacity, refreshed by fresh() */

/* ---------------------------------------------------------------- helpers */

static int dev_open(int flags)
{
	int fd = open(kq_device_path(), flags | O_CLOEXEC);

	if (fd < 0)
		KQ_FAIL("open(%s): %s (is the module loaded and the node accessible?)",
			kq_device_path(), strerror(errno));
	return fd;
}

static struct kq_stats dev_stats(int fd)
{
	struct kq_stats st;

	if (kq_get_stats(fd, &st) != 0)
		KQ_FAIL("KQ_IOC_GET_STATS: %s", strerror(errno));
	return st;
}

/* Empty the queue, zero the counters, then open with the given flags. */
static int fresh(int flags)
{
	int ctl = dev_open(O_RDWR);

	KQ_CHECK(ioctl(ctl, KQ_IOC_RESET) == 0);
	KQ_CHECK(ioctl(ctl, KQ_IOC_CLEAR_STATS) == 0);
	g_cap = (size_t)dev_stats(ctl).capacity;
	KQ_CHECK(g_cap >= KQ_MIN_CAPACITY && g_cap <= KQ_MAX_CAPACITY);
	close(ctl);
	return dev_open(flags);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);

	if (!p)
		KQ_FAIL("malloc(%zu) failed", n);
	return p;
}

static void fill_pattern(uint8_t *buf, uint64_t start, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = kq_pattern_byte(start + i);
}

static void check_pattern(const uint8_t *buf, uint64_t start, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (buf[i] != kq_pattern_byte(start + i))
			KQ_FAIL("stream byte %llu: got 0x%02x, expected 0x%02x",
				(unsigned long long)(start + i), buf[i],
				kq_pattern_byte(start + i));
}

/* Queue n stream bytes starting at offset start, in atomic-sized chunks. */
static void put_stream(int fd, uint64_t start, size_t n)
{
	uint8_t chunk[KQ_ATOMIC_MAX];

	while (n) {
		size_t k = n < sizeof(chunk) ? n : sizeof(chunk);

		fill_pattern(chunk, start, k);
		KQ_CHECK_EQ(write(fd, chunk, k), k);
		start += k;
		n -= k;
	}
}

/* Dequeue exactly n bytes and check they are stream bytes start.. */
static void get_stream(int fd, uint64_t start, size_t n)
{
	uint8_t chunk[KQ_ATOMIC_MAX];

	while (n) {
		size_t k = n < sizeof(chunk) ? n : sizeof(chunk);
		ssize_t r = read(fd, chunk, k);

		KQ_CHECK(r > 0);
		check_pattern(chunk, start, (size_t)r);
		start += (uint64_t)r;
		n -= (size_t)r;
	}
}

/* Wait (bounded) until a statistics counter reaches at least min. */
#define WAIT_STAT_GE(fd, field, min)                                       \
	wait_stat_ge(fd, offsetof(struct kq_stats, field), (min), #field, \
		     __LINE__)

static void wait_stat_ge(int fd, size_t off, uint64_t min, const char *name,
			 int line)
{
	uint64_t deadline = kq_now_ns() + 3000000000ULL;
	uint64_t v;

	for (;;) {
		struct kq_stats st = dev_stats(fd);

		memcpy(&v, (const char *)&st + off, sizeof(v));
		if (v >= min)
			return;
		if (kq_now_ns() > deadline)
			kq_test_fail(__FILE__, line,
				     "stats.%s stayed at %llu, expected >= %llu",
				     name, (unsigned long long)v,
				     (unsigned long long)min);
		kq_sleep_ms(1);
	}
}

/*
 * A deliberately invalid user pointer, laundered through a volatile so that
 * glibc's fortify checks cannot reject the call at compile time; testing how
 * the driver handles such pointers is the point.
 */
static void *bad_ptr(uintptr_t addr)
{
	volatile uintptr_t v = addr;

	return (void *)v;
}

static short poll_now(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };

	KQ_CHECK(poll(&p, 1, 0) >= 0);
	return (short)(p.revents & POLL_MASK);
}

/* Map npages pages read/write, with the page after them inaccessible. */
static uint8_t *map_with_guard(size_t npages)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *m = mmap(NULL, (npages + 1) * pg, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	KQ_CHECK(m != MAP_FAILED);
	KQ_CHECK(mprotect(m + npages * pg, pg, PROT_NONE) == 0);
	return m;
}

/* Move head to the given offset on an empty queue (to force wraparound). */
static void set_head(int fd, size_t offset)
{
	put_stream(fd, 0, offset);
	get_stream(fd, 0, offset);
}

/* Read and discard n bytes. */
static void drain(int fd, size_t n)
{
	uint8_t chunk[KQ_ATOMIC_MAX];

	while (n) {
		ssize_t r = read(fd, chunk, n < sizeof(chunk) ? n : sizeof(chunk));

		KQ_CHECK(r > 0);
		n -= (size_t)r;
	}
}

/* Queue filler bytes until exactly target bytes are queued. */
static void fill_to(int fd, size_t target)
{
	size_t used = (size_t)dev_stats(fd).used;

	KQ_CHECK(target >= used);
	put_stream(fd, 0, target - used);
	KQ_CHECK_EQ(dev_stats(fd).used, target);
}

struct io_thread {
	pthread_t tid;
	int fd;
	size_t len;
	uint8_t buf[KQ_ATOMIC_MAX];
	ssize_t ret;
	int err;
	atomic_bool done;
};

static void *reader_main(void *arg)
{
	struct io_thread *t = arg;

	t->ret = read(t->fd, t->buf, t->len);
	t->err = errno;
	atomic_store(&t->done, true);
	return NULL;
}

static void *writer_main(void *arg)
{
	struct io_thread *t = arg;

	t->ret = write(t->fd, t->buf, t->len);
	t->err = errno;
	atomic_store(&t->done, true);
	return NULL;
}

static void start_thread(struct io_thread *t, void *(*fn)(void *))
{
	atomic_init(&t->done, false);
	KQ_CHECK(pthread_create(&t->tid, NULL, fn, t) == 0);
}

static void join_within(struct io_thread *t, unsigned int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	int rc = pthread_timedjoin_np(t->tid, NULL, &ts);

	if (rc != 0)
		KQ_FAIL("thread did not finish within %u ms (%s)", ms,
			strerror(rc));
}

/* ------------------------------------------------------------------ tests */

static void t_open_modes(void)
{
	int rw = fresh(O_RDWR);
	int ro = dev_open(O_RDONLY);
	int wo = dev_open(O_WRONLY);
	char c = 'x';

	KQ_CHECK_EQ(dev_stats(rw).open_files, 3);

	/* dup() shares the open file description: no open(), no release(). */
	int d = dup(rw);
	KQ_CHECK(d >= 0);
	KQ_CHECK_EQ(dev_stats(rw).open_files, 3);
	close(d);
	KQ_CHECK_EQ(dev_stats(rw).open_files, 3);

	/* The VFS enforces the access mode before the driver is called. */
	KQ_CHECK_ERRNO(write(ro, &c, 1), EBADF);
	KQ_CHECK_ERRNO(read(wo, &c, 1), EBADF);

	close(ro);
	close(wo);
	KQ_CHECK_EQ(dev_stats(rw).open_files, 1);
	close(rw);
}

static void t_capacity_matches_param(void)
{
	int fd = fresh(O_RDONLY);
	FILE *f = fopen("/sys/module/kernqueue/parameters/capacity", "r");
	unsigned long long param = 0;

	KQ_CHECK(f != NULL);
	KQ_CHECK(fscanf(f, "%llu", &param) == 1);
	fclose(f);
	KQ_CHECK_EQ(dev_stats(fd).capacity, param);
	close(fd);
}

static void t_roundtrip_and_counters(void)
{
	int fd = fresh(O_RDWR);
	char buf[32] = { 0 };
	struct kq_stats st;

	KQ_CHECK_EQ(write(fd, "hello", 5), 5);
	KQ_CHECK_EQ(write(fd, " world", 6), 6);
	st = dev_stats(fd);
	KQ_CHECK_EQ(st.used, 11);
	KQ_CHECK_EQ(st.bytes_written, 11);
	KQ_CHECK_EQ(st.write_calls, 2);

	KQ_CHECK_EQ(read(fd, buf, 3), 3);
	KQ_CHECK(memcmp(buf, "hel", 3) == 0);
	/* Asking for more than is queued returns what is there: a short read. */
	KQ_CHECK_EQ(read(fd, buf, sizeof(buf)), 8);
	KQ_CHECK(memcmp(buf, "lo world", 8) == 0);

	st = dev_stats(fd);
	KQ_CHECK_EQ(st.used, 0);
	KQ_CHECK_EQ(st.bytes_read, 11);
	KQ_CHECK_EQ(st.read_calls, 2);
	KQ_CHECK_EQ(st.read_waits, 0);
	KQ_CHECK_EQ(st.write_waits, 0);
	close(fd);
}

static void t_zero_length(void)
{
	int fd = fresh(O_RDWR);
	uint8_t *buf = xmalloc(g_cap);

	/* A zero-byte read must not block on an empty queue (or touch memory). */
	KQ_CHECK_EQ(read(fd, bad_ptr(0), 0), 0);
	KQ_CHECK_EQ(write(fd, bad_ptr(0), 0), 0);

	fill_pattern(buf, 0, g_cap);
	KQ_CHECK_EQ(write(fd, buf, g_cap), g_cap);
	/* ...and a zero-byte write must not block on a full one. */
	KQ_CHECK_EQ(write(fd, bad_ptr(0), 0), 0);

	struct kq_stats st = dev_stats(fd);
	KQ_CHECK_EQ(st.write_calls, 1);
	KQ_CHECK_EQ(st.read_calls, 0);
	KQ_CHECK_EQ(st.write_waits + st.read_waits, 0);
	free(buf);
	close(fd);
}

static void t_nonblock_empty_read(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	char c;

	KQ_CHECK_ERRNO(read(fd, &c, 1), EAGAIN);
	struct kq_stats st = dev_stats(fd);
	KQ_CHECK_EQ(st.read_eagain, 1);
	KQ_CHECK_EQ(st.read_waits, 0);
	close(fd);
}

/* O_NONBLOCK is sampled per call, so fcntl() changes take effect at once. */
static void t_nonblock_toggle(void)
{
	int fd = fresh(O_RDWR);
	uint8_t *buf = xmalloc(g_cap);
	int flags = fcntl(fd, F_GETFL);
	char c;

	KQ_CHECK(flags >= 0);
	KQ_CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
	KQ_CHECK_ERRNO(read(fd, &c, 1), EAGAIN);

	fill_pattern(buf, 0, g_cap);
	KQ_CHECK_EQ(write(fd, buf, g_cap), g_cap);
	KQ_CHECK_ERRNO(write(fd, buf, 1), EAGAIN);

	KQ_CHECK(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
	/* Blocking read with data queued returns immediately. */
	KQ_CHECK_EQ(read(fd, &c, 1), 1);
	KQ_CHECK_EQ((uint8_t)c, kq_pattern_byte(0));

	struct kq_stats st = dev_stats(fd);
	KQ_CHECK_EQ(st.read_eagain, 1);
	KQ_CHECK_EQ(st.write_eagain, 1);
	KQ_CHECK_EQ(st.read_waits, 0);
	free(buf);
	close(fd);
}

static void t_fill_to_capacity(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	uint8_t *buf = xmalloc(g_cap + 1);

	fill_pattern(buf, 0, g_cap + 1);
	/* One write of exactly the capacity fits whether or not it is atomic. */
	KQ_CHECK_EQ(write(fd, buf, g_cap), g_cap);
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap);

	KQ_CHECK_ERRNO(write(fd, buf, 1), EAGAIN);
	/* A non-atomic write needs only one free byte, and there is none. */
	KQ_CHECK_ERRNO(write(fd, buf, g_cap + 1), EAGAIN);
	KQ_CHECK_EQ(poll_now(fd), POLLIN);

	get_stream(fd, 0, 1);
	KQ_CHECK_EQ(write(fd, buf + g_cap, 1), 1);
	get_stream(fd, 1, g_cap);
	KQ_CHECK_EQ(dev_stats(fd).used, 0);
	KQ_CHECK_EQ(poll_now(fd), POLLOUT);
	free(buf);
	close(fd);
}

static void t_atomic_write_semantics(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	static uint8_t big[KQ_ATOMIC_MAX + 1];

	put_stream(fd, 0, g_cap - 1000);

	/* 2000 > 1000 free: an atomic write queues nothing. */
	KQ_CHECK_ERRNO(write(fd, big, 2000), EAGAIN);
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap - 1000);
	KQ_CHECK_EQ(write(fd, big, 1000), 1000);
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap);

	/* With 3000 free, a non-atomic (KQ_ATOMIC_MAX + 1) write is short. */
	get_stream(fd, 0, 3000);
	KQ_CHECK_EQ(write(fd, big, sizeof(big)), 3000);

	/* Same on a blocking fd: a short write, not a wait. */
	drain(fd, 3000);
	int flags = fcntl(fd, F_GETFL);
	KQ_CHECK(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
	KQ_CHECK_EQ(write(fd, big, sizeof(big)), 3000);
	KQ_CHECK_EQ(dev_stats(fd).write_waits, 0);
	close(fd);
}

/*
 * Single-threaded stream through a non-blocking fd with awkward write and
 * read sizes, so the head and tail cross the end of the ring at many offsets
 * via both atomic and non-atomic writes. Each call's result is predicted
 * exactly from the model (used, free) and every byte read is checked.
 */
static void t_wraparound_stream(void)
{
	static const size_t wsizes[] = { 1, 7, 61, 509, 4093, 4096, 4097,
					 8191, 20011 };
	static const size_t rsizes[] = { 3, 4096, 1, 777, 12289, 5, 65537 };
	int fd = fresh(O_RDWR | O_NONBLOCK);
	uint8_t *wbuf = xmalloc(20011), *rbuf = xmalloc(65537);
	uint64_t total = 6 * (uint64_t)g_cap + 12345, wpos = 0, rpos = 0;
	size_t wi = 0, ri = 0;

	while (rpos < total) {
		size_t used = (size_t)(wpos - rpos), avail = g_cap - used;
		size_t wn = wsizes[wi++ % (sizeof(wsizes) / sizeof(wsizes[0]))];
		ssize_t expect, r;

		if (wn > total - wpos)
			wn = (size_t)(total - wpos);
		if (wn) {
			if (wn <= KQ_ATOMIC_MAX)
				expect = avail >= wn ? (ssize_t)wn : -1;
			else
				expect = avail ? (ssize_t)(wn < avail ? wn : avail) : -1;
			fill_pattern(wbuf, wpos, wn);
			r = write(fd, wbuf, wn);
			if (expect < 0) {
				KQ_CHECK(r == -1 && errno == EAGAIN);
			} else {
				KQ_CHECK_EQ(r, expect);
				wpos += (uint64_t)r;
			}
		}

		size_t rn = rsizes[ri++ % (sizeof(rsizes) / sizeof(rsizes[0]))];
		used = (size_t)(wpos - rpos);
		expect = used ? (ssize_t)(rn < used ? rn : used) : -1;
		r = read(fd, rbuf, rn);
		if (expect < 0) {
			KQ_CHECK(r == -1 && errno == EAGAIN);
		} else {
			KQ_CHECK_EQ(r, expect);
			check_pattern(rbuf, rpos, (size_t)r);
			rpos += (uint64_t)r;
		}
		KQ_CHECK_EQ(dev_stats(fd).used, wpos - rpos);
	}
	struct kq_stats st = dev_stats(fd);
	KQ_CHECK_EQ(st.bytes_written, total);
	KQ_CHECK_EQ(st.bytes_read, total);
	free(wbuf);
	free(rbuf);
	close(fd);
}

static void t_blocking_read_wakes(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 64 };

	start_thread(&t, reader_main);
	WAIT_STAT_GE(fd, read_waits, 1); /* the reader is asleep in the driver */
	KQ_CHECK(!atomic_load(&t.done));
	KQ_CHECK_EQ(write(fd, "ping", 4), 4);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, 4);
	KQ_CHECK(memcmp(t.buf, "ping", 4) == 0);
	close(fd);
}

static void t_blocking_atomic_write_waits(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 10 };

	put_stream(fd, 0, g_cap);
	memset(t.buf, 'w', sizeof(t.buf));
	start_thread(&t, writer_main);
	WAIT_STAT_GE(fd, write_waits, 1);

	/* 5 bytes free is not enough for an atomic 10-byte write. */
	get_stream(fd, 0, 5);
	kq_sleep_ms(50);
	KQ_CHECK(!atomic_load(&t.done));
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap - 5);

	get_stream(fd, 5, 5);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, 10);
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap);
	close(fd);
}

static void on_sigusr1(int sig)
{
	(void)sig;
}

static void install_sigusr1(int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigusr1;
	sa.sa_flags = flags;
	sigemptyset(&sa.sa_mask);
	KQ_CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
}

/* -ERESTARTSYS becomes EINTR when the handler lacks SA_RESTART. */
static void t_signal_interrupts_read(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 8 };

	install_sigusr1(0);
	start_thread(&t, reader_main);
	WAIT_STAT_GE(fd, read_waits, 1);
	KQ_CHECK(pthread_kill(t.tid, SIGUSR1) == 0);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, -1);
	KQ_CHECK_EQ(t.err, EINTR);
	KQ_CHECK_EQ(dev_stats(fd).read_calls, 0);
	close(fd);
}

/* ...and with SA_RESTART the kernel transparently re-issues the read. */
static void t_signal_restarts_read(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 8 };

	install_sigusr1(SA_RESTART);
	start_thread(&t, reader_main);
	WAIT_STAT_GE(fd, read_waits, 1);
	KQ_CHECK(pthread_kill(t.tid, SIGUSR1) == 0);
	/* The restarted read() found the queue still empty and slept again. */
	WAIT_STAT_GE(fd, read_waits, 2);
	KQ_CHECK(!atomic_load(&t.done));
	KQ_CHECK_EQ(write(fd, "x", 1), 1);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, 1);
	KQ_CHECK_EQ(t.buf[0], 'x');
	close(fd);
}

/* Check poll on an O_RDWR, an O_RDONLY and an O_WRONLY fd at once. */
static void expect_poll(int rw, int ro, int wo, short in, short out)
{
	KQ_CHECK_EQ(poll_now(rw), in | out);
	KQ_CHECK_EQ(poll_now(ro), in);
	KQ_CHECK_EQ(poll_now(wo), out);
}

static void t_poll_states(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	int ro = dev_open(O_RDONLY | O_NONBLOCK);
	int wo = dev_open(O_WRONLY | O_NONBLOCK);
	char c = 'p';

	expect_poll(fd, ro, wo, 0, POLLOUT);

	/* Exactly KQ_ATOMIC_MAX free: writable (data queued unless cap == 4096). */
	fill_to(fd, g_cap - KQ_ATOMIC_MAX);
	expect_poll(fd, ro, wo, g_cap > KQ_ATOMIC_MAX ? POLLIN : 0, POLLOUT);

	/* One byte less free: not writable, although a 1-byte write still fits. */
	KQ_CHECK_EQ(write(fd, &c, 1), 1);
	expect_poll(fd, ro, wo, POLLIN, 0);
	KQ_CHECK_EQ(write(fd, &c, 1), 1);

	fill_to(fd, g_cap);
	expect_poll(fd, ro, wo, POLLIN, 0);
	close(ro);
	close(wo);
	close(fd);
}

struct poll_thread {
	pthread_t tid;
	int fd;
	short events;
	int ret;
	short revents;
	uint64_t entered_ns, returned_ns;
};

static void *poller_main(void *arg)
{
	struct poll_thread *p = arg;
	struct pollfd pfd = { .fd = p->fd, .events = p->events };

	p->entered_ns = kq_now_ns();
	p->ret = poll(&pfd, 1, 5000);
	p->returned_ns = kq_now_ns();
	p->revents = pfd.revents;
	return NULL;
}

/* A poller sleeping on an empty queue is woken by a write, and vice versa. */
static void t_poll_wakeups(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	struct poll_thread p = { .fd = fd, .events = POLLIN };
	uint64_t acted;

	KQ_CHECK(pthread_create(&p.tid, NULL, poller_main, &p) == 0);
	kq_sleep_ms(100);
	acted = kq_now_ns();
	KQ_CHECK_EQ(write(fd, "z", 1), 1);
	KQ_CHECK(pthread_join(p.tid, NULL) == 0);
	KQ_CHECK_EQ(p.ret, 1);
	KQ_CHECK(p.revents & POLLIN);
	KQ_CHECK(p.entered_ns < acted && p.returned_ns >= acted);
	KQ_CHECK(p.returned_ns - acted < 1000000000ULL);

	/* Full queue: a POLLOUT waiter wakes once KQ_ATOMIC_MAX bytes drain. */
	put_stream(fd, 0, g_cap - 1);
	p = (struct poll_thread){ .fd = fd, .events = POLLOUT };
	KQ_CHECK(pthread_create(&p.tid, NULL, poller_main, &p) == 0);
	kq_sleep_ms(100);
	acted = kq_now_ns();
	uint8_t drain[KQ_ATOMIC_MAX];
	KQ_CHECK_EQ(read(fd, drain, sizeof(drain)), sizeof(drain));
	KQ_CHECK(pthread_join(p.tid, NULL) == 0);
	KQ_CHECK_EQ(p.ret, 1);
	KQ_CHECK(p.revents & POLLOUT);
	KQ_CHECK(p.entered_ns < acted && p.returned_ns >= acted);
	close(fd);
}

static void t_not_seekable(void)
{
	int fd = fresh(O_RDWR);
	char c = 0;

	KQ_CHECK_ERRNO(lseek(fd, 0, SEEK_SET), ESPIPE);
	KQ_CHECK_ERRNO(pread(fd, &c, 1, 0), ESPIPE);
	KQ_CHECK_ERRNO(pwrite(fd, &c, 1, 0), ESPIPE);
	close(fd);
}

static void t_bad_user_pointers(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *ro_page = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
				-1, 0);
	uint8_t *none_page = mmap(NULL, pg, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	KQ_CHECK(ro_page != MAP_FAILED && none_page != MAP_FAILED);
	put_stream(fd, 0, 16);

	KQ_CHECK_ERRNO(read(fd, bad_ptr(0), 10), EFAULT);
	KQ_CHECK_ERRNO(read(fd, bad_ptr(1), 10), EFAULT);
	KQ_CHECK_ERRNO(read(fd, ro_page, 10), EFAULT);
#if defined(__x86_64__)
	/* A kernel address must be refused by access_ok(), not written to. */
	KQ_CHECK_ERRNO(read(fd, bad_ptr(0xffff888000000000ULL), 10), EFAULT);
#endif
	/* Failed reads consumed nothing. */
	KQ_CHECK_EQ(dev_stats(fd).used, 16);
	get_stream(fd, 0, 16);

	KQ_CHECK_ERRNO(write(fd, bad_ptr(0), 10), EFAULT);
	KQ_CHECK_ERRNO(write(fd, none_page, 10), EFAULT);
	KQ_CHECK_EQ(dev_stats(fd).used, 0);
	KQ_CHECK_EQ(dev_stats(fd).write_calls, 1);
	munmap(ro_page, pg);
	munmap(none_page, pg);
	close(fd);
}

/*
 * read() into a buffer whose tail runs into an inaccessible page returns the
 * bytes that did arrive and leaves the rest queued. Run once with the data
 * contiguous and once wrapped, so the fault lands in the second span.
 */
static void t_partial_read_fault(void)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *m = map_with_guard(1);
	uint8_t *dst = m + pg - 100;

	for (int wrapped = 0; wrapped < 2; wrapped++) {
		int fd = fresh(O_RDWR | O_NONBLOCK);

		if (wrapped)
			set_head(fd, g_cap - 50);
		put_stream(fd, 0, 300);
		ssize_t r = read(fd, dst, 300);
#if defined(__x86_64__)
		/* x86-64 copy_to_user() stops exactly at the faulting byte. */
		KQ_CHECK_EQ(r, 100);
#else
		KQ_CHECK(r >= 1 && r <= 100);
#endif
		check_pattern(dst, 0, (size_t)r);
		KQ_CHECK_EQ(dev_stats(fd).used, 300 - (size_t)r);
		get_stream(fd, (uint64_t)r, 300 - (size_t)r);
		close(fd);
	}
	munmap(m, 2 * pg);
}

/* An atomic write whose source faults part-way publishes nothing. */
static void t_atomic_write_fault(void)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *m = map_with_guard(1);
	uint8_t *src = m + pg - 100;

	fill_pattern(src, 0, 100);
	for (int wrapped = 0; wrapped < 2; wrapped++) {
		int fd = fresh(O_RDWR | O_NONBLOCK);
		char out[8];

		if (wrapped)
			set_head(fd, g_cap - 50);
		uint64_t calls = dev_stats(fd).write_calls;
		KQ_CHECK_ERRNO(write(fd, src, 300), EFAULT);
		KQ_CHECK_EQ(dev_stats(fd).used, 0);
		KQ_CHECK_EQ(dev_stats(fd).write_calls, calls);
		/* Nothing torn was published: the next data read back is ours. */
		KQ_CHECK_EQ(write(fd, "ok", 2), 2);
		KQ_CHECK_EQ(read(fd, out, sizeof(out)), 2);
		KQ_CHECK(memcmp(out, "ok", 2) == 0);
		close(fd);
	}
	munmap(m, 2 * pg);
}

/* A non-atomic write keeps the prefix that was copied before the fault. */
static void t_large_write_partial_fault(void)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *m = map_with_guard(2);
	const size_t valid = 5000;
	uint8_t *src = m + 2 * pg - valid;
	int fd = fresh(O_RDWR | O_NONBLOCK);
	size_t expect = valid < g_cap ? valid : g_cap;

	KQ_CHECK(2 * pg >= valid);
	fill_pattern(src, 0, valid);
	ssize_t r = write(fd, src, 6000);
#if defined(__x86_64__)
	KQ_CHECK_EQ(r, expect);
#else
	KQ_CHECK(r >= 1 && (size_t)r <= expect);
#endif
	KQ_CHECK_EQ(dev_stats(fd).used, r);
	get_stream(fd, 0, (size_t)r);
	munmap(m, 3 * pg);
	close(fd);
}

static void t_ioctl_invalid(void)
{
	int fd = fresh(O_RDWR);
	int ro = dev_open(O_RDONLY);
	int wo = dev_open(O_WRONLY);
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	struct kq_stats st;
	uint8_t *m = map_with_guard(1);
	uint8_t *ro_page = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
				-1, 0);

	KQ_CHECK(ro_page != MAP_FAILED);
	KQ_CHECK_ERRNO(ioctl(fd, _IO(KQ_IOC_MAGIC, 0x7f)), ENOTTY);
	KQ_CHECK_ERRNO(ioctl(fd, _IO('x', 0x01)), ENOTTY);
	/* Same number, wrong direction or size encoding: a different command. */
	KQ_CHECK_ERRNO(ioctl(fd, _IOW(KQ_IOC_MAGIC, 0x01, struct kq_stats), &st),
		       ENOTTY);
	KQ_CHECK_ERRNO(ioctl(fd, _IOR(KQ_IOC_MAGIC, 0x01, uint32_t), &st),
		       ENOTTY);
	/* isatty() issues TCGETS and relies on ENOTTY for non-terminals. */
	errno = 0;
	KQ_CHECK(isatty(fd) == 0 && errno == ENOTTY);

	KQ_CHECK_ERRNO(ioctl(fd, KQ_IOC_GET_STATS, bad_ptr(0)), EFAULT);
	KQ_CHECK_ERRNO(ioctl(fd, KQ_IOC_GET_STATS, bad_ptr(1)), EFAULT);
	KQ_CHECK_ERRNO(ioctl(fd, KQ_IOC_GET_STATS, ro_page), EFAULT);
	/* A struct that straddles into the guard page. */
	KQ_CHECK_ERRNO(ioctl(fd, KQ_IOC_GET_STATS, m + pg - 8), EFAULT);

	/* Mutating commands need a writable fd; querying does not. */
	KQ_CHECK_ERRNO(ioctl(ro, KQ_IOC_RESET), EBADF);
	KQ_CHECK_ERRNO(ioctl(ro, KQ_IOC_CLEAR_STATS), EBADF);
	KQ_CHECK_EQ(ioctl(ro, KQ_IOC_GET_STATS, &st), 0);
	KQ_CHECK_EQ(ioctl(wo, KQ_IOC_RESET), 0);
	KQ_CHECK_EQ(ioctl(wo, KQ_IOC_GET_STATS, &st), 0);
	KQ_CHECK_EQ(st.resets, 1);

	munmap(m, 2 * pg);
	munmap(ro_page, pg);
	close(ro);
	close(wo);
	close(fd);
}

static void t_reset_and_clear_stats(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	struct kq_stats st;
	char c;

	put_stream(fd, 0, 100);
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_RESET), 0);
	st = dev_stats(fd);
	KQ_CHECK_EQ(st.used, 0);
	KQ_CHECK_EQ(st.resets, 1);
	KQ_CHECK_EQ(st.bytes_written, 100); /* RESET drops data, not counters */
	KQ_CHECK_ERRNO(read(fd, &c, 1), EAGAIN);

	put_stream(fd, 0, 7);
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_CLEAR_STATS), 0);
	st = dev_stats(fd);
	/* CLEAR_STATS zeroes counters but not the state fields. */
	KQ_CHECK_EQ(st.bytes_written, 0);
	KQ_CHECK_EQ(st.read_eagain, 0);
	KQ_CHECK_EQ(st.resets, 0);
	KQ_CHECK_EQ(st.used, 7);
	KQ_CHECK_EQ(st.capacity, g_cap);
	KQ_CHECK_EQ(st.open_files, 1);
	close(fd);
}

static void t_reset_wakes_blocked_writer(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 10 };

	put_stream(fd, 0, g_cap);
	memset(t.buf, 'r', sizeof(t.buf));
	start_thread(&t, writer_main);
	WAIT_STAT_GE(fd, write_waits, 1);
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_RESET), 0);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, 10);
	KQ_CHECK_EQ(dev_stats(fd).used, 10);
	close(fd);
}

/*
 * The next three tests target exclusive wake-ups (exclusive_wake=Y), where
 * an event wakes one sleeper. Each would hang, and so TIMEOUT, if a wake-up
 * were swallowed by a task that cannot use it or never handed on. With
 * exclusive_wake=N they pass trivially, since everybody is woken.
 */

/* A large atomic writer must not take the wake-up a small writer can use. */
static void t_wake_skips_unfit_writer(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread big = { .fd = fd, .len = 4000 };
	struct io_thread small = { .fd = fd, .len = 10 };

	put_stream(fd, 0, g_cap);
	start_thread(&big, writer_main);
	WAIT_STAT_GE(fd, write_waits, 1);	/* big is first in the queue */
	start_thread(&small, writer_main);
	WAIT_STAT_GE(fd, write_waits, 2);

	drain(fd, 100);				/* room for small, not for big */
	join_within(&small, 2000);
	KQ_CHECK_EQ(small.ret, 10);
	kq_sleep_ms(50);
	KQ_CHECK(!atomic_load(&big.done));

	drain(fd, 4000);
	join_within(&big, 2000);
	KQ_CHECK_EQ(big.ret, 4000);
	close(fd);
}

/* One write with data for two sleeping readers must reach both. */
static void t_wake_hands_on_to_next_reader(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread a = { .fd = fd, .len = 1 };
	struct io_thread b = { .fd = fd, .len = 1 };

	start_thread(&a, reader_main);
	WAIT_STAT_GE(fd, read_waits, 1);
	start_thread(&b, reader_main);
	WAIT_STAT_GE(fd, read_waits, 2);
	KQ_CHECK_EQ(write(fd, "xy", 2), 2);	/* a single wake-up event */
	join_within(&a, 2000);
	join_within(&b, 2000);
	KQ_CHECK_EQ(a.ret, 1);
	KQ_CHECK_EQ(b.ret, 1);
	KQ_CHECK_EQ(dev_stats(fd).used, 0);
	close(fd);
}

/* One read that frees room for two sleeping writers must release both. */
static void t_wake_hands_on_to_next_writer(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread a = { .fd = fd, .len = 100 };
	struct io_thread b = { .fd = fd, .len = 100 };

	put_stream(fd, 0, g_cap);
	start_thread(&a, writer_main);
	WAIT_STAT_GE(fd, write_waits, 1);
	start_thread(&b, writer_main);
	WAIT_STAT_GE(fd, write_waits, 2);
	drain(fd, 300);				/* a single read() */
	join_within(&a, 2000);
	join_within(&b, 2000);
	KQ_CHECK_EQ(a.ret, 100);
	KQ_CHECK_EQ(b.ret, 100);
	KQ_CHECK_EQ(dev_stats(fd).used, g_cap - 100);
	close(fd);
}

/* All file descriptions share one queue, and data outlives close(). */
static void t_shared_and_persistent(void)
{
	int a = fresh(O_RDWR), b = dev_open(O_RDWR);
	char buf[16];

	KQ_CHECK_EQ(write(a, "abc", 3), 3);
	KQ_CHECK_EQ(read(b, buf, sizeof(buf)), 3);
	KQ_CHECK(memcmp(buf, "abc", 3) == 0);

	KQ_CHECK_EQ(write(a, "persist", 7), 7);
	close(a);
	close(b);
	int c = dev_open(O_RDONLY);
	KQ_CHECK_EQ(read(c, buf, sizeof(buf)), 7);
	KQ_CHECK(memcmp(buf, "persist", 7) == 0);
	close(c);
}

/* RESET discards data; it must neither wake a blocked reader nor feed it stale bytes. */
static void t_reset_keeps_reader_blocked(void)
{
	int fd = fresh(O_RDWR);
	struct io_thread t = { .fd = fd, .len = 64 };

	start_thread(&t, reader_main);
	WAIT_STAT_GE(fd, read_waits, 1);
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_RESET), 0);
	kq_sleep_ms(50);
	KQ_CHECK(!atomic_load(&t.done));
	KQ_CHECK_EQ(write(fd, "n", 1), 1);
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, 1);
	KQ_CHECK_EQ(t.buf[0], 'n');
	close(fd);
}

/* RESET frees the whole ring, so a poll() waiting for POLLOUT must wake. */
static void t_reset_wakes_poll_writer(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	struct poll_thread p = { .fd = fd, .events = POLLOUT };
	uint64_t acted;

	fill_to(fd, g_cap);
	KQ_CHECK(pthread_create(&p.tid, NULL, poller_main, &p) == 0);
	kq_sleep_ms(100);
	acted = kq_now_ns();
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_RESET), 0);
	KQ_CHECK(pthread_join(p.tid, NULL) == 0);
	KQ_CHECK_EQ(p.ret, 1);
	KQ_CHECK(p.revents & POLLOUT);
	KQ_CHECK(p.entered_ns < acted && p.returned_ns >= acted);
	close(fd);
}

/*
 * The POLLOUT transition: a waiter must stay in poll() while reads leave fewer
 * than KQ_ATOMIC_MAX bytes free, and return once that threshold is reached.
 */
static void t_poll_out_threshold(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	struct poll_thread p = { .fd = fd, .events = POLLOUT };
	uint64_t acted;

	fill_to(fd, g_cap);
	KQ_CHECK(pthread_create(&p.tid, NULL, poller_main, &p) == 0);
	kq_sleep_ms(100);
	drain(fd, KQ_ATOMIC_MAX - 1);		/* KQ_ATOMIC_MAX - 1 bytes free */
	kq_sleep_ms(100);
	KQ_CHECK_EQ(pthread_tryjoin_np(p.tid, NULL), EBUSY);	/* still in poll() */
	acted = kq_now_ns();
	drain(fd, 1);				/* exactly KQ_ATOMIC_MAX free */
	KQ_CHECK(pthread_join(p.tid, NULL) == 0);
	KQ_CHECK_EQ(p.ret, 1);
	KQ_CHECK(p.revents & POLLOUT);
	KQ_CHECK(p.returned_ns >= acted);
	close(fd);
}

/*
 * A process killed while blocked must be reaped promptly (the sleep is
 * interruptible, not D state), release() must run for its file, and the
 * queue must still serve the next blocked caller.
 */
static void killed_while_blocked(bool in_write)
{
	int fd = fresh(O_RDWR);
	uint64_t deadline;
	int status = 0;
	pid_t pid;

	if (in_write)
		fill_to(fd, g_cap);
	pid = fork();
	KQ_CHECK(pid >= 0);
	if (pid == 0) {
		char buf[16] = { 0 };
		int cfd;

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		cfd = open(kq_device_path(), O_RDWR);
		if (cfd >= 0 && in_write)
			(void)!write(cfd, buf, sizeof(buf));
		else if (cfd >= 0)
			(void)!read(cfd, buf, sizeof(buf));
		_exit(0);	/* only reached if the call returned */
	}
	if (in_write)
		WAIT_STAT_GE(fd, write_waits, 1);
	else
		WAIT_STAT_GE(fd, read_waits, 1);
	KQ_CHECK_EQ(dev_stats(fd).open_files, 2);

	KQ_CHECK(kill(pid, SIGKILL) == 0);
	deadline = kq_now_ns() + 2000000000ULL;
	while (waitpid(pid, &status, WNOHANG) != pid) {
		if (kq_now_ns() > deadline)
			KQ_FAIL("child blocked in %s was not reaped within 2 s after SIGKILL",
				in_write ? "write" : "read");
		kq_sleep_ms(1);
	}
	KQ_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	KQ_CHECK_EQ(dev_stats(fd).open_files, 1);

	struct io_thread t = { .fd = fd, .len = in_write ? 10 : 1 };

	if (in_write) {
		memset(t.buf, 'w', 10);
		start_thread(&t, writer_main);
		WAIT_STAT_GE(fd, write_waits, 2);
		drain(fd, 100);
	} else {
		start_thread(&t, reader_main);
		WAIT_STAT_GE(fd, read_waits, 2);
		KQ_CHECK_EQ(write(fd, "k", 1), 1);
	}
	join_within(&t, 2000);
	KQ_CHECK_EQ(t.ret, in_write ? 10 : 1);
	close(fd);
}

static void t_killed_blocked_reader(void)
{
	killed_while_blocked(false);
}

static void t_killed_blocked_writer(void)
{
	killed_while_blocked(true);
}

/*
 * RESET racing with active I/O: 4 writers and 4 readers move 64-byte records
 * (non-blocking) while another thread resets every millisecond for 1 s.
 * Resets discard whole records, so delivery is not checked, but every record
 * read must be intact and every read exactly one record long.
 */
#define LOAD_REC 64

struct load_ctx {
	int fd;
	atomic_bool stop;
	atomic_ulong records, corrupt, bad_len, errors;
};

static void *load_writer(void *arg)
{
	struct load_ctx *c = arg;
	uint8_t rec[LOAD_REC];
	uint64_t seq = 0;

	while (!atomic_load(&c->stop)) {
		kq_rec_build(rec, LOAD_REC, 0, seq, 0);
		ssize_t r = write(c->fd, rec, LOAD_REC);

		if (r == LOAD_REC)
			seq++;
		else if (r == -1 && errno == EAGAIN)
			sched_yield();
		else
			atomic_fetch_add(&c->errors, 1);
	}
	return NULL;
}

static void *load_reader(void *arg)
{
	struct load_ctx *c = arg;
	uint8_t rec[LOAD_REC];

	while (!atomic_load(&c->stop)) {
		ssize_t r = read(c->fd, rec, LOAD_REC);

		if (r == LOAD_REC) {
			atomic_fetch_add(&c->records, 1);
			if (kq_rec_verify(rec, LOAD_REC))
				atomic_fetch_add(&c->corrupt, 1);
		} else if (r == -1 && errno == EAGAIN) {
			sched_yield();
		} else if (r >= 0) {
			atomic_fetch_add(&c->bad_len, 1);
		} else {
			atomic_fetch_add(&c->errors, 1);
		}
	}
	return NULL;
}

static void *load_resetter(void *arg)
{
	struct load_ctx *c = arg;

	while (!atomic_load(&c->stop)) {
		if (ioctl(c->fd, KQ_IOC_RESET))
			atomic_fetch_add(&c->errors, 1);
		kq_sleep_ms(1);
	}
	return NULL;
}

static void t_reset_under_load(void)
{
	int fd = fresh(O_RDWR | O_NONBLOCK);
	struct load_ctx c = { .fd = fd };
	pthread_t th[9];
	struct kq_stats st;
	int i;

	for (i = 0; i < 4; i++)
		KQ_CHECK(pthread_create(&th[i], NULL, load_writer, &c) == 0);
	for (i = 4; i < 8; i++)
		KQ_CHECK(pthread_create(&th[i], NULL, load_reader, &c) == 0);
	KQ_CHECK(pthread_create(&th[8], NULL, load_resetter, &c) == 0);
	kq_sleep_ms(1000);
	atomic_store(&c.stop, true);
	for (i = 0; i < 9; i++)
		KQ_CHECK(pthread_join(th[i], NULL) == 0);

	st = dev_stats(fd);
	KQ_CHECK_EQ(atomic_load(&c.corrupt), 0);
	KQ_CHECK_EQ(atomic_load(&c.bad_len), 0);
	KQ_CHECK_EQ(atomic_load(&c.errors), 0);
	KQ_CHECK(atomic_load(&c.records) > 0);
	KQ_CHECK(st.resets >= 100);
	KQ_CHECK(st.used % LOAD_REC == 0 && st.used <= st.capacity);
	KQ_CHECK(st.bytes_read <= st.bytes_written);
	KQ_CHECK_EQ(ioctl(fd, KQ_IOC_RESET), 0);
	KQ_CHECK_EQ(dev_stats(fd).used, 0);
	close(fd);
}

int main(int argc, char **argv)
{
	static const struct kq_test_case tests[] = {
		{ "open_modes", t_open_modes, 0 },
		{ "capacity_matches_param", t_capacity_matches_param, 0 },
		{ "roundtrip_and_counters", t_roundtrip_and_counters, 0 },
		{ "zero_length", t_zero_length, 0 },
		{ "nonblock_empty_read", t_nonblock_empty_read, 0 },
		{ "nonblock_toggle", t_nonblock_toggle, 0 },
		{ "fill_to_capacity", t_fill_to_capacity, 0 },
		{ "atomic_write_semantics", t_atomic_write_semantics, 0 },
		{ "wraparound_stream", t_wraparound_stream, 60000 },
		{ "blocking_read_wakes", t_blocking_read_wakes, 0 },
		{ "blocking_atomic_write_waits", t_blocking_atomic_write_waits, 0 },
		{ "signal_interrupts_read", t_signal_interrupts_read, 0 },
		{ "signal_restarts_read", t_signal_restarts_read, 0 },
		{ "poll_states", t_poll_states, 0 },
		{ "poll_wakeups", t_poll_wakeups, 0 },
		{ "not_seekable", t_not_seekable, 0 },
		{ "bad_user_pointers", t_bad_user_pointers, 0 },
		{ "partial_read_fault", t_partial_read_fault, 0 },
		{ "atomic_write_fault", t_atomic_write_fault, 0 },
		{ "large_write_partial_fault", t_large_write_partial_fault, 0 },
		{ "ioctl_invalid", t_ioctl_invalid, 0 },
		{ "reset_and_clear_stats", t_reset_and_clear_stats, 0 },
		{ "reset_wakes_blocked_writer", t_reset_wakes_blocked_writer, 0 },
		{ "wake_skips_unfit_writer", t_wake_skips_unfit_writer, 0 },
		{ "wake_hands_on_to_next_reader", t_wake_hands_on_to_next_reader, 0 },
		{ "wake_hands_on_to_next_writer", t_wake_hands_on_to_next_writer, 0 },
		{ "shared_and_persistent", t_shared_and_persistent, 0 },
		{ "reset_keeps_reader_blocked", t_reset_keeps_reader_blocked, 0 },
		{ "reset_wakes_poll_writer", t_reset_wakes_poll_writer, 0 },
		{ "poll_out_threshold", t_poll_out_threshold, 0 },
		{ "killed_blocked_reader", t_killed_blocked_reader, 0 },
		{ "killed_blocked_writer", t_killed_blocked_writer, 0 },
		{ "reset_under_load", t_reset_under_load, 20000 },
	};
	FILE *f = fopen("/sys/module/kernqueue/parameters/exclusive_wake", "r");
	int mode = f ? fgetc(f) : '?';

	if (f)
		fclose(f);
	printf("test_functional: device %s, exclusive_wake=%c\n",
	       kq_device_path(), mode);
	return kq_test_main(tests, sizeof(tests) / sizeof(tests[0]), argc, argv);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * test_stress.c - multi-producer/multi-consumer integrity test.
 *
 * W writer threads each send R fixed-size records (see struct kq_rec_hdr);
 * M reader threads receive them. Checked:
 *   - every read() returns exactly one record (atomic writes keep records
 *     whole, so a short read means atomicity was broken);
 *   - every record's magic, length, checksum and payload bytes;
 *   - per reader, each writer's sequence numbers strictly increase (FIFO);
 *     with a single reader they must also be consecutive (no gaps);
 *   - globally, each (writer, seq) arrives exactly once: an atomic bitmap per
 *     writer catches duplicates, and a final sweep catches losses;
 *   - on a kernqueue device, the driver's byte and call counters agree.
 *
 * Shutdown: after all writers finish, the main thread queues one "poison"
 * record per reader. Because the queue is FIFO, every real record is dequeued
 * before any poison record, and each reader exits after taking one.
 *
 * A watchdog thread fails the run (and prints per-thread progress) if it does
 * not finish within the timeout, so a lost wake-up shows up as a diagnosable
 * failure instead of a hang. The same harness can be pointed at a named pipe
 * (--device /path/to/fifo) to validate the harness itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kq_common.h"

#define MAX_THREADS 256
#define NO_SEQ UINT64_MAX

/*
 * Deliberate faults for the harness self-test (scripts/test_harness_fifo.sh):
 * writer 0 misbehaves once, and the checker must notice.
 */
enum inject { INJ_NONE, INJ_DUP, INJ_SKIP, INJ_CORRUPT, INJ_SPLIT };

static struct {
	const char *path;
	unsigned int writers, readers;
	uint64_t records;      /* per writer (upper bound in duration mode) */
	uint32_t size;         /* bytes per record */
	double duration;       /* seconds; 0 = send exactly `records` */
	unsigned int timeout;  /* seconds */
	bool nonblock;
	bool quiet;
	enum inject inject;
} cfg = {
	.writers = 4,
	.readers = 4,
	.records = 100000,
	.size = 64,
	.timeout = 120,
};

struct writer {
	pthread_t tid;
	uint32_t id;
	_Atomic uint64_t sent;
};

struct reader {
	pthread_t tid;
	uint32_t id;
	_Atomic uint64_t got;
	uint64_t *last_seq; /* per writer, NO_SEQ until the first record */
};

static struct writer writers[MAX_THREADS];
static struct reader readers[MAX_THREADS];
static _Atomic uint64_t *seen[MAX_THREADS]; /* per-writer bitmaps */
static atomic_bool stop_writers;
static atomic_bool finished;

static void __attribute__((noreturn, format(printf, 1, 2)))
fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "test_stress: FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	_exit(1);
}

static int open_queue(void)
{
	/* O_RDWR also keeps a FIFO's open() from blocking on the other side. */
	int fd = open(cfg.path, O_RDWR | O_CLOEXEC | (cfg.nonblock ? O_NONBLOCK : 0));

	if (fd < 0)
		fail("open(%s): %s", cfg.path, strerror(errno));
	return fd;
}

static void wait_ready(int fd, short events)
{
	struct pollfd p = { .fd = fd, .events = events };

	if (poll(&p, 1, 1000) < 0 && errno != EINTR)
		fail("poll: %s", strerror(errno));
}

static void put_record(int fd, const uint8_t *rec)
{
	for (;;) {
		ssize_t r = write(fd, rec, cfg.size);

		if (r == (ssize_t)cfg.size)
			return;
		if (r >= 0)
			fail("short write: %zd of %u bytes (atomic write split)",
			     r, cfg.size);
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN && cfg.nonblock) {
			wait_ready(fd, POLLOUT);
			continue;
		}
		fail("write: %s", strerror(errno));
	}
}

static void get_record(int fd, uint8_t *rec)
{
	for (;;) {
		ssize_t r = read(fd, rec, cfg.size);

		if (r == (ssize_t)cfg.size)
			return;
		if (r >= 0)
			fail("short read: %zd of %u bytes (record split)", r,
			     cfg.size);
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN && cfg.nonblock) {
			wait_ready(fd, POLLIN);
			continue;
		}
		fail("read: %s", strerror(errno));
	}
}

/* INJ_SPLIT: send a record as two writes, so other writers can interleave. */
static void put_split(int fd, const uint8_t *rec)
{
	size_t half = cfg.size / 2, off = 0;

	while (off < cfg.size) {
		size_t len = off < half ? half - off : cfg.size - off;
		ssize_t r = write(fd, rec + off, len);

		if (r < 0 && errno != EINTR && errno != EAGAIN)
			fail("write: %s", strerror(errno));
		if (r > 0)
			off += (size_t)r;
	}
}

static void *writer_main(void *arg)
{
	struct writer *w = arg;
	uint8_t rec[KQ_ATOMIC_MAX];
	int fd = open_queue();
	uint64_t fault_seq = cfg.records > 10 ? 10 : cfg.records - 1;
	uint64_t seq;

	for (seq = 0; seq < cfg.records; seq++) {
		if (atomic_load_explicit(&stop_writers, memory_order_relaxed))
			break;
		kq_rec_build(rec, cfg.size, w->id, seq, 0);
		if (cfg.inject != INJ_NONE && w->id == 0 && seq == fault_seq) {
			if (cfg.inject == INJ_SKIP) {
				/* Claim it was sent without sending it. */
				atomic_store(&w->sent, seq + 1);
				continue;
			}
			if (cfg.inject == INJ_CORRUPT)
				rec[cfg.size - 1] ^= 0x5a;
			if (cfg.inject == INJ_DUP)
				put_record(fd, rec);
		}
		if (cfg.inject == INJ_SPLIT)
			put_split(fd, rec);
		else
			put_record(fd, rec);
		atomic_store_explicit(&w->sent, seq + 1, memory_order_relaxed);
	}
	close(fd);
	return NULL;
}

static void *reader_main(void *arg)
{
	struct reader *r = arg;
	uint8_t rec[KQ_ATOMIC_MAX];
	int fd = open_queue();

	for (;;) {
		struct kq_rec_hdr h;
		const char *err;

		get_record(fd, rec);
		err = kq_rec_verify(rec, cfg.size);
		if (err)
			fail("reader %u: corrupt record: %s", r->id, err);
		memcpy(&h, rec, sizeof(h));
		if (h.writer == KQ_REC_POISON)
			break;
		if (h.writer >= cfg.writers || h.seq >= cfg.records)
			fail("reader %u: impossible record writer=%u seq=%llu",
			     r->id, h.writer, (unsigned long long)h.seq);

		uint64_t last = r->last_seq[h.writer];
		if (last != NO_SEQ && h.seq <= last)
			fail("reader %u: FIFO order broken for writer %u: seq %llu after %llu",
			     r->id, h.writer, (unsigned long long)h.seq,
			     (unsigned long long)last);
		if (cfg.readers == 1 && h.seq != (last == NO_SEQ ? 0 : last + 1))
			fail("gap for writer %u: expected seq %llu, got %llu",
			     h.writer,
			     (unsigned long long)(last == NO_SEQ ? 0 : last + 1),
			     (unsigned long long)h.seq);
		r->last_seq[h.writer] = h.seq;

		uint64_t bit = 1ULL << (h.seq % 64);
		if (atomic_fetch_or(&seen[h.writer][h.seq / 64], bit) & bit)
			fail("duplicate record writer=%u seq=%llu", h.writer,
			     (unsigned long long)h.seq);
		atomic_store_explicit(&r->got, atomic_load_explicit(&r->got,
				      memory_order_relaxed) + 1,
				      memory_order_relaxed);
	}
	close(fd);
	return NULL;
}

static void print_progress(FILE *out)
{
	for (unsigned int i = 0; i < cfg.writers; i++)
		fprintf(out, "  writer %u: sent %llu\n", i,
			(unsigned long long)atomic_load(&writers[i].sent));
	for (unsigned int i = 0; i < cfg.readers; i++)
		fprintf(out, "  reader %u: got %llu\n", i,
			(unsigned long long)atomic_load(&readers[i].got));
}

static void *watchdog_main(void *arg)
{
	uint64_t deadline = kq_now_ns() + (uint64_t)cfg.timeout * 1000000000ULL;
	struct kq_stats st;
	int fd = *(int *)arg;

	while (kq_now_ns() < deadline) {
		if (atomic_load(&finished))
			return NULL;
		kq_sleep_ms(50);
	}
	fprintf(stderr, "test_stress: FAIL: TIMEOUT after %u s; progress:\n",
		cfg.timeout);
	print_progress(stderr);
	if (kq_get_stats(fd, &st) == 0)
		fprintf(stderr, "  device: used=%llu read_waits=%llu write_waits=%llu\n",
			(unsigned long long)st.used,
			(unsigned long long)st.read_waits,
			(unsigned long long)st.write_waits);
	_exit(3);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -d, --device PATH    queue to test (default $KQ_DEVICE or %s)\n"
		"  -w, --writers N      writer threads (default 4)\n"
		"  -r, --readers N      reader threads (default 4)\n"
		"  -n, --records N      records per writer (default 100000)\n"
		"  -s, --size BYTES     record size, %u..%u (default 64)\n"
		"  -t, --duration SEC   send until SEC elapse instead (-n is the cap)\n"
		"  -T, --timeout SEC    fail if not done in SEC (default 120)\n"
		"  -N, --nonblock       O_NONBLOCK + poll() instead of blocking I/O\n"
		"  -q, --quiet          only print the result line\n"
		"      --inject FAULT   harness self-test: writer 0 misbehaves once\n"
		"                       (dup|skip|corrupt|split); the run must FAIL\n",
		prog, KQ_DEVICE_PATH, KQ_REC_MIN, KQ_ATOMIC_MAX);
}

static uint64_t parse_or_die(const char *s, const char *what, uint64_t lo,
			     uint64_t hi)
{
	uint64_t v;

	if (!kq_parse_size(s, &v) || v < lo || v > hi) {
		fprintf(stderr, "invalid %s '%s' (allowed %llu..%llu)\n", what, s,
			(unsigned long long)lo, (unsigned long long)hi);
		exit(2);
	}
	return v;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "writers", required_argument, NULL, 'w' },
		{ "readers", required_argument, NULL, 'r' },
		{ "records", required_argument, NULL, 'n' },
		{ "size", required_argument, NULL, 's' },
		{ "duration", required_argument, NULL, 't' },
		{ "timeout", required_argument, NULL, 'T' },
		{ "nonblock", no_argument, NULL, 'N' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "inject", required_argument, NULL, 'I' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static const char *const inject_names[] = {
		[INJ_DUP] = "dup", [INJ_SKIP] = "skip",
		[INJ_CORRUPT] = "corrupt", [INJ_SPLIT] = "split",
	};
	bool records_given = false;
	struct kq_stats before, after;
	pthread_t watchdog;
	bool is_kq;
	int opt, ctl;

	cfg.path = kq_device_path();
	while ((opt = getopt_long(argc, argv, "d:w:r:n:s:t:T:Nqh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': cfg.path = optarg; break;
		case 'w': cfg.writers = (unsigned int)parse_or_die(optarg, "writers", 1, MAX_THREADS); break;
		case 'r': cfg.readers = (unsigned int)parse_or_die(optarg, "readers", 1, MAX_THREADS); break;
		case 'n': cfg.records = parse_or_die(optarg, "records", 1, 1ULL << 32); records_given = true; break;
		case 's': cfg.size = (uint32_t)parse_or_die(optarg, "size", KQ_REC_MIN, KQ_ATOMIC_MAX); break;
		case 't': cfg.duration = (double)parse_or_die(optarg, "duration", 1, 86400); break;
		case 'T': cfg.timeout = (unsigned int)parse_or_die(optarg, "timeout", 1, 86400); break;
		case 'N': cfg.nonblock = true; break;
		case 'q': cfg.quiet = true; break;
		case 'I':
			for (int i = INJ_DUP; i <= INJ_SPLIT; i++)
				if (strcmp(optarg, inject_names[i]) == 0)
					cfg.inject = (enum inject)i;
			if (cfg.inject == INJ_NONE) {
				fprintf(stderr, "unknown --inject '%s'\n", optarg);
				return 2;
			}
			break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (optind != argc) {
		usage(argv[0]);
		return 2;
	}
	/* Duration mode: -n bounds the per-writer bitmap; default 16M records. */
	if (cfg.duration > 0 && !records_given)
		cfg.records = 16ULL << 20;

	ctl = open(cfg.path, O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		fail("open(%s): %s", cfg.path, strerror(errno));
	is_kq = kq_get_stats(ctl, &before) == 0;
	if (is_kq) {
		if (ioctl(ctl, KQ_IOC_RESET) || ioctl(ctl, KQ_IOC_CLEAR_STATS) ||
		    kq_get_stats(ctl, &before))
			fail("reset ioctls: %s", strerror(errno));
	}

	if (!cfg.quiet)
		printf("test_stress: %s (%s), %u writers x %llu records%s, %u readers, "
		       "%u-byte records, %s I/O\n",
		       cfg.path, is_kq ? "kernqueue" : "not kernqueue: no stats checks",
		       cfg.writers, (unsigned long long)cfg.records,
		       cfg.duration > 0 ? " max" : "", cfg.readers, cfg.size,
		       cfg.nonblock ? "non-blocking+poll" : "blocking");

	for (unsigned int i = 0; i < cfg.writers; i++) {
		seen[i] = calloc((size_t)(cfg.records + 63) / 64, sizeof(uint64_t));
		if (!seen[i])
			fail("out of memory for bitmaps");
	}
	if (pthread_create(&watchdog, NULL, watchdog_main, &ctl))
		fail("pthread_create(watchdog)");

	uint64_t t0 = kq_now_ns();
	for (unsigned int i = 0; i < cfg.readers; i++) {
		readers[i].id = i;
		readers[i].last_seq = malloc(cfg.writers * sizeof(uint64_t));
		if (!readers[i].last_seq)
			fail("out of memory");
		for (unsigned int j = 0; j < cfg.writers; j++)
			readers[i].last_seq[j] = NO_SEQ;
		if (pthread_create(&readers[i].tid, NULL, reader_main, &readers[i]))
			fail("pthread_create(reader)");
	}
	for (unsigned int i = 0; i < cfg.writers; i++) {
		writers[i].id = i;
		if (pthread_create(&writers[i].tid, NULL, writer_main, &writers[i]))
			fail("pthread_create(writer)");
	}
	if (cfg.duration > 0) {
		kq_sleep_ms((unsigned int)(cfg.duration * 1000));
		atomic_store(&stop_writers, true);
	}

	uint64_t total_sent = 0;
	for (unsigned int i = 0; i < cfg.writers; i++) {
		pthread_join(writers[i].tid, NULL);
		total_sent += atomic_load(&writers[i].sent);
	}

	/* Poison records go through the same queue, behind all real records. */
	{
		uint8_t rec[KQ_ATOMIC_MAX];
		int pfd = open_queue();

		for (unsigned int i = 0; i < cfg.readers; i++) {
			kq_rec_build(rec, cfg.size, KQ_REC_POISON, i, 0);
			put_record(pfd, rec);
		}
		close(pfd);
	}

	uint64_t total_got = 0;
	for (unsigned int i = 0; i < cfg.readers; i++) {
		pthread_join(readers[i].tid, NULL);
		total_got += atomic_load(&readers[i].got);
	}
	double secs = (double)(kq_now_ns() - t0) / 1e9;
	atomic_store(&finished, true);
	pthread_join(watchdog, NULL);

	/* Every record sent must have been received exactly once. */
	if (total_got != total_sent)
		fail("received %llu records but %llu were sent",
		     (unsigned long long)total_got, (unsigned long long)total_sent);
	for (unsigned int w = 0; w < cfg.writers; w++) {
		uint64_t sent = atomic_load(&writers[w].sent);

		for (uint64_t s = 0; s < sent; s++)
			if (!(atomic_load_explicit(&seen[w][s / 64],
						   memory_order_relaxed) &
			      (1ULL << (s % 64))))
				fail("lost record writer=%u seq=%llu", w,
				     (unsigned long long)s);
	}

	if (is_kq) {
		uint64_t recs = total_sent + cfg.readers; /* + poison */

		if (kq_get_stats(ctl, &after))
			fail("GET_STATS: %s", strerror(errno));
		if (after.used != 0)
			fail("queue not empty at the end: used=%llu",
			     (unsigned long long)after.used);
		if (after.bytes_written != recs * cfg.size ||
		    after.bytes_read != recs * cfg.size)
			fail("byte counters disagree: written=%llu read=%llu expected=%llu",
			     (unsigned long long)after.bytes_written,
			     (unsigned long long)after.bytes_read,
			     (unsigned long long)(recs * cfg.size));
		if (after.write_calls != recs || after.read_calls != recs)
			fail("call counters disagree: writes=%llu reads=%llu expected=%llu",
			     (unsigned long long)after.write_calls,
			     (unsigned long long)after.read_calls,
			     (unsigned long long)recs);
	}

	printf("test_stress: PASS %llu records (%.1f MiB) in %.2f s, %ux%u, %u B, %s",
	       (unsigned long long)total_sent,
	       (double)total_sent * cfg.size / (1024.0 * 1024.0), secs,
	       cfg.writers, cfg.readers, cfg.size,
	       cfg.nonblock ? "nonblock" : "blocking");
	if (is_kq)
		printf(", waits r/w %llu/%llu, eagain r/w %llu/%llu",
		       (unsigned long long)after.read_waits,
		       (unsigned long long)after.write_waits,
		       (unsigned long long)after.read_eagain,
		       (unsigned long long)after.write_eagain);
	printf("\n");
	if (!cfg.quiet)
		print_progress(stdout);
	return 0;
}

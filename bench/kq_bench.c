// SPDX-License-Identifier: GPL-2.0
/*
 * kq_bench.c - throughput and latency benchmark for kernqueue (or a FIFO).
 *
 * W writers send fixed-size records as fast as possible (or at --rate per
 * writer); M readers receive them. After --warmup seconds, every thread
 * counts operations for --duration seconds and records:
 *   - syscall latency: time spent inside each write()/read() call;
 *   - end-to-end latency: reader's receive time minus the send timestamp the
 *     writer stored in the record (same CLOCK_MONOTONIC, same machine).
 * In unthrottled runs the queue is mostly full, so end-to-end latency is
 * dominated by queueing delay (roughly capacity / throughput); use --rate to
 * measure it on a lightly loaded queue.
 *
 * Latency samples are kept per thread with reservoir sampling (uniform over
 * that thread's operations, bounded memory); the reported percentiles merge
 * all threads' reservoirs and are therefore approximate when threads perform
 * very different numbers of operations. Max is tracked exactly.
 *
 * This program measures; it does not verify data (that is test_stress's job).
 * It only checks the record magic and length to catch gross misuse.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "kq_common.h"

#define MAX_THREADS 256

enum phase { PH_WARMUP, PH_MEASURE, PH_STOP };

static struct {
	const char *path;
	const char *label;
	unsigned int writers, readers;
	uint32_t size;
	unsigned int duration, warmup;
	uint64_t rate;     /* records/s per writer, 0 = unthrottled */
	uint64_t samples;  /* reservoir size per thread per metric */
	bool nonblock, csv, header;
} cfg = {
	.label = "-",
	.writers = 1,
	.readers = 1,
	.size = 64,
	.duration = 5,
	.warmup = 1,
	.samples = 200000,
};

static atomic_int phase = PH_WARMUP;

struct reservoir {
	uint64_t *v;
	uint64_t n_kept, n_seen, max;
	uint64_t rng;
};

struct thread_res {
	pthread_t tid;
	uint32_t id;
	uint64_t ops, bytes;
	struct reservoir sys, e2e;
};

static struct thread_res wres[MAX_THREADS], rres[MAX_THREADS];

static void __attribute__((noreturn)) die(const char *what)
{
	fprintf(stderr, "kq_bench: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void res_init(struct reservoir *r, uint64_t seed)
{
	r->v = malloc(cfg.samples * sizeof(uint64_t));
	if (!r->v)
		die("malloc");
	r->n_kept = r->n_seen = r->max = 0;
	r->rng = seed | 1;
}

/* Algorithm R: keep each of the n_seen values with probability cap/n_seen. */
static void res_add(struct reservoir *r, uint64_t x)
{
	r->n_seen++;
	if (x > r->max)
		r->max = x;
	if (r->n_kept < cfg.samples) {
		r->v[r->n_kept++] = x;
		return;
	}
	r->rng ^= r->rng << 13;
	r->rng ^= r->rng >> 7;
	r->rng ^= r->rng << 17;
	uint64_t j = r->rng % r->n_seen;
	if (j < cfg.samples)
		r->v[j] = x;
}

static int open_queue(void)
{
	int fd = open(cfg.path, O_RDWR | O_CLOEXEC | (cfg.nonblock ? O_NONBLOCK : 0));

	if (fd < 0)
		die(cfg.path);
	return fd;
}

/* One record transfer, retrying on EINTR and (non-blocking mode) EAGAIN. */
static void xfer(int fd, uint8_t *rec, bool is_write)
{
	for (;;) {
		ssize_t r = is_write ? write(fd, rec, cfg.size)
				     : read(fd, rec, cfg.size);

		if (r == (ssize_t)cfg.size)
			return;
		if (r >= 0) {
			fprintf(stderr, "kq_bench: short %s (%zd of %u)\n",
				is_write ? "write" : "read", r, cfg.size);
			exit(1);
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN && cfg.nonblock) {
			struct pollfd p = { .fd = fd,
					    .events = is_write ? POLLOUT : POLLIN };
			poll(&p, 1, 100);
			continue;
		}
		die(is_write ? "write" : "read");
	}
}

static void *writer_main(void *arg)
{
	struct thread_res *t = arg;
	uint8_t rec[KQ_ATOMIC_MAX];
	struct kq_rec_hdr h;
	int fd = open_queue();
	uint64_t next = kq_now_ns();

	/* Build the payload once; only the header changes per record. */
	kq_rec_build(rec, cfg.size, t->id, 0, 0);
	memcpy(&h, rec, sizeof(h));
	h.csum = 0;
	for (uint64_t seq = 0;; seq++) {
		int ph = atomic_load_explicit(&phase, memory_order_relaxed);

		if (ph == PH_STOP)
			break;
		if (cfg.rate) {
			struct timespec ts;

			next += 1000000000ULL / cfg.rate;
			ts.tv_sec = (time_t)(next / 1000000000ULL);
			ts.tv_nsec = (long)(next % 1000000000ULL);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}
		h.seq = seq;
		uint64_t t0 = kq_now_ns();
		h.ts_ns = t0;
		memcpy(rec, &h, sizeof(h));
		xfer(fd, rec, true);
		uint64_t t1 = kq_now_ns();
		if (ph == PH_MEASURE) {
			t->ops++;
			t->bytes += cfg.size;
			res_add(&t->sys, t1 - t0);
		}
	}
	close(fd);
	return NULL;
}

static void *reader_main(void *arg)
{
	struct thread_res *t = arg;
	uint8_t rec[KQ_ATOMIC_MAX];
	struct kq_rec_hdr h;
	int fd = open_queue();

	for (;;) {
		uint64_t t0 = kq_now_ns();
		xfer(fd, rec, false);
		uint64_t t1 = kq_now_ns();

		memcpy(&h, rec, sizeof(h));
		if (h.magic != KQ_REC_MAGIC || h.len != cfg.size) {
			fprintf(stderr, "kq_bench: malformed record\n");
			exit(1);
		}
		if (h.writer == KQ_REC_POISON)
			break;
		if (atomic_load_explicit(&phase, memory_order_relaxed) == PH_MEASURE) {
			t->ops++;
			t->bytes += cfg.size;
			res_add(&t->sys, t1 - t0);
			res_add(&t->e2e, t1 - h.ts_ns);
		}
	}
	close(fd);
	return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

struct summary {
	uint64_t ops, bytes, p50, p90, p99, p999, max;
};

/* Merge one metric's reservoirs from n threads and compute percentiles. */
static struct summary summarize(struct thread_res *ts, unsigned int n, bool e2e)
{
	struct summary s = { 0 };
	uint64_t total = 0, k = 0, *all;

	for (unsigned int i = 0; i < n; i++)
		total += (e2e ? &ts[i].e2e : &ts[i].sys)->n_kept;
	all = malloc((total ? total : 1) * sizeof(uint64_t));
	if (!all)
		die("malloc");
	for (unsigned int i = 0; i < n; i++) {
		struct reservoir *r = e2e ? &ts[i].e2e : &ts[i].sys;

		memcpy(all + k, r->v, r->n_kept * sizeof(uint64_t));
		k += r->n_kept;
		s.ops += ts[i].ops;
		s.bytes += ts[i].bytes;
		if (r->max > s.max)
			s.max = r->max;
	}
	if (total) {
		qsort(all, total, sizeof(uint64_t), cmp_u64);
		s.p50 = all[(total - 1) * 50 / 100];
		s.p90 = all[(total - 1) * 90 / 100];
		s.p99 = all[(total - 1) * 99 / 100];
		s.p999 = all[(total - 1) * 999 / 1000];
	}
	free(all);
	return s;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -d, --device PATH   queue (default $KQ_DEVICE or %s)\n"
		"  -w, --writers N     writer threads (default 1)\n"
		"  -r, --readers N     reader threads (default 1)\n"
		"  -s, --size BYTES    record size %u..%u (default 64)\n"
		"  -t, --duration SEC  measured seconds (default 5)\n"
		"      --warmup SEC    unmeasured warm-up seconds (default 1)\n"
		"      --rate N        records/s per writer (default unthrottled)\n"
		"      --samples N     latency reservoir per thread (default 200000)\n"
		"  -N, --nonblock      O_NONBLOCK + poll()\n"
		"      --csv           one CSV line; --header prints the column names\n"
		"      --label TEXT    label for the CSV line\n",
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

#define CSV_HEADER                                                         \
	"label,device,writers,readers,size,nonblock,seconds,write_ops,read_ops," \
	"write_MBps,read_MBps,write_kops,read_kops,"                        \
	"w_p50_ns,w_p99_ns,w_p999_ns,w_max_ns,"                             \
	"r_p50_ns,r_p99_ns,r_p999_ns,r_max_ns,"                             \
	"e2e_p50_ns,e2e_p99_ns,e2e_p999_ns,e2e_max_ns,write_waits,read_waits"

int main(int argc, char **argv)
{
	enum { OPT_WARMUP = 1000, OPT_RATE, OPT_SAMPLES, OPT_CSV, OPT_HEADER,
	       OPT_LABEL };
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "writers", required_argument, NULL, 'w' },
		{ "readers", required_argument, NULL, 'r' },
		{ "size", required_argument, NULL, 's' },
		{ "duration", required_argument, NULL, 't' },
		{ "warmup", required_argument, NULL, OPT_WARMUP },
		{ "rate", required_argument, NULL, OPT_RATE },
		{ "samples", required_argument, NULL, OPT_SAMPLES },
		{ "nonblock", no_argument, NULL, 'N' },
		{ "csv", no_argument, NULL, OPT_CSV },
		{ "header", no_argument, NULL, OPT_HEADER },
		{ "label", required_argument, NULL, OPT_LABEL },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct kq_stats st0 = { 0 }, st1 = { 0 };
	bool is_kq;
	int opt, ctl;

	cfg.path = kq_device_path();
	while ((opt = getopt_long(argc, argv, "d:w:r:s:t:Nh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': cfg.path = optarg; break;
		case 'w': cfg.writers = (unsigned int)parse_or_die(optarg, "writers", 1, MAX_THREADS); break;
		case 'r': cfg.readers = (unsigned int)parse_or_die(optarg, "readers", 1, MAX_THREADS); break;
		case 's': cfg.size = (uint32_t)parse_or_die(optarg, "size", KQ_REC_MIN, KQ_ATOMIC_MAX); break;
		case 't': cfg.duration = (unsigned int)parse_or_die(optarg, "duration", 1, 3600); break;
		case OPT_WARMUP: cfg.warmup = (unsigned int)parse_or_die(optarg, "warmup", 0, 600); break;
		case OPT_RATE: cfg.rate = parse_or_die(optarg, "rate", 1, 100000000); break;
		case OPT_SAMPLES: cfg.samples = parse_or_die(optarg, "samples", 1000, 100000000); break;
		case 'N': cfg.nonblock = true; break;
		case OPT_CSV: cfg.csv = true; break;
		case OPT_HEADER: cfg.header = true; break;
		case OPT_LABEL: cfg.label = optarg; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (optind != argc) {
		usage(argv[0]);
		return 2;
	}
	if (cfg.header) {
		printf("%s\n", CSV_HEADER);
		return 0;
	}

	ctl = open(cfg.path, O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		die(cfg.path);
	is_kq = kq_get_stats(ctl, &st0) == 0;
	if (is_kq && ioctl(ctl, KQ_IOC_RESET) != 0)
		die("KQ_IOC_RESET");

	for (unsigned int i = 0; i < cfg.writers; i++) {
		wres[i].id = i;
		res_init(&wres[i].sys, 0x1000 + i);
		if (pthread_create(&wres[i].tid, NULL, writer_main, &wres[i]))
			die("pthread_create");
	}
	for (unsigned int i = 0; i < cfg.readers; i++) {
		rres[i].id = i;
		res_init(&rres[i].sys, 0x2000 + i);
		res_init(&rres[i].e2e, 0x3000 + i);
		if (pthread_create(&rres[i].tid, NULL, reader_main, &rres[i]))
			die("pthread_create");
	}

	kq_sleep_ms(cfg.warmup * 1000);
	if (is_kq)
		kq_get_stats(ctl, &st0);
	uint64_t t_start = kq_now_ns();
	atomic_store(&phase, PH_MEASURE);
	kq_sleep_ms(cfg.duration * 1000);
	atomic_store(&phase, PH_STOP);
	uint64_t t_end = kq_now_ns();
	if (is_kq)
		kq_get_stats(ctl, &st1);

	for (unsigned int i = 0; i < cfg.writers; i++)
		pthread_join(wres[i].tid, NULL);
	{
		/* One poison record per reader, queued behind everything else. */
		uint8_t rec[KQ_ATOMIC_MAX];
		int pfd = open(cfg.path, O_RDWR | O_CLOEXEC);

		if (pfd < 0)
			die(cfg.path);
		/* pfd is a blocking fd, so xfer() never sees EAGAIN here. */
		for (unsigned int i = 0; i < cfg.readers; i++) {
			kq_rec_build(rec, cfg.size, KQ_REC_POISON, i, 0);
			xfer(pfd, rec, true);
		}
		close(pfd);
	}
	for (unsigned int i = 0; i < cfg.readers; i++)
		pthread_join(rres[i].tid, NULL);

	double secs = (double)(t_end - t_start) / 1e9;
	struct summary w = summarize(wres, cfg.writers, false);
	struct summary r = summarize(rres, cfg.readers, false);
	struct summary e = summarize(rres, cfg.readers, true);
	double w_mbps = (double)w.bytes / secs / 1e6, r_mbps = (double)r.bytes / secs / 1e6;
	uint64_t ww = st1.write_waits - st0.write_waits;
	uint64_t rw = st1.read_waits - st0.read_waits;

	if (cfg.csv) {
		printf("%s,%s,%u,%u,%u,%d,%.3f,%llu,%llu,%.2f,%.2f,%.1f,%.1f,"
		       "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
		       "%llu,%llu\n",
		       cfg.label, is_kq ? "kernqueue" : "other", cfg.writers,
		       cfg.readers, cfg.size, cfg.nonblock, secs,
		       (unsigned long long)w.ops, (unsigned long long)r.ops,
		       w_mbps, r_mbps, (double)w.ops / secs / 1e3,
		       (double)r.ops / secs / 1e3,
		       (unsigned long long)w.p50, (unsigned long long)w.p99,
		       (unsigned long long)w.p999, (unsigned long long)w.max,
		       (unsigned long long)r.p50, (unsigned long long)r.p99,
		       (unsigned long long)r.p999, (unsigned long long)r.max,
		       (unsigned long long)e.p50, (unsigned long long)e.p99,
		       (unsigned long long)e.p999, (unsigned long long)e.max,
		       (unsigned long long)(is_kq ? ww : 0),
		       (unsigned long long)(is_kq ? rw : 0));
		return 0;
	}

	printf("kq_bench: %s (%s), %u writers, %u readers, %u-byte records, %s, "
	       "%.2f s measured after %u s warm-up%s\n",
	       cfg.path, is_kq ? "kernqueue" : "not kernqueue", cfg.writers,
	       cfg.readers, cfg.size, cfg.nonblock ? "non-blocking" : "blocking",
	       secs, cfg.warmup, cfg.rate ? " (rate-limited)" : "");
	printf("  throughput  write %10.2f MB/s %10.1f kops/s\n", w_mbps,
	       (double)w.ops / secs / 1e3);
	printf("              read  %10.2f MB/s %10.1f kops/s\n", r_mbps,
	       (double)r.ops / secs / 1e3);
	printf("  latency ns        p50        p90        p99      p99.9        max\n");
	printf("  write()    %10llu %10llu %10llu %10llu %10llu\n",
	       (unsigned long long)w.p50, (unsigned long long)w.p90,
	       (unsigned long long)w.p99, (unsigned long long)w.p999,
	       (unsigned long long)w.max);
	printf("  read()     %10llu %10llu %10llu %10llu %10llu\n",
	       (unsigned long long)r.p50, (unsigned long long)r.p90,
	       (unsigned long long)r.p99, (unsigned long long)r.p999,
	       (unsigned long long)r.max);
	printf("  end-to-end %10llu %10llu %10llu %10llu %10llu\n",
	       (unsigned long long)e.p50, (unsigned long long)e.p90,
	       (unsigned long long)e.p99, (unsigned long long)e.p999,
	       (unsigned long long)e.max);
	if (is_kq)
		printf("  driver: writer sleeps %llu, reader sleeps %llu during measurement\n",
		       (unsigned long long)ww, (unsigned long long)rw);
	return 0;
}

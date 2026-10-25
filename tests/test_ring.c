// SPDX-License-Identifier: GPL-2.0
/*
 * test_ring.c - user-space unit test for driver/kq_ring.h.
 *
 * kq_ring.h is the exact index arithmetic the kernel module uses. This test
 * drives it with millions of random operations and compares the result with a
 * reference model that keeps the queued bytes in a flat array (oldest byte at
 * index 0) and dequeues with memmove(). The two share no arithmetic, so
 * agreement is meaningful evidence that head/tail/count and the wraparound
 * split are right.
 *
 * Scope: this does NOT test the driver. Locking, user copies, blocking and
 * poll are covered by tests/test_functional.c and tests/test_stress.c, which
 * need the module loaded.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kq_ring.h"

static int failures;

/* Report and abandon the current test function on the first failed check. */
#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #cond);                            \
			failures++;                                          \
			return;                                              \
		}                                                            \
	} while (0)

/* xorshift64*: small, deterministic and good enough for test input. */
static uint64_t rng_state;

static uint64_t rng_next(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545F4914F6CDD1DULL;
}

/* Uniform-ish value in [0, n]; n may be 0. */
static size_t rng_upto(size_t n)
{
	return (size_t)(rng_next() % ((uint64_t)n + 1));
}

/* Byte i of the stream. A hash rather than i % k so any offset error shows. */
static uint8_t pattern(uint64_t i)
{
	return (uint8_t)((i * 0x9E3779B97F4A7C15ULL) >> 56);
}

static void test_init(void)
{
	struct kq_ring r;

	kq_ring_init(&r, 1);
	CHECK(kq_ring_valid(&r));
	CHECK(kq_ring_free(&r) == 1);

	kq_ring_init(&r, 4096);
	CHECK(kq_ring_valid(&r));
	CHECK(r.head == 0 && r.tail == 0 && r.count == 0);
	CHECK(kq_ring_free(&r) == 4096);
}

static void test_advance(void)
{
	CHECK(kq_ring_advance(0, 0, 8) == 0);
	CHECK(kq_ring_advance(6, 1, 8) == 7);
	CHECK(kq_ring_advance(7, 1, 8) == 0);
	CHECK(kq_ring_advance(0, 8, 8) == 0);
	CHECK(kq_ring_advance(3, 8, 8) == 3);
	CHECK(kq_ring_advance(0, 1, 1) == 0);

	/* Largest capacity for which idx + n cannot overflow. */
	size_t big = SIZE_MAX / 2;
	CHECK(kq_ring_advance(big - 1, big, big) == big - 1);
	CHECK(kq_ring_advance(big - 1, 1, big) == 0);
}

static void test_split(void)
{
	static const struct {
		size_t cap, start, n, len0, len1;
	} cases[] = {
		{ 8, 0, 0, 0, 0 },	/* empty transfer */
		{ 8, 0, 8, 8, 0 },	/* whole buffer, no wrap */
		{ 8, 5, 3, 3, 0 },	/* ends exactly at the end */
		{ 8, 5, 4, 3, 1 },	/* wraps by one byte */
		{ 8, 7, 8, 1, 7 },	/* whole buffer starting at the last byte */
		{ 1, 0, 1, 1, 0 },	/* smallest ring */
		{ 4099, 4000, 200, 99, 101 }, /* non power-of-two capacity */
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct kq_span s;

		kq_ring_split(cases[i].cap, cases[i].start, cases[i].n, &s);
		CHECK(s.off[0] == cases[i].start);
		CHECK(s.len[0] == cases[i].len0);
		CHECK(s.off[1] == 0);
		CHECK(s.len[1] == cases[i].len1);
		CHECK(s.len[0] + s.len[1] == cases[i].n);
	}
}

/* head == tail in both states; count must disambiguate them. */
static void test_full_vs_empty(void)
{
	struct kq_ring r;

	kq_ring_init(&r, 16);
	kq_ring_produce(&r, 5);
	kq_ring_consume(&r, 5);
	CHECK(r.head == 5 && r.tail == 5 && r.count == 0);

	kq_ring_produce(&r, 16);
	CHECK(kq_ring_valid(&r));
	CHECK(r.head == r.tail && r.count == 16 && kq_ring_free(&r) == 0);

	kq_ring_consume(&r, 16);
	CHECK(kq_ring_valid(&r));
	CHECK(r.head == r.tail && r.count == 0 && kq_ring_free(&r) == 16);

	kq_ring_produce(&r, 3);
	kq_ring_reset(&r);
	CHECK(kq_ring_valid(&r));
	CHECK(r.head == 0 && r.tail == 0 && r.count == 0);
}

/* Copy n bytes into the ring the way the driver does, via the spans. */
static void ring_put(struct kq_ring *r, uint8_t *buf, const uint8_t *src,
		     size_t n)
{
	struct kq_span s;

	kq_ring_write_spans(r, n, &s);
	memcpy(buf + s.off[0], src, s.len[0]);
	memcpy(buf + s.off[1], src + s.len[0], s.len[1]);
	kq_ring_produce(r, n);
}

static void ring_get(struct kq_ring *r, const uint8_t *buf, uint8_t *dst,
		     size_t n)
{
	struct kq_span s;

	kq_ring_read_spans(r, n, &s);
	memcpy(dst, buf + s.off[0], s.len[0]);
	memcpy(dst + s.len[0], buf + s.off[1], s.len[1]);
	kq_ring_consume(r, n);
}

/*
 * Random produce/consume/reset sequence checked against the flat-array model.
 * Checks I1-I4 after every operation and I5 (queued bytes are at head..head+
 * count-1, in order) by an independent '%'-based walk.
 */
static void run_model(size_t cap, unsigned long ops)
{
	uint8_t *buf = malloc(cap), *model = malloc(cap), *tmp = malloc(cap);
	uint64_t wseq = 0, rseq = 0;
	size_t model_len = 0;
	struct kq_ring r;

	if (!buf || !model || !tmp) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	kq_ring_init(&r, cap);

	for (unsigned long op = 0; op < ops; op++) {
		unsigned int kind = (unsigned int)(rng_next() % 100);

		if (kind < 48) {
			/* Produce; 1 in 8 fills the ring exactly. */
			size_t n = (rng_next() % 8 == 0) ? kq_ring_free(&r)
							 : rng_upto(kq_ring_free(&r));
			for (size_t i = 0; i < n; i++)
				tmp[i] = pattern(wseq + i);
			ring_put(&r, buf, tmp, n);
			memcpy(model + model_len, tmp, n);
			model_len += n;
			wseq += n;
		} else if (kind < 99) {
			/* Consume; 1 in 8 drains the ring exactly. */
			size_t n = (rng_next() % 8 == 0) ? r.count
							 : rng_upto(r.count);
			ring_get(&r, buf, tmp, n);
			if (memcmp(tmp, model, n) != 0) {
				fprintf(stderr, "data mismatch cap=%zu op=%lu\n",
					cap, op);
				failures++;
				goto out;
			}
			for (size_t i = 0; i < n; i++) {
				if (tmp[i] != pattern(rseq + i)) {
					fprintf(stderr,
						"stream mismatch cap=%zu op=%lu\n",
						cap, op);
					failures++;
					goto out;
				}
			}
			memmove(model, model + n, model_len - n);
			model_len -= n;
			rseq += n;
		} else {
			kq_ring_reset(&r);
			model_len = 0;
			rseq = wseq; /* discarded bytes are never read */
		}

		if (!kq_ring_valid(&r) || r.count != model_len) {
			fprintf(stderr,
				"invariant broken cap=%zu op=%lu head=%zu tail=%zu count=%zu model=%zu\n",
				cap, op, r.head, r.tail, r.count, model_len);
			failures++;
			goto out;
		}
		if (op % 64 == 0) {
			for (size_t i = 0; i < r.count; i++) {
				if (buf[(r.head + i) % cap] != model[i]) {
					fprintf(stderr, "I5 broken cap=%zu op=%lu i=%zu\n",
						cap, op, i);
					failures++;
					goto out;
				}
			}
		}
	}
out:
	free(buf);
	free(model);
	free(tmp);
}

static void test_random_model(void)
{
	/* Includes 1 and 2, powers of two, and awkward non-powers of two. */
	static const size_t caps[] = { 1, 2, 3, 7, 64, 255, 4096, 4099, 65536 };
	int before = failures;

	for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		run_model(caps[i], 200000);
		if (failures != before) {
			fprintf(stderr, "  rerun with the seed printed above\n");
			return;
		}
	}
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "init", test_init },
		{ "advance", test_advance },
		{ "split", test_split },
		{ "full_vs_empty", test_full_vs_empty },
		{ "random_model", test_random_model },
	};
	uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x4B51ULL;
	int total_failed = 0;

	rng_state = seed ? seed : 1;
	printf("test_ring: seed=0x%llx\n", (unsigned long long)seed);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;

		tests[i].fn();
		if (failures != before) {
			printf("FAIL %s\n", tests[i].name);
			total_failed++;
		} else {
			printf("PASS %s\n", tests[i].name);
		}
	}
	printf("test_ring: %zu tests, %d failed\n",
	       sizeof(tests) / sizeof(tests[0]), total_failed);
	return total_failed ? 1 : 0;
}

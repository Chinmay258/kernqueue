/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kq_ring.h - index bookkeeping for the KernQueue byte ring.
 *
 * Nothing in here locks, allocates or copies. It only maintains head, tail and
 * count, and splits a transfer into the (at most two) contiguous regions it
 * covers. Keeping it free of kernel dependencies lets the same code be built
 * into the module and into tests/test_ring.c, a user-space unit test that
 * checks it against a reference model.
 *
 * Naming follows queue terminology. Note that <linux/circ_buf.h> uses the
 * opposite convention (its "head" is the producer index).
 *   head  - index of the oldest queued byte, i.e. the next one a reader gets
 *   tail  - index where the next written byte will be stored
 *   count - number of bytes queued
 *
 * Invariants, which hold whenever the lock protecting the ring is free:
 *   I1  cap > 0
 *   I2  head < cap and tail < cap
 *   I3  count <= cap
 *   I4  tail == (head + count) mod cap
 *   I5  the queued bytes, oldest first, are at head, head+1, ..., head+count-1
 *       (all mod cap)
 * head == tail holds both when empty and when full; count tells them apart.
 * That costs one extra word but uses every byte of the buffer and works for
 * any capacity, not just powers of two (unlike kfifo's free-running indices).
 *
 * The caller must hold the ring's lock for every function that modifies it.
 * "count" is the one field the driver also reads without the lock (in wait
 * conditions and poll), so updates to it go through KQ_WRITE_ONCE.
 */
#ifndef KQ_RING_H
#define KQ_RING_H

#ifdef __KERNEL__
#include <linux/compiler.h>
#include <linux/types.h>
#define KQ_WRITE_ONCE(x, val) WRITE_ONCE(x, val)
#else
#include <stdbool.h>
#include <stddef.h>
/* The user-space unit test is single-threaded; a plain store is enough. */
#define KQ_WRITE_ONCE(x, val) ((x) = (val))
#endif

struct kq_ring {
	size_t head;
	size_t tail;
	size_t count;
	size_t cap;
};

/* A transfer of n <= cap bytes covers at most two contiguous regions. */
struct kq_span {
	size_t off[2];
	size_t len[2];
};

static inline void kq_ring_init(struct kq_ring *r, size_t cap)
{
	r->head = 0;
	r->tail = 0;
	r->count = 0;
	r->cap = cap;
}

static inline size_t kq_ring_free(const struct kq_ring *r)
{
	return r->cap - r->count;
}

/*
 * Advance an index by n (n <= cap) and wrap at cap. A conditional subtraction
 * instead of '%' avoids a division; it is enough because idx + n < 2 * cap.
 * That sum cannot overflow as long as cap <= SIZE_MAX / 2 (the driver caps the
 * ring at KQ_MAX_CAPACITY, 16 MiB).
 */
static inline size_t kq_ring_advance(size_t idx, size_t n, size_t cap)
{
	idx += n;
	if (idx >= cap)
		idx -= cap;
	return idx;
}

/*
 * Split n bytes starting at index start (start < cap, n <= cap) into a region
 * that runs to the end of the buffer and, if the transfer wraps, a second one
 * that starts at index 0. len[1] is 0 when the transfer does not wrap.
 */
static inline void kq_ring_split(size_t cap, size_t start, size_t n,
				 struct kq_span *s)
{
	size_t first = cap - start;

	if (first > n)
		first = n;
	s->off[0] = start;
	s->len[0] = first;
	s->off[1] = 0;
	s->len[1] = n - first;
}

/* Regions holding the next n queued bytes. Requires n <= count. */
static inline void kq_ring_read_spans(const struct kq_ring *r, size_t n,
				      struct kq_span *s)
{
	kq_ring_split(r->cap, r->head, n, s);
}

/* Regions where the next n bytes will be stored. Requires n <= free. */
static inline void kq_ring_write_spans(const struct kq_ring *r, size_t n,
				       struct kq_span *s)
{
	kq_ring_split(r->cap, r->tail, n, s);
}

/* Dequeue n bytes that have already been copied out. Requires n <= count. */
static inline void kq_ring_consume(struct kq_ring *r, size_t n)
{
	r->head = kq_ring_advance(r->head, n, r->cap);
	KQ_WRITE_ONCE(r->count, r->count - n);
}

/* Publish n bytes that have already been copied in. Requires n <= free. */
static inline void kq_ring_produce(struct kq_ring *r, size_t n)
{
	r->tail = kq_ring_advance(r->tail, n, r->cap);
	KQ_WRITE_ONCE(r->count, r->count + n);
}

static inline void kq_ring_reset(struct kq_ring *r)
{
	r->head = 0;
	r->tail = 0;
	KQ_WRITE_ONCE(r->count, 0);
}

/* Check invariants I1-I4 (I5 is about buffer contents, see test_ring.c). */
static inline bool kq_ring_valid(const struct kq_ring *r)
{
	return r->cap > 0 && r->head < r->cap && r->tail < r->cap &&
	       r->count <= r->cap &&
	       r->tail == kq_ring_advance(r->head, r->count, r->cap);
}

#endif /* KQ_RING_H */

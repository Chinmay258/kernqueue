/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kq_common.h - helpers shared by kqctl, the tests and the benchmark.
 */
#ifndef KQ_COMMON_H
#define KQ_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernqueue.h>

/* Device path: $KQ_DEVICE if set, else /dev/kernqueue. */
const char *kq_device_path(void);

/* CLOCK_MONOTONIC in nanoseconds. */
uint64_t kq_now_ns(void);

/* Sleep for ms milliseconds, resuming after signal interruptions. */
void kq_sleep_ms(unsigned int ms);

/* KQ_IOC_GET_STATS wrapper: 0 on success, -1 with errno set on failure. */
int kq_get_stats(int fd, struct kq_stats *st);

/*
 * Strict unsigned parse: rejects empty strings, signs, trailing junk and
 * overflow. Accepts an optional K/M/G suffix (powers of 1024).
 */
bool kq_parse_size(const char *s, uint64_t *out);

/* Byte i of the deterministic test stream (a hash, so offsets errors show). */
static inline uint8_t kq_pattern_byte(uint64_t i)
{
	return (uint8_t)((i * 0x9E3779B97F4A7C15ULL) >> 56);
}

/*
 * Record format used by the stress test and the benchmark. Every write() and
 * read() transfers exactly one record of a fixed size <= KQ_ATOMIC_MAX, so the
 * driver's atomic-write guarantee keeps records whole with many writers.
 */
#define KQ_REC_MAGIC 0x4B515243u   /* "KQRC" */
#define KQ_REC_POISON UINT32_MAX   /* writer id of a "stop reading" record */

struct kq_rec_hdr {
	uint32_t magic;
	uint32_t writer;   /* producer index, or KQ_REC_POISON */
	uint64_t seq;      /* per-producer sequence number, starting at 0 */
	uint64_t ts_ns;    /* send time (CLOCK_MONOTONIC), used by the benchmark */
	uint32_t len;      /* whole record length, header included */
	uint32_t csum;     /* FNV-1a over the record with csum = 0; 0 if unused */
};

#define KQ_REC_MIN ((uint32_t)sizeof(struct kq_rec_hdr))

/* FNV-1a, incremental: pass KQ_FNV_INIT first, then the previous result. */
#define KQ_FNV_INIT 2166136261u
uint32_t kq_fnv1a(uint32_t h, const void *data, size_t len);

/* Fill a len-byte record with a header and a payload derived from (writer, seq). */
void kq_rec_build(void *rec, uint32_t len, uint32_t writer, uint64_t seq,
		  uint64_t ts_ns);

/*
 * Validate a record built by kq_rec_build(): magic, length, checksum and every
 * payload byte. Returns NULL if valid, otherwise a static description.
 */
const char *kq_rec_verify(const void *rec, uint32_t len);

#endif /* KQ_COMMON_H */

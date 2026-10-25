// SPDX-License-Identifier: GPL-2.0
/*
 * kq_common.c - helpers shared by kqctl, the tests and the benchmark.
 */
#include "kq_common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

const char *kq_device_path(void)
{
	const char *p = getenv("KQ_DEVICE");

	return (p && *p) ? p : KQ_DEVICE_PATH;
}

uint64_t kq_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void kq_sleep_ms(unsigned int ms)
{
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};

	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

int kq_get_stats(int fd, struct kq_stats *st)
{
	return ioctl(fd, KQ_IOC_GET_STATS, st);
}

bool kq_parse_size(const char *s, uint64_t *out)
{
	unsigned long long v;
	unsigned int shift = 0;
	char *end;

	if (!s || *s < '0' || *s > '9')
		return false;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno)
		return false;
	switch (*end) {
	case '\0':
		break;
	case 'K': case 'k':
		shift = 10;
		end++;
		break;
	case 'M': case 'm':
		shift = 20;
		end++;
		break;
	case 'G': case 'g':
		shift = 30;
		end++;
		break;
	default:
		return false;
	}
	if (*end != '\0' || v > (UINT64_MAX >> shift))
		return false;
	*out = (uint64_t)v << shift;
	return true;
}

uint32_t kq_fnv1a(uint32_t h, const void *data, size_t len)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

/* Checksum of a record: the header with csum = 0, then the payload. */
static uint32_t rec_checksum(struct kq_rec_hdr h, const uint8_t *rec,
			     uint32_t len)
{
	h.csum = 0;
	return kq_fnv1a(kq_fnv1a(KQ_FNV_INIT, &h, sizeof(h)),
			rec + KQ_REC_MIN, len - KQ_REC_MIN);
}

/* Payload byte j of record (writer, seq). */
static uint8_t rec_payload_byte(uint32_t writer, uint64_t seq, uint32_t j)
{
	return kq_pattern_byte(((uint64_t)writer << 40) ^ (seq << 12) ^ j);
}

void kq_rec_build(void *rec, uint32_t len, uint32_t writer, uint64_t seq,
		  uint64_t ts_ns)
{
	struct kq_rec_hdr h = {
		.magic = KQ_REC_MAGIC,
		.writer = writer,
		.seq = seq,
		.ts_ns = ts_ns,
		.len = len,
		.csum = 0,
	};
	uint8_t *p = rec;

	for (uint32_t j = KQ_REC_MIN; j < len; j++)
		p[j] = rec_payload_byte(writer, seq, j);
	h.csum = rec_checksum(h, p, len);
	memcpy(p, &h, sizeof(h));
}

const char *kq_rec_verify(const void *rec, uint32_t len)
{
	const uint8_t *p = rec;
	struct kq_rec_hdr h;

	if (len < KQ_REC_MIN)
		return "record shorter than its header";
	memcpy(&h, p, sizeof(h));
	if (h.magic != KQ_REC_MAGIC)
		return "bad magic (record boundary lost?)";
	if (h.len != len)
		return "length field does not match record size";
	if (h.writer == KQ_REC_POISON)
		return NULL;
	if (rec_checksum(h, p, len) != h.csum)
		return "checksum mismatch";
	for (uint32_t j = KQ_REC_MIN; j < len; j++)
		if (p[j] != rec_payload_byte(h.writer, h.seq, j))
			return "payload byte mismatch";
	return NULL;
}

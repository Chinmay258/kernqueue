/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kq_test.h - a minimal test runner for the device tests.
 *
 * Each test case runs in its own forked child. A failed check prints the
 * location and exits the child with status 1; the parent enforces a timeout,
 * so a test stuck in a blocking call is reported as TIMEOUT instead of hanging
 * the whole run. The runner's exit status is nonzero if any test failed.
 */
#ifndef KQ_TEST_H
#define KQ_TEST_H

#include <errno.h>
#include <stddef.h>
#include <string.h>

struct kq_test_case {
	const char *name;
	void (*fn)(void);
	unsigned int timeout_ms; /* 0 selects the default (10 s) */
};

void kq_test_fail(const char *file, int line, const char *fmt, ...)
	__attribute__((noreturn, format(printf, 3, 4)));

#define KQ_FAIL(...) kq_test_fail(__FILE__, __LINE__, __VA_ARGS__)

#define KQ_CHECK(cond)                                                   \
	do {                                                             \
		if (!(cond))                                             \
			KQ_FAIL("CHECK(%s) failed (errno %d: %s)", #cond, \
				errno, strerror(errno));                  \
	} while (0)

#define KQ_CHECK_EQ(actual, expected)                                     \
	do {                                                              \
		long long kq_a_ = (long long)(actual);                    \
		long long kq_e_ = (long long)(expected);                  \
		if (kq_a_ != kq_e_)                                       \
			KQ_FAIL("%s == %s failed: got %lld, expected %lld" \
				" (errno %d: %s)", #actual, #expected,    \
				kq_a_, kq_e_, errno, strerror(errno));    \
	} while (0)

/* expr must fail with -1 and set errno to err. */
#define KQ_CHECK_ERRNO(expr, err)                                          \
	do {                                                               \
		errno = 0;                                                 \
		long long kq_r_ = (long long)(expr);                       \
		int kq_err_ = errno;                                       \
		if (kq_r_ != -1 || kq_err_ != (err))                       \
			KQ_FAIL("%s: expected -1 with errno %s, got %lld" \
				" with errno %d (%s)", #expr, #err, kq_r_, \
				kq_err_, strerror(kq_err_));               \
	} while (0)

/*
 * Run the selected tests (all, or those whose name contains one of the
 * non-option arguments) and return the process exit status.
 */
int kq_test_main(const struct kq_test_case *tests, size_t n, int argc,
		 char **argv);

#endif /* KQ_TEST_H */

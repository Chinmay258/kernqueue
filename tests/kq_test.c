// SPDX-License-Identifier: GPL-2.0
/*
 * kq_test.c - fork-per-test runner with parent-side timeouts.
 */
#include "kq_test.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kq_common.h"

#define DEFAULT_TIMEOUT_MS 10000u
#define KILL_GRACE_MS 2000u

void kq_test_fail(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "    %s:%d: ", file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	/* _exit, not exit: works from any thread and skips atexit handlers. */
	_exit(1);
}

enum outcome { OUT_PASS, OUT_FAIL, OUT_TIMEOUT, OUT_STUCK };

/*
 * Wait for the child up to timeout_ms. On timeout, SIGKILL it and wait a
 * little longer; a child that still cannot be reaped is in uninterruptible
 * sleep, which almost certainly means the driver is wedged.
 */
static enum outcome wait_child(pid_t pid, unsigned int timeout_ms, int *status)
{
	uint64_t deadline = kq_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
	bool killed = false;

	for (;;) {
		pid_t r = waitpid(pid, status, WNOHANG);

		if (r == pid)
			break;
		if (r < 0 && errno != EINTR) {
			perror("waitpid");
			return OUT_FAIL;
		}
		if (kq_now_ns() >= deadline) {
			if (killed)
				return OUT_STUCK;
			kill(pid, SIGKILL);
			killed = true;
			deadline = kq_now_ns() + KILL_GRACE_MS * 1000000ULL;
		}
		kq_sleep_ms(1);
	}
	if (killed)
		return OUT_TIMEOUT;
	return (WIFEXITED(*status) && WEXITSTATUS(*status) == 0) ? OUT_PASS
								  : OUT_FAIL;
}

static bool selected(const char *name, int argc, char **argv)
{
	bool any_filter = false;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;
		any_filter = true;
		if (strstr(name, argv[i]))
			return true;
	}
	return !any_filter;
}

int kq_test_main(const struct kq_test_case *tests, size_t n, int argc,
		 char **argv)
{
	unsigned int passed = 0, failed = 0, ran = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0) {
			for (size_t t = 0; t < n; t++)
				printf("%s\n", tests[t].name);
			return 0;
		}
		if (argv[i][0] == '-') {
			fprintf(stderr, "usage: %s [--list] [name-substring...]\n",
				argv[0]);
			return 2;
		}
	}

	for (size_t t = 0; t < n; t++) {
		unsigned int timeout = tests[t].timeout_ms ? tests[t].timeout_ms
							   : DEFAULT_TIMEOUT_MS;
		uint64_t start;
		enum outcome out;
		int status = 0;
		pid_t pid;

		if (!selected(tests[t].name, argc, argv))
			continue;
		ran++;
		/* Flush so buffered output is not duplicated into the child. */
		fflush(stdout);
		fflush(stderr);
		start = kq_now_ns();
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			/* Die with the runner rather than linger holding the device. */
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			tests[t].fn();
			fflush(stdout);
			_exit(0);
		}

		out = wait_child(pid, timeout, &status);
		double ms = (double)(kq_now_ns() - start) / 1e6;

		switch (out) {
		case OUT_PASS:
			passed++;
			printf("PASS     %-34s %8.1f ms\n", tests[t].name, ms);
			break;
		case OUT_FAIL:
			failed++;
			if (WIFSIGNALED(status))
				printf("FAIL     %-34s (killed by signal %d)\n",
				       tests[t].name, WTERMSIG(status));
			else
				printf("FAIL     %-34s\n", tests[t].name);
			break;
		case OUT_TIMEOUT:
			failed++;
			printf("TIMEOUT  %-34s (> %u ms, killed)\n",
			       tests[t].name, timeout);
			break;
		case OUT_STUCK:
			failed++;
			printf("STUCK    %-34s (unkillable after timeout; "
			       "check dmesg, stopping)\n", tests[t].name);
			printf("SUMMARY: %u run, %u passed, %u failed (aborted)\n",
			       ran, passed, failed);
			return 1;
		}
	}

	printf("SUMMARY: %u run, %u passed, %u failed\n", ran, passed, failed);
	if (ran == 0) {
		fprintf(stderr, "no test matched the given filters\n");
		return 1;
	}
	return failed ? 1 : 0;
}

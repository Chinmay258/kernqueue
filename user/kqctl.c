// SPDX-License-Identifier: GPL-2.0
/*
 * kqctl.c - command-line client for /dev/kernqueue.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kq_common.h"

static void usage(FILE *out)
{
	fprintf(out,
		"usage: kqctl [-d DEVICE] [-n] COMMAND [ARG]\n"
		"  -d DEVICE        device path (default $KQ_DEVICE or %s)\n"
		"  -n               open with O_NONBLOCK\n"
		"commands:\n"
		"  stats            print the driver statistics\n"
		"  write TEXT       queue TEXT (no newline is added)\n"
		"  write -          copy stdin into the queue until EOF\n"
		"  read N           read up to N bytes and copy them to stdout\n"
		"  dump             read until the queue is empty, to stdout\n"
		"  reset            discard all queued bytes\n"
		"  clear-stats      zero the cumulative counters\n"
		"  poll [MS]        wait up to MS ms (default 0) and report readiness\n",
		KQ_DEVICE_PATH);
}

static int print_stats(int fd)
{
	struct kq_stats st;

	if (kq_get_stats(fd, &st) != 0) {
		perror("KQ_IOC_GET_STATS");
		return 1;
	}
	printf("capacity       %llu\n", (unsigned long long)st.capacity);
	printf("used           %llu\n", (unsigned long long)st.used);
	printf("open_files     %llu\n", (unsigned long long)st.open_files);
	printf("bytes_written  %llu\n", (unsigned long long)st.bytes_written);
	printf("bytes_read     %llu\n", (unsigned long long)st.bytes_read);
	printf("write_calls    %llu\n", (unsigned long long)st.write_calls);
	printf("read_calls     %llu\n", (unsigned long long)st.read_calls);
	printf("write_waits    %llu\n", (unsigned long long)st.write_waits);
	printf("read_waits     %llu\n", (unsigned long long)st.read_waits);
	printf("write_eagain   %llu\n", (unsigned long long)st.write_eagain);
	printf("read_eagain    %llu\n", (unsigned long long)st.read_eagain);
	printf("resets         %llu\n", (unsigned long long)st.resets);
	return 0;
}

/* Write all n bytes, looping over short writes (writes > KQ_ATOMIC_MAX). */
static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t r = write(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("write");
			return 1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static int copy_stdin(int fd)
{
	char buf[KQ_ATOMIC_MAX];

	for (;;) {
		ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));

		if (n == 0)
			return 0;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read(stdin)");
			return 1;
		}
		if (write_all(fd, buf, (size_t)n))
			return 1;
	}
}

static int read_to_stdout(int fd, size_t limit, bool until_empty)
{
	char buf[KQ_ATOMIC_MAX];
	size_t total = 0;

	while (until_empty || total < limit) {
		size_t want = sizeof(buf);
		ssize_t n;

		if (!until_empty && limit - total < want)
			want = limit - total;
		n = read(fd, buf, want);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN && until_empty)
				break;
			perror("read");
			return 1;
		}
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			return 1;
		total += (size_t)n;
		/* A plain "read N" behaves like one read(2): it returns what is there. */
		if (!until_empty)
			break;
	}
	fflush(stdout);
	fprintf(stderr, "kqctl: read %zu bytes\n", total);
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = kq_device_path();
	int flags = O_RDWR | O_CLOEXEC;
	const char *cmd;
	int opt, fd, rc;

	while ((opt = getopt(argc, argv, "d:nh")) != -1) {
		switch (opt) {
		case 'd':
			path = optarg;
			break;
		case 'n':
			flags |= O_NONBLOCK;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind >= argc) {
		usage(stderr);
		return 2;
	}
	cmd = argv[optind];
	/* dump stops at the first EAGAIN, so it always runs non-blocking. */
	if (strcmp(cmd, "dump") == 0)
		flags |= O_NONBLOCK;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "kqctl: open(%s): %s\n", path, strerror(errno));
		return 1;
	}

	if (strcmp(cmd, "stats") == 0) {
		rc = print_stats(fd);
	} else if (strcmp(cmd, "write") == 0 && optind + 1 < argc) {
		const char *text = argv[optind + 1];

		rc = strcmp(text, "-") == 0 ? copy_stdin(fd)
					    : write_all(fd, text, strlen(text));
	} else if (strcmp(cmd, "read") == 0 && optind + 1 < argc) {
		uint64_t n;

		if (!kq_parse_size(argv[optind + 1], &n) || n == 0) {
			fprintf(stderr, "kqctl: invalid byte count\n");
			rc = 2;
		} else {
			rc = read_to_stdout(fd, (size_t)n, false);
		}
	} else if (strcmp(cmd, "dump") == 0) {
		rc = read_to_stdout(fd, 0, true);
	} else if (strcmp(cmd, "reset") == 0) {
		rc = ioctl(fd, KQ_IOC_RESET) ? (perror("KQ_IOC_RESET"), 1) : 0;
	} else if (strcmp(cmd, "clear-stats") == 0) {
		rc = ioctl(fd, KQ_IOC_CLEAR_STATS) ? (perror("KQ_IOC_CLEAR_STATS"), 1) : 0;
	} else if (strcmp(cmd, "poll") == 0) {
		uint64_t ms = 0;
		struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };

		if (optind + 1 < argc && !kq_parse_size(argv[optind + 1], &ms)) {
			fprintf(stderr, "kqctl: invalid timeout\n");
			rc = 2;
		} else if (poll(&p, 1, ms > 60000 ? 60000 : (int)ms) < 0) {
			perror("poll");
			rc = 1;
		} else {
			printf("%s%s%s\n", (p.revents & POLLIN) ? "POLLIN " : "",
			       (p.revents & POLLOUT) ? "POLLOUT" : "",
			       p.revents ? "" : "(not ready)");
			rc = 0;
		}
	} else {
		usage(stderr);
		rc = 2;
	}
	close(fd);
	return rc;
}

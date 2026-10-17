// SPDX-License-Identifier: GPL-2.0-or-later
/* One-shot read-only TLMM signal capture, not a production PCM receiver. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "opendectcell.h"

static int write_all(int fd, const void *data, size_t size)
{
	const uint8_t *ptr = data;

	while (size) {
		ssize_t n = write(fd, ptr, size);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		ptr += n;
		size -= (size_t)n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct dect_ioc_capture c = { 0 };
	uint8_t *samples;
	unsigned long duration = 1000;
	char *end;
	int devfd, outfd, ret = 1;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s OUT.bin [duration_us: 1..8000]\n",
			argv[0]);
		return 1;
	}
	if (argc == 3) {
		errno = 0;
		duration = strtoul(argv[2], &end, 10);
		if (errno || !argv[2][0] || *end ||
		    duration < 1 || duration > DECT_CAPTURE_MAX_US) {
			fprintf(stderr, "Invalid duration\n");
			return 1;
		}
	}

	samples = calloc(1, DECT_CAPTURE_BUFFER_BYTES);
	if (!samples) {
		perror("calloc");
		return 1;
	}
	devfd = open("/dev/dect0", O_RDONLY | O_CLOEXEC);
	if (devfd < 0) {
		perror("open /dev/dect0");
		goto out_free;
	}
	c.duration_us = (uint32_t)duration;
	c.sample_capacity = DECT_CAPTURE_BUFFER_BYTES;
	c.samples_ptr = (uint64_t)(uintptr_t)samples;
	if (ioctl(devfd, DECT_IOC_CAPTURE, &c) < 0) {
		perror("DECT_IOC_CAPTURE");
		goto out_close;
	}
	printf("DCL: %u Hz (period %u ns), read: %u ns, "
	       "reads/period: %u, reads/half: %u\n",
	       c.observed_clock_hz, c.dcl_period_ns, c.ns_per_read,
	       c.reads_per_period, c.reads_per_half_period);
	printf("Capture: %u samples, flags 0x%08x, "
	       "start=%" PRIu64 " ns end=%" PRIu64 " ns\n",
	       c.sample_count, c.flags, (uint64_t)c.start_ns,
	       (uint64_t)c.end_ns);
	if (c.flags & DECT_CAPTURE_F_NO_CLOCK)
		fprintf(stderr, "No sufficiently observable DCL clock\n");
	if (c.flags & DECT_CAPTURE_F_TOO_SLOW)
		fprintf(stderr, "MMIO polling too slow/unreliable\n");
	if (c.flags & DECT_CAPTURE_F_INCOMPLETE)
		fprintf(stderr, "Warning: observed edges below expected rate\n");

	if (c.sample_count > DECT_CAPTURE_BUFFER_BYTES) {
		fprintf(stderr, "Invalid kernel sample count\n");
		goto out_close;
	}
	outfd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		       0600);
	if (outfd < 0) {
		perror("open output");
		goto out_close;
	}
	if (write_all(outfd, samples, c.sample_count) < 0) {
		perror("write output");
		close(outfd);
		goto out_close;
	}
	if (close(outfd) < 0) {
		perror("close output");
		goto out_close;
	}
	ret = c.flags & (DECT_CAPTURE_F_NO_CLOCK | DECT_CAPTURE_F_TOO_SLOW)
		? 2 : 0;
out_close:
	close(devfd);
out_free:
	free(samples);
	return ret;
}

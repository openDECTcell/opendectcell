// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gpio-activity: crude liveness check for fast signals (e.g. the PCM
 * bit clock) using the sysfs GPIO interface.
 *
 * Each pin is read back-to-back through its value file. A static pin
 * shows no changes; a signal much faster than the read rate shows random
 * changes (aliasing), so activity can be detected even though the clock
 * cannot be measured. The pins are exported as inputs and unexported
 * afterwards. Only give it pins that nothing else uses.
 *
 * usage: gpio-activity BASE PIN[,PIN...] [SAMPLES]
 *        BASE = number of the gpiochip's first GPIO (512 on the 7520)
 */
#define _DEFAULT_SOURCE 1

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, text, strlen(text));
	close(fd);
	return n < 0 ? -1 : 0;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s BASE PIN[,PIN...] [SAMPLES]\n", argv[0]);
		return 2;
	}
	long base = strtol(argv[1], NULL, 0);
	long samples = argc > 3 ? strtol(argv[3], NULL, 0) : 100000;
	char *list = strdup(argv[2]);

	printf("%-5s %9s %9s %8s\n", "pin", "high %", "changes", "rate/s");
	for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
		long pin = strtol(tok, NULL, 0);
		char path[96], num[16];

		snprintf(num, sizeof(num), "%ld", base + pin);
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%ld", base + pin);
		int was_exported = !access(path, F_OK);
		if (!was_exported)
			put("/sys/class/gpio/export", num);
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%ld/direction", base + pin);
		if (put(path, "in")) {
			printf("%-5ld (cannot use)\n", pin);
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%ld/value", base + pin);
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			printf("%-5ld (cannot open)\n", pin);
			continue;
		}
		char c, prev = 0;
		long high = 0, changes = 0;
		double t0 = now();
		for (long i = 0; i < samples; i++) {
			if (pread(fd, &c, 1, 0) != 1)
				break;
			high += c == '1';
			if (i && c != prev)
				changes++;
			prev = c;
		}
		double dt = now() - t0;
		printf("%-5ld %8.1f%% %9ld %8.0f\n", pin,
		       100.0 * (double)high / (double)samples, changes,
		       (double)changes / dt);
		close(fd);
		if (!was_exported) {
			snprintf(num, sizeof(num), "%ld", base + pin);
			put("/sys/class/gpio/unexport", num);
		}
	}
	return 0;
}

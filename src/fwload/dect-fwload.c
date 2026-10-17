// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-fwload: upload the STUB to the DECT chip over a UART.
 *
 * Bring-up version for gate G1: reset goes through the sysfs GPIO
 * interface and the UART is a plain tty. This deliberately takes the
 * GPIO number on the command line; it is replaced by /dev/dect0 and the
 * board profile (docs/hardware-abstraction.md) once the kernel module
 * exists. The boot-mode strap is not available from userspace, so the
 * "slow" stage-1 attempts run without it.
 */
#define _DEFAULT_SOURCE 1

#include "fwimage.h"
#include "loader.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct dev {
	int uart;
	int gpio_value;
	int verbose;
	unsigned baud;
	const char *uart_path;
	int strap_gpio;          /* sysfs number of the UART RX pin, or -1 */
	const char *pinctrl_dir; /* debugfs directory of the pin controller */
	const char *pinmux_uart; /* "group function" that restores the UART */
};

/* Reset line to release if we are interrupted (a held reset would leave
 * the chip dead until the next run). */
static int release_fd = -1;

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	if (release_fd >= 0 && write(release_fd, "1", 1) < 0) {
		/* nothing more to do */
	}
	_exit(3);
}

/* Reopen the tty after a hangup and reapply the line settings. */
static int dev_set_baud(void *ctx, unsigned baud);

static int uart_reopen_raw(struct dev *d)
{
	if (d->uart >= 0)
		close(d->uart);
	d->uart = open(d->uart_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (d->uart < 0)
		return -errno;
	fprintf(stderr, "uart: reopened after hangup\n");
	return 0;
}

static int uart_reopen(struct dev *d)
{
	if (d->uart >= 0)
		close(d->uart);
	d->uart = open(d->uart_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (d->uart < 0)
		return -errno;
	fprintf(stderr, "uart: reopened after hangup\n");
	return dev_set_baud(d, d->baud ? d->baud : 115200);
}

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	int r = 0;

	if (fd < 0)
		return -errno;
	if (write(fd, text, strlen(text)) < 0)
		r = -errno;
	close(fd);
	return r;
}

static int dev_read_byte(void *ctx, unsigned timeout_ms)
{
	struct dev *d = ctx;
	struct pollfd p = { .fd = d->uart, .events = POLLIN };
	uint8_t b;

	int r = poll(&p, 1, (int)timeout_ms);
	if (r < 0)
		return -errno;
	if (r == 0)
		return -ETIMEDOUT;
	ssize_t n = read(d->uart, &b, 1);
	if (n != 1) {
		fprintf(stderr, "uart: read returned %zd, poll events 0x%x\n",
			n, (unsigned)p.revents);
		if (!uart_reopen(d))
			return -ETIMEDOUT; /* treat as "nothing received yet" */
		return -EIO;
	}
	if (d->verbose)
		fprintf(stderr, "  [%ld] rx 0x%02x\n", now_ms() % 100000, b);
	return b;
}

static int dev_poll_byte(void *ctx)
{
	int r = dev_read_byte(ctx, 0);

	return r == -ETIMEDOUT ? -EAGAIN : r;
}

static int dev_write(void *ctx, const uint8_t *buf, size_t len)
{
	struct dev *d = ctx;

	if (d->verbose)
		fprintf(stderr, "  [%ld] tx %zu bytes, first 0x%02x\n",
			now_ms() % 100000, len, buf[0]);
	while (len) {
		ssize_t n = write(d->uart, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		buf += n;
		len -= (size_t)n;
	}
	tcdrain(d->uart);
	return 0;
}

static int dev_set_baud(void *ctx, unsigned baud)
{
	struct dev *d = ctx;
	struct termios t;
	speed_t s;

	switch (baud) {
	case 9600: s = B9600; break;
	case 19200: s = B19200; break;
	case 115200: s = B115200; break;
	case 230400: s = B230400; break;
	case 460800: s = B460800; break;
	default: return -EINVAL;
	}
	d->baud = baud;
	if (d->uart < 0)
		return 0; /* applied when the port is reopened */
	if (tcgetattr(d->uart, &t) && errno == EIO && !uart_reopen_raw(d))
		(void)0; /* reopened; fall through with the new fd */
	if (tcgetattr(d->uart, &t)) {
		fprintf(stderr, "uart: tcgetattr failed: %s\n", strerror(errno));
		return -errno;
	}
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(unsigned)(CSTOPB | PARENB | CRTSCTS);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, s);
	cfsetospeed(&t, s);
	if (tcsetattr(d->uart, TCSAFLUSH, &t)) {
		fprintf(stderr, "uart: tcsetattr failed: %s\n", strerror(errno));
		return -errno;
	}
	fprintf(stderr, "uart: %u baud\n", baud);
	return 0;
}

static int dev_reset(void *ctx, int asserted)
{
	struct dev *d = ctx;

	/* asserted = reset line driven low */
	if (pwrite(d->gpio_value, asserted ? "0" : "1", 1, 0) != 1)
		return -errno;
	if (!asserted)
		tcflush(d->uart, TCIFLUSH);
	return 0;
}

/*
 * Boot-mode strap: the chip samples the UART RX pin during reset.
 * Exporting the pin as a GPIO switches it away from the UART; releasing
 * it does not switch it back, so the UART function is restored through
 * the pinctrl debug file "pinmux-select". (Unbinding the serial driver
 * instead crashes the box: this UART is the kernel console.)
 */
static int dev_strap(void *ctx, int low)
{
	struct dev *d = ctx;
	char path[160], buf[16];

	if (d->strap_gpio < 0)
		return 0;
	snprintf(buf, sizeof(buf), "%d", d->strap_gpio);
	if (low) {
		write_file("/sys/class/gpio/export", buf);
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction",
			 d->strap_gpio);
		return write_file(path, "low");
	}
	write_file("/sys/class/gpio/unexport", buf);
	snprintf(path, sizeof(path), "%s/pinmux-select", d->pinctrl_dir);
	return write_file(path, d->pinmux_uart);
}

static void dev_sleep(void *ctx, unsigned ms)
{
	(void)ctx;
	usleep(ms * 1000u);
}

static void dev_log(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "loader: %s\n", msg);
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *p;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	p = malloc((size_t)n + 1);
	if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		p = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return p;
}

/* Export a sysfs GPIO as an output, held in "released" (high). */
static int gpio_open(unsigned num)
{
	char path[64], buf[16];

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u", num);
	if (access(path, F_OK)) {
		snprintf(buf, sizeof(buf), "%u", num);
		write_file("/sys/class/gpio/export", buf);
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", num);
	if (write_file(path, "high"))
		return -1;
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", num);
	return open(path, O_WRONLY);
}

/* After the upload: show what the STUB prints on the UART. */
static void listen(struct dev *d, unsigned seconds)
{
	unsigned n = 0;
	int b;

	fprintf(stderr, "listening for %u s ...\n", seconds);
	for (unsigned i = 0; i < seconds * 10; i++) {
		while ((b = dev_read_byte(d, 100)) >= 0) {
			unsigned char c = (unsigned char)b;
			fputc(c >= 0x20 && c < 0x7f ? (int)c : (c == '\n' ? '\n' : '.'),
			      stdout);
			n++;
		}
	}
	printf("\n");
	fprintf(stderr, "received %u bytes from the STUB\n", n);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-fwload --uart DEV --reset-gpio N --first FILE.hex "
		"--second FILE.hex [--strap-gpio N] [--mode 0x105] [--listen SECONDS] [-v]\n");
}

/* Diagnostic: open the port, set it up and report poll events for a while. */
static int probe(const char *path, unsigned baud, unsigned seconds)
{
	struct dev d = { .uart = -1, .gpio_value = -1, .strap_gpio = -1,
			 .uart_path = path };
	unsigned bytes = 0;

	d.uart = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (d.uart < 0 || dev_set_baud(&d, baud)) {
		fprintf(stderr, "probe: open/set baud failed: %s\n", strerror(errno));
		return 1;
	}
	for (unsigned i = 0; i < seconds * 10; i++) {
		struct pollfd p = { .fd = d.uart, .events = POLLIN };
		int r = poll(&p, 1, 100);
		if (r > 0) {
			uint8_t b[64];
			ssize_t n = read(d.uart, b, sizeof(b));
			fprintf(stderr, "probe t=%u00ms: events 0x%x read %zd\n", i,
				(unsigned)p.revents, n);
			if (n > 0)
				bytes += (unsigned)n;
			if (n <= 0)
				break;
		}
	}
	fprintf(stderr, "probe: %u bytes received\n", bytes);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "--probe"))
		return probe(argv[2], argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 115200, 3);

	const char *uart = NULL, *first_path = NULL, *second_path = NULL;
	unsigned reset_gpio = 0, mode = 0, listen_s = 5;
	int have_gpio = 0;
	struct dev d = { .uart = -1, .gpio_value = -1, .strap_gpio = -1,
			 .pinctrl_dir = "/sys/kernel/debug/pinctrl/1000000.pinctrl",
			 .pinmux_uart = "gpio16 blsp_uart0" };

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--uart") && i + 1 < argc)
			uart = argv[++i];
		else if (!strcmp(argv[i], "--reset-gpio") && i + 1 < argc)
			reset_gpio = (unsigned)strtoul(argv[++i], NULL, 0), have_gpio = 1;
		else if (!strcmp(argv[i], "--strap-gpio") && i + 1 < argc)
			d.strap_gpio = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--first") && i + 1 < argc)
			first_path = argv[++i];
		else if (!strcmp(argv[i], "--second") && i + 1 < argc)
			second_path = argv[++i];
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc)
			mode = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--listen") && i + 1 < argc)
			listen_s = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-v"))
			d.verbose = 1;
		else {
			usage();
			return 2;
		}
	}
	if (!uart || !have_gpio || !first_path || !second_path) {
		usage();
		return 2;
	}

	size_t n1, n2;
	char *hex1 = slurp(first_path, &n1), *hex2 = slurp(second_path, &n2);
	if (!hex1 || !hex2) {
		fprintf(stderr, "cannot read firmware files\n");
		return 1;
	}
	const struct fwi_family *fam = fwi_family_from_name(second_path);
	struct fwi_image first, second;
	if (fwi_build_firstlevel(hex1, n1, fam->firstlevel_addr, &first) ||
	    fwi_build_secondlevel(hex2, n2, &second)) {
		fprintf(stderr, "firmware files are not valid Intel HEX\n");
		return 1;
	}
	struct fwi_patch_input in = { .mode = (uint8_t)mode };
	struct fwi_patch_report rep;
	if (fwi_patch(&second, fam, &in, &rep)) {
		fprintf(stderr, "no patch area in the STUB image\n");
		return 1;
	}
	fprintf(stderr, "images: first %zu bytes, STUB %zu bytes, mode byte 0x%02x "
			"(%u TLV patched)\n", first.len, second.len, mode & 0xFF,
		rep.replaced);

	d.uart_path = uart;
	d.uart = open(uart, O_RDWR | O_NOCTTY | O_NONBLOCK);
	d.gpio_value = gpio_open(reset_gpio);
	release_fd = d.gpio_value;
	signal(SIGPIPE, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (d.uart < 0 || d.gpio_value < 0) {
		fprintf(stderr, "cannot open uart or reset gpio: %s\n", strerror(errno));
		return 1;
	}

	struct fwl_io io = { .ctx = &d, .read_byte = dev_read_byte,
			     .poll_byte = dev_poll_byte, .write = dev_write,
			     .set_baud = dev_set_baud, .reset = dev_reset,
			     .strap = dev_strap, .sleep_ms = dev_sleep,
			     .log = dev_log };
	/* With a strap pin the stock order applies (slow first, bit 8 set);
	 * without one, fast attempts come first. */
	struct fwl_params p = { .mode_word = (mode & 0xFFu) |
					     (d.strap_gpio >= 0 ? 0x100u : 0),
				.stage2_baud = fwl_stage2_baud(first_path) };
	struct fwl_result res;

	int r = fwl_upload(&io, &p, &first, &second, &res);
	fprintf(stderr, "upload %s (rounds %u, stage 1 attempts %u, stage 2 attempts %u)\n",
		r ? "FAILED" : "OK", res.rounds, res.stage1_attempts, res.stage2_attempts);
	if (r) {
		fprintf(stderr, "error %d (%s); last NAK code bytes: %u\n", r,
			strerror(-r), res.last_nak_len);
		/* leave the pins as we found them */
		dev_reset(&d, 0);
		dev_strap(&d, 0);
		return 1;
	}
	listen(&d, listen_s);
	return 0;
}

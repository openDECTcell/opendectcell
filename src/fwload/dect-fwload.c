// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-fwload: upload the STUB to the DECT chip over a UART.
 *
 * Chip reset and the boot-mode strap go through /dev/dect0 (the kernel
 * module, docs/kmod-spec.md); the UART is a plain tty. No GPIO numbers
 * or pin-controller details are handled here.
 */
#define _DEFAULT_SOURCE 1

#include "../../kernel/include/uapi/opendectcell.h"
#include "fwimage.h"
#include "loader.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct dev {
	int uart;
	int dect_fd;             /* /dev/dect0 */
	int verbose;
	unsigned baud;
	const char *uart_path;
};

/* Released if we are interrupted: a held reset would leave the chip dead
 * until the next run. (The module also releases it when the fd closes.) */
static int release_dect_fd = -1;

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	if (release_dect_fd >= 0) {
		__u32 v = 0;

		if (ioctl(release_dect_fd, DECT_IOC_RESET, &v) < 0) {
			/* nothing more to do */
		}
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

	__u32 v = asserted ? 1u : 0u;

	if (ioctl(d->dect_fd, DECT_IOC_RESET, &v) < 0)
		return -errno;
	if (!asserted)
		tcflush(d->uart, TCIFLUSH);
	return 0;
}

/*
 * Boot-mode strap: the chip samples the UART RX pin during reset. The
 * module drives it low and, on release, gives the pin back to the UART.
 */
static int dev_strap(void *ctx, int low)
{
	struct dev *d = ctx;
	__u32 v = low ? 1u : 0u;

	return ioctl(d->dect_fd, DECT_IOC_STRAP, &v) < 0 ? -errno : 0;
}

/*
 * Boards without a boot-mode strap (the 7412): the strap step exists only
 * so the "slow" reset order can drive a pin. Substitute a no-op so the
 * order stays the same but no pin is touched.
 */
static int dev_strap_none(void *ctx, int low)
{
	(void)ctx;
	(void)low;
	return 0;
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

/*
 * Factory DECT block (docs/models/7520-DECT-analysis.md §8.3): a 4-byte
 * header (type 0x02, id 0x01, length as 16-bit big-endian) followed by the
 * TLV list. Read-only access to the boot-loader partition.
 */
static int read_factory(const char *path, off_t offset, uint8_t *buf, size_t *len)
{
	uint8_t hdr[4];
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -errno;
	if (pread(fd, hdr, sizeof(hdr), offset) != (ssize_t)sizeof(hdr)) {
		close(fd);
		return -EIO;
	}
	size_t n = (size_t)hdr[2] << 8 | hdr[3];
	if (hdr[0] != 0x02 || hdr[1] != 0x01 || n == 0 || n > FWI_FACTORY_MAX ||
	    pread(fd, buf, n, offset + 4) != (ssize_t)n) {
		close(fd);
		return -EINVAL;
	}
	close(fd);
	*len = n;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-fwload --uart DEV {--dect-dev /dev/dect0 | --dect-fd N} "
		"--first FILE.hex --second FILE.hex [--factory-mtd DEV --factory-offset N]\n"
		"                   [--mode 0x105] [--listen SECONDS] [--no-strap] [-v]\n"
		"       dect-fwload --probe DEV [BAUD]\n");
}

/* Diagnostic: open the port, set it up and report poll events for a while. */
static int probe(const char *path, unsigned baud, unsigned seconds)
{
	struct dev d = { .uart = -1, .dect_fd = -1, .uart_path = path };
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
	unsigned mode = 0, listen_s = 5;
	const char *factory_mtd = NULL, *dect_dev = NULL;
	int dect_fd_arg = -1;	/* inherited, e.g. from dect-pcmcap -e */
	off_t factory_off = 0;
	int no_strap = 0;
	uint8_t factory[FWI_FACTORY_MAX];
	size_t factory_len = 0;
	struct dev d = { .uart = -1, .dect_fd = -1 };

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--uart") && i + 1 < argc)
			uart = argv[++i];
		else if (!strcmp(argv[i], "--dect-dev") && i + 1 < argc)
			dect_dev = argv[++i];
		else if (!strcmp(argv[i], "--dect-fd") && i + 1 < argc)
			dect_fd_arg = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--first") && i + 1 < argc)
			first_path = argv[++i];
		else if (!strcmp(argv[i], "--second") && i + 1 < argc)
			second_path = argv[++i];
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc)
			mode = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--factory-mtd") && i + 1 < argc)
			factory_mtd = argv[++i];
		else if (!strcmp(argv[i], "--factory-offset") && i + 1 < argc)
			factory_off = (off_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--listen") && i + 1 < argc)
			listen_s = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--no-strap"))
			no_strap = 1;
		else if (!strcmp(argv[i], "-v"))
			d.verbose = 1;
		else {
			usage();
			return 2;
		}
	}
	if (!uart || !dect_dev == (dect_fd_arg < 0) || !first_path || !second_path) {
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
	if (factory_mtd && read_factory(factory_mtd, factory_off, factory, &factory_len)) {
		fprintf(stderr, "cannot read the factory DECT block from %s\n", factory_mtd);
		return 1;
	}
	struct fwi_patch_input in = { .factory = factory_len ? factory : NULL,
				      .factory_len = factory_len,
				      .mode = (uint8_t)mode };
	struct fwi_patch_report rep;
	if (fwi_patch(&second, fam, &in, &rep)) {
		fprintf(stderr, "no patch area in the STUB image\n");
		return 1;
	}
	fprintf(stderr, "images: first %zu bytes, STUB %zu bytes, mode byte 0x%02x; "
			"factory block %zu bytes; %u TLV patched, %u skipped\n",
		first.len, second.len, mode & 0xFF, factory_len, rep.replaced,
		rep.skipped);

	d.uart_path = uart;
	d.uart = open(uart, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (d.uart < 0) {
		fprintf(stderr, "cannot open %s: %s\n", uart, strerror(errno));
		return 1;
	}
	if (dect_fd_arg >= 0 && fcntl(dect_fd_arg, F_GETFD) < 0) {
		fprintf(stderr, "--dect-fd %d: %s\n", dect_fd_arg, strerror(errno));
		return 1;
	}
	d.dect_fd = dect_fd_arg >= 0 ? dect_fd_arg : open(dect_dev, O_RDWR);
	if (d.dect_fd < 0) {
		fprintf(stderr, "cannot open %s: %s (is the opendectcell module loaded?)\n",
			dect_dev, strerror(errno));
		return 1;
	}
	release_dect_fd = d.dect_fd;
	signal(SIGPIPE, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	struct fwl_io io = { .ctx = &d, .read_byte = dev_read_byte,
			     .poll_byte = dev_poll_byte, .write = dev_write,
			     .set_baud = dev_set_baud, .reset = dev_reset,
			     .strap = no_strap ? dev_strap_none : dev_strap,
			     .sleep_ms = dev_sleep, .log = dev_log };
	/* Bit 8 set: the stock order, "slow" (strap low) attempts first. */
	struct fwl_params p = { .mode_word = (mode & 0xFFu) | 0x100u,
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

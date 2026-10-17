// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-pcmcap: bring up the PCM receive path of /dev/dect0 step by step,
 * capture blocks, and search the data for STUB control mails.
 *
 * The mail framing (docs/stub-pcm-control-channel.md): 0xAA, sequence,
 * 16-bit LE length L (<= 0xFB), L + 2 body bytes, 16-bit LE sum of the
 * L + 4 bytes from the length field on. Which slot carries them, and in
 * which byte and bit order, is what the search tries to find out.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "opendectcell.h"

#define MAX_SHOW	3
/* Blocks kept in memory for the analysis (long runs only count). */
#define MAX_KEEP	2500u

struct opts {
	unsigned int last_step;
	int skip_stereo;
	unsigned int wait_ms;
	unsigned int blocks;
	unsigned int hex_frames;
	int quiet;
	const char *out;
	const char *offline;
	const char *exec_cmd;
	struct dect_pcm_config cfg;
};

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-pcmcap [options]\n"
		"  -s STEP  bring-up up to STEP 1..6 (default 6), one step at a time\n"
		"  -k       skip step 5 (receiver on without DMA)\n"
		"  -w MS    below step 6: wait MS ms, then show counters (default 1000)\n"
		"  -c N     slots per frame (default 8)\n"
		"  -r D     RX data delay 0 or 1 (default 1); -t D: TX delay\n"
		"  -f N     frames per DMA block (default 32); -n N: descriptors (4)\n"
		"  -b N     blocks to capture at step 6 (default 250, about 1 s)\n"
		"  -o FILE  write the raw payload to FILE\n"
		"  -x N     hex-dump the first N frames\n"
		"  -q       no register dumps\n"
		"  -e CMD   at step 6, run CMD (sh -c) while capturing; CMD gets the\n"
		"           open device as $DECT_FD (e.g. dect-fwload --dect-fd $DECT_FD)\n"
		"  -S FILE  offline: analyse a raw payload FILE (no device)\n");
}

static int parse_uint(const char *s, unsigned int *v)
{
	char *end;
	unsigned long x;

	errno = 0;
	x = strtoul(s, &end, 0);
	if (errno || *end || end == s || x > 0xffffffffUL)
		return -1;
	*v = (unsigned int)x;
	return 0;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = {
		.tv_sec = (time_t)(ms / 1000u),
		.tv_nsec = (long)(ms % 1000u) * 1000000L,
	};

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* ---- analysis (also offline) ---- */

static uint8_t bitrev8(uint8_t b)
{
	b = (uint8_t)((b & 0xf0u) >> 4 | (b & 0x0fu) << 4);
	b = (uint8_t)((b & 0xccu) >> 2 | (b & 0x33u) << 2);
	b = (uint8_t)((b & 0xaau) >> 1 | (b & 0x55u) << 1);
	return b;
}

/* Re-cut a byte stream at a bit offset (MSB first). */
static void shift_stream(const uint8_t *in, size_t n, unsigned int shift,
			 uint8_t *out)
{
	size_t i;

	for (i = 0; i + 1 < n; i++)
		out[i] = (uint8_t)((unsigned int)in[i] << shift |
				   (unsigned int)in[i + 1] >> (8u - shift));
}

struct hunt {
	unsigned int mails;
	unsigned int in_seq;
	size_t offs[MAX_SHOW];
	uint8_t seq[MAX_SHOW];
	unsigned int len[MAX_SHOW];
};

static void hunt_mails(const uint8_t *s, size_t n, struct hunt *h)
{
	size_t i = 0;
	int last_seq = -1;

	memset(h, 0, sizeof(*h));
	while (i + 10 <= n) {
		unsigned int len, sum = 0, want;
		size_t total, k;

		if (s[i] != 0xaa) {
			i++;
			continue;
		}
		len = s[i + 2] | (unsigned int)s[i + 3] << 8;
		total = 4u + len + 2u + 2u;
		if (len > 0xfb || i + total > n) {
			i++;
			continue;
		}
		for (k = i + 2; k < i + 4 + len + 2; k++)
			sum += s[k];
		want = s[i + total - 2] | (unsigned int)s[i + total - 1] << 8;
		if ((sum & 0xffffu) != want) {
			i++;
			continue;
		}
		if (h->mails < MAX_SHOW) {
			h->offs[h->mails] = i;
			h->seq[h->mails] = s[i + 1];
			h->len[h->mails] = len;
		}
		if (last_seq >= 0 && s[i + 1] == (uint8_t)(last_seq + 1))
			h->in_seq++;
		last_seq = s[i + 1];
		h->mails++;
		i += total;
	}
}

/*
 * Byte stream of one slot (or of all slots, slot < 0) in one of eight
 * variants: bit 0 swaps the two bytes of a word, bit 1 keeps only one
 * byte of the word (the first after the swap), bit 2 reverses the bits.
 */
static size_t make_stream(const uint8_t *data, size_t frames,
			  unsigned int slots, int slot, unsigned int variant,
			  uint8_t *out)
{
	unsigned int first = slot < 0 ? 0 : (unsigned int)slot;
	unsigned int last = slot < 0 ? slots - 1 : (unsigned int)slot;
	size_t f, n = 0;
	unsigned int w;

	for (f = 0; f < frames; f++) {
		for (w = first; w <= last; w++) {
			const uint8_t *p = data + (f * slots + w) * 2u;
			uint8_t a = (variant & 1u) ? p[1] : p[0];
			uint8_t b = (variant & 1u) ? p[0] : p[1];

			if (variant & 4u) {
				a = bitrev8(a);
				b = bitrev8(b);
			}
			out[n++] = a;
			if (!(variant & 2u))
				out[n++] = b;
		}
	}
	return n;
}

static void search(const uint8_t *data, size_t frames, unsigned int slots)
{
	static const char *const vname[8] = {
		"lo,hi", "hi,lo", "lo only", "hi only",
		"lo,hi rev", "hi,lo rev", "lo only rev", "hi only rev",
	};
	size_t cap = frames * slots * 2u;
	uint8_t *s = malloc(cap), *t = malloc(cap);
	unsigned int found = 0, v, shift, k;
	int slot;

	if (!s || !t) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	for (slot = -1; slot < (int)slots; slot++) {
		for (v = 0; v < 8; v++) {
			size_t n;

			if (slot < 0 && (v & 2u))
				continue;
			n = make_stream(data, frames, slots, slot, v, s);
			for (shift = 0; shift < 8; shift++) {
				struct hunt h;
				const uint8_t *src = s;
				size_t len = n;

				if (shift) {
					shift_stream(s, n, shift, t);
					src = t;
					len = n - 1;
				}
				hunt_mails(src, len, &h);
				if (!h.mails)
					continue;
				found++;
				printf("mails: slot %s%d, %s, bit shift %u: %u valid, %u in sequence\n",
				       slot < 0 ? "all" : "", slot < 0 ? 0 : slot,
				       vname[v], shift, h.mails, h.in_seq);
				for (k = 0; k < h.mails && k < MAX_SHOW; k++)
					printf("    at %zu: seq %u len %u\n",
					       h.offs[k], h.seq[k], h.len[k]);
			}
		}
	}
	if (!found)
		printf("mails: no 0xAA-framed mail with a valid checksum in any variant\n");
out:
	free(s);
	free(t);
}

static void analyse(const uint8_t *data, size_t bytes, unsigned int slots,
		    unsigned int hex_frames)
{
	size_t frame_bytes = slots * 2u, frames = bytes / frame_bytes, f;
	unsigned int w, i;

	printf("analysis: %zu bytes, %zu frames of %u slots\n", bytes, frames,
	       slots);
	for (w = 0; w < slots; w++) {
		size_t ff = 0, zero = 0, other = 0;
		unsigned int shown = 0;

		printf("  slot %u:", w);
		for (f = 0; f < frames; f++) {
			const uint8_t *p = data + f * frame_bytes + w * 2u;
			unsigned int x = p[0] | (unsigned int)p[1] << 8;

			if (x == 0xffff) {
				ff++;
			} else if (x == 0) {
				zero++;
			} else {
				other++;
				if (shown < 6) {
					printf(" %04x", x);
					shown++;
				}
			}
		}
		printf("%s  (ffff %zu, 0000 %zu, other %zu)\n",
		       shown ? "" : " -", ff, zero, other);
	}
	for (f = 0; f < frames && f < hex_frames; f++) {
		printf("  %6zu:", f);
		for (i = 0; i < frame_bytes; i++)
			printf("%s%02x", (i & 1u) ? "" : " ",
			       data[f * frame_bytes + i]);
		printf("\n");
	}
	search(data, frames, slots);
}

static int analyse_file(const char *path, const struct opts *o)
{
	FILE *fp = fopen(path, "rb");
	uint8_t *data = NULL;
	size_t cap = 0, n = 0;

	if (!fp) {
		perror(path);
		return 1;
	}
	for (;;) {
		size_t r;

		if (n == cap) {
			uint8_t *d;

			cap = cap ? cap * 2u : 65536u;
			d = realloc(data, cap);
			if (!d) {
				fclose(fp);
				free(data);
				fprintf(stderr, "out of memory\n");
				return 1;
			}
			data = d;
		}
		r = fread(data + n, 1, cap - n, fp);
		if (!r)
			break;
		n += r;
	}
	fclose(fp);
	analyse(data, n, o->cfg.slots ? o->cfg.slots : 8u, o->hex_frames);
	free(data);
	return 0;
}

/* ---- device ---- */

static void dump_regs(int fd)
{
	static const unsigned int adcc_off[17] = {
		0x104, 0x108, 0x10c, 0x124, 0x128, 0x12c,
		0x144, 0x148, 0x14c, 0x164, 0x168, 0x16c,
		0x200, 0x204, 0x208, 0x20c, 0x210,
	};
	static const unsigned int mbox_off[10] = {
		0x10, 0x18, 0x1c, 0x20, 0x24, 0x38, 0x40, 0x44, 0x4c, 0x58,
	};
	struct dect_pcm_regs r;
	unsigned int i;

	memset(&r, 0, sizeof(r));
	if (ioctl(fd, DECT_IOC_PCM_REGS, &r)) {
		perror("PCM_REGS");
		return;
	}
	printf("  regs at step %u (valid %#x)\n  tlmm 20-27:", r.step, r.valid);
	for (i = 0; i < 8; i++)
		printf(" %08x", r.tlmm[i]);
	printf("\n");
	if (r.valid & DECT_PCM_REGS_GLB) {
		printf("  glb: CHIP_CTRL %08x I2S_RST %08x CLK_CTRL %08x TDM_CTRL %08x AUDIO_MODE %08x\n",
		       r.glb[0], r.glb[1], r.glb[2], r.glb[3], r.glb[4]);
		printf("  adcc:");
		for (i = 0; i < 17; i++)
			printf("%s %03x=%08x", (i % 6 || !i) ? "" : "\n       ",
			       adcc_off[i], r.adcc[i]);
		printf("\n");
	}
	if (r.valid & DECT_PCM_REGS_STEREO) {
		printf("  stereo:");
		for (i = 0; i < 11; i++)
			printf(" %02x=%08x", i * 4u, r.stereo[i]);
		printf("\n");
	}
	if (r.valid & DECT_PCM_REGS_MBOX) {
		printf("  mbox:");
		for (i = 0; i < 10; i++)
			printf(" %02x=%08x", mbox_off[i], r.mbox[i]);
		printf("\n");
	}
}

static int get_stats(int fd, struct dect_pcm_stats *s)
{
	memset(s, 0, sizeof(*s));
	if (ioctl(fd, DECT_IOC_PCM_STATS, s)) {
		perror("PCM_STATS");
		return -1;
	}
	return 0;
}

static void print_stats(const struct dect_pcm_stats *s)
{
	printf("  stats: irqs %" PRIu64 " blocks %" PRIu64 " dropped %" PRIu64
	       " multi %" PRIu64 " empty %" PRIu64 " errors %" PRIu64
	       " (last %#x) int_status %#x\n"
	       "         rx samples %08x:%08x overruns %u\n",
	       (uint64_t)s->irqs, (uint64_t)s->blocks, (uint64_t)s->dropped,
	       (uint64_t)s->multi, (uint64_t)s->empty, (uint64_t)s->errors,
	       s->last_error_status, s->int_status, s->rx_samples_msb,
	       s->rx_samples_lsb, s->overruns);
}

static pid_t spawn(const char *cmd, int fd)
{
	char num[16];
	pid_t pid;

	snprintf(num, sizeof(num), "%d", fd);
	fflush(NULL);
	pid = fork();
	if (pid == 0) {
		setenv("DECT_FD", num, 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		perror("fork");
	return pid;
}

static int capture(int fd, const struct opts *o, const struct dect_pcm_config *c)
{
	size_t block = c->slots * 2u * c->frames_per_block;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	size_t bufsize = slot * 16u, total = 0;
	unsigned int keep = o->blocks < MAX_KEEP ? o->blocks : MAX_KEEP;
	uint8_t *buf = malloc(bufsize), *data = malloc(block * keep);
	unsigned int got = 0, gaps = 0, flagged = 0;
	uint64_t first_ts = 0, last_ts = 0;
	uint32_t next_seq = 0;
	FILE *out = NULL;
	pid_t child = -1;
	int ret = 1, status;

	if (!buf || !data) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	if (o->out && !(out = fopen(o->out, "wb"))) {
		perror(o->out);
		goto out;
	}
	if (o->exec_cmd && (child = spawn(o->exec_cmd, fd)) < 0)
		goto out;
	while (got < o->blocks) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		size_t off;
		int pr = poll(&pfd, 1, 1000);

		if (pr < 0 && errno == EINTR)
			continue;
		if (pr == 0 && child > 0) {
			/* The chip gives no clocks while it is being loaded. */
			if (waitpid(child, &status, WNOHANG) == 0)
				continue;
			child = -1;
		}
		if (pr <= 0) {
			fprintf(stderr, "capture: %s after %u blocks\n",
				pr ? strerror(errno) : "no block for 1 s", got);
			goto out;
		}
		n = read(fd, buf, bufsize);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("read");
			goto out;
		}
		for (off = 0; off + slot <= (size_t)n && got < o->blocks;
		     off += slot) {
			const struct dect_pcm_block *h = (const void *)(buf + off);

			if (h->magic != DECT_PCM_BLOCK_MAGIC || h->len != block) {
				fprintf(stderr, "bad block header\n");
				goto out;
			}
			if (got && h->seq != next_seq)
				gaps++;
			if (h->flags)
				flagged++;
			if (!got)
				first_ts = h->ts_ns;
			last_ts = h->ts_ns;
			next_seq = h->seq + 1u;
			if (got < keep) {
				memcpy(data + total, buf + off + sizeof(*h),
				       block);
				total += block;
			}
			got++;
		}
	}
	printf("capture: %u blocks (%zu bytes kept), seq gaps %u, flagged %u",
	       got, total, gaps, flagged);
	if (got > 1 && last_ts > first_ts)
		printf(", %.1f blocks/s",
		       (double)(got - 1u) * 1e9 / (double)(last_ts - first_ts));
	printf("\n");
	if (out && fwrite(data, 1, total, out) != total) {
		perror(o->out);
		goto out;
	}
	analyse(data, total, c->slots, o->hex_frames);
	ret = 0;
out:
	if (child > 0 && waitpid(child, &status, 0) == child)
		printf("command exit status %d\n",
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	if (out && fclose(out))
		ret = 1;
	free(buf);
	free(data);
	return ret;
}

static int run_device(const struct opts *o)
{
	struct dect_ioc_info info;
	struct dect_pcm_config cfg = o->cfg;
	struct dect_pcm_stats s1, s2;
	unsigned int step;
	int fd, ret = 1;

	fd = open("/dev/dect0", O_RDWR);
	if (fd < 0) {
		perror("/dev/dect0");
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, DECT_IOC_INFO, &info) || info.version < 2 ||
	    !(info.features & DECT_FEATURE_PCM_RX)) {
		fprintf(stderr, "module without PCM receive (version %u)\n",
			info.version);
		goto out;
	}
	if (ioctl(fd, DECT_IOC_PCM_CONFIG, &cfg)) {
		perror("PCM_CONFIG");
		goto out;
	}
	printf("config: slots %u rx_delay %u tx_delay %u frames %u desc %u\n",
	       cfg.slots, cfg.rx_delay, cfg.tx_delay, cfg.frames_per_block,
	       cfg.n_desc);

	for (step = 1; step <= o->last_step; step++) {
		if (step == DECT_PCM_STEP_STEREO && o->skip_stereo)
			continue;
		printf("step %u ...\n", step);
		fflush(stdout);
		if (ioctl(fd, DECT_IOC_PCM_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			if (!o->quiet)
				dump_regs(fd);
			goto out;
		}
		if (!o->quiet)
			dump_regs(fd);
	}

	if (o->last_step < DECT_PCM_STEP_DMA) {
		if (o->last_step >= DECT_PCM_STEP_TDM && !get_stats(fd, &s1)) {
			sleep_ms(o->wait_ms);
			if (!get_stats(fd, &s2)) {
				print_stats(&s2);
				printf("  rx samples +%u in %u ms\n",
				       s2.rx_samples_lsb - s1.rx_samples_lsb,
				       o->wait_ms);
			}
		}
		if (!o->quiet)
			dump_regs(fd);
		ret = 0;
	} else {
		ret = capture(fd, o, &cfg);
		if (!get_stats(fd, &s2))
			print_stats(&s2);
		if (!o->quiet)
			dump_regs(fd);
	}
	step = DECT_PCM_STEP_OFF;
	if (ioctl(fd, DECT_IOC_PCM_STEP, &step)) {
		perror("PCM_STEP 0");
		ret = 1;
	}
out:
	close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	struct opts o;
	unsigned int v;
	int c;

	memset(&o, 0, sizeof(o));
	o.last_step = DECT_PCM_STEP_DMA;
	o.wait_ms = 1000;
	o.blocks = 250;
	while ((c = getopt(argc, argv, "s:kw:c:r:t:f:n:b:o:x:qS:e:h")) != -1) {
		switch (c) {
		case 's':
			if (parse_uint(optarg, &o.last_step) || !o.last_step ||
			    o.last_step > DECT_PCM_STEP_DMA)
				goto bad;
			break;
		case 'k':
			o.skip_stereo = 1;
			break;
		case 'w':
			if (parse_uint(optarg, &o.wait_ms))
				goto bad;
			break;
		case 'c':
			if (parse_uint(optarg, &o.cfg.slots) || !o.cfg.slots ||
			    o.cfg.slots > 16)
				goto bad;
			break;
		case 'r':
			if (parse_uint(optarg, &v) || v > 1)
				goto bad;
			o.cfg.rx_delay = v;
			o.cfg.flags |= DECT_PCM_CF_RX_DELAY_SET;
			break;
		case 't':
			if (parse_uint(optarg, &v) || v > 1)
				goto bad;
			o.cfg.tx_delay = v;
			o.cfg.flags |= DECT_PCM_CF_TX_DELAY_SET;
			break;
		case 'f':
			if (parse_uint(optarg, &o.cfg.frames_per_block))
				goto bad;
			break;
		case 'n':
			if (parse_uint(optarg, &o.cfg.n_desc))
				goto bad;
			break;
		case 'b':
			if (parse_uint(optarg, &o.blocks) || !o.blocks ||
			    o.blocks > 1000000u)
				goto bad;
			break;
		case 'o':
			o.out = optarg;
			break;
		case 'x':
			if (parse_uint(optarg, &o.hex_frames))
				goto bad;
			break;
		case 'q':
			o.quiet = 1;
			break;
		case 'S':
			o.offline = optarg;
			break;
		case 'e':
			o.exec_cmd = optarg;
			break;
		default:
			goto bad;
		}
	}
	if (optind != argc)
		goto bad;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (o.offline)
		return analyse_file(o.offline, &o);
	return run_device(&o);
bad:
	usage();
	return 2;
}

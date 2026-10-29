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
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "opendectcell.h"
#include "dect_audio.h"
#include "opendectcell_version.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
	const char *tx_hex;
	const char *mail[16];
	unsigned int n_mail;
	int show_params;
	int tone;
	int gen_slot;		/* -1 = off: transmit a tone in this slot */
	unsigned int gen_freq;
	unsigned int ctrl_slot;
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
		"  -T       also run the transmit DMA (drives pin 27)\n"
		"  -m SRC:DST:PRIM[:HEX]  send a mail in the control slot after the\n"
		"           DMA starts (implies -T), e.g. -m 0x22:0x02:0xf600; repeat\n"
		"           -m to send several in order (e.g. contact then connect)\n"
		"  -A       per-slot tone analysis (A-law low byte: RMS and frequency)\n"
		"  -G SLOT:FREQ  transmit an A-law sine of FREQ Hz in SLOT while\n"
		"           capturing (for the dect_loop loopback test; implies -T)\n"
		"  -p       show decoded mail parameter bytes (may hold identity data)\n"
		"  -l SLOT  control slot for -m and the mail decoder (default 1)\n"
		"  -P HEX   transmit frame template, slots*2 bytes in DMA-buffer\n"
		"           order (default all ff)\n"
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

/*
 * Is there a STUB -> host mail at s[i]? (docs/stub-pcm-control-channel.md:
 * 0xAA, seq, length L, body of L + 2 bytes, 16-bit sum of length and body,
 * all little-endian.) Returns its total length with *len = L, or 0. Unlike
 * dect_rx_pull() this looks at one position of a buffer, so the callers can
 * say where each mail sits in the capture.
 */
static size_t mail_at(const uint8_t *s, size_t n, size_t i, unsigned int *len)
{
	unsigned int sum = 0, want;
	size_t total, k;

	if (i + 10 > n || s[i] != 0xaa)
		return 0;
	*len = s[i + 2] | (unsigned int)s[i + 3] << 8;
	total = 4u + *len + 2u + 2u;
	if (*len > 0xfb || i + total > n)
		return 0;
	for (k = i + 2; k < i + total - 2u; k++)
		sum += s[k];
	want = s[i + total - 2] | (unsigned int)s[i + total - 1] << 8;
	return (sum & 0xffffu) == want ? total : 0;
}

static void hunt_mails(const uint8_t *s, size_t n, struct hunt *h)
{
	size_t i = 0;
	int last_seq = -1;

	memset(h, 0, sizeof(*h));
	while (i + 10 <= n) {
		unsigned int len;
		size_t total = mail_at(s, n, i, &len);

		if (!total) {
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

/*
 * STUB -> host mails in the control slot, two bytes per frame in buffer
 * order: see mail_at().
 */
static void print_mails(const uint8_t *data, size_t frames,
			unsigned int slots, unsigned int slot, int show_params)
{
	uint8_t *s = malloc(frames * 2u);
	size_t f, i = 0, n = frames * 2u;
	unsigned int count = 0;

	if (!s || slot >= slots) {
		free(s);
		return;
	}
	for (f = 0; f < frames; f++)
		memcpy(s + 2u * f, data + (f * slots + slot) * 2u, 2);
	while (i + 10 <= n) {
		unsigned int len, k;
		size_t total = mail_at(s, n, i, &len);

		if (!total) {
			i++;
			continue;
		}
		printf("  mail at frame %zu: seq %3u len %3u src %02x dst %02x prim %04x",
		       i / 2u, s[i + 1], len, s[i + 4], s[i + 5],
		       s[i + 6] | (unsigned int)s[i + 7] << 8);
		if (len > 2 && !show_params) {
			printf(" [params hidden]");
		} else if (len > 2) {
			printf(" par");
			for (k = 0; k + 2 < len && k < 24; k++)
				printf(" %02x", s[i + 8 + k]);
		}
		printf("\n");
		count++;
		i += total;
	}
	printf("mails in slot %u: %u\n", slot, count);
	free(s);
}

/*
 * Treat the low byte of each slot's word as an 8-bit A-law sample, decode
 * it and report energy and the zero-crossing frequency. A constant or idle
 * slot shows rms ~ 0; a STUB test tone shows a clear frequency.
 */
static void tone_report(const uint8_t *data, size_t frames, unsigned int slots)
{
	int16_t *a = malloc(frames * sizeof(*a));
	int16_t *l = malloc(frames * sizeof(*l));
	unsigned int w;
	size_t f;

	if (!a || !l) {
		fprintf(stderr, "tone: out of memory\n");
		free(a);
		free(l);
		return;
	}
	/*
	 * Each slot is tried two ways: as an 8-bit A-law sample in the low
	 * byte, and as a 16-bit little-endian linear sample. A real tone shows
	 * a stable frequency and energy in whichever interpretation is right.
	 */
	printf("tone (%u Hz) -- alaw(low byte) and lin16(word):\n",
	       DECT_AUDIO_RATE_HZ);
	for (w = 0; w < slots; w++) {
		struct dect_tone_stats sa, sl;
		unsigned int wmin = 0xffffu, wmax = 0;

		for (f = 0; f < frames; f++) {
			const uint8_t *p = data + (f * slots + w) * 2u;
			unsigned int word = p[0] | (unsigned int)p[1] << 8;

			if (word < wmin)
				wmin = word;
			if (word > wmax)
				wmax = word;
			a[f] = dect_alaw_decode(p[0]);
			l[f] = (int16_t)word;
		}
		dect_tone_analyze(a, frames, DECT_AUDIO_RATE_HZ, &sa);
		dect_tone_analyze(l, frames, DECT_AUDIO_RATE_HZ, &sl);
		printf("  slot %u word %04x..%04x | alaw rms %7.1f freq %6.1f | lin16 rms %8.1f freq %6.1f\n",
		       w, wmin, wmax, sa.rms, sa.freq_hz, sl.rms, sl.freq_hz);
	}
	free(a);
	free(l);
}

static void analyse(const uint8_t *data, size_t bytes, unsigned int slots,
		    unsigned int hex_frames, int tone)
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
	if (tone)
		tone_report(data, frames, slots);
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
	analyse(data, n, o->cfg.slots ? o->cfg.slots : 8u, o->hex_frames,
		o->tone);
	print_mails(data, n / ((o->cfg.slots ? o->cfg.slots : 8u) * 2u),
		    o->cfg.slots ? o->cfg.slots : 8u, o->ctrl_slot, o->show_params);
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
	       "         rx samples %08x:%08x overruns %u tx irqs %u blocks %u\n",
	       (uint64_t)s->irqs, (uint64_t)s->blocks, (uint64_t)s->dropped,
	       (uint64_t)s->multi, (uint64_t)s->empty, (uint64_t)s->errors,
	       s->last_error_status, s->int_status, s->rx_samples_msb,
	       s->rx_samples_lsb, s->overruns, s->tx_irqs, s->tx_blocks);
}

/*
 * Host -> STUB element (docs/stub-pcm-control-channel.md): word count
 * (n + 5) / 2, sequence, the n mail bytes [L, src, dst, primitive,
 * parameters] with L = 2 + parameters, 16-bit sum of the mail bytes,
 * padded with 0xFF; two bytes per frame into the control slot.
 */
static int send_mail(int fd, const char *spec, unsigned int slots,
		     unsigned int slot, unsigned int seq)
{
	unsigned int src, dst, prim, v, i, n, sum = 0;
	uint8_t mail[64], el[80], *frames;
	const char *par = NULL;
	size_t fb = slots * 2u, len, nf, k;
	char *end;
	ssize_t w;

	errno = 0;
	src = (unsigned int)strtoul(spec, &end, 0);
	if (*end != ':')
		return -1;
	dst = (unsigned int)strtoul(end + 1, &end, 0);
	if (*end != ':')
		return -1;
	prim = (unsigned int)strtoul(end + 1, &end, 0);
	if (*end == ':')
		par = end + 1;
	else if (*end)
		return -1;
	if (src > 0xff || dst > 0xff || prim > 0xffff || slot >= slots)
		return -1;

	n = 6;
	mail[2] = (uint8_t)src;
	mail[3] = (uint8_t)dst;
	mail[4] = (uint8_t)prim;
	mail[5] = (uint8_t)(prim >> 8);
	for (; par && *par && n < sizeof(mail); par += 2) {
		if (sscanf(par, "%2x", &v) != 1)
			return -1;
		mail[n++] = (uint8_t)v;
	}
	mail[0] = (uint8_t)(n - 4);
	mail[1] = 0;

	len = 0;
	el[len++] = (uint8_t)((n + 5) / 2);
	el[len++] = (uint8_t)(((n + 5) / 2) >> 8);
	el[len++] = (uint8_t)seq;
	el[len++] = (uint8_t)(seq >> 8);
	for (i = 0; i < n; i++) {
		el[len++] = mail[i];
		sum += mail[i];
	}
	el[len++] = (uint8_t)sum;
	el[len++] = (uint8_t)(sum >> 8);
	if (len & 1)
		el[len++] = 0xff;

	nf = len / 2;
	frames = malloc(nf * fb);
	if (!frames)
		return -1;
	memset(frames, 0xff, nf * fb);
	for (k = 0; k < nf; k++)
		memcpy(frames + k * fb + slot * 2u, el + 2 * k, 2);
	w = write(fd, frames, nf * fb);
	free(frames);
	printf("sent mail src %02x dst %02x prim %04x (%u bytes) in slot %u: %s\n",
	       src, dst, prim, n, slot, w == (ssize_t)(nf * fb) ? "ok" :
	       strerror(errno));
	return w == (ssize_t)(nf * fb) ? 0 : -1;
}

/*
 * Child process that transmits a continuous A-law sine of `freq` Hz in one
 * voice slot (low byte), idle elsewhere, for the dect_loop loopback test.
 * The parent captures and kills it when done.
 */
static pid_t spawn_tone(int fd, unsigned int slots, unsigned int slot,
			unsigned int freq)
{
	pid_t pid;

	fflush(NULL);
	pid = fork();
	if (pid == 0) {
		size_t fb = slots * 2u, batch = 800u;	/* ~0.1 s */
		uint8_t *frames = malloc(batch * fb);
		double phase = 0.0;
		double dp = 2.0 * M_PI * (double)freq / (double)DECT_AUDIO_RATE_HZ;
		unsigned int i;

		if (!frames)
			_exit(1);
		for (;;) {
			for (i = 0; i < batch; i++) {
				int16_t s = (int16_t)(20000.0 * sin(phase));

				memset(frames + i * fb, 0xff, fb);
				frames[i * fb + slot * 2u] = dect_alaw_encode(s);
				frames[i * fb + slot * 2u + 1u] = 0;
				phase += dp;
				if (phase > 2.0 * M_PI)
					phase -= 2.0 * M_PI;
			}
			if (write(fd, frames, batch * fb) != (ssize_t)(batch * fb))
				break;
		}
		free(frames);
		_exit(0);
	}
	if (pid < 0)
		perror("fork");
	return pid;
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
	pid_t child = -1, tone_child = -1;
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
	for (unsigned int mi = 0; mi < o->n_mail; mi++) {
		if (send_mail(fd, o->mail[mi], c->slots, o->ctrl_slot, mi)) {
			fprintf(stderr, "-m: bad mail or write failed\n");
			goto out;
		}
		if (mi + 1 < o->n_mail)
			sleep_ms(100);	/* let the STUB act on the prior mail */
	}
	if (o->gen_slot >= 0) {
		sleep_ms(100);		/* let the mail frames drain first */
		tone_child = spawn_tone(fd, c->slots, (unsigned int)o->gen_slot,
					o->gen_freq);
		if (tone_child < 0)
			goto out;
	}
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
	analyse(data, total, c->slots, o->hex_frames, o->tone);
	print_mails(data, total / (c->slots * 2u), c->slots, o->ctrl_slot,
		    o->show_params);
	ret = 0;
out:
	if (tone_child > 0) {
		kill(tone_child, SIGTERM);
		waitpid(tone_child, &status, 0);
	}
	if (child > 0 && waitpid(child, &status, 0) == child)
		printf("command exit status %d\n",
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	if (out && fclose(out))
		ret = 1;
	free(buf);
	free(data);
	return ret;
}

static int set_tx_frame(int fd, const char *hex, unsigned int slots)
{
	struct dect_pcm_tx_frame f;
	unsigned int i, v;

	memset(&f, 0, sizeof(f));
	f.len = slots * 2u;
	if (strlen(hex) != f.len * 2u) {
		fprintf(stderr, "-P needs %u hex digits\n", f.len * 2u);
		return -1;
	}
	for (i = 0; i < f.len; i++) {
		if (sscanf(hex + 2u * i, "%2x", &v) != 1) {
			fprintf(stderr, "-P: bad hex\n");
			return -1;
		}
		f.frame[i] = (uint8_t)v;
	}
	if (ioctl(fd, DECT_IOC_PCM_TX_FRAME, &f)) {
		perror("PCM_TX_FRAME");
		return -1;
	}
	return 0;
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
	printf("config: slots %u rx_delay %u tx_delay %u frames %u desc %u%s\n",
	       cfg.slots, cfg.rx_delay, cfg.tx_delay, cfg.frames_per_block,
	       cfg.n_desc, (cfg.flags & DECT_PCM_CF_TX) ? " tx" : "");
	if (o->tx_hex && set_tx_frame(fd, o->tx_hex, cfg.slots))
		goto out;

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

	if (argc >= 2 && !strcmp(argv[1], "--version")) {
		printf("dect-pcmcap %s\n", OPENDECTCELL_VERSION);
		return 0;
	}
	memset(&o, 0, sizeof(o));
	o.last_step = DECT_PCM_STEP_DMA;
	o.wait_ms = 1000;
	o.blocks = 250;
	o.ctrl_slot = 1;
	o.gen_slot = -1;
	while ((c = getopt(argc, argv, "s:kw:c:r:t:f:n:b:o:x:qS:e:TP:m:AG:pl:h")) != -1) {
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
		case 'T':
			o.cfg.flags |= DECT_PCM_CF_TX;
			break;
		case 'P':
			o.tx_hex = optarg;
			break;
		case 'm':
			if (o.n_mail >= sizeof(o.mail) / sizeof(o.mail[0]))
				goto bad;
			o.mail[o.n_mail++] = optarg;
			o.cfg.flags |= DECT_PCM_CF_TX;
			break;
		case 'A':
			o.tone = 1;
			break;
		case 'G': {
			char *end;
			unsigned long sl = strtoul(optarg, &end, 0);
			unsigned long fr;

			if (*end != ':')
				goto bad;
			fr = strtoul(end + 1, &end, 0);
			if (*end || sl > 15 || !fr || fr > 4000)
				goto bad;
			o.gen_slot = (int)sl;
			o.gen_freq = (unsigned int)fr;
			o.cfg.flags |= DECT_PCM_CF_TX;
			break;
		}
		case 'p':
			o.show_params = 1;
			break;
		case 'l':
			if (parse_uint(optarg, &o.ctrl_slot) || o.ctrl_slot > 15)
				goto bad;
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

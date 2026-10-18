// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd: keep the STUB control channel up on /dev/dect0.
 *
 * It brings up the PCM receive and transmit path step by step (the same
 * six DECT_IOC_PCM_STEP bring-up as dect-pcmcap), finds the control slot,
 * sends the 0xF600 contact mail and then maintains the link: it decodes
 * the STUB's 0xAA-framed mails (libopendectcell/dect_mail.h), runs the contact
 * and liveness state machine (libopendectcell/dect_session.h), and re-contacts
 * the STUB when it goes quiet.
 *
 * The control slot is not at a fixed position: after a STUB restart the
 * DMA buffers can come out rotated (docs/stub-pcm-control-channel.md), so
 * the slot is found by probing, not hard-coded. Transmit and receive may
 * be rotated independently, so the transmit slot (the one the STUB answers
 * in) and the receive slot (where the answer appears) are found separately.
 *
 * This is the only part of the control channel with device I/O; the
 * framing and the state machine are platform-free and unit-tested on the PC.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "opendectcell.h"
#include "dect_mail.h"
#include "dect_session.h"
#include "dect_proto.h"
#include "dect_cs.h"
#include "dect_lc.h"
#include "dect_lapc.h"
#include "dect_mm.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u
#define POLL_MS		100

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static uint64_t mono_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

struct stubd {
	int fd;
	unsigned int slots;
	int tx_slot;		/* -1 = unknown */
	int rx_slot;		/* -1 = unknown */
	uint16_t tx_seq;
	int verbose;
	int show_params;	/* log parameter bytes (may hold identity data) */
	const char *xmail[20];	/* extra mails to send once the session is up */
	unsigned int n_xmail;
	unsigned int xmail_idx;	/* next extra mail to send (one per loop tick) */
	struct dect_rx rx;
	struct dect_session sess;

	/* GAP registration (-g): the host-side C-plane stack per connection. */
	int do_register;
	const char *pin;	/* registration PIN / AC */
	int rand_fd;		/* /dev/urandom, -1 if unavailable */
	uint8_t rfpi[DECT_MM_RFPI_LEN];
	int have_rfpi;
	struct reg_ctx {
		int active;
		uint8_t handle;
		struct dect_lc_rx lc;
		struct dect_lapc lapc;
		struct dect_mm mm;
	} reg;
};

/*
 * Log a mail. Parameter bytes are hidden by default: the 0xF601/0xF602
 * answers carry box identity (RFPI-like) data that must not leak into
 * shared logs or issues. Pass -p to show them for local debugging only.
 */
static void log_mail(const struct dect_mail *m, const char *dir, int show_params)
{
	unsigned int i;

	printf("%s src %02x dst %02x prim %04x len %u", dir, m->src, m->dst,
	       m->prim, m->param_len);
	if (!show_params) {
		printf("%s\n", m->param_len ? " [params hidden]" : "");
		return;
	}
	for (i = 0; i < m->param_len && i < 24u; i++)
		printf(" %02x", m->param[i]);
	if (m->param_len > 24u)
		printf(" ...");
	printf("\n");
}

/*
 * Send a mail: encode the transmit element (libopendectcell) and spread its two
 * bytes per frame into the transmit control slot, idle (0xFF) elsewhere.
 */
static int send_mail(struct stubd *s, unsigned int tx_slot,
		     const struct dect_mail *m)
{
	uint8_t el[DECT_TX_ELEM_MAX], *frames;
	size_t fb = s->slots * 2u, nf, k;
	int el_len = dect_mail_encode(m, s->tx_seq, el, sizeof(el));
	ssize_t w;

	if (el_len <= 0 || tx_slot >= s->slots)
		return -1;
	nf = (size_t)el_len / 2u;
	frames = malloc(nf * fb);
	if (!frames)
		return -1;
	memset(frames, 0xff, nf * fb);
	for (k = 0; k < nf; k++)
		memcpy(frames + k * fb + tx_slot * 2u, el + 2u * k, 2);
	w = write(s->fd, frames, nf * fb);
	free(frames);
	if (w != (ssize_t)(nf * fb))
		return -1;
	s->tx_seq++;
	return 0;
}

/* Parse "SRC:DST:PRIM[:HEX]" and send it as a mail in the control slot. */
static int send_mail_spec(struct stubd *s, const char *spec)
{
	struct dect_mail m;
	unsigned long src, dst, prim;
	const char *par = NULL;
	char *end;

	memset(&m, 0, sizeof(m));
	src = strtoul(spec, &end, 0);
	if (*end != ':')
		return -1;
	dst = strtoul(end + 1, &end, 0);
	if (*end != ':')
		return -1;
	prim = strtoul(end + 1, &end, 0);
	if (*end == ':')
		par = end + 1;
	else if (*end)
		return -1;
	if (src > 0xff || dst > 0xff || prim > 0xffff)
		return -1;
	m.src = (uint8_t)src;
	m.dst = (uint8_t)dst;
	m.prim = (uint16_t)prim;
	for (; par && *par; par += 2) {
		unsigned int v;

		if (sscanf(par, "%2x", &v) != 1 ||
		    m.param_len >= DECT_MAIL_PARAM_MAX)
			return -1;
		m.param[m.param_len++] = (uint8_t)v;
	}
	if (send_mail(s, (unsigned int)s->tx_slot, &m))
		return -1;
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	return 0;
}

/* Read up to max_blocks of raw frame data into buf (block payload only). */
static ssize_t read_frames(int fd, unsigned int slots, unsigned int fpb,
			   unsigned int max_blocks, uint8_t *buf, size_t cap,
			   unsigned int timeout_ms)
{
	size_t block = slots * 2u * fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *tmp = malloc(slot * 16u);
	size_t got = 0;
	unsigned int blocks = 0;

	if (!tmp)
		return -1;
	while (blocks < max_blocks && !stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		size_t off;
		int pr = poll(&pfd, 1, (int)timeout_ms);

		if (pr < 0 && errno == EINTR)
			break;
		if (pr <= 0)
			break;
		n = read(fd, tmp, slot * 16u);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			free(tmp);
			return -1;
		}
		for (off = 0; off + slot <= (size_t)n && blocks < max_blocks;
		     off += slot) {
			const struct dect_pcm_block *h = (const void *)(tmp + off);

			if (h->magic != DECT_PCM_BLOCK_MAGIC || h->len != block)
				continue;
			if (got + block > cap)
				goto done;
			memcpy(buf + got, tmp + off + sizeof(*h), block);
			got += block;
			blocks++;
		}
	}
done:
	free(tmp);
	return (ssize_t)got;
}

/* Does the byte stream of one slot in frame data hold a valid 0xAA mail? */
static int slot_has_mail(const uint8_t *data, size_t frames, unsigned int slots,
			 unsigned int slot, struct dect_mail *out)
{
	struct dect_rx rx;
	uint8_t pair[2];
	size_t f;

	dect_rx_init(&rx);
	for (f = 0; f < frames; f++) {
		memcpy(pair, data + (f * slots + slot) * 2u, 2);
		dect_rx_push(&rx, pair, 2);
	}
	return dect_rx_pull(&rx, out);
}

/*
 * Find the control slot pair by probing: send the contact in each transmit
 * slot, then look for a valid STUB answer in any receive slot. Safe because
 * the STUB ignores the contact in every slot but its control slot
 * (docs/stub-pcm-control-channel.md). Sets s->tx_slot and s->rx_slot.
 */
static int find_control_slot(struct stubd *s, unsigned int fpb)
{
	size_t block = s->slots * 2u * fpb;
	size_t cap = block * PROBE_BLOCKS;
	uint8_t *data = malloc(cap);
	unsigned int tx, rx;
	int ret = -1;

	if (!data)
		return -1;
	for (tx = 0; tx < s->slots && !stop; tx++) {
		struct dect_mail cm, ans;
		ssize_t n;
		size_t frames;

		dect_session_contact_mail(&s->sess, &cm);
		if (send_mail(s, tx, &cm)) {
			if (s->verbose)
				fprintf(stderr, "probe: send in slot %u failed\n", tx);
			continue;
		}
		n = read_frames(s->fd, s->slots, fpb, PROBE_BLOCKS, data, cap, 500);
		if (n <= 0)
			continue;
		frames = (size_t)n / (s->slots * 2u);
		for (rx = 0; rx < s->slots; rx++) {
			if (!slot_has_mail(data, frames, s->slots, rx, &ans))
				continue;
			s->tx_slot = (int)tx;
			s->rx_slot = (int)rx;
			printf("control slot found: tx %u, rx %u (answer prim %04x)\n",
			       tx, rx, ans.prim);
			ret = 0;
			goto out;
		}
	}
out:
	free(data);
	return ret;
}

static int bring_up(struct stubd *s, struct dect_pcm_config *cfg)
{
	struct dect_ioc_info info;
	unsigned int step;

	memset(&info, 0, sizeof(info));
	if (ioctl(s->fd, DECT_IOC_INFO, &info) || info.version < 2 ||
	    !(info.features & DECT_FEATURE_PCM_RX)) {
		fprintf(stderr, "module without PCM receive (version %u)\n",
			info.version);
		return -1;
	}
	cfg->flags |= DECT_PCM_CF_TX;
	if (ioctl(s->fd, DECT_IOC_PCM_CONFIG, cfg)) {
		perror("PCM_CONFIG");
		return -1;
	}
	s->slots = cfg->slots;
	printf("config: slots %u rx_delay %u tx_delay %u frames %u desc %u tx\n",
	       cfg->slots, cfg->rx_delay, cfg->tx_delay, cfg->frames_per_block,
	       cfg->n_desc);
	for (step = 1; step <= DECT_PCM_STEP_DMA; step++) {
		if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void tear_down(struct stubd *s)
{
	unsigned int step = DECT_PCM_STEP_OFF;

	if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step))
		perror("PCM_STEP 0");
}

/* ---- GAP registration: drive the host C-plane stack over the STUB ---- */

/* Fill buf with n random bytes (urandom, with a weak clock fallback). */
static void gen_rand(struct stubd *s, uint8_t *buf, size_t n)
{
	if (s->rand_fd >= 0 && read(s->rand_fd, buf, n) == (ssize_t)n)
		return;
	{
		uint64_t x = mono_ms() * 0x9E3779B97F4A7C15ull;
		size_t i;

		for (i = 0; i < n; i++) {
			x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
			buf[i] = (uint8_t)x;
		}
	}
}

/* Wrap a LAPC frame in an Lc Cs frame and send it (APPEND_CONNECTION_DATA +
 * TRIGGER_SIGNALLING). LSIG = 0: the PP has no assigned PMID during registration. */
static void send_cplane(struct stubd *s, uint8_t handle,
			const uint8_t *lapc, size_t lapc_len)
{
	uint8_t cs[DECT_LC_FRAME_MAX];
	struct dect_mail m;
	size_t flen = dect_lc_build(lapc, lapc_len, 0, cs, sizeof(cs));

	if (flen == 0)
		return;
	if (dect_cs_append_mail(&m, DECT_TASK_PCM, handle, cs, flen) == 0)
		send_mail(s, (unsigned int)s->tx_slot, &m);
	dect_cs_trigger_mail(&m, DECT_TASK_PCM, handle);
	send_mail(s, (unsigned int)s->tx_slot, &m);
}

/* Flush any LAPC frames the FSM wants to transmit. */
static void pump_lapc(struct stubd *s)
{
	uint8_t frame[DECT_LC_LAPC_MAX];
	size_t n;

	while ((n = dect_lapc_poll_tx(&s->reg.lapc, frame, sizeof(frame))) > 0)
		send_cplane(s, s->reg.handle, frame, n);
}

static void reg_start(struct stubd *s, uint8_t handle)
{
	struct dect_mm_cfg mcfg;

	memset(&mcfg, 0, sizeof(mcfg));
	if (s->pin)
		snprintf(mcfg.pin, sizeof(mcfg.pin), "%s", s->pin);
	mcfg.authenticate = 1;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);

	s->reg.active = 1;
	s->reg.handle = handle;
	dect_lc_rx_init(&s->reg.lc, 0);
	dect_lapc_init(&s->reg.lapc, 0);
	dect_mm_init(&s->reg.mm, &mcfg);
	printf("reg: connection up (handle %u), C-plane armed\n", handle);
}

/* Feed one 0x0203 signalling segment up the stack; send any responses. */
static void reg_segment(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	struct dect_cs_seg seg;
	uint8_t lapcbuf[DECT_LC_LAPC_MAX], nwk[DECT_LAPC_MSG_MAX];
	struct dect_lapc_frame f;
	int lapc_len, nwk_len;

	if (dect_cs_decode_segment(m, &seg) < 0)
		return;
	lapc_len = dect_lc_rx_fragment(&s->reg.lc, seg.data, lapcbuf, sizeof(lapcbuf));
	if (lapc_len <= 0)
		return;
	if (dect_lapc_parse(lapcbuf, (size_t)lapc_len, &f) < 0)
		return;
	nwk_len = dect_lapc_rx(&s->reg.lapc, &f, now, nwk, sizeof(nwk));
	pump_lapc(s);				/* at least the LAPC ack */
	if (nwk_len > 0) {
		uint8_t rand_f[8], mmsg[DECT_LAPC_MSG_MAX];
		size_t mlen;

		gen_rand(s, rand_f, sizeof(rand_f));
		dect_mm_rx(&s->reg.mm, nwk, (size_t)nwk_len, rand_f);
		mlen = dect_mm_poll(&s->reg.mm, mmsg, sizeof(mmsg));
		if (mlen > 0 && dect_lapc_send(&s->reg.lapc, mmsg, mlen, now) == 0)
			pump_lapc(s);
		if (s->reg.mm.state == DECT_MM_REGISTERED)
			printf("reg: handset registered\n");
		else if (s->reg.mm.state == DECT_MM_FAILED)
			printf("reg: registration rejected\n");
	}
}

/* Route a received mail into the registration stack (when -g). */
static void handle_reg_mail(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	switch (m->prim) {
	case DECT_PRIM_CONTACT_ACK1:	/* 0xF601 carries the base RFPI */
		if (!s->have_rfpi && m->param_len >= DECT_MM_RFPI_LEN) {
			memcpy(s->rfpi, m->param, DECT_MM_RFPI_LEN);
			s->have_rfpi = 1;
		}
		break;
	case DECT_PRIM_CONN_UP:
		reg_start(s, m->param_len ? m->param[0] : 0);
		break;
	case DECT_PRIM_CONN_DOWN:
		if (s->reg.active)
			printf("reg: connection released\n");
		s->reg.active = 0;
		break;
	case DECT_PRIM_SIGNALLING:
		if (s->reg.active)
			reg_segment(s, m, now);
		break;
	default:
		break;
	}
}

/* Main loop: decode mails, drive the session, (re)send the contact. */
static int run_loop(struct stubd *s, unsigned int fpb, unsigned int run_seconds)
{
	size_t block = s->slots * 2u * fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *buf = malloc(slot * 16u);
	uint64_t start = mono_ms();

	if (!buf)
		return -1;
	while (!stop) {
		struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
		uint64_t now;
		int pr = poll(&pfd, 1, POLL_MS);

		if (pr < 0 && errno == EINTR)
			continue;
		if (pr > 0) {
			ssize_t n = read(s->fd, buf, slot * 16u);
			size_t off;

			if (n < 0 && errno != EINTR && errno != EAGAIN) {
				perror("read");
				break;
			}
			for (off = 0; n > 0 && off + slot <= (size_t)n;
			     off += slot) {
				const struct dect_pcm_block *h =
					(const void *)(buf + off);
				const uint8_t *fr = buf + off + sizeof(*h);
				struct dect_mail m;
				unsigned int f;

				if (h->magic != DECT_PCM_BLOCK_MAGIC)
					continue;
				if (h->flags & DECT_PCM_BF_DROPPED)
					fprintf(stderr, "warning: dropped blocks before seq %u\n",
						h->seq);
				for (f = 0; f < fpb; f++) {
					uint8_t pair[2];

					memcpy(pair, fr + (f * s->slots +
						(unsigned int)s->rx_slot) * 2u, 2);
					dect_rx_push(&s->rx, pair, 2);
				}
				now = mono_ms();
				while (dect_rx_pull(&s->rx, &m)) {
					if (s->verbose)
						log_mail(&m, "rx  ", s->show_params);
					dect_session_rx(&s->sess, &m, now);
					if (s->do_register)
						handle_reg_mail(s, &m, now);
				}
			}
		}

		now = mono_ms();
		if (dect_session_tick(&s->sess, now) == DECT_ACT_SEND_CONTACT) {
			struct dect_mail cm;

			dect_session_contact_mail(&s->sess, &cm);
			if (send_mail(s, (unsigned int)s->tx_slot, &cm))
				fprintf(stderr, "contact send failed\n");
			else if (s->verbose)
				log_mail(&cm, "tx  ", s->show_params);
		}
		/* Once up, send the extra mails in order, one per tick (~100 ms). */
		if (s->sess.state == DECT_SESS_UP && s->xmail_idx < s->n_xmail) {
			if (send_mail_spec(s, s->xmail[s->xmail_idx]))
				fprintf(stderr, "extra mail %u (%s) failed\n",
					s->xmail_idx, s->xmail[s->xmail_idx]);
			s->xmail_idx++;
			if (s->xmail_idx == s->n_xmail)
				printf("all %u extra mails sent\n", s->n_xmail);
		}
		/* Drive LAPC retransmission timers for the registration link. */
		if (s->do_register && s->reg.active) {
			dect_lapc_tick(&s->reg.lapc, now);
			pump_lapc(s);
		}
		if (run_seconds && now - start >= (uint64_t)run_seconds * 1000u)
			break;
	}
	printf("session: state %d contacts %u answers %u status %u deaths %u\n",
	       s->sess.state, s->sess.stats.contacts_sent, s->sess.stats.answers,
	       s->sess.stats.status_mails, s->sess.stats.deaths);
	printf("rx: mails %u seq_gaps %u csum_errors %u framing_errors %u resyncs %u\n",
	       s->rx.stats.mails, s->rx.stats.seq_gaps, s->rx.stats.csum_errors,
	       s->rx.stats.framing_errors, s->rx.stats.resyncs);
	free(buf);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dect-stubd [options]\n"
		"  -d DEV   device (default /dev/dect0)\n"
		"  -c N     slots per frame (default 8)\n"
		"  -T SLOT  transmit control slot (skip probing)\n"
		"  -R SLOT  receive control slot (skip probing)\n"
		"  -t SECS  run for SECS seconds then exit (default: until signal)\n"
		"  -v       log every mail (prim and length; parameters hidden)\n"
		"  -p       also log parameter bytes (may hold identity data; do\n"
		"           not share such logs) -- implies -v\n"
		"  -M SRC:DST:PRIM[:HEX]  after contact, send this mail (repeatable,\n"
		"           one per ~100 ms; e.g. the MAC bring-up sequence)\n"
		"  -g       GAP registration mode: bring up the radio + open\n"
		"           registration, then run the host C-plane stack so a\n"
		"           handset can register (parks the radio on exit)\n"
		"  -P PIN   registration PIN / AC (default 0000; used with -g)\n");
}

int main(int argc, char **argv)
{
	struct stubd s;
	struct dect_pcm_config cfg;
	struct dect_sess_cfg scfg = {
		.src_task = DECT_TASK_PCM,
		.dst_task = DECT_TASK_CONTACT_DST,
	};
	const char *dev = "/dev/dect0";
	unsigned int run_seconds = 0, fpb;
	int c, ret = 1;

	memset(&s, 0, sizeof(s));
	s.tx_slot = s.rx_slot = -1;
	s.rand_fd = -1;
	memset(&cfg, 0, sizeof(cfg));
	while ((c = getopt(argc, argv, "d:c:T:R:t:vpM:gP:h")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'c': cfg.slots = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'T': s.tx_slot = (int)strtol(optarg, NULL, 0); break;
		case 'R': s.rx_slot = (int)strtol(optarg, NULL, 0); break;
		case 't': run_seconds = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'v': s.verbose = 1; break;
		case 'p': s.verbose = 1; s.show_params = 1; break;
		case 'M':
			if (s.n_xmail >= sizeof(s.xmail) / sizeof(s.xmail[0])) {
				usage();
				return 2;
			}
			s.xmail[s.n_xmail++] = optarg;
			break;
		case 'g': s.do_register = 1; break;
		case 'P': s.pin = optarg; break;
		default: usage(); return 2;
		}
	}

	/* -g auto-sends the MAC bring-up (radio on, open registration) unless the
	 * caller supplied its own -M mails. [CR COMMAND_SET §6.1/§6.2] */
	if (s.do_register && s.n_xmail == 0) {
		s.xmail[s.n_xmail++] = "22:02:015b:01";	/* radio/beacon on */
		s.xmail[s.n_xmail++] = "22:02:022b:41";	/* 'A': open registration */
	}
	if (optind != argc) {
		usage();
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	dect_rx_init(&s.rx);
	dect_session_init(&s.sess, &scfg);

	s.fd = open(dev, O_RDWR);
	if (s.fd < 0) {
		perror(dev);
		return 1;
	}
	if (bring_up(&s, &cfg))
		goto out;
	fpb = cfg.frames_per_block;

	if (s.tx_slot < 0 || s.rx_slot < 0) {
		if (find_control_slot(&s, fpb)) {
			fprintf(stderr, "control slot not found\n");
			goto out;
		}
	} else {
		printf("control slot given: tx %d, rx %d\n", s.tx_slot, s.rx_slot);
	}
	if (s.tx_slot >= (int)s.slots || s.rx_slot >= (int)s.slots) {
		fprintf(stderr, "slot out of range\n");
		goto out;
	}
	if (s.do_register)
		s.rand_fd = open("/dev/urandom", O_RDONLY);
	ret = run_loop(&s, fpb, run_seconds);

	/* Park the radio so the base does not keep beaconing with registration
	 * open after we exit. [CR COMMAND_SET §6.2/§6.1] */
	if (s.do_register && s.tx_slot >= 0) {
		send_mail_spec(&s, "22:02:022c:01");	/* close registration */
		send_mail_spec(&s, "22:02:015b:00");	/* radio off */
		printf("reg: radio parked\n");
	}
out:
	if (s.rand_fd >= 0)
		close(s.rand_fd);
	tear_down(&s);
	close(s.fd);
	return ret;
}

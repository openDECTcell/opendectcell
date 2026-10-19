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
#include "dect_nwk.h"
#include "dect_mm.h"
#include "dect_cc.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u
#define POLL_MS		100

static volatile sig_atomic_t stop;
static uint64_t g_start;		/* for relative timestamps in the log */

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
	/*
	 * The handset's long-term key, kept across connections: registration and a
	 * later call run on different connections (each re-inits the MM FSM), and
	 * call-time ciphering needs the UAK. Persisted in the subscription store
	 * (-s) so a call after a daemon restart needs no fresh enrolment.
	 */
	uint8_t uak[DECT_MM_UAK_LEN];
	int uak_valid;
	const char *sub_path;	/* host-side subscription store (holds the UAK) */
	int sub_restored;	/* stored subscription already pushed to the chip */
	/*
	 * Experimental chip cipher-enable (0x0208). Its field layout is only
	 * statically derived and the key offsets are unproven [CR A-0027 Q1], so
	 * it is never sent unless -k asks for a specific selector and layout.
	 */
	struct chip_cipher {
		int     enabled;
		uint8_t selector;
		int     use_rs;		/* 1 = RS in the input field, else RAND_F */
		int     no_reverse;	/* 1 = send DCK/RES unreversed */
		int     long_form;	/* 1 = 33-byte mail variant */
	} chip_cipher;
	/* Voice-slot probe: which TDM slot carries a call's audio (mapping open). */
	uint16_t vs_last[8];
	uint32_t vs_changes[8];
	uint32_t vs_frames;
	/*
	 * Voice capture (-w): dump the call's TDM slot raw, two bytes per frame,
	 * so the sample format can be worked out offline. The slot is measured,
	 * the encoding inside it is not: 8000 frames/s x 2 bytes could be 16-bit
	 * linear at 8 kHz, A-law at 16 kHz, or A-law in one byte of each pair.
	 */
	int         no_cipher;		/* -C: never send CIPHER-REQUEST */
	/*
	 * Continuous transmit [CR A-0030 §1.1, §10]. Stock AVM services the TDM
	 * transmit DMA every block, interleaving the control slot and the voice
	 * lanes into one stream; it does not write a burst of frames only when a
	 * mail is pending. Worse, our burst wrote 0xFF across *every* slot, which
	 * A-0030 §4 Q8 warns against explicitly: it overwrites the voice lanes.
	 *
	 * So mails become a queue of control-slot bytes (two per frame) that the
	 * block loop drains, and each transmitted block fills the voice lanes with
	 * their own idle value instead of trampling them.
	 */
	int         tx_ring;		/* ring active: queue mails, do not write */
	uint8_t     txq[4096];		/* pending control-slot bytes */
	size_t      txq_head, txq_len;
	uint32_t    txq_drops;
	uint8_t     tx_idle;		/* filler for unmapped slots (-a txidle) */
	uint8_t     tx_voice_idle;	/* filler for the voice lanes */
	uint32_t    tx_blocks;
	/*
	 * Audio bind parameters [CR A-0029 §1]. Every field AVM derives from its
	 * negotiated call state is settable here, because we cannot derive them
	 * and each hardware run costs a person's attention -- so one run should
	 * be able to sweep a hypothesis rather than needing a rebuild per guess.
	 */
	struct audio_cfg {
		uint8_t  sel;		/* 0x0251 channel parameter selector */
		uint8_t  gain;		/* 0x0251 value (stored << 8 by the STUB) */
		int      no_gain;	/* skip 0x0251 entirely */
		uint8_t  flags;		/* 0x0230 chan_flags (low nibble = channel) */
		uint16_t mask;		/* 0x0230 chan_mask */
		uint8_t  mode;		/* 0x0230 extra mode byte */
		uint8_t  codec;		/* 0x0230 codec byte */
		int      send_f626;	/* also send 0xF626 (absent from AVM's bind) */
		int      nowait;	/* send 0x022D immediately (old behaviour) */
		uint8_t  tx_idle;	/* transmit filler for unmapped slots */
		int      have_tx_idle;
		uint8_t  tx_voice_idle;	/* transmit filler for the voice lanes */
		int      have_voice_idle;
	} audio;
	const char *voice_path;
	FILE       *voice_fp;
	int         voice_slot;		/* -V: one slot only; <0 = every slot */
	uint32_t    voice_bytes;
	struct reg_ctx {
		int active;
		int persisted;		/* chip subscription written this connection */
		int call_bound;		/* audio channel bound for the active call */
		int audio_connected;	/* 0x022D sent (after 0x0242, or fallback) */
		uint64_t bind_ms;	/* when 0x0230 went out, for the fallback */
		int cipher_started;	/* call-time authentication kicked off */
		int cipher_done;	/* cipher outcome already reported */
		uint8_t handle;
		struct dect_lc_rx lc;
		struct dect_lapc lapc;
		struct dect_mm mm;
		struct dect_cc cc;
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

	printf("[%5llu] %s src %02x dst %02x prim %04x len %u",
	       (unsigned long long)(mono_ms() - g_start), dir, m->src, m->dst,
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

	/*
	 * Once the ring is running every write goes through it, so the mail is
	 * appended to the control-slot byte queue and the block loop paces it
	 * out. Before that -- control-slot probing and bring-up -- there is no
	 * ring yet, so the original burst write is still used.
	 */
	if (s->tx_ring) {
		if (s->txq_len + (size_t)el_len > sizeof(s->txq)) {
			s->txq_drops++;
			return -1;
		}
		for (k = 0; k < (size_t)el_len; k++)
			s->txq[(s->txq_head + s->txq_len + k) % sizeof(s->txq)] =
				el[k];
		s->txq_len += (size_t)el_len;
		s->tx_seq++;
		return 0;
	}

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

/*
 * Transmit one DMA block: every frame carries the control slot (the next two
 * queued mail bytes, or idle when the queue is empty) and every other slot its
 * configured idle value. Called once per received block so the transmit side
 * keeps pace with the receive side without per-frame syscalls, which is the
 * cadence A-0030 §2 Q1 describes ("refills occur per buffer, not per
 * individual 8-kHz frame").
 */
static void tx_block(struct stubd *s, unsigned int fpb, uint8_t *frames)
{
	size_t fb = s->slots * 2u;
	unsigned int f, n;

	if (s->tx_slot < 0)
		return;
	for (f = 0; f < fpb; f++) {
		uint8_t *fr = frames + f * fb;

		for (n = 0; n < s->slots; n++) {
			uint8_t fill = (n == (unsigned int)s->tx_slot)
				? 0xFFu : s->tx_idle;

			fr[n * 2u] = fill;
			fr[n * 2u + 1u] = fill;
		}
		/*
		 * Voice lanes get their own filler so a silent call sends
		 * silence rather than whatever the control path happens to use.
		 */
		if (s->reg.call_bound && s->tx_voice_idle != s->tx_idle) {
			for (n = 0; n < s->slots; n++) {
				if (n == (unsigned int)s->tx_slot)
					continue;
				fr[n * 2u] = s->tx_voice_idle;
				fr[n * 2u + 1u] = s->tx_voice_idle;
			}
		}
		if (s->txq_len >= 2u) {
			fr[(unsigned int)s->tx_slot * 2u] =
				s->txq[s->txq_head];
			fr[(unsigned int)s->tx_slot * 2u + 1u] =
				s->txq[(s->txq_head + 1u) % sizeof(s->txq)];
			s->txq_head = (s->txq_head + 2u) % sizeof(s->txq);
			s->txq_len -= 2u;
		}
	}
	if (write(s->fd, frames, fpb * fb) != (ssize_t)(fpb * fb))
		s->txq_drops++;
	s->tx_blocks++;
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

/*
 * Wrap a LAPC frame in an Lc Cs frame and send it via APPEND_CONNECTION_DATA
 * (0x0244) only. AVM's host driver transmits C-plane replies this way and does
 * NOT follow with 0x0202; in AVM's code 0x0202 is sent on an abnormal link
 * release, which matches the ~40 ms connection drop we saw when we triggered it
 * [CR AVM]. LSIG = 0: the PP has no assigned PMID during registration.
 */
static void send_cplane(struct stubd *s, uint8_t handle,
			const uint8_t *lapc, size_t lapc_len)
{
	uint8_t cs[DECT_LC_FRAME_MAX];
	struct dect_mail m;
	/* Sign with the link's signature (0 while unregistered, the PP's PMID-
	 * derived LSIG once learned from its establishment frame). */
	size_t flen = dect_lc_build(lapc, lapc_len, s->reg.lc.lsig, cs, sizeof(cs));

	if (flen == 0)
		return;
	if (dect_cs_append_mail(&m, DECT_TASK_PCM, handle, cs, flen) == 0)
		send_mail(s, (unsigned int)s->tx_slot, &m);
}

/*
 * Host-side subscription store.
 *
 * The chip's subscription is volatile: it is lost whenever the radio is parked,
 * so AVM keeps the record host-side and reloads it into the chip at start-up
 * [CR A-0026]. We do the same with one small file, which also gives call-time
 * ciphering the UAK after a daemon restart (registration and a later call run on
 * different connections). The file holds long-term key material, so it is
 * created 0600 and must never leave the box.
 */
#define SUB_MAGIC	0x44535531u	/* "DSU1" */

struct sub_record {
	uint32_t magic;
	uint8_t  slot;
	uint8_t  uak[DECT_MM_UAK_LEN];
	uint8_t  port_id_len;
	uint8_t  port_id[DECT_MM_PORTID_MAX];
};

static void sub_save(struct stubd *s)
{
	struct sub_record r;
	int fd;

	if (!s->sub_path || !s->uak_valid)
		return;
	memset(&r, 0, sizeof(r));
	r.magic = SUB_MAGIC;
	r.slot = DECT_MM_REG_SLOT;
	memcpy(r.uak, s->uak, DECT_MM_UAK_LEN);
	r.port_id_len = (uint8_t)s->reg.mm.port_id_len;
	memcpy(r.port_id, s->reg.mm.port_id, sizeof(r.port_id));

	fd = open(s->sub_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("sub: cannot write %s: %s\n", s->sub_path, strerror(errno));
		return;
	}
	if (write(fd, &r, sizeof(r)) != (ssize_t)sizeof(r))
		printf("sub: short write to %s\n", s->sub_path);
	else
		printf("sub: subscription saved to %s\n", s->sub_path);
	close(fd);
}

/* Load the stored subscription, if any. Returns 1 when a record was adopted. */
static int sub_load(struct stubd *s)
{
	struct sub_record r;
	int fd;
	ssize_t n;

	if (!s->sub_path)
		return 0;
	fd = open(s->sub_path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, &r, sizeof(r));
	close(fd);
	if (n != (ssize_t)sizeof(r) || r.magic != SUB_MAGIC ||
	    r.port_id_len > DECT_MM_PORTID_MAX)
		return 0;
	memcpy(s->uak, r.uak, DECT_MM_UAK_LEN);
	s->uak_valid = 1;
	printf("sub: restored subscription for slot %u from %s\n",
	       r.slot, s->sub_path);
	return 1;
}

/*
 * Persist the just-enrolled handset in the chip so it holds standby and is
 * reachable: 0x0247 {slot, op=1, status} then 0x020A {slot, UAK[16], extra},
 * to STUB task 0x02, in that order, after LOCATE-ACCEPT [CR A-0026]. The 16-byte
 * 0x020A record is the allocated UAK. The `status`/`extra` security-mode bytes
 * have no known enumeration [CR A-0026]; we send 0x00 and validate on hardware.
 * 0xF605 / 0x015F / 0x017C are intentionally NOT sent (not on AVM's ordinary
 * voice-handset path).
 */
static void send_subscription(struct stubd *s)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0247u;
	m.param[0] = DECT_MM_REG_SLOT;	/* slot */
	m.param[1] = 0x01u;		/* op: install/refresh */
	m.param[2] = 0x00u;		/* status (unknown enumeration) */
	m.param_len = 3;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x020Au;
	m.param[0] = DECT_MM_REG_SLOT;	/* slot */
	memcpy(&m.param[1], s->uak, DECT_MM_UAK_LEN);		/* 16-byte credential */
	m.param[1 + DECT_MM_UAK_LEN] = 0x00u;			/* extra (unknown) */
	m.param_len = (uint16_t)(2 + DECT_MM_UAK_LEN);
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)			/* params carry the UAK: -p is local only */
		log_mail(&m, "tx M", s->show_params);
}

/*
 * Bind a connected call's voice path to DSP audio channel 1 [CR COMMAND_SET
 * §6.5]: assign the channel (0x0230), program the DSP route (0xF626 on), then
 * connect the audio path (0x022D). Reply 0x0242 confirms the channel. This sets
 * up the bearer; feeding actual voice (echo/tone) is a later step.
 */
/*
 * Bind a connected call's voice path, in the order AVM's own bind routine uses
 * [CR A-0029 §1]: 0x0251 SET_CHANNEL_PARAM *first*, then 0x0230 in its full
 * six-parameter form. Our earlier sequence had three faults, all from A-0029:
 *
 *  - 0x0251 was never sent at all. AVM sends it before the channel is assigned,
 *    not after; its third byte is an EEPROM-derived value that A-0029 could not
 *    recover, so -a gain=N makes it settable.
 *  - 0x0230 was sent in the two-byte short form. The STUB defaults the missing
 *    fields, so it is not invalid, but it does not reproduce AVM's setup.
 *  - 0xF626 was sent between the two. A-0029 found it absent from the normal
 *    binding producer -- it lives in the transport's serialisation logic -- so
 *    it is off unless -a f626 asks for it.
 *
 * 0x022D stays last: A-0029 found it produced by separate state-dependent
 * paths (mute/unmute, audio-state events) rather than as a fixed third step,
 * but connecting the path explicitly is still what we want.
 */
static void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan);

static void send_audio_bind(struct stubd *s, uint8_t handle)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;

	if (!a->no_gain) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0x0251u;	/* SET_CHANNEL_PARAM, before the assign */
		m.param[0] = handle;
		m.param[1] = a->sel;	/* channel parameter selector (1..3) */
		m.param[2] = a->gain;	/* stored as value << 8 in the DSP block */
		m.param_len = 3;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0230u;		/* ASSIGN_AUDIO_CHANNEL, full form */
	m.param[0] = handle;
	m.param[1] = a->flags;		/* chan_flags: low nibble = channel */
	m.param[2] = (uint8_t)(a->mask & 0xFFu);	/* chan_mask (LE) */
	m.param[3] = (uint8_t)(a->mask >> 8);
	m.param[4] = a->mode;		/* extra mode byte */
	m.param[5] = a->codec;		/* codec byte */
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

	printf("call: audio assign: 0x0251 %s(sel %u gain 0x%02x), "
	       "0x0230 {%02x %04x %02x %02x}%s\n",
	       a->no_gain ? "SKIPPED " : "", a->sel, a->gain,
	       a->flags, a->mask, a->mode, a->codec,
	       a->nowait ? "" : " -- 0x022D waits for 0x0242");

	/*
	 * 0x022D connects the path of a connection "whose channel is already
	 * assigned" [CR COMMAND_SET §6.5]. The assignment is asynchronous: the
	 * 0x0242 confirmation arrives tens of milliseconds after 0x0230, while a
	 * 0x022D queued here is on the wire within a few. AVM sends 0x022D from
	 * state-dependent paths rather than as a fixed third mail [CR A-0029 §2,
	 * A-0030 §2 Q6], so we wait for the confirmation too -- see the 0x0242
	 * case in handle_reg_mail() -- unless -a nowait asks for the old order.
	 */
	if (a->nowait)
		send_audio_connect(s, handle, a->flags & 0x0Fu);
}

/*
 * Connect the assigned channel: optionally route it (0xF626, not part of
 * AVM's bind), then 0x022D with bit 7 clear. `chan` is the channel the chip
 * confirmed in 0x0242 -- A-0032 Q9: trust the returned channel, not the one
 * we asked for.
 */
static void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;

	if (s->reg.audio_connected)
		return;
	s->reg.audio_connected = 1;

	if (a->send_f626) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0xF626u;	/* SET_AUDIO_ROUTE */
		m.param[0] = handle;
		m.param[1] = 0x01u;	/* enable */
		m.param[2] = chan;	/* chan_cfg */
		m.param[3] = 0x00u;	/* param (LE) */
		m.param[4] = 0x00u;
		m.param_len = 5;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x022Du;		/* AUDIO_PATH_ON_OFF */
	m.param[0] = handle;
	m.param[1] = 0x00u;		/* bit 7 = 0: connect */
	m.param_len = 2;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: audio connect: %s0x022D on channel %u\n",
	       a->send_f626 ? "0xF626, " : "", chan);
}

/*
 * Fallback: if the chip never confirms the assignment, connect anyway after
 * half a second so the deferred order can never do worse than the old one.
 */
static void audio_connect_fallback(struct stubd *s, uint64_t now)
{
	if (!s->reg.call_bound || s->reg.audio_connected || !s->reg.active)
		return;
	if (now - s->reg.bind_ms < 500u)
		return;
	printf("call: audio connect: no 0x0242 within 500 ms, connecting anyway\n");
	send_audio_connect(s, s->reg.handle, s->audio.flags & 0x0Fu);
}

/*
 * Hand the call's security material to the chip (0x0208), so the STUB can
 * switch the radio to DSC. Layout traced to its producer in [CR A-0028 §1],
 * which corrects A-0027: offsets below are into the complete mail, so they are
 * two higher than the parameter indices used here.
 *
 *   mail[2]      selector (see below)
 *   mail[3..10]  the authentication worker's 8-byte computed result, BYTE-
 *                REVERSED as a group before the event that feeds this mail
 *   mail[11..14] its 4-byte computed result, likewise byte-reversed
 *   mail[15..22] the last 8 bytes of a 16-byte authentication *input*, copied
 *                intact -- an input, not a derived value
 *   mail[23..25] zeroed, mail[26..27] FF FF
 *   mail[28..29] EEPROM 0x51/0x52, whose defaults are 00 00
 *   mail[30..32] only in the 33-byte form: 0x40 and two per-terminal bytes
 *
 * Mapping those onto DSAA1: the two computed results are the 64-bit DCK and
 * the 32-bit RES, and the 16-byte input is the pair of 8-byte randoms, so its
 * last half is RAND_F (",rs" swaps in RS, in case the halves are the other way
 * round). A-0028 is explicit that the DCK identification is a candidate, not a
 * proven fact, so ",norev" keeps the unreversed order available too.
 *
 * Selector: A-0028 §2 found two producer branches. The live-call one takes a
 * context identifier from the authentication event's routing nibble -- which is
 * NOT proven equal to our MAC handle -- while the subscription-oriented branch
 * uses terminal index + 7, i.e. 7..12. It also withdraws A-0027's implication
 * that low selectors are safe: neither family is certified harmless without a
 * consumer audit, though the state lives in RAM-loaded firmware. So -k still
 * has to name the selector; the daemon never picks one.
 */
static void send_chip_cipher(struct stubd *s)
{
	struct dect_mail m;
	uint8_t dck[DECT_MM_DCK_LEN];
	size_t n = 0, i;

	if (!dect_mm_cipher_dck(&s->reg.mm, dck))
		return;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0208u;		/* chip security/cipher material */
	m.param[n++] = s->chip_cipher.selector;

	/* mail[3..10]: computed 8-byte result (DCK candidate), reversed. */
	for (i = 0; i < DECT_MM_DCK_LEN; i++)
		m.param[n + i] = s->chip_cipher.no_reverse
			? dck[i] : dck[DECT_MM_DCK_LEN - 1u - i];
	n += DECT_MM_DCK_LEN;

	/* mail[11..14]: computed 4-byte result (RES), reversed. */
	for (i = 0; i < 4u; i++)
		m.param[n + i] = s->chip_cipher.no_reverse
			? s->reg.mm.cipher_res[i] : s->reg.mm.cipher_res[3u - i];
	n += 4u;

	/* mail[15..22]: second half of the 16-byte input, copied intact. */
	memcpy(&m.param[n], s->chip_cipher.use_rs ? s->reg.mm.rs
						  : s->reg.mm.rand_f, 8);
	n += 8u;

	m.param[n++] = 0x00u;		/* zeroed by the producer */
	m.param[n++] = 0x00u;
	m.param[n++] = 0x00u;
	m.param[n++] = 0xFFu;		/* FF FF initialised by the producer */
	m.param[n++] = 0xFFu;
	m.param[n++] = 0x00u;		/* EEPROM 0x51, default 00 [CR A-0028] */
	m.param[n++] = 0x00u;		/* EEPROM 0x52, default 00 */
	if (s->chip_cipher.long_form) {
		m.param[n++] = 0x40u;	/* fixed in the 33-byte form */
		m.param[n++] = 0x00u;	/* per-terminal status/config */
		m.param[n++] = 0x00u;
	}
	m.param_len = (uint16_t)n;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	memset(dck, 0, sizeof(dck));
	memset(&m, 0, sizeof(m));	/* the mail held the DCK */
	/* Never log this mail's parameters: they carry the cipher key. */
	printf("call: cipher: chip command 0x0208 sent (selector %u, %u bytes, "
	       "%s order, input %s)\n",
	       s->chip_cipher.selector, (unsigned int)(n + 2),
	       s->chip_cipher.no_reverse ? "plain" : "reversed",
	       s->chip_cipher.use_rs ? "RS" : "RAND_F");
}

/* Flush any LAPC frames the FSM wants to transmit. */
static void pump_lapc(struct stubd *s)
{
	uint8_t frame[DECT_LC_LAPC_MAX];
	size_t n;

	while ((n = dect_lapc_poll_tx(&s->reg.lapc, frame, sizeof(frame))) > 0) {
		if (s->verbose)
			printf("[%5llu] reg: tx cplane %zu bytes (addr %02x ctrl %02x)\n",
			       (unsigned long long)(mono_ms() - g_start), n,
			       frame[0], frame[1]);
		send_cplane(s, s->reg.handle, frame, n);
	}
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
	s->reg.persisted = 0;
	s->reg.call_bound = 0;
	s->reg.cipher_started = 0;
	s->reg.cipher_done = 0;
	s->reg.handle = handle;
	dect_lc_rx_init(&s->reg.lc, 0);
	dect_lapc_init(&s->reg.lapc, 0);
	dect_mm_init(&s->reg.mm, &mcfg);
	dect_cc_init(&s->reg.cc);
	printf("reg: connection up (handle %u), C-plane armed\n", handle);
}

/* Drain queued NWK responses (MM and CC) into LAPC, one per free window. */
static void drain_nwk(struct stubd *s, uint64_t now)
{
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t mlen;

	while (!s->reg.lapc.outstanding) {
		mlen = dect_mm_poll(&s->reg.mm, msg, sizeof(msg));
		if (mlen == 0)
			mlen = dect_cc_poll(&s->reg.cc, msg, sizeof(msg));
		if (mlen == 0)
			break;
		if (s->verbose) {
			size_t i;

			printf("reg: nwk out --> ");
			for (i = 0; i < mlen; i++)
				printf("%02x ", msg[i]);
			printf("\n");
		}
		if (dect_lapc_send(&s->reg.lapc, msg, mlen, now) != 0)
			break;
		pump_lapc(s);
	}
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
	if (s->verbose)
		printf("reg: cs in %02x %02x %02x (addr/ctrl/li), lc_rc=%d\n",
		       seg.data[0], seg.data[1], seg.data[2], lapc_len);
	if (lapc_len <= 0)
		return;
	if (dect_lapc_parse(lapcbuf, (size_t)lapc_len, &f) < 0) {
		if (s->verbose)
			printf("reg: lapc parse failed (len %d)\n", lapc_len);
		return;
	}
	if (s->verbose)
		printf("reg: lapc type=%d ns=%u nr=%u more=%u infolen=%zu\n",
		       f.type, f.ns, f.nr, f.more, f.info_len);
	nwk_len = dect_lapc_rx(&s->reg.lapc, &f, now, nwk, sizeof(nwk));
	if (s->verbose)
		printf("reg: nwk_len=%d\n", nwk_len);
	pump_lapc(s);				/* at least the LAPC ack */
	if (nwk_len > 0) {
		struct dect_nwk_hdr h;
		const uint8_t *ies;
		size_t ies_len;

		if (s->verbose) {		/* local only: carries IPUI, never commit */
			int i;

			printf("reg: nwk in  <-- ");
			for (i = 0; i < nwk_len; i++)
				printf("%02x ", nwk[i]);
			printf("\n");
		}
		/* Route by protocol discriminator: CC to call control, else MM. */
		if (dect_nwk_parse(nwk, (size_t)nwk_len, &h, &ies, &ies_len) == 0 &&
		    h.pd == DECT_PD_CC) {
			dect_cc_rx(&s->reg.cc, nwk, (size_t)nwk_len);
		} else {
			uint8_t rand_f[8], rs[16];

			gen_rand(s, rand_f, sizeof(rand_f));
			gen_rand(s, rs, sizeof(rs));
			dect_mm_rx(&s->reg.mm, nwk, (size_t)nwk_len, rand_f, rs);
		}
	}

	/* The next message goes out when the handset acknowledges the previous one. */
	drain_nwk(s, now);

	/*
	 * Once the handset is authenticated (UAK valid) and we have answered its
	 * LOCATE-REQUEST, install the subscription in the chip, once per connection.
	 */
	if (!s->reg.persisted && s->reg.mm.subscribed && s->reg.mm.stats.located) {
		/* Keep the UAK for the later call's ciphering, and across restarts. */
		memcpy(s->uak, s->reg.mm.uak, DECT_MM_UAK_LEN);
		s->uak_valid = 1;
		send_subscription(s);
		sub_save(s);
		/*
		 * Close registration once the handset is enrolled. AVM's documented
		 * sequence opens registration (0x022B 'A') and closes it afterwards
		 * with 0x022C 01, which clears the a44 "registration open" capability
		 * [CR COMMAND_SET §6.2/§9.7]. Leaving it open keeps the base
		 * advertising open enrolment and the handset re-attaching.
		 */
		send_mail_spec(s, "0x22:0x02:0x022c:01");
		s->reg.persisted = 1;
		printf("reg: subscription persisted + registration closed (slot %u)\n",
		       DECT_MM_REG_SLOT);
	}

	if (s->reg.mm.state == DECT_MM_REGISTERED)
		printf("reg: handset registered\n");
	else if (s->reg.mm.state == DECT_MM_FAILED)
		printf("reg: registration rejected\n");

	/*
	 * Bind the voice path once we have answered the call (CONNECT sent). The
	 * C4 treats CONNECT as answered even though it does not send CONNECT-ACK.
	 */
	if (!s->reg.call_bound &&
	    (s->reg.cc.state == DECT_CC_ST_CONNECT_PENDING ||
	     s->reg.cc.state == DECT_CC_ST_ACTIVE)) {
		s->reg.audio_connected = 0;
		s->reg.bind_ms = now;
		send_audio_bind(s, s->reg.handle);
		s->reg.call_bound = 1;
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;		/* start the voice-slot probe */
		printf("call: connected (handle %u) -- audio channel assigned\n",
		       s->reg.handle);

		/*
		 * Start call ciphering: challenge the handset with the stored UAK so
		 * both sides derive the DCK, then ask it to switch on DSC. The chip
		 * cipher-enable (0x0208) only goes out when -k asks for it, its field
		 * mapping being unproven [CR A-0028 §1].
		 *
		 * -C suppresses all of this. The handset ciphers the whole bearer --
		 * voice included -- the moment it sees our CIPHER-REQUEST, and we
		 * cannot decipher, so a captured voice slot is ciphertext from ~400 ms
		 * into the call. Suppressing the request keeps the call in the clear
		 * long enough to work out the sample format, at the price of the
		 * handset dropping it with its unencrypted-connection complaint.
		 */
		if (s->no_cipher && !s->reg.cipher_started) {
			s->reg.cipher_started = 1;
			printf("call: cipher: suppressed (-C), call stays in the clear\n");
		} else if (s->uak_valid && !s->reg.cipher_started) {
			uint8_t rand_f[8], rs[16];

			gen_rand(s, rand_f, sizeof(rand_f));
			gen_rand(s, rs, sizeof(rs));
			dect_mm_start_cipher(&s->reg.mm, s->uak, rand_f, rs);
			s->reg.cipher_started = 1;
			printf("call: cipher: authenticating handset\n");
			drain_nwk(s, now);
		} else if (!s->uak_valid) {
			printf("call: cipher: no UAK (handset must enrol this session)\n");
		}
	}

	/* Never log key material: state only. */
	if (s->reg.mm.cipher_state == DECT_MM_CIPHER_READY && !s->reg.cipher_done) {
		s->reg.cipher_done = 1;
		/*
		 * Key material first, then the over-air request: A-0027 Q5 makes
		 * loading the chip and negotiating the cipher distinct steps in
		 * that order. The CIPHER-REQUEST is only queued here; it still has
		 * to travel through LAPC, so the mail below wins the race.
		 */
		if (s->chip_cipher.enabled)
			send_chip_cipher(s);
		printf("call: cipher: DCK derived, CIPHER-REQUEST sent\n");
	} else if (s->reg.mm.cipher_state == DECT_MM_CIPHER_FAILED &&
		   !s->reg.cipher_done) {
		s->reg.cipher_done = 1;
		printf("call: cipher: authentication FAILED (RES mismatch)\n");
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
	case 0x0242:
		/*
		 * Channel assignment confirmed: {handle, chan_flags, mask}. The
		 * low nibble is the channel the chip actually gave us; 0 or 0xF
		 * means none. Only the long form answers 0x0230 [CR COMMAND_SET
		 * §8.4]; the short one reports later state changes.
		 */
		if (s->reg.call_bound && !s->reg.audio_connected &&
		    m->param_len >= 4 && m->param[0] == s->reg.handle) {
			uint8_t chan = m->param[1] & 0x0Fu;

			if (chan >= 1u && chan <= 6u)
				send_audio_connect(s, s->reg.handle, chan);
		}
		break;
	default:
		break;
	}
}

/*
 * While a call is up, count per-slot sample changes. The STUB's audio-channel ->
 * TDM-slot mapping is not documented (docs/stub-pcm-control-channel.md), so find
 * the call's voice slot empirically: it is the one that keeps changing while the
 * idle slots hold a constant fill. Read-only.
 */
static void voice_slot_probe(struct stubd *s, const uint8_t *fr, unsigned int fpb)
{
	unsigned int f, n, nslots = s->slots < 8u ? s->slots : 8u;

	if (!s->reg.call_bound)
		return;
	for (f = 0; f < fpb; f++) {
		for (n = 0; n < nslots; n++) {
			uint16_t v;

			memcpy(&v, fr + (f * s->slots + n) * 2u, 2);
			if (s->vs_frames && v != s->vs_last[n])
				s->vs_changes[n]++;
			s->vs_last[n] = v;
		}
		s->vs_frames++;
	}
	if (s->vs_frames >= 8000u) {		/* 8 kHz frame rate: about one second */
		printf("call: slot activity (changes/s):");
		for (n = 0; n < nslots; n++)
			printf(" %u:%u", n, s->vs_changes[n]);
		printf("\n");
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;
	}
}

/*
 * Dump the call's voice slot while it is bound: the raw two bytes per frame,
 * in frame order, with nothing interpreted. Decoding happens offline -- see
 * tools/voice-decode.py, which tries the candidate formats against the file.
 */
static void voice_capture(struct stubd *s, const uint8_t *fr, unsigned int fpb)
{
	unsigned int f;

	if (!s->voice_fp || !s->reg.call_bound)
		return;

	/*
	 * Default (-V absent) dumps every slot of every frame, so one capture
	 * can be searched offline for the voice: A-0029 §4 warns that a 16-bit
	 * TDM word may hold two independent 8-bit channels, that the lane
	 * matters, and that a slot picked by index in the DMA buffer is not
	 * necessarily the logical channel. Guessing a slot per run would cost a
	 * dial each; capturing all of them costs disk.
	 */
	if (s->voice_slot < 0) {
		size_t n = (size_t)fpb * s->slots * 2u;

		if (fwrite(fr, 1, n, s->voice_fp) != n)
			goto fail;
		s->voice_bytes += (uint32_t)n;
		return;
	}

	if ((unsigned int)s->voice_slot >= s->slots)
		return;
	for (f = 0; f < fpb; f++) {
		if (fwrite(fr + (f * s->slots + (unsigned int)s->voice_slot) * 2u,
			   2, 1, s->voice_fp) != 1)
			goto fail;
		s->voice_bytes += 2u;
	}
	return;
fail:
	perror("voice capture");
	fclose(s->voice_fp);
	s->voice_fp = NULL;
}

/* Main loop: decode mails, drive the session, (re)send the contact. */
static int run_loop(struct stubd *s, unsigned int fpb, unsigned int run_seconds)
{
	size_t block = s->slots * 2u * fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *buf = malloc(slot * 16u);
	uint8_t *txbuf = malloc(block);
	uint64_t start = mono_ms();

	if (!buf || !txbuf) {
		free(buf);
		free(txbuf);
		return -1;
	}
	s->tx_ring = 1;		/* from here on, mails are paced by tx_block() */
	g_start = start;
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
				voice_slot_probe(s, fr, fpb);
				voice_capture(s, fr, fpb);
				tx_block(s, fpb, txbuf);
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
		if (s->do_register)
			audio_connect_fallback(s, now);
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
		/*
		 * Reload a stored subscription into the chip once the radio is up,
		 * the way AVM restores saved handsets at boot [CR A-0026], so the
		 * handset stays known across daemon restarts.
		 */
		if (s->sess.state == DECT_SESS_UP && s->xmail_idx >= s->n_xmail &&
		    s->uak_valid && !s->sub_restored) {
			send_subscription(s);
			s->sub_restored = 1;
			printf("sub: stored subscription reloaded into the chip\n");
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
	printf("tx: blocks %u queue_drops %u idle %02x voice_idle %02x\n",
	       s->tx_blocks, s->txq_drops, s->tx_idle, s->tx_voice_idle);
	printf("rx: mails %u seq_gaps %u csum_errors %u framing_errors %u resyncs %u\n",
	       s->rx.stats.mails, s->rx.stats.seq_gaps, s->rx.stats.csum_errors,
	       s->rx.stats.framing_errors, s->rx.stats.resyncs);
	free(buf);
	free(txbuf);
	return 0;
}

/*
 * Parse -a key=value[,key=value...]: the audio bind fields (see struct
 * audio_cfg). Bare keys "f626" and "nogain" take no value. Returns 0 on
 * success, -1 on a malformed spec.
 */
static int parse_audio(struct audio_cfg *a, const char *spec)
{
	while (*spec) {
		const char *key = spec, *eq;
		unsigned long v = 0;
		size_t klen;
		char *end;

		while (*spec && *spec != ',')
			spec++;
		eq = memchr(key, '=', (size_t)(spec - key));
		klen = eq ? (size_t)(eq - key) : (size_t)(spec - key);
		if (eq) {
			v = strtoul(eq + 1, &end, 0);
			if (end != spec)
				return -1;
		}
		if (!eq && klen == 4 && !memcmp(key, "f626", 4))
			a->send_f626 = 1;
		else if (!eq && klen == 6 && !memcmp(key, "nogain", 6))
			a->no_gain = 1;
		else if (!eq && klen == 6 && !memcmp(key, "nowait", 6))
			a->nowait = 1;
		else if (!eq)
			return -1;
		else if (klen == 3 && !memcmp(key, "sel", 3))
			a->sel = (uint8_t)v;
		else if (klen == 4 && !memcmp(key, "gain", 4))
			a->gain = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "flags", 5))
			a->flags = (uint8_t)v;
		else if (klen == 4 && !memcmp(key, "mask", 4))
			a->mask = (uint16_t)v;
		else if (klen == 4 && !memcmp(key, "mode", 4))
			a->mode = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "codec", 5))
			a->codec = (uint8_t)v;
		else if (klen == 6 && !memcmp(key, "txidle", 6))
			a->tx_idle = (uint8_t)v, a->have_tx_idle = 1;
		else if (klen == 9 && !memcmp(key, "voiceidle", 9))
			a->tx_voice_idle = (uint8_t)v, a->have_voice_idle = 1;
		else
			return -1;
		if (*spec == ',')
			spec++;
	}
	return 0;
}

/* Parse -k SEL[,rs][,33]. Returns 0 on success, -1 on a malformed spec. */
static int parse_chip_cipher(struct chip_cipher *cc, const char *spec)
{
	char *end;
	unsigned long sel;

	sel = strtoul(spec, &end, 0);
	if (end == spec || sel > 0xFFu)
		return -1;
	cc->enabled = 1;
	cc->selector = (uint8_t)sel;
	while (*end == ',') {
		const char *opt = ++end;

		while (*end && *end != ',')
			end++;
		size_t len = (size_t)(end - opt);

		if (len == 2 && memcmp(opt, "rs", 2) == 0)
			cc->use_rs = 1;
		else if (len == 2 && memcmp(opt, "33", 2) == 0)
			cc->long_form = 1;
		else if (len == 5 && memcmp(opt, "norev", 5) == 0)
			cc->no_reverse = 1;
		else
			return -1;
	}
	return *end ? -1 : 0;
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
		"  -P PIN   registration PIN / AC (default 0000; used with -g)\n"
		"  -s PATH  subscription store, holds the handset key; reloaded into\n"
		"           the chip at start-up (default /etc/opendectcell-sub, \"\" = off)\n"
		"  -k SPEC  EXPERIMENTAL: hand the call's cipher material to the chip\n"
		"           (0x0208) once the DCK is derived. SPEC is\n"
		"           SEL[,rs][,norev][,33]: SEL = selector (7..12 is the\n"
		"           subscription-oriented branch), rs = put RS rather than\n"
		"           RAND_F in the input field, norev = do not byte-reverse the\n"
		"           DCK/RES, 33 = long mail variant. Layout per [CR A-0028];\n"
		"           the DCK identification is a candidate only. Off by default\n"
		"  -w FILE  capture the call's voice slot to FILE (raw, 2 bytes per\n"
		"           frame, while a call is bound) -- holds call audio, keep\n"
		"           it local\n"
		"  -V SLOT  capture only this slot (default: every slot, so the\n"
		"           voice can be found offline)\n"
		"  -C       do not start call ciphering. The handset ciphers the whole\n"
		"           bearer once it sees CIPHER-REQUEST, so captured voice is\n"
		"           ciphertext; -C keeps the call in the clear for audio work\n"
		"           (the handset then drops it as unencrypted)\n"
		"  -a SPEC  audio bind parameters, comma separated [CR A-0029]:\n"
		"           sel=N gain=N (0x0251), flags=N mask=N mode=N codec=N\n"
		"           (0x0230), nogain = skip 0x0251, f626 = also send 0xF626,\n"
		"           nowait = send 0x022D at once instead of after 0x0242\n"
		"           txidle=N voiceidle=N set the transmit filler bytes\n"
		"           Default: sel=1,gain=0x2b,flags=1,mask=0,mode=0,codec=0,\n"
		"           txidle=0xff,voiceidle=0xff\n");
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
	s.sub_path = "/etc/opendectcell-sub";	/* overridden by -s */
	s.voice_slot = -1;		/* capture every slot unless -V says one */
	s.audio.sel = 1;		/* simple single-channel case [CR A-0029] */
	/*
	 * 0x2B is the fresh-default byte AVM's own 0x0251 producer reads from
	 * EEPROM 0x2A + channel index (all six records initialised to 0x2B)
	 * [CR A-0036]. Its DSP semantics are still unproven; it is simply the
	 * one value with provenance, where 1 was a placeholder.
	 */
	s.audio.gain = 0x2Bu;
	s.audio.flags = 1;		/* channel 1 */
	s.tx_idle = 0xFFu;
	s.tx_voice_idle = 0xFFu;
	while ((c = getopt(argc, argv, "d:c:T:R:t:vpM:gP:s:k:w:V:Ca:h")) != -1) {
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
		case 's': s.sub_path = optarg[0] ? optarg : NULL; break;
		case 'C': s.no_cipher = 1; break;
		case 'a':
			if (parse_audio(&s.audio, optarg)) {
				usage();
				return 2;
			}
			break;
		case 'w': s.voice_path = optarg; break;
		case 'V': s.voice_slot = (int)strtol(optarg, NULL, 0); break;
		case 'k':
			if (parse_chip_cipher(&s.chip_cipher, optarg) < 0) {
				usage();
				return 2;
			}
			break;
		default: usage(); return 2;
		}
	}

	/* -g auto-sends the MAC bring-up (radio on, open registration) unless the
	 * caller supplied its own -M mails. [CR COMMAND_SET §6.1/§6.2] */
	if (s.do_register && s.n_xmail == 0) {
		/* src/dst/prim parse with base 0, so hex needs the 0x prefix. */
		s.xmail[s.n_xmail++] = "0x22:0x02:0x015b:01";	/* radio/beacon on */
		s.xmail[s.n_xmail++] = "0x22:0x02:0x022b:41";	/* 'A': open reg */
	}
	if (optind != argc) {
		usage();
		return 2;
	}

	if (s.do_register)
		sub_load(&s);		/* restores the UAK; chip reload once up */

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
	if (s.audio.have_tx_idle)
		s.tx_idle = s.audio.tx_idle;
	if (s.audio.have_voice_idle)
		s.tx_voice_idle = s.audio.tx_voice_idle;
	if (s.do_register)
		s.rand_fd = open("/dev/urandom", O_RDONLY);
	if (s.voice_path) {
		s.voice_fp = fopen(s.voice_path, "wb");
		if (!s.voice_fp) {
			perror(s.voice_path);
			goto out;
		}
	}
	ret = run_loop(&s, fpb, run_seconds);
	if (s.voice_fp) {
		fclose(s.voice_fp);
		printf("voice: captured %u bytes from slot %d (%u frames)\n",
		       s.voice_bytes, s.voice_slot, s.voice_bytes / 2u);
	}

	/* Park the radio so the base does not keep beaconing with registration
	 * open after we exit. [CR COMMAND_SET §6.2/§6.1] */
	if (s.do_register && s.tx_slot >= 0) {
		send_mail_spec(&s, "0x22:0x02:0x022c:01");	/* close registration */
		send_mail_spec(&s, "0x22:0x02:0x015b:00");	/* radio off */
		printf("reg: radio parked\n");
	}
out:
	if (s.rand_fd >= 0)
		close(s.rand_fd);
	tear_down(&s);
	close(s.fd);
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, speech: the lanes of a bridged call (G.722 or linear), the transmit block
 * (tx_block: pacing of control mails, idle fill, speech) and the audio socket that baresip and
 * dect-cell connect to. Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

static void ab_pop(struct ab_lane *l)
{
	size_t cap = sizeof(l->down);
	size_t prime = AB_PRIME_MS * (l->wideband ? 8u : 16u);

	if (l->dn >= 2 && (l->primed || l->dn >= prime)) {
		l->primed = 1;
		l->cur[0] = l->down[l->dh];
		l->cur[1] = l->down[(l->dh + 1u) % cap];
		l->dh = (l->dh + 2u) % cap;
		l->dn -= 2u;
		return;
	}
	if (l->wideband) {
		static const int16_t z[4];

		(void)dect_g722_encode(&l->sil, z, 4, l->cur);
	} else {
		l->cur[0] = l->cur[1] = 0;
	}
	if (l->primed)
		l->underruns++;
}

static void ab_level(int16_t *peak, uint32_t *sig, const int16_t *pcm, size_t n)
{
	int16_t top = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];

		if (v > 32767)
			v = 32767;
		if (v > top)
			top = (int16_t)v;
	}
	if (top > *peak)
		*peak = top;
	if (top > AB_SIGNAL)
		(*sig)++;
}

static void ab_up(struct ab_lane *l, const uint8_t *w)
{
	int16_t wb[AB_FRAME / 2u], nb[AB_FRAME / 4u];
	uint8_t g[AB_FRAME / 4u];
	const void *out;
	size_t n, len;

	l->up[l->un++] = w[0];
	l->up[l->un++] = w[1];
	if (l->un < (l->wideband ? 160u : 320u))
		return;
	l->un = 0;
	if (l->cfd < 0)
		return;
	if (l->wideband && l->g722) {
		out = l->up;			/* the octets as they are */
		len = 160u;
		goto send;
	}
	/* linear: 16 kHz from a wideband lane, 8 kHz from a narrowband one */
	if (l->wideband) {
		n = dect_g722_decode(&l->dec, l->up, 160u, wb);
		out = wb;
	} else {
		unsigned i;

		for (i = 0; i < 160u; i++)
			nb[i] = (int16_t)(l->up[2u * i] |
					  (l->up[2u * i + 1u] << 8));
		n = 160u;
		out = nb;
	}
	ab_level(&l->up_peak, &l->up_sig, out, n);
	if (l->wideband && l->rate == 8000u) {
		n = dect_rs2_down(&l->rs_up, wb, n, nb);
		out = nb;
	} else if (!l->wideband && l->rate != 8000u) {
		n = dect_rs2_up(&l->rs_up, nb, n, wb);
		out = wb;
	}
	len = n * sizeof(int16_t);
	if (l->g722) {
		len = dect_g722_encode(&l->sip_enc, out, n, g);
		out = g;
	}
send:
	if (send(l->cfd, out, len, MSG_DONTWAIT | MSG_NOSIGNAL) ==
	    (ssize_t)len)
		l->frames_up++;
	else
		l->dropped++;
}

/*
 * A frame from the bridge into the lane's own format, appended to
 * down[]: buf, len octets, in the socket's format.
 */
static void ab_down(struct ab_lane *l, const void *buf, size_t len)
{
	int16_t wb[AB_FRAME / 2u], nb[AB_FRAME / 4u];
	uint8_t d[AB_FRAME / 2u];
	const int16_t *pcm;
	size_t n, k, j, cap = sizeof(l->down);

	if (l->g722 && l->wideband) {
		memcpy(d, buf, len);		/* the octets as they are */
		k = len;
		goto queue;
	}
	if (l->g722) {
		n = dect_g722_decode(&l->sip_dec, buf, len, wb);
		pcm = wb;
	} else {
		n = len / sizeof(int16_t);
		pcm = buf;
	}
	ab_level(&l->down_peak, &l->down_sig, pcm, n);
	/* the socket's rate (16 kHz for G.722) to the lane's */
	if (l->wideband && l->rate == 8000u && !l->g722) {
		n = dect_rs2_up(&l->rs_down, pcm, n, wb);
		pcm = wb;
	} else if (!l->wideband && (l->rate != 8000u || l->g722)) {
		n = dect_rs2_down(&l->rs_down, pcm, n, nb);
		pcm = nb;
	}
	if (l->wideband) {
		k = dect_g722_encode(&l->enc, pcm, n, d);
	} else {
		for (j = 0; j < n; j++) {
			d[2u * j] = (uint8_t)(pcm[j] & 0xFF);
			d[2u * j + 1u] = (uint8_t)((uint16_t)pcm[j] >> 8);
		}
		k = 2u * n;
	}
queue:
	if (l->dn + k > cap) {
		l->dropped++;		/* 1 s (wideband) or 0.25 s behind */
		return;
	}
	for (j = 0; j < k; j++)
		l->down[(l->dh + l->dn + j) % cap] = d[j];
	l->dn += k;
	l->frames_down++;
}

/* One frame of lane l, TDM word w: src is the handset's word, or NULL. */
void ab_frame(struct ab_lane *l, uint8_t *fr, unsigned int w,
		     const uint8_t *src)
{
	if (!l->wideband) {
		if (src)
			ab_up(l, src);
		ab_pop(l);
	} else {
		if (src) {
			uint32_t v = (uint32_t)src[0] |
				     ((uint32_t)src[1] << 8);

			if (v == l->rx_prev) {
				if (++l->rx_run == 2u && lane_word_live(v))
					ab_up(l, src);
			} else {
				l->rx_run = 1;
				/* the first change between live words: lock */
				if (!l->locked && lane_word_live(v) &&
				    lane_word_live(l->rx_prev)) {
					l->locked = 1;
					l->phase = 0;
				}
			}
			l->rx_prev = v;
		}
		if (!l->phase)
			ab_pop(l);
		l->phase ^= 1;
	}
	fr[w * 2u] = l->cur[0];
	fr[w * 2u + 1u] = l->cur[1];
}

/* An internal call's other party's word for lane x, or -1. */
static int ic_peer_word(const struct stubd *s, const struct xlane *x)
{
	int c = s->base.ic.caller, t = s->base.ic.tside;

	if (s->base.ic.st != IC_UP)
		return -1;
	if (t >= 0 && s->side[t].bl->lane >= 0 && &s->xl[s->side[t].bl->lane] == x)
		return ic_word(s, c);
	if (c >= 0 && s->side[c].bl->lane >= 0 && &s->xl[s->side[c].bl->lane] == x)
		return ic_word(s, t);
	return -1;
}

/*
 * The extra lanes' downlink for frame f (-a twocalls): the bridge's speech;
 * an internal call's other party; the G.722 file phase-locked to the chip's
 * two-frame words as on lane 0; a raw (linear) file two bytes a frame; the
 * loop-back; else silence. Each lane keeps its own file position, so two
 * calls each hear the whole file.
 */
static void xlane_tx(struct stubd *s, uint8_t *fr, const uint8_t *rx,
		     unsigned int f)
{
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++) {
		struct xlane *x = &s->xl[i];
		unsigned w = x->word;
		const uint8_t *src;
		int peer;

		if (!x->used || !x->connected || w >= s->slots ||
		    w == (unsigned)s->tx_slot)
			continue;
		src = rx ? rx + (f * s->slots + w) * 2u : NULL;
		if (s->ab.l[1u + i].cfd >= 0 && s->ab.l[1u + i].call) {
			ab_frame(&s->ab.l[1u + i], fr, w, src);
			continue;
		}
		fr[w * 2u] = s->tx_voice_idle;
		fr[w * 2u + 1u] = s->tx_voice_idle;
		peer = rx ? ic_peer_word(s, x) : -1;
		if (peer >= 0) {
			/* the other handset's uplink, as it comes */
			const uint8_t *p = rx + (f * s->slots + (unsigned)peer) * 2u;

			fr[w * 2u] = p[0];
			fr[w * 2u + 1u] = p[1];
		} else if (s->audio.play && !s->audio.play_raw && x->wideband) {
			int advance;

			if (src) {
				uint32_t v = (uint32_t)src[0] |
					     ((uint32_t)src[1] << 8);

				if (v != x->rx_prev && lane_word_live(v) &&
				    lane_word_live(x->rx_prev) && !x->locked) {
					x->locked = 1;
					x->phase = 0;
				}
				x->rx_prev = v;
			}
			advance = !x->phase || !x->pos;
			x->phase ^= 1;
			if (advance) {
				if (x->pos + 2u > s->audio.play_len)
					x->pos = 0;
				x->pos += 2u;
			}
			fr[w * 2u] = s->audio.play[x->pos - 2u];
			fr[w * 2u + 1u] = s->audio.play[x->pos - 1u];
		} else if (s->audio.play && s->audio.play_raw && !x->wideband) {
			if (x->pos + 2u > s->audio.play_len)
				x->pos = 0;
			fr[w * 2u] = s->audio.play[x->pos];
			fr[w * 2u + 1u] = s->audio.play[x->pos + 1u];
			x->pos += 2u;
		} else if (s->audio.loopback && src) {
			fr[w * 2u] = src[0];
			fr[w * 2u + 1u] = src[1];
		}
	}
}

/*
 * The idle frames tx_block() starts every frame from: [0] while no call is
 * bound, every lane the transmit filler; [1] during a call, the voice lanes'
 * own filler, so a silent call sends silence rather than whatever the
 * control path uses. The control slot is idle (0xFF) in both. Their inputs
 * (the control slot, -a txidle and voiceidle) are fixed before run_loop()
 * starts, so it builds them once.
 */
void tx_templates(const struct stubd *s, uint8_t *t)
{
	size_t fb = s->slots * 2u;
	unsigned int c = (unsigned int)s->tx_slot;

	memset(t, s->tx_idle, fb);
	memset(t + fb, s->tx_voice_idle, fb);
	if (s->tx_slot >= 0 && c < s->slots) {
		t[c * 2u] = t[c * 2u + 1u] = 0xFFu;
		t[fb + c * 2u] = t[fb + c * 2u + 1u] = 0xFFu;
	}
}

static enum lane0_src lane0_source(const struct stubd *s, const uint8_t *rx,
				   unsigned int *peer)
{
	unsigned int w = s->audio.lane0_word;
	int mode2 = s->reg.call_wideband || !s->audio.remode_ms ||
		    s->reg.remode_done;

	if (!s->base.main_link.bound || w >= s->slots ||
	    w == (unsigned int)s->tx_slot)
		return L0_IDLE;
	if (s->ab.l[0].cfd >= 0 && s->ab.l[0].call)
		return mode2 ? L0_BRIDGE : L0_IDLE;
	if (s->base.ic.st == IC_UP && rx) {
		int p = ic_word(s, s->base.ic.caller == -1 ? s->base.ic.tside :
				   s->base.ic.tside == -1 ? s->base.ic.caller :
				   IC_PARTY_NONE);

		if (p >= 0) {
			*peer = (unsigned int)p;
			return L0_PEER;
		}
	}
	if (s->audio.play && s->audio.play_raw)
		return mode2 ? L0_RAW : L0_IDLE;
	if (s->audio.play)
		return L0_PLAY;
	if (s->audio.loopback && rx)
		return L0_LOOP;
	return L0_IDLE;
}

/*
 * One frame of -a play on lane 0: out is the lane's word in the frame we
 * send, in the received one (NULL when there is none).
 *
 * Lock the phase once, then free-run at two frames per word. The chip takes
 * the downlink at a steady 8000 octets/s, so the rate is ours to keep; what
 * we cannot know is which frame of each pair starts a word. The first change
 * between two live received words shows the chip's pair boundary. Both
 * directions run off one frame clock, so there is no drift to follow
 * afterwards, and following every transition would starve the lane whenever
 * the handset is silent. Idle fills are not live, or the lock would land on
 * a block boundary.
 */
static void lane0_play(struct stubd *s, uint8_t *out, const uint8_t *in)
{
	int advance;

	if ((s->audio.play_sync || s->reg.call_wideband) && in) {
		uint32_t v = (uint32_t)in[0] | ((uint32_t)in[1] << 8);

		if (v != s->rx_word_prev && lane_word_live(v) &&
		    lane_word_live(s->rx_word_prev) && !s->play_locked) {
			s->play_locked = 1;
			s->play_phase = 0;
		}
		s->rx_word_prev = v;
	}
	advance = !s->play_phase || !s->play_pos;
	s->play_phase ^= 1;
	if (advance) {
		if (s->play_pos + 2u > s->audio.play_len)
			s->play_pos = 0;
		s->play_pos += 2u;
	}
	out[0] = s->audio.play[s->play_pos - 2u];
	out[1] = s->audio.play[s->play_pos - 1u];
}

/*
 * Transmit one DMA block: every frame carries the control slot (the next two
 * queued mail bytes, or idle when the queue is empty), the calls' lanes, and
 * every other slot its idle value. Called once per received block (rx, the
 * same frames' receive side, or NULL), so the transmit side keeps pace with
 * the receive side without per-frame syscalls, which is the cadence A-0030
 * §2 Q1 describes ("refills occur per buffer, not per individual 8-kHz
 * frame").
 */
void tx_block(struct stubd *s, unsigned int fpb, uint8_t *frames,
		     const uint8_t *rx)
{
	size_t fb = s->slots * 2u;
	unsigned int f, w = s->audio.lane0_word, peer = 0;
	const uint8_t *tmpl;
	enum lane0_src src0;
	/*
	 * Control-slot bytes are taken from a copy of the queue position and
	 * committed only for the frames the driver accepted, so a failed or
	 * short write loses no part of a mail. Frames take the queue in pairs
	 * from the first frame on, so the bytes owed for the first k accepted
	 * frames are min(2k, bytes taken).
	 */
	size_t qh = s->txq_head, ql = s->txq_len, taken, owed;
	ssize_t wr;

	if (s->tx_slot < 0)
		return;
	tmpl = s->tx_tmpl + (s->base.main_link.bound || s->cell.media_n ?
			     fb : 0u);
	src0 = lane0_source(s, rx, &peer);
	for (f = 0; f < fpb; f++) {
		uint8_t *fr = frames + f * fb;
		const uint8_t *in = rx ? rx + f * fb : NULL;

		memcpy(fr, tmpl, fb);
		switch (src0) {
		case L0_BRIDGE:
			ab_frame(&s->ab.l[0], fr, w, in ? in + w * 2u : NULL);
			break;
		case L0_PEER:
			fr[w * 2u] = in[peer * 2u];
			fr[w * 2u + 1u] = in[peer * 2u + 1u];
			break;
		case L0_RAW:
			if (s->play_pos + 2u > s->audio.play_len)
				s->play_pos = 0;
			fr[w * 2u] = s->audio.play[s->play_pos];
			fr[w * 2u + 1u] = s->audio.play[s->play_pos + 1u];
			s->play_pos += 2u;
			break;
		case L0_PLAY:
			lane0_play(s, fr + w * 2u, in ? in + w * 2u : NULL);
			break;
		case L0_LOOP: {
			uint16_t v = (uint16_t)(in[w * 2u] |
						(in[w * 2u + 1u] << 8));

			/*
			 * Only real bearer data: a routed but empty lane reads
			 * 0x0000 and an unrouted one the 0xFEFE fill, and
			 * feeding either back while the chip's path comes up
			 * flattened a lane that was coming alive. Until there
			 * is something real to send, send the filler.
			 */
			if (v != 0x0000u && v != 0xFEFEu) {
				fr[w * 2u] = in[w * 2u];
				fr[w * 2u + 1u] = in[w * 2u + 1u];
			}
			break;
		}
		case L0_IDLE:
			break;
		}
		xlane_tx(s, fr, rx, f);
		if (s->cell_mode)
			cell_media_tx(s, fr, in);
		if (ql >= 2u) {
			fr[(unsigned int)s->tx_slot * 2u] = s->txq[qh];
			fr[(unsigned int)s->tx_slot * 2u + 1u] =
				s->txq[(qh + 1u) % sizeof(s->txq)];
			qh = (qh + 2u) % sizeof(s->txq);
			ql -= 2u;
		}
	}
	taken = s->txq_len - ql;
	wr = pcm_write(s, frames, fpb * fb, 0);
	if (wr == (ssize_t)(fpb * fb)) {
		owed = taken;
	} else {
		owed = wr > 0 ? 2u * ((size_t)wr / fb) : 0u;
		if (owed > taken)
			owed = taken;
		s->tx_write_short++;
	}
	s->txq_head = (s->txq_head + owed) % sizeof(s->txq);
	s->txq_len -= owed;
	s->tx_blocks++;
}

/*
 * The audio socket: listening while the control socket is. A connection
 * names its call in its first message, "call=N"; the lane that call is
 * bound to takes it.
 */
void ab_init(struct stubd *s)
{
	unsigned i;

	s->ab.lfd = -1;
	for (i = 0; i < AB_LANES; i++)
		s->ab.pend[i] = s->ab.l[i].cfd = -1;
}

/* The listening audio socket, for the bridge or, in cell mode, dect-cell. */
/*
 * The bridge sockets belong to root with mode 0600; with sockgroup=NAME (the service sets it when baresip runs
 * as an unprivileged user) they belong to that group with mode 0660, so exactly that user can connect. A group
 * that does not exist leaves them root-only.
 */
int sock_perms(const struct stubd *s, const char *path)
{
	const char *name = s->audio.sock_group;
	struct group *g;

	if (!name || !*name)
		return chmod(path, 0600);
	g = getgrnam(name);
	if (!g) {
		printf("sock: no group %s -- %s stays root-only\n", name, path);
		return chmod(path, 0600);
	}
	if (chown(path, (uid_t)-1, g->gr_gid))
		return -1;
	return chmod(path, 0660);
}

void ab_listen(struct stubd *s)
{
	struct sockaddr_un a;
	mode_t old;
	int fd;

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, AB_PATH);
	(void)unlink(AB_PATH);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	old = umask(077);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    sock_perms(s, AB_PATH) || listen(fd, (int)AB_LANES)) {
		printf("ctl: %s: %s -- no audio socket\n", AB_PATH,
		       strerror(errno));
		umask(old);
		close(fd);
		return;
	}
	umask(old);
	s->ab.lfd = fd;
}

void ab_open(struct stubd *s)
{
	ab_init(s);
	if (s->ctl.lfd >= 0)
		ab_listen(s);
}

void ab_end(struct ab_lane *l, const char *why)
{
	if (l->cfd < 0)
		return;
	close(l->cfd);
	l->cfd = -1;
	printf("audio: call %u's socket closed (%s): %u frames up, %u down, "
	       "%u underruns, %u dropped\n", l->call, why, l->frames_up,
	       l->frames_down, l->underruns, l->dropped);
	printf("audio: call %u's levels: from the handset %u of %u frames with signal, peak %d; "
	       "to the handset %u of %u frames with signal, peak %d\n", l->call, l->up_sig,
	       l->frames_up, l->up_peak, l->down_sig, l->frames_down, l->down_peak);
}

void ab_close(struct stubd *s)
{
	unsigned i;

	for (i = 0; i < AB_LANES; i++) {
		ab_end(&s->ab.l[i], "exit");
		if (s->ab.pend[i] >= 0)
			close(s->ab.pend[i]);
		s->ab.pend[i] = -1;
	}
	if (s->ab.lfd < 0)
		return;
	close(s->ab.lfd);
	s->ab.lfd = -1;
	(void)unlink(AB_PATH);
}

/* The bridged call whose speech lane i carries now, or NULL. */
static const struct dect_base_call *ab_call_of(struct stubd *s, unsigned i, int *wb)
{
	const struct xlane *x;
	unsigned k;

	if (i == 0) {
		*wb = s->reg.call_wideband;
		return s->base.main_link.active && s->base.main_link.bound ? bc_active(s, -1)
							  : NULL;
	}
	x = &s->xl[i - 1u];
	*wb = x->wideband;
	if (!x->used || !x->connected)
		return NULL;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl->active && s->side[k].bl->lane == (int)(i - 1u))
			return bc_active(s, (int)k);
	return NULL;
}

/* A pending connection's first message: "call=N" names its lane. */
static void ab_hello(struct stubd *s, unsigned j, uint64_t now)
{
	char buf[32];
	unsigned i, id = 0, rate = 16000u;
	int g722 = 0;
	ssize_t n = recv(s->ab.pend[j], buf, sizeof(buf) - 1u, MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
		      errno == EINTR) && now - s->ab.pend_since[j] < AB_HELLO_MS)
		return;
	if (n > 0) {
		unsigned mid;

		buf[n] = '\0';
		if (sscanf(buf, "call=%u rate=%u", &id, &rate) < 1)
			id = 0;
		/*
		 * cell mode: "media=N codec=g722" or "media=N rate=8000",
		 * from dect-cell for the controller's media session N (any
		 * u32, 0 too): the lane that session holds
		 */
		if (s->cell_mode && sscanf(buf, "media=%u rate=%u", &mid,
					   &rate) >= 1) {
			for (i = 0; i < AB_LANES; i++)
				if (s->cell.media[i].open &&
				    s->cell.media[i].id == mid)
					id = s->ab.l[i].call;
		}
		g722 = strstr(buf, " codec=g722") != NULL;
		if (rate != 8000u || g722)
			rate = 16000u;
	}
	for (i = 0; id && i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];

		if (l->call != id)
			continue;
		/* again: the SIP codec changed, the socket's format with it */
		if (l->cfd >= 0)
			ab_end(l, "the bridge connected again");
		l->cfd = s->ab.pend[j];
		l->rate = rate;
		l->g722 = g722;
		dect_g722_init(&l->sip_enc);
		dect_g722_init(&l->sip_dec);
		dect_rs2_init(&l->rs_up);
		dect_rs2_init(&l->rs_down);
		s->ab.pend[j] = -1;
		printf("audio: the bridge connected for call %u (%s) -- %s\n",
		       id, g722 ? "G.722" : rate == 8000u ? "8 kHz" : "16 kHz",
		       l->wideband ? (g722 ? "G.722 passed as it is" :
				      rate == 8000u ?
		       "G.722 decoded and resampled here (no common codec)" :
		       "G.722 decoded here") : rate == 8000u ?
		       "narrowband, passed as it is" : g722 ?
		       "narrowband, resampled and G.722 coded here (no common "
		       "codec)" : "narrowband, resampled here (no common codec)");
		return;
	}
	close(s->ab.pend[j]);
	s->ab.pend[j] = -1;
	printf("audio: a connection named no call waiting for one -- "
	       "closed\n");
}

void ab_tick(struct stubd *s, uint64_t now)
{
	unsigned i;
	int c;

	if (s->ab.lfd < 0)
		return;
	/* each lane: the call on it, announced once its speech is bound */
	for (i = 0; !s->cell_mode && i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];
		const struct dect_base_call *b;
		struct dect_ctl_out o;
		int wb;

		b = ab_call_of(s, i, &wb);
		if (l->call && (!b || l->call != b->id)) {
			ab_end(l, "the call ended");
			l->call = 0;
		}
		if (!b || l->call == b->id)
			continue;
		memset(l, 0, sizeof(*l));
		l->cfd = -1;
		l->call = b->id;
		l->wideband = wb;
		l->rx_prev = 0xFFFFFFFFu;
		dect_g722_init(&l->enc);
		dect_g722_init(&l->dec);
		dect_g722_init(&l->sip_enc);
		dect_g722_init(&l->sip_dec);
		dect_g722_init(&l->sil);
		dect_rs2_init(&l->rs_up);
		dect_rs2_init(&l->rs_down);
		dect_ctl_begin(&o, "audio");
		dect_ctl_add_uint(&o, "call", b->id);
		dect_ctl_add(&o, "path", AB_PATH);
		ctl_send_bridge(s, &o);
		printf("audio: call %u's speech on %s (lane %u, %s)\n", b->id,
		       AB_PATH, i, wb ? "wideband" : "narrowband");
	}
	while ((c = accept(s->ab.lfd, NULL, NULL)) >= 0) {
		for (i = 0; i < AB_LANES && s->ab.pend[i] >= 0; i++)
			;
		if (i == AB_LANES) {
			close(c);
			continue;
		}
		(void)fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		(void)fcntl(c, F_SETFD, FD_CLOEXEC);
		s->ab.pend[i] = c;
		s->ab.pend_since[i] = now;
	}
	for (i = 0; i < AB_LANES; i++)
		if (s->ab.pend[i] >= 0)
			ab_hello(s, i, now);
	for (i = 0; i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];

		while (l->cfd >= 0) {
			int16_t pcm[AB_FRAME / 2u];
			ssize_t n = recv(l->cfd, pcm, sizeof(pcm),
					 MSG_DONTWAIT);

			if (n == 0) {
				ab_end(l, "the bridge closed it");
				break;
			}
			if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK &&
				    errno != EINTR)
					ab_end(l, "read failed");
				break;
			}
			/*
			 * 20 ms in the socket's format; one octet is the
			 * bridge saying it has nothing (the fill covers it),
			 * anything else is not ours.
			 */
			if ((size_t)n == (l->g722 ? 160u :
					  l->rate / 50u * sizeof(pcm[0])))
				ab_down(l, pcm, (size_t)n);
		}
	}
}

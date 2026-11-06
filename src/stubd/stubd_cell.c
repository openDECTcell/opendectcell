// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, cell mode (-K): the chip's mails go verbatim to dect-cell over a
 * local socket, and the controller's media sessions are bound to chip lanes.
 * Moved here unchanged from dect-stubd.c; see src/stubd/README.md and
 * docs/cell-protocol.md.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

void cell_open(struct stubd *s)
{
	struct sockaddr_un a;
	mode_t old;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	s->cell.cfd = -1;
	s->cell.lfd = -1;
	if (fd < 0) {
		perror("cell: socket");
		return;
	}
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, CELL_PATH);
	(void)unlink(CELL_PATH);
	old = umask(077);		/* the mails carry keys (§7.3): root only */
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) {
		umask(old);
		perror("cell: " CELL_PATH);
		close(fd);
		return;
	}
	umask(old);
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	s->cell.lfd = fd;
	s->cell.lost_at = mono_ms();	/* no controller until dect-cell says */
	ab_listen(s);			/* §14.2: the sessions' speech */
	printf("cell: cell mode, waiting for dect-cell on " CELL_PATH "\n");
}

void cell_close(struct stubd *s)
{
	if (s->cell.cfd >= 0)
		close(s->cell.cfd);
	if (s->cell.lfd >= 0) {
		close(s->cell.lfd);
		(void)unlink(CELL_PATH);
	}
	s->cell.cfd = s->cell.lfd = -1;
	ab_close(s);
	(void)unlink(CELL_STATE_PATH);
}

/* One frame to dect-cell; -1 if it has none or its queue is full. */
static int cell_send(struct stubd *s, uint16_t type, uint16_t corr,
		     const uint8_t *body, size_t len)
{
	uint8_t f[DECT_CELL_HDR + 512];
	size_t n = dect_cell_put(f, sizeof(f), type, corr, body, len);

	if (s->cell.cfd < 0 || !n)
		return -1;
	return send(s->cell.cfd, f, n, MSG_DONTWAIT | MSG_NOSIGNAL) ==
	       (ssize_t)n ? 0 : -1;
}

/*
 * The chip firmware build, "06.88", from the second stage's file name
 * (dectfw_secondlevel_441-06.88-7412.hex): the controller tells the builds
 * apart by it (§7.1, §7.4). The whole name if it has none.
 */
static const char *stub_version(const struct stubd *s, char out[8])
{
	const char *p;

	if (s->stub_build == STUB_UNKNOWN)
		return "unknown";
	/* the 08.25 default file name carries no version: the controller needs
	 * "08.25" to know the key table's address (found 2026-11-01 on the 7520) */
	if (s->stub_build == STUB_0825)
		return "08.25";
	for (p = s->stub_name; *p; p++)
		if ((p[0] == '-' || p[0] == '_') &&
		    isdigit((unsigned char)p[1]) &&
		    isdigit((unsigned char)p[2]) && p[3] == '.' &&
		    isdigit((unsigned char)p[4]) &&
		    isdigit((unsigned char)p[5])) {
			memcpy(out, p + 1, 5);
			out[5] = '\0';
			return out;
		}
	return s->stub_name;
}

/* §10 STATUS: the radio, the chip session, the build in detail. */
/*
 * STATS (cell-protocol.md §10, §14.2): the chip side's counters, tag 1 per
 * counter {id u16, value u64}; dect-cell adds its own and forwards them.
 */
static void cell_stats(struct stubd *s, uint16_t corr)
{
	const struct {
		uint16_t id;
		uint64_t v;
	} c[] = {
		{ 1, s->cell.up },			/* mails up */
		{ 2, s->cell.down },			/* mails down */
		{ 3, s->cell.rejected },		/* rejected or dropped */
		{ 13, s->txq_hwm },			/* chip queue high water */
		{ 15, s->txq_drops },			/* STUB link counters */
		{ 16, s->rx.stats.csum_errors },
		{ 17, s->rx.stats.framing_errors },
		{ 18, s->rx.stats.seq_gaps },
		{ 19, s->rx.stats.resyncs },
	};
	/* per counter a TLV: 3 octets of header, 10 of value */
	uint8_t b[sizeof(c) / sizeof(c[0]) * 13u];
	struct dect_cell_tlv t;
	unsigned k, j;

	dect_cell_tlv_init(&t, b, sizeof(b));
	for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
		uint8_t v[10];

		v[0] = (uint8_t)(c[k].id >> 8);
		v[1] = (uint8_t)c[k].id;
		for (j = 0; j < 8u; j++)
			v[2 + j] = (uint8_t)(c[k].v >> (56u - 8u * j));
		dect_cell_tlv_put(&t, 1, v, sizeof(v));
	}
	if (t.err)
		printf("cell: STATS did not fit its buffer -- sent short\n");
	(void)cell_send(s, DECT_CELL_STATS, corr, b, t.len);
}

void cell_status(struct stubd *s, uint64_t now)
{
	uint8_t b[160];
	struct dect_cell_tlv t;
	char ver[8];
	const char *detail = stub_version(s, ver);

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u8(&t, 1, s->cell.radio_on ? 1 : 0);
	dect_cell_tlv_u8(&t, 2, s->sess.state == DECT_SESS_UP ? 2 : 1);
	dect_cell_tlv_u32(&t, 3, s->cell.epoch);
	dect_cell_tlv_u32(&t, 4, (uint32_t)((now - g_start) / 1000u));
	dect_cell_tlv_str(&t, 5, detail);
	if (s->cell.have_rfpi)
		dect_cell_tlv_put(&t, 6, s->cell.rfpi, sizeof(s->cell.rfpi));
	if (!t.err)
		(void)cell_send(s, DECT_CELL_STATUS, 0, b, t.len);
}

/*
 * STATE_SNAPSHOT (cell-protocol.md §6.4) after a resume, at dect-cell's
 * request: the chip's epoch, the radio, each radio connection still up
 * (handle and handset tag), each media session still open.
 */
static void cell_snapshot(struct stubd *s, uint16_t corr)
{
	uint8_t b[96];
	struct dect_cell_tlv t;
	unsigned h;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, s->cell.epoch);
	dect_cell_tlv_u8(&t, 2, s->cell.radio_on ? 1 : 0);
	for (h = 0; h < 8u; h++) {
		const uint8_t c[4] = { (uint8_t)h, s->cell.tag[h][0],
				       s->cell.tag[h][1], s->cell.tag[h][2] };

		if (s->cell.handles & (1u << h))
			dect_cell_tlv_put(&t, 3, c, sizeof(c));
	}
	for (h = 0; h < AB_LANES; h++)
		if (s->cell.media[h].open)
			dect_cell_tlv_u32(&t, 4, s->cell.media[h].id);
	if (!t.err)
		(void)cell_send(s, DECT_CELL_STATE_SNAPSHOT, corr, b, t.len);
}

/* ERROR {code, the frame's type, its corr} (§3.4), forwarded upstream. */
static void cell_error(struct stubd *s, uint16_t code,
		       const struct dect_cell_frame *f)
{
	uint8_t b[16];
	struct dect_cell_tlv t;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u16(&t, 1, code);
	dect_cell_tlv_u16(&t, 2, f->type);
	dect_cell_tlv_u16(&t, 3, f->corr);
	(void)cell_send(s, DECT_CELL_ERROR, 0, b, t.len);
}

static void cell_reject(struct stubd *s, uint16_t corr, uint8_t why)
{
	uint8_t b[3] = { (uint8_t)(corr >> 8), (uint8_t)corr, why };

	s->cell.rejected++;
	if (corr)
		(void)cell_send(s, DECT_CELL_MAIL_REJECT, 0, b, sizeof(b));
}

static void cell_park(struct stubd *s, const char *why)
{
	const uint8_t closed = 0x01, off = 0x00;

	chip_mail_to(s, DECT_TASK_CONTACT_DST, 0x022Cu, &closed, 1);
	chip_mail_to(s, DECT_TASK_CONTACT_DST, 0x015Bu, &off, 1);
	s->cell.radio_on = 0;
	printf("cell: radio parked (%s)\n", why);
}

/*
 * §8 media. The controller binds a call's audio itself (0x0251, 0x0230,
 * 0x022D by MAIL_DOWN, cell-protocol.md §8.1) and then names the chip's
 * channel in MEDIA_OPEN: the low nibble of the long 0x0242 that answered
 * its 0x0230. That 0x0242 echoes the bind {h, flags, mask}, and its mask
 * bit is the channel k = 1..3 the lanes are configured by (lane 0 for
 * k = 1, lane_ch[] for the others, clean room A-0124), its flag bit 6 the
 * coded G.722 lane. The lane's speech then goes over the audio socket as
 * a bridged call's does (docs/bridge-interface.md §3), with dect-cell as
 * the other end and the RTP its business.
 */
static void cell_media_end(struct stubd *s, unsigned i, uint8_t reason,
			   int tell)
{
	uint8_t b[16];
	struct dect_cell_tlv t;
	uint32_t id = s->cell.media[i].id;

	if (!s->cell.media[i].open)
		return;
	ab_end(&s->ab.l[i], reason == 0 ? "closed" : reason == 1 ?
	       "the chip released the channel" : "local error");
	s->ab.l[i].call = 0;
	memset(&s->cell.media[i], 0, sizeof(s->cell.media[i]));
	s->cell.media_n--;
	printf("cell: media %u closed (%s)\n", id, reason == 0 ? "requested" :
	       reason == 1 ? "the chip released the channel" : "local");
	if (!tell)
		return;
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u8(&t, 2, reason);
	(void)cell_send(s, DECT_CELL_MEDIA_CLOSED, 0, b, t.len);
}

static void cell_media_ack(struct stubd *s, uint16_t corr, uint32_t id,
			   uint16_t status)
{
	uint8_t b[16];
	struct dect_cell_tlv t;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u16(&t, 2, status);
	(void)cell_send(s, DECT_CELL_MEDIA_OPEN_ACK, corr, b, t.len);
}

/* MEDIA_OPEN, as dect-cell passes it on: no keys, no address (§14.2). */
static void cell_media_open(struct stubd *s, const struct dect_cell_frame *f)
{
	uint32_t id;
	uint8_t ch, fmt, fmt_up, h, k = 0, wb = 0;
	unsigned i, lane = AB_LANES, word = 0;
	struct ab_lane *l;

	if (dect_cell_tlv_check(f->body, f->len) ||
	    dect_cell_tlv_get_u32(f->body, f->len, 1, &id) ||
	    dect_cell_tlv_get_u8(f->body, f->len, 2, &ch) ||
	    dect_cell_tlv_get_u8(f->body, f->len, 3, &fmt)) {
		cell_error(s, DECT_CELL_ERR_MALFORMED, f);
		return;
	}
	fmt_up = fmt;
	(void)dect_cell_tlv_get_u8(f->body, f->len, 13, &fmt_up);
	/*
	 * One socket format both ways: the lane's own (G.722 octets or 8 kHz
	 * linear) or, converted here, the other (bridge-interface.md §3)
	 */
	if ((fmt != DECT_CELL_FMT_G722 && fmt != DECT_CELL_FMT_L16_8K) ||
	    fmt_up != fmt) {
		cell_media_ack(s, f->corr, id, 2);
		return;
	}
	for (i = 0; i < AB_LANES; i++)
		if (s->cell.media[i].open && s->cell.media[i].id == id) {
			cell_media_ack(s, f->corr, id, 3);	/* not unique */
			return;
		}
	for (h = 0; h < 8u; h++)
		if ((s->cell.handles & (1u << h)) && s->cell.bind[h].nib &&
		    s->cell.bind[h].nib == ch) {
			k = s->cell.bind[h].k;
			wb = s->cell.bind[h].wb;
			break;
		}
	if (k == 1u) {
		lane = 0;
		word = s->audio.lane0_word;
	}
	for (i = 0; k > 1u && i < XLANE_MAX; i++)
		if (s->audio.lane_ch[i] == k) {
			lane = 1u + i;
			word = s->audio.lane_word[i];
		}
	if (lane == AB_LANES || word >= s->slots ||
	    word == (unsigned)s->tx_slot) {
		printf("cell: media %u: no lane for channel %u (%s)\n", id, ch,
		       h < 8u ? "its bind's mask names none" :
		       "no long 0x0242 on a live handle names it");
		cell_media_ack(s, f->corr, id, 1);
		return;
	}
	if (s->cell.media[lane].open) {
		cell_media_ack(s, f->corr, id, 3);
		return;
	}
	l = &s->ab.l[lane];
	ab_end(l, "a new session on the lane");
	memset(l, 0, sizeof(*l));
	l->cfd = -1;
	if (!++s->cell.media_gen)
		s->cell.media_gen = 1;
	l->call = s->cell.media_gen;
	l->wideband = wb;
	l->rx_prev = 0xFFFFFFFFu;
	dect_g722_init(&l->enc);
	dect_g722_init(&l->dec);
	dect_g722_init(&l->sip_enc);
	dect_g722_init(&l->sip_dec);
	dect_g722_init(&l->sil);
	dect_rs2_init(&l->rs_up);
	dect_rs2_init(&l->rs_down);
	s->cell.media[lane].open = 1;
	s->cell.media[lane].id = id;
	s->cell.media[lane].chan = ch;
	s->cell.media[lane].handle = h;
	s->cell.media[lane].word = word;
	s->cell.media_n++;
	printf("cell: media %u: channel %u on handle %u, lane %u (TDM word "
	       "%u, %s), %s on the socket\n", id, ch, h, lane, word,
	       wb ? "G.722" : "narrowband", fmt == DECT_CELL_FMT_G722 ?
	       "G.722" : "8 kHz linear");
	cell_media_ack(s, f->corr, id, 0);
}

static void cell_media_close(struct stubd *s, const struct dect_cell_frame *f)
{
	uint8_t b[16];
	struct dect_cell_tlv t;
	uint32_t id;
	unsigned i;

	if (dect_cell_tlv_check(f->body, f->len) ||
	    dect_cell_tlv_get_u32(f->body, f->len, 1, &id)) {
		cell_error(s, DECT_CELL_ERR_MALFORMED, f);
		return;
	}
	for (i = 0; i < AB_LANES; i++)
		if (s->cell.media[i].open && s->cell.media[i].id == id) {
			cell_media_end(s, i, 0, 1);
			return;
		}
	/* gone already (the chip released it): closed all the same */
	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u8(&t, 2, 0);
	(void)cell_send(s, DECT_CELL_MEDIA_CLOSED, f->corr, b, t.len);
}

/* The open sessions' lanes for one frame: what dect-cell sent, or silence. */
void cell_media_tx(struct stubd *s, uint8_t *fr, const uint8_t *in)
{
	unsigned i;

	for (i = 0; s->cell.media_n && i < AB_LANES; i++) {
		unsigned w = s->cell.media[i].word;

		if (s->cell.media[i].open)
			ab_frame(&s->ab.l[i], fr, w, in ? in + w * 2u : NULL);
	}
}

/* Every mail from the chip, up as it came (§7.1), with the links it opens. */
void cell_mail_up(struct stubd *s, const struct dect_mail *m,
			 uint64_t now)
{
	struct dect_cell_mail cm = { (uint32_t)(now * 1000u), m->src, m->dst,
				     m->prim, m->param, m->param_len };
	uint8_t b[9u + DECT_CELL_MAIL_PARAM_MAX];
	size_t n = dect_cell_mail_up(b, sizeof(b), &cm);
	uint8_t h = m->param_len ? m->param[0] : 0xFFu;

	if (h < 8u && (m->prim == DECT_PRIM_CONN_UP ||
		       m->prim == DECT_PRIM_CONN_DOWN))
		s->led_traffic++;	/* a handset came or went: one blink */
	if (h < 8u && m->prim == DECT_PRIM_CONN_UP) {
		s->cell.handles |= (uint8_t)(1u << h);
		memset(s->cell.tag[h], 0, 3);
		memset(&s->cell.bind[h], 0, sizeof(s->cell.bind[h]));
	} else if (h < 8u && m->prim == DECT_PRIM_CONN_DOWN) {
		unsigned i;

		s->cell.handles &= (uint8_t)~(1u << h);
		memset(&s->cell.bind[h], 0, sizeof(s->cell.bind[h]));
		for (i = 0; i < AB_LANES; i++)
			if (s->cell.media[i].open &&
			    s->cell.media[i].handle == h)
				cell_media_end(s, i, 1, 1);
	} else if (h < 8u && m->prim == 0x0242u && m->param_len >= 4u) {
		/* the long form answers a 0x0230: the bind as it took */
		unsigned mask = m->param[2] | (unsigned)m->param[3] << 8;

		s->cell.bind[h].nib = m->param[1] & 0x0Fu;
		s->cell.bind[h].wb = (m->param[1] & 0x40u) != 0;
		s->cell.bind[h].k = (mask & (mask - 1u)) || !mask ? 0 :
				    (uint8_t)__builtin_ctz(mask);
	} else if (h < 8u && m->prim == 0x0203u && m->param_len >= 13u) {
		/* 07.56, 08.25: {handle, 01, seg[5], 00, 00, tag[3], handle};
		 * 06.88 sends no tag (cell-protocol.md §7.1) */
		memcpy(s->cell.tag[h], &m->param[9], 3);
	}
	if (n && cell_send(s, DECT_CELL_MAIL_UP, 0, b, n) == 0)
		s->cell.up++;
	else
		s->cell.lost_up++;	/* no dect-cell, or it is not reading */
}

/* A MAIL_DOWN from the controller, to the chip as it came. */
static void cell_mail_down(struct stubd *s, const struct dect_cell_frame *f)
{
	struct dect_cell_mail cm;
	struct dect_mail m;

	/*
	 * §5.3: only an active cell's radio is the controller's. dect-cell
	 * refuses these too; this is the second lock (review D2, finding 2:
	 * a disabled cell's radio came back on with a 0x015B {01}).
	 */
	if (!s->cell.active) {
		s->cell.rejected++;
		cell_error(s, DECT_CELL_ERR_NOT_ALLOWED, f);
		return;
	}
	if (dect_cell_mail_parse(f->body, f->len, 0, &cm)) {
		s->cell.rejected++;
		cell_error(s, DECT_CELL_ERR_MALFORMED, f);
		return;
	}
	if (s->sess.state != DECT_SESS_UP || s->tx_slot < 0) {
		cell_reject(s, f->corr, DECT_CELL_REJ_CHIP_DOWN);
		return;
	}
	memset(&m, 0, sizeof(m));
	m.src = cm.src;
	m.dst = cm.dst;
	m.prim = cm.prim;
	m.param_len = (uint16_t)cm.plen;
	memcpy(m.param, cm.param, cm.plen);
	if (send_mail(s, (unsigned int)s->tx_slot, &m)) {
		cell_reject(s, f->corr, DECT_CELL_REJ_QUEUE_FULL);
		return;
	}
	s->cell.down++;
	if (s->verbose)
		log_mail(&m, "tx C", s->show_params);
	if (m.prim == 0x015Bu && m.param_len) {
		s->cell.radio_on = m.param[0] != 0;
		printf("cell: the controller turned the radio %s\n",
		       s->cell.radio_on ? "on" : "off");
	}
}

/* CELL_STATE: the controller's state for us, or dect-cell's link-lost. */
static void cell_state(struct stubd *s, const struct dect_cell_frame *f,
		       uint64_t now)
{
	uint8_t st;
	size_t rn = 0;
	const uint8_t *r;

	if (dect_cell_tlv_check(f->body, f->len) ||
	    dect_cell_tlv_get_u8(f->body, f->len, 1, &st))
		return;
	r = dect_cell_tlv_get(f->body, f->len, 2, &rn);
	s->cell.active = st == DECT_CELL_ST_ACTIVE;
	if (r && rn == 9 && !memcmp(r, "link-lost", 9)) {
		if (!s->cell.lost_at) {
			s->cell.lost_at = now;
			printf("cell: the controller link is down\n");
		}
		return;
	}
	if (s->cell.lost_at)
		printf("cell: the controller link is up\n");
	s->cell.lost_at = 0;
	s->cell.released = s->cell.orphan_parked = 0;
	if ((st == DECT_CELL_ST_DISABLED || st == DECT_CELL_ST_REVOKED) &&
	    s->sess.state == DECT_SESS_UP)
		cell_park(s, st == DECT_CELL_ST_REVOKED ? "revoked" :
						      "disabled");
}

/* What dect-cell sent us, one frame per message. */
static void cell_frame(struct stubd *s, const uint8_t *buf, size_t n,
		       uint64_t now)
{
	struct dect_cell_frame f;

	if (dect_cell_parse(buf, n, &f) != (long)n)
		return;			/* one frame per message, whole */
	switch (f.type) {
	case DECT_CELL_MAIL_DOWN:
		cell_mail_down(s, &f);
		break;
	case DECT_CELL_CELL_STATE:
		cell_state(s, &f, now);
		break;
	case DECT_CELL_STATUS_REQUEST:
		cell_status(s, now);
		break;
	case DECT_CELL_STATS_REQUEST:
		cell_stats(s, f.corr);
		break;
	case DECT_CELL_STATE_SNAPSHOT:		/* dect-cell asks after a resume */
		cell_snapshot(s, f.corr);
		break;
	case DECT_CELL_MEDIA_OPEN:
	case DECT_CELL_MEDIA_CLOSE:
		if (!s->cell.active) {		/* §5.3, as for MAIL_DOWN */
			cell_error(s, DECT_CELL_ERR_NOT_ALLOWED, &f);
			break;
		}
		if (f.type == DECT_CELL_MEDIA_OPEN)
			cell_media_open(s, &f);
		else
			cell_media_close(s, &f);
		break;
	default:
		cell_error(s, DECT_CELL_ERR_UNSUPPORTED, &f);
	}
}

/* For Status > DECT in cell mode: the chip side; dect-cell writes the link. */
static void cell_state_write(struct stubd *s, uint64_t now)
{
	char tmp[sizeof(CELL_STATE_PATH) + 4];
	FILE *f;

	if (now < s->state_next)
		return;
	s->state_next = now + STATE_EVERY_MS;
	snprintf(tmp, sizeof(tmp), "%s.tmp", CELL_STATE_PATH);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "{\"mode\":\"cell\",\"uptime_ms\":%llu,\"radio\":\"%s\","
		"\"chip\":\"%s\",\"stub\":",
		(unsigned long long)(now - g_start),
		s->cell.radio_on ? "up" : "parked",
		s->sess.state == DECT_SESS_UP ? "running" : "starting");
	json_str(f, s->stub_build == STUB_UNKNOWN ? "unknown" : s->stub_name);
	fprintf(f, ",\"dect_cell\":%s,\"controller\":%s,\"links\":%u,"
		"\"mails_up\":%u,\"mails_down\":%u,\"rejected\":%u,"
		"\"not_forwarded\":%u,\"queue_bytes\":%zu,\"queue_drops\":%u,"
		"\"media\":%u}\n",
		s->cell.cfd >= 0 ? "true" : "false",
		s->cell.lost_at ? "false" : "true",
		(unsigned)__builtin_popcount(s->cell.handles), s->cell.up,
		s->cell.down, s->cell.rejected, s->cell.lost_up, s->txq_len,
		s->txq_drops, s->cell.media_n);
	if (fclose(f) == 0)
		(void)rename(tmp, CELL_STATE_PATH);
}

void cell_tick(struct stubd *s, uint64_t now)
{
	uint8_t buf[DECT_CELL_FRAME_MAX];
	int c;
	unsigned k;
	uint8_t flow;

	if (s->cell.lfd < 0)
		return;
	/* dect-cell, again after a restart: the newer one wins */
	c = accept(s->cell.lfd, NULL, NULL);
	if (c >= 0) {
		if (s->cell.cfd >= 0)
			close(s->cell.cfd);
		(void)fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		s->cell.cfd = c;
		s->cell.flow = 0;
		s->cell.active = 0;	/* until it tells us the state */
		printf("cell: dect-cell connected\n");
		cell_status(s, now);
	}
	for (c = 0; s->cell.cfd >= 0 && c < CELL_FRAMES_PER_TICK; c++) {
		ssize_t n = recv(s->cell.cfd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n > 0) {
			cell_frame(s, buf, (size_t)n, now);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == EINTR))
			break;
		/* dect-cell has gone: as good as a lost controller link */
		close(s->cell.cfd);
		s->cell.cfd = -1;
		s->cell.active = 0;
		printf("cell: dect-cell went away\n");
		for (k = 0; k < AB_LANES; k++)	/* its RTP went with it */
			cell_media_end(s, k, 2, 0);
		if (!s->cell.lost_at)
			s->cell.lost_at = now;
	}

	/* the chip answered: park first, whatever its build boots with */
	if (s->sess.state == DECT_SESS_UP && !s->cell.sess_up) {
		s->cell.epoch++;
		if (!s->cell.start_parked) {
			cell_park(s, "the chip is up; the controller decides");
			s->cell.start_parked = 1;
		}
		cell_status(s, now);
	}
	s->cell.sess_up = s->sess.state == DECT_SESS_UP;

	/* §6.4: links released after grace_ms, the radio parked after orphan_s */
	if (s->cell.lost_at && s->sess.state == DECT_SESS_UP) {
		if (!s->cell.released && now - s->cell.lost_at >= CELL_GRACE_MS) {
			unsigned h;

			for (h = 0; h < 8u; h++) {
				const uint8_t link = (uint8_t)h;

				if (!(s->cell.handles & (1u << h)))
					continue;
				send_audio_disconnect(s, link);
				chip_mail_to(s, DECT_TASK_CONTACT_DST, 0x0202u,
					     &link, 1);	/* [CR A-0127] */
			}
			printf("cell: no controller for %u ms: links released\n",
			       CELL_GRACE_MS);
			s->cell.released = 1;
		}
		if (!s->cell.orphan_parked && s->cell.radio_on &&
		    now - s->cell.lost_at >= CELL_ORPHAN_MS) {
			cell_park(s, "no controller");
			s->cell.orphan_parked = 1;
		}
	}

	/* §9.3 FLOW, on every change */
	flow = s->txq_drops != s->cell.txq_drops_seen ? 2 :
	       s->txq_len > CELL_FLOW_HIGH ? 1 : 0;
	s->cell.txq_drops_seen = s->txq_drops;
	if (flow != s->cell.flow) {
		uint8_t b[3] = { (uint8_t)(s->txq_len >> 8),
				 (uint8_t)s->txq_len, flow };

		if (cell_send(s, DECT_CELL_FLOW, 0, b, sizeof(b)) == 0)
			s->cell.flow = flow;
	}
	cell_state_write(s, now);
}

/*
 * rfpicheck= (bench B0): the RFPI in the chip's contact answer against the
 * one the loader patched into tag 1 [CR A-0132]. The bytes are an identity
 * and are not printed; the verdict names the octets that differ (the 5th
 * holds the RPN's low bits).
 */
/*
 * The RFPI the chip broadcasts, from its contact answer, kept for the
 * controller (STATUS tag 6, HELLO tag 17): it compares it with the identity
 * it wants this cell to have. Returns 1 when it is new or changed.
 */
int cell_rfpi_note(struct stubd *s, const struct dect_mail *m)
{
	if (m->prim != DECT_PRIM_CONTACT_ACK1 || m->param_len < 5u)
		return 0;
	if (s->cell.have_rfpi && !memcmp(s->cell.rfpi, m->param, 5))
		return 0;
	memcpy(s->cell.rfpi, m->param, 5);
	s->cell.have_rfpi = 1;
	return 1;
}

void rfpi_check(const struct stubd *s, const struct dect_mail *m)
{
	char diff[32] = "";
	size_t at = 0;
	unsigned k;

	if (!s->audio.rfpi_check_set || m->prim != DECT_PRIM_CONTACT_ACK1 ||
	    m->param_len < 5u)
		return;
	for (k = 0; k < 5u; k++)
		if (m->param[k] != s->audio.rfpi_check[k])
			at += (size_t)snprintf(diff + at, sizeof(diff) - at,
					       " %u", k + 1u);
	if (!at)
		printf("rfpi: the chip reports the RFPI rfpicheck= names, all "
		       "5 octets (0xF601)\n");
	else
		printf("rfpi: the chip's RFPI DIFFERS from rfpicheck= in "
		       "octet(s)%s (0xF601)\n", diff);
}

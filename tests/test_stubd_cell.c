// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd -K, the cell side's media (opendectcell-controller
 * docs/cell-protocol.md §8, §14.2): the daemon's files (stubd_unit.h) compiled in with
 * STUBD_TEST as for tests/test_stubd.c, dect-cell played on a socket pair
 * and the audio socket, the chip by the mails cell_mail_up() takes, the TDM
 * by cell_media_tx() one frame at a time.
 *
 * What it holds: MEDIA_* only while the cell is active; the lane found from
 * the long 0x0242 that confirmed the controller's bind (mask bit = channel
 * k, flag bit 6 = the G.722 lane); status 1, 2, 3 where §8.1 says; a
 * session's speech both ways through the audio socket, G.722 octets as
 * they are and 8 kHz linear; the snapshot naming open sessions; the chip's
 * release closing a session with MEDIA_CLOSED reason 1.
 */
#define main stubd_main
#include "stubd_unit.h"
#undef main

#include "check.h"

uint64_t stubd_test_ms = 1000;
time_t stubd_test_wall = 1793350800;

void stubd_test_sent(const struct dect_mail *m)
{
	(void)m;
}

static int cellfd = -1;			/* dect-cell's end */

/* A frame from dect-cell to dect-stubd. */
static void to_stubd(struct stubd *s, uint16_t type, uint16_t corr,
		     const uint8_t *body, size_t len)
{
	uint8_t f[DECT_CELL_FRAME_MAX];
	size_t n = dect_cell_put(f, sizeof(f), type, corr, body, len);

	cell_frame(s, f, n, stubd_test_ms);
}

/*
 * The next frame dect-stubd sent dect-cell, past the chip's mails it
 * forwards: its type, or 0 for none.
 */
static uint16_t from_stubd(struct dect_cell_frame *f, uint8_t *buf,
			   size_t cap)
{
	for (;;) {
		ssize_t n = recv(cellfd, buf, cap, MSG_DONTWAIT);

		if (n <= 0 || dect_cell_parse(buf, (size_t)n, f) != n)
			return 0;
		if (f->type != DECT_CELL_MAIL_UP)
			return f->type;
	}
}

static void chip(struct stubd *s, uint16_t prim, const uint8_t *p,
		 size_t n)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = 0x02;
	m.dst = 0x22;
	m.prim = prim;
	m.param_len = (uint16_t)n;
	memcpy(m.param, p, n);
	cell_mail_up(s, &m, stubd_test_ms);
}

static void media_open(struct stubd *s, uint32_t id, uint8_t ch,
		       uint8_t fmt, int fmt_up)
{
	uint8_t b[32];
	struct dect_cell_tlv t;

	dect_cell_tlv_init(&t, b, sizeof(b));
	dect_cell_tlv_u32(&t, 1, id);
	dect_cell_tlv_u8(&t, 2, ch);
	dect_cell_tlv_u8(&t, 3, fmt);
	if (fmt_up >= 0)
		dect_cell_tlv_u8(&t, 13, (uint8_t)fmt_up);
	to_stubd(s, DECT_CELL_MEDIA_OPEN, 0x0300, b, t.len);
}

/* MEDIA_OPEN_ACK's status for id, or -1 (another frame or none). */
static int ack_status(uint32_t id)
{
	uint8_t buf[256];
	struct dect_cell_frame f;
	uint32_t got;
	uint16_t st;

	if (from_stubd(&f, buf, sizeof(buf)) != DECT_CELL_MEDIA_OPEN_ACK ||
	    dect_cell_tlv_get_u32(f.body, f.len, 1, &got) || got != id ||
	    dect_cell_tlv_get_u16(f.body, f.len, 2, &st))
		return -1;
	return st;
}

static int audio_connect(struct stubd *s, const char *hello)
{
	struct sockaddr_un a;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", AB_PATH);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    send(fd, hello, strlen(hello), 0) != (ssize_t)strlen(hello))
		return -1;
	ab_tick(s, stubd_test_ms);	/* accepted, and the hello read */
	return fd;
}

int stubd_test_run(struct stubd *s)
{
	int sp[2];
	uint8_t buf[DECT_CELL_FRAME_MAX], fr[16], in[16];
	struct dect_cell_frame f;
	unsigned w0 = s->audio.lane0_word, w2 = s->audio.lane_word[0];
	unsigned k, ok;
	int afd, bfd;
	uint32_t got;
	uint8_t reason;

	s->fd = -1;
	s->led_fd = -1;
	s->slots = 8;
	s->tx_slot = 1;
	s->rx_slot = 1;
	s->sess.state = DECT_SESS_UP;
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp) == 0);
	s->cell.cfd = sp[0];
	(void)fcntl(sp[0], F_SETFL, O_NONBLOCK);
	cellfd = sp[1];
	s->cell.lfd = -1;
	ab_listen(s);
	CHECK(s->ab.lfd >= 0);
	CHECK(w0 != (unsigned)s->tx_slot && w2 != (unsigned)s->tx_slot &&
	      w0 != w2 && w0 < 8u && w2 < 8u);

	/* §5.3: not before the controller made the cell active */
	media_open(s, 5, 1, DECT_CELL_FMT_G722, -1);
	CHECK(from_stubd(&f, buf, sizeof(buf)) == DECT_CELL_ERROR);
	{
		const uint8_t st[] = { 1, 0, 1, 1 };	/* tag 1: active */

		to_stubd(s, DECT_CELL_CELL_STATE, 0, st, sizeof(st));
	}
	CHECK(s->cell.active);

	/* no bind on any live handle: unknown channel */
	media_open(s, 5, 1, DECT_CELL_FMT_G722, -1);
	CHECK(ack_status(5) == 1);

	/* handle 1 up, the controller's wideband bind confirmed: channel 1 */
	chip(s, DECT_PRIM_CONN_UP, (const uint8_t *)"\x01", 1);
	chip(s, 0x0242u, (const uint8_t *)"\x01\x41\x02\x00", 4);
	media_open(s, 5, 1, 3 /* PCMA */, -1);
	CHECK(ack_status(5) == 2);
	media_open(s, 5, 1, DECT_CELL_FMT_G722, DECT_CELL_FMT_L16_8K);
	CHECK(ack_status(5) == 2);
	media_open(s, 0, 1, DECT_CELL_FMT_G722, -1);	/* media_id 0 is fine */
	CHECK(ack_status(0) == 0);
	CHECK(s->cell.media[0].open && s->cell.media[0].word == w0 &&
	      s->ab.l[0].wideband);
	media_open(s, 0, 1, DECT_CELL_FMT_G722, -1);	/* the same id again */
	CHECK(ack_status(0) == 3);

	/*
	 * Its speech: three frames of G.722 octets down (40 ms primes the
	 * lane), each pair on the lane for two frames; the handset's words,
	 * each held two frames, up as 160 octets.
	 */
	afd = audio_connect(s, "media=0 codec=g722");
	CHECK(afd >= 0 && s->ab.l[0].cfd >= 0 && s->ab.l[0].g722);
	for (k = 0; k < 3u; k++) {
		uint8_t g[160];
		unsigned j;

		for (j = 0; j < 160u; j++)
			g[j] = (uint8_t)(k * 160u + j);
		CHECK(send(afd, g, sizeof(g), 0) == (ssize_t)sizeof(g));
	}
	ab_tick(s, stubd_test_ms);
	ok = 1;
	for (k = 0; k < 2u * 240u; k++) {
		unsigned pair = k / 2u;

		memset(fr, 0xEE, sizeof(fr));
		memset(in, 0, sizeof(in));
		in[w0 * 2u] = (uint8_t)(0x10u + pair);	/* live, changing */
		in[w0 * 2u + 1u] = 0x20;
		cell_media_tx(s, fr, in);
		if (fr[w0 * 2u] != (uint8_t)(2u * pair) ||
		    fr[w0 * 2u + 1u] != (uint8_t)(2u * pair + 1u))
			ok = 0;
		if (fr[w2 * 2u] != 0xEE)	/* another lane: left alone */
			ok = 0;
	}
	CHECK(ok);
	{
		uint8_t up[200];
		ssize_t n = recv(afd, up, sizeof(up), MSG_DONTWAIT);

		/* the first word pair locks; from then on one per word */
		CHECK(n == 160);
		CHECK(n == 160 && up[1] == 0x20 && up[3] == 0x20 &&
		      (uint8_t)(up[2] - up[0]) == 1u);
	}

	/* a narrowband call on channel 2 of handle 2: lane_ch[0], 8 kHz */
	chip(s, DECT_PRIM_CONN_UP, (const uint8_t *)"\x02", 1);
	chip(s, 0x0242u, (const uint8_t *)"\x02\x02\x04\x00", 4);
	media_open(s, 7, 2, DECT_CELL_FMT_L16_8K, DECT_CELL_FMT_L16_8K);
	CHECK(ack_status(7) == 0);
	CHECK(s->cell.media[1].open && s->cell.media[1].word == w2 &&
	      !s->ab.l[1].wideband);
	bfd = audio_connect(s, "media=7 rate=8000");
	CHECK(bfd >= 0 && s->ab.l[1].cfd >= 0 && s->ab.l[1].rate == 8000u);
	for (k = 0; k < 3u; k++) {
		int16_t pcm[160];
		unsigned j;

		for (j = 0; j < 160u; j++)
			pcm[j] = (int16_t)(1000 + (int)(k * 160u + j));
		CHECK(send(bfd, pcm, sizeof(pcm), 0) == (ssize_t)sizeof(pcm));
	}
	ab_tick(s, stubd_test_ms);
	ok = 1;
	for (k = 0; k < 480u; k++) {
		memset(fr, 0, sizeof(fr));
		memset(in, 0, sizeof(in));
		cell_media_tx(s, fr, in);
		if ((fr[w2 * 2u] | fr[w2 * 2u + 1u] << 8) != 1000 + (int)k)
			ok = 0;
	}
	CHECK(ok);

	/* the snapshot after a resume names both sessions */
	to_stubd(s, DECT_CELL_STATE_SNAPSHOT, 9, NULL, 0);
	while (from_stubd(&f, buf, sizeof(buf)) == DECT_CELL_MEDIA_OPEN_ACK)
		;
	{
		size_t off = 0, n;
		const uint8_t *v;
		unsigned seen = 0;

		CHECK(f.type == DECT_CELL_STATE_SNAPSHOT && f.corr == 9);
		while ((v = dect_cell_tlv_next(f.body, f.len, 4, &off, &n)))
			if (n == 4u)
				seen |= v[3] == 0 ? 1u : v[3] == 7 ? 2u : 4u;
		CHECK(seen == 3u);
	}

	/* the chip releases handle 1: MEDIA_CLOSED, reason 1, unasked */
	chip(s, DECT_PRIM_CONN_DOWN, (const uint8_t *)"\x01", 1);
	CHECK(from_stubd(&f, buf, sizeof(buf)) == DECT_CELL_MEDIA_CLOSED &&
	      !dect_cell_tlv_get_u32(f.body, f.len, 1, &got) && got == 0 &&
	      !dect_cell_tlv_get_u8(f.body, f.len, 2, &reason) &&
	      reason == 1);
	CHECK(!s->cell.media[0].open && s->ab.l[0].cfd < 0 &&
	      s->cell.media_n == 1u);
	{
		ssize_t n;	/* the rest of the uplink, then the end */

		while ((n = recv(afd, buf, sizeof(buf), MSG_DONTWAIT)) == 160)
			;
		CHECK(n == 0);
	}

	/* MEDIA_CLOSE: closed, reason 0; and again, closed all the same */
	{
		const uint8_t c[] = { 1, 0, 4, 0, 0, 0, 7 };

		to_stubd(s, DECT_CELL_MEDIA_CLOSE, 0x31, c, sizeof(c));
		CHECK(from_stubd(&f, buf, sizeof(buf)) ==
		      DECT_CELL_MEDIA_CLOSED &&
		      !dect_cell_tlv_get_u8(f.body, f.len, 2, &reason) &&
		      reason == 0);
		CHECK(s->cell.media_n == 0u && s->ab.l[1].cfd < 0);
		to_stubd(s, DECT_CELL_MEDIA_CLOSE, 0x32, c, sizeof(c));
		CHECK(from_stubd(&f, buf, sizeof(buf)) ==
		      DECT_CELL_MEDIA_CLOSED && f.corr == 0x32);
	}
	close(afd);
	close(bfd);
	ab_close(s);
	close(sp[0]);
	close(sp[1]);
	return 0;
}

int main(void)
{
	char *argv[] = { "dect-stubd", "-K", NULL };

	int fd, saved = dup(1);

	if (mkdir(DECT_RUN_DIR, 0700) && errno != EEXIST)
		perror(DECT_RUN_DIR);
	/* the daemon's own log, out of the way */
	fd = open("build/test_stubd_cell.log", O_WRONLY | O_CREAT | O_TRUNC,
		  0600);
	if (fd >= 0) {
		fflush(stdout);
		dup2(fd, 1);
		close(fd);
	}
	CHECK(stubd_main(2, argv) == 0);
	fflush(stdout);
	dup2(saved, 1);
	return CHECK_DONE();
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Daemon-level golden traces (docs/stack-in-library.md, stage S0): the
 * whole of dect-stubd above the chip transport, driven by a script that
 * stands in for the chip and the handsets, and every mail the daemon sends
 * compared with what tests/stubd-traces/ holds. The stack is moving into
 * libopendectcell; these traces say whether the move changed anything.
 *
 * dect-stubd.c is compiled into this file with STUBD_TEST: main() hands
 * its fully set-up state to stubd_test_run() where it would open the chip,
 * send_mail() hands each mail to stubd_test_sent(), and the clocks are
 * ours. The handset side is played as in tests/test_trace.c: I-frames with
 * N(S) and N(R) modulo 2, each of the base's I-frames acknowledged at once.
 * Identities and keys are made up.
 *
 * A trace is text, one action per line, '#' starts a comment:
 *
 *   sub N HEX       before start: a handset record for chip slot N with
 *                   this <<PORTABLE-IDENTITY>> content and a made-up UAK,
 *                   in RUN/sub.N, passed as -s
 *   store N         before start: an empty store RUN/sub.N, passed as -s,
 *                   for a handset to enrol into
 *   args ...        before start: more daemon arguments
 *   start           the daemon starts: main() with -g and the above
 *   up H            the chip: connection up on handle H (0x0200); H is
 *                   now the current handle
 *   on H            the following actions are on handle H
 *   pp HEX          the handset on the current handle sends this NWK message
 *   down            the chip: the current handle's connection is gone (0x0201)
 *   chip PRIM HEX   any other mail from the chip, PRIM in hex
 *   tick MS         MS milliseconds pass, the daemon's loop every 10 ms
 *   ctl LINE        a line on the control socket (docs/bridge-interface.md),
 *                   from the trace's client, connected at start
 *
 * After each action, what the daemon sent because of it:
 *
 *   fp HEX          a NWK message to the handset (0x0244, reassembled)
 *   mail PRIM HEX   any other mail, PRIM in hex, then its parameters
 *   ev LINE         a line the daemon wrote to the client (api=N: the
 *                   version is not compared)
 *
 * GOLDEN_UPDATE=1 rewrites the expectations from what the daemon sends
 * now; review the diff before committing it. The daemon's own log goes to
 * build/test_stubd.log.
 *
 *   build/test_stubd [TRACE...]     (default: every tests/stubd-traces/ *.trace)
 */
#define main stubd_main
#include "../src/stubd/dect-stubd.c"
#undef main

#include <ctype.h>
#include <dirent.h>

#include "check.h"

uint64_t stubd_test_ms = 1000;
time_t stubd_test_wall = 1793350800;	/* 2026-10-30 09:00:00 UTC */

#define T_LINE_MAX	1024u
#define T_OUT_MAX	64u
#define T_TEXT_MAX	600u
#define T_STEP_MS	10u

/* What the daemon sent since the last action, as trace lines. */
static char out_line[T_OUT_MAX][T_TEXT_MAX];
static unsigned n_out;

/* The handset on each handle: its LAPC, and the base's frames as they come. */
struct t_pp {
	uint8_t ns, nr, first;
	uint8_t msg[DECT_LAPC_MSG_MAX];	/* the base's NWK message so far */
	size_t msg_len;
};

static struct t_pp pps[8];
static unsigned cur_handle = 1;
static struct stubd *ts;
static FILE *trace;
static const char *trace_name;
static unsigned trace_line;
static int updating;
static FILE *upd;
static int client = -1;		/* the trace's control socket client */
static char client_in[4096];
static size_t client_len;

static void out_add(const char *line);

/* The daemon's lines to the client, as "ev" output. */
static void client_read(void)
{
	ssize_t r;
	char *nl;

	while (client >= 0 &&
	       (r = recv(client, client_in + client_len,
			 sizeof(client_in) - 1u - client_len, MSG_DONTWAIT)) > 0)
		client_len += (size_t)r;
	while ((nl = memchr(client_in, '\n', client_len))) {
		char line[T_TEXT_MAX], *api;
		size_t n = (size_t)(nl - client_in);

		snprintf(line, sizeof(line), "ev %.*s",
			 (int)(n < T_TEXT_MAX - 4u ? n : T_TEXT_MAX - 4u),
			 client_in);
		api = strstr(line, " api=");
		if (api) {
			char *e = api + 5;

			while (isdigit((unsigned char)*e))
				e++;
			memmove(api + 6, e, strlen(e) + 1u);
			api[5] = 'N';
		}
		out_add(line);
		memmove(client_in, nl + 1, client_len - n - 1u);
		client_len -= n + 1u;
	}
}

static size_t hex_parse(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while (*s && n < cap) {
		unsigned v;
		int k;

		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (sscanf(s, "%2x%n", &v, &k) != 1 || k != 2)
			return 0;
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static void hex_put(char *out, size_t cap, const uint8_t *b, size_t n)
{
	size_t i, at = strlen(out);

	for (i = 0; i < n && at + 4u < cap; i++)
		at += (size_t)snprintf(out + at, cap - at, " %02x", b[i]);
}

static void out_add(const char *line)
{
	if (n_out < T_OUT_MAX)
		snprintf(out_line[n_out++], T_TEXT_MAX, "%s", line);
}

static struct dect_link *t_link(unsigned h)
{
	unsigned k;

	if (ts->base.main_link.active && ts->base.main_link.handle == h)
		return &ts->base.main_link.link;
	for (k = 0; k < SIDE_MAX; k++)
		if (ts->side[k].bl->active && ts->side[k].bl->handle == h)
			return &ts->side[k].bl->link;
	return NULL;
}

static void t_chip_mail(uint16_t prim, const uint8_t *param, size_t n)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_CONTACT_DST;
	m.dst = DECT_TASK_PCM;
	m.prim = prim;
	m.param_len = (uint8_t)n;
	memcpy(m.param, param, n);
	dect_session_rx(&ts->sess, &m, stubd_test_ms);
	rfpi_check(ts, &m);		/* as run_loop() does */
	handle_reg_mail(ts, &m, stubd_test_ms);
}

/* One Lc frame from the handset, as 0x0203 segments on handle h. */
static void pp_frame(unsigned h, const uint8_t *lapc, size_t n)
{
	uint8_t cs[DECT_LC_FRAME_MAX], p[13];
	size_t cl = dect_lc_build(lapc, n, 0, cs, sizeof(cs)), off;

	CHECK(cl > 0);
	for (off = 0; off + 5u <= cl; off += 5u) {
		/* 07.56 and 08.25: {handle, 01, seg[5], 00, 00, tag[3], handle} */
		memset(p, 0, sizeof(p));
		p[0] = (uint8_t)h;
		p[1] = 0x01;
		memcpy(&p[2], cs + off, 5);
		p[12] = (uint8_t)h;
		t_chip_mail(0x0203u, p, sizeof(p));
	}
}

static void pp_send(unsigned h, const uint8_t *nwk, size_t n)
{
	struct t_pp *pp = &pps[h & 7u];
	uint8_t frame[DECT_LC_LAPC_MAX];
	size_t fl = dect_lapc_build_i(frame, sizeof(frame), 0, pp->first,
				      DECT_LAPC_LLN_A, pp->ns, pp->nr, 0, 0,
				      nwk, n);

	CHECK(fl > 0);
	pp->first = 0;
	pp->ns ^= 1u;
	pp_frame(h, frame, fl);
}

/* A 0x0244 from the daemon: decode its Lc frame, answer an I-frame. */
static void base_cplane(const struct dect_mail *m)
{
	unsigned h = m->param[0], n = m->param[1], off;
	struct dect_link *l = t_link(h);
	struct t_pp *pp = &pps[h & 7u];
	struct dect_lc_rx rx;
	uint8_t lapc[DECT_LC_LAPC_MAX];
	struct dect_lapc_frame f;
	int got = 0;

	dect_lc_rx_init(&rx, l ? l->lc.lsig : 0);
	for (off = 0; off + 5u <= n && 2u + off + 5u <= m->param_len; off += 5u) {
		int r = dect_lc_rx_fragment(&rx, &m->param[2 + off], lapc,
					    sizeof(lapc));

		if (r > 0)
			got = r;
	}
	if (got <= 0 || dect_lapc_parse(lapc, (size_t)got, &f)) {
		char line[T_TEXT_MAX] = "mail 0244";

		hex_put(line, sizeof(line), m->param, m->param_len);
		out_add(line);		/* not a frame we can read: as it is */
		return;
	}
	if (f.type != DECT_LAPC_I)
		return;			/* the base's RR and RNR: LAPC's own */
	if (pp->msg_len + f.info_len <= sizeof(pp->msg)) {
		memcpy(pp->msg + pp->msg_len, f.info, f.info_len);
		pp->msg_len += f.info_len;
	}
	if (!f.more) {
		char line[T_TEXT_MAX] = "fp";

		hex_put(line, sizeof(line), pp->msg, pp->msg_len);
		out_add(line);
		pp->msg_len = 0;
	}
	{
		uint8_t rr[4];
		size_t rn;

		pp->nr = (uint8_t)((f.ns + 1u) & 1u);
		rn = dect_lapc_build_rr(rr, sizeof(rr), 0, 0, DECT_LAPC_LLN_A,
					pp->nr, 0);
		pp_frame(h, rr, rn);
	}
}

/* Every mail the daemon sends comes here (send_mail(), STUBD_TEST). */
static struct dect_mail sent_q[64];
static unsigned sent_n;

void stubd_test_sent(const struct dect_mail *m)
{
	if (sent_n < 64u)
		sent_q[sent_n++] = *m;
}

/* Run the daemon at this time until it sends nothing more. */
static void settle_now(void)
{
	int guard = 32;

	do {
		unsigned i, n;

		loop_tick(ts, stubd_test_ms);
		client_read();
		n = sent_n;
		sent_n = 0;
		for (i = 0; i < n; i++) {
			const struct dect_mail *m = &sent_q[i];

			if (m->prim == 0x0244u && m->param_len >= 2u) {
				base_cplane(m);
			} else {
				char line[T_TEXT_MAX];

				snprintf(line, sizeof(line), "mail %04x",
					 (unsigned)m->prim);
				hex_put(line, sizeof(line), m->param,
					m->param_len);
				out_add(line);
			}
		}
		if (!n && !client_len)
			break;
	} while (--guard > 0);
	CHECK(guard > 0);
}

static void advance(unsigned ms)
{
	unsigned t;

	for (t = 0; t < ms; t += T_STEP_MS) {
		stubd_test_ms += T_STEP_MS;
		settle_now();
	}
	stubd_test_wall += (time_t)(ms / 1000u);
}

/* The last action's output against the trace's lines (or into the update). */
static char want_line[T_OUT_MAX][T_TEXT_MAX];
static unsigned n_want;

static void compare(unsigned at)
{
	unsigned i;

	if (updating) {
		for (i = 0; i < n_out; i++)
			fprintf(upd, "%s\n", out_line[i]);
	} else {
		for (i = 0; i < n_out || i < n_want; i++) {
			const char *got = i < n_out ? out_line[i] : "(nothing)";
			const char *want = i < n_want ? want_line[i] : "(nothing)";

			if (strcmp(got, want)) {
				fprintf(stderr, "%s:%u: output %u\n  want %s\n"
					"  got  %s\n", trace_name, at, i + 1u,
					want, got);
				CHECK(0);
				break;
			}
		}
	}
	n_out = 0;
	n_want = 0;
}

static int is_output(const char *line)
{
	return !strncmp(line, "fp", 2) || !strncmp(line, "mail ", 5) ||
	       !strncmp(line, "ev ", 3);
}

/* The trace from "start" on, with the daemon set up. */
int stubd_test_run(struct stubd *s)
{
	char line[T_LINE_MAX];
	unsigned at = trace_line;
	int acted = 1;

	ts = s;
	s->fd = -1;
	s->led_fd = -1;
	s->slots = 8;
	s->tx_slot = 1;
	s->rx_slot = 1;
	s->tx_ring = 1;
	s->sess.state = DECT_SESS_UP;
	s->rand_fd = open(DECT_RUN_DIR "/random", O_RDONLY);
	g_start = stubd_test_ms;
	s->base.start_ms = stubd_test_ms;
	s->audio.ctl_path = DECT_RUN_DIR "/ctl.sock";
	ctl_open(s);
	{
		struct sockaddr_un a;

		memset(&a, 0, sizeof(a));
		a.sun_family = AF_UNIX;
		snprintf(a.sun_path, sizeof(a.sun_path), "%s", s->ctl.path);
		client = socket(AF_UNIX, SOCK_STREAM, 0);
		CHECK(client >= 0 &&
		      connect(client, (struct sockaddr *)&a, sizeof(a)) == 0);
		client_len = 0;
	}
	memset(pps, 0, sizeof(pps));
	/* the chip's contact answer carries its RFPI: made up, the PARK the
	 * traces' handsets name (a0 a5 00 01 23 45 60) */
	t_chip_mail(DECT_PRIM_CONTACT_ACK1,
		    (const uint8_t *)"\x00\x01\x23\x45\x60", 5);
	settle_now();

	while (fgets(line, sizeof(line), trace)) {
		char *c = strchr(line, '#'), *arg;
		uint8_t b[DECT_LAPC_MSG_MAX];
		size_t n;

		trace_line++;
		if (!is_output(line)) {
			/* any other line: the last action's output is complete */
			if (acted)
				compare(at);
			acted = 0;
			if (updating)
				fputs(line, upd);
		}
		if (c)
			*c = 0;
		for (n = strlen(line); n && isspace((unsigned char)line[n - 1]); )
			line[--n] = 0;
		if (!line[0])
			continue;
		if (is_output(line)) {
			if (!updating && n_want < T_OUT_MAX)
				snprintf(want_line[n_want++], T_TEXT_MAX, "%.*s",
					 (int)T_TEXT_MAX - 1, line);
			continue;
		}
		at = trace_line;
		acted = 1;
		arg = strchr(line, ' ');
		arg = arg ? arg + 1 : line + strlen(line);
		if (!strncmp(line, "up ", 3)) {
			uint8_t h = (uint8_t)atoi(arg);

			cur_handle = h;
			memset(&pps[h & 7u], 0, sizeof(pps[0]));
			pps[h & 7u].first = 1;
			t_chip_mail(DECT_PRIM_CONN_UP, &h, 1);
			settle_now();
		} else if (!strncmp(line, "chip ", 5)) {
			char *sp;
			unsigned prim = (unsigned)strtoul(arg, &sp, 16);

			n = hex_parse(sp, b, sizeof(b));
			t_chip_mail((uint16_t)prim, b, n);
			settle_now();
		} else if (!strncmp(line, "on ", 3)) {
			cur_handle = (unsigned)atoi(arg);
			acted = 0;
		} else if (!strcmp(line, "down")) {
			uint8_t h = (uint8_t)cur_handle;

			t_chip_mail(DECT_PRIM_CONN_DOWN, &h, 1);
			settle_now();
		} else if (!strncmp(line, "pp ", 3)) {
			n = hex_parse(arg, b, sizeof(b));
			CHECK(n > 0);
			pp_send(cur_handle, b, n);
			settle_now();
		} else if (!strncmp(line, "tick ", 5)) {
			advance((unsigned)atoi(arg));
		} else if (!strncmp(line, "ctl ", 4)) {
			char l2[T_LINE_MAX + 1];
			size_t ln = (size_t)snprintf(l2, sizeof(l2), "%s\n", arg);

			CHECK(send(client, l2, ln, MSG_NOSIGNAL) == (ssize_t)ln);
			settle_now();
		} else {
			fprintf(stderr, "%s:%u: unknown action \"%s\"\n",
				trace_name, trace_line, line);
			CHECK(0);
			acted = 0;
		}
	}
	if (acted)
		compare(at);
	if (client >= 0)
		close(client);
	client = -1;
	ctl_close(s);
	if (s->rand_fd >= 0)
		close(s->rand_fd);
	return 0;
}

/* A made-up handset record, as sub_save() writes one. */
static void write_sub(unsigned slot, const char *hex, char *path, size_t cap)
{
	struct sub_record r;
	uint8_t id[DECT_MM_PORTID_MAX];
	size_t n = hex_parse(hex, id, sizeof(id)), k;
	FILE *f;

	CHECK(n > 0);
	memset(&r, 0, sizeof(r));
	r.magic = SUB_MAGIC2;
	r.slot = (uint8_t)slot;
	for (k = 0; k < DECT_MM_UAK_LEN; k++)
		r.uak[k] = (uint8_t)(0xA0u + slot * 16u + k);
	r.port_id_len = (uint8_t)n;
	memcpy(r.port_id, id, n);
	/* the call identities, as tests/test_trace.c's setup uses them */
	r.cc_port_id_len = (uint8_t)n;
	memcpy(r.cc_port_id, id, n);
	r.cc_fixed_id_len = 5;
	memcpy(r.cc_fixed_id, "\x00\x01\x23\x45\x60", 5);
	snprintf(path, cap, DECT_RUN_DIR "/sub.%u", slot);
	f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		CHECK(fwrite(&r, sizeof(r), 1, f) == 1);
		fclose(f);
	}
}

static void prepare_run_dir(void)
{
	FILE *f;
	unsigned i;

	if (system("rm -rf " DECT_RUN_DIR " && mkdir -p " DECT_RUN_DIR) != 0)
		CHECK(0);
	f = fopen(DECT_STUB_RECORD, "w");
	if (f) {
		fprintf(f, "%s\n", DECT_STUB_0825);	/* the 7520's build */
		fclose(f);
		CHECK(chmod(DECT_STUB_RECORD, 0644) == 0);
	}
	f = fopen(DECT_RUN_DIR "/random", "wb");
	for (i = 0; f && i < 65536u; i++)
		fputc((int)((i * 37u + 11u) & 0xFFu), f);
	if (f)
		fclose(f);
}

static void run_trace(const char *file)
{
	static char subs[6][300];
	char line[T_LINE_MAX], tmp[600];
	char *argv[64];
	char args[T_LINE_MAX] = "";
	int argc = 0, n_sub = 0, saved_out;

	trace = fopen(file, "r");
	CHECK(trace != NULL);
	if (!trace)
		return;
	trace_name = file;
	trace_line = 0;
	n_out = n_want = 0;
	if (updating) {
		snprintf(tmp, sizeof(tmp), "%s.new", file);
		upd = fopen(tmp, "w");
		CHECK(upd != NULL);
	}
	prepare_run_dir();
	stubd_test_ms = 1000;
	stubd_test_wall = 1793350800;

	argv[argc++] = "dect-stubd";
	argv[argc++] = "-g";
	argv[argc++] = "-P";
	argv[argc++] = "12345678";
	while (fgets(line, sizeof(line), trace)) {
		char *c;
		size_t n;

		trace_line++;
		if (updating)
			fputs(line, upd);
		c = strchr(line, '#');
		if (c)
			*c = 0;
		for (n = strlen(line); n && isspace((unsigned char)line[n - 1]); )
			line[--n] = 0;
		if (!line[0])
			continue;
		if (!strncmp(line, "sub ", 4) && n_sub < 6) {
			char *sp;
			unsigned slot = (unsigned)strtoul(line + 4, &sp, 10);

			write_sub(slot, sp, subs[n_sub], sizeof(subs[0]));
			argv[argc++] = "-s";
			argv[argc++] = subs[n_sub++];
		} else if (!strncmp(line, "store ", 6) && n_sub < 6) {
			snprintf(subs[n_sub], sizeof(subs[0]),
				 DECT_RUN_DIR "/sub.%u",
				 (unsigned)strtoul(line + 6, NULL, 10));
			argv[argc++] = "-s";
			argv[argc++] = subs[n_sub++];
		} else if (!strncmp(line, "args ", 5)) {
			char *tok;

			snprintf(args, sizeof(args), "%s", line + 5);
			for (tok = strtok(args, " "); tok && argc < 60;
			     tok = strtok(NULL, " "))
				argv[argc++] = tok;
		} else if (!strcmp(line, "start")) {
			break;
		} else {
			fprintf(stderr, "%s:%u: before start: \"%s\"\n", file,
				trace_line, line);
			CHECK(0);
		}
	}
	argv[argc] = NULL;
	optind = 1;
	fflush(stdout);
	saved_out = dup(1);
	{
		int fd = open("build/test_stubd.log",
			      O_WRONLY | O_CREAT | O_APPEND, 0644);

		if (fd >= 0) {
			dup2(fd, 1);
			close(fd);
		}
	}
	printf("==== %s\n", file);
	CHECK_EQ(stubd_main(argc, argv), 0);
	fflush(stdout);
	dup2(saved_out, 1);
	close(saved_out);
	fclose(trace);
	if (upd) {
		fclose(upd);
		upd = NULL;
		CHECK(rename(tmp, file) == 0);
		printf("%s: recorded\n", file);
	}
}

/*
 * One STUB -> host mail into lane `slot` of frame data from frame f on,
 * two bytes per frame: 0xAA, seq, L(LE), src dst prim(LE) params,
 * sum16(LE) of the L field and body. Returns the frame after it.
 */
static size_t probe_put(uint8_t *data, unsigned int slots, unsigned int slot,
			size_t f, uint8_t seq, uint16_t prim, size_t plen)
{
	uint8_t b[64];
	unsigned int L = (unsigned int)(2u + plen), sum = 0, i, total;

	memset(b, 0x11, sizeof(b));
	b[0] = 0xaa;
	b[1] = seq;
	b[2] = (uint8_t)L;
	b[3] = (uint8_t)(L >> 8);
	b[4] = 0x02;
	b[5] = 0x14;
	b[6] = (uint8_t)prim;
	b[7] = (uint8_t)(prim >> 8);
	total = 4u + L + 2u + 2u;
	for (i = 2; i < total - 2u; i++)
		sum += b[i];
	b[total - 2] = (uint8_t)sum;
	b[total - 1] = (uint8_t)(sum >> 8);
	if (total & 1u)
		b[total++] = 0xfe;
	for (i = 0; i < total; i += 2, f++)
		memcpy(data + (f * slots + slot) * 2u, b + i, 2);
	return f;
}

/*
 * The control-slot search on a freshly booted chip: the contact answer
 * comes right after the 0xFFF2 build stamp, late in a probe window longer
 * than the receive buffer (the 7362 SL, 2026-11-01). It is found there;
 * the stamp alone, and the other lanes, are no answer.
 */
static void test_probe_window(void)
{
	enum { SLOTS = 8, FRAMES = 1280 };	/* 40 blocks of 32 frames */
	static uint8_t data[FRAMES * SLOTS * 2];
	struct dect_mail m;
	size_t f;

	memset(data, 0xfe, sizeof(data));
	f = probe_put(data, SLOTS, 1, 700, 0, 0xfff2, 24);
	CHECK_EQ(slot_has_contact_ack(data, FRAMES, SLOTS, 1, &m), 0);
	probe_put(data, SLOTS, 1, f + 3, 1, DECT_PRIM_CONTACT_ACK1, 5);
	CHECK_EQ(slot_has_contact_ack(data, FRAMES, SLOTS, 1, &m), 1);
	CHECK_EQ(m.prim, DECT_PRIM_CONTACT_ACK1);
	CHECK_EQ(m.param_len, 5);
	CHECK_EQ(slot_has_contact_ack(data, FRAMES, SLOTS, 0, &m), 0);
	CHECK_EQ(slot_has_contact_ack(data, FRAMES, SLOTS, 2, &m), 0);
}

#define TRACE_DIR	"tests/stubd-traces"

int main(int argc, char **argv)
{
	const char *u = getenv("GOLDEN_UPDATE");
	int i, ran = 0;

	updating = u && *u == '1';
	setenv("TZ", "UTC", 1);
	tzset();
	unlink("build/test_stubd.log");
	test_probe_window();
	for (i = 1; i < argc; i++, ran++)
		run_trace(argv[i]);
	if (argc < 2) {
		DIR *d = opendir(TRACE_DIR);
		struct dirent *e;
		char path[512];

		CHECK(d != NULL);
		while (d && (e = readdir(d))) {
			size_t n = strlen(e->d_name);

			if (n < 7 || strcmp(e->d_name + n - 6, ".trace"))
				continue;
			snprintf(path, sizeof(path), TRACE_DIR "/%s", e->d_name);
			run_trace(path);
			ran++;
		}
		if (d)
			closedir(d);
	}
	CHECK(ran > 0);
	return CHECK_DONE();
}

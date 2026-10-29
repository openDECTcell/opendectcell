// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Golden traces (ROADMAP M8): scripted handset messages through the host
 * stack (dect_link: Cs, Lc, LAPC, MM, CC), and every NWK message the base
 * sends compared, byte for byte, with what tests/traces/ holds. A change to
 * anything we put on the air shows here, even when no unit test asserts it:
 * the A540 refused a CONNECT once over one changed element.
 *
 * The handset side is played properly: its I-frames carry N(S) and N(R)
 * modulo 2, and it acknowledges each of the base's I-frames at once, so a
 * queued second message goes out as on the air (class A, window one).
 * Identities are made up; no trace holds a real handset's identity or key.
 *
 * A trace is text, one action per line, '#' starts a comment:
 *
 *   hold HEX          MM holds this <<PORTABLE-IDENTITY>> content (slot 1)
 *   callstatus N      call status on (Part 3 handset) with call id N
 *   pp HEX            the handset sends this NWK message
 *   setup HEX         the base calls the handset (dect_cc_setup_id(); HEX
 *                     is its <<PORTABLE-IDENTITY>> content)
 *   release           the base releases the call
 *   tick MS           MS milliseconds pass
 *   fp HEX            the base's next NWK message, after the action above
 *
 * The fp lines after an action are exactly what that action makes the base
 * send, in order. GOLDEN_UPDATE=1 rewrites them from what the stack sends
 * now, for a deliberate change; review the diff before committing it.
 *
 *   build/test_trace [TRACE...]     (default: every tests/traces/ *.trace)
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dect_link.h"

#define MSG_MAX		256u
#define OUT_MAX		16u
#define LINE_MAX_	1024u

/* What the base sent since the last action. */
static uint8_t out_msg[OUT_MAX][MSG_MAX];
static size_t out_len[OUT_MAX];
static unsigned n_out;

static void tx_log(void *ctx, const uint8_t *msg, size_t len)
{
	(void)ctx;
	if (n_out < OUT_MAX && len <= MSG_MAX) {
		memcpy(out_msg[n_out], msg, len);
		out_len[n_out++] = len;
	}
}

/* The handset's LAPC: modulo-2 sequence numbers, NLF on its first frame. */
struct pp {
	struct dect_link *l;
	struct dect_lc_rx rx;
	uint8_t ns, nr, first;
	uint64_t now;
};

/* One Lc frame from the handset, cut into 0x0203 segments for handle 1. */
static void pp_frame(struct pp *p, const uint8_t *lapc, size_t n)
{
	uint8_t cs[DECT_LC_FRAME_MAX];
	size_t cl = dect_lc_build(lapc, n, 0, cs, sizeof(cs)), off;

	CHECK(cl > 0);
	for (off = 0; off + 5u <= cl; off += 5u) {
		static const uint8_t rand_f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		static const uint8_t rs[16] = { 9, 9, 9, 9, 9, 9, 9, 9,
						9, 9, 9, 9, 9, 9, 9, 9 };
		struct dect_mail m;

		memset(&m, 0, sizeof(m));
		m.prim = 0x0203u;
		m.param_len = 15;
		m.param[0] = 1;
		m.param[1] = 0x01;
		memcpy(&m.param[2], cs + off, 5);
		m.param[12] = 1;
		(void)dect_link_rx_segment(p->l, &m, p->now, rand_f, rs, NULL);
	}

}

/*
 * Everything the base transmits, until it has nothing more: each I-frame
 * acknowledged at once with an RR, as a handset does.
 */
static void pp_pump(struct pp *p)
{
	struct dect_mail m;
	int guard = 64;

	dect_link_tick(p->l, p->now);
	while (guard-- > 0 && dect_link_poll_append_mail(p->l, &m)) {
		uint8_t lapc[DECT_LC_LAPC_MAX];
		size_t n = m.param[1], off;
		int got = 0;

		dect_lc_rx_init(&p->rx, p->l->lc.lsig);
		for (off = 0; off + 5u <= n; off += 5u) {
			int r = dect_lc_rx_fragment(&p->rx, &m.param[2 + off],
						    lapc, sizeof(lapc));

			if (r > 0)
				got = r;
		}
		CHECK(got > 0);
		if (got > 0) {
			struct dect_lapc_frame f;

			if (dect_lapc_parse(lapc, (size_t)got, &f) == 0 &&
			    f.type == DECT_LAPC_I) {
				uint8_t rr[4];
				size_t rn;

				p->nr = (uint8_t)((f.ns + 1u) & 1u);
				rn = dect_lapc_build_rr(rr, sizeof(rr), 0, 0,
							DECT_LAPC_LLN_A, p->nr, 0);
				pp_frame(p, rr, rn);
			}
		}
		dect_link_tick(p->l, p->now);
	}
	CHECK(guard > 0);
}

static void pp_send(struct pp *p, const uint8_t *nwk, size_t n)
{
	uint8_t frame[DECT_LC_LAPC_MAX];
	size_t fl = dect_lapc_build_i(frame, sizeof(frame), 0, p->first,
				      DECT_LAPC_LLN_A, p->ns, p->nr, 0, 0, nwk, n);

	CHECK(fl > 0);
	p->first = 0;
	p->ns ^= 1u;
	pp_frame(p, frame, fl);
}

static size_t hex_parse(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while (*s) {
		unsigned v;

		if (isspace((unsigned char)*s)) {
			s++;
			continue;
		}
		if (n == cap || !isxdigit((unsigned char)s[0]) ||
		    !isxdigit((unsigned char)s[1]) || sscanf(s, "%2x", &v) != 1)
			return 0;
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static void hex_put(FILE *f, const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		fprintf(f, "%s%02x", i ? " " : "", b[i]);
}

/* Compare what the last action sent with its fp lines (or record them). */
static void settle(const char *file, unsigned at, uint8_t want[][MSG_MAX],
		   const size_t *want_len, unsigned n_want, FILE *upd)
{
	unsigned i;

	if (upd) {
		for (i = 0; i < n_out; i++) {
			fputs("fp ", upd);
			hex_put(upd, out_msg[i], out_len[i]);
			fputc('\n', upd);
		}
		return;
	}
	CHECK_EQ(n_out, n_want);
	for (i = 0; i < n_out && i < n_want; i++) {
		int same = out_len[i] == want_len[i] &&
			   !memcmp(out_msg[i], want[i], out_len[i]);

		CHECK(same);
		if (!same) {
			fprintf(stderr, "%s, action at line %u, message %u:\n"
				"  want ", file, at, i + 1);
			hex_put(stderr, want[i], want_len[i]);
			fputs("\n  got  ", stderr);
			hex_put(stderr, out_msg[i], out_len[i]);
			fputc('\n', stderr);
		}
	}
	if (n_out != n_want)
		fprintf(stderr, "%s, action at line %u: %u message(s) sent, "
			"the trace has %u\n", file, at, n_out, n_want);
}

static void run_trace(const char *file, int update)
{
	static uint8_t want[OUT_MAX][MSG_MAX];
	size_t want_len[OUT_MAX];
	unsigned n_want = 0, at = 0, lineno = 0;
	char line[LINE_MAX_], tmp[512];
	struct dect_mm_cfg cfg;
	struct dect_link l;
	struct pp p;
	FILE *f = fopen(file, "r"), *upd = NULL;
	int acted = 0;

	CHECK(f != NULL);
	if (!f)
		return;
	if (update) {
		snprintf(tmp, sizeof(tmp), "%s.new", file);
		upd = fopen(tmp, "w");
		CHECK(upd != NULL);
		if (!upd) {
			fclose(f);
			return;
		}
	}
	memset(&cfg, 0, sizeof(cfg));
	strcpy(cfg.pin, "12345678");
	dect_link_init(&l, 1, &cfg);
	dect_link_set_tx_log(&l, tx_log, NULL);
	memset(&p, 0, sizeof(p));
	p.l = &l;
	p.first = 1;
	p.now = 1000;
	n_out = 0;

	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, '#'), *arg;
		uint8_t b[MSG_MAX];
		size_t n;

		lineno++;
		if (upd && strncmp(line, "fp ", 3)) {
			/* recording: the last action's output right after it */
			if (acted) {
				settle(file, at, want, want_len, n_want, upd);
				acted = 0;
			}
			fputs(line, upd);
		}
		if (c)
			*c = 0;
		for (n = strlen(line); n && isspace((unsigned char)line[n - 1]); )
			line[--n] = 0;
		if (!line[0])
			continue;
		arg = strchr(line, ' ');
		arg = arg ? arg + 1 : line + strlen(line);
		if (!strncmp(line, "fp ", 3)) {
			if (!update && n_want < OUT_MAX) {
				want_len[n_want] = hex_parse(arg, want[n_want],
							     MSG_MAX);
				CHECK(want_len[n_want] > 0);
				n_want++;
			}
			continue;
		}
		/* a new action: the last one's output is complete */
		if (acted)
			settle(file, at, want, want_len, n_want, upd);
		n_want = 0;
		n_out = 0;
		at = lineno;
		acted = 1;
		if (!strncmp(line, "hold ", 5)) {
			n = hex_parse(arg, b, sizeof(b));
			CHECK(n > 0 && dect_mm_hold(&l.mm, b, n, 1, NULL, 0) == 0);
			acted = 0;
		} else if (!strncmp(line, "callstatus ", 11)) {
			dect_cc_set_call_status(&l.cc, 1, (uint8_t)atoi(arg));
			acted = 0;
		} else if (!strncmp(line, "pp ", 3)) {
			n = hex_parse(arg, b, sizeof(b));
			CHECK(n > 0);
			pp_send(&p, b, n);
			pp_pump(&p);
		} else if (!strncmp(line, "setup ", 6)) {
			static const uint8_t fixed[] = { 0x00, 0x01, 0x23,
							 0x45, 0x60 };

			n = hex_parse(arg, b, sizeof(b));
			CHECK(dect_cc_setup_id(&l.cc, b, n, fixed,
					       sizeof(fixed), 0x40, NULL) == 0);
			pp_pump(&p);
		} else if (!strcmp(line, "release")) {
			dect_cc_release(&l.cc);
			pp_pump(&p);
		} else if (!strncmp(line, "tick ", 5)) {
			p.now += (uint64_t)atoi(arg);
			pp_pump(&p);
		} else {
			fprintf(stderr, "%s:%u: unknown action \"%s\"\n", file,
				lineno, line);
			CHECK(0);
			acted = 0;
		}
	}
	if (acted)
		settle(file, at, want, want_len, n_want, upd);
	fclose(f);
	if (upd) {
		fclose(upd);
		CHECK(rename(tmp, file) == 0);
		printf("%s: recorded\n", file);
	}
}

#define TRACE_DIR	"tests/traces"

int main(int argc, char **argv)
{
	const char *u = getenv("GOLDEN_UPDATE");
	int i, update = u && *u == '1', ran = 0;

	for (i = 1; i < argc; i++, ran++)
		run_trace(argv[i], update);
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
			run_trace(path, update);
			ran++;
		}
		if (d)
			closedir(d);
	}
	CHECK(ran > 0);
	return CHECK_DONE();
}

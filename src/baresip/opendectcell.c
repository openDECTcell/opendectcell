// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * openDECTcell: the baresip module that is the bridge between dect-stubd and SIP
 * (docs/bridge-interface.md §5 step 4). One module, no separate daemon:
 *
 *   - a client of dect-stubd's control socket (/var/run/opendectcell.sock): it
 *     says "bridge", and from then on dect-stubd hands it the handsets'
 *     external calls;
 *   - the audio source and player "openDECTcell", fed through dect-stubd's
 *     audio socket: 20 ms frames of 16 kHz linear PCM both ways. The DECT
 *     side sets the pace -- each frame from the handset is handed to
 *     baresip as the microphone, and in the same breath one frame is
 *     pulled from baresip's player and sent to the handset;
 *   - the line map /var/run/opendectcell/lines ("<hs> <line> <aor>" per line,
 *     written by the init script from /etc/config/opendectcell): which account
 *     belongs to which handset. A call to an account rings its handset; a
 *     handset's calls go out on its Line 1;
 *   - the VoIP status /var/run/opendectcell/voip.json for the Monitor (Status >
 *     DECT, tab VoIP), rewritten every 2 s and on every registration
 *     event: each line's registration, and each call's codec, endpoints,
 *     data rates, loss, jitter and encryption.
 *
 * One bridged call at a time, as dect-stubd's bridge is today.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "dect_ctl.h"

#define CTL_PATH	"/var/run/opendectcell.sock"
#define LINES_PATH	"/var/run/opendectcell/lines"
#define STATUS_PATH	"/var/run/opendectcell/voip.json"
#define STATUS_MS	2000u
#define FRAME		320u	/* samples: 20 ms at 16 kHz */
#define RATE		16000u
#define RETRY_MS	2000u
#define MAX_LINES	12u
#define MAX_CALLS	4u

struct line {
	unsigned hs, line;
	char     aor[256];		/* sip:user@domain */
	/* its registration as the last event told it, for the status */
	const char *reg;		/* registering, registered, failed, ... */
	char     why[96];		/* the server's answer to a failure */
};

struct bcall {
	unsigned     id;		/* dect-stubd's call id, 0 = not yet */
	unsigned     hs;
	struct call *call;
	struct ua   *ua;
	int          out;		/* the handset called out */
	int          rang, hung;	/* remote-ringing sent; we hung up */
	int          answered;		/* the handset took a call-in */
	char         tok[16];		/* our sip= token for a call-in */
};

struct ausrc_st {
	struct ausrc_prm prm;
	ausrc_read_h    *rh;
	void            *arg;
	uint64_t         ts;
};

struct auplay_st {
	struct auplay_prm prm;
	auplay_write_h   *wh;
	void             *arg;
};

static struct line lines[MAX_LINES];
static unsigned n_lines;
static struct bcall calls[MAX_CALLS];
static unsigned next_tok;

static struct {
	int            fd;
	struct re_fhs *fhs;
	char           in[2 * DECT_CTL_LINE_MAX];
	size_t         il;
	struct tmr     retry;
} ctl = { .fd = -1 };

static struct {
	int            fd;
	struct re_fhs *fhs;
	unsigned       call;
} aud = { .fd = -1 };

static struct ausrc *ausrc;
static struct auplay *auplay;
static struct ausrc_st *src;		/* the call's, while it exists */
static struct auplay_st *play;
static struct tmr status_tmr;

static void ctl_connect(void *arg);

/* ---- the line map ------------------------------------------------------ */

static void lines_load(void)
{
	char buf[320];
	FILE *f = fopen(LINES_PATH, "r");

	n_lines = 0;
	if (!f) {
		warning("openDECTcell: %s: %m -- no handset has a line\n",
			LINES_PATH, errno);
		return;
	}
	while (n_lines < MAX_LINES && fgets(buf, sizeof(buf), f)) {
		struct line *l = &lines[n_lines];

		if (sscanf(buf, "%u %u %255s", &l->hs, &l->line, l->aor) == 3 &&
		    l->hs && (l->line == 1 || l->line == 2))
			n_lines++;
	}
	fclose(f);
	info("openDECTcell: %u line(s) in %s\n", n_lines, LINES_PATH);
}

/* The account's AOR up to its parameters: sip:user@domain. */
static int aor_is(const struct ua *ua, const char *aor)
{
	const char *a = account_aor(ua_account(ua));
	size_t n = strlen(aor);

	return a && !strncmp(a, aor, n) && (a[n] == '\0' || a[n] == ';');
}

static struct ua *line_ua(unsigned hs, unsigned line)
{
	struct le *le;
	unsigned i;

	for (i = 0; i < n_lines; i++) {
		if (lines[i].hs != hs || lines[i].line != line)
			continue;
		LIST_FOREACH(uag_list(), le) {
			struct ua *ua = le->data;

			if (aor_is(ua, lines[i].aor))
				return ua;
		}
	}
	return NULL;
}

static unsigned ua_hs(const struct ua *ua)
{
	unsigned i;

	for (i = 0; i < n_lines; i++)
		if (aor_is(ua, lines[i].aor))
			return lines[i].hs;
	return 0;
}

/* ---- calls --------------------------------------------------------------- */

static struct bcall *bc_by_id(unsigned id)
{
	unsigned i;

	for (i = 0; id && i < MAX_CALLS; i++)
		if (calls[i].call && calls[i].id == id)
			return &calls[i];
	return NULL;
}

static struct bcall *bc_by_call(const struct call *call)
{
	unsigned i;

	for (i = 0; call && i < MAX_CALLS; i++)
		if (calls[i].call == call)
			return &calls[i];
	return NULL;
}

static struct bcall *bc_by_tok(const char *tok)
{
	unsigned i;

	for (i = 0; tok && i < MAX_CALLS; i++)
		if (calls[i].call && !strcmp(calls[i].tok, tok))
			return &calls[i];
	return NULL;
}

static struct bcall *bc_new(void)
{
	unsigned i;

	for (i = 0; i < MAX_CALLS; i++)
		if (!calls[i].call) {
			memset(&calls[i], 0, sizeof(calls[i]));
			return &calls[i];
		}
	return NULL;
}

/* ---- the control socket -------------------------------------------------- */

static void ctl_send(struct dect_ctl_out *o)
{
	int n = dect_ctl_end(o);

	if (ctl.fd < 0 || n < 0)
		return;
	if (send(ctl.fd, o->buf, (size_t)n, MSG_NOSIGNAL) != n)
		warning("openDECTcell: control socket write failed\n");
}

static void ctl_call(const char *verb, unsigned id, const char *k,
		     const char *v)
{
	struct dect_ctl_out o;

	dect_ctl_begin(&o, verb);
	dect_ctl_add_uint(&o, "call", id);
	if (k)
		dect_ctl_add(&o, k, v);
	ctl_send(&o);
}

/* The SIP status of a closed call as remote-released's reason. */
static const char *reason_of(uint16_t scode)
{
	switch (scode) {
	case 486: case 600:		return "busy";
	case 404: case 410: case 480:
	case 484: case 604:		return "unreachable";
	case 603:			return "rejected";
	default:			return "normal";
	}
}

static void aud_close(void)
{
	if (aud.fd < 0)
		return;
	aud.fhs = fd_close(aud.fhs);
	close(aud.fd);
	aud.fd = -1;
	aud.call = 0;
	info("openDECTcell: audio socket closed\n");
}

/*
 * One 20 ms frame from the handset: the microphone, then the player's
 * frame back. Frames are split to the codec's packet time if it is not 20.
 */
static void aud_read(int flags, void *arg)
{
	int16_t up[FRAME], down[FRAME];
	ssize_t n;
	size_t off;

	(void)flags;
	(void)arg;
	n = recv(aud.fd, up, sizeof(up), MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		aud_close();
		return;
	}
	if (n != (ssize_t)sizeof(up))
		return;
	if (src) {
		size_t sc = src->prm.srate * src->prm.ptime / 1000u;

		for (off = 0; sc && off + sc <= FRAME; off += sc) {
			struct auframe af;

			auframe_init(&af, AUFMT_S16LE, up + off, sc,
				     src->prm.srate, 1);
			af.timestamp = src->ts;
			src->ts += (uint64_t)sc * 1000000u / src->prm.srate;
			src->rh(&af, src->arg);
		}
	}
	memset(down, 0, sizeof(down));
	if (play) {
		size_t sc = play->prm.srate * play->prm.ptime / 1000u;

		for (off = 0; sc && off + sc <= FRAME; off += sc) {
			struct auframe af;

			auframe_init(&af, AUFMT_S16LE, down + off, sc,
				     play->prm.srate, 1);
			play->wh(&af, play->arg);
		}
	}
	(void)send(aud.fd, down, sizeof(down), MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void aud_open(unsigned id, const char *path)
{
	struct sockaddr_un a;
	int fd;

	aud_close();
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	str_ncpy(a.sun_path, path, sizeof(a.sun_path));
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		warning("openDECTcell: audio socket %s: %m\n", path, errno);
		if (fd >= 0)
			close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (fd_listen(&aud.fhs, fd, FD_READ, aud_read, NULL)) {
		close(fd);
		return;
	}
	aud.fd = fd;
	aud.call = id;
	info("openDECTcell: call %u's audio connected\n", id);
}

static void call_out(unsigned id, unsigned hs, const char *number)
{
	struct ua *ua = line_ua(hs, 1);
	struct bcall *bc;
	struct pl pl;
	char *uri = NULL;
	int err;

	if (!ua || !number || !*number || !(bc = bc_new())) {
		warning("openDECTcell: call %u from DECT %u: %s\n", id, hs,
			!ua ? "no Line 1 account for this handset" :
			"no number");
		ctl_call("remote-released", id, "reason", "unreachable");
		return;
	}
	pl_set_str(&pl, number);
	err = account_uri_complete_strdup(ua_account(ua), &uri, &pl);
	if (!err)
		err = ua_connect(ua, &bc->call, NULL, uri, VIDMODE_OFF);
	mem_deref(uri);
	if (err) {
		warning("openDECTcell: call %u to %s: %m\n", id, number, err);
		bc->call = NULL;
		ctl_call("remote-released", id, "reason", "unreachable");
		return;
	}
	bc->id = id;
	bc->hs = hs;
	bc->ua = ua;
	bc->out = 1;
	info("openDECTcell: call %u: DECT %u calls %s\n", id, hs, number);
}

static void ctl_line(const char *line, size_t len)
{
	struct dect_ctl_msg m;
	struct bcall *bc;
	unsigned id = 0, hs = 0;

	if (dect_ctl_parse(line, len, &m))
		return;
	(void)dect_ctl_get_uint(&m, "call", 1, 127, &id);
	(void)dect_ctl_get_uint(&m, "hs", 1, 6, &hs);

	if (!strcmp(m.verb, "call-out")) {
		call_out(id, hs, dect_ctl_get(&m, "number"));
	} else if (!strcmp(m.verb, "call-in-ok")) {
		bc = bc_by_tok(dect_ctl_get(&m, "sip"));
		if (bc)
			bc->id = id;
	} else if (!strcmp(m.verb, "busy")) {
		bc = bc_by_tok(dect_ctl_get(&m, "sip"));
		if (bc) {
			bc->hung = 1;
			ua_hangup(bc->ua, bc->call, 486, "Busy Here");
			bc->call = NULL;
		}
	} else if (!strcmp(m.verb, "answered")) {
		bc = bc_by_id(id);
		if (bc && !bc->out) {
			bc->answered = 1;
			(void)ua_answer(bc->ua, bc->call, VIDMODE_OFF);
		}
	} else if (!strcmp(m.verb, "released")) {
		const char *by = dect_ctl_get(&m, "by");

		bc = bc_by_id(id);
		if (bc) {
			/*
			 * An unanswered call-in: nobody answered the page (by
			 * the base) is 480, the handset declined it is 486; an
			 * answered or outgoing call ends with a BYE or CANCEL.
			 */
			uint16_t sc = bc->out || bc->answered ? 0 :
				      by && !strcmp(by, "base") ? 480 : 486;

			bc->hung = 1;
			ua_hangup(bc->ua, bc->call, sc,
				  sc == 480 ? "Temporarily Unavailable" :
				  sc == 486 ? "Busy Here" : NULL);
			bc->call = NULL;
		}
		if (aud.call == id)
			aud_close();
	} else if (!strcmp(m.verb, "audio")) {
		if (bc_by_id(id) && dect_ctl_get(&m, "path"))
			aud_open(id, dect_ctl_get(&m, "path"));
	}
}

static void ctl_drop(void)
{
	unsigned i;

	if (ctl.fd < 0)
		return;
	ctl.fhs = fd_close(ctl.fhs);
	close(ctl.fd);
	ctl.fd = -1;
	ctl.il = 0;
	aud_close();
	/* the handsets' side is gone: so are their calls */
	for (i = 0; i < MAX_CALLS; i++)
		if (calls[i].call) {
			calls[i].hung = 1;
			ua_hangup(calls[i].ua, calls[i].call, 503, NULL);
			calls[i].call = NULL;
		}
	warning("openDECTcell: control socket lost -- reconnecting\n");
	tmr_start(&ctl.retry, RETRY_MS, ctl_connect, NULL);
}

static void ctl_read(int flags, void *arg)
{
	ssize_t n;
	char *nl;

	(void)flags;
	(void)arg;
	n = recv(ctl.fd, ctl.in + ctl.il, sizeof(ctl.in) - ctl.il - 1u,
		 MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		ctl_drop();
		return;
	}
	if (n < 0)
		return;
	ctl.il += (size_t)n;
	while ((nl = memchr(ctl.in, '\n', ctl.il)) != NULL) {
		size_t l = (size_t)(nl - ctl.in);

		ctl_line(ctl.in, l);
		if (ctl.fd < 0)
			return;
		memmove(ctl.in, nl + 1, ctl.il - l - 1u);
		ctl.il -= l + 1u;
	}
	if (ctl.il == sizeof(ctl.in) - 1u)
		ctl.il = 0;		/* a line too long: drop it */
}

static void ctl_connect(void *arg)
{
	static const char hello[] = "bridge\n";
	struct sockaddr_un a;
	int fd;

	(void)arg;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	str_ncpy(a.sun_path, CTL_PATH, sizeof(a.sun_path));
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    write(fd, hello, sizeof(hello) - 1) != (ssize_t)sizeof(hello) - 1) {
		if (fd >= 0)
			close(fd);
		tmr_start(&ctl.retry, RETRY_MS, ctl_connect, NULL);
		return;
	}
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (fd_listen(&ctl.fhs, fd, FD_READ, ctl_read, NULL)) {
		close(fd);
		tmr_start(&ctl.retry, RETRY_MS, ctl_connect, NULL);
		return;
	}
	ctl.fd = fd;
	lines_load();		/* the service may have rewritten it */
	info("openDECTcell: bridge connected to %s\n", CTL_PATH);
}

/* ---- baresip's events ---------------------------------------------------- */

static void call_in(struct ua *ua, struct call *call)
{
	struct dect_ctl_out o;
	struct bcall *bc;
	unsigned hs = ua_hs(ua);
	const char *peer = call_peeruri(call), *at, *colon;
	char num[64] = "";

	if (!hs)
		return;			/* not one of the handsets' accounts */
	if (ctl.fd < 0 || !(bc = bc_new())) {
		ua_hangup(ua, call, 486, "Busy Here");
		return;
	}
	/* the caller's number: the user part of the peer's URI */
	if (peer && (colon = strchr(peer, ':')) && (at = strchr(colon, '@')) &&
	    at > colon + 1)
		str_ncpy(num, colon + 1, (size_t)(at - colon) <= sizeof(num)
			 ? (size_t)(at - colon) : sizeof(num));
	bc->call = call;
	bc->ua = ua;
	bc->hs = hs;
	re_snprintf(bc->tok, sizeof(bc->tok), "b%u", ++next_tok);
	dect_ctl_begin(&o, "call-in");
	dect_ctl_add_uint(&o, "line", 1);
	dect_ctl_add_uint(&o, "hs", hs);
	dect_ctl_add(&o, "number", num);
	if (call_peername(call))
		dect_ctl_add(&o, "name", call_peername(call));
	dect_ctl_add(&o, "sip", bc->tok);
	ctl_send(&o);
	info("openDECTcell: call from %s for DECT %u\n", peer ? peer : "?", hs);
}

/* ---- the VoIP status ----------------------------------------------------- */

static struct line *line_of(const struct ua *ua)
{
	unsigned i;

	for (i = 0; i < n_lines; i++)
		if (aor_is(ua, lines[i].aor))
			return &lines[i];
	return NULL;
}

static void json_str(FILE *f, const char *v)
{
	fputc('"', f);
	for (; v && *v; v++) {
		unsigned char c = (unsigned char)*v;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static void json_sa(FILE *f, const struct sa *sa)
{
	char b[64];

	if (sa && sa_isset(sa, SA_ALL) && re_snprintf(b, sizeof(b), "%J", sa) > 0)
		json_str(f, b);
	else
		fputs("null", f);
}

static const char *state_name(enum call_state st)
{
	switch (st) {
	case CALL_STATE_INCOMING:	return "incoming";
	case CALL_STATE_OUTGOING:	return "calling";
	case CALL_STATE_RINGING:	return "ringing";
	case CALL_STATE_EARLY:		return "early media";
	case CALL_STATE_ESTABLISHED:	return "established";
	case CALL_STATE_TERMINATED:	return "ended";
	default:			return "other";
	}
}

static void status_call(FILE *f, const struct line *l, struct call *call)
{
	struct audio *au = call_audio(call);
	struct stream *strm = au ? audio_strm(au) : NULL;
	const struct aucodec *tx = au ? audio_codec(au, true) : NULL;
	const struct aucodec *rx = au ? audio_codec(au, false) : NULL;
	const struct sdp_media *m = strm ? stream_sdpmedia(strm) : NULL;
	const struct rtcp_stats *rs = strm ? stream_rtcp_stats(strm) : NULL;
	const char *enc = account_mediaenc(call_account(call));

	fprintf(f, "{\"hs\":%u,\"line\":%u,\"direction\":\"%s\",\"state\":\"%s\","
		"\"duration_s\":%u,\"peer\":", l->hs, l->line,
		call_is_outgoing(call) ? "out" : "in",
		state_name(call_state(call)), call_duration(call));
	json_str(f, call_peeruri(call));
	fprintf(f, ",\"transport\":\"%s\"", sip_transp_name(call_transp(call)));
	if (tx) {
		fprintf(f, ",\"codec\":");
		json_str(f, tx->name);
		fprintf(f, ",\"srate\":%u", tx->srate);
	}
	if (rx && rx != tx) {
		fprintf(f, ",\"codec_rx\":");
		json_str(f, rx->name);
	}
	fprintf(f, ",\"local\":");
	json_sa(f, m ? sdp_media_laddr(m) : NULL);
	fprintf(f, ",\"remote\":");
	json_sa(f, m ? sdp_media_raddr(m) : NULL);
	if (strm)
		fprintf(f, ",\"tx_bps\":%u,\"rx_bps\":%u,\"tx_packets\":%u,"
			"\"rx_packets\":%u", stream_metric_get_tx_bitrate(strm),
			stream_metric_get_rx_bitrate(strm),
			stream_metric_get_tx_n_packets(strm),
			stream_metric_get_rx_n_packets(strm));
	if (rs)
		fprintf(f, ",\"rx_lost\":%d,\"rx_jitter_us\":%u,\"tx_lost\":%d,"
			"\"rtt_us\":%u", rs->rx.lost, rs->rx.jit, rs->tx.lost,
			rs->rtt);
	fprintf(f, ",\"encryption\":");
	json_str(f, enc && *enc ? enc : "none");
	fprintf(f, ",\"secure\":%s}", strm && stream_is_secure(strm) ?
		"true" : "false");
}

/* Written to a temporary file and renamed, so a reader never sees half. */
static void status_write(void)
{
	FILE *f = fopen(STATUS_PATH ".tmp", "w");
	unsigned i, first = 1;
	struct le *le;

	if (!f)
		return;
	fprintf(f, "{\"time\":%lld,\"base\":%s,\"lines\":[",
		(long long)time(NULL), ctl.fd >= 0 ? "true" : "false");
	for (i = 0; i < n_lines; i++) {
		struct line *l = &lines[i];
		struct ua *ua = line_ua(l->hs, l->line);
		const char *reg = !ua ? "no account" :
			!account_regint(ua_account(ua)) ? "off" :
			ua_isregistered(ua) ? "registered" :
			l->reg ? l->reg : "registering";

		fprintf(f, "%s{\"hs\":%u,\"line\":%u,\"aor\":", i ? "," : "",
			l->hs, l->line);
		json_str(f, l->aor);
		fprintf(f, ",\"registration\":\"%s\"", reg);
		if (l->why[0] && !(ua && ua_isregistered(ua))) {
			fprintf(f, ",\"reason\":");
			json_str(f, l->why);
		}
		if (ua)
			fprintf(f, ",\"interval_s\":%u",
				account_regint(ua_account(ua)));
		fputc('}', f);
	}
	fprintf(f, "],\"calls\":[");
	LIST_FOREACH(uag_list(), le) {
		struct ua *ua = le->data;
		const struct line *l = line_of(ua);
		struct le *ce;

		if (!l)
			continue;
		LIST_FOREACH(ua_calls(ua), ce) {
			if (!first)
				fputc(',', f);
			status_call(f, l, ce->data);
			first = 0;
		}
	}
	fprintf(f, "]}\n");
	if (fclose(f) == 0)
		(void)rename(STATUS_PATH ".tmp", STATUS_PATH);
}

static void status_tick(void *arg)
{
	(void)arg;
	status_write();
	tmr_start(&status_tmr, STATUS_MS, status_tick, NULL);
}

/* A registration event: remembered for the status, written at once. */
static void status_reg(struct ua *ua, const char *state, const char *why)
{
	struct line *l = line_of(ua);

	if (!l)
		return;
	l->reg = state;
	str_ncpy(l->why, why ? why : "", sizeof(l->why));
	status_write();
}

static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	struct bcall *bc = bc_by_call(call);

	(void)arg;
	switch (ev) {
	case UA_EVENT_REGISTERING:
		status_reg(bevent_get_ua(event), "registering", NULL);
		break;
	case UA_EVENT_REGISTER_OK:
		status_reg(bevent_get_ua(event), "registered", NULL);
		break;
	case UA_EVENT_REGISTER_FAIL:
		status_reg(bevent_get_ua(event), "failed", bevent_get_text(event));
		break;
	case UA_EVENT_UNREGISTERING:
		status_reg(bevent_get_ua(event), "unregistering", NULL);
		break;
	case UA_EVENT_SIPSESS_CONN: {
		/*
		 * baresip 3 leaves accepting a new incoming session to the
		 * application (the menu module does it otherwise): accept it
		 * for the handsets' accounts, so CALL_INCOMING follows.
		 */
		const struct sip_msg *msg = bevent_get_msg(event);
		struct ua *ua = msg ? uag_find_msg(msg) : NULL;

		if (!ua || !ua_hs(ua)) {
			if (msg)
				(void)sip_treply(NULL, uag_sip(), msg, 404,
						 "Not Found");
			bevent_stop(event);
			break;
		}
		if (ctl.fd < 0) {
			(void)sip_treply(NULL, uag_sip(), msg, 503,
					 "Service Unavailable");
			bevent_stop(event);
			break;
		}
		if (ua_accept(ua, msg))
			warning("openDECTcell: could not accept the call\n");
		break;
	}
	case UA_EVENT_CALL_INCOMING:
		if (call && !bc)
			call_in(bevent_get_ua(event), call);
		break;
	case UA_EVENT_CALL_RINGING:
	case UA_EVENT_CALL_PROGRESS:
		if (bc && bc->out && !bc->rang) {
			bc->rang = 1;
			ctl_call("remote-ringing", bc->id, NULL, NULL);
		}
		break;
	case UA_EVENT_CALL_ESTABLISHED:
		if (bc && bc->out)
			ctl_call("remote-answered", bc->id, NULL, NULL);
		break;
	case UA_EVENT_CALL_CLOSED:
		if (!bc)
			break;
		if (!bc->hung && bc->id)
			ctl_call("remote-released", bc->id, "reason",
				 reason_of(call_scode(call)));
		if (aud.call && aud.call == bc->id)
			aud_close();
		bc->call = NULL;
		break;
	default:
		break;
	}
}

/* ---- the audio device ---------------------------------------------------- */

static void src_destructor(void *arg)
{
	if (src == arg)
		src = NULL;
}

static void play_destructor(void *arg)
{
	if (play == arg)
		play = NULL;
}

static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *device,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;

	(void)as;
	(void)device;
	(void)errh;
	if (prm->fmt != AUFMT_S16LE || prm->ch != 1 || prm->srate != RATE) {
		warning("openDECTcell: the source needs 16 kHz mono S16LE "
			"(ausrc_srate 16000, ausrc_channels 1)\n");
		return ENOTSUP;
	}
	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;
	st->prm = *prm;
	st->rh = rh;
	st->arg = arg;
	src = st;
	*stp = st;
	return 0;
}

static int play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;

	(void)ap;
	(void)device;
	if (prm->fmt != AUFMT_S16LE || prm->ch != 1 || prm->srate != RATE) {
		warning("openDECTcell: the player needs 16 kHz mono S16LE "
			"(auplay_srate 16000, auplay_channels 1)\n");
		return ENOTSUP;
	}
	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;
	st->prm = *prm;
	st->wh = wh;
	st->arg = arg;
	play = st;
	*stp = st;
	return 0;
}

/* ---- the module ---------------------------------------------------------- */

static int module_init(void)
{
	int err;

	err  = ausrc_register(&ausrc, baresip_ausrcl(), "opendectcell", src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "opendectcell",
			       play_alloc);
	err |= bevent_register(event_handler, NULL);
	if (err)
		return err;
	lines_load();
	tmr_init(&ctl.retry);
	ctl_connect(NULL);
	tmr_init(&status_tmr);
	status_tick(NULL);
	return 0;
}

static int module_close(void)
{
	tmr_cancel(&status_tmr);
	(void)unlink(STATUS_PATH);
	tmr_cancel(&ctl.retry);
	ctl_drop();
	tmr_cancel(&ctl.retry);
	bevent_unregister(event_handler);
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(opendectcell) = {
	"opendectcell",
	"application",
	module_init,
	module_close
};

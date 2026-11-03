// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * openDECTcell: the baresip module that is the bridge between dect-stubd and SIP
 * (docs/bridge-interface.md §5 step 4). One module, no separate daemon:
 *
 *   - a client of dect-stubd's control socket (/var/run/opendectcell.sock): it
 *     says "bridge", and from then on dect-stubd hands it the handsets'
 *     external calls;
 *   - the audio source and player "openDECTcell", fed through dect-stubd's
 *     audio socket, one connection per call: 20 ms frames of linear PCM
 *     both ways, at the rate of the call's SIP codec (8 kHz for G.711,
 *     16 kHz for G.722). The DECT side sets the pace -- each frame from
 *     the handset is handed to baresip as the microphone, and in the
 *     same breath one frame is pulled from baresip's player and sent to
 *     the handset. Each call's audio device is named after its slot here
 *     (audio_set_devicename()), so its source and player find it;
 *   - "G722" as a codec of its own that passes the coded octets through:
 *     the handset's G.722 goes to RTP as it is, and the far end's to the
 *     handset, so a wideband call is coded by the two phones alone.
 *     baresip's sample pipeline carries each octet as two equal samples
 *     (so a frame split at any sample still yields whole octets), and the
 *     audio connection carries the octets (call=N codec=g722); dect-stubd
 *     codes G.722 itself only for a narrowband handset. baresip's g722
 *     module is not loaded;
 *   - codecs matched end to end where signalling allows, so neither the
 *     box nor baresip converts (docs/bridge-interface.md §3): a
 *     narrowband handset's call offers G.711 only, a caller who offers
 *     no G.722 rings the handsets narrowband, an account whose handsets
 *     are all narrowband answers G.711, and the far end's codec goes
 *     back with remote-answered;
 *   - a Part 3 handset's parallel calls: hold and resume from dect-stubd
 *     become re-INVITEs (call_hold()), keys during a call DTMF
 *     (call_send_digit(), as the account's dtmfmode says);
 *   - voicemail: the message summary a VoIP profile with MWI subscribes to
 *     (baresip's mwi module, RFC 3842) goes to dect-stubd as "mwi" for
 *     every handset line the account is, which tells the handsets
 *     (TS 102 527-3 §7.4.1.2);
 *   - the line map /var/run/opendectcell-sip/lines ("<hs> <line> <aor>" per line,
 *     written by the init script from /etc/config/opendectcell): which accounts
 *     belong to which handsets. An account may be the line of several
 *     handsets: a call to it rings all of them, one leg each, and the
 *     first to answer takes it. A handset's calls go out on the line it
 *     chose, else its Line 1;
 *   - the VoIP status /var/run/opendectcell-sip/voip.json for the Monitor (Status >
 *     DECT, tab VoIP), rewritten every 2 s and on every registration
 *     event: each line's registration, and each call's codec, endpoints,
 *     data rates, loss, jitter and encryption.
 *
 * Up to MAX_CALLS calls at once; dect-stubd takes as many as the DECT
 * chip has audio channels (three).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "dect_ctl.h"
#include "dect_clip.h"
#include "peers.h"

#define CTL_PATH	"/var/run/opendectcell.sock"
#define LINES_PATH	"/var/run/opendectcell-sip/lines"
#define STATUS_PATH	"/var/run/opendectcell-sip/voip.json"
#define STATUS_MS	2000u
#define FRAME_MAX	320u	/* samples: 20 ms at 16 kHz */
#define RETRY_MS	2000u
#define MAX_LINES	12u
#define MAX_CALLS	8u	/* DECT calls: legs ringing, and talking */
#define RING_MS		90000u	/* a call-in's limit, above dect-stubd's 75 s */

struct line {
	unsigned hs, line;
	char     aor[256];		/* sip:user@domain */
	/* its registration as the last event told it, for the status */
	const char *reg;		/* registering, registered, failed, ... */
	char     why[96];		/* the server's answer to a failure */
};

/*
 * A slot is one DECT call: a handset's call out, or one leg of a call in
 * (one per handset the account rings; the legs share the SIP call). It is
 * taken while it has a SIP call or waits for dect-stubd's answer to its
 * call-in: a caller who gives up before call-in-ok leaves the slot waiting
 * (call NULL), so that call can still be released when the answer comes.
 */
struct ausrc_st;
struct auplay_st;

struct bcall {
	unsigned     id;		/* dect-stubd's call id, 0 = not yet */
	unsigned     hs, line;
	struct call *call;		/* NULL once the SIP side has ended */
	struct ua   *ua;
	int          out;		/* the handset called out */
	int          rang;		/* remote-ringing sent */
	int          answered;		/* the handset took a call-in */
	int          wait;		/* call-in sent, no answer yet */
	unsigned     seq;		/* the call-in's place in the order sent */
	char         tok[16];		/* our sip= token for a call-in */
	struct tmr   ring;		/* a call-in's RING_MS limit */
	/* its speech: the audio socket connection, baresip's two ends */
	int              afd;		/* -1 = none */
	char             apath[108];	/* the socket dect-stubd named */
	unsigned         rate;		/* the connection's: 8000 or 16000 */
	bool             g722;		/* ... carries G.722 octets, 16 kHz */
	struct re_fhs   *afhs;
	struct ausrc_st *src;
	struct auplay_st *play;
};

struct ausrc_st {
	struct bcall    *bc;
	ausrc_read_h    *rh;
	void            *arg;
	uint64_t         ts;
};

struct auplay_st {
	struct bcall     *bc;
	auplay_write_h   *wh;
	void             *arg;
};

static struct line lines[MAX_LINES];
static unsigned n_lines;
/* handsets that speak narrowband only, by slot (dect-stubd's hs lines) */
static bool hs_narrow[7];
static struct bcall calls[MAX_CALLS];

/*
 * An incoming SIP call for the base's call lists: who called, and whether
 * any handset took it. dect-stubd sees one call per handset it rang; only
 * here is the outcome over all of them known, so it goes to dect-stubd as
 * one "calllog" when the SIP call ends (docs/phonebook.md, Call lists).
 */
struct incall {
	struct call *call;		/* NULL = free */
	struct ua   *ua;
	char         num[DECT_CLIP_NUMBER_MAX + 1];
	char         name[DECT_CLIP_NAME_MAX + 1];
	int          answered;
};
static struct incall incalls[MAX_CALLS];
static unsigned next_tok;
static bool any_source;		/* config opendectcell_any_source: see sip_origin_ok() */
static struct dect_peers peers;	/* config opendectcell_peers: who may call an account that does not register */

static struct {
	int            fd;
	struct re_fhs *fhs;
	char           in[2 * DECT_CTL_LINE_MAX];
	size_t         il;
	struct tmr     retry;
	struct tmr     lost;		/* drops it from the main loop */
	int            broken;		/* half a line went out */
} ctl = { .fd = -1 };

static struct ausrc *ausrc;
static struct auplay *auplay;
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

/* The account's first line (its handset); NULL: none of the handsets'. */
static struct line *line_of(const struct ua *ua)
{
	unsigned i;

	for (i = 0; i < n_lines; i++)
		if (aor_is(ua, lines[i].aor))
			return &lines[i];
	return NULL;
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

/* A call-in waiting for its answer, by its sip= token. */
static struct bcall *bc_by_tok(const char *tok)
{
	unsigned i;

	for (i = 0; tok && i < MAX_CALLS; i++)
		if (calls[i].wait && !strcmp(calls[i].tok, tok))
			return &calls[i];
	return NULL;
}

/* The call-in sent first of those still waiting for an answer. */
static struct bcall *bc_oldest_wait(void)
{
	struct bcall *bc = NULL;
	unsigned i;

	for (i = 0; i < MAX_CALLS; i++)
		if (calls[i].wait &&
		    (!bc || (int)(calls[i].seq - bc->seq) < 0))
			bc = &calls[i];
	return bc;
}

static void aud_close(struct bcall *bc);

/* A free slot; its source and player, if baresip still holds them, stay. */
static struct bcall *bc_new(void)
{
	unsigned i;

	for (i = 0; i < MAX_CALLS; i++)
		if (!calls[i].call && !calls[i].wait) {
			struct bcall *bc = &calls[i];

			bc->id = bc->hs = 0;
			bc->ua = NULL;
			bc->out = bc->rang = bc->answered = 0;
			bc->seq = 0;
			bc->tok[0] = '\0';
			return bc;
		}
	return NULL;
}

static void bc_free(struct bcall *bc)
{
	tmr_cancel(&bc->ring);
	aud_close(bc);
	bc->call = NULL;
	bc->wait = 0;
}

/*
 * The call's audio device: its slot's number, which src_alloc() and
 * play_alloc() read back. Set before its audio starts (the answer, or
 * the far end's SDP).
 */
static void bc_device(struct bcall *bc)
{
	char dev[8];

	re_snprintf(dev, sizeof(dev), "%u", (unsigned)(bc - calls));
	if (audio_set_devicename(call_audio(bc->call), dev, dev))
		warning("openDECTcell: could not name call %u's audio device\n",
			bc->id);
}

/* Another slot still holds this SIP call: one of its legs. */
static int call_has_legs(const struct call *call)
{
	unsigned i;

	for (i = 0; call && i < MAX_CALLS; i++)
		if (calls[i].call == call)
			return 1;
	return 0;
}

/*
 * Frees the slot, telling dect-stubd nothing, and ends the SIP side if
 * this was its last leg. The slot goes first, so the CALL_CLOSED that
 * ua_hangup() emits at once no longer finds it.
 */
static void bc_end(struct bcall *bc, uint16_t scode, const char *reason)
{
	struct call *call = bc->call;
	struct ua *ua = bc->ua;

	bc_free(bc);
	if (call && !call_has_legs(call))
		ua_hangup(ua, call, scode, reason);
}

/* Frees every leg of the slot's SIP call, then ends that call once. */
static void call_end(struct bcall *bc, uint16_t scode, const char *reason)
{
	struct call *call = bc->call;
	unsigned i;

	for (i = 0; call && i < MAX_CALLS; i++)
		if (calls[i].call == call && &calls[i] != bc)
			bc_free(&calls[i]);
	bc_end(bc, scode, reason);
}

/* ---- the call lists ------------------------------------------------------ */

static int ctl_send(struct dect_ctl_out *o);

/*
 * One finished or placed call for dect-stubd's call lists: kind, number,
 * name, and legs=HS:LINE,... -- every handset that has the account as a
 * line, and which of its lines.
 */
static void calllog(const char *kind, const struct ua *ua, const char *num,
		    const char *name)
{
	struct dect_ctl_out o;
	char legs[64] = "";
	size_t n = 0;
	unsigned i;

	for (i = 0; i < n_lines && n + 8u < sizeof(legs); i++)
		if (aor_is(ua, lines[i].aor))
			n += (size_t)re_snprintf(legs + n, sizeof(legs) - n,
						 "%s%u:%u", n ? "," : "",
						 lines[i].hs, lines[i].line);
	if (!n)
		return;
	dect_ctl_begin(&o, "calllog");
	dect_ctl_add(&o, "kind", kind);
	if (num && *num)
		dect_ctl_add(&o, "number", num);
	if (name && *name)
		dect_ctl_add(&o, "name", name);
	dect_ctl_add(&o, "legs", legs);
	(void)ctl_send(&o);
}

static struct incall *incall_of(const struct call *call)
{
	unsigned i;

	for (i = 0; call && i < MAX_CALLS; i++)
		if (incalls[i].call == call)
			return &incalls[i];
	return NULL;
}

/* The SIP call ended: answered by a handset or missed. */
static void incall_end(const struct call *call)
{
	struct incall *ic = incall_of(call);

	if (!ic)
		return;
	calllog(ic->answered ? "accepted" : "missed", ic->ua, ic->num, ic->name);
	ic->call = NULL;
}

/* ---- the control socket -------------------------------------------------- */

static void ctl_drop(int reconnect);

static void ctl_lost(void *arg)
{
	(void)arg;
	ctl_drop(1);
}

/*
 * 0 when the whole line went out, -1 when none of it did (no socket, the
 * line over DECT_CTL_LINE_MAX, the socket full). Half a line leaves the
 * stream without a line boundary: nothing more is sent, and the socket is
 * dropped from the main loop, not under the caller's feet.
 */
static int ctl_send(struct dect_ctl_out *o)
{
	int n = dect_ctl_end(o);
	ssize_t w;

	if (ctl.fd < 0 || ctl.broken || n < 0) {
		if (n < 0)
			warning("openDECTcell: a control line over %u octets -- "
				"not sent\n", DECT_CTL_LINE_MAX);
		return -1;
	}
	w = send(ctl.fd, o->buf, (size_t)n, MSG_NOSIGNAL);
	if (w == n)
		return 0;
	warning("openDECTcell: control socket write failed\n");
	if (w > 0) {
		ctl.broken = 1;
		tmr_start(&ctl.lost, 0, ctl_lost, NULL);
	}
	return -1;
}

static int ctl_call(const char *verb, unsigned id, const char *k,
		    const char *v)
{
	struct dect_ctl_out o;

	dect_ctl_begin(&o, verb);
	dect_ctl_add_uint(&o, "call", id);
	if (k)
		dect_ctl_add(&o, k, v);
	return ctl_send(&o);
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

static void aud_close(struct bcall *bc)
{
	if (bc->afd < 0)
		return;
	bc->afhs = fd_close(bc->afhs);
	close(bc->afd);
	bc->afd = -1;
	info("openDECTcell: call %u's audio closed\n", bc->id);
}

/*
 * G.722 as it is, carried through baresip's sample pipeline: each octet
 * as two equal samples (frames split at any sample still give whole
 * octets), and back by taking every other sample.
 */
#define G722_OCTETS	160u		/* 20 ms */
#define G722_GAP	1		/* a message of one octet: nothing */

static void aud_read_g722(struct bcall *bc, const uint8_t *in, ssize_t n)
{
	int16_t s[2u * G722_OCTETS];
	uint8_t out[G722_OCTETS];
	struct auframe af;
	unsigned i;
	bool gap = true;

	if (n != (ssize_t)G722_OCTETS)
		return;
	if (bc->src) {
		for (i = 0; i < G722_OCTETS; i++)
			s[2u * i] = s[2u * i + 1u] = in[i];
		auframe_init(&af, AUFMT_S16LE, s, 2u * G722_OCTETS, 16000u, 1);
		af.timestamp = bc->src->ts;
		bc->src->ts += 20000u;
		bc->src->rh(&af, bc->src->arg);
	}
	memset(s, 0, sizeof(s));
	if (bc->play) {
		auframe_init(&af, AUFMT_S16LE, s, 2u * G722_OCTETS, 16000u, 1);
		bc->play->wh(&af, bc->play->arg);
	}
	/*
	 * A frame of zeros is the player's silence (nothing received), not
	 * 160 octets of 00H: dect-stubd fills the gap with coded silence.
	 */
	for (i = 0; i < G722_OCTETS; i++) {
		out[i] = (uint8_t)s[2u * i];
		gap = gap && !s[2u * i];
	}
	(void)send(bc->afd, out, gap ? G722_GAP : sizeof(out),
		   MSG_DONTWAIT | MSG_NOSIGNAL);
}

struct auenc_state {
	int unused;
};

struct audec_state {
	int unused;
};

static int g722_encupd(struct auenc_state **aesp, const struct aucodec *ac,
		       struct auenc_param *prm, const char *fmtp)
{
	(void)ac;
	(void)prm;
	(void)fmtp;
	if (!*aesp)
		*aesp = mem_zalloc(sizeof(**aesp), NULL);
	return *aesp ? 0 : ENOMEM;
}

static int g722_decupd(struct audec_state **adsp, const struct aucodec *ac,
		       const char *fmtp)
{
	(void)ac;
	(void)fmtp;
	if (!*adsp)
		*adsp = mem_zalloc(sizeof(**adsp), NULL);
	return *adsp ? 0 : ENOMEM;
}

static int g722_enc(struct auenc_state *aes, bool *marker, uint8_t *buf,
		    size_t *len, int fmt, const void *sampv, size_t sampc)
{
	const int16_t *s = sampv;
	size_t i, n = sampc / 2u;

	(void)aes;
	(void)marker;
	if (fmt != AUFMT_S16LE)
		return ENOTSUP;
	if (n > *len)
		return EOVERFLOW;
	for (i = 0; i < n; i++)
		buf[i] = (uint8_t)s[2u * i];
	*len = n;
	return 0;
}

static int g722_dec(struct audec_state *ads, int fmt, void *sampv,
		    size_t *sampc, bool marker, const uint8_t *buf, size_t len)
{
	int16_t *s = sampv;
	size_t i;

	(void)ads;
	(void)marker;
	if (fmt != AUFMT_S16LE)
		return ENOTSUP;
	if (2u * len > *sampc)
		return EOVERFLOW;
	for (i = 0; i < len; i++)
		s[2u * i] = s[2u * i + 1u] = buf[i];
	*sampc = 2u * len;
	return 0;
}

/* RFC 3551: payload type 9, clock 8000 for a 16 kHz codec. */
static struct aucodec g722_through = {
	.pt      = "9",
	.name    = "G722",
	.srate   = 16000,
	.crate   = 8000,
	.ch      = 1,
	.pch     = 1,
	.encupdh = g722_encupd,
	.ench    = g722_enc,
	.decupdh = g722_decupd,
	.dech    = g722_dec,
};

/*
 * One 20 ms frame from the handset: the microphone, then the player's
 * frame back. Whole frames whatever the packet time: baresip buffers
 * both ways (an aubuf before the encoder and after the decoder).
 */
static void aud_read(int flags, void *arg)
{
	struct bcall *bc = arg;
	int16_t up[FRAME_MAX], down[FRAME_MAX];
	size_t sampc = bc->rate / 50u;
	struct auframe af;
	ssize_t n;

	(void)flags;
	n = recv(bc->afd, up, sizeof(up), MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		aud_close(bc);
		return;
	}
	if (bc->g722) {
		aud_read_g722(bc, (const uint8_t *)up, n);
		return;
	}
	if (n != (ssize_t)(sampc * sizeof(up[0])))
		return;
	/* at the connection's rate; auresamp covers a codec changed since */
	if (bc->src) {
		struct ausrc_st *src = bc->src;

		auframe_init(&af, AUFMT_S16LE, up, sampc, bc->rate, 1);
		af.timestamp = src->ts;
		src->ts += 20000u;
		src->rh(&af, src->arg);
	}
	memset(down, 0, sampc * sizeof(down[0]));
	if (bc->play) {
		auframe_init(&af, AUFMT_S16LE, down, sampc, bc->rate, 1);
		bc->play->wh(&af, bc->play->arg);
	}
	(void)send(bc->afd, down, sampc * sizeof(down[0]),
		   MSG_DONTWAIT | MSG_NOSIGNAL);
}

/*
 * The account's audio codecs as its audio_codecs parameter writes them;
 * narrow: only the 8 kHz ones. Empty when none qualifies.
 */
static void codecs_str(const struct account *acc, bool narrow, char *buf,
		       size_t sz)
{
	const struct le *le;
	size_t n = 0;

	buf[0] = '\0';
	for (le = list_head(account_aucodecl(acc)); le; le = le->next) {
		const struct aucodec *ac = le->data;

		if (narrow && ac->srate != 8000u)
			continue;
		n += (size_t)re_snprintf(buf + n, sz - n, "%s%s/%u/%u",
					 n ? "," : "", ac->name, ac->srate,
					 ac->ch);
		if (n >= sz - 1u)
			break;
	}
}

/*
 * A call that baresip creates now (ua_connect(), ua_accept()) offers or
 * answers only the account's narrowband codecs, when it has any: a call
 * keeps the formats it was made with, so narrow_end() puts the account's
 * list back right after. saved: at least 256 octets.
 */
static bool narrow_begin(struct account *acc, char *saved, size_t sz)
{
	char nb[256];

	codecs_str(acc, false, saved, sz);
	codecs_str(acc, true, nb, sizeof(nb));
	if (!nb[0] || !strcmp(nb, saved))
		return false;		/* nothing narrowband, or nothing else */
	return account_set_audio_codecs(acc, nb) == 0;
}

static void narrow_end(struct account *acc, const char *saved)
{
	if (account_set_audio_codecs(acc, saved))
		warning("openDECTcell: could not restore the account's codecs\n");
}

/* The call's SIP codec, once one is chosen. */
static const struct aucodec *call_codec(const struct call *call)
{
	return call_audio(call) ? audio_codec(call_audio(call), true) : NULL;
}

/* The rate of the call's SIP codec, 16000 until one is chosen. */
static unsigned call_rate(const struct call *call)
{
	const struct aucodec *ac = call_codec(call);

	return ac && ac->srate == 8000u ? 8000u : 16000u;
}

/*
 * Connect to the audio socket and name the call in the first message,
 * with the rate its frames take: the SIP codec's, so a narrowband call
 * to G.711 is resampled nowhere.
 */
static void aud_open(struct bcall *bc, const char *path)
{
	struct sockaddr_un a;
	char hello[32];
	int fd, n;

	aud_close(bc);
	if (path != bc->apath)
		str_ncpy(bc->apath, path, sizeof(bc->apath));
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	str_ncpy(a.sun_path, path, sizeof(a.sun_path));
	bc->rate = call_rate(bc->call);
	bc->g722 = call_codec(bc->call) == &g722_through;
	n = bc->g722 ? re_snprintf(hello, sizeof(hello), "call=%u codec=g722",
				   bc->id)
		     : re_snprintf(hello, sizeof(hello), "call=%u rate=%u",
				   bc->id, bc->rate);
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    send(fd, hello, (size_t)n, MSG_NOSIGNAL) != n) {
		warning("openDECTcell: audio socket %s: %m\n", path, errno);
		if (fd >= 0)
			close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (fd_listen(&bc->afhs, fd, FD_READ, aud_read, bc)) {
		close(fd);
		return;
	}
	bc->afd = fd;
	info("openDECTcell: call %u's audio connected (%s)\n", bc->id,
	     bc->g722 ? "G.722 as it is" : bc->rate == 8000u ? "8 kHz" : "16 kHz");
}

/*
 * A handset's call out; narrow: it offered no G.722 (dect-stubd's
 * codec=nb), so the INVITE offers G.711 only and nothing converts.
 */
static void call_out(unsigned id, unsigned hs, unsigned line,
		     const char *number, bool narrow)
{
	struct ua *ua = line_ua(hs, line);
	struct bcall *bc;
	struct pl pl;
	char *uri = NULL, saved[256];
	bool narrowed;
	int err;

	if (!ua && line != 1)
		ua = line_ua(hs, line = 1);
	if (!ua || !number || !*number || !(bc = bc_new())) {
		warning("openDECTcell: call %u from DECT %u: %s\n", id, hs,
			!ua ? "no Line 1 account for this handset" :
			!number || !*number ? "no number" : "no call free");
		ctl_call("remote-released", id, "reason", "unreachable");
		return;
	}
	pl_set_str(&pl, number);
	err = account_uri_complete_strdup(ua_account(ua), &uri, &pl);
	narrowed = narrow && narrow_begin(ua_account(ua), saved, sizeof(saved));
	if (!err)
		err = ua_connect(ua, &bc->call, NULL, uri, VIDMODE_OFF);
	if (narrowed)
		narrow_end(ua_account(ua), saved);
	mem_deref(uri);
	if (err) {
		warning("openDECTcell: call %u to %s: %m\n", id, number, err);
		bc->call = NULL;
		ctl_call("remote-released", id, "reason", "unreachable");
		return;
	}
	bc->id = id;
	bc->hs = hs;
	bc->line = line;
	bc->ua = ua;
	bc->out = 1;
	bc_device(bc);
	calllog("outgoing", ua, number, NULL);
	info("openDECTcell: call %u: DECT %u calls %s on Line %u\n", id, hs,
	     number, line);
}

static void ctl_line(const char *line, size_t len)
{
	struct dect_ctl_msg m;
	struct bcall *bc;
	unsigned id = 0, hs = 0, ln = 1;

	if (dect_ctl_parse(line, len, &m))
		return;
	(void)dect_ctl_get_uint(&m, "call", 1, 127, &id);
	(void)dect_ctl_get_uint(&m, "hs", 1, 6, &hs);
	(void)dect_ctl_get_uint(&m, "line", 1, 2, &ln);

	if (!strcmp(m.verb, "call-out")) {
		const char *c = dect_ctl_get(&m, "codec");

		call_out(id, hs, ln, dect_ctl_get(&m, "number"),
			 c && !strcmp(c, "nb"));
	} else if (!strcmp(m.verb, "hs")) {
		const char *wb = dect_ctl_get(&m, "wideband");

		if (hs)
			hs_narrow[hs] = wb && !strcmp(wb, "0");
	} else if (!strcmp(m.verb, "call-in-ok")) {
		bc = bc_by_tok(dect_ctl_get(&m, "sip"));
		if (bc && bc->call) {
			bc->id = id;
			bc->wait = 0;
		} else if (id) {
			/*
			 * The caller gave up before this came (an INVITE and
			 * its CANCEL at once), or the call is not ours: stop
			 * the paging now, not after dect-stubd's 75 s.
			 */
			(void)ctl_call("remote-released", id, "reason", "normal");
			if (bc)
				bc_free(bc);
		}
	} else if (!strcmp(m.verb, "busy")) {
		bc = bc_by_tok(dect_ctl_get(&m, "sip"));
		if (bc)
			bc_end(bc, 486, "Busy Here");
	} else if (!strcmp(m.verb, "error")) {
		const char *verb = dect_ctl_get(&m, "verb");
		const char *why = dect_ctl_get(&m, "reason");

		/*
		 * A refused call-in carries no sip= token, but dect-stubd
		 * answers every command in the order it came: the error is
		 * the oldest call-in still waiting. "?" is a line it could
		 * not parse; of ours only a call-in carries the caller's
		 * text, so that is a call-in too.
		 */
		if (verb && (!strcmp(verb, "call-in") || !strcmp(verb, "?")) &&
		    (bc = bc_oldest_wait()) != NULL) {
			int gone = why && (!strcmp(why, "no-handset") ||
					   !strcmp(why, "bad-hs"));

			warning("openDECTcell: call for DECT %u refused by the "
				"base (%s)\n", bc->hs, why ? why : "?");
			bc_end(bc, gone ? 480 : 503, gone ?
			       "Temporarily Unavailable" : "Service Unavailable");
		}
	} else if (!strcmp(m.verb, "answered")) {
		bc = bc_by_id(id);
		if (bc && !bc->out && !bc->answered) {
			struct incall *ic = incall_of(bc->call);
			unsigned i;

			if (ic)
				ic->answered = 1;
			/* the first to answer takes it; the other legs stop */
			for (i = 0; i < MAX_CALLS; i++) {
				struct bcall *o = &calls[i];

				if (o == bc || o->call != bc->call)
					continue;
				if (o->id) {
					(void)ctl_call("remote-released", o->id,
						       "reason", "normal");
					bc_free(o);
				} else {
					o->call = NULL;	/* call-in-ok releases it */
				}
			}
			bc->answered = 1;
			tmr_cancel(&bc->ring);
			bc_device(bc);
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

			/* an unanswered leg: the others may still ring */
			bc_end(bc, sc, sc == 480 ? "Temporarily Unavailable" :
				       sc == 486 ? "Busy Here" : NULL);
		}
	} else if (!strcmp(m.verb, "audio")) {
		bc = bc_by_id(id);
		if (bc && dect_ctl_get(&m, "path"))
			aud_open(bc, dect_ctl_get(&m, "path"));
	} else if (!strcmp(m.verb, "hold") || !strcmp(m.verb, "resume")) {
		/* the handset put the call on hold or took it back */
		bc = bc_by_id(id);
		if (bc && call_hold(bc->call, m.verb[0] == 'h'))
			warning("openDECTcell: call %u: could not %s it\n", id,
				m.verb);
	} else if (!strcmp(m.verb, "digit")) {
		/* a key during the call: DTMF as the account sends it */
		const char *d = dect_ctl_get(&m, "d");

		bc = bc_by_id(id);
		if (bc && d && d[0] && !d[1]) {
			(void)call_send_digit(bc->call, d[0]);
			(void)call_send_digit(bc->call, KEYCODE_REL);
		}
	}
}

/* Closes the control socket and ends its calls; reconnect: try again. */
static void ctl_drop(int reconnect)
{
	unsigned i;

	tmr_cancel(&ctl.lost);
	if (ctl.fd < 0)
		return;
	ctl.fhs = fd_close(ctl.fhs);
	close(ctl.fd);
	ctl.fd = -1;
	ctl.il = 0;
	ctl.broken = 0;
	/* the handsets' side is gone: so are their calls */
	for (i = 0; i < MAX_CALLS; i++)
		if (calls[i].call || calls[i].wait)
			call_end(&calls[i], 503, NULL);
	if (!reconnect)
		return;
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
		ctl_drop(1);
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

/*
 * An INVITE only from where the account registers: the address the last
 * 2xx to its REGISTER came from (the registrar, or the outbound proxy the
 * REGISTER went through), for each of its registrations. That is
 * baresip's own check, ua_req_check_origin(); it runs only for the
 * transports in the config's filter_registrar, and setting that there
 * would also stop listening on TCP and TLS, so it is switched on for this
 * one call. An account that does not register has no such address and is
 * has no such address: it takes an INVITE only from the peers the
 * administrator named (`list sip_peer`, config line `opendectcell_peers`), and
 * from nobody when none is named. `opendectcell_any_source yes` in the config
 * turns the check off, for a provider whose INVITEs come from elsewhere.
 */
static int sip_origin_ok(const struct ua *ua, const struct sip_msg *msg)
{
	struct config *cfg = conf_config();
	uint32_t keep;
	int ok;

	if (any_source)
		return 1;
	if (!account_regint(ua_account(ua))) {
		uint8_t a[16];

		if (sa_af(&msg->src) == AF_INET) {
			uint32_t v = htonl(sa_in(&msg->src));

			memcpy(a, &v, 4);
			return dect_peers_match(&peers, 4, a);
		}
		if (sa_af(&msg->src) == AF_INET6) {
			sa_in6(&msg->src, a);
			return dect_peers_match(&peers, 6, a);
		}
		return 0;
	}
	if (!cfg)
		return 1;
	keep = cfg->sip.reg_filt;
	cfg->sip.reg_filt = UINT32_MAX;
	ok = ua_req_check_origin(ua, msg);
	cfg->sip.reg_filt = keep;
	return ok;
}

/* s, cut to at most max octets without splitting a UTF-8 sequence. */
static void utf8_cut(char *dst, const char *s, size_t max)
{
	size_t n = strlen(s);

	if (n > max) {
		n = max;
		while (n && ((unsigned char)s[n] & 0xC0u) == 0x80u)
			n--;
	}
	memcpy(dst, s, n);
	dst[n] = '\0';
}

/*
 * A call-in nobody took in RING_MS: dect-stubd gives up after 75 s and
 * says so, this is for the answer or the release that never came.
 */
static void ring_timeout(void *arg)
{
	struct bcall *bc = arg;

	warning("openDECTcell: call %u for DECT %u: no answer in %u s -- ended\n",
		bc->id, bc->hs, RING_MS / 1000u);
	if (bc->id)
		(void)ctl_call("remote-released", bc->id, "reason", "normal");
	bc_end(bc, 480, "Temporarily Unavailable");
}

/* One leg of a call in: hand it to dect-stubd for line l's handset. */
static int call_in_leg(struct ua *ua, struct call *call, const struct line *l,
		       const char *num, const char *name, bool narrow)
{
	struct dect_ctl_out o;
	struct bcall *bc = bc_new();

	if (!bc)
		return -1;
	bc->call = call;
	bc->ua = ua;
	bc->hs = l->hs;
	bc->line = l->line;
	bc->seq = ++next_tok;
	re_snprintf(bc->tok, sizeof(bc->tok), "b%u", bc->seq);
	dect_ctl_begin(&o, "call-in");
	dect_ctl_add_uint(&o, "line", l->line);
	dect_ctl_add_uint(&o, "hs", l->hs);
	dect_ctl_add(&o, "number", num);
	if (name[0])
		dect_ctl_add(&o, "name", name);
	if (narrow)
		dect_ctl_add(&o, "codec", "nb");
	dect_ctl_add(&o, "sip", bc->tok);
	if (ctl_send(&o)) {
		bc->call = NULL;
		return -1;
	}
	bc->wait = 1;
	tmr_start(&bc->ring, RING_MS, ring_timeout, bc);
	return 0;
}

/* A call to an account: every handset it is the line of rings. */
static void call_in(struct ua *ua, struct call *call)
{
	const char *peer = call_peeruri(call), *at, *colon;
	/* what dect-stubd puts into the CLIP: longer, it drops the field */
	char num[DECT_CLIP_NUMBER_MAX + 1] = "", name[DECT_CLIP_NAME_MAX + 1] = "";
	struct stream *strm = call_audio(call) ? audio_strm(call_audio(call))
					       : NULL;
	unsigned i, legs = 0;
	bool narrow;

	if (!line_of(ua))
		return;			/* not one of the handsets' accounts */
	if (ctl.fd < 0) {
		ua_hangup(ua, call, 503, "Service Unavailable");
		return;
	}
	/* the caller's number: the user part of the peer's URI */
	if (peer && (colon = strchr(peer, ':')) && (at = strchr(colon, '@')) &&
	    at > colon + 1)
		str_ncpy(num, colon + 1, (size_t)(at - colon) <= sizeof(num)
			 ? (size_t)(at - colon) : sizeof(num));
	/*
	 * The caller's display name, at most the CLIP's 64 octets: longer,
	 * the value would be over DECT_CTL_VAL_MAX or the line over
	 * DECT_CTL_LINE_MAX, and the call-in refused.
	 */
	if (call_peername(call))
		utf8_cut(name, call_peername(call), DECT_CLIP_NAME_MAX);
	/* a caller who offers no G.722: the handsets ring narrowband */
	narrow = !strm || !sdp_media_rformat(stream_sdpmedia(strm), "G722");
	for (i = 0; i < n_lines; i++)
		if (aor_is(ua, lines[i].aor) &&
		    call_in_leg(ua, call, &lines[i], num, name, narrow) == 0)
			legs++;
	if (!legs) {
		warning("openDECTcell: call from %s: not handed to the base\n",
			peer ? peer : "?");
		ua_hangup(ua, call, 503, "Service Unavailable");
		return;
	}
	for (i = 0; i < MAX_CALLS; i++)
		if (!incalls[i].call) {
			incalls[i].call = call;
			incalls[i].ua = ua;
			incalls[i].answered = 0;
			str_ncpy(incalls[i].num, num, sizeof(incalls[i].num));
			str_ncpy(incalls[i].name, name, sizeof(incalls[i].name));
			break;
		}
	info("openDECTcell: call from %s rings %u handset(s)\n", peer ? peer : "?",
	     legs);
}

/* Every handset the account rings speaks narrowband only. */
static bool account_narrow(const struct ua *ua)
{
	unsigned i;
	bool any = false;

	for (i = 0; i < n_lines; i++) {
		if (!aor_is(ua, lines[i].aor))
			continue;
		if (lines[i].hs >= RE_ARRAY_SIZE(hs_narrow) ||
		    !hs_narrow[lines[i].hs])
			return false;
		any = true;
	}
	return any;
}

/*
 * The new voice messages in an RFC 3842 message summary: "Voice-Message:
 * new/old", else 1 or 0 from "Messages-Waiting: yes|no"; -1: neither.
 */
static int mwi_count(const char *body)
{
	struct pl yes, nw;

	if (!body)
		return -1;
	if (!re_regex(body, strlen(body), "Voice-Message:[ \t]*[0-9]+",
		      NULL, &nw))
		return (int)pl_u32(&nw);
	if (!re_regex(body, strlen(body), "Messages-Waiting:[ \t]*[a-z]+",
		      NULL, &yes))
		return !pl_strcasecmp(&yes, "yes");
	return -1;
}

/* A message summary for an account: to every handset line it is. */
static void mwi_notify(const struct ua *ua, const char *body)
{
	int n = mwi_count(body);
	unsigned i;

	if (n < 0)
		return;
	for (i = 0; i < n_lines; i++) {
		struct dect_ctl_out o;

		if (!aor_is(ua, lines[i].aor))
			continue;
		dect_ctl_begin(&o, "mwi");
		dect_ctl_add_uint(&o, "hs", lines[i].hs);
		dect_ctl_add_uint(&o, "line", lines[i].line);
		dect_ctl_add_uint(&o, "count", n > 16383 ? 16383u : (unsigned)n);
		(void)ctl_send(&o);
	}
	info("openDECTcell: %s: %d voice message(s) waiting\n",
	     account_aor(ua_account(ua)), n);
}

/* ---- the VoIP status ----------------------------------------------------- */

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

static void status_call(FILE *f, unsigned hs, unsigned line,
			struct call *call)
{
	struct audio *au = call_audio(call);
	struct stream *strm = au ? audio_strm(au) : NULL;
	const struct aucodec *tx = au ? audio_codec(au, true) : NULL;
	const struct aucodec *rx = au ? audio_codec(au, false) : NULL;
	const struct sdp_media *m = strm ? stream_sdpmedia(strm) : NULL;
	const struct rtcp_stats *rs = strm ? stream_rtcp_stats(strm) : NULL;
	const char *enc = account_mediaenc(call_account(call));

	fprintf(f, "{\"hs\":%u,\"line\":%u,\"direction\":\"%s\",\"state\":\"%s\","
		"\"duration_s\":%u,\"peer\":", hs, line,
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
			/*
			 * The handset that talks, else one still ringing,
			 * else the account's first line.
			 */
			unsigned hs = l->hs, ln = l->line;
			int talks = 0;

			for (i = 0; i < MAX_CALLS && !talks; i++)
				if (calls[i].call == ce->data) {
					hs = calls[i].hs;
					ln = calls[i].line;
					talks = calls[i].out ||
						calls[i].answered;
				}
			if (!first)
				fputc(',', f);
			status_call(f, hs, ln, ce->data);
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

/*
 * A registration event: remembered for the status of every line the
 * account is, written at once.
 */
static void status_reg(struct ua *ua, const char *state, const char *why)
{
	unsigned i;

	for (i = 0; i < n_lines; i++) {
		if (!aor_is(ua, lines[i].aor))
			continue;
		lines[i].reg = state;
		str_ncpy(lines[i].why, why ? why : "", sizeof(lines[i].why));
	}
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

		if (!ua || !line_of(ua)) {
			if (msg)
				(void)sip_treply(NULL, uag_sip(), msg, 404,
						 "Not Found");
			bevent_stop(event);
			break;
		}
		if (!sip_origin_ok(ua, msg)) {
			info("openDECTcell: INVITE from %J is not from where the "
			     "account registers or a named peer -- 403\n", &msg->src);
			(void)sip_treply(NULL, uag_sip(), msg, 403,
					 "Forbidden");
			bevent_stop(event);
			break;
		}
		if (ctl.fd < 0) {
			(void)sip_treply(NULL, uag_sip(), msg, 503,
					 "Service Unavailable");
			bevent_stop(event);
			break;
		}
		{
			char saved[256];
			bool narrowed = account_narrow(ua) &&
				narrow_begin(ua_account(ua), saved,
					     sizeof(saved));

			if (ua_accept(ua, msg))
				warning("openDECTcell: could not accept the call\n");
			if (narrowed)
				narrow_end(ua_account(ua), saved);
		}
		break;
	}
	case UA_EVENT_MWI_NOTIFY:
		mwi_notify(bevent_get_ua(event), bevent_get_text(event));
		break;
	case UA_EVENT_CALL_INCOMING:
		if (call && !bc)
			call_in(bevent_get_ua(event), call);
		break;
	case UA_EVENT_CALL_RINGING:
	case UA_EVENT_CALL_PROGRESS:
		if (bc && bc->out && !bc->rang) {
			bc->rang = 1;
			(void)ctl_call("remote-ringing", bc->id, NULL, NULL);
		}
		break;
	case UA_EVENT_CALL_ESTABLISHED:
		/* the far end's codec: a narrowband one connects the handset
		 * narrowband */
		if (bc && bc->out)
			(void)ctl_call("remote-answered", bc->id, "codec",
				       call_rate(call) == 8000u ? "nb" : "wb");
		break;
	case UA_EVENT_CALL_CLOSED: {
		unsigned i;

		incall_end(call);
		/* every leg of it: the handsets ringing, or the one talking */
		for (i = 0; call && i < MAX_CALLS; i++) {
			bc = &calls[i];
			if (bc->call != call)
				continue;
			if (bc->id)
				(void)ctl_call("remote-released", bc->id,
					       "reason",
					       reason_of(call_scode(call)));
			aud_close(bc);
			if (bc->wait)
				bc->call = NULL;  /* call-in-ok releases it */
			else
				bc_free(bc);
		}
		break;
	}
	default:
		break;
	}
}

/* ---- the audio device ---------------------------------------------------- */

static void src_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->bc && st->bc->src == st)
		st->bc->src = NULL;
}

static void play_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (st->bc && st->bc->play == st)
		st->bc->play = NULL;
}

/* The call slot an audio device names (bc_device()), or NULL. */
static struct bcall *device_bc(const char *device)
{
	unsigned i;

	if (!device || sscanf(device, "%u", &i) != 1 || i >= MAX_CALLS)
		return NULL;
	return &calls[i];
}

static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *device,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct bcall *bc = device_bc(device);
	struct ausrc_st *st;

	(void)as;
	(void)errh;
	if (!bc) {
		warning("openDECTcell: audio source \"%s\" is no call of ours\n",
			device ? device : "");
		return ENODEV;
	}
	if (prm->fmt != AUFMT_S16LE || prm->ch != 1) {
		warning("openDECTcell: the source needs mono S16LE "
			"(ausrc_channels 1)\n");
		return ENOTSUP;
	}
	/*
	 * baresip starts the source afresh when a re-INVITE changed the
	 * codec: the connection then carries the new one's format.
	 */
	if (bc->afd >= 0 && bc->call &&
	    (bc->g722 != (call_codec(bc->call) == &g722_through) ||
	     (!bc->g722 && bc->rate != call_rate(bc->call))))
		aud_open(bc, bc->apath);
	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;
	st->bc = bc;
	st->rh = rh;
	st->arg = arg;
	bc->src = st;
	*stp = st;
	return 0;
}

static int play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg)
{
	struct bcall *bc = device_bc(device);
	struct auplay_st *st;

	(void)ap;
	if (!bc) {
		warning("openDECTcell: audio player \"%s\" is no call of ours\n",
			device ? device : "");
		return ENODEV;
	}
	if (prm->fmt != AUFMT_S16LE || prm->ch != 1) {
		warning("openDECTcell: the player needs mono S16LE "
			"(auplay_channels 1)\n");
		return ENOTSUP;
	}
	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;
	st->bc = bc;
	st->wh = wh;
	st->arg = arg;
	bc->play = st;
	*stp = st;
	return 0;
}

/* ---- the module ---------------------------------------------------------- */

static int module_init(void)
{
	unsigned i;
	int err;

	aucodec_register(baresip_aucodecl(), &g722_through);
	err  = ausrc_register(&ausrc, baresip_ausrcl(), "opendectcell", src_alloc);
	err |= auplay_register(&auplay, baresip_auplayl(), "opendectcell",
			       play_alloc);
	err |= bevent_register(event_handler, NULL);
	if (err) {
		bevent_unregister(event_handler);
		ausrc = mem_deref(ausrc);
		auplay = mem_deref(auplay);
		aucodec_unregister(&g722_through);
		return err;
	}
	for (i = 0; i < MAX_CALLS; i++)
		calls[i].afd = -1;
	(void)conf_get_bool(conf_cur(), "opendectcell_any_source", &any_source);
	{
		char list[512];

		if (!conf_get_str(conf_cur(), "opendectcell_peers", list, sizeof(list)) &&
		    dect_peers_parse(&peers, list))
			warning("openDECTcell: opendectcell_peers has an entry that is not an "
				"address or a prefix; the good ones before it are used\n");
		if (peers.n)
			info("openDECTcell: %u peer(s) may call an account that does not register\n", peers.n);
	}
	if (any_source)
		warning("openDECTcell: opendectcell_any_source -- INVITEs taken from "
			"any address\n");
	lines_load();
	tmr_init(&ctl.retry);
	tmr_init(&ctl.lost);
	ctl_connect(NULL);
	tmr_init(&status_tmr);
	status_tick(NULL);
	return 0;
}

static int module_close(void)
{
	unsigned i;

	tmr_cancel(&status_tmr);
	(void)unlink(STATUS_PATH);
	tmr_cancel(&ctl.retry);
	ctl_drop(0);
	for (i = 0; i < MAX_CALLS; i++) {
		tmr_cancel(&calls[i].ring);	/* none may fire unloaded */
		aud_close(&calls[i]);
	}
	bevent_unregister(event_handler);
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	aucodec_unregister(&g722_through);
	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(opendectcell) = {
	"opendectcell",
	"application",
	module_init,
	module_close
};

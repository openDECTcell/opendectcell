// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, the control socket (/var/run/opendectcell.sock, docs/bridge-interface.md): clients,
 * commands, the bridge to the SIP side. Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/* functions used before their definition below */
static void bc_clear(struct stubd *s, struct dect_base_call *b);

static void ctl_drop(struct stubd *s, int c, const char *why)
{
	if (c < 0 || s->ctl.cl[c].fd < 0)
		return;
	close(s->ctl.cl[c].fd);
	s->ctl.cl[c].fd = -1;
	s->ctl.cl[c].in_len = 0;
	s->ctl.cl[c].discard = 0;
	if (c == s->ctl.bridge) {
		unsigned i;

		s->ctl.bridge = -1;
		s->base.bridge = 0;
		for (i = 0; i < BC_MAX; i++) {
			struct dect_base_call *b = &s->base.calls[i];
			int p = b->party;

			if (!b->id)
				continue;
			/* the far end went with the bridge */
			printf("bridge: call %u released -- the bridge left\n",
			       b->id);
			bc_clear(s, b);
			if (p != BC_PAGING && ic_active(s, p) &&
			    dect_cc_busy(ic_cc(s, p)))
				(void)dect_cc_release(ic_cc(s, p));
		}
	}
	printf("ctl: client %d %s\n", c + 1, why);
}

static void ctl_send_to(struct stubd *s, int c, struct dect_ctl_out *o)
{
	int n = dect_ctl_end(o);

	if (c < 0 || s->ctl.cl[c].fd < 0)
		return;
	if (n < 0) {
		printf("ctl: a line overflowed %u octets -- not sent\n",
		       DECT_CTL_LINE_MAX);
		return;
	}
	if (send(s->ctl.cl[c].fd, o->buf, (size_t)n,
		 MSG_DONTWAIT | MSG_NOSIGNAL) != n)
		ctl_drop(s, c, "not reading (or gone) -- closed");
}

/* To the client whose command is running: answers. */
static void ctl_send(struct stubd *s, struct dect_ctl_out *o)
{
	ctl_send_to(s, s->ctl.cur, o);
}

/* To the bridge: the calls it carries. */
void ctl_send_bridge(struct stubd *s, struct dect_ctl_out *o)
{
	ctl_send_to(s, s->ctl.bridge, o);
}

/* The handsets the base holds: the ones `hs` lines name. */
static int ctl_listed(const struct stubd *s, unsigned i)
{
	return dect_base_listed(&s->base, i);
}

/* One `hs` line to client c, or to every client with c < 0. */
static void ctl_hs_to(struct stubd *s, int c, unsigned i, const char *state)
{
	struct dect_ctl_out o;
	char nm[DECT_IWU_NAME_MAX];
	unsigned k;

	if (i >= s->base.n_hs)
		return;
	dect_ctl_begin(&o, "hs");
	dect_ctl_add_uint(&o, "hs", hs_slot(s, i));
	dect_ctl_add(&o, "state", state);
	if (strcmp(state, "gone")) {
		hs_name(s, i, nm, sizeof(nm));
		dect_ctl_add(&o, "name", nm);
		if (s->hs[i].prof_id[0])
			dect_ctl_add(&o, "model", s->hs[i].prof_id);
		if (s->base.cap[i].known)
			dect_ctl_add_uint(&o, "part3", s->base.cap[i].part3 ? 1 : 0);
		/* 0: we call it narrowband, so its account may answer G.711 */
		dect_ctl_add_uint(&o, "wideband", !s->base.cfg.narrowband &&
				  (target_svc(s, (int)i, s->base.cfg.basic_svc) &
				   0x0Fu) == 0x08u);
	}
	if (c >= 0) {
		ctl_send_to(s, c, &o);
		return;
	}
	for (k = 0; k < CTL_CLIENTS; k++) {
		struct dect_ctl_out copy = o;

		ctl_send_to(s, (int)k, &copy);
	}
}

void ctl_hs(struct stubd *s, unsigned i, const char *state)
{
	ctl_hs_to(s, -1, i, state);
}

static void ctl_hello(struct stubd *s)
{
	struct dect_ctl_out o;
	unsigned i, n = 0;

	for (i = 0; i < s->base.n_hs; i++)
		n += (unsigned)ctl_listed(s, i);
	dect_ctl_begin(&o, "hello");
	dect_ctl_add_uint(&o, "api", OPENDECTCELL_API_VERSION);
	dect_ctl_add_uint(&o, "lines", 1);
	dect_ctl_add_uint(&o, "hs", n);
	ctl_send(s, &o);
	for (i = 0; i < s->base.n_hs; i++)
		if (ctl_listed(s, i))
			ctl_hs_to(s, s->ctl.cur, i, "registered");
}

static void ctl_reply(struct stubd *s, const char *verb, const char *reason,
		      const char *key, unsigned v)
{
	struct dect_ctl_out o;

	dect_ctl_begin(&o, reason ? "error" : "ok");
	dect_ctl_add(&o, "verb", verb);
	if (key)
		dect_ctl_add_uint(&o, key, v);
	if (reason)
		dect_ctl_add(&o, "reason", reason);
	ctl_send(s, &o);
}

/*
 * The call is over for the bridge. On the main link its ring stops too,
 * whatever state it was in: with ring_ms=0 ring_tick() runs only while
 * ring_now is set, so a ring left in PAGING would hold the main link
 * "busy" for good (code review, 2026-10-29).
 */
static void bc_clear(struct stubd *s, struct dect_base_call *b)
{
	dect_base_call_clear(&s->base, b);
}

/* The handset on link p dialled a number: hand it to the bridge. */
int bridge_call_out(struct stubd *s, int p, uint64_t now)
{
	return dect_base_call_out(&s->base, p, now);
}

/* -------------------------------------------------------------------------
 * Parallel calls on one link, for a Part 3 handset (TS 102 527-3 §7.4.3):
 * call waiting, accepting (holding or releasing the active call) and
 * rejecting it, hold, resume, toggle, releasing one call, and a second
 * call out (1CH 15H + number). The link keeps one CC transaction; the
 * other calls ride it in {CC-INFO} with their own call ids, and the lane
 * carries whichever is in CS call connect. Keypad digits during a call
 * are its DTMF.
 */

/* The call link p's lane carries -- in CS call connect -- or NULL. */
struct dect_base_call *bc_active(struct stubd *s, int p)
{
	return dect_base_call_active(&s->base, p);
}

/* A bridge command about a call. Returns 1 when the verb was one of ours. */
static int bridge_command(struct stubd *s, const struct dect_ctl_msg *m,
			  uint64_t now)
{
	if (!strcmp(m->verb, "bridge")) {
		if (s->ctl.bridge >= 0 && s->ctl.bridge != s->ctl.cur) {
			ctl_reply(s, m->verb, "busy", NULL, 0);
			return 1;
		}
		s->ctl.bridge = s->ctl.cur;
		s->base.bridge = 1;
		printf("bridge: client %d carries external calls\n",
		       s->ctl.cur + 1);
		ctl_reply(s, m->verb, NULL, NULL, 0);
		return 1;
	}
	{
		struct dect_base_reply r;

		/* the call verbs are the library's (dect_base.h, S5a, S5b) */
		if (!dect_base_ctl_call(&s->base, m, s->ctl.cur == s->ctl.bridge,
					now, &r))
			return 0;
		if (!r.sent)
			ctl_reply(s, m->verb, r.err, r.key, r.val);
		return 1;
	}
}

/* The bridged calls' progress, as events (dect_base.h, S3t). */
void bridge_tick(struct stubd *s, uint64_t now)
{
	dect_base_bridge_tick(&s->base, now);
}

static void ctl_command(struct stubd *s, const char *line, size_t len,
			uint64_t now)
{
	struct dect_ctl_msg m;
	struct dect_base_reply r;
	const char *pr;
	unsigned v;

	if (dect_ctl_parse(line, len, &m)) {
		printf("ctl: a malformed line -- ignored\n");
		ctl_reply(s, "?", "malformed", NULL, 0);
		return;
	}
	/* deregister and rename are the library's (dect_base.h, S5c) */
	if (dect_base_ctl_admin(&s->base, &m, now, &r)) {
		ctl_reply(s, m.verb, r.err, r.key, r.val);
		if (r.named >= 0) {
			name_save(s, (unsigned)r.named);
			ctl_hs(s, (unsigned)r.named, "registered");
		}
	} else if (!strcmp(m.verb, "register-window")) {
		v = s->audio.reg_signal_s;
		if (dect_ctl_get(&m, "seconds") &&
		    dect_ctl_get_uint(&m, "seconds", 1, 3600, &v)) {
			ctl_reply(s, m.verb, "bad-seconds", NULL, 0);
			return;
		}
		pr = dect_ctl_get(&m, "profile");
		if (pr && !reg_profile_ok(s, pr)) {
			ctl_reply(s, m.verb, "bad-profile", NULL, 0);
			return;
		}
		/* not before the start-up mails, as reg_window_tick() */
		if (s->sess.state != DECT_SESS_UP || s->tx_slot < 0 ||
		    s->xmail_idx < s->n_xmail) {
			ctl_reply(s, m.verb, "not-ready", NULL, 0);
			return;
		}
		reg_window(s, 1, v, now, "the control socket");
		snprintf(s->reg_profile, sizeof(s->reg_profile), "%s",
			 pr ? pr : "");
		if (pr)
			printf("reg: the next handset gets the profile %s\n", pr);
		ctl_reply(s, m.verb, NULL, "seconds", v);
	} else if (bridge_command(s, &m, now)) {
		/* answered there */
	} else if (!strcmp(m.verb, "reload-profiles")) {
		want_profiles = 1;		/* as SIGHUP */
		ctl_reply(s, m.verb, NULL, NULL, 0);
	} else {
		printf("ctl: unknown verb \"%s\" -- ignored\n", m.verb);
		ctl_reply(s, m.verb, "unknown", NULL, 0);
	}
}

void ctl_open(struct stubd *s)
{
	struct sockaddr_un a;
	mode_t old;
	int fd;

	s->ctl.lfd = -1;
	for (fd = 0; fd < (int)CTL_CLIENTS; fd++)
		s->ctl.cl[fd].fd = -1;
	s->ctl.cur = s->ctl.bridge = -1;
	ab_init(s);		/* ab_open() only after a ctl socket */
	s->ctl.path = s->audio.ctl_path ? s->audio.ctl_path : DECT_CTL_PATH;
	if (s->audio.no_ctl || !s->do_register)
		return;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(s->ctl.path) >= sizeof(a.sun_path)) {
		printf("ctl: %s is too long for a socket path -- no control "
		       "socket\n", s->ctl.path);
		return;
	}
	strcpy(a.sun_path, s->ctl.path);
	/* a live socket belongs to another dect-stubd: leave it alone */
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		close(fd);
		printf("ctl: %s answers -- another dect-stubd holds it, no "
		       "control socket here\n", s->ctl.path);
		return;
	}
	if (fd >= 0)
		close(fd);
	(void)unlink(s->ctl.path);	/* a stale one from a crash */
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		printf("ctl: socket: %s -- no control socket\n",
		       strerror(errno));
		return;
	}
	old = umask(077);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    sock_perms(s, s->ctl.path) || listen(fd, (int)CTL_CLIENTS)) {
		printf("ctl: %s: %s -- no control socket\n", s->ctl.path,
		       strerror(errno));
		umask(old);
		close(fd);
		return;
	}
	umask(old);
	s->ctl.lfd = fd;
	printf("ctl: listening on %s (0600)\n", s->ctl.path);
	ab_open(s);
}

void ctl_close(struct stubd *s)
{
	unsigned c;

	ab_close(s);
	for (c = 0; c < CTL_CLIENTS; c++)
		ctl_drop(s, (int)c, "closed (exit)");
	if (s->ctl.lfd < 0)
		return;
	close(s->ctl.lfd);
	s->ctl.lfd = -1;
	(void)unlink(s->ctl.path);
}

/* One client's input: whole lines become commands, answered to it. */
static void ctl_read(struct stubd *s, int c, uint64_t now)
{
	char *nl;
	ssize_t n;

	while (s->ctl.cl[c].fd >= 0) {
		n = recv(s->ctl.cl[c].fd, s->ctl.cl[c].in + s->ctl.cl[c].in_len,
			 sizeof(s->ctl.cl[c].in) - s->ctl.cl[c].in_len,
			 MSG_DONTWAIT);
		if (n == 0) {
			ctl_drop(s, c, "left");
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR)
				ctl_drop(s, c, "read failed -- closed");
			return;
		}
		s->ctl.cl[c].in_len += (size_t)n;
		while (s->ctl.cl[c].fd >= 0 &&
		       (nl = memchr(s->ctl.cl[c].in, '\n',
				    s->ctl.cl[c].in_len)) != NULL) {
			size_t l = (size_t)(nl - s->ctl.cl[c].in);

			if (!s->ctl.cl[c].discard) {
				s->ctl.cur = c;
				ctl_command(s, s->ctl.cl[c].in, l, now);
				s->ctl.cur = -1;
			}
			if (s->ctl.cl[c].fd < 0)
				return;		/* dropped while answering */
			s->ctl.cl[c].discard = 0;
			memmove(s->ctl.cl[c].in, nl + 1,
				s->ctl.cl[c].in_len - l - 1);
			s->ctl.cl[c].in_len -= l + 1;
		}
		if (s->ctl.cl[c].in_len == sizeof(s->ctl.cl[c].in)) {
			if (!s->ctl.cl[c].discard)
				printf("ctl: a line over %u octets -- dropped\n",
				       DECT_CTL_LINE_MAX);
			s->ctl.cl[c].discard = 1;
			s->ctl.cl[c].in_len = 0;
		}
	}
}

void ctl_tick(struct stubd *s, uint64_t now)
{
	unsigned k;
	int c;

	if (s->ctl.lfd < 0)
		return;
	while ((c = accept(s->ctl.lfd, NULL, NULL)) >= 0) {
		(void)fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		(void)fcntl(c, F_SETFD, FD_CLOEXEC);
		for (k = 0; k < CTL_CLIENTS; k++)
			if (s->ctl.cl[k].fd < 0)
				break;
		if (k == CTL_CLIENTS) {
			static const char busy[] = "error reason=busy\n";

			(void)send(c, busy, sizeof(busy) - 1,
				   MSG_DONTWAIT | MSG_NOSIGNAL);
			close(c);
			printf("ctl: a client turned away -- all %u in use\n",
			       CTL_CLIENTS);
			continue;
		}
		s->ctl.cl[k].fd = c;
		s->ctl.cl[k].in_len = 0;
		s->ctl.cl[k].discard = 0;
		printf("ctl: client %u connected\n", k + 1);
		s->ctl.cur = (int)k;
		ctl_hello(s);
		s->ctl.cur = -1;
	}
	for (k = 0; k < CTL_CLIENTS; k++)
		ctl_read(s, (int)k, now);
}

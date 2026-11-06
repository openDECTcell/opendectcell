// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, host of the base stack: the callbacks struct dect_base asks its host for
 * (store, send, random, clock, events), the subscription store and the handset keys written into
 * the chip, roaming fetch, stored capabilities, the park and its read-back. Moved out of
 * dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/* ---- GAP registration: drive the host C-plane stack over the STUB ---- */

/*
 * Fill buf with n random bytes from the kernel. RAND_F, RS and the base's challenges come from here: if the
 * kernel cannot give random numbers the daemon stops rather than hand out guessable ones (security review
 * 2026-11-02, L-1: it used to fall back to a clock-seeded generator without a word).
 */
static void gen_rand(struct stubd *s, uint8_t *buf, size_t n)
{
	size_t got = 0;

	/* the file the daemon opened (/dev/urandom; the tests' fixed bytes) first, then the kernel's call */
	if (s->rand_fd >= 0 && read(s->rand_fd, buf, n) == (ssize_t)n)
		return;
	while (got < n) {
		ssize_t r = getrandom(buf + got, n - got, 0);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			fprintf(stderr, "dect-stubd: no random numbers (%s); stopping\n", strerror(errno));
			_exit(70);
		}
		got += (size_t)r;
	}
}

/*
 * The chip subscription slot this run is working with.
 *
 * Resolution order: -a subslot=N, else the slot the store was saved with,
 * else DECT_MM_REG_SLOT.
 *
 * This must NOT go through dect_mm_sub_slot(). MM only learns the slot in
 * reg_start(), which runs on connection-up, while send_subscription() runs
 * from the poll loop as soon as the session is up -- before any handset has
 * connected. Reading it from MM therefore returned the default 1 for every
 * handset, so all three UAKs were written to chip slot 1, overwriting each
 * other, while the log cheerfully printed the stored slot from the record.
 * Measured 2026-10-24: "restored subscription for slot 2" immediately
 * followed by "cleared chip subscription slots other than 1".
 */
uint8_t stubd_sub_slot(const struct stubd *s)
{
	return (uint8_t)dect_base_slot(&s->base, s->base.cur);
}

/* Handset i became current: its file, and its warnings not given yet. */
static void hs_took(struct stubd *s, unsigned i)
{
	s->sub_path = s->hs[i].path;
}

/* Handset i's record as the working copy, at start (dect_base_load()). */
void hs_load(struct stubd *s, unsigned i)
{
	dect_base_load(&s->base, i);
	hs_took(s, i);
}

/* The library made another handset current: its file, and say so. */
static void hs_switched(struct stubd *s, const char *why)
{
	hs_took(s, s->base.cur);
	printf("sub: now serving %s (slot %u)%s\n", s->sub_path,
	       stubd_sub_slot(s), why);
}

/* dect_base's switched(): the library made another handset current. */
static void base_switched(void *ctx, const char *why)
{
	hs_switched(ctx, why);
}

static void sub_save(struct stubd *s)
{
	struct sub_record r;
	int fd;

	if (!s->sub_path || !s->base.rec.uak_valid)
		return;
	memset(&r, 0, sizeof(r));
	r.magic = SUB_MAGIC;
	r.slot = stubd_sub_slot(s);
	memcpy(r.uak, s->base.rec.uak, DECT_MM_UAK_LEN);
	r.magic = SUB_MAGIC2;
	/* the current handset's identity; the link MM may serve another */
	if (s->base.rec.port_id_len) {
		r.port_id_len = (uint8_t)s->base.rec.port_id_len;
		memcpy(r.port_id, s->base.rec.port_id, sizeof(r.port_id));
	} else {
		r.port_id_len = (uint8_t)s->base.main_link.link.mm.port_id_len;
		memcpy(r.port_id, s->base.main_link.link.mm.port_id, sizeof(r.port_id));
	}
	r.cc_port_id_len = (uint8_t)s->base.rec.cc_port_id_len;
	memcpy(r.cc_port_id, s->base.rec.cc_port_id, sizeof(r.cc_port_id));
	r.cc_fixed_id_len = (uint8_t)s->base.rec.cc_fixed_id_len;
	memcpy(r.cc_fixed_id, s->base.rec.cc_fixed_id, sizeof(r.cc_fixed_id));

	/*
	 * Replace the store atomically: write a temporary file beside it,
	 * fsync it, rename it over the store, then fsync the directory. The
	 * record holds the UAK, and losing it means re-registering the handset
	 * by hand. It used to be truncated and rewritten in place, so a crash,
	 * a full overlay or a power cut mid-write destroyed the old record
	 * before the new one existed. fchmod() because O_CREAT's mode does not
	 * touch a temporary file left over from an earlier attempt. Found by
	 * the second external review, the clean-room record/an external review-1-24102026.md,
	 * finding 6.
	 */
	{
		char tmp[512], dir[512];
		char *slash;
		int ok;

		if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", s->sub_path) >=
		    sizeof(tmp)) {
			printf("sub: path too long: %s\n", s->sub_path);
			return;
		}
		fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) {
			printf("sub: cannot write %s: %s\n", tmp, strerror(errno));
			return;
		}
		ok = fchmod(fd, 0600) == 0 &&
		     write(fd, &r, sizeof(r)) == (ssize_t)sizeof(r) &&
		     fsync(fd) == 0;
		if (close(fd) != 0)
			ok = 0;
		if (!ok || rename(tmp, s->sub_path) != 0) {
			printf("sub: could not save %s (%s); the old record "
			       "is untouched\n", s->sub_path, strerror(errno));
			unlink(tmp);
			return;
		}
		snprintf(dir, sizeof(dir), "%s", s->sub_path);
		slash = strrchr(dir, '/');
		if (slash) {
			*(slash == dir ? slash + 1 : slash) = '\0';
			fd = open(dir, O_RDONLY);
			if (fd >= 0) {
				(void)fsync(fd);
				close(fd);
			}
		}
		printf("sub: subscription saved to %s\n", s->sub_path);
		if (s->base.cur < s->base.n_hs) {
			s->base.loaded[s->base.cur] = 1;	/* it exists now */
			s->base.absent[s->base.cur] = 0;	/* and is no longer free */
		}
	}
}

/* dect_base's save(): the current handset's file (dect_base.h). */
static void base_save(void *ctx, unsigned i, const struct dect_hs_rec *r)
{
	(void)i;
	(void)r;		/* the working copy, which sub_save() writes */
	sub_save(ctx);
}

/* dect_base's send(): the control-slot queue (send_mail()). */
static int base_send(void *ctx, const struct dect_mail *m)
{
	struct stubd *s = ctx;
	int r = send_mail(s, (unsigned int)s->tx_slot, m);

	if (s->verbose)
		log_mail(m, "tx M", s->show_params);
	return r;
}

/* dect_base's random(): /dev/urandom (gen_rand()). */
static void base_random(void *ctx, uint8_t *out, size_t n)
{
	gen_rand(ctx, out, n);
}

/* dect_base's log(): standard output, which procd puts in the syslog. */
static void base_log(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);
}

/* dect_base's install(): every handset this box serves (send_subscription()). */
static void base_install(void *ctx)
{
	send_subscription(ctx);
}

/* dect_base's event(): to the bridge's client. */
static void base_event(void *ctx, struct dect_ctl_out *o)
{
	ctl_send_bridge(ctx, o);
}

/* dect_base's pump(): the link's queued frames to the control slot. */
static void base_pump(void *ctx, struct dect_base_link *bl)
{
	struct stubd *s = ctx;
	unsigned k;

	if (bl == &s->base.main_link) {
		pump_lapc(s);
		return;
	}
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl == bl)
			side_pump(s, &s->side[k]);
}

/* dect_base's lanes(): the audio channels a new call can count on. */
static int base_lanes(void *ctx, int p)
{
	struct stubd *s = ctx;

	return lanes_available(s, p >= 0 ? &s->side[p] : NULL);
}

static void base_switched(void *ctx, const char *why);

static void base_cap_learned(void *ctx, unsigned i);

const struct dect_base_ops base_ops = {
	.save = base_save, .send = base_send, .random = base_random,
	.log = base_log, .install = base_install, .event = base_event,
	.pump = base_pump, .lanes = base_lanes, .forget = base_forget,
	.mask = base_mask, .clock = base_clock, .named = base_named,
	.wall = base_wall, .switched = base_switched, .link_up = base_link_up,
	.side_down = base_side_down, .registered = base_registered,
	.call_up = base_call_up, .side_call = base_side_call,
	.swapped = base_swapped, .cap_learned = base_cap_learned,
	.call_down = base_call_down,
};

/* Load the stored subscription, if any. Returns 1 when a record was adopted. */
int sub_load(struct stubd *s)
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
	/*
	 * Two sizes are valid: the original record, and the current one that
	 * adds the call identities. Checking the size against the matching
	 * magic keeps an old file loading instead of being discarded, which
	 * would silently lose the subscription.
	 */
	if (!((n == (ssize_t)sizeof(struct sub_record_v1) &&
	       r.magic == SUB_MAGIC) ||
	      (n == (ssize_t)sizeof(r) && r.magic == SUB_MAGIC2)) ||
	    r.port_id_len > DECT_MM_PORTID_MAX)
		return 0;
	memcpy(s->base.rec.uak, r.uak, DECT_MM_UAK_LEN);
	s->base.rec.uak_valid = 1;
	/*
	 * The record has always carried the MM portable identity and we have
	 * never restored it, so the identity gate in on_locate_request() could
	 * not fire: measured 2026-10-23, a handset we hold no record for had
	 * its location registration accepted anyway.
	 */
	if (r.port_id_len && r.port_id_len <= DECT_MM_PORTID_MAX) {
		memcpy(s->base.rec.port_id, r.port_id, r.port_id_len);
		s->base.rec.port_id_len = r.port_id_len;
	}
	if (r.magic == SUB_MAGIC2 && r.cc_port_id_len <= DECT_CC_ID_MAX &&
	    r.cc_fixed_id_len <= DECT_CC_ID_MAX) {
		memcpy(s->base.rec.cc_port_id, r.cc_port_id, sizeof(s->base.rec.cc_port_id));
		s->base.rec.cc_port_id_len = r.cc_port_id_len;
		memcpy(s->base.rec.cc_fixed_id, r.cc_fixed_id, sizeof(s->base.rec.cc_fixed_id));
		s->base.rec.cc_fixed_id_len = r.cc_fixed_id_len;
		/*
		 * Older records carry no MM identity -- sub_save() stores
		 * reg.mm.port_id, which is empty unless the handset registered
		 * in this session, so every stored file has port_id_len = 0.
		 * The CC portable identity is the same handset and the same
		 * bytes: the A540's record holds 80 a8 02 de ad 00 01 and that
		 * is exactly what its {LOCATE-REQUEST} puts in
		 * <<PORTABLE-IDENTITY>>. Use it so the gate has something to
		 * compare against.
		 */
		if (!s->base.rec.port_id_len && r.cc_port_id_len &&
		    r.cc_port_id_len <= DECT_MM_PORTID_MAX) {
			memcpy(s->base.rec.port_id, r.cc_port_id, r.cc_port_id_len);
			s->base.rec.port_id_len = r.cc_port_id_len;
		}
		/*
		 * Call identities that are not the subscribed handset's were
		 * learned from another handset's call (see the learning gate
		 * where they are kept) and would address every incoming call
		 * to it. GAP table 10 makes a PT's {CC-SETUP} carry its IPUI,
		 * the identity it registered with, so the two must agree.
		 * Drop them; the handset's next call teaches the right pair.
		 */
		if (s->base.rec.cc_port_id_len &&
		    (s->base.rec.cc_port_id_len != s->base.rec.port_id_len ||
		     memcmp(s->base.rec.cc_port_id, s->base.rec.port_id,
			    s->base.rec.port_id_len))) {
			memset(s->base.rec.cc_port_id, 0, sizeof(s->base.rec.cc_port_id));
			memset(s->base.rec.cc_fixed_id, 0, sizeof(s->base.rec.cc_fixed_id));
			s->base.rec.cc_port_id_len = 0;
			s->base.rec.cc_fixed_id_len = 0;
			printf("sub: *** stored call identities belong to "
			       "ANOTHER handset *** -- dropped; they are "
			       "relearned from this handset's next call\n");
		}
	}
	printf("sub: identity gate %s (%zu bytes)\n",
	       s->base.rec.port_id_len ? "armed" : "DISABLED -- no stored identity",
	       s->base.rec.port_id_len);
	/*
	 * Honour the slot the record was saved with, unless -a subslot=N said
	 * otherwise. Without this a handset enrolled in slot 3 came back as
	 * slot 1 on the next run, taking another handset's UAK slot and TPUI
	 * with it.
	 */
	if ((!s->audio.sub_slot || s->base.cur != 0) &&
	    r.slot >= 1u && r.slot <= DECT_MM_MAX_SLOT)
		s->base.rec.slot = r.slot;	/* subslot= is the ring target's only */
	printf("sub: restored subscription for slot %u from %s%s\n",
	       r.slot, s->sub_path,
	       s->base.rec.cc_fixed_id_len ? " (with call identities)" : "");
	return 1;
}

/*
 * holdback=MS (docs/journals/multicell-experiments.md, test M2): every store but the
 * first stays out of the chip until MS after start, as if a controller
 * pushed it then. The host still holds them; only the chip lacks the key.
 */
static int held_back(const struct stubd *s, unsigned i)
{
	if (s->audio.no_chip_keys)	/* every slot cleared, no key at all */
		return 1;
	if (i < s->base.n_hs && s->base.remote[i] && !s->base.fetched[i])
		return 1;		/* a roamer: not in this cell's chip */
	return s->audio.holdback_ms && !s->holdback_done && i != 0;
}

static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN]);

/* The controller's answer: hold the fetched record on the main link. */
static void fetch_keep(struct stubd *s)
{
	if (s->fetch_idx < 0)
		return;
	s->base.fetched[s->fetch_idx] = 1;	/* the next link holds it from hs[] */
	if (s->base.main_link.active)
		(void)dect_mm_hold(&s->base.main_link.link.mm,
				   s->base.hs[s->fetch_idx].port_id,
				   s->base.hs[s->fetch_idx].port_id_len,
				   (uint8_t)s->base.hs[s->fetch_idx].slot, NULL,
				   s->fetch_idx);
}

/*
 * Test M4 (docs/journals/multicell-experiments.md): play the controller for a
 * roamer. MM holds a {LOCATE-REQUEST} from an identity this link does not
 * know; if it is the remote= store's, its record "arrives" fetchdelay ms
 * after the request, and the request is answered. Times are from the
 * request, so the handset's patience is measured, not ours.
 */
void fetch_tick(struct stubd *s, uint64_t now)
{
	const uint8_t *id;
	size_t len;
	unsigned i, delay;
	int r, setup;

	if (!s->audio.remote_path)
		return;
	/* a held LOCATE-REQUEST, or a held CC-SETUP from a roamer that did
	 * not locate (same location area) */
	if (s->base.main_link.active &&
	    dect_mm_locate_pending(&s->base.main_link.link.mm, &id, &len))
		setup = 0;
	else if (s->base.main_link.active &&
		 dect_link_setup_pending(&s->base.main_link.link, &id, &len))
		setup = 1;
	else {
		/*
		 * The handset stopped waiting (a list access it closed after
		 * 1.7 s, M4-15000-063617), or its link went down. The
		 * controller's answer arrives anyway and is kept: dropping it
		 * left the roamer unknown, and every later attempt was turned
		 * away as a stranger. The fetch belongs to the handset, not
		 * the link: a new link does not cancel it (M4-15000-063907).
		 */
		if (!s->fetch_req_ms || now < s->fetch_at)
			return;
		fetch_keep(s);
		printf("fetch: record arrived at +%llu ms after the request; "
		       "the handset had stopped waiting -- kept for its next "
		       "attempt\n",
		       (unsigned long long)(now - s->fetch_req_ms));
		fflush(stdout);
		s->fetch_req_ms = 0;
		s->fetch_at = 0;
		return;
	}
	if (!s->fetch_req_ms) {
		s->fetch_req_ms = now;
		s->fetch_idx = -1;
		for (i = 0; i < s->base.n_hs; i++)
			if (s->base.remote[i] && s->base.hs[i].port_id_len == len &&
			    !memcmp(s->base.hs[i].port_id, id, len))
				s->fetch_idx = (int)i;
		/*
		 * A record fetched earlier this run is here already: a
		 * {CC-SETUP} held behind the {LOCATE-REQUEST} that fetched
		 * it is served at once. Waiting again let the A540's
		 * <CC.03> (20 s) run out (M4-30000-x-054928).
		 */
		delay = s->fetch_idx < 0 ? 0u :
			s->base.fetched[s->fetch_idx] ? 0u :
			s->audio.fetch_delay_ms;
		s->fetch_at = now + delay;
		printf("fetch: %s from a handset this cell does not hold -- %s "
		       "(simulated, %u ms)\n",
		       setup ? "CC-SETUP" : "LOCATE-REQUEST",
		       s->fetch_idx < 0 ? "unknown to the controller too" :
		       s->base.fetched[s->fetch_idx] ? "record already fetched" :
						     "asking the controller",
		       delay);
	}
	if (s->base.main_link.link.mm.stats.locate_retrans != s->fetch_retrans_seen) {
		s->fetch_retrans_seen = s->base.main_link.link.mm.stats.locate_retrans;
		printf("fetch: the handset repeated its LOCATE-REQUEST at +%llu "
		       "ms\n", (unsigned long long)(now - s->fetch_req_ms));
	}
	if (now < s->fetch_at)
		return;
	fetch_keep(s);
	r = setup ? dect_link_setup_resume(&s->base.main_link.link, now)
		  : dect_mm_locate_resume(&s->base.main_link.link.mm);
	printf("fetch: %s at +%llu ms after the request\n",
	       setup ? (r > 0 ? "CC-SETUP served" : "CC-SETUP turned away")
		     : (r > 0 ? "LOCATE-ACCEPT sent" : "LOCATE-REJECT sent"),
	       (unsigned long long)(now - s->fetch_req_ms));
	fflush(stdout);
	s->fetch_req_ms = 0;
	s->fetch_at = 0;
	dect_link_drain(&s->base.main_link.link, now);
	pump_lapc(s);
}

/*
 * A handset's capability as it last reported it, kept beside its store as
 * <store>.cap ("part3 0|1" and "ng 0|1"): a handset reports it only with a
 * location registration, which it may not repeat for hours after the base
 * restarts, and without it an A540 rings with no number and no line.
 */
static void cap_save(const struct stubd *s, unsigned i)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s.cap", s->hs[i].path);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "part3 %d\nng %d\n", s->base.cap[i].part3, s->base.cap[i].ng);
	fclose(f);
}

void cap_load(struct stubd *s, unsigned i)
{
	char path[256], key[8];
	int v, got = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s.cap", s->hs[i].path);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fscanf(f, "%7s %d", key, &v) == 2) {
		if (!strcmp(key, "part3"))
			s->base.cap[i].part3 = v != 0, got |= 1;
		else if (!strcmp(key, "ng"))
			s->base.cap[i].ng = v != 0, got |= 2;
	}
	fclose(f);
	if (got != 3)
		return;
	s->base.cap[i].known = 1;
	printf("cap: %s reported %s (remembered)\n", s->hs[i].path,
	       s->base.cap[i].part3 ? "NG-DECT Part 3" :
	       s->base.cap[i].ng ? "NG-DECT Part 1" : "GAP only");
}

/*
 * Learn each handset's <<TERMINAL-CAPABILITY>> from whichever link carried
 * its {LOCATE-REQUEST}, matched by the IPUI that link adopted, log it once
 * and keep it beside its store (cap_save()).
 */
void cap_tick(struct stubd *s, struct dect_link *l)
{
	dect_base_cap_tick(&s->base, l);
	fflush(stdout);
}

/* dect_base's cap_learned(): beside the store, for the next start. */
static void base_cap_learned(void *ctx, unsigned i)
{
	cap_save(ctx, i);
}

/*
 * Say so, once, when LAPC gave up after N250 retransmissions and released
 * the link (EN 300 175-4 §9.2.3.6; new 2026-10-28). Loud on purpose: the
 * corpus does not count retransmissions, so whether a working link ever
 * needs more than three is unmeasured, and a false release must show.
 */
void lapc_failed_note(struct stubd *s, struct dect_link *l,
			     const char *which)
{
	dect_base_lapc_note(&s->base, l, which);
	fflush(stdout);
}

/* -a nlr: the release after a call is the library's (dect_base.h, S3u). */
void nlr_tick(struct stubd *s, uint64_t now)
{
	dect_base_registration_tick(&s->base, now);
	dect_base_nlr_tick(&s->base, now);
	fflush(stdout);
}

/* The main link's call rings: the name, or infoie= (dect_base.h, S3r). */
void info_tick(struct stubd *s, uint64_t now)
{
	dect_base_info_tick(&s->base, now);
}

/* Authenticate first (dect_base_call_auth_ok(), S3p). */
int call_auth_ok(const struct stubd *s, const struct dect_link *l)
{
	return dect_base_call_auth_ok(&s->base, l);
}

/*
 * Overlap receiving: accept a handset-placed call once its digits have
 * been quiet for dialwait ms. Stands in for a dial plan or the
 * controller, which would decide on the number instead.
 */
void dial_tick(struct stubd *s, uint64_t now)
{
	dect_base_main_dial_tick(&s->base, now);
	fflush(stdout);
}

/* alertms / bindfirst: send the CONNECT once its conditions hold. */
void connect_tick(struct stubd *s, uint64_t now)
{
	dect_base_connect_tick(&s->base, now);
}

void holdback_tick(struct stubd *s, uint64_t now)
{
	unsigned i;

	if (!s->audio.holdback_ms || s->holdback_done || !s->sub_restored ||
	    now - g_start < s->audio.holdback_ms)
		return;
	s->holdback_done = 1;
	for (i = 1; i < s->base.n_hs; i++) {
		const uint8_t *uak = i == s->base.cur ? s->base.rec.uak : s->base.hs[i].uak;
		int valid = i == s->base.cur ? s->base.rec.uak_valid : s->base.hs[i].uak_valid;

		if (!s->base.loaded[i] || !valid)
			continue;
		install_subscription(s, (uint8_t)hs_slot(s, i), uak);
		printf("sub: holdback over: %s installed in chip slot %u at "
		       "t=%llu ms\n", s->hs[i].path, hs_slot(s, i),
		       (unsigned long long)(now - g_start));
	}
	send_subscription_tail(s);	/* the mask now includes them */
	fflush(stdout);
}

/* Does any handset we hold have a key to put in the chip? */
int any_uak(const struct stubd *s)
{
	unsigned i;

	if (s->base.rec.uak_valid)
		return 1;
	for (i = 0; i < s->base.n_hs; i++)
		if (i != s->base.cur && s->base.loaded[i] && s->base.hs[i].uak_valid)
			return 1;
	return 0;
}

/*
 * Clear every subscription slot we do not own.
 *
 * The chip keeps a subscription table of its own and it SURVIVES our daemon:
 * we only ever write one slot, so a handset enrolled in an earlier session
 * stays reachable in another slot for ever. Measured 2026-10-23:
 *
 *   vF426  slot 1 = handset A, slots 2..6 untouched -> 51 connections, every
 *          one of them from handset B, which our store did not contain
 *   vF427  slot 1 = handset A, slots 2..6 cleared   -> 0 connections, no
 *          C-plane traffic at all
 *
 * So the base was accepting a handset it had no record of, which is wrong
 * under GAP -- an FP serves subscribed handsets -- and it made every identity
 * question this week ambiguous, because the store said one thing and the air
 * another. [CR A-0065] decodes 0x0247 as {slot, op (0 = clear, 1 = set),
 * flag}, which is what makes the fix a three-byte mail per slot.
 */
static uint8_t live_slot_mask(const struct stubd *s);

static void clear_foreign_slots(struct stubd *s, uint8_t keep,
				unsigned keep_mask)
{
	dect_base_slot_clear(&s->base, keep_mask);
	/*
	 * ...and now check it actually happened. The clear is 0x0247, which
	 * has no reply and no error path, so until today "cleared" meant
	 * "asked". vF426 is what asking without checking looks like.
	 */
	dect_base_slot_audit(&s->base, keep, live_slot_mask(s), mono_ms());
}

/*
 * Every handset we hold goes into its chip slot, the current one last so its
 * read-back is the one outstanding; only slots no store uses are cleared.
 * With one store loaded this is what it always was.
 */
void send_subscription(struct stubd *s)
{
	uint8_t slot = stubd_sub_slot(s);
	unsigned keep = s->base.rec.uak_valid && !held_back(s, s->base.cur) ? 1u << slot : 0u;
	unsigned i;

	for (i = 0; i < s->base.n_hs; i++) {
		unsigned hsl = hs_slot(s, i);

		if (i != s->base.cur && s->base.loaded[i] && s->base.hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && !held_back(s, i))
			keep |= 1u << hsl;
	}
	clear_foreign_slots(s, slot, keep);
	for (i = 0; i < s->base.n_hs; i++) {
		unsigned hsl = hs_slot(s, i);

		if (i != s->base.cur && s->base.loaded[i] && s->base.hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && hsl != slot &&
		    !held_back(s, i)) {
			install_subscription(s, (uint8_t)hsl, s->base.hs[i].uak);
			printf("sub: %s installed in chip slot %u\n",
			       s->hs[i].path, hsl);
		}
	}
	if (s->base.rec.uak_valid && !held_back(s, s->base.cur)) {
		install_subscription(s, slot, s->base.rec.uak);
		/* [CR A-0085] the only check */
		dect_base_uak_verify(&s->base, slot, mono_ms());
	}
	if (s->audio.install_dummy) {	/* A-0134 bench test: the install mask without a key */
		char spec[40];

		snprintf(spec, sizeof(spec), "0x22:0x02:0x0247:%02x0100",
			 s->audio.install_dummy);
		if (!send_mail_spec(s, spec))
			printf("sub: slot %u installed with 0x0247 and NO key "
			       "(installdummy)\n", s->audio.install_dummy);
	}
	send_subscription_tail(s);
}

/*
 * Put a handset into its chip slot so it holds standby and is reachable:
 * 0x0247 {slot, op 1, flag} then 0x020A {slot, UAK[16], config}, to STUB
 * task 0x02, in that order [CR A-0026]. send_subscription_tail() follows
 * with 0x017C.
 */
static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN])
{
	(void)dect_base_slot_install(&s->base, slot, uak);	/* the log masks the UAK */
}

/*
 * The slots holding a live 0x020A record, as 0x017c wants them
 * [CR A-0129]: the same set send_subscription() installs.
 */
static uint8_t live_slot_mask(const struct stubd *s)
{
	unsigned i, mask = 0;

	for (i = 0; i < s->base.n_hs; i++) {
		unsigned sl = hs_slot(s, i);
		int valid = i == s->base.cur ? s->base.rec.uak_valid
					: s->base.loaded[i] && s->base.hs[i].uak_valid;

		if (valid && sl >= 1u && sl <= DECT_MM_MAX_SLOT &&
		    !held_back(s, i))
			mask |= 1u << (sl - 1u);
	}
	return (uint8_t)mask;
}

void send_subscription_tail(struct stubd *s)
{
	uint8_t mask = s->audio.slot_mask_fixed ? s->audio.slot_mask
						: live_slot_mask(s);

	/*
	 * 0x017C {00, mask}: the handsets locked in (see audio_cfg.slot_mask,
	 * [CR A-0129]). Stock sends it right after the 0x020A records and
	 * after every deregistration.
	 */
	dect_base_slot_mask(&s->base, mask);
	if (s->audio.slot_mask_fixed && !s->audio.no_slot_cfg)
		printf("sub: the mask is pinned by slotmask=\n");
}

void park_record(const char *how)
{
	FILE *f = fopen_private(PARK_RECORD);

	if (!f || fprintf(f, "%s\n", how) < 0 || fclose(f))
		fprintf(stderr, "reg: %s not written\n", PARK_RECORD);
}

static void park_read(struct stubd *s, uint32_t addr)
{
	chip_mem_read(s, addr, 1);
}

/* After the park mails are out: read the park back, where we know how. */
int park_check_start(struct stubd *s, uint64_t now)
{
	if (strcmp(s->stub_name, DECT_STUB_0688) || s->tx_slot < 0)
		return 0;
	s->park_radio = s->park_timer = -1;
	s->park_check_at = now;
	park_read(s, PARK_0688_RADIO);
	park_read(s, PARK_0688_TIMER);
	return 1;
}

static void park_check_reply(struct stubd *s, uint32_t addr, uint8_t v)
{
	if (addr == PARK_0688_RADIO)
		s->park_radio = v;
	else if (addr == PARK_0688_TIMER)
		s->park_timer = v;
}

/*
 * Both answers in, or given up: say what the chip holds. Returns 1 then.
 * A missing answer is asked for once more: one service stop of six on
 * 2026-10-29 got none within 1 s, for no reason we could find.
 */
int park_check_done(struct stubd *s, uint64_t now)
{
	if ((s->park_radio < 0 || s->park_timer < 0) &&
	    now - s->park_check_at < PARK_CHECK_MS)
		return 0;
	if ((s->park_radio < 0 || s->park_timer < 0) &&
	    !s->park_check_retried) {
		s->park_check_retried = 1;
		s->park_check_at = now;
		printf("reg: park read-back: no answer within %u ms, asking "
		       "again\n", PARK_CHECK_MS);
		if (s->park_radio < 0)
			park_read(s, PARK_0688_RADIO);
		if (s->park_timer < 0)
			park_read(s, PARK_0688_TIMER);
		return 0;
	}
	if (s->park_radio == 0 && s->park_timer == 0) {
		printf("reg: park verified on the chip: radio switch "
		       "0x%04X = 00, beacon timer cancelled (0x%04X = 00)\n",
		       PARK_0688_RADIO, PARK_0688_TIMER);
		park_record("verified");
	} else if (s->park_radio < 0 || s->park_timer < 0)
		printf("reg: PARK NOT VERIFIED -- no read-back from the chip "
		       "within %u ms\n", PARK_CHECK_MS);
	else
		printf("reg: PARK NOT VERIFIED -- radio switch 0x%04X = %02x, "
		       "beacon timer 0x%04X = %02x (both should be 00)\n",
		       PARK_0688_RADIO, (unsigned)s->park_radio,
		       PARK_0688_TIMER, (unsigned)s->park_timer);
	return 1;
}

/*
 * -a memdump: print the bytes of a reply whose range lies outside the
 * subscription tables (key table and identity block, 0x9518..0x95E3; the
 * masks after them are readable), so a probe of chip RAM can be read without -p. -p also shows box identity
 * data in other mails; this line shows only what the probe asked for.
 */
static void mem_probe_log(struct stubd *s, const struct dect_mail *m,
			  uint32_t addr)
{
	uint32_t n = m->param_len - 7u, i;

	if (!s->audio.n_memdump || !n)
		return;
	if (addr < 0x95E4u && addr + n > SUB_UAK_BASE)
		return;
	printf("[%5llu] mem: probe 0x%05x len %u:",
	       (unsigned long long)(mono_ms() - g_start), addr, n);
	for (i = 0; i < n && 7u + i < sizeof(m->param); i++)
		printf(" %02x", m->param[7u + i]);
	printf("\n");
}

/*
 * A 0xFFF1 answer from the chip: our own park's read-back, then the
 * library's subscription read-back and slot audit (S2.2b).
 */
void mem_reply(struct stubd *s, const struct dect_mail *m)
{
	uint32_t addr;

	if (m->param_len < 7u || m->param[0] != 0x08u)
		return;
	addr = (uint32_t)m->param[1] | ((uint32_t)m->param[2] << 8)
	     | ((uint32_t)m->param[3] << 16) | ((uint32_t)m->param[4] << 24);
	if (s->park_check_at && m->param_len > 7u)
		park_check_reply(s, addr, m->param[7]);
	mem_probe_log(s, m, addr);
	dect_base_mem_reply(&s->base, m);
	fflush(stdout);
}

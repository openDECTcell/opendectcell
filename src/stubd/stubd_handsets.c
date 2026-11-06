// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, handset data and state: the LED, state.json, the registration window, names,
 * lines, contact and call lists, handset profiles. Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

void led_write(struct stubd *s, int on)
{
	if (s->led_fd < 0 || s->led_state == on)
		return;
	if (pwrite(s->led_fd, on ? "1" : "0", 1, 0) == 1) {
		s->led_state = on;
		s->led_changes++;
	}
}

void led_open(struct stubd *s)
{
	const char *path = s->audio.led_path ? s->audio.led_path
					     : LED_DEFAULT_PATH;

	s->led_fd = -1;
	s->led_state = -1;
	if (s->audio.no_led)
		return;
	s->led_fd = open(path, O_WRONLY | O_CLOEXEC);
	if (s->led_fd < 0 && s->audio.led_path)
		printf("led: cannot open %s: %s\n", path, strerror(errno));
}

void led_tick(struct stubd *s, uint64_t now, int radio_on)
{
	if (s->led_fd < 0 || now < s->led_next)
		return;
	s->led_next = now + LED_TICK_MS;
	if (!radio_on) {
		led_write(s, 0);
		return;
	}
	/* a cell has no main link of its own: a handset's link is a handle */
	if (s->led_traffic != s->led_traffic_seen || s->base.main_link.bound ||
	    (s->cell_mode && s->cell.handles)) {
		s->led_traffic_seen = s->led_traffic;
		s->led_blink_ticks++;
		led_write(s, s->led_state != 1);	/* toggle */
	} else {
		led_write(s, 1);
	}
}

/* A JSON string: quotes, backslashes and control characters escaped. */
void json_str(FILE *f, const char *v)
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

static void state_link(struct stubd *s, FILE *f, const struct dect_link *l,
		       const struct dect_diag_status *st, uint64_t frames,
		       int *first)
{
	struct dect_diag_link d;
	char js[768];

	memset(&d, 0, sizeof(d));
	if (st)
		d.radio = *st;
	d.call_frames = frames;
	dect_diag_from_link(l, &d);
	if (d.handset_tag < 0 && l == &s->base.main_link.link)
		d.handset_tag = (int)s->base.cur;	/* the current store */
	if (dect_diag_json(&d, s->show_params, js, sizeof(js))) {
		fprintf(f, "%s%s", *first ? "" : ",", js);
		*first = 0;
	}
}

void state_write(struct stubd *s, uint64_t now, int radio_on,
			int force)
{
	const char *path = s->audio.state_path ? s->audio.state_path
					       : STATE_DEFAULT_PATH;
	char tmp[512];
	FILE *f;
	unsigned i;
	int first;

	if (s->audio.no_state || !s->do_register ||
	    (!force && now < s->state_next))
		return;
	s->state_next = now + STATE_EVERY_MS;
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
		return;
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "{\"uptime_ms\":%llu,\"radio\":\"%s\",\"stub\":",
		(unsigned long long)(now - g_start),
		radio_on ? "up" : "parked");
	json_str(f, s->stub_build == STUB_UNKNOWN ? "unknown" : s->stub_name);
	fprintf(f, ",\"registration\":\"%s\"",
		radio_on && s->base.reg_open ? "open" : "closed");
	if (radio_on && s->base.reg_open && s->base.reg_open_until > now)
		fprintf(f, ",\"registration_left_s\":%llu",
			(unsigned long long)((s->base.reg_open_until - now + 999) /
					     1000));
	/* what the base is set to, for the Monitor's base section */
	fprintf(f, ",\"api\":%u,\"cipher\":%s,\"locauth\":%s,"
		"\"wideband\":%s,\"twocalls\":%s,\"line\":",
		OPENDECTCELL_API_VERSION, s->no_cipher ? "false" : "true",
		s->base.cfg.locauth ? "true" : "false",
		s->base.cfg.narrowband ? "false" : "true",
		s->base.cfg.two_calls ? "true" : "false");
	json_str(f, s->audio.line_name ? s->audio.line_name : "Line 1");
	fprintf(f, ",\"handsets\":[");
	first = 1;
	for (i = 0; i < s->base.n_hs; i++) {
		unsigned k, linked = 0;
		uint64_t seen = s->base.last_seen_ms[i];

		/* the handsets the base holds: not a store just deregistered */
		if ((i != s->base.cur && !s->base.loaded[i]) || s->base.absent[i])
			continue;
		/*
		 * parked: no handset has a link, whatever was up at exit.
		 * adopted -1 means the link serves the current store itself
		 * (a call we placed to it): that handset is linked too, or the
		 * Monitor shows it "idle" mid-call (bench test 12a).
		 */
		if (radio_on && s->base.main_link.active &&
		    (s->base.main_link.link.mm.adopted == (int)i ||
		     (s->base.main_link.link.mm.adopted < 0 && i == s->base.cur &&
		      s->base.main_link.link.mm.port_id_len)))
			linked = 1;
		for (k = 0; radio_on && k < SIDE_MAX; k++)
			if (s->side[k].bl->active && s->side[k].bl->adopted == (int)i)
				linked = 1;
		{
			char nm[DECT_IWU_NAME_MAX];

			hs_name(s, i, nm, sizeof(nm));
			fprintf(f, "%s{\"slot\":%u,\"name\":",
				first ? "" : ",", hs_slot(s, i));
			json_str(f, nm);
			fprintf(f, ",\"store\":");
		}
		json_str(f, s->hs[i].path);
		{
			char ipei[DECT_IPEI_TEXT_LEN];

			if (!dect_ipei_text(s->base.hs[i].port_id, s->base.hs[i].port_id_len,
					    ipei, sizeof(ipei))) {
				fprintf(f, ",\"ipei\":");
				json_str(f, ipei);
			}
		}
		fprintf(f, ",\"link\":%s", linked ? "true" : "false");
		if (s->hs[i].prof_id[0]) {
			fprintf(f, ",\"profile\":");
			json_str(f, s->hs[i].prof_id);
		}
		if (s->base.cap[i].known)
			fprintf(f, ",\"part3\":%s",
				s->base.cap[i].part3 ? "true" : "false");
		/* in contact right now while it holds a link */
		if (seen || linked)
			fprintf(f, ",\"seen_ms_ago\":%llu", linked ? 0ull :
				(unsigned long long)(now - seen));
		fprintf(f, "}");
		first = 0;
	}
	fprintf(f, "],\"links\":[");
	first = 1;
	if (radio_on && s->base.main_link.active)
		state_link(s, f, &s->base.main_link.link,
			   s->reg.last_status.valid ? &s->reg.last_status
						    : NULL,
			   s->reg.frames_total, &first);
	for (i = 0; radio_on && i < SIDE_MAX; i++)
		if (s->side[i].bl->active)
			state_link(s, f, &s->side[i].bl->link,
				   s->side[i].bl->lane >= 0 &&
				   s->side[i].last_status.valid
				   ? &s->side[i].last_status : NULL,
				   s->side[i].frames.total, &first);
	fprintf(f, "]}\n");
	if (fclose(f) == 0)
		(void)rename(tmp, path);
	else
		unlink(tmp);
}

/*
 * Move whatever the main link's LAPC has queued onto the wire, as 0x0244
 * mails. The framing, the Lc signature and the 0x0244 mail are the
 * library's (openDECTcell#67); ours is only the transport.
 */
void pump_lapc(struct stubd *s)
{
	struct dect_mail m;

	/*
	 * Take a frame out of LAPC only when the control queue can hold a
	 * whole mail: once dequeued it is gone, and LAPC retransmits a lost
	 * I-frame by timer but never a lost RR or UA.
	 */
	while (sizeof(s->txq) - s->txq_len >= DECT_TX_ELEM_MAX &&
	       dect_link_poll_append_mail(&s->base.main_link.link, &m)) {
		if (s->verbose)
			printf("[%5llu] reg: tx cplane %u bytes\n",
			       (unsigned long long)(mono_ms() - g_start),
			       (unsigned)m.param_len);
		if (send_mail(s, (unsigned int)s->tx_slot, &m))
			printf("reg: C-plane mail LOST (%u bytes) -- send "
			       "failed\n", (unsigned)m.param_len);
		else
			s->led_traffic++;
	}
}


/*
 * Open or close the registration window: the chip's mode (0x022B 'A' /
 * 0x022C 01 [CR COMMAND_SET §6.2]) and MM's policy together. secs = 0
 * keeps it open until closed.
 */
void reg_window(struct stubd *s, int open, unsigned secs,
		       uint64_t now, const char *why)
{
	if (!open)
		s->reg_profile[0] = '\0';	/* the choice was for this window */
	dect_base_reg_window(&s->base, open, secs, now, why);
	fflush(stdout);
}

/* SIGUSR1 opens a window; a timed one closes itself. */
void reg_window_tick(struct stubd *s, uint64_t now)
{
	/*
	 * Not before the start-up mails are out: one of them sets the chip's
	 * registration mode, and it would undo a window opened earlier
	 * (review, finding 11).
	 */
	if (s->sess.state != DECT_SESS_UP || s->tx_slot < 0 ||
	    s->xmail_idx < s->n_xmail)
		return;
	if (want_register) {
		want_register = 0;
		s->reg_profile[0] = '\0';	/* a window without a pick */
		reg_window(s, 1, s->audio.reg_signal_s, now, "SIGUSR1");
	}
	if (s->base.reg_open && s->base.reg_open_until && now >= s->base.reg_open_until)
		reg_window(s, 0, 0, now, "window elapsed");
}

/* dect_base's link_up(): our own state for a link set up afresh. */
void base_link_up(void *ctx, int p)
{
	struct stubd *s = ctx;
	struct side_link *sl;

	if (p < 0) {
		s->fetch_retrans_seen = 0;	/* MM's counter restarts */
		s->reg.call_wideband = 0;
		s->reg.nonspeech_noted = 0;
		dect_link_set_tx_log(&s->base.main_link.link, log_nwk_out, s);
		return;
	}
	sl = &s->side[p];
	dect_link_set_tx_log(&sl->bl->link, log_nwk_out_side, s);
	memset(&sl->last_status, 0, sizeof(sl->last_status));
	memset(&sl->frames, 0, sizeof(sl->frames));
	sl->status_ms = 0;
}

/* dect_base's side_down(): never leak a side link's lane. */
void base_side_down(void *ctx, unsigned k)
{
	struct stubd *s = ctx;

	xlane_release(s, &s->side[k]);
}

/* Slot and identity of handset table entry i, current or stashed. */
unsigned hs_slot(const struct stubd *s, unsigned i)
{
	return dect_base_slot(&s->base, i);
}

/* The handset's name for the lists and the internal caller (dect_base.h). */
void hs_name(const struct stubd *s, unsigned i, char *buf, size_t cap)
{
	dect_base_hs_name(&s->base, i, buf, cap);
}

/* <path>.name, one line of UTF-8, beside the subscription store. */
void name_path(const struct stubd *s, unsigned i, char *buf, size_t cap)
{
	snprintf(buf, cap, "%s.name", s->hs[i].path);
}

/*
 * A handset's lines (Network > DECT, Handsets, Line 1 / Line 2): written by
 * the init script as /etc/opendectcell/handset<N>.lines, one "<line> <name>" per
 * line, Line 2 only when its account is on. Its Line Settings List shows
 * these; without the file, Line 1 under the base's default name.
 */
static void lines_load(struct stubd *s, unsigned i)
{
	char path[256], buf[64];
	FILE *f;

	s->base.line_name[i][0][0] = s->base.line_name[i][1][0] = '\0';
	s->base.line2[i] = 0;
	snprintf(path, sizeof(path), "/etc/opendectcell/handset%u.lines",
		 hs_slot(s, i));
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(buf, sizeof(buf), f)) {
		char *nm = buf + 2;

		buf[strcspn(buf, "\r\n")] = '\0';
		if ((buf[0] != '1' && buf[0] != '2') || buf[1] != ' ')
			continue;
		snprintf(s->base.line_name[i][buf[0] - '1'],
			 sizeof(s->base.line_name[i][0]), "%.15s", nm);
		if (buf[0] == '2')
			s->base.line2[i] = 1;
	}
	fclose(f);
	printf("sub: DECT %u's lines: 1 \"%s\"%s%s%s\n", hs_slot(s, i),
	       s->base.line_name[i][0], s->base.line2[i] ? ", 2 \"" : "",
	       s->base.line2[i] ? s->base.line_name[i][1] : "",
	       s->base.line2[i] ? "\"" : "");
}

/* The lines a link's Line Settings List offers: those of its handset. */
struct dect_contacts stubd_contacts;	/* every link's Contact List */

struct dect_calls stubd_calls;		/* ... and call lists */

/*
 * The Contact List's file, once a second: read whole when its size or time
 * changed (the sync replaces it by rename); and the changes handsets made,
 * appended to FILE.changes for the sync to write back. Formats in
 * dect_contacts.h. A journal line that cannot be written stays queued.
 */
void contacts_tick(struct stubd *s, uint64_t now)
{
	struct dect_contacts *c = &stubd_contacts;
	char path[300], line[512];
	struct stat st;
	size_t n;
	int fd;

	if (!s->contacts_path)
		return;
	if (c->n_journal) {
		snprintf(path, sizeof(path), "%s.changes", s->contacts_path);
		fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
		while (fd >= 0 && (n = dect_contacts_take_change(c, line,
								 sizeof(line) - 1u))) {
			line[n++] = '\n';
			if (write(fd, line, n) != (ssize_t)n) {
				fprintf(stderr, "contacts: %s: %s\n", path,
					strerror(errno));
				break;
			}
		}
		if (fd >= 0)
			close(fd);
	}
	if (now - s->contacts_at < 1000u)
		return;
	s->contacts_at = now;
	if (stat(s->contacts_path, &st) ||
	    ((long long)st.st_mtime == s->contacts_mtime &&
	     (long long)st.st_size == s->contacts_size))
		return;
	s->contacts_mtime = (long long)st.st_mtime;
	s->contacts_size = (long long)st.st_size;
	if (st.st_size > 1024 * 1024) {
		fprintf(stderr, "contacts: %s is larger than 1 MiB, not read\n",
			s->contacts_path);
		return;
	}
	{
		char *text = malloc((size_t)st.st_size + 1u);
		FILE *f = fopen(s->contacts_path, "r");

		n = text && f ? fread(text, 1, (size_t)st.st_size, f) : 0;
		if (f)
			fclose(f);
		if (!text)
			return;
		if (dect_contacts_load(c, text, n))
			fprintf(stderr, "contacts: %s is not a contacts file\n",
				s->contacts_path);
		else
			printf("contacts: %u from %s%s\n", c->n,
			       s->contacts_path,
			       c->dropped ? " (some lines not taken)" : "");
		free(text);
	}
}

static char calls_text[CALLS_TEXT_MAX];

static uint64_t calls_at, calls_hash;

static uint64_t calls_fnv(const char *t, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	while (n--)
		h = (h ^ (unsigned char)*t++) * 0x100000001b3ull;
	return h;
}

void calls_tick(uint64_t now, int force)
{
	size_t n;
	uint64_t h;
	FILE *f;
	int ok;

	if (!force && now - calls_at < 1000u)
		return;
	calls_at = now;
	n = dect_calls_save(&stubd_calls, calls_text, sizeof(calls_text));
	h = calls_fnv(calls_text, n);
	if (!n || h == calls_hash)
		return;
	f = fopen_private(CALLS_PATH ".tmp");
	if (!f)
		return;
	ok = fwrite(calls_text, 1, n, f) == n;
	if (fclose(f) == 0 && ok && rename(CALLS_PATH ".tmp", CALLS_PATH) == 0) {
		calls_hash = h;
		return;
	}
	fprintf(stderr, "calls: %s: %s\n", CALLS_PATH, strerror(errno));
	(void)unlink(CALLS_PATH ".tmp");
}

static void calls_load(void)
{
	FILE *f = fopen(CALLS_PATH, "r");
	size_t n;

	dect_calls_init(&stubd_calls);
	if (!f)
		return;
	n = fread(calls_text, 1, sizeof(calls_text), f);
	fclose(f);
	if (dect_calls_load(&stubd_calls, calls_text, n)) {
		fprintf(stderr, "calls: %s is not a calls file\n", CALLS_PATH);
		return;
	}
	printf("calls: %u missed, %u outgoing, %u accepted from %s\n",
	       stubd_calls.n[0], stubd_calls.n[1], stubd_calls.n[2], CALLS_PATH);
	n = dect_calls_save(&stubd_calls, calls_text, sizeof(calls_text));
	calls_hash = calls_fnv(calls_text, n);
}

/*
 * Journal numbers go on from the last one written before a restart, the
 * contacts file's seq=N and every FILE.changes* line, so a change after
 * the restart is never taken for one the sync has already applied.
 */
void contacts_init(struct stubd *s)
{
	static const char *const ext[] = { ".changes", ".changes.work" };
	char path[300], line[512];
	unsigned long v;
	unsigned i;
	FILE *f;

	calls_load();
	/* untouched without -b: the store's pages are never mapped in */
	if (!s->contacts_path)
		return;
	dect_contacts_init(&stubd_contacts);
	for (i = 0; i < 2u; i++) {
		snprintf(path, sizeof(path), "%s%s", s->contacts_path, ext[i]);
		f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f)) {
			v = strtoul(line, NULL, 10);
			if (v > stubd_contacts.seq && v < 0xFFFFFFFFul)
				stubd_contacts.seq = (uint32_t)v;
		}
		if (f)
			fclose(f);
	}
	s->contacts_mtime = -1;
}

void name_load(struct stubd *s, unsigned i)
{
	char path[256];
	FILE *f;
	size_t n;

	name_path(s, i, path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return;
	n = fread(s->base.name[i], 1, sizeof(s->base.name[i]) - 1u, f);
	fclose(f);
	s->base.name[i][n] = '\0';
	s->base.name[i][strcspn(s->base.name[i], "\r\n")] = '\0';
	if (s->base.name[i][0])
		printf("sub: %s is called \"%s\"\n", s->hs[i].path,
		       s->base.name[i]);
}

void name_save(const struct stubd *s, unsigned i)
{
	char path[256];
	FILE *f;

	name_path(s, i, path, sizeof(path));
	if (!s->base.name[i][0]) {
		(void)remove(path);		/* back to the default */
		return;
	}
	f = fopen(path, "w");
	if (!f) {
		printf("names: cannot write %s\n", path);
		return;
	}
	fprintf(f, "%s\n", s->base.name[i]);
	fclose(f);
}

/*
 * Handset i's profile: <path>.profile names the model's id (and on a
 * second line its firmware version); the device file and its manufacturer
 * file come from the profile directory. Resolved into base.prof[i] and
 * logged once, with each key that differs from the standard and the keys
 * this build accepts but does not act on yet. Anything missing or broken:
 * the defaults, said so.
 */
static void profile_resolve_choice(struct stubd *s, unsigned i)
{
	static struct dect_profile dev, mfr;
	const char *dir = s->audio.profile_dir ? s->audio.profile_dir
					       : PROFILE_DIR_DEFAULT;
	char path[320], line[64], desc[1024];
	const struct dect_profile *m = NULL, *d = NULL;
	struct dect_hs_profile *hp = &s->base.prof[i], def;
	FILE *f;

	dect_profile_defaults(hp);
	s->hs[i].prof_id[0] = '\0';
	s->hs[i].prof_fw[0] = '\0';
	snprintf(path, sizeof(path), "%s.profile", s->hs[i].path);
	f = fopen(path, "r");
	if (!f)
		return;				/* no choice made: standard */
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, " \t\r\n")] = '\0';
		snprintf(s->hs[i].prof_id, sizeof(s->hs[i].prof_id), "%.31s",
			 line);
	}
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, " \t\r\n")] = '\0';
		snprintf(s->hs[i].prof_fw, sizeof(s->hs[i].prof_fw), "%.23s",
			 line);
	}
	fclose(f);
	if (!s->hs[i].prof_id[0])
		return;
	snprintf(path, sizeof(path), "%s/%s.profile", dir, s->hs[i].prof_id);
	if (profile_load(path, &dev) || strcmp(dev.id, s->hs[i].prof_id)) {
		printf("profile: %s: %s unusable -- standard defaults\n",
		       s->hs[i].path, path);
		return;
	}
	if (dev.kind == DECT_PROFILE_DEVICE) {
		d = &dev;
		snprintf(path, sizeof(path), "%s/%s.profile", dir, dev.extends);
		if (profile_load(path, &mfr) ||
		    mfr.kind != DECT_PROFILE_MANUFACTURER ||
		    strcmp(mfr.id, dev.extends))
			printf("profile: %s: manufacturer file %s unusable -- "
			       "the device layer alone\n", s->hs[i].path,
			       path);
		else
			m = &mfr;
	} else {
		m = &dev;			/* a manufacturer file chosen */
	}
	dect_profile_resolve(m, d, s->hs[i].prof_fw, hp);
	if (dect_profile_describe(hp, desc, sizeof(desc)))
		printf("profile: DECT %u = %s%s%s%s%s: %s\n", hs_slot(s, i),
		       s->hs[i].prof_id, d && m ? " (via " : "",
		       d && m ? m->id : "", d && m ? ")" : "",
		       s->hs[i].prof_fw[0] ? ", firmware given" : "", desc);
	else
		printf("profile: DECT %u = %s: the standard defaults\n",
		       hs_slot(s, i), s->hs[i].prof_id);
	/* accepted but not acted on in this build: say so, not silently */
	dect_profile_defaults(&def);
	if (hp->cc_ring_callinfo != def.cc_ring_callinfo ||
	    hp->cc_connect_status_info != def.cc_connect_status_info ||
	    hp->cc_bind_before_connect != def.cc_bind_before_connect ||
	    hp->cc_name_alphabet != def.cc_name_alphabet ||
	    hp->cc_timer != def.cc_timer ||
	    hp->cc_number_method != def.cc_number_method ||
	    hp->lists_serve != def.lists_serve ||
	    hp->line_selection != def.line_selection ||
	    hp->cc_signal_internal != def.cc_signal_internal ||
	    hp->feature_call_waiting != def.feature_call_waiting ||
	    hp->feature_call_lists != def.feature_call_lists ||
	    hp->feature_contacts != def.feature_contacts)
		printf("profile: DECT %u: some of its keys are not acted on "
		       "in this build (see docs/handset-profiles.md, "
		       "'today')\n", hs_slot(s, i));
}

static void ovr_say(void *ctx, unsigned line, const char *msg)
{
	printf("profile: %s, line %u: %s\n", (const char *)ctx, line, msg);
}

static void profile_resolve(struct stubd *s, unsigned i)
{
	char path[320], text[4096], desc[1024];
	size_t n;
	FILE *f;

	profile_resolve_choice(s, i);
	snprintf(path, sizeof(path), "%s/handset%u.override", OVERRIDE_DIR,
		 hs_slot(s, i));
	f = fopen(path, "r");
	if (!f)
		return;
	n = fread(text, 1, sizeof(text), f);
	fclose(f);
	if (n == sizeof(text)) {
		printf("profile: %s is too long -- not applied\n", path);
		return;
	}
	if (dect_profile_override(text, n, &s->base.prof[i], ovr_say, path)) {
		printf("profile: DECT %u's own settings (%s) not applied\n",
		       hs_slot(s, i), path);
		return;
	}
	if (dect_profile_describe(&s->base.prof[i], desc, sizeof(desc)))
		printf("profile: DECT %u with its own settings: %s\n",
		       hs_slot(s, i), desc);
}

/*
 * The handset that just enrolled into the current store gets the profile
 * picked for it before the registration (register-window profile=): the
 * choice is written where a later pick in LuCI goes, <store>.profile.
 */
void reg_profile_apply(struct stubd *s)
{
	char path[320];
	unsigned i;
	FILE *f;

	for (i = 0; i < s->base.n_hs; i++)
		if (s->sub_path && !strcmp(s->hs[i].path, s->sub_path))
			break;
	if (i == s->base.n_hs)
		return;
	snprintf(path, sizeof(path), "%s.profile", s->hs[i].path);
	f = fopen(path, "w");
	if (!f || fprintf(f, "%s\n", s->reg_profile) < 0 || fclose(f)) {
		printf("profile: could not write %s\n", path);
		return;
	}
	printf("profile: DECT %u registered as %s\n", hs_slot(s, i),
	       s->reg_profile);
	profile_resolve(s, i);
}

/* A profile id the user may pick: a file in the profile directory. */
int reg_profile_ok(const struct stubd *s, const char *id)
{
	const char *dir = s->audio.profile_dir ? s->audio.profile_dir
					       : PROFILE_DIR_DEFAULT;
	char path[320];
	size_t n = strlen(id);

	if (!n || n >= DECT_PROFILE_ID_MAX ||
	    strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-_") != n)
		return 0;
	snprintf(path, sizeof(path), "%s/%s.profile", dir, id);
	return access(path, R_OK) == 0;
}

void profiles_resolve_all(struct stubd *s, const char *why)
{
	unsigned i;

	printf("profile: resolving the handsets' profiles (%s)\n", why);
	for (i = 0; i < s->base.n_hs; i++) {
		profile_resolve(s, i);
		lines_load(s, i);	/* their line names travel with it */
	}
	fflush(stdout);
}

void missed_changed_tick(struct stubd *s)
{
	dect_base_missed_changed_tick(&s->base);
}

/* dect_base's named(): keep the name in its .name file, tell the clients. */
void base_named(void *ctx, unsigned i)
{
	struct stubd *s = ctx;

	name_save(s, i);
	ctl_hs(s, i, "registered");
}

void names_tick(struct stubd *s)
{
	if (want_profiles) {
		want_profiles = 0;
		profiles_resolve_all(s, "SIGHUP");
	}
	/* the lists' hooks per link are the library's (dect_base.h, S4d) */
	dect_base_lists_tick(&s->base, mono_ms());
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd: the daemon that runs the base on a box. It keeps the STUB
 * control channel up, hosts the base-station stack of libopendectcell (struct
 * dect_base: handsets, calls, registration, lists), serves the control and
 * audio sockets to the SIP side, and in cell mode (-K) hands every chip mail
 * to dect-cell instead. See src/stubd/README.md for a map of this file.
 *
 * Below the stack it brings up the PCM receive and transmit path step by step (the same
 * six DECT_IOC_PCM_STEP bring-up as dect-pcmcap), finds the control slot,
 * sends the 0xF600 contact mail and then maintains the link: it decodes
 * the STUB's 0xAA-framed mails (libopendectcell/dect_mail.h), runs the contact
 * and liveness state machine (libopendectcell/dect_session.h), and re-contacts
 * the STUB when it goes quiet.
 *
 * The control slot is not at a fixed position: after a STUB restart the
 * DMA buffers can come out rotated (docs/stub-pcm-control-channel.md), so
 * the slot is found by probing, not hard-coded. Transmit and receive may
 * be rotated independently, so the transmit slot (the one the STUB answers
 * in) and the receive slot (where the answer appears) are found separately.
 *
 * This is the only part of the control channel with device I/O; the
 * framing and the state machine are platform-free and unit-tested on the PC.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/* Free bytes on the filesystem holding `path`, or 0 if it cannot be read. */
static uint64_t fs_free_bytes(const char *path)
{
	char dir[512];
	struct statvfs vfs;
	char *slash;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	if (statvfs(slash ? dir : ".", &vfs) != 0)
		return 0;
	return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}

static time_t wall_now(void)
{
#ifdef STUBD_TEST
	return stubd_test_wall;
#else
	return time(NULL);
#endif
}

static int clock_is_trustworthy(time_t now_s, uint8_t mode)
{
	struct timex tx;

	if (mode == TIMESYNC_OFF)
		return 0;
	/*
	 * The epoch floor holds even when forced: it catches the case this
	 * whole gate exists for, a box that has never had a clock at all.
	 */
	if (now_s <= CLOCK_SANE_AFTER)
		return 0;
	if (mode == TIMESYNC_FORCE)
		return 1;
	memset(&tx, 0, sizeof(tx));
	if (adjtimex(&tx) >= 0 && !(tx.status & STA_UNSYNC))
		return 1;
	{
		struct stat st;

		return stat(CLOCK_NTP_FILE, &st) == 0 &&
		       now_s >= st.st_mtime &&
		       now_s - st.st_mtime <= CLOCK_NTP_MAX_S;
	}
}

/*
 * Seconds east of GMT at `t`. `tm_gmtoff` would say this in one line but it
 * is a BSD extension that musl hides without a feature macro, and this file
 * cross-compiles against musl. The difference of the two broken-down times is
 * portable C: take the seconds-of-day difference, then correct by a day when
 * the two land on different dates (including across a year boundary, where
 * tm_yday jumps by ~364 rather than 1).
 */
static long tz_offset_seconds(time_t t)
{
	struct tm lt, gt;
	long d;
	int dday;

	if (!localtime_r(&t, &lt) || !gmtime_r(&t, &gt))
		return 0;
	d = (lt.tm_hour - gt.tm_hour) * 3600L
	  + (lt.tm_min  - gt.tm_min)  * 60L
	  + (lt.tm_sec  - gt.tm_sec);
	dday = lt.tm_yday - gt.tm_yday;
	if (dday == 1 || dday < -1)
		d += 86400L;
	else if (dday == -1 || dday > 1)
		d -= 86400L;
	return d;
}

/*
 * The box's local time as a <<TIME-DATE>> value, when the clock may be
 * trusted (clock_is_trustworthy). Returns 1 with dt and off_s (seconds east
 * of GMT) filled, 0 when there is nothing we may send.
 */
/*
 * The box's local time for a call list entry: a call happened whether the
 * clock is trusted or not, and a handset shows the time it is given.
 */
void clock_any(struct dect_time *dt)
{
	time_t now_s = wall_now();
	struct tm tm;
	long off_s = tz_offset_seconds(now_s);

	memset(dt, 0, sizeof(*dt));
	if (!localtime_r(&now_s, &tm))
		return;
	dt->year   = (uint8_t)(tm.tm_year % 100);
	dt->month  = (uint8_t)(tm.tm_mon + 1);
	dt->day    = (uint8_t)tm.tm_mday;
	dt->hour   = (uint8_t)tm.tm_hour;
	dt->minute = (uint8_t)tm.tm_min;
	dt->second = (uint8_t)tm.tm_sec;
	dt->tz_quarters = (int16_t)(off_s / 900);
	dt->tz_valid = 1;
}

int clock_now(uint8_t mode, struct dect_time *dt, long *off_s)
{
	time_t now_s = wall_now();
	struct tm tm;

	if (!clock_is_trustworthy(now_s, mode) || !localtime_r(&now_s, &tm))
		return 0;
	*off_s = tz_offset_seconds(now_s);
	memset(dt, 0, sizeof(*dt));
	dt->year   = (uint8_t)(tm.tm_year % 100);
	dt->month  = (uint8_t)(tm.tm_mon + 1);
	dt->day    = (uint8_t)tm.tm_mday;
	dt->hour   = (uint8_t)tm.tm_hour;
	dt->minute = (uint8_t)tm.tm_min;
	dt->second = (uint8_t)tm.tm_sec;
	dt->tz_quarters = (int16_t)(*off_s / 900);
	dt->tz_valid = 1;
	return 1;
}

volatile sig_atomic_t stop;

volatile sig_atomic_t want_register;	/* SIGUSR1: open a window */

uint64_t g_start;		/* for relative timestamps in the log */

void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void on_usr1(int sig)
{
	(void)sig;
	want_register = 1;
}

volatile sig_atomic_t want_deregister;	/* SIGUSR2: see dereg_tick */

volatile sig_atomic_t want_profiles;	/* SIGHUP: reload profiles */

static void on_hup(int sig)
{
	(void)sig;
	want_profiles = 1;
}

static void on_usr2(int sig)
{
	(void)sig;
	want_deregister = 1;
}

uint64_t mono_ms(void)
{
#ifdef STUBD_TEST
	return stubd_test_ms;
#else
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

uint32_t sub_uak_base = SUB_UAK_BASE_0825;

/* Route a received mail into the registration stack (when -g). */
static void handle_reg_mail(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	switch (m->prim) {
	/* the connection mails are the library's (dect_base.h, S6a) */
	case 0x0209:
		if (m->param_len >= 1)
			dect_base_cipher_on(&s->base, m->param[0]);
		break;
	case DECT_PRIM_CONTACT_ACK1:	/* 0xF601 carries the base RFPI */
		dect_base_contact(&s->base, m);
		break;
	case DECT_PRIM_CONN_UP:
		dect_base_conn_up(&s->base, m->param_len ? m->param[0] : 0, now);
		break;
	case DECT_PRIM_CONN_DOWN:
		dect_base_conn_down(&s->base, m->param_len ? m->param[0] : 0,
				    m->param_len != 0);
		break;
	case DECT_PRIM_SIGNALLING: {
		s->led_traffic++;
		/* the segments are the library's (dect_base.h, S6b) */
		(void)dect_base_segment(&s->base, m, now);
		break;
	}
	case 0x0242:
		/*
		 * {handle, chan_flags, mask}. The low nibble is the channel
		 * the chip names; 0 or 0xF means none. Only the long form
		 * follows 0x0230 [CR COMMAND_SET §8.4]; the short one reports
		 * later state changes.
		 *
		 * NOT a confirmation, though it was called one here until
		 * 2026-10-23. `[CR A-0075]`: 0x0242 is emitted on EVERY path
		 * through the chip's handler, including the early-out that
		 * does nothing -- it means "mail processed", not "matrix
		 * changed". vF418 holds six byte-identical 0x0242 replies to
		 * sends that changed nothing, beside one that worked.
		 *
		 * We still use the channel it names, because that is still
		 * the channel. What we must not do is read arrival as success:
		 * the act/no-act gate is the connection record's +0x116 bit 5,
		 * which we do not yet read. Until we do, binding audio here is
		 * an assumption, and the run summary says so.
		 */
		if (s->base.main_link.bound && !s->base.main_link.path_on &&
		    m->param_len >= 4 && m->param[0] == s->base.main_link.handle) {
			uint8_t chan = m->param[1] & 0x0Fu;

			/*
			 * Valid STUB channels are 0..9 (ADPCM 0..3, PCM 4..6,
			 * wideband 7..9); 0xF means "not connected"
			 * [CR the hardware-analysis notes §6 1.6]. We used to stop at 6,
			 * which silently skipped the connect for a wideband
			 * channel.
			 */
			if (chan >= 1u && chan <= 9u)
				send_audio_connect(s, s->base.main_link.handle, chan);
		} else if (m->param_len >= 2 && xlane_of(s, m->param[0])) {
			xlane_connect(s, xlane_of(s, m->param[0]));
		}
		break;
	case 0xFFF1u:
		mem_reply(s, m);
		break;
	case 0xF625:
		/* [0] echoes the request's link: the main call or a side one */
		if (m->param_len >= 1 && s->base.main_link.active &&
		    m->param[0] != s->base.main_link.handle) {
			struct side_link *sl = side_find(s, m->param[0]);
			struct dect_diag_status st;

			if (sl && sl->bl->active &&
			    !dect_diag_decode_status(m->param, m->param_len,
						     &st)) {
				sl->last_status = st;
				if (st.has_counters)
					(void)dect_diag_frames_update(
						&sl->frames, &st);
			}
			break;
		}
		if (s->base.main_link.bound)
			report_conn_status(s, m);
		break;
	default:
		break;
	}
}

/*
 * Downlink starvation for the last second: how many transmit frames the driver
 * had to fill with the idle template because our queue had nothing ready.
 *
 * The DECT chip is the TDM clock master and the SoC its slave, so a transmit
 * block is refilled when the chip asks for one, not when we are ready. Any
 * frame the queue cannot supply is sent as the idle pattern [see pcm_tx_fill()
 * in kernel/opendectcell_pcm.c], which is a discontinuity in the handset's ear. One
 * DMA block is 32 frames of 125 us = 4 ms, the same block size stock's host
 * side uses [CR A-DSP-1 §8].
 *
 * This is the direct readout for the clock-crossing reading of the downlink
 * noise [CR A-DSP-1 §7/§10]: a free-running source should starve the queue at
 * a measurable rate, a transition-paced one should not. It is a measurement,
 * not a verdict -- the number has never been taken per-second, only summed at
 * exit, where priming and playback could not be told apart.
 */
static void report_tx_starvation(struct stubd *s)
{
	struct dect_pcm_stats ps;
	uint32_t d_blocks, d_frames, want, idle;

	if (s->ssc) {
		/* The SSC driver counts idle frames itself. */
		struct ssc_pcm_stats st;

		memset(&st, 0, sizeof(st));
		if (!s->fpb || ioctl(s->fd, SSC_PCM_IOC_STATS, &st))
			return;
		if (s->pcm_tx_primed) {
			idle = (uint32_t)(st.tx_idle - s->ssc_tx_idle_prev);
			want = idle + (uint32_t)(st.tx_frames -
						 s->ssc_tx_sent_prev);
			if (want)
				printf("call: downlink starvation: %u of %u "
				       "frames idle-filled (%u.%02u%%), %u "
				       "blocks\n", idle, want,
				       idle * 100u / want,
				       (unsigned)((uint64_t)idle * 10000u /
						  want % 100u),
				       want / s->fpb);
		}
		s->pcm_tx_primed = 1;
		s->ssc_tx_idle_prev = st.tx_idle;
		s->ssc_tx_sent_prev = st.tx_frames;
		return;
	}
	if (!s->fpb || ioctl(s->fd, DECT_IOC_PCM_STATS, &ps))
		return;
	if (!s->pcm_tx_primed) {
		s->pcm_tx_primed = 1;
		goto save;
	}
	d_blocks = ps.tx_blocks - s->pcm_tx_blocks_prev;
	d_frames = ps.tx_frames - s->pcm_tx_frames_prev;
	want = d_blocks * s->fpb;
	if (!want)
		goto save;
	/*
	 * d_frames can only exceed want if a counter wrapped or the ioctl
	 * raced the refill; report nothing rather than a negative dressed up
	 * as a huge unsigned.
	 */
	idle = d_frames > want ? 0u : want - d_frames;
	printf("call: downlink starvation: %u of %u frames idle-filled "
	       "(%u.%02u%%), %u blocks\n",
	       idle, want, idle * 100u / want,
	       (unsigned)((uint64_t)idle * 10000u / want % 100u), d_blocks);
save:
	s->pcm_tx_blocks_prev = ps.tx_blocks;
	s->pcm_tx_frames_prev = ps.tx_frames;
}

/*
 * While a call is up, count per-slot sample changes. The STUB's audio-channel ->
 * TDM-slot mapping is not documented (docs/stub-pcm-control-channel.md), so find
 * the call's voice slot empirically: it is the one that keeps changing while the
 * idle slots hold a constant fill. Read-only.
 */
static void voice_slot_probe(struct stubd *s, const uint8_t *fr, unsigned int fpb)
{
	/*
	 * The 7520's eight slots, or every lane of the 7412's SSC frame: its
	 * voice lanes are not known yet (Q-0015), and the frame starts at a
	 * different lane every run, so the SSC report counts from the
	 * control lane.
	 */
	unsigned int f, n, nslots = s->ssc ? (s->slots < 64u ? s->slots : 64u)
					   : (s->slots < 8u ? s->slots : 8u);

	if (!s->base.main_link.bound || (!s->verbose && !s->voice_fp))
		return;
	for (f = 0; f < fpb; f++) {
		for (n = 0; n < nslots; n++) {
			uint16_t v;

			memcpy(&v, fr + (f * s->slots + n) * 2u, 2);
			if (s->vs_frames && v != s->vs_last[n])
				s->vs_changes[n]++;
			s->vs_last[n] = v;
		}
		s->vs_frames++;
	}
	if (s->vs_frames >= 8000u) {		/* 8 kHz frame rate: about one second */
		if (s->ssc && s->rx_slot >= 0) {
			printf("call: lane activity (changes/s, lane = rx "
			       "control lane + k, only k that changed):");
			for (n = 0; n < nslots; n++) {
				unsigned int ln = (s->rx_slot + n) % nslots;

				if (s->vs_changes[ln])
					printf(" +%u:%u", n, s->vs_changes[ln]);
			}
			printf("\n");
		} else {
			printf("call: slot activity (changes/s):");
			for (n = 0; n < nslots; n++)
				printf(" %u:%u", n, s->vs_changes[n]);
			printf("\n");
		}
		report_tx_starvation(s);
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;
	}
}

/*
 * A lab file that may hold call audio or chip mails: the owner's alone,
 * and never through a link someone else placed (security audit
 * 2026-10-29, F5).
 */
FILE *fopen_private(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);

	return fd < 0 ? NULL : fdopen(fd, "wb");
}

/*
 * -w: dump the call's voice slot (-V), or every slot, while a call is bound:
 * the raw two bytes per slot and frame, in frame order, with nothing
 * interpreted. Decoding happens offline -- tools/voice-decode.py tries the
 * candidate formats against the file. Every slot is the default so that one
 * capture can be searched for the voice: A-0029 §4 warns that a 16-bit TDM
 * word may hold two independent 8-bit channels and that a slot picked by
 * index in the DMA buffer is not necessarily the logical channel.
 */
static void voice_capture(struct stubd *s, const uint8_t *fr, unsigned int fpb)
{
	uint8_t *cp = s->voice_buf;
	unsigned int f;
	size_t n;

	/*
	 * Normally only a bound call is captured, which keeps the files small.
	 * `-a capall` captures from start-up instead, so the TDM lanes can be
	 * watched with the radio off -- which is how the chip's DSP routing is
	 * probed without transmitting anything.
	 */
	if (!s->voice_fp || (!s->base.main_link.bound && !s->audio.cap_all))
		return;

	/*
	 * Check the filesystem every megabyte or so and stop before it fills.
	 * The capture's own writes report ENOSPC, but the log's do not, so a
	 * full filesystem silences the daemon instead of failing it.
	 */
	if (s->voice_bytes - s->voice_space_checked > (1u << 20)) {
		uint64_t have = fs_free_bytes(s->voice_path);

		s->voice_space_checked = s->voice_bytes;
		if (have && have < CAPTURE_FLOOR) {
			printf("voice capture: stopping, only %llu MiB left on "
			       "%s -- the log would go silent next\n",
			       (unsigned long long)(have >> 20), s->voice_path);
			fflush(stdout);
			fclose(s->voice_fp);
			s->voice_fp = NULL;
			return;
		}
	}

	if (s->voice_slot < 0) {
		n = (size_t)fpb * s->slots * 2u;
		memcpy(cp, fr, n);
		/*
		 * Not the control slot: it carries the chip's mails, and a
		 * key read back from the chip among them. Idle bytes there.
		 */
		if (s->rx_slot >= 0 && (unsigned int)s->rx_slot < s->slots)
			for (f = 0; f < fpb; f++)
				memset(cp + (f * s->slots +
					     (unsigned int)s->rx_slot) * 2u,
				       0xff, 2);
	} else {
		if ((unsigned int)s->voice_slot >= s->slots ||
		    s->voice_slot == s->rx_slot)
			return;
		for (f = 0; f < fpb; f++)
			memcpy(cp + f * 2u, fr + (f * s->slots +
			       (unsigned int)s->voice_slot) * 2u, 2);
		n = (size_t)fpb * 2u;
	}
	if (fwrite(cp, 1, n, s->voice_fp) == n) {
		s->voice_bytes += (uint32_t)n;
		return;
	}
	perror("voice capture");
	fclose(s->voice_fp);
	s->voice_fp = NULL;
}

/*
 * One pass of everything above the chip transport, after the received
 * blocks: the timers, the queues, the links, the sockets. run_loop() calls
 * it once per pass; the daemon-level traces (tests/test_stubd.c) call it
 * with their own clock.
 */
static void loop_tick(struct stubd *s, uint64_t now)
{
	/* in cell mode the radio is on when the controller turned it on */
	led_tick(s, now, (s->do_register && s->sess.state == DECT_SESS_UP) ||
		 (s->cell_mode && s->cell.radio_on));
	state_write(s, now, s->do_register &&
		    s->sess.state == DECT_SESS_UP, 0);
	holdback_tick(s, now);
	dect_base_sub_check_tick(&s->base, now);
	fetch_tick(s, now);
	dial_tick(s, now);
	info_tick(s, now);
	cs_connect_tick(s, now);
	nlr_tick(s, now);
	connect_tick(s, now);
	reg_window_tick(s, now);
	contacts_tick(s, now);
	calls_tick(now, 0);
	missed_changed_tick(s);
	{
		unsigned k;

		for (k = 0; k < SIDE_MAX; k++)
			if (s->side[k].bl->active)
				side_call_tick(s, &s->side[k], now);
		ic_tick(s, now);
		dereg_tick(s, now);
		time_tick(s, now, s->do_register &&
			  s->sess.state == DECT_SESS_UP);
		ctl_tick(s, now);
		bridge_tick(s, now);
		ab_tick(s, now);
		if (s->cell_mode)
			cell_tick(s, now);
	}
	if (s->do_register) {
		audio_connect_fallback(s, now);
		send_conn_status_req(s, now);
		ring_tick(s, now);
	}
	if (s->sess.state == DECT_SESS_UP)
		mem_dump_tick(s, now);
	remode_tick(s, now);
	seq_tick(s, now);
	if (dect_session_tick(&s->sess, now) == DECT_ACT_SEND_CONTACT) {
		struct dect_mail cm;

		dect_session_contact_mail(&s->sess, &cm);
		if (send_mail(s, (unsigned int)s->tx_slot, &cm))
			fprintf(stderr, "contact send failed\n");
		else if (s->verbose)
			log_mail(&cm, "tx  ", s->show_params);
	}
	/* Once up, send the extra mails in order, one per tick (~100 ms). */
	if (s->sess.state == DECT_SESS_UP && s->xmail_idx < s->n_xmail) {
		if (send_mail_spec(s, s->xmail[s->xmail_idx]))
			fprintf(stderr, "extra mail %u (%s) failed\n",
				s->xmail_idx, s->xmail[s->xmail_idx]);
		s->xmail_idx++;
		if (s->xmail_idx == s->n_xmail)
			printf("all %u extra mails sent\n", s->n_xmail);
	}
	/*
	 * latemail=MS:...: raw mails sent MS ms after the stored subscriptions
	 * were reloaded (-M sends before the radio is on), in list order.
	 */
	if (s->sess.state == DECT_SESS_UP && s->audio.n_latemail &&
	    s->xmail_idx >= s->n_xmail && (s->sub_restored || !any_uak(s))) {
		if (!s->late_t0)
			s->late_t0 = now ? now : 1u;
		while (s->late_idx < s->audio.n_latemail &&
		       now - s->late_t0 >= s->audio.latemail[s->late_idx].ms) {
			const char *ls = s->audio.latemail[s->late_idx].spec;

			if (send_mail_spec(s, ls))
				fprintf(stderr, "latemail %u (%s) failed\n",
					s->late_idx, ls);
			else
				printf("late: mail %u sent %llu ms after the "
				       "subscriptions (%s)\n", s->late_idx,
				       (unsigned long long)(now - s->late_t0),
				       ls);
			fflush(stdout);
			s->late_idx++;
		}
	}
	/*
	 * Reload the stored subscriptions into the chip once the radio is
	 * up, the way AVM restores saved handsets at boot [CR A-0026], so
	 * the handsets stay known across daemon restarts. After the extra
	 * mails, so a -M sequence runs before the chip knows any handset.
	 */
	if (s->sess.state == DECT_SESS_UP && any_uak(s) &&
	    !s->sub_restored && s->xmail_idx >= s->n_xmail) {
		send_subscription(s);
		s->sub_restored = 1;
		printf("sub: stored subscription reloaded into the chip\n");
	}
	/*
	 * Side links keep their own timers and queues whether or not
	 * the main link is up: an internal call's caller can be on a
	 * side link after the main connection went (review, finding 4).
	 */
	if (s->do_register) {
		unsigned k;

		for (k = 0; k < SIDE_MAX; k++) {
			if (!s->side[k].bl->active)
				continue;
			dect_link_tick(&s->side[k].bl->link, now);
			lapc_failed_note(s, &s->side[k].bl->link, "side");
			cap_tick(s, &s->side[k].bl->link);
			side_cipher_tick(s, &s->side[k], now);
			side_pump(s, &s->side[k]);
		}
	}
	/* Drive LAPC retransmission timers for the registration link. */
	if (s->do_register && s->base.main_link.active) {
		/*
		 * dect_link_tick() and not the two timers it contains.
		 * This loop used to call dect_lapc_tick() and
		 * dect_cc_tick() directly, which open-codes two of the
		 * four duties dect_link_tick() has and silently drops
		 * the other two: dect_mm_tick() (<MM_ident.1>) and,
		 * far worse, drain_nwk().
		 *
		 * Without the drain, a NWK message queued outside an
		 * inbound-segment context never reached LAPC until the
		 * handset happened to send us something. An
		 * FP-originated {CC-SETUP} is exactly such a message:
		 * ring_queue_setup() runs after dect_link_rx_segment()
		 * has already drained, so the SETUP sat in the CC
		 * output queue until the next inbound frame.
		 *
		 * Measured in vBR01: queued at t=21464 ms, on the wire
		 * at t=25324 ms. 3 860 ms late -- by which time the
		 * A540 had given up waiting and opened a CAT-iq
		 * service call of its own, so our stale SETUP landed
		 * as a {CC-SETUP} collision and the handset released
		 * it with "insufficient resources" exactly as
		 * EN 300 175-5 §9.5.2.3 requires. See
		 * docs/spec-conformance.md.
		 */
		dect_link_tick(&s->base.main_link.link, now);
		lapc_failed_note(s, &s->base.main_link.link, "main");
		cap_tick(s, &s->base.main_link.link);
		names_tick(s);		/* a handset renamed one */
		/*
		 * The call has gone away: release the audio channel.
		 * Without this it stays armed for the life of the link
		 * and the next call starts on top of the last one's
		 * state.
		 */
		if (s->base.main_link.bound && !dect_cc_busy(&s->base.main_link.link.cc)) {
			s->base.main_link.bound = 0;
			send_audio_disconnect(s, s->base.main_link.handle);
			s->reg.call_wideband = 0;
		}
		if (!dect_cc_busy(&s->base.main_link.link.cc))
			s->reg.nonspeech_noted = 0;
		pump_lapc(s);
	}
}

static int run_loop(struct stubd *s, unsigned int fpb, unsigned int run_seconds)
{
	size_t block = s->slots * 2u * fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *buf = malloc(slot * 16u);
	uint8_t *txbuf = malloc(block);
	uint64_t start = mono_ms();

	s->tx_tmpl = malloc(2u * s->slots * 2u);
	if (!buf || !txbuf || !s->tx_tmpl) {
		free(buf);
		free(txbuf);
		free(s->tx_tmpl);
		s->tx_tmpl = NULL;
		return -1;
	}
	tx_templates(s, s->tx_tmpl);
	uint64_t park_at = 0, drained_at = 0;

	s->tx_ring = 1;		/* from here on, mails are paced by tx_block() */
	g_start = start;
	s->base.start_ms = start;
	ctl_open(s);
	if (s->cell_mode)
		cell_open(s);
	for (;;) {
		struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
		uint64_t now = mono_ms();
		int pr;

		/*
		 * End of the run, by -t or by a signal: park the radio from
		 * INSIDE the loop. Once tx_ring is set, send_mail() only
		 * appends to txq and only tx_block() -- driven by the blocks
		 * this loop reads -- moves those bytes onto the wire. The park
		 * used to be queued after the loop had returned, so it was
		 * never sent, while the log still said "radio parked" and the
		 * chip went on beaconing with registration open (AGENTS.md §6).
		 * Found by the external review, the clean-room record
		 * an external review-24102026.md, finding 2.
		 *
		 * So: queue close-registration and radio-off, stop every
		 * periodic sender, keep pumping until the queue is empty plus
		 * PARK_FLUSH_MS for the driver to clock the last block out,
		 * and give up loudly after PARK_DRAIN_MS.
		 */
		if (!park_at && (stop || (run_seconds &&
		    now - start >= (uint64_t)run_seconds * 1000u))) {
			if ((!s->do_register && !s->cell_mode) ||
			    s->tx_slot < 0)
				break;
			send_mail_spec(s, "0x22:0x02:0x022c:01"); /* close reg */
			send_mail_spec(s, "0x22:0x02:0x015b:00"); /* radio off */
			park_at = now;
			printf("reg: parking the radio, %zu control bytes "
			       "queued\n", s->txq_len);
		}
		if (park_at) {
			if (s->txq_len)
				drained_at = 0;
			else if (!drained_at)
				drained_at = now;
			if (drained_at && now - drained_at >= PARK_FLUSH_MS &&
			    !s->parked) {
				s->parked = 1;
				printf("reg: radio parked -- park mails on the "
				       "wire %llu ms after queueing\n",
				       (unsigned long long)(drained_at -
							    park_at));
				if (!park_check_start(s, now)) {
					park_record("sent");
					break;
				}
			}
			if (s->parked) {
				if (park_check_done(s, now))
					break;
			} else if (now - park_at >= PARK_DRAIN_MS) {
				printf("reg: RADIO NOT PARKED -- %zu control "
				       "bytes still queued after %u ms\n",
				       s->txq_len, PARK_DRAIN_MS);
				break;
			}
		}

		pr = poll(&pfd, 1, POLL_MS);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr > 0) {
			ssize_t n = read(s->fd, buf, slot * 16u);
			size_t off;

			if (n < 0 && errno != EINTR && errno != EAGAIN) {
				perror("read");
				break;
			}
			for (off = 0; n > 0 && off + slot <= (size_t)n;
			     off += slot) {
				const struct dect_pcm_block *h =
					(const void *)(buf + off);
				uint8_t *fr = buf + off + sizeof(*h);
				struct dect_mail m;
				unsigned int f;

				if (!pcm_block_ok(s, h))
					continue;
				ssc_watch(s, fr, block, now);
				ssc_rx_fix(s, fr, block);
				if (h->flags & DECT_PCM_BF_DROPPED)
					fprintf(stderr, "warning: dropped blocks before seq %u\n",
						h->seq);
				for (f = 0; f < fpb; f++) {
					uint8_t pair[2];

					memcpy(pair, fr + (f * s->slots +
						(unsigned int)s->rx_slot) * 2u, 2);
					dect_rx_push(&s->rx, pair, 2);
				}
				voice_slot_probe(s, fr, fpb);
				voice_capture(s, fr, fpb);
				tx_block(s, fpb, txbuf, fr);
				now = mono_ms();
				while (dect_rx_pull(&s->rx, &m)) {
					if (s->verbose)
						log_mail(&m, "rx  ", s->show_params);
					dect_session_rx(&s->sess, &m, now);
					rfpi_check(s, &m);
					if (s->cell_mode && cell_rfpi_note(s, &m))
						cell_status(s, now);
					if (s->do_register)
						handle_reg_mail(s, &m, now);
					else if (s->cell_mode) {
						cell_mail_up(s, &m, now);
						/* our own park's read-back */
						if (m.prim == 0xFFF1u &&
						    s->park_check_at)
							mem_reply(s, &m);
					}
				}
			}
		}

		/* While parking, nothing may queue behind the park mails. */
		if (park_at) {
			led_write(s, 0);
			continue;
		}

		loop_tick(s, mono_ms());
	}
	ctl_close(s);
	if (s->cell_mode)
		cell_close(s);
	if (s->ssc && s->ssc_w_off != -2) {
		char is[200];

		lanes_str(is, sizeof(is), s->ssc_w_mask);
		printf("ssc: watch summary: idle lanes at bit %d, positions %s; "
		       "driver frames %llu overruns %u dropped %llu "
		       "tx_underruns %u\n", s->ssc_w_off, is,
		       (unsigned long long)s->ssc_w_st.frames,
		       s->ssc_w_st.overruns,
		       (unsigned long long)s->ssc_w_st.dropped,
		       s->ssc_w_st.tx_underruns);
	}
	printf("session: state %d contacts %u answers %u status %u deaths %u\n",
	       s->sess.state, s->sess.stats.contacts_sent, s->sess.stats.answers,
	       s->sess.stats.status_mails, s->sess.stats.deaths);
	printf("tx: blocks %u queue_drops %u write_short %u idle %02x "
	       "voice_idle %02x\n",
	       s->tx_blocks, s->txq_drops, s->tx_write_short, s->tx_idle,
	       s->tx_voice_idle);
	printf("cc: setups_in %u alerting %u connected %u released %u "
	       "collisions %u represented %u setup_timeouts %u closed_unknown %u\n",
	       s->base.main_link.link.cc.stats.setup, s->base.main_link.link.cc.stats.alerting,
	       s->base.main_link.link.cc.stats.connected, s->base.main_link.link.cc.stats.released,
	       s->base.main_link.link.cc.stats.collisions, s->base.main_link.link.cc.stats.represented,
	       s->base.main_link.link.cc.stats.setup_timeouts,
	       s->base.main_link.link.cc.stats.closed_unknown);
	printf("rx: mails %u seq_gaps %u csum_errors %u framing_errors %u resyncs %u\n",
	       s->rx.stats.mails, s->rx.stats.seq_gaps, s->rx.stats.csum_errors,
	       s->rx.stats.framing_errors, s->rx.stats.resyncs);
	/*
	 * Transport bookkeeping from the driver. `dropped` counts receive
	 * blocks lost because read() was too slow, which would show up in a
	 * capture as words held over -- the audio stalls we are chasing. If it
	 * is zero while the capture still stalls, the gaps are the chip's.
	 */
	if (s->ssc) {
		struct ssc_pcm_stats st;

		memset(&st, 0, sizeof(st));
		if (!ioctl(s->fd, SSC_PCM_IOC_STATS, &st))
			printf("pcm: blocks %llu dropped %llu overruns %u "
			       "tx_frames %llu tx_idle %llu tx_underruns %u\n",
			       (unsigned long long)st.blocks,
			       (unsigned long long)st.dropped, st.overruns,
			       (unsigned long long)st.tx_frames,
			       (unsigned long long)st.tx_idle, st.tx_underruns);
	} else {
		struct dect_pcm_stats ps;

		if (!ioctl(s->fd, DECT_IOC_PCM_STATS, &ps))
			printf("pcm: blocks %llu dropped %llu multi %llu empty %llu "
			       "errors %llu overruns %u tx_blocks %u tx_frames %u\n",
			       (unsigned long long)ps.blocks,
			       (unsigned long long)ps.dropped,
			       (unsigned long long)ps.multi,
			       (unsigned long long)ps.empty,
			       (unsigned long long)ps.errors,
			       ps.overruns, ps.tx_blocks, ps.tx_frames);
	}
	verify_report(s);
	free(buf);
	free(txbuf);
	free(s->tx_tmpl);
	s->tx_tmpl = NULL;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "--version")) {
		printf("dect-stubd %s\n", OPENDECTCELL_VERSION);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "--ctl"))
		return ctl_client(argc - 2, argv + 2);
	if (argc >= 2 && !strcmp(argv[1], "--profile-keys")) {
		static char js[8192];

		if (!dect_profile_keys_json(js, sizeof(js)))
			return 1;
		puts(js);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "--bridge-echo"))
		return bridge_echo(argc - 2, argv + 2);
	if (argc >= 3 && !strcmp(argv[1], "--check-profile"))
		return check_profile(argv[2], argc >= 4 ? argv[3] : NULL);
	if (getenv("OPENDECTCELL_SWAP"))
		dect_mail_set_swap(1);
	struct stubd s;
	struct dect_pcm_config cfg;
	struct dect_sess_cfg scfg = {
		.src_task = DECT_TASK_PCM,
		.dst_task = DECT_TASK_CONTACT_DST,
	};
	const char *dev = "/dev/dect0";
	unsigned int run_seconds = 0, fpb;
	struct dect_mail spec_check;	/* -M validated at startup */
	int c, ret = 1, sub_off = 0, auto_bringup = 0, passes;

	memset(&s, 0, sizeof(s));
	s.base.ops = &base_ops;
	s.base.ctx = &s;
	{
		unsigned k;

		for (k = 0; k < SIDE_MAX; k++)
			s.side[k].bl = &s.base.side[k];
	}
	s.tx_slot = s.rx_slot = -1;
	s.rand_fd = -1;
	memset(&cfg, 0, sizeof(cfg));
	s.sub_path = "/etc/opendectcell-sub";	/* overridden by -s */
	s.voice_slot = -1;		/* capture every slot unless -V says one */
	s.audio.sel = 1;		/* simple single-channel case [CR A-0029] */
	/*
	 * 0x2B is the fresh-default byte AVM's own 0x0251 producer reads from
	 * EEPROM 0x2A + channel index (all six records initialised to 0x2B)
	 * [CR A-0036]. Its DSP semantics are still unproven; it is simply the
	 * one value with provenance, where 1 was a placeholder.
	 */
	s.audio.gain = 0x2Bu;
	s.audio.flags = 1;		/* channel 1 */
	s.rx_word_prev = 0xFFFFFFFFu;	/* no word seen yet */
	s.audio.memdump_ms = 50u;	/* default chip-memory poll interval */
	s.audio.basic_svc = RING_BASIC_SVC;
	s.audio.dial_wait_ms = 3000u;
	s.audio.reg_signal_s = 120u;
	s.audio.lane_ch[0] = 2; s.audio.lane_word[0] = 3;	/* Q-0063 */
	s.audio.lane_ch[1] = 3; s.audio.lane_word[1] = 2;
	s.audio.slot_mask = 0x00u;	/* computed unless slotmask= pins it */
	ab_init(&s);
	/*
	 * The mode-2 re-assign (remode_tick()), sent only when -a flags= set
	 * bit 6 in the first 0x0230 (see the resolution after option parsing):
	 * without it such a bind stays in mode 9 and no downlink audio reaches
	 * the handset. With bit 6 clear in flags -- the default, 1 -- none is
	 * sent: a narrowband bind reaches mode 2 by itself once 0x022D turns
	 * the path on (A-0046/A-0047), a wideband call gets stock's bind,
	 * which needs none, and a re-assign would only add a 500 ms mode-9
	 * window of noise.
	 *
	 * 500 ms and not less: the chip's MAC-side bearer handler runs its own
	 * configurator pass after 0x022D, and a re-assign that arrives before
	 * it is overwritten (at 150 ms the enable word never reached 1).
	 */
	s.audio.remode_ms = 500;
	/*
	 * <<SIGNAL>> 0x41 (external-call alerting) in every FP-originated
	 * SETUP by default. EN 300 444 §8.14 requires the FT to alert; without
	 * it the A540 shows the call and stays silent (run 18). -a nosignal
	 * omits it, -a signal=N picks another pattern.
	 */
	s.audio.signal_on = 1;
	s.audio.signal_value = DECT_SIGNAL_ALERT_PATTERN1;
	s.audio.nlr = 1;		/* bench 2026-10-28, vNLR: harmless */
	s.audio.locauth = 1;	/* bench 2026-10-28, test 19: all three answer */
	s.ssc_w_off = -2;	/* ssc_watch(): nothing measured yet */
	s.audio.auth_first = 1;	/* security audit 2026-10-29, A1 */
	s.tx_idle = 0xFFu;
	s.tx_voice_idle = 0xFFu;
	s.pin = "0000";		/* only for code paths that never register: -g needs -P (below) */
	int pin_given = 0;
	while ((c = getopt(argc, argv, "d:c:T:R:t:vpM:gKb:P:s:k:w:V:Ca:h")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'c': cfg.slots = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'T': s.tx_slot = (int)strtol(optarg, NULL, 0); break;
		case 'R': s.rx_slot = (int)strtol(optarg, NULL, 0); break;
		case 't': run_seconds = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'v': s.verbose = 1; break;
		case 'p': s.verbose = 1; s.show_params = 1; break;
		case 'M':
			if (s.n_xmail >= sizeof(s.xmail) / sizeof(s.xmail[0])) {
				usage();
				return 2;
			}
			if (parse_mail_spec(optarg, &spec_check)) {
				fprintf(stderr, "bad -M mail spec: %s "
					"(SRC:DST:PRIM[:HEX], hex in pairs)\n",
					optarg);
				return 2;
			}
			s.xmail[s.n_xmail++] = optarg;
			break;
		case 'g': s.do_register = 1; break;
		case 'K': s.cell_mode = 1; break;
		case 'b': s.contacts_path = optarg; break;
		case 'P':
			/*
			 * @FILE: the PIN from a root-only file, so it is not on
			 * the command line for every local user to read in
			 * /proc (security audit 2026-10-29, F2).
			 */
			if (optarg[0] == '@') {
				static char pinbuf[DECT_PIN_DIGITS_MAX + 2];
				FILE *pf = fopen_root_only(optarg + 1);

				if (!pf || !fgets(pinbuf, sizeof(pinbuf), pf)) {
					fprintf(stderr, "-P: cannot read %s\n",
						optarg + 1);
					if (pf)
						fclose(pf);
					return 2;
				}
				fclose(pf);
				pinbuf[strcspn(pinbuf, "\r\n")] = '\0';
				optarg = pinbuf;
			}
			/* GAP: up to 8 decimal digits (a 32-bit AC) */
			if (dect_dsaa_pin_check(optarg, DECT_PIN_DIGITS_MIN)) {
				fprintf(stderr, "-P wants %u to %u decimal "
					"digits\n", DECT_PIN_DIGITS_MIN,
					DECT_PIN_DIGITS_MAX);
				return 2;
			}
			s.pin = optarg;
			pin_given = 1;
			break;
		case 's':
			if (!optarg[0]) {
				sub_off = 1;
				break;
			}
			if (s.base.n_hs >= DECT_MM_MAX_SLOT) {
				fprintf(stderr, "-s: at most %u stores\n",
					DECT_MM_MAX_SLOT);
				return 2;
			}
			s.hs[s.base.n_hs++].path = optarg;
			s.sub_path = s.hs[0].path;
			break;
		case 'C': s.no_cipher = 1; break;
		case 'a':
			/*
			 * Say which option string was rejected before printing
			 * 60 lines of usage. A bad -a used to look exactly
			 * like a missing argument, and finding that out cost a
			 * run -- it would have cost an attended one.
			 */
			if (parse_audio(&s.audio, optarg)) {
				fprintf(stderr, "bad -a option string: %s\n",
					optarg);
				usage();
				return 2;
			}
			break;
		case 'w': s.voice_path = optarg; break;
		case 'V': s.voice_slot = (int)strtol(optarg, NULL, 0); break;
		case 'k':
			if (parse_chip_cipher(&s.chip_cipher, optarg) < 0) {
				usage();
				return 2;
			}
			break;
		default: usage(); return 2;
		}
	}

	/* -g auto-sends the MAC bring-up (radio on, open registration) unless the
	 * caller supplied its own -M mails. [CR COMMAND_SET §6.1/§6.2] */
	if (s.do_register && !pin_given) {
		fprintf(stderr, "-g needs -P PIN (or -P @FILE): there is no default registration PIN\n");
		return 2;
	}
	if (s.do_register && s.n_xmail == 0) {
		auto_bringup = 1;
		/*
		 * Stock's MAC bring-up, in stock's order, from the 2026-10-24
		 * capture of FRITZ!OS 175.07.56 on this hardware.
		 *
		 * Stock's whole bring-up is three config mails and then the
		 * radio. We sent only the radio, and `0x0153`, `0x0157` and
		 * `0x0169` appeared nowhere in this source tree. Every
		 * FP-originated call to a Gigaset was refused, on three
		 * handsets, with nineteen per-call variables making no
		 * difference -- consistent with a gap in one-time init rather
		 * than anything in a call.
		 *
		 * `[CR A-0109]`: 0x96ad is built at BRING-UP. It is unchanged
		 * by paging (A-0106) and unchanged by registering or
		 * deregistering a handset, so our all-zero page table is a
		 * bring-up gap, not a registration gap, and the fix belongs
		 * here.
		 *
		 * The order matters: the radio-on path latches state, so
		 * configuring afterwards may not re-beacon. Stock sends these
		 * from src 0x14, the FP-management task, and the dispatcher
		 * may only accept config from it -- so we match stock exactly
		 * rather than reusing our own 0x22.
		 *
		 * What these mails DO is not established. An earlier decode
		 * said they configure the beacon capability block; that was
		 * read against 08.25 firmware while this box runs 07.56, and
		 * it is withdrawn -- our capability templates are identical to
		 * stock's, QH=7 included. So this is a faithful replay of
		 * stock's sequence, not an implementation of an understood
		 * mechanism. Judge it by whether 0x96ad populates and whether
		 * the handset rings.
		 */
		/* src/dst/prim parse with base 0, so hex needs the 0x prefix. */
		s.xmail[s.n_xmail++] = "0x14:0x02:0x0153:00";	/* ECO_CONFIG */
		s.xmail[s.n_xmail++] = "0x14:0x02:0x0169:07";	/* FEATURE_FLAGS */
		s.xmail[s.n_xmail++] = "0x14:0x02:0x0157:00";	/* CAPABILITY_PROFILE */
		s.xmail[s.n_xmail++] = "0x22:0x02:0x015b:01";	/* radio/beacon on */
		/*
		 * Registration open ('A') only when asked (-a regopen): nothing
		 * enrols unless we mean it to. Closed is 0x022C 01.
		 */
		if (s.audio.reg_open)
			s.xmail[s.n_xmail++] = "0x22:0x02:0x022b:41";	/* 'A': open reg */
		else
			s.xmail[s.n_xmail++] = "0x22:0x02:0x022c:01";	/* closed */
	}
	if (optind != argc || (s.cell_mode && s.do_register)) {
		usage();
		return 2;
	}
	if (sub_off) {			/* -s "" */
		s.sub_path = NULL;
		s.base.n_hs = 0;
	} else if (!s.base.n_hs && s.sub_path) {
		s.hs[0].path = s.sub_path;	/* the default store */
		s.base.n_hs = 1;
	}
	s.base.chip.att_flag = s.audio.att_flag;	/* the chip's slots (S2.2) */
	s.base.chip.att_cfg = s.audio.att_cfg;
	s.base.chip.no_mask = s.audio.no_slot_cfg;
	s.base.slot0 = s.audio.sub_slot;
	contacts_init(&s);
	if (!s.audio.remode_set && !(s.audio.flags & 0x40u))
		s.audio.remode_ms = 0;	/* mode 2 from the first assign */
	/* the window the start-up mail opens (0x022B), if regopen asked */
	s.base.reg_open = s.audio.reg_open;
	s.base.reg_open_until = s.base.reg_open && s.audio.reg_window_s ?
			   mono_ms() + s.audio.reg_window_s * 1000ull : 0;
	if (!s.audio.basic_svc_set)
		s.audio.basic_svc = s.audio.narrowband ? RING_BASIC_SVC
						       : RING_BASIC_SVC_WB;
	/* how calls run: the library's copy (dect_base.h, S3i) */
	s.base.cfg.narrowband = s.audio.narrowband;
	s.base.cfg.two_calls = s.audio.two_calls;
	s.base.cfg.auth_first = s.audio.auth_first;
	s.base.cfg.no_cipher = s.no_cipher;
	s.base.cfg.no_chip_key = s.chip_cipher.disabled;
	s.base.cfg.verbose = s.verbose;
	s.base.cfg.log_raw = s.show_params;
	s.base.cfg.locauth = s.audio.locauth;
	s.base.cfg.no_overlap = s.audio.no_overlap;
	s.base.cfg.dial_wait_ms = s.audio.dial_wait_ms;
	s.base.cfg.alert_ms = s.audio.alert_ms;
	s.base.cfg.bind_first = s.audio.bind_first;
	s.base.cfg.no_call_status = s.audio.no_call_status;
	s.base.cfg.all_call_status = s.audio.all_call_status;
	s.base.cfg.nlr = s.audio.nlr;
	s.base.cfg.setup_tries = s.audio.setup_tries;
	s.base.cfg.signal_on = s.audio.signal_on;
	s.base.cfg.signal_value = s.audio.signal_value;
	s.base.cfg.signal_set = s.audio.signal_set;
	s.base.cfg.basic_svc = s.audio.basic_svc;
	s.base.cfg.basic_svc_set = s.audio.basic_svc_set;
	s.base.cfg.ring_ms = s.audio.ring_ms;
	s.base.cfg.ring_attach_ms = s.audio.ring_attach_ms;
	s.base.cfg.pin = s.pin;
	s.base.cfg.line_name = s.audio.line_name;
	memcpy(s.base.cfg.codeclist, s.audio.codeclist, s.audio.codeclist_len);
	s.base.cfg.codeclist_len = s.audio.codeclist_len;
	s.base.cfg.locate_validity = s.audio.locate_validity;
	s.base.cfg.tpui_offset = s.audio.tpui_offset;
	s.base.cfg.defer_unknown = s.audio.remote_path != NULL;
	s.base.cfg.ipuio_set = s.audio.ipuio_set;
	s.base.cfg.ipuio = s.audio.ipuio;
	s.base.cfg.time_every_min = s.audio.time_every_min;
	s.base.cfg.callinfo = s.audio.callinfo;
	s.base.contacts = s.contacts_path ? &stubd_contacts : NULL;
	s.base.call_lists = &stubd_calls;
	memcpy(s.base.cfg.info_ie, s.audio.info_ie, s.audio.info_ie_len);
	s.base.cfg.info_ie_len = s.audio.info_ie_len;

	s.stub_build = stub_build_read(s.stub_name, sizeof(s.stub_name));
	printf("chip: STUB second stage %s\n",
	       s.stub_build == STUB_UNKNOWN ? "unknown (no " DECT_STUB_RECORD
					      ")"
					    : s.stub_name);
	/*
	 * The 7412's 06.88 STUB reacts to the bring-up only with its radio
	 * switched off first. From boot its 0xF602 byte 2 reads 01, as after
	 * a radio-on; 0x015B 00 makes it 02. Sent in the 7520's order, the
	 * 0xF623 scan report stayed flat (every carrier 0x1B) for 26, 30 and
	 * 120 s after radio-on; with radio-off before the configuration --
	 * or after a park in an earlier run -- it showed levels up to 0x3B
	 * within 1-2 s, five runs of five. Radio-off after the configuration
	 * did not help (runs X2 and Z1, observed on a 7412, 2026-10-29).
	 * Whether the levels are our beacon or a neighbour the scan only sees
	 * while it runs is open; a handset decides (docs/journals/bench-7412.md).
	 */
	if (!strcmp(s.stub_name, DECT_STUB_0688)) {
		sub_uak_base = SUB_UAK_BASE_0688;
		s.uak_0688 = 1;
	}
	/*
	 * The library's read-back and slot audit (S2.2b): the table where
	 * this build keeps it; for a build nobody named, the 08.25 place as
	 * a guess the log owns up to; none for another build.
	 */
	if (s.stub_build != STUB_OTHER || s.uak_0688)
		s.base.chip.uak_table = sub_uak_base;
	s.base.chip.uak_guess = s.stub_build == STUB_UNKNOWN;
	s.base.chip.check_delay_ms = SUB_CHECK_DELAY_0688_MS;
	if (s.audio.keycheck_set)	/* key-write delay sweep */
		s.base.chip.check_delay_ms = s.audio.keycheck_ms;
	s.base.chip.stub = s.stub_build == STUB_UNKNOWN ? NULL : s.stub_name;
	if (auto_bringup && !strcmp(s.stub_name, DECT_STUB_0688)) {
		memmove(&s.xmail[1], &s.xmail[0],
			s.n_xmail * sizeof(s.xmail[0]));
		s.xmail[0] = "0x22:0x02:0x015b:00";	/* radio off first */
		s.n_xmail++;
		printf("chip: 06.88 bring-up: radio off before the "
		       "configuration\n");
	}
	if (s.do_register && s.base.n_hs) {
		unsigned i, j;

		/* restores the UAKs; chip reload once up */
		for (i = 0; i < s.base.n_hs; i++) {
			struct stat sst;

			s.base.cur = i;
			hs_load(&s, i);		/* path set, the rest empty */
			s.base.loaded[i] = sub_load(&s);
			dect_base_stash(&s.base);
			/*
			 * Only a store that does not exist yet may take an
			 * enrolment. One that exists but did not load (wrong
			 * size or magic, a slot clash) holds someone's record;
			 * routing a new handset there would overwrite it
			 * (review, finding 9).
			 */
			s.base.absent[i] = s.hs[i].path &&
				stat(s.hs[i].path, &sst) != 0 && errno == ENOENT;
			for (j = 0; s.base.loaded[i] && j < i; j++) {
				if (!s.base.loaded[j] ||
				    s.base.hs[j].slot != s.base.hs[i].slot)
					continue;
				printf("sub: %s claims slot %u, which %s "
				       "holds -- not loaded\n", s.hs[i].path,
				       s.base.hs[i].slot, s.hs[j].path);
				s.base.loaded[i] = 0;
			}
		}
		/* remote=: one more store, marked as a roamer (test M4) */
		if (s.audio.remote_path && s.base.n_hs < DECT_MM_MAX_SLOT) {
			i = s.base.n_hs++;
			s.hs[i].path = s.audio.remote_path;
			s.base.cur = i;
			hs_load(&s, i);
			s.base.loaded[i] = sub_load(&s);
			dect_base_stash(&s.base);
			s.base.remote[i] = 1;
			printf("sub: %s is a roamer: not held, not in the chip, "
			       "fetched on demand after %u ms\n",
			       s.audio.remote_path, s.audio.fetch_delay_ms);
		}
		for (i = 0; i < s.base.n_hs; i++) {
			name_load(&s, i);
			cap_load(&s, i);
		}
		profiles_resolve_all(&s, "startup");
		/*
		 * A first store that does not exist yet (an enrolment) has no
		 * slot; it used to default to 1 and silently share it with
		 * whichever store held 1 (review, 2026-10-25). Give it the
		 * lowest free one, unless subslot= chose.
		 */
		if (!s.base.loaded[0] && !s.audio.sub_slot) {
			unsigned used = 0, slot;

			for (i = 1; i < s.base.n_hs; i++)
				if (s.base.loaded[i])
					used |= 1u << s.base.hs[i].slot;
			for (slot = 1; slot <= DECT_MM_MAX_SLOT &&
			     (used & (1u << slot)); slot++)
				;
			if (slot > DECT_MM_MAX_SLOT) {
				fprintf(stderr, "sub: every chip slot is taken "
					"by the other stores\n");
				return 2;
			}
			s.base.hs[0].slot = slot;
			printf("sub: %s is new; it gets chip slot %u\n",
			       s.hs[0].path, slot);
		}
		/*
		 * Every other store that does not exist yet is a place a new
		 * handset can enrol (enrol_route()): it needs a slot of its
		 * own as well, distinct from the loaded stores and from each
		 * other.
		 */
		for (i = 1; i < s.base.n_hs; i++) {
			unsigned used = 0, slot, k;

			if (s.base.loaded[i] || s.base.remote[i] ||
			    !s.base.absent[i])
				continue;
			for (k = 0; k < s.base.n_hs; k++) {
				unsigned ks = k == 0 && s.audio.sub_slot ?
					      s.audio.sub_slot : s.base.hs[k].slot;

				if (k != i && (s.base.loaded[k] ||
				    (k < i && !s.base.remote[k])))
					used |= 1u << ks;
			}
			for (slot = 1; slot <= DECT_MM_MAX_SLOT &&
			     (used & (1u << slot)); slot++)
				;
			if (slot > DECT_MM_MAX_SLOT) {
				printf("sub: no chip slot left for %s; it cannot "
				       "take an enrolment\n", s.hs[i].path);
				s.base.remote[i] = 1;	/* never a free store */
				continue;
			}
			s.base.hs[i].slot = slot;
			printf("sub: %s is free for an enrolment, chip slot %u\n",
			       s.hs[i].path, slot);
		}
		if (!s.base.loaded[0] && s.audio.sub_slot) {
			for (i = 1; i < s.base.n_hs; i++)
				if (s.base.loaded[i] &&
				    s.base.hs[i].slot == s.audio.sub_slot)
					printf("sub: *** subslot=%u is %s's slot "
					       "-- enrolling here overwrites it ***\n",
					       s.audio.sub_slot, s.hs[i].path);
		}
		s.base.cur = 0;
		hs_load(&s, 0);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	signal(SIGHUP, on_hup);
	dect_rx_init(&s.rx);
	dect_session_init(&s.sess, &scfg);

	dect_clip_init(&s.base.clip);
	if (s.audio.clip_number
	    && dect_clip_set_number(&s.base.clip, s.audio.clip_number,
				    DECT_CLIP_TON_UNKNOWN, DECT_CLIP_NPI_E164,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED)) {
		fprintf(stderr, "clip: number rejected (printable IA5, "
			"at most %u bytes)\n", DECT_CLIP_NUMBER_MAX);
		return 1;
	}
	if (s.audio.clip_name
	    && dect_clip_set_name(&s.base.clip, s.audio.clip_name,
				  DECT_CLIP_ALPHABET_UTF8,
				  DECT_CLIP_PRES_ALLOWED,
				  DECT_CLIP_SCR_USER_UNSCREENED)) {
		fprintf(stderr, "cnip: name rejected (well-formed UTF-8 "
			"without control characters, at most %u bytes)\n",
			DECT_CLIP_NAME_MAX);
		return 1;
	}
	if (!dect_callinfo_valid(&s.audio.callinfo)) {
		fprintf(stderr, "callinfo: rejected (linetype needs lineid; "
			"callstatus must be a defined call status)\n");
		return 1;
	}
	{
		const struct dect_callinfo *ci = &s.audio.callinfo;

		if (ci->has_line || ci->has_call_id || ci->has_call_status)
			printf("callinfo: CC-SETUP will carry "
			       "<<CALL-INFORMATION>>%s%s%s%s\n",
			       ci->has_line ? " line" : "",
			       ci->has_line_type ? " linetype" : "",
			       ci->has_call_id ? " callid" : "",
			       ci->has_call_status ? " callstatus" : "");
		if (ci->has_line && ci->line_id != STUBD_LINE_ID)
			printf("callinfo: WARNING: lineid=%u names a line the "
			       "line list does not have (it has line %u only)\n",
			       ci->line_id, STUBD_LINE_ID);
	}
	if (s.audio.clip_number || s.audio.clip_name)
		printf("clip: CC-SETUP will carry %s%s%s\n",
		       s.audio.clip_number ? "a number" : "",
		       (s.audio.clip_number && s.audio.clip_name) ? " and " : "",
		       s.audio.clip_name ? "a name" : "");
	/* its files' directory; the service makes it too (opendectcell.init) */
	if (mkdir(DECT_RUN_DIR, 0700) && errno != EEXIST)
		perror(DECT_RUN_DIR);
	(void)unlink(PARK_RECORD);	/* this run's park, not the last one's */
#ifdef STUBD_TEST
	return stubd_test_run(&s);	/* the chip is a script from here on */
#endif
	s.fd = open(dev, O_RDWR);
	if (s.fd < 0) {
		perror(dev);
		return 1;
	}
	if (bring_up(&s, &cfg))
		goto out;
	fpb = cfg.frames_per_block;

	if (s.tx_slot < 0 || s.rx_slot < 0) {
		/*
		 * Straight after a firmware upload the chip may not answer
		 * yet: the service starts us the moment dect-fwload returns,
		 * and the first search came back empty (2026-10-25, service
		 * test; the same transient as run M1 that morning). Three
		 * passes, ~4 s each, before giving up. The 7412's chip has
		 * answered only 30 to 60 s after its upload (2026-10-29), and
		 * as late as 66 s after a start without one, first after a
		 * package install (T13 U6: ten passes, 68 s, were not enough;
		 * U6b found it at pass 9). The lane offset stayed put
		 * meanwhile, so the SSC transport makes twenty passes, ~7 s
		 * each, at the offset it measured and the one 8 bits on.
		 */
		int pass;

		passes = s.ssc ? 20 : 3;
		for (pass = 1; find_control_slot(&s, fpb); pass++) {
			if (pass >= passes || stop) {
				fprintf(stderr, "control slot not found\n");
				goto out;
			}
			fprintf(stderr, "control slot not found yet (pass %d of "
				"%d) -- the chip may still be starting\n", pass,
				passes);
			if (s.ssc)
				ssc_flip(&s);
			sleep(1);
		}
	} else {
		printf("control slot given: tx %d, rx %d\n", s.tx_slot, s.rx_slot);
	}
	if (s.tx_slot >= (int)s.slots || s.rx_slot >= (int)s.slots) {
		fprintf(stderr, "slot out of range\n");
		goto out;
	}
	if (s.ssc && s.rx_slot != s.tx_slot) {
		/*
		 * The SSC has no frame sync: the control lane lands at one
		 * index in what we receive and at another in what we send.
		 * Delay the receive side by the difference, so both share
		 * one numbering as on the 7520 and a lane index means the
		 * same lane both ways [CR A-0015: a call uses the same lane
		 * in each direction].
		 */
		unsigned int d = ((unsigned int)s.tx_slot + s.slots -
				  (unsigned int)s.rx_slot) % s.slots;

		ssc_lane_delay_init(&s.ssc_ld, d);
		dect_rx_init(&s.rx);	/* a mail cut by the switch */
		printf("ssc: receive delayed by %u lanes: control lane %d "
		       "both ways\n", d, s.tx_slot);
		s.rx_slot = s.tx_slot;
	}
	if (s.ssc && !s.audio.lane0_word_set) {
		/*
		 * The 7412's audio lanes follow the control lane, one per
		 * call, the first call's next to it [CR A-0015]; observed:
		 * that lane leaves the idle pattern when a call connects.
		 */
		s.audio.lane0_word = (uint8_t)(((unsigned int)s.tx_slot + 1u) %
					      s.slots);
		printf("ssc: speech lane %u (control lane + 1)\n",
		       (unsigned int)s.audio.lane0_word);
	}
	if (s.ssc) {
		/* call n's speech in control lane + n [CR A-0015] */
		unsigned int k;

		for (k = 0; k < XLANE_MAX; k++)
			if (!s.audio.lane_set[k])
				s.audio.lane_word[k] = (uint8_t)
					(((unsigned int)s.tx_slot +
					  s.audio.lane_ch[k]) % s.slots);
		printf("ssc: side lanes: channel %u -> lane %u, channel %u "
		       "-> lane %u\n", s.audio.lane_ch[0],
		       s.audio.lane_word[0], s.audio.lane_ch[1],
		       s.audio.lane_word[1]);
	}
	if (s.audio.play_path) {
		FILE *pf = fopen(s.audio.play_path, "rb");
		long n;

		if (!pf || fseek(pf, 0, SEEK_END) || (n = ftell(pf)) <= 0) {
			fprintf(stderr, "cannot read %s\n", s.audio.play_path);
			if (pf)
				fclose(pf);
			return 1;
		}
		rewind(pf);
		s.audio.play = malloc((size_t)n);
		if (!s.audio.play || fread(s.audio.play, 1, (size_t)n, pf)
		    != (size_t)n) {
			fprintf(stderr, "cannot read %s\n", s.audio.play_path);
			fclose(pf);
			return 1;
		}
		fclose(pf);
		s.audio.play_len = (size_t)n & ~(size_t)1;
		if (s.audio.play_pcm) {
			/*
			 * The file is 16 kHz s16le; encode it to G.722 in
			 * place. Two input bytes per sample, two samples per
			 * output octet, so the coded stream is a quarter of
			 * the size and the buffer always has room.
			 */
			struct dect_g722_state enc;
			size_t ns = s.audio.play_len / 2u, i, coded;
			int16_t *pcm = malloc(ns * sizeof(*pcm));

			if (!pcm) {
				fprintf(stderr, "out of memory for %s\n",
					s.audio.play_path);
				return 1;
			}
			for (i = 0; i < ns; i++)
				pcm[i] = (int16_t)((uint16_t)s.audio.play[2 * i]
					 | ((uint16_t)s.audio.play[2 * i + 1] << 8));
			dect_g722_init(&enc);
			coded = dect_g722_encode(&enc, pcm, ns, s.audio.play);
			free(pcm);
			s.audio.play_len = coded & ~(size_t)1;
			printf("play: %s, %zu samples of 16 kHz PCM encoded to "
			       "%zu bytes of G.722\n",
			       s.audio.play_path, ns, s.audio.play_len);
		}
		if (s.audio.play_raw)
			printf("play: %zu bytes = %.1f s of verbatim TDM lane "
			       "at 16000 B/s\n", s.audio.play_len,
			       (double)s.audio.play_len / 16000.0);
		else
			printf("play: %zu bytes = %.1f s of G.722 at 64 kbit/s\n",
			       s.audio.play_len,
			       (double)s.audio.play_len / 8000.0);
	}
	if (s.audio.have_tx_idle)
		s.tx_idle = s.audio.tx_idle;
	if (s.audio.have_voice_idle)
		s.tx_voice_idle = s.audio.tx_voice_idle;
	if (s.do_register)
		s.rand_fd = open("/dev/urandom", O_RDONLY);
	if (s.voice_path) {
		/*
		 * The capture writes slots * 2 bytes per 8 kHz frame -- 128 KB/s
		 * for the usual eight slots, so a four-minute run costs ~30 MB.
		 * On OpenWrt /tmp is a RAM disk, and filling it does not fail
		 * usefully: the capture's own writes return ENOSPC and are
		 * handled, but the log's do not, so the daemon goes silent
		 * mid-line while the radio keeps running. On 2026-10-22 that
		 * cost two bench runs and read exactly like a handset that
		 * would not synchronise. Refuse to start instead.
		 */
		uint64_t need = (uint64_t)s.slots * 2u * 8000u *
				(run_seconds ? (uint64_t)run_seconds : 240u);
		uint64_t have = fs_free_bytes(s.voice_path);

		if (have && have < need + CAPTURE_HEADROOM) {
			fprintf(stderr,
				"%s: %llu MiB free, but this run needs about "
				"%llu MiB of capture plus headroom.\n"
				"Free some space or drop -w; a full filesystem "
				"silently wedges the log.\n",
				s.voice_path,
				(unsigned long long)(have >> 20),
				(unsigned long long)(need >> 20));
			goto out;
		}
		s.voice_buf = malloc((size_t)s.slots * 2u * fpb);
		s.voice_fp = s.voice_buf ? fopen_private(s.voice_path) : NULL;
		if (!s.voice_fp) {
			perror(s.voice_path);
			goto out;
		}
	}
	led_open(&s);
	led_write(&s, 0);	/* a park or probe run leaves it dark */
	ret = run_loop(&s, fpb, run_seconds);
	calls_tick(mono_ms(), 1);
	state_write(&s, mono_ms(), 0, 1);	/* says "parked" */
	led_write(&s, 0);
	if (s.led_fd >= 0) {
		close(s.led_fd);
		if (s.do_register)
			printf("led: %u changes, %u blink ticks of %u ms\n",
			       s.led_changes, s.led_blink_ticks, LED_TICK_MS);
	}
	if (s.voice_fp) {
		fclose(s.voice_fp);
		/* two bytes a frame for one slot, a whole frame for all */
		if (s.voice_slot < 0)
			printf("voice: captured %u bytes from every slot "
			       "(%u frames)\n", s.voice_bytes,
			       s.voice_bytes / (s.slots * 2u));
		else
			printf("voice: captured %u bytes from slot %d "
			       "(%u frames)\n", s.voice_bytes, s.voice_slot,
			       s.voice_bytes / 2u);
	}

	/*
	 * run_loop() parks the radio itself, inside the loop, because only the
	 * loop drains the transmit queue [CR COMMAND_SET §6.2/§6.1]. Anything
	 * queued here would never be sent. If the loop could not park -- a
	 * transport error, or the drain timed out -- say so instead.
	 */
	if (s.do_register && s.tx_slot >= 0 && !s.parked)
		printf("reg: RADIO NOT PARKED -- park it by hand: dect-stubd "
		       "-M 0x22:0x02:0x022c:01 -M 0x22:0x02:0x015b:00 -t 8\n");
out:
	if (s.rand_fd >= 0)
		close(s.rand_fd);
	free(s.voice_buf);
	tear_down(&s);
	close(s.fd);
	return ret;
}

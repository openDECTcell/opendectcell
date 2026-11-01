// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd: keep the STUB control channel up on /dev/dect0.
 *
 * It brings up the PCM receive and transmit path step by step (the same
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include "opendectcell.h"
#include "ssc_pcm.h"
#include "ssc_align.h"
#include "dect_mail.h"
#include "dect_session.h"
#include "dect_proto.h"
#include "dect_cs.h"
#include "dect_lc.h"
#include "dect_lapc.h"
#include "dect_nwk.h"
#include "dect_mm.h"
#include "dect_dsaa.h"
#include "dect_cc.h"
#include "dect_link.h"
#include "dect_diag.h"
#include "dect_audio.h"
#include "dect_profile.h"
#include "dect_callinfo.h"
#include "dect_clip.h"
#include "dect_ctl.h"
#include "dect_cell.h"
#include "dect_base.h"
#include "dect_calls.h"
#include "dect_contacts.h"
#include "opendectcell_version.h"

/* Headroom left for the log and anything else sharing the filesystem. */
#define CAPTURE_HEADROOM	(16u * 1024u * 1024u)
/* Stop capturing rather than fill the filesystem completely. */
#define CAPTURE_FLOOR		(8u * 1024u * 1024u)

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


/*
 * Our one line, as the line list (TS 102 527-3 7.4.11) names it. A
 * <<CALL-INFORMATION>> line identifier (lineid=) must name the same line:
 * bench test 21c sent line 0 against this list's line 1 and the A540 rang
 * silently, as for a line it is not attached to.
 */
#define STUBD_LINE_ID	DECT_BASE_LINE_ID

/*
 * <<BASIC-SERVICE>>: high nibble call class, low nibble basic service, per
 * EN 300 175-5 §7.7.5 figure 18. The class table is NOT what we were told:
 *
 *   0010 LiA service   0011 ULE   0100 Message call   0111 DECT/ISDN IIP
 *   1000 NORMAL CALL   1001 Internal   1010 Emergency   1011 Service call
 *   1100 EXTERNAL HANDOVER   1101 Supplementary service   1110 OA&M
 *
 * and the service table: 0000 basic speech, 1000 wideband speech.
 *
 * So 0x80 is a normal narrowband call and 0x88 a normal wideband one.
 * `[CR A-0069]` read 0x88 as "call class 8 = external handover" and had us
 * send 0x40 instead -- which this table makes "**Message call setup** plus
 * basic speech". External handover is 1100, i.e. 0xC0, and class 8 was right
 * all along. Read out of the spec, 2026-10-23 night; the room is told.
 *
 * None of the three values changes the A540 refusal (0x88, 0x80 and 0x40 all
 * drew {CC-RELEASE-COM} 0x32), so this is a conformance fix, not a defect fix.
 */
#define RING_BASIC_SVC	0x80u		/* normal call setup, basic speech */
#define RING_BASIC_SVC_WB 0x88u		/* ... wideband speech, the default */

/*
 * Below this the box clock is certainly unset: the 7520 has no RTC, so a
 * freshly booted OpenWrt sits near the epoch until NTP lands. 2020-01-01.
 */
#define CLOCK_SANE_AFTER	1577836800L

/*
 * Is the system clock actually trustworthy?
 *
 * The epoch check above is not enough, and the bench box proves it: on
 * 2026-10-23 it read "Jun 30 2026" -- four months out, with ntpd running but
 * no route to a server, 34 hours into an uptime. That date passes any
 * plausible threshold while being completely wrong, and we would have pushed
 * it to every handset that registered.
 *
 * The kernel already tracks this. adjtimex(2) with modes = 0 is a read-only
 * query needing no privilege, and STA_UNSYNC means "no reference has
 * disciplined this clock". A handset with no time shows a blank or a default;
 * a handset we confidently told the wrong date is worse, because the user has
 * no reason to doubt it -- and TS 102 527-3 7.4.20.2 makes our duty to send
 * conditional on our own time being valid, which is exactly this question.
 *
 * On OpenWrt the flag alone is not enough the other way round: busybox ntpd
 * never lowers the kernel's error estimate, which grows until the kernel
 * sets STA_UNSYNC again, some hours after boot, on a clock ntpd keeps right
 * (the 7412, 2026-10-29: status 0x41, maxerror 16 s, no date to any
 * handset). The package's ntp hotplug script keeps CLOCK_NTP_FILE fresh
 * while ntpd reports a stratum; younger than CLOCK_NTP_MAX_S, it counts.
 */
#define CLOCK_NTP_FILE		"/var/run/opendectcell/time-valid"
#define CLOCK_NTP_MAX_S		(25 * 60)	/* ntpd reports every 11 min */
#define TIMESYNC_AUTO	0u
#define TIMESYNC_FORCE	1u
#define TIMESYNC_OFF	2u

#ifdef STUBD_TEST
/*
 * tests/test_stubd.c: the daemon-level traces run on a clock of their own,
 * take the mails the daemon sends, and stand in for the chip (main()).
 */
struct stubd;
struct dect_mail;
extern uint64_t stubd_test_ms;
extern time_t stubd_test_wall;
void stubd_test_sent(const struct dect_mail *m);
int stubd_test_run(struct stubd *s);
#endif

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
static void clock_any(struct dect_time *dt)
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

static int clock_now(uint8_t mode, struct dect_time *dt, long *off_s)
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
#include "dect_g722.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u
#define POLL_MS		100
#define DECT_CTL_PATH	"/var/run/opendectcell.sock"
/* End-of-run park: how long to wait for the queue, then for the driver. */
#define PARK_DRAIN_MS	2000u
#define PARK_FLUSH_MS	200u

static volatile sig_atomic_t stop;
static volatile sig_atomic_t want_register;	/* SIGUSR1: open a window */
static uint64_t g_start;		/* for relative timestamps in the log */

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void on_usr1(int sig)
{
	(void)sig;
	want_register = 1;
}

static volatile sig_atomic_t want_deregister;	/* SIGUSR2: see dereg_tick */
static volatile sig_atomic_t want_profiles;	/* SIGHUP: reload profiles */

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

static uint64_t mono_ms(void)
{
#ifdef STUBD_TEST
	return stubd_test_ms;
#else
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

/*
 * One subscription store. dect-stubd loads every store -s names; the first is
 * the handset it rings. struct stubd's sub_*, uak and cc_* fields are the
 * CURRENT handset's -- the one the link is serving -- and hs_switch() moves
 * them in and out of this table, so everything that learns, saves or rings
 * keeps working on "the" handset.
 */
struct handset {
	const char *path;
	/* whether it holds a subscription: base.loaded[] (dect_base.h) */
	/* its last segment: base.last_seen_ms[] */
	/* remote=, a roamer the cell must fetch: base.remote[] */
	/* the file does not exist, free for enrolment: base.absent[] */
	/* its <<TERMINAL-CAPABILITY>>: base.cap[] (dect_base.h, S3k) */
	/* its name, kept in <path>.name: base.name[] (dect_base.h) */
	/* its lines' names and Line 2: base.line_name[], base.line2[] (lines_load) */
	/*
	 * Its handset profile (docs/handset-profiles.md): the model the user
	 * picked, in <path>.profile (the id, and optionally the firmware
	 * version on a second line), resolved at startup and on SIGHUP.
	 * Defaults when there is none.
	 */
	char     prof_id[DECT_PROFILE_ID_MAX];
	char     prof_fw[24];		/* resolved into base.prof[] */
	/* its messages waiting and missed-call notices: base.note[] (S4a) */
};

/*
 * A second connection while the main link is busy. It gets a link of its own
 * for location registration and list access -- the gates and adoption work
 * as on the main link -- but no audio, no ringing and no learning: those
 * stay with the main link. Runs 25-28 refused 1-6 connection-ups per call,
 * handsets that wanted to locate or sync their lists while we were on a
 * call.
 */
#define SIDE_MAX	DECT_BASE_SIDES

/* control socket clients at once: the bridge and a few one-shot ones */
#define CTL_CLIENTS	4u

struct side_link {
	/* dect_base's side[] entry: up, its handle, its C-plane stack, the
	 * record it serves (adopted) */
	struct dect_base_link *bl;
	/* its xlane (-a twocalls): bl->lane, -1 = none */
	/* the chip's 0xF625 for this link's call, as the main link's */
	struct dect_diag_status last_status;
	struct dect_diag_frames frames;
	uint64_t status_ms;
};

/*
 * A second (third) call's audio, on a side link: -a twocalls,
 * docs/two-calls.md. Lane 0 -- the main link's call -- stays the code it
 * always was; these lanes have only the production sources (the G.722 or
 * raw test file, the loop-back, silence), each on its own audio channel
 * and TDM word: channel 2 -> raw word 3, channel 3 -> raw word 2, in calls
 * as in the loop-back (clean room A-0124; logical word k, raw k XOR 1).
 * lane2= / lane3= override.
 */
#define XLANE_MAX	2u

/* The internal call is dect_base's (dect_base.h, S3q). */
#define IC_NONE		DECT_BASE_IC_NONE	/* -1 is the main link */
#define IC_IDLE		DECT_BASE_IC_IDLE
#define IC_PAGING	DECT_BASE_IC_PAGING
#define IC_SETUP	DECT_BASE_IC_SETUP
#define IC_RINGING	DECT_BASE_IC_RINGING
#define IC_UP		DECT_BASE_IC_UP
#define IC_PARTY_NONE	DECT_BASE_IC_NONE
#define IC_PAGES	DECT_BASE_IC_PAGES
#define IC_PAGE_MS	DECT_BASE_IC_PAGE_MS

struct xlane {
	int      used;		/* bound to a side link's call */
	uint8_t  handle;
	uint8_t  channel;	/* 0x0230 flags low nibble */
	unsigned word;		/* TDM word of its audio, both directions */
	int      wideband;	/* the coded G.722 lane */
	int      connected;	/* 0x022D sent */
	size_t   pos;		/* in the test file */
	int      phase, locked;	/* the G.722 two-frame lock */
	uint32_t rx_prev;
};

/* The ring sequence's states are dect_base's (dect_base.h, S3f). */
#define RING_IDLE		DECT_BASE_RING_IDLE
#define RING_RELEASING		DECT_BASE_RING_RELEASING
#define RING_PAGING		DECT_BASE_RING_PAGING
#define RING_WAIT_PAGE_RESP	DECT_BASE_RING_WAIT_PAGE_RESP
#define RING_DONE		DECT_BASE_RING_DONE

/* The bridge's calls are dect_base's (dect_base.h, S3c). */
#define BC_MAX		DECT_BASE_CALLS
#define BC_PAGING	DECT_BASE_PARTY_PAGING
#define BC_NONE		DECT_BASE_PARTY_NONE

/*
 * One lane's speech on the audio socket: lane 0 is the main link's, 1 + i
 * extra lane i's. The lane carries G.722 octets (wideband, a word held two
 * frames) or one 16-bit linear sample per frame (narrowband, 8 kHz); the
 * socket carries what the bridge asked for, its SIP codec's: G.722 octets
 * as they are, or linear PCM at 8 or 16 kHz. Matched -- G.722 to G.722,
 * narrowband to 8 kHz -- nothing is coded here; otherwise the lane's speech
 * is coded or resampled. down[] holds what goes to the handset in the
 * lane's own format.
 */
#define AB_LANES	(1u + XLANE_MAX)

struct ab_lane {
	int      cfd;		/* the bridge's connection, -1 = none */
	unsigned call;		/* announced for this call id, 0 = none */
	int      wideband;
	unsigned rate;		/* the socket's: 8000 or 16000 */
	int      g722;		/* ... G.722 octets (16 kHz) */
	struct dect_g722_state enc, dec;	/* the lane's G.722, coded here */
	struct dect_g722_state sip_enc, sip_dec;	/* the socket's */
	struct dect_g722_state sil;	/* the fill's own: enc codes the speech */
	struct dect_rs2 rs_up, rs_down;	/* towards the bridge, from it */
	uint8_t  down[8192];
	size_t   dh, dn;
	uint8_t  up[320];	/* from the handset: 20 ms in the lane's format */
	size_t   un;
	uint8_t  cur[2];	/* the word on the lane now */
	uint32_t rx_prev;
	unsigned rx_run;
	int      phase, locked, primed;
	uint32_t underruns, dropped, frames_up, frames_down;
};

struct stubd {
	int fd;
	unsigned int slots;
	unsigned int fpb;		/* frames per DMA block (32 = 4 ms) */
	/*
	 * Transport. 0: openDECTcell's own PCM interface on /dev/dect0 (the 7520's
	 * ADSS TDM). 1: the generic SSC PCM transport, /dev/ssc-pcm0 (the
	 * 7412). The SSC has no frame sync, so its words start at an
	 * arbitrary bit offset into the chip's frame, and it delivers bytes
	 * in bus order. ssc_rx_fix() and pcm_write() turn both directions
	 * into the 7520's layout, so nothing above them differs.
	 */
	int ssc;
	struct ssc_align ssc_al;	/* lane offset and stream carries */
	int ssc_aligned;		/* offset measured: fix what we read */
	struct ssc_lane_delay ssc_ld;	/* receive lanes onto the send numbering */
	/*
	 * Scratch for the SSC's per-block alignment, sized at bring-up so the
	 * block path never allocates: ssc_rx_fix() needs a block + 2 bytes,
	 * pcm_write() up to ssc_tx_cap bytes of stream (+ 2 for t).
	 */
	uint8_t *ssc_rx_t, *ssc_tx_t, *ssc_tx_out;
	size_t   ssc_tx_cap;
	uint64_t ssc_tx_sent_prev, ssc_tx_idle_prev;
	/*
	 * The alignment watch (ssc_watch()): the idle lanes' bit offset and
	 * frame positions in the raw stream, and the driver's counters, at
	 * the last line it printed. The 7412's control channel went silent
	 * 23-60 s after contact in 4 of 5 runs on 2026-10-29; this says
	 * whether the stream lost data, slipped a bit or drifted.
	 */
	int      ssc_w_off;		/* -2: not measured yet */
	uint64_t ssc_w_mask;
	int      ssc_w_pend_off;	/* a change seen once, not yet printed */
	uint64_t ssc_w_pend_mask;
	uint64_t ssc_w_next;		/* next check, mono ms */
	uint32_t ssc_w_deaths;		/* session deaths already reported */
	struct ssc_pcm_stats ssc_w_st;
	/*
	 * Downlink starvation, sampled once a second. The driver refills a
	 * transmit block when the chip's clock asks for one and fills any
	 * frame the queue cannot supply with the idle template, so
	 * tx_blocks * fpb - tx_frames is the number of frames the handset
	 * heard as idle pattern instead of audio. Printing it only at exit
	 * could not separate starvation during playback from priming at
	 * start-up [CR A-DSP-1 §9/§10].
	 */
	uint32_t pcm_tx_blocks_prev;
	uint32_t pcm_tx_frames_prev;
	int pcm_tx_primed;
	int tx_slot;		/* -1 = unknown */
	int rx_slot;		/* -1 = unknown */
	uint16_t tx_seq;
	int verbose;
	int show_params;	/* log parameter bytes (may hold identity data) */
	const char *xmail[20];	/* extra mails to send once the session is up */
	struct xlane xl[XLANE_MAX];
	/* An internal call: base.ic (dect_base.h, S3q); its codec mismatch reported */
	int ic_warned;
	/* Deregistering a handset: base.dereg (dect_base.h, S4b) */
	/* The periodic date and time: base.tu (dect_base.h, S4c) */
	/*
	 * The control socket (docs/bridge-interface.md §2): one message per
	 * line, from up to CTL_CLIENTS clients at once.
	 */
	struct {
		int      lfd;		/* listening; -1 = none */
		const char *path;
		/*
		 * Up to CTL_CLIENTS at once: the bridge (baresip) stays
		 * connected, and LuCI's one-shot commands must not find the
		 * socket taken.
		 */
		struct {
			int    fd;	/* -1 = free */
			char   in[DECT_CTL_LINE_MAX];
			size_t in_len;
			int    discard;	/* an over-long line: drop to its newline */
		} cl[CTL_CLIENTS];
		int      cur;		/* the client whose command runs, -1 */
		int      bridge;	/* the client that said "bridge", -1 */
	} ctl;
	/*
	 * The bridged calls' speech (docs/bridge-interface.md §3): one
	 * listening SOCK_SEQPACKET socket, a connection per call, 20 ms
	 * frames of 16 kHz linear PCM both ways; per lane the coding to and
	 * from the call's G.722 or narrowband linear lane. New connections
	 * wait in pend[] until they name their call.
	 */
	struct {
		int      lfd;
		int      pend[AB_LANES];	/* -1 = free */
		uint64_t pend_since[AB_LANES];
		struct ab_lane l[AB_LANES];
	} ab;
	/* The registration window: open now, and until when (0 = no limit). */
	/* the registration window: base.reg_open (dect_base.h, S3z) */
	/* the profile the user picked for the next handset to enrol (LuCI) */
	char     reg_profile[DECT_PROFILE_ID_MAX];
	unsigned int n_xmail;
	unsigned int xmail_idx;	/* next extra mail to send (one per loop tick) */
	uint64_t late_t0;	/* latemail: when the subscriptions were reloaded */
	unsigned late_idx;	/* latemail: next one to send */
	struct dect_rx rx;
	struct dect_session sess;

	/* GAP registration (-g): the host-side C-plane stack per connection. */
	int do_register;
	/*
	 * Cell mode (-K): the box is a cell of an openDECTcell controller; no stack
	 * here, every mail goes through dect-cell (cell_tick()).
	 */
	int cell_mode;
	struct {
		int      lfd, cfd;	/* listening, dect-cell's; -1 = none */
		uint64_t lost_at;	/* the controller link went, 0 = up */
		int      released;	/* §6.4: links released after grace */
		int      orphan_parked;	/* ... and the radio parked after orphan */
		int      start_parked;	/* parked once the chip answered */
		int      radio_on;	/* the controller turned it on */
		int      sess_up;	/* the chip session, as last reported */
		uint8_t  flow;		/* §9.3 FLOW state last sent */
		int      active;	/* CELL_STATE active: mails may go down */
		uint32_t epoch;		/* chip sessions since start */
		uint8_t  rfpi[5];	/* the RFPI the chip reports (0xF601) */
		int      have_rfpi;
		uint8_t  handles;	/* links up, bit per handle 0..7 */
		uint8_t  tag[8][3];	/* each one's handset tag (0x0203) */
		uint32_t up, down, rejected, lost_up;
		uint32_t txq_drops_seen;
		/*
		 * §8 media: one session per speech lane (ab.l[], same
		 * index), and each handle's bind as the chip confirmed it
		 * in the long 0x0242 {h, chan_flags, mask0, mask1}.
		 */
		struct {
			int      open;
			uint32_t id;	/* the controller's media_id */
			uint8_t  chan, handle;
			unsigned word;	/* TDM word, both directions */
		} media[AB_LANES];
		unsigned media_n;
		unsigned media_gen;	/* the lanes' call ids, never 0 */
		struct {
			uint8_t nib;	/* chan_flags low nibble; 0 = no bind */
			uint8_t k;	/* the mask's bit: channel 1..3 */
			uint8_t wb;	/* bit 6: the coded G.722 lane */
		} bind[8];
	} cell;
	const char *pin;	/* registration PIN / AC */
	/*
	 * Wrong PINs in the System Settings List across all connections, and
	 * the lockout they earn (security audit 2026-10-29, A4): the library
	 * allows a few per connection, this bounds the connections.
	 */
	/* wrong PINs and their lock: base.pin_wrong (dect_base.h, S4d) */
	int rand_fd;		/* /dev/urandom, -1 if unavailable */
	/* the chip's RFPI: base.rfpi (dect_base.h, S3w) */
	/*
	 * The handset's long-term key, kept across connections: registration and a
	 * later call run on different connections (each re-inits the MM FSM), and
	 * call-time ciphering needs the UAK. Persisted in the subscription store
	 * (-s) so a call after a daemon restart needs no fresh enrolment.
	 */
	const char *sub_path;	/* host-side subscription store (holds the UAK) */
	/* -b: the Contact List's file (the CardDAV sync writes it) and when we
	 * last read it; NULL = no Contact List for the handsets */
	const char *contacts_path;
	uint64_t contacts_at;
	long long contacts_mtime;
	long long contacts_size;
	/*
	 * Every -s store, [0] = ring target: the records in base.hs[], the
	 * rest of each handset (its file, names, profile) in hs[] at the same
	 * index. base.rec is the current handset's working copy
	 * (dect_base.h); reg_start() pushes its port_id into the MM state
	 * machine, so a location registration from a handset we do not hold
	 * is rejected.
	 */
	struct handset hs[DECT_BASE_HS_MAX];
	struct dect_base base;
	/* rejected_identity of this connection's CC, as last reported */
	struct side_link side[SIDE_MAX];
	/* The Fon/DECT LED, see led_tick(). */
	int         led_fd;
	int         led_state;		/* last written, -1 = unknown */
	uint64_t    led_next;
	uint32_t    led_traffic, led_traffic_seen;
	unsigned    led_changes, led_blink_ticks;
	uint64_t    state_next;		/* next state-file write */
	int         holdback_done;
	/* Test M4: the held LOCATE-REQUEST and the simulated fetch. */
	uint64_t    fetch_req_ms, fetch_at;
	int         fetch_idx;		/* matching remote store, -1 none */
	uint32_t    fetch_retrans_seen;
	/* Ringing the handset: base.ring (dect_base.h, S3f). */
	int sub_restored;	/* stored subscription already pushed to the chip */
	/*
	 * Chip subscription slot, 1..DECT_MM_MAX_SLOT, 0 = not chosen yet.
	 *
	 * This decides two things at once, which is why it matters for
	 * multi-handset work: the chip UAK slot at 0x9518 + (slot-1)*0x11,
	 * and the assigned TPUI, which dect_mm_tpui() derives as
	 * {0x00, 0x11, slot}. Left at the default every handset is slot 1,
	 * so a second handset overwrites the first's UAK in the chip AND is
	 * given the same TPUI 0x1101 -- after which a page for 0x1101 can be
	 * answered by either of them and a per-handset test is meaningless.
	 *
	 * The store has always carried the slot; sub_load() printed it and
	 * threw it away, and reg_start() never set mcfg.sub_slot at all.
	 * It is rec.slot.
	 */
	/*
	 * The call key for the chip (0x0208, stock's call layout [CR A-0126]),
	 * sent whenever a call is ciphered, in plain byte order (docs/stub-api.md
	 * §2.1); -k off withholds it.
	 */
	struct chip_cipher {
		int     disabled;	/* -k off */
	} chip_cipher;
	/* voice_slot_probe(): per-lane changes in the last second of a call */
	uint16_t vs_last[64];		/* every lane of an SSC frame */
	uint32_t vs_changes[64];
	uint32_t vs_frames;
	int         no_cipher;		/* -C: never send CIPHER-REQUEST */
	/*
	 * Continuous transmit [CR A-0030 §1.1, §10]. Stock AVM services the TDM
	 * transmit DMA every block, interleaving the control slot and the voice
	 * lanes into one stream; it does not write a burst of frames only when a
	 * mail is pending. Worse, our burst wrote 0xFF across *every* slot, which
	 * A-0030 §4 Q8 warns against explicitly: it overwrites the voice lanes.
	 *
	 * So mails become a queue of control-slot bytes (two per frame) that the
	 * block loop drains, and each transmitted block fills the voice lanes with
	 * their own idle value instead of trampling them.
	 */
	int         tx_ring;		/* ring active: queue mails, do not write */
	uint8_t     txq[4096];		/* pending control-slot bytes */
	size_t      txq_head, txq_len;
	uint32_t    txq_drops;		/* mails refused: control queue full */
	size_t      txq_hwm;		/* the queue's high-water mark */
	uint32_t    tx_write_short;	/* block writes the driver did not take whole */
	int         parked;		/* run_loop() drained the park mails */
	uint64_t    park_check_at;	/* 06.88: park read-back sent at */
	int         park_radio, park_timer;	/* its answers, -1 = none yet */
	int         park_check_retried;
	int         uak_0688;		/* 06.88: UAK table at 0x067F */
	uint8_t     tx_idle;		/* filler for unmapped slots (-a txidle) */
	uint8_t     tx_voice_idle;	/* filler for the voice lanes */
	uint32_t    tx_blocks;
	uint8_t    *tx_tmpl;		/* tx_templates(): two idle frames */
	/*
	 * Audio bind parameters [CR A-0029 §1]. Every field AVM derives from its
	 * negotiated call state is settable here, because we cannot derive them
	 * and each hardware run costs a person's attention -- so one run should
	 * be able to sweep a hypothesis rather than needing a rebuild per guess.
	 */
	struct audio_cfg {
		uint8_t  sel;		/* 0x0251 selector, valid 1..6; the stock
					   host derives it from the 0x0230 mode
					   byte's low nibble [CR A-CodecModes-3] */
		uint8_t  gain;		/* 0x0251 value (stored << 8 by the STUB) */
		/*
		 * 0x0230 is {link, mode, mask_le16, master, extra}
		 * [CR A-CodecModes-1]. The CLI keys below are the historical
		 * names and are kept so the recorded hardware runs in
		 * docs/call-g7.md stay reproducible -- `flags` is the mode
		 * byte, `mode` is the master byte, `codec` is the extra byte.
		 */
		uint8_t  flags;		/* 0x0230 mode byte: low nibble = audio
					   channel, bit 6 (0x40) = codec-class
					   flag. 0x41 = channel 1 wideband (our
					   working G.722 bind); 0x01 = the
					   channel-1 narrowband candidate,
					   untested and only meaningful on a
					   bearer negotiated narrowband; 0x40
					   alone allocates no channel -- never
					   send it */
		uint16_t mask;		/* 0x0230 TDM slot mask, little-endian */
		uint8_t  mode;		/* 0x0230 master byte (not a mode) */
		uint8_t  codec;		/* 0x0230 extra byte (not a codec id) */
		/* subslot=N: chip subscription slot, 1..6. 0 = use the store's. */
		unsigned sub_slot;
		/*
		 * attcfg=N / attflag=N: the two subscription-attach bytes the
		 * clean room [CR A-0113] found stock sends non-zero and we send
		 * as 0x00 -- the suspected cause of GAP (Gigaset) handsets
		 * refusing an incoming call with 0x32 while the AVM C4 rings.
		 *
		 *  attcfg  = the 0x020A config byte after the 16-octet UAK.
		 *            Stock sends 0x01 per Gigaset (0x02 for the C4); the
		 *            0x020A handler stores it at the UAK slot +0x10 and
		 *            the CC state machine copies it into the connection
		 *            record's auth/capability word (conn+0x21). Sending
		 *            0 seeds a different security class, which a GAP
		 *            handset gates call acceptance on. PRIMARY.
		 *  attflag = the 0x0247 install flag byte (third byte). Stock
		 *            sends bit 0 = 1 (A540 = 0x03); our 0x00 leaves a
		 *            per-slot mask bit set where stock clears it.
		 *            SECONDARY.
		 *
		 * Both default to the historical 0x00 so nothing changes unless
		 * asked; set them to match stock for a given handset.
		 */
		uint8_t  att_cfg;
		uint8_t  att_flag;
		/*
		 * ipuio=N: a handset registering is assigned IPUI O number N
		 * in {ACCESS-RIGHTS-ACCEPT} instead of keeping its IPUI N
		 * (bench B4a, docs/multicell-identities.md). One handset per
		 * value.
		 */
		uint32_t ipuio;
		int      ipuio_set;
		/*
		 * rfpicheck=HEX10: compare the RFPI the chip reports in 0xF601
		 * with these 5 bytes and log only the verdict (bench B0: a
		 * class B RFPI patched into tag 1, docs/multicell-identities.md).
		 */
		uint8_t  rfpi_check[5];
		int      rfpi_check_set;
		/*
		 * 0x017c 00 <mask>: the handsets locked in, bit n = chip slot
		 * n+1 holding a live 0x020A record. [CR A-0129], from AVM's
		 * symboled driver: stock builds it from its active-handset
		 * table and sends it after every registration and every
		 * deregistration (1 handset 01, slots 1-3 07, 1-4 0f). It
		 * supersedes A-0114, which read it as a bearer-slot mask:
		 * the `09` A-0114 saw was slots 1 and 4 in that capture. We
		 * sent a fixed 09 from 2026-10-24 to 2026-10-28; our handsets
		 * in slots 2 and 3 rang all the same, so whatever the chip
		 * does with it, paging by TPUI from the host does not depend
		 * on it. Now computed (live_slot_mask()); slotmask=N pins it,
		 * noslotcfg omits the mail (controls).
		 */
		uint8_t  slot_mask;
		int      slot_mask_fixed;	/* slotmask=N given */
		int      no_slot_cfg;
		/*
		 * codeclist=HEX replaces the <<CODEC-LIST>> contents (the IE
		 * value, without tag or length). EN 300 175-5 §7.7.54: one
		 * negotiation octet then 3 octets per codec
		 * {identifier, direction+MAC/DLC service, routing+slot size}.
		 */
		uint8_t  codeclist[31];
		size_t   codeclist_len;
		/*
		 * infoie=HEX: information elements, already encoded, sent once
		 * in a {CC-INFO} when the handset starts ringing on our call.
		 * A lab tool for the caller-name test (spec sweep S3, A-0125):
		 * the bytes come from the bench card, so no display framing
		 * lives in the code.
		 */
		uint8_t  info_ie[64];
		size_t   info_ie_len;
		/*
		 * remode=MS: MS ms after 0x022D, a second 0x0230 for the same
		 * handle and channel with flag bit 6 clear. [CR A-0039]: the
		 * chip's channel configurator picks its mode from the assign's
		 * flags and the path state. A first 0x0230 with bit 6 set and
		 * the 0x022D connect both run mode 1, which feeds PCM out(k)
		 * from the silence constant and disables the channel's transmit
		 * chain; a second 0x0230 with bit 6 clear while the path is on
		 * selects mode 2: PCM out(k) <- channel k, channel k <- PCM
		 * in(k). main() says when it is sent.
		 */
		unsigned remode_ms;
		const char *led_path;	/* led=PATH, NULL = the default */
		const char *state_path;	/* statefile=PATH, NULL = the default */
		int      no_state;	/* nostatefile */
		int      reg_open;	/* regopen: open registration at start-up */
		unsigned reg_window_s;	/* regopen=SECS: close after; 0 = never */
		int      two_calls;	/* twocalls: speech on side links */
		int      locauth;	/* locauth: authenticate on locating */
		int      auth_first;	/* authfirst: no service before proof */
		uint8_t  lane_ch[XLANE_MAX], lane_word[XLANE_MAX];
		uint8_t  lane_set[XLANE_MAX];	/* lane2=/lane3= given */
		unsigned reg_signal_s;	/* regwindow=SECS for SIGUSR1, 120 */
		unsigned holdback_ms;	/* holdback=MS, multi-cell test M2 */
		int      no_chip_keys;	/* nochipkeys, multi-cell test M2 */
		const char *remote_path;	/* remote=PATH, test M4 */
		unsigned fetch_delay_ms;	/* fetchdelay=MS, test M4 */
		unsigned keycheck_ms;	/* keycheckms=MS: read-back delay */
		unsigned tpui_offset;	/* tpuioffset=N: TPUI off the slot */
		unsigned install_dummy;	/* installdummy=N: 0x0247 installs slot N with no key */
		struct {		/* latemail=MS:SRC:DST:PRIM[:HEX], repeatable */
			unsigned ms;
			char     spec[80];
		} latemail[8];
		unsigned n_latemail;
		int      keycheck_set;
		unsigned locate_validity;	/* locvalid=N x 40.96 s, M4 */
		int      no_led;	/* noled */
		int      remode_set;	/* remode= or noremode given */
		/*
		 * seq=MS with repeated seqstep=FLAGS:CODEC walks a list of
		 * 0x0230 configurations during one call, MS ms per window,
		 * starting when the call binds -- the same instant playraw
		 * restarts the lane file at offset 0. Build the lane file with
		 * windows of the same length and the formats stay aligned with
		 * the modes by construction.
		 *
		 * Window 0 is left alone, so it records whatever the bind itself
		 * produced. That matters: the bind is not reliable. On
		 * 2026-10-23 vF49 reached dest 6 = 085d with the enable word 3,
		 * while vF412 read dest 6 = 0410 and enable 0 at the same point
		 * with identical mails, which made that run's result
		 * uninterpretable rather than negative.
		 *
		 * Pair this with memdump of the route words: every window then
		 * carries its own proof of which state the chip was actually in,
		 * so one attended call tests many hypotheses and none of them
		 * rests on the bind having worked.
		 */
		unsigned seq_ms;
		const char *seq_step[16];
		unsigned n_seq;
		/*
		 * toneword=N: the TDM word of lane 0, the main link's call, in
		 * both directions. 0 on the 7520; on the SSC transport it
		 * follows the control lane unless given.
		 */
		uint8_t  lane0_word;
		int      lane0_word_set;	/* toneword= given */
		uint8_t  tx_idle;	/* transmit filler for unmapped slots */
		int      have_tx_idle;
		uint8_t  tx_voice_idle;	/* transmit filler for the voice lanes */
		int      have_voice_idle;
		unsigned status_ms;	/* poll 0xF624 connection status (0 = off) */
		int      cap_all;	/* -w captures from start-up, not just calls */
		int      loopback;	/* echo the received bearer word back */
		/*
		 * playsync: advance the coded stream when the *received* lane
		 * word changes, instead of free-running a two-frame cadence
		 * of our own. v31 showed why this matters: a loopback delayed
		 * by half a second came back as clear speech, so the content
		 * can be arbitrarily old -- what the chip needs is that our
		 * word changes on the same frame boundary its own does. A
		 * free-running cadence slips against it, which a steady sine
		 * survives (v25) and speech does not (v29, v30).
		 */
		int      play_sync;
		/*
		 * memdump=ADDR:LEN sweeps chip memory with the documented
		 * read-any-address debug mail (0xFFF1 sub 0x07, 32 bytes a
		 * time) [CR COMMAND_SET 7.1], logging each reply. Repeat the
		 * key for several ranges. Polling runs from start-up, not from
		 * the call bind, so one run captures an idle baseline and the
		 * live state and we can diff them ourselves rather than relying
		 * on another box's absolute byte values.
		 *
		 * Targets from [CR A-StockTrace-2]: the DSP vector table, the
		 * per-slot DSP descriptor table and the media region.
		 * READ ONLY -- A-RouteMedia-FINAL rules out writes to chip RAM.
		 */
		/*
		 * 24, not 8. Eight was enough while a run watched one region;
		 * a run now carries the matrix, the connection record for both
		 * handles and the five slot descriptors, which is 13 before
		 * anything specific to the question being asked. Exceeding the
		 * cap used to fail the whole option string -- see the parser.
		 */
		struct { uint32_t addr; uint32_t len; } memdump[24];
		unsigned n_memdump;
		unsigned memdump_ms;
		/*
		 * basicsvc=N: the <<BASIC-SERVICE>> value octet. Low nibble
		 * is the service (8 = wideband speech), high nibble the call
		 * class. v32 rang with 0x88 and the C4 labelled the call
		 * "Intern" rather than showing the number it had plainly
		 * parsed, so the call class is the first thing to sweep.
		 */
		uint8_t  basic_svc;
		int      basic_svc_set;	/* basicsvc= given */
		/*
		 * narrowband: never wideband. By default (since bench tests
		 * 10-11, 2026-10-25) our SETUP asks for wideband (0x88), our
		 * CONNECT chooses G.722 when offered, and a call whose codec
		 * is G.722 gets stock's wideband bind -- flags 0x41, codec
		 * byte 0, no mode-2 re-assign -- with the downlink phase-locked
		 * to the chip's two-frame words (playsync). Any other call
		 * gets the narrowband bind (mode 2, linear PCM) as before.
		 */
		int      narrowband;
		/*
		 * Overlap receiving, on unless nooverlap: a handset-placed call
		 * gets {CC-SETUP-ACK}, its digits arrive in {CC-INFO}, and the
		 * call is accepted (CALL-PROC, CONNECT) dialwait=MS after the
		 * last digit (default 3000). Without it we CONNECT before the
		 * digits arrive and the Gigasets show neither "connected" nor a
		 * call timer (bench test 2, 2026-10-25).
		 */
		int      no_overlap;
		unsigned dial_wait_ms;
		/*
		 * The call timer on handset-placed calls (bench test 12b: none
		 * on the A540). alertms=N: {CC-ALERTING} after CALL-PROC and
		 * CONNECT N ms later. bindfirst: bind the audio after CALL-PROC
		 * and CONNECT once it is up, as EN 300 175-5 §9.3.1.8 orders
		 * it. Both: CONNECT when both hold.
		 */
		unsigned alert_ms;
		int      bind_first;
		/*
		 * nocallstatus: no <<CALL-INFORMATION>> call status in our
		 * answers to a handset's call. By default CONNECT carries
		 * "CS call connect", which TS 102 527-3 §7.4.6.4.3 requires and
		 * the Gigasets key their call timer on (clean room A-0122).
		 */
		int      no_call_status;
		/*
		 * allcallstatus: call status to every handset, as before
		 * 2026-10-28. By default only a handset that says it is NG-DECT
		 * Part 3 gets it: "A Part 3 FP shall not send any call status
		 * to GAP or NG Part 1 devices" (TS 102 527-3 §7.4.6.4.1), and
		 * stock sends it to the A540 (Part 3) only (A-0125).
		 */
		int      all_call_status;
		/*
		 * nlr: release the link ourselves once its last call or list
		 * session has ended (EN 300 175-5 §14.2.7), as stock does:
		 * `0x0202 <link>` ~30 ms after the last {CC-RELEASE-COM}, a
		 * LAPC DISC first on a list-access link [CR A-0127]. On by
		 * default since bench 2026-10-28 (vNLR: two calls and a list
		 * session on the A540, chip ack 24 ms later, no handset
		 * complaint); nonlr turns it off. The list-access DISC branch
		 * did not run there: the A540 ended its list links itself.
		 */
		int      nlr;
		unsigned setup_tries;	/* 0 = RING_SETUP_TRIES */
		/*
		 * timesync: 0 = auto (send only when the kernel says the
		 * clock is disciplined), 1 = force (send anyway), 2 = off.
		 *
		 * "force" is not only a test lever. A deployment can have a
		 * trustworthy clock the kernel knows nothing about -- the
		 * controller pushing time over the cell link, for one -- and
		 * STA_UNSYNC would refuse it. The operator gets to say.
		 */
		uint8_t  timesync;
		/*
		 * timeupdate=MIN: every MIN minutes, the date and time to
		 * every handset we hold (time_tick), on top of the one after
		 * each location registration. 0 = only then.
		 */
		unsigned time_every_min;
		/*
		 * signal=N: send <<SIGNAL>> N in the SETUP. v33 showed a
		 * call-class-3 SETUP displays the caller but never alerts,
		 * so the alerting pattern has to be stated rather than left
		 * to the handset. 0x40..0x47 are alerting patterns 0..7.
		 */
		int      signal_on;
		uint8_t  signal_value;
		int      signal_set;	/* -a signal= given: beats a profile */
		const char *profile_dir;	/* -a profiles=DIR */
		const char *ctl_path;	/* -a ctl=PATH, the control socket */
		const char *line_name;	/* -a linename=NAME, "Line 1" */
		int      no_ctl;	/* -a noctl */
		/*
		 * ring=MS: MS ms after the radio is up, call the handset.
		 * Page it with 0x0300, then send an FP-originated CC-SETUP on
		 * the link it brings up. Milestone B; see docs/call-g7.md.
		 */
		unsigned ring_ms;
		/*
		 * ringattach=MS: as ring=MS, but counted from the handset's
		 * FIRST C-plane contact rather than from start-up. An A540
		 * that has lost the base re-locks anywhere from 2 s to several
		 * minutes after the radio comes up (2026-10-24: 1.6 s, 4.8 s,
		 * 57 s, 117 s, and not within 170 s), so a fixed settle
		 * either wastes the run or rings into thin air. Unattended
		 * runs want the ring to land while the handset is demonstrably
		 * there. 0 = off.
		 */
		unsigned ring_attach_ms;
		/*
		 * clip=NUMBER and cnip=NAME: the caller identity an
		 * FP-originated CC-SETUP carries. Both are presented as
		 * "allowed, user-provided not screened", because that is the
		 * only honest claim we can make for a number a person typed
		 * on our command line -- we have no network screening verdict
		 * and must not assert one. Absent options mean no IE at all.
		 */
		const char *clip_number;
		const char *clip_name;
		/*
		 * lineid=N linetype=N callid=N callstatus=N: the
		 * <<CALL-INFORMATION>> subrecords of an FP-originated SETUP.
		 * TS 102 527-3 7.4.5.3.2 requires a line identifier in every
		 * incoming external call and none in an internal one, and we
		 * have never sent it -- the leading reason the C4 says
		 * "Intern". Each is opt-in; none means no IE.
		 */
		struct dect_callinfo callinfo;
		/*
		 * play=FILE: transmit a raw G.722 stream to the handset while a
		 * call is up, looping. The lane wants one coded 16-bit word per
		 * two TDM frames, i.e. 8000 bytes/s, which is exactly G.722's
		 * rate, so the file is consumed two bytes every two frames.
		 *
		 * This is the clean downlink source the loopback cannot be: the
		 * loopback re-sends the uplink including its underruns, so any
		 * glitch the handset plays might be ours. A file plays the same
		 * every time.
		 */
		const char *play_path;
		/*
		 * playpcm=FILE takes the same lane but feeds it ordinary
		 * 16 kHz signed 16-bit little-endian PCM, which is encoded to
		 * G.722 once at start-up. The box has no ffmpeg, so this is
		 * the only way to put real audio on the downlink there rather
		 * than a file somebody pre-encoded on a workstation.
		 */
		int       play_pcm;
		/*
		 * playraw=FILE: the file is not a G.722 stream but a verbatim
		 * TDM lane -- two bytes per frame, 16000 B/s, exactly as a
		 * -w capture holds them. Nothing is interpreted: no cadence
		 * logic, no two-frame hold, no octet pairing. Feeding back a
		 * lane the chip itself produced is the one downlink test that
		 * assumes nothing about the convention.
		 */
		int       play_raw;
		uint8_t  *play;
		size_t    play_len;
	} audio;
	uint32_t    rx_word_prev;	/* last received lane word, for playsync */
	unsigned    md_idx;		/* memdump: range being swept */
	uint32_t    md_off;		/* memdump: offset within that range */
	uint64_t    md_at;		/* memdump: last request */
	unsigned    md_pass;		/* memdump: completed sweeps */
	int         play_locked;	/* playsync has seen its first transition */
	size_t      play_pos;		/* byte offset into audio.play */
	int         play_phase;		/* 0 = load a new word, 1 = hold it */
	/*
	 * Voice capture (-w): the call's TDM slot raw, two bytes per frame, or
	 * every slot, so the sample format can be worked out offline.
	 */
	const char *voice_path;
	FILE       *voice_fp;
	uint8_t    *voice_buf;		/* a block, the control slot blanked */
	int         voice_slot;		/* -V: one slot only; <0 = every slot */
	uint32_t    voice_bytes;
	uint32_t    voice_space_checked;
	/* Which STUB is loaded, from DECT_STUB_RECORD; see stub_build_read(). */
	int      stub_build;
	char     stub_name[64];
	/*
	 * Traffic from handsets that are not the one this run serves.
	 * Counted rather than silently dropped: a busy neighbour is the
	 * difference between a result and a confounded one, and the run
	 * should say so instead of leaving it to be worked out from a log.
	 */

	/* Per-run tally of commands sent, by verification class. */
	uint32_t verify_n[17];
	uint32_t verify_other;
	/*
	 * The main link's call and audio state; its connection and
	 * registration state is base.main_link (dect_base.h).
	 */
	struct reg_ctx {
		int nonspeech_noted;	/* logged "not a speech call" once */
		uint64_t remode_at;	/* when to send the mode-2 re-assign, 0 = off */
		int      remode_done;	/* the mode-2 re-assign has been sent */
		uint64_t seq_t0;	/* when the call bound, 0 = no call */
		unsigned seq_idx;
		uint8_t  remode_handle;	/* handle the call bound on; 0 is valid */
		uint64_t bind_ms;	/* when 0x0230 went out, for the fallback */
		uint64_t status_ms;	/* when the last 0xF624 poll went out */
		uint16_t counter[11];	/* previous 0xF625 counters, for deltas */
		uint16_t frames_last;	/* previous [36..37], for wrap detection */
		uint64_t frames_total;	/* unwrapped frame count for this call */
		int have_counters;
		struct dect_diag_status last_status;	/* for the state file */
		int call_wideband;	/* this call's bind is the G.722 one */
	} reg;
};

/*
 * The chip's UAK table [CR A-0085]: six 17-byte records, 16 key bytes and a
 * config byte each. The address is per STUB build: 0x9518 on 08.25, the one
 * A-0085 was read from; 0x067F on the 7412's 06.88, where a marker record
 * written with 0x020A was found [CR A-0007, A-0011] (observed 2026-10-29).
 * On 07.56 0x9518 holds something else and the table's address is not known
 * yet (Q-0059). main() picks the address once the build is known.
 */
#define SUB_UAK_BASE_0825	0x9518u
#define SUB_UAK_BASE_0688	0x067Fu
static uint32_t sub_uak_base = SUB_UAK_BASE_0825;
#define SUB_UAK_BASE	sub_uak_base
#define SUB_UAK_END	(SUB_UAK_BASE + DECT_MM_MAX_SLOT * DECT_BASE_UAK_STRIDE)
/*
 * On the 7412's 06.88 STUB the read-back and the slot audit wait this long
 * after the writes (observed 2026-10-29; see dect_base_uak_verify()).
 */
#define SUB_CHECK_DELAY_0688_MS	50u	/* a write lands within 20 ms (T-A, 2026-10-31) */

/*
 * The daemon's files live in a directory only root can enter, not in the
 * world-writable /tmp, where any local user could plant them first: a
 * forged STUB record or status, a deregistration request (security audit
 * 2026-10-29, F1).
 *
 * Which second-stage STUB the chip runs. The chip says so itself only in the
 * 0xFFF2 build stamp right after a boot, which a run reusing a loaded STUB
 * never sees, so the service's load_chip (opendectcell.init) records the blob it
 * uploaded in DECT_STUB_RECORD. The chip RAM addresses we read, the UAK
 * table's above, belong to one build: read against another they are
 * unrelated memory.
 */
#ifndef DECT_RUN_DIR			/* tests/test_stubd.c sets its own */
#define DECT_RUN_DIR		"/var/run/opendectcell"
#endif
#define DECT_STUB_RECORD	DECT_RUN_DIR "/stub.second"

#ifdef STUBD_TEST
#define ROOT_UID	getuid()	/* the traces run as their user */
#else
#define ROOT_UID	0u
#endif

/* fopen(path, "r"), if path is root's own and only root may write it. */
static FILE *fopen_root_only(const char *path)
{
	struct stat st;
	FILE *f = fopen(path, "r");

	if (f && (fstat(fileno(f), &st) || st.st_uid != ROOT_UID ||
		  (st.st_mode & (S_IWGRP | S_IWOTH)) || !S_ISREG(st.st_mode))) {
		fprintf(stderr, "ignoring %s: not root's own file\n", path);
		fclose(f);
		f = NULL;
	}
	return f;
}
#define DECT_STUB_0825		"dectfw_secondlevel_441.hex"
#define DECT_STUB_0688		"dectfw_secondlevel_441-06.88-7412.hex"	/* 7412 */

enum { STUB_UNKNOWN, STUB_0825, STUB_OTHER };

/*
 * Where an init script from before 1c081ee records it. apk keeps an edited
 * /etc/init.d/opendectcell over the packaged one (as .apk-new), so an upgraded
 * box can run this daemon under that script: the lab 7520 did, and its
 * audit read the 08.25 address on a 07.56 chip (T13, 2026-10-30). Only a
 * file root owns counts, so one another user plants in /tmp is ignored.
 */
#ifndef STUBD_TEST
#define DECT_STUB_RECORD_OLD	"/tmp/dect-stub.second"
#endif

static int stub_build_read(char *name, size_t cap)
{
	FILE *f = fopen_root_only(DECT_STUB_RECORD);
	size_t n;

	name[0] = '\0';
#ifdef DECT_STUB_RECORD_OLD
	if (!f)
		f = fopen_root_only(DECT_STUB_RECORD_OLD);
#endif
	if (!f)
		return STUB_UNKNOWN;
	if (!fgets(name, (int)cap, f))
		name[0] = '\0';
	fclose(f);
	n = strcspn(name, "\r\n");
	name[n] = '\0';
	if (!n)
		return STUB_UNKNOWN;
	return strcmp(name, DECT_STUB_0825) ? STUB_OTHER : STUB_0825;
}

/*
 * Whether parameter byte i of m is key material: the 16 UAK octets of a
 * 0x020A subscription record, or a byte of a 0xFFF1 memory-read reply
 * (sub 0x08: address in [1..4], data from [7]) that falls in the UAK table.
 */
static int mail_byte_is_key(const struct dect_mail *m, unsigned int i)
{
	uint32_t addr;

	if (m->prim == 0x020Au)
		return i >= 1u && i <= DECT_MM_UAK_LEN;
	if (m->prim == 0x0208u)		/* the call key */
		return i >= 1u && i <= DECT_MM_DCK_LEN;
	if (m->prim != 0xFFF1u || m->param_len < 7u || m->param[0] != 0x08u
	    || i < 7u)
		return 0;
	addr = (uint32_t)m->param[1] | ((uint32_t)m->param[2] << 8)
	     | ((uint32_t)m->param[3] << 16) | ((uint32_t)m->param[4] << 24);
	addr += i - 7u;
	return addr >= SUB_UAK_BASE && addr < SUB_UAK_END;
}

/*
 * Log a mail. Parameter bytes are hidden by default: the 0xF601/0xF602
 * answers carry box identity (RFPI-like) data that must not leak into
 * shared logs or issues. Pass -p to show them for local debugging only.
 * Key bytes print as ** even under -p: the UAK in a 0x020A record or in a
 * read-back of the table. Until 2026-10-24 -p printed both, in the clear,
 * in every run that loaded or registered a handset.
 */
static void log_mail(const struct dect_mail *m, const char *dir, int show_params)
{
	unsigned int i;

	printf("[%5llu] %s src %02x dst %02x prim %04x len %u",
	       (unsigned long long)(mono_ms() - g_start), dir, m->src, m->dst,
	       m->prim, m->param_len);
	if (!show_params) {
		printf("%s\n", m->param_len ? " [params hidden]" : "");
		return;
	}
	/*
	 * Print the whole payload. The old 24-byte cut silently discarded the
	 * part of 0xF625 that matters most: its 28 per-terminal counters sit
	 * at offsets 16..43 [CR COMMAND_SET 8.1], so every run up to
	 * 2026-10-20 logged 8 of them and threw the other 20 away.
	 */
	for (i = 0; i < m->param_len && i < sizeof(m->param); i++) {
		if (mail_byte_is_key(m, i))
			printf(" **");
		else
			printf(" %02x", m->param[i]);
	}
	/*
	 * The build stamp is ASCII ("RAM: <date>"). It arrives once, as the
	 * first mail after the first contact following a chip boot, so a run
	 * that reuses an already-loaded STUB never sees it. Decoding it here
	 * makes "which chip build is actually loaded" readable in the log,
	 * which is the confirmation the 08.25/07.56 blob swap needs.
	 */
	if (m->prim == DECT_PRIM_BUILD_STAMP && m->param_len) {
		printf("  \"");
		for (i = 0; i < m->param_len && i < sizeof(m->param); i++)
			putchar((m->param[i] >= 0x20 && m->param[i] < 0x7f)
				? (char)m->param[i] : '.');
		printf("\"");
	}
	printf("\n");
}

/*
 * What we can and cannot know about a command after we send it.
 *
 * The maintainer asked whether we check that anything we tell the chip
 * actually took effect. We do not, almost anywhere, and two findings make
 * that worse than it sounds: `[CR A-0070]` says the dispatcher SILENTLY DROPS
 * anything that is not a command, so a wrong primitive gives no error and no
 * reply; and `[CR A-0075]` says 0x0242 is emitted on the no-act early-out, so
 * the one reply we had been treating as a confirmation never was one.
 *
 * `[CR A-0086]` then settled how much of it matters, and the answer is: less
 * than it looks, with one exception.
 *
 *   - **The chip has no NAK and no error indication of any kind**, so a
 *     read-back is the only check that can exist. There is nothing to listen
 *     for, and we were not missing a mechanism.
 *   - **Stock does not verify either**, and stock has run this chip in the
 *     field for years -- but it periodically **re-asserts** most subscription
 *     state, so a silently lost write self-heals. That is how it gets away
 *     with it, and it is a better design than checking.
 *
 *     We re-assert too: the stored subscriptions go back into the chip at
 *     start-up, and 0x017C, the locked-in handset mask, follows every
 *     registration and deregistration [CR A-0129]. The subscription table --
 *     the thing that has actually caused us trouble -- gets an audit as well
 *     as the UAK read-back.
 *   - **The UAK is the exception**: written once at enrolment, never
 *     re-asserted, and it is the credential a handset needs to attach. Stock
 *     has that hole too.
 *
 * So we check the UAK (dect_base_uak_verify) and deliberately do not build
 * general command verification. This table stays because it keeps the
 * assumption visible -- a quiet log should not read as a verified one.
 */
enum verify_class {
	VERIFY_QUERY,		/* a read or poll: the reply IS the result */
	VERIFY_READBACK,	/* a state change we verify by reading it back */
	VERIFY_READABLE,	/* a state change, readable, that we do not check */
	VERIFY_NONE,		/* a state change with nothing to read */
};

static const struct {
	uint16_t prim;
	enum verify_class cls;
	const char *what;
	const char *note;
} verify_tbl[] = {
	{ 0xFFF1u, VERIFY_QUERY,     "memory read",            "the reply is the result" },
	{ 0xF624u, VERIFY_QUERY,     "status poll",            "answered by 0xF625" },
	{ 0x020Au, VERIFY_READBACK,  "subscription record",    "read back at 0x9518 + (slot-1)*0x11 [A-0085]" },
	{ 0x0247u, VERIFY_READBACK,  "subscription slot op",   "slot occupancy audited at 0x9518 after every clear" },
	{ 0x0230u, VERIFY_READABLE,  "assign audio channel",   "0x0242 is 'processed', NOT 'applied' [A-0075]; real gate is record +0x116 bit 5" },
	{ 0x016Au, VERIFY_READABLE,  "channel config",         "matrix is readable; we do not check" },
	{ 0x0251u, VERIFY_READABLE,  "set channel params",     "matrix is readable; we do not check" },
	{ 0x022Du, VERIFY_READABLE,  "audio path on",          "record +0x116 bit 6; we do not check" },
	{ 0x0233u, VERIFY_READABLE,  "audio disconnect",       "record +0x116 bit 6; we do not check" },
	{ 0x0208u, VERIFY_NONE,      "cipher enable",          "field mapping unproven [A-0028]" },
	{ 0x0300u, VERIFY_NONE,      "page a handset",         "a page response is a NWK event, not a confirmation we paged" },
	{ 0xF626u, VERIFY_NONE,      "DSP route",              "no known readable effect" },
	{ 0x015Bu, VERIFY_NONE,      "radio on/park",          "no known readable effect" },
	{ 0x022Bu, VERIFY_NONE,      "enable token",           "no known readable effect" },
	{ 0x022Cu, VERIFY_NONE,      "close registration",     "no known readable effect" },
	{ 0xF600u, VERIFY_QUERY,     "contact",                "answered by 0xF601" },
	{ 0x0244u, VERIFY_NONE,      "transmit C-plane",       "DLC acks the frame; the chip never says it sent it" },
};

#define VERIFY_TBL_N	(sizeof(verify_tbl) / sizeof(verify_tbl[0]))

/*
 * The tally lives on struct stubd, which is declared before this table, so
 * its size is written out by hand. Catch a mismatch at compile time rather
 * than overflowing it -- it would be a poor joke to get this wrong in the
 * change whose whole point is checking things.
 */
_Static_assert(VERIFY_TBL_N <= sizeof(((struct stubd *)0)->verify_n)
			       / sizeof(((struct stubd *)0)->verify_n[0]),
	       "verify_n[] is smaller than verify_tbl[]");

static void verify_count(struct stubd *s, uint16_t prim)
{
	size_t i;

	for (i = 0; i < VERIFY_TBL_N; i++) {
		if (verify_tbl[i].prim == prim) {
			if (s->verify_n[i] < UINT32_MAX)
				s->verify_n[i]++;
			return;
		}
	}
	if (s->verify_other < UINT32_MAX)
		s->verify_other++;
}

/*
 * One block per run. Turns "we never check anything" from a fact buried in
 * the source into a line somebody reads after every bench run.
 */
static void verify_report(const struct stubd *s)
{
	unsigned long reads = 0, changes = 0, verified = 0;
	size_t i;

	for (i = 0; i < VERIFY_TBL_N; i++) {
		switch (verify_tbl[i].cls) {
		case VERIFY_QUERY:
			reads += s->verify_n[i];
			break;
		case VERIFY_READBACK:
			changes += s->verify_n[i];
			if (s->stub_build == STUB_0825 || s->uak_0688)
				verified += s->verify_n[i];
			break;
		default:
			changes += s->verify_n[i];
			break;
		}
	}
	changes += s->verify_other;
	if (!reads && !changes)
		return;

	printf("chip: command verification summary\n");
	for (i = 0; i < VERIFY_TBL_N; i++) {
		if (!s->verify_n[i])
			continue;
		int ok = verify_tbl[i].cls == VERIFY_QUERY ||
			 (verify_tbl[i].cls == VERIFY_READBACK &&
			  (s->stub_build == STUB_0825 || s->uak_0688));

		printf("chip:   0x%04X x%-5u %-22s %s (%s)\n",
		       verify_tbl[i].prim, s->verify_n[i], verify_tbl[i].what,
		       ok ? "verified" : "NOT VERIFIED",
		       verify_tbl[i].cls == VERIFY_READBACK && !ok
			       ? "the read-back address is the 08.25 build's"
			       : verify_tbl[i].note);
	}
	if (s->verify_other)
		printf("chip:   (%u other command%s, not classified)\n",
		       s->verify_other, s->verify_other == 1 ? "" : "s");
	/*
	 * Count reads separately. Lumping them in reads as "88 % verified"
	 * when every state change is unchecked, which is the opposite of the
	 * truth. A summary that flatters is worse than no summary.
	 */
	/*
	 * Say whether anyone else was on the air. A run with another handset
	 * competing for this link cannot be read as a result about the handset
	 * we were serving, and the run should declare that itself rather than
	 * leave it to be reconstructed from the log afterwards.
	 */
	if (s->base.foreign_seg || s->base.foreign_conn)
		printf("reg: *** OTHER HANDSETS WERE ON AIR *** %lu C-plane "
		       "segment(s) for another handle (last: %u) and %lu "
		       "connection-up(s) refused. This link serves one "
		       "handset; a run with competing traffic is NOT a clean "
		       "measurement. Power the others off.\n",
		       (unsigned long)s->base.foreign_seg,
		       (unsigned)s->base.foreign_seg_handle,
		       (unsigned long)s->base.foreign_conn);
	printf("chip: %lu state-changing command%s sent, %lu verified by "
	       "read-back. %lu query/read%s.\n",
	       changes, changes == 1 ? "" : "s", verified,
	       reads, reads == 1 ? "" : "s");
	if (changes > verified)
		printf("chip: the other %lu were sent and assumed -- the chip "
		       "has no error indication to have missed [A-0086]. "
		       "See docs/verification-gap.md\n", changes - verified);
	fflush(stdout);
}

/* ---- SSC transport (7412): bit alignment and byte order ---- */

/* Both transports' block headers are read through the same code. */
_Static_assert(sizeof(struct ssc_pcm_block) == sizeof(struct dect_pcm_block),
	       "block headers differ in size");
_Static_assert(SSC_PCM_BF_DROPPED == DECT_PCM_BF_DROPPED,
	       "dropped flags differ");

static int pcm_block_ok(const struct stubd *s, const void *hdr)
{
	const struct dect_pcm_block *h = hdr;

	return h->magic == (s->ssc ? SSC_PCM_BLOCK_MAGIC : DECT_PCM_BLOCK_MAGIC);
}

/*
 * Receive, in place: the chip's lanes onto slot boundaries, each slot a
 * little-endian sample as in the 7520's DMA buffers (ssc_align.h). The
 * stream comes out one slot late; slot probing does not mind.
 */
static void ssc_rx_fix(struct stubd *s, uint8_t *fr, size_t len)
{
	if (!s->ssc || !s->ssc_aligned || len < 2u ||
	    len > s->slots * 2u * s->fpb)
		return;
	ssc_align_rx(&s->ssc_al, fr, s->ssc_rx_t, len);
	ssc_lane_delay_apply(&s->ssc_ld, fr, len);
}

/* "10,11,12" for the lane positions in mask; "-" for none. */
static void lanes_str(char *out, size_t cap, uint64_t mask)
{
	size_t n = 0;
	unsigned int i;

	out[0] = '\0';
	for (i = 0; i < 64u && n + 4u < cap; i++)
		if (mask & (1ull << i))
			n += (size_t)snprintf(out + n, cap - n, "%s%u",
					      n ? "," : "", i);
	if (!n && cap > 1u)
		snprintf(out, cap, "-");
}

/*
 * The SSC alignment watch, once a second on a raw block (before
 * ssc_rx_fix()): where the chip's idle lanes sit (ssc_idle_lanes()) and the
 * driver's loss counters. Prints the first measurement, any change that
 * holds for two checks, any lost data and every session death, with the
 * counter deltas since the last line. Reads only; changes nothing.
 *   lanes rotated, offset kept, overruns up -> the SSC lost words;
 *   offset changed, counters still          -> a bit slipped on the bus;
 *   lanes rotating step by step             -> the frame length is wrong;
 *   nothing moved at a death                -> the STUB stopped talking.
 */
static void ssc_watch(struct stubd *s, const uint8_t *raw, size_t len,
		      uint64_t now)
{
	struct ssc_pcm_stats st;
	char was[200], is[200];
	const char *why = NULL;
	uint64_t mask;
	int off, rot;

	if (!s->ssc || !s->slots || now < s->ssc_w_next)
		return;
	s->ssc_w_next = now + 1000u;
	off = ssc_idle_lanes(raw, len, s->slots, &mask);
	memset(&st, 0, sizeof(st));
	if (ioctl(s->fd, SSC_PCM_IOC_STATS, &st))
		return;
	if (s->ssc_w_off == -2) {
		lanes_str(is, sizeof(is), mask);
		printf("ssc: watch: idle lanes at bit %d, positions %s; "
		       "driver frames %llu overruns %u dropped %llu "
		       "tx_underruns %u\n", off, is,
		       (unsigned long long)st.frames, st.overruns,
		       (unsigned long long)st.dropped, st.tx_underruns);
		s->ssc_w_off = s->ssc_w_pend_off = off;
		s->ssc_w_mask = s->ssc_w_pend_mask = mask;
		s->ssc_w_st = st;
		s->ssc_w_deaths = s->sess.stats.deaths;
		return;
	}
	if (off != s->ssc_w_off || mask != s->ssc_w_mask) {
		/* one check is noise (a lane in use); two in a row are not */
		if (off == s->ssc_w_pend_off && mask == s->ssc_w_pend_mask)
			why = "LANES MOVED";
		s->ssc_w_pend_off = off;
		s->ssc_w_pend_mask = mask;
	} else {
		s->ssc_w_pend_off = off;
		s->ssc_w_pend_mask = mask;
	}
	if (!why && (st.overruns != s->ssc_w_st.overruns ||
		     st.dropped != s->ssc_w_st.dropped ||
		     st.tx_underruns != s->ssc_w_st.tx_underruns))
		why = "driver lost data";
	if (s->sess.stats.deaths != s->ssc_w_deaths)
		why = "STUB SILENT";
	if (!why)
		return;
	lanes_str(was, sizeof(was), s->ssc_w_mask);
	lanes_str(is, sizeof(is), mask);
	rot = ssc_lanes_rotation(s->ssc_w_mask, mask, s->slots);
	printf("ssc: watch: %s at %llu ms: bit %d -> %d, idle lanes %s -> %s "
	       "(%s%d); driver +%llu frames, overruns +%u, dropped +%llu, "
	       "tx_underruns +%u\n", why,
	       (unsigned long long)(now - g_start), s->ssc_w_off, off, was, is,
	       rot < 0 ? "no rotation " : "rotated by ", rot < 0 ? 0 : rot,
	       (unsigned long long)(st.frames - s->ssc_w_st.frames),
	       st.overruns - s->ssc_w_st.overruns,
	       (unsigned long long)(st.dropped - s->ssc_w_st.dropped),
	       st.tx_underruns - s->ssc_w_st.tx_underruns);
	fflush(stdout);
	s->ssc_w_off = off;
	s->ssc_w_mask = mask;
	s->ssc_w_st = st;
	s->ssc_w_deaths = s->sess.stats.deaths;
}

/*
 * Write whole frames. On the SSC transport, the inverse of ssc_rx_fix().
 * burst: a one-off write between the driver's idle frames (probing, before
 * the block loop runs); its head then comes from the idle frame, and one
 * idle frame is appended so the bits the shift pushes past the end are idle
 * bits too. Returns the bytes of frames taken, as write() would.
 */
static ssize_t pcm_write(struct stubd *s, const uint8_t *frames, size_t len,
			 int burst)
{
	size_t fb = s->slots * 2u, tail = burst ? fb : 0u, n = len + tail;
	ssize_t w;

	if (!s->ssc)
		return write(s->fd, frames, len);
	if (n > s->ssc_tx_cap) {
		errno = EMSGSIZE;
		return -1;
	}
	ssc_align_tx(&s->ssc_al, s->ssc_tx_out, s->ssc_tx_t, frames, len, tail,
		     burst);
	w = write(s->fd, s->ssc_tx_out, n);
	if (w > 0)
		ssc_align_tx_commit(&s->ssc_al, s->ssc_tx_t, (size_t)w);
	if (w > (ssize_t)len)
		w = (ssize_t)len;
	return w;
}

/*
 * Send a mail: encode the transmit element (libopendectcell), two bytes per frame
 * in the transmit control slot. Once run_loop() runs, the bytes join the
 * control-slot queue that tx_block() drains; before that (probing, bring-up)
 * they go out at once in a burst of frames, idle (0xFF) elsewhere.
 */
static int send_mail(struct stubd *s, unsigned int tx_slot,
		     const struct dect_mail *m)
{
	uint8_t el[DECT_TX_ELEM_MAX], *frames;
	size_t fb = s->slots * 2u, nf, k;
	int el_len = dect_mail_encode(m, s->tx_seq, el, sizeof(el));
	ssize_t w;

	if (el_len <= 0 || tx_slot >= s->slots)
		return -1;
	verify_count(s, (uint16_t)m->prim);
#ifdef STUBD_TEST
	stubd_test_sent(m);
	return 0;
#endif

	/*
	 * Once the ring is running every write goes through it, so the mail is
	 * appended to the control-slot byte queue and the block loop paces it
	 * out. Before that -- control-slot probing and bring-up -- there is no
	 * ring yet, so the original burst write is still used.
	 */
	if (s->tx_ring) {
		if (s->txq_len + (size_t)el_len > sizeof(s->txq)) {
			s->txq_drops++;
			return -1;
		}
		for (k = 0; k < (size_t)el_len; k++)
			s->txq[(s->txq_head + s->txq_len + k) % sizeof(s->txq)] =
				el[k];
		s->txq_len += (size_t)el_len;
		if (s->txq_len > s->txq_hwm)
			s->txq_hwm = s->txq_len;
		s->tx_seq++;
		return 0;
	}

	nf = (size_t)el_len / 2u;
	frames = malloc(nf * fb);
	if (!frames)
		return -1;
	memset(frames, 0xff, nf * fb);
	for (k = 0; k < nf; k++)
		memcpy(frames + k * fb + tx_slot * 2u, el + 2u * k, 2);
	w = pcm_write(s, frames, nf * fb, 1);
	free(frames);
	if (w != (ssize_t)(nf * fb))
		return -1;
	s->tx_seq++;
	return 0;
}

/*
 * A command to the chip from our PCM task, on the control slot: to task dst
 * (the contact task for every command but the 0xFFF1 debug requests), with
 * n parameter bytes p. Logged under -v; log_mail() masks key bytes.
 */
static void chip_mail_to(struct stubd *s, uint8_t dst, uint16_t prim,
			 const uint8_t *p, uint16_t n)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = dst;
	m.prim = prim;
	if (n > sizeof(m.param))
		n = sizeof(m.param);
	memcpy(m.param, p, n);
	m.param_len = n;
	(void)dect_base_send(&s->base, &m);	/* base_send() logs it */
}

static void chip_mail(struct stubd *s, uint16_t prim, const uint8_t *p,
		      uint16_t n)
{
	chip_mail_to(s, DECT_TASK_CONTACT_DST, prim, p, n);
}

/* Read n bytes of chip memory at addr; see dect_base_mem_read(). */
static void chip_mem_read(struct stubd *s, uint32_t addr, uint16_t n)
{
	dect_base_mem_read(&s->base, addr, n);
}

/*
 * 0x0230 ASSIGN_AUDIO_CHANNEL in its full form: {link, mode byte, TDM slot
 * mask (LE), master, extra} [CR A-CodecModes-1]; the -a keys call the mode
 * byte flags, the master byte mode and the extra byte codec.
 */
static void chip_assign(struct stubd *s, uint8_t handle, uint8_t flags,
			uint16_t mask, uint8_t mode, uint8_t codec)
{
	dect_base_audio_assign(&s->base, handle, flags, mask, mode, codec);
}

/* A received lane word that carries bearer data rather than a fill. */
static int lane_word_live(uint32_t v)
{
	return v <= 0xFFFFu && v != 0x0000u && v != 0xFEFEu && v != 0xFFFFu;
}

/* -------------------------------------------------------------------------
 * A bridged call's speech on its lane, frame by frame. Wideband, down: two
 * G.722 octets per two frames from the socket's FIFO, locked to the
 * handset's word changes as playsync locks the test file. Up: each of the
 * handset's coded words once -- a word is held two frames, a longer hold
 * is a chip underrun whose repeats would make the decoder re-adapt
 * (tools/dect_g722_extract.py) -- decoded per 20 ms and sent. Narrowband:
 * one linear sample per frame each way (clean room A-0042, 7412 A-0015).
 * Silence when the FIFO runs dry.
 */
#ifndef AB_PATH				/* tests/test_stubd.c sets its own */
#define AB_PATH		"/var/run/opendectcell-audio.sock"
#endif
#define AB_FRAME	640u	/* octets: 20 ms of 16-bit PCM at 16 kHz, the most */
#define AB_PRIME_MS	40u	/* buffered towards the handset before the first */
#define AB_HELLO_MS	2000u	/* a new connection names its call within */

static void ab_pop(struct ab_lane *l)
{
	size_t cap = sizeof(l->down);
	size_t prime = AB_PRIME_MS * (l->wideband ? 8u : 16u);

	if (l->dn >= 2 && (l->primed || l->dn >= prime)) {
		l->primed = 1;
		l->cur[0] = l->down[l->dh];
		l->cur[1] = l->down[(l->dh + 1u) % cap];
		l->dh = (l->dh + 2u) % cap;
		l->dn -= 2u;
		return;
	}
	if (l->wideband) {
		static const int16_t z[4];

		(void)dect_g722_encode(&l->sil, z, 4, l->cur);
	} else {
		l->cur[0] = l->cur[1] = 0;
	}
	if (l->primed)
		l->underruns++;
}

/*
 * One word from the handset; every 20 ms a frame to the bridge in the
 * socket's format. Nothing is decoded while no bridge is connected.
 */
static void ab_up(struct ab_lane *l, const uint8_t *w)
{
	int16_t wb[AB_FRAME / 2u], nb[AB_FRAME / 4u];
	uint8_t g[AB_FRAME / 4u];
	const void *out;
	size_t n, len;

	l->up[l->un++] = w[0];
	l->up[l->un++] = w[1];
	if (l->un < (l->wideband ? 160u : 320u))
		return;
	l->un = 0;
	if (l->cfd < 0)
		return;
	if (l->wideband && l->g722) {
		out = l->up;			/* the octets as they are */
		len = 160u;
		goto send;
	}
	/* linear: 16 kHz from a wideband lane, 8 kHz from a narrowband one */
	if (l->wideband) {
		n = dect_g722_decode(&l->dec, l->up, 160u, wb);
		out = wb;
	} else {
		unsigned i;

		for (i = 0; i < 160u; i++)
			nb[i] = (int16_t)(l->up[2u * i] |
					  (l->up[2u * i + 1u] << 8));
		n = 160u;
		out = nb;
	}
	if (l->wideband && l->rate == 8000u) {
		n = dect_rs2_down(&l->rs_up, wb, n, nb);
		out = nb;
	} else if (!l->wideband && l->rate != 8000u) {
		n = dect_rs2_up(&l->rs_up, nb, n, wb);
		out = wb;
	}
	len = n * sizeof(int16_t);
	if (l->g722) {
		len = dect_g722_encode(&l->sip_enc, out, n, g);
		out = g;
	}
send:
	if (send(l->cfd, out, len, MSG_DONTWAIT | MSG_NOSIGNAL) ==
	    (ssize_t)len)
		l->frames_up++;
	else
		l->dropped++;
}

/*
 * A frame from the bridge into the lane's own format, appended to
 * down[]: buf, len octets, in the socket's format.
 */
static void ab_down(struct ab_lane *l, const void *buf, size_t len)
{
	int16_t wb[AB_FRAME / 2u], nb[AB_FRAME / 4u];
	uint8_t d[AB_FRAME / 2u];
	const int16_t *pcm;
	size_t n, k, j, cap = sizeof(l->down);

	if (l->g722 && l->wideband) {
		memcpy(d, buf, len);		/* the octets as they are */
		k = len;
		goto queue;
	}
	if (l->g722) {
		n = dect_g722_decode(&l->sip_dec, buf, len, wb);
		pcm = wb;
	} else {
		n = len / sizeof(int16_t);
		pcm = buf;
	}
	/* the socket's rate (16 kHz for G.722) to the lane's */
	if (l->wideband && l->rate == 8000u && !l->g722) {
		n = dect_rs2_up(&l->rs_down, pcm, n, wb);
		pcm = wb;
	} else if (!l->wideband && (l->rate != 8000u || l->g722)) {
		n = dect_rs2_down(&l->rs_down, pcm, n, nb);
		pcm = nb;
	}
	if (l->wideband) {
		k = dect_g722_encode(&l->enc, pcm, n, d);
	} else {
		for (j = 0; j < n; j++) {
			d[2u * j] = (uint8_t)(pcm[j] & 0xFF);
			d[2u * j + 1u] = (uint8_t)((uint16_t)pcm[j] >> 8);
		}
		k = 2u * n;
	}
queue:
	if (l->dn + k > cap) {
		l->dropped++;		/* 1 s (wideband) or 0.25 s behind */
		return;
	}
	for (j = 0; j < k; j++)
		l->down[(l->dh + l->dn + j) % cap] = d[j];
	l->dn += k;
	l->frames_down++;
}

/* One frame of lane l, TDM word w: src is the handset's word, or NULL. */
static void ab_frame(struct ab_lane *l, uint8_t *fr, unsigned int w,
		     const uint8_t *src)
{
	if (!l->wideband) {
		if (src)
			ab_up(l, src);
		ab_pop(l);
	} else {
		if (src) {
			uint32_t v = (uint32_t)src[0] |
				     ((uint32_t)src[1] << 8);

			if (v == l->rx_prev) {
				if (++l->rx_run == 2u && lane_word_live(v))
					ab_up(l, src);
			} else {
				l->rx_run = 1;
				/* the first change between live words: lock */
				if (!l->locked && lane_word_live(v) &&
				    lane_word_live(l->rx_prev)) {
					l->locked = 1;
					l->phase = 0;
				}
			}
			l->rx_prev = v;
		}
		if (!l->phase)
			ab_pop(l);
		l->phase ^= 1;
	}
	fr[w * 2u] = l->cur[0];
	fr[w * 2u + 1u] = l->cur[1];
}

static int ic_word(const struct stubd *s, int side);

/* An internal call's other party's word for lane x, or -1. */
static int ic_peer_word(const struct stubd *s, const struct xlane *x)
{
	int c = s->base.ic.caller, t = s->base.ic.tside;

	if (s->base.ic.st != IC_UP)
		return -1;
	if (t >= 0 && s->side[t].bl->lane >= 0 && &s->xl[s->side[t].bl->lane] == x)
		return ic_word(s, c);
	if (c >= 0 && s->side[c].bl->lane >= 0 && &s->xl[s->side[c].bl->lane] == x)
		return ic_word(s, t);
	return -1;
}

/*
 * The extra lanes' downlink for frame f (-a twocalls): the bridge's speech;
 * an internal call's other party; the G.722 file phase-locked to the chip's
 * two-frame words as on lane 0; a raw (linear) file two bytes a frame; the
 * loop-back; else silence. Each lane keeps its own file position, so two
 * calls each hear the whole file.
 */
static void xlane_tx(struct stubd *s, uint8_t *fr, const uint8_t *rx,
		     unsigned int f)
{
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++) {
		struct xlane *x = &s->xl[i];
		unsigned w = x->word;
		const uint8_t *src;
		int peer;

		if (!x->used || !x->connected || w >= s->slots ||
		    w == (unsigned)s->tx_slot)
			continue;
		src = rx ? rx + (f * s->slots + w) * 2u : NULL;
		if (s->ab.l[1u + i].cfd >= 0 && s->ab.l[1u + i].call) {
			ab_frame(&s->ab.l[1u + i], fr, w, src);
			continue;
		}
		fr[w * 2u] = s->tx_voice_idle;
		fr[w * 2u + 1u] = s->tx_voice_idle;
		peer = rx ? ic_peer_word(s, x) : -1;
		if (peer >= 0) {
			/* the other handset's uplink, as it comes */
			const uint8_t *p = rx + (f * s->slots + (unsigned)peer) * 2u;

			fr[w * 2u] = p[0];
			fr[w * 2u + 1u] = p[1];
		} else if (s->audio.play && !s->audio.play_raw && x->wideband) {
			int advance;

			if (src) {
				uint32_t v = (uint32_t)src[0] |
					     ((uint32_t)src[1] << 8);

				if (v != x->rx_prev && lane_word_live(v) &&
				    lane_word_live(x->rx_prev) && !x->locked) {
					x->locked = 1;
					x->phase = 0;
				}
				x->rx_prev = v;
			}
			advance = !x->phase || !x->pos;
			x->phase ^= 1;
			if (advance) {
				if (x->pos + 2u > s->audio.play_len)
					x->pos = 0;
				x->pos += 2u;
			}
			fr[w * 2u] = s->audio.play[x->pos - 2u];
			fr[w * 2u + 1u] = s->audio.play[x->pos - 1u];
		} else if (s->audio.play && s->audio.play_raw && !x->wideband) {
			if (x->pos + 2u > s->audio.play_len)
				x->pos = 0;
			fr[w * 2u] = s->audio.play[x->pos];
			fr[w * 2u + 1u] = s->audio.play[x->pos + 1u];
			x->pos += 2u;
		} else if (s->audio.loopback && src) {
			fr[w * 2u] = src[0];
			fr[w * 2u + 1u] = src[1];
		}
	}
}

/*
 * The idle frames tx_block() starts every frame from: [0] while no call is
 * bound, every lane the transmit filler; [1] during a call, the voice lanes'
 * own filler, so a silent call sends silence rather than whatever the
 * control path uses. The control slot is idle (0xFF) in both. Their inputs
 * (the control slot, -a txidle and voiceidle) are fixed before run_loop()
 * starts, so it builds them once.
 */
static void tx_templates(const struct stubd *s, uint8_t *t)
{
	size_t fb = s->slots * 2u;
	unsigned int c = (unsigned int)s->tx_slot;

	memset(t, s->tx_idle, fb);
	memset(t + fb, s->tx_voice_idle, fb);
	if (s->tx_slot >= 0 && c < s->slots) {
		t[c * 2u] = t[c * 2u + 1u] = 0xFFu;
		t[fb + c * 2u] = t[fb + c * 2u + 1u] = 0xFFu;
	}
}

/*
 * What lane 0, the main link's call, carries towards the handset, in order
 * of precedence:
 *
 *   BRIDGE  the bridge's speech (docs/bridge-interface.md §3);
 *   PEER    an internal call's other party, its uplink word as it comes;
 *   RAW     -a playraw: a verbatim TDM lane, two bytes every frame;
 *   PLAY    -a play/playpcm: G.722, one coded word per two frames;
 *   LOOP    -a loopback: the handset's own word, straight back -- the
 *           lane carries the coded bearer, so this needs no encoder;
 *   IDLE    the voice filler.
 *
 * A narrowband call's lane stays silent until the mode-2 re-assign has gone
 * out: before it the chip is in mode 9, where linear PCM decodes as a burst
 * of noise. None of the inputs changes within a block, so tx_block() asks
 * once per block.
 */
enum lane0_src { L0_IDLE, L0_BRIDGE, L0_PEER, L0_RAW, L0_PLAY, L0_LOOP };

static void cell_media_tx(struct stubd *s, uint8_t *fr, const uint8_t *in);

static enum lane0_src lane0_source(const struct stubd *s, const uint8_t *rx,
				   unsigned int *peer)
{
	unsigned int w = s->audio.lane0_word;
	int mode2 = s->reg.call_wideband || !s->audio.remode_ms ||
		    s->reg.remode_done;

	if (!s->base.main_link.bound || w >= s->slots ||
	    w == (unsigned int)s->tx_slot)
		return L0_IDLE;
	if (s->ab.l[0].cfd >= 0 && s->ab.l[0].call)
		return mode2 ? L0_BRIDGE : L0_IDLE;
	if (s->base.ic.st == IC_UP && rx) {
		int p = ic_word(s, s->base.ic.caller == -1 ? s->base.ic.tside :
				   s->base.ic.tside == -1 ? s->base.ic.caller :
				   IC_PARTY_NONE);

		if (p >= 0) {
			*peer = (unsigned int)p;
			return L0_PEER;
		}
	}
	if (s->audio.play && s->audio.play_raw)
		return mode2 ? L0_RAW : L0_IDLE;
	if (s->audio.play)
		return L0_PLAY;
	if (s->audio.loopback && rx)
		return L0_LOOP;
	return L0_IDLE;
}

/*
 * One frame of -a play on lane 0: out is the lane's word in the frame we
 * send, in the received one (NULL when there is none).
 *
 * Lock the phase once, then free-run at two frames per word. The chip takes
 * the downlink at a steady 8000 octets/s, so the rate is ours to keep; what
 * we cannot know is which frame of each pair starts a word. The first change
 * between two live received words shows the chip's pair boundary. Both
 * directions run off one frame clock, so there is no drift to follow
 * afterwards, and following every transition would starve the lane whenever
 * the handset is silent. Idle fills are not live, or the lock would land on
 * a block boundary.
 */
static void lane0_play(struct stubd *s, uint8_t *out, const uint8_t *in)
{
	int advance;

	if ((s->audio.play_sync || s->reg.call_wideband) && in) {
		uint32_t v = (uint32_t)in[0] | ((uint32_t)in[1] << 8);

		if (v != s->rx_word_prev && lane_word_live(v) &&
		    lane_word_live(s->rx_word_prev) && !s->play_locked) {
			s->play_locked = 1;
			s->play_phase = 0;
		}
		s->rx_word_prev = v;
	}
	advance = !s->play_phase || !s->play_pos;
	s->play_phase ^= 1;
	if (advance) {
		if (s->play_pos + 2u > s->audio.play_len)
			s->play_pos = 0;
		s->play_pos += 2u;
	}
	out[0] = s->audio.play[s->play_pos - 2u];
	out[1] = s->audio.play[s->play_pos - 1u];
}

/*
 * Transmit one DMA block: every frame carries the control slot (the next two
 * queued mail bytes, or idle when the queue is empty), the calls' lanes, and
 * every other slot its idle value. Called once per received block (rx, the
 * same frames' receive side, or NULL), so the transmit side keeps pace with
 * the receive side without per-frame syscalls, which is the cadence A-0030
 * §2 Q1 describes ("refills occur per buffer, not per individual 8-kHz
 * frame").
 */
static void tx_block(struct stubd *s, unsigned int fpb, uint8_t *frames,
		     const uint8_t *rx)
{
	size_t fb = s->slots * 2u;
	unsigned int f, w = s->audio.lane0_word, peer = 0;
	const uint8_t *tmpl;
	enum lane0_src src0;
	/*
	 * Control-slot bytes are taken from a copy of the queue position and
	 * committed only for the frames the driver accepted, so a failed or
	 * short write loses no part of a mail. Frames take the queue in pairs
	 * from the first frame on, so the bytes owed for the first k accepted
	 * frames are min(2k, bytes taken).
	 */
	size_t qh = s->txq_head, ql = s->txq_len, taken, owed;
	ssize_t wr;

	if (s->tx_slot < 0)
		return;
	tmpl = s->tx_tmpl + (s->base.main_link.bound || s->cell.media_n ?
			     fb : 0u);
	src0 = lane0_source(s, rx, &peer);
	for (f = 0; f < fpb; f++) {
		uint8_t *fr = frames + f * fb;
		const uint8_t *in = rx ? rx + f * fb : NULL;

		memcpy(fr, tmpl, fb);
		switch (src0) {
		case L0_BRIDGE:
			ab_frame(&s->ab.l[0], fr, w, in ? in + w * 2u : NULL);
			break;
		case L0_PEER:
			fr[w * 2u] = in[peer * 2u];
			fr[w * 2u + 1u] = in[peer * 2u + 1u];
			break;
		case L0_RAW:
			if (s->play_pos + 2u > s->audio.play_len)
				s->play_pos = 0;
			fr[w * 2u] = s->audio.play[s->play_pos];
			fr[w * 2u + 1u] = s->audio.play[s->play_pos + 1u];
			s->play_pos += 2u;
			break;
		case L0_PLAY:
			lane0_play(s, fr + w * 2u, in ? in + w * 2u : NULL);
			break;
		case L0_LOOP: {
			uint16_t v = (uint16_t)(in[w * 2u] |
						(in[w * 2u + 1u] << 8));

			/*
			 * Only real bearer data: a routed but empty lane reads
			 * 0x0000 and an unrouted one the 0xFEFE fill, and
			 * feeding either back while the chip's path comes up
			 * flattened a lane that was coming alive. Until there
			 * is something real to send, send the filler.
			 */
			if (v != 0x0000u && v != 0xFEFEu) {
				fr[w * 2u] = in[w * 2u];
				fr[w * 2u + 1u] = in[w * 2u + 1u];
			}
			break;
		}
		case L0_IDLE:
			break;
		}
		xlane_tx(s, fr, rx, f);
		if (s->cell_mode)
			cell_media_tx(s, fr, in);
		if (ql >= 2u) {
			fr[(unsigned int)s->tx_slot * 2u] = s->txq[qh];
			fr[(unsigned int)s->tx_slot * 2u + 1u] =
				s->txq[(qh + 1u) % sizeof(s->txq)];
			qh = (qh + 2u) % sizeof(s->txq);
			ql -= 2u;
		}
	}
	taken = s->txq_len - ql;
	wr = pcm_write(s, frames, fpb * fb, 0);
	if (wr == (ssize_t)(fpb * fb)) {
		owed = taken;
	} else {
		owed = wr > 0 ? 2u * ((size_t)wr / fb) : 0u;
		if (owed > taken)
			owed = taken;
		s->tx_write_short++;
	}
	s->txq_head = (s->txq_head + owed) % sizeof(s->txq);
	s->txq_len -= owed;
	s->tx_blocks++;
}

/*
 * Parse "SRC:DST:PRIM[:HEX]" into a mail. HEX must be pairs of hex digits:
 * the loop used to step two characters at a time with "%2x", which takes
 * one digit as readily as two, so an odd-length string ("...:1") stepped
 * past the terminating NUL and read beyond it. Found by the second external
 * review, the clean-room record/an external review-1-24102026.md, finding 8.
 */
static int parse_mail_spec(const char *spec, struct dect_mail *m)
{
	unsigned long src, dst, prim;
	const char *par = NULL;
	char *end;
	size_t plen = 0, k;

	memset(m, 0, sizeof(*m));
	src = strtoul(spec, &end, 0);
	if (*end != ':')
		return -1;
	dst = strtoul(end + 1, &end, 0);
	if (*end != ':')
		return -1;
	prim = strtoul(end + 1, &end, 0);
	if (*end == ':')
		par = end + 1;
	else if (*end)
		return -1;
	if (src > 0xff || dst > 0xff || prim > 0xffff)
		return -1;
	if (par) {
		plen = strlen(par);
		if (plen % 2u || plen / 2u > DECT_MAIL_PARAM_MAX)
			return -1;
		for (k = 0; k < plen; k++)
			if (!isxdigit((unsigned char)par[k]))
				return -1;
	}
	m->src = (uint8_t)src;
	m->dst = (uint8_t)dst;
	m->prim = (uint16_t)prim;
	for (k = 0; k < plen; k += 2) {
		unsigned int v;

		if (sscanf(par + k, "%2x", &v) != 1)
			return -1;
		m->param[m->param_len++] = (uint8_t)v;
	}
	return 0;
}

/* Parse a mail spec and send it in the control slot. */
static int send_mail_spec(struct stubd *s, const char *spec)
{
	struct dect_mail m;

	if (parse_mail_spec(spec, &m))
		return -1;
	if (send_mail(s, (unsigned int)s->tx_slot, &m))
		return -1;
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	return 0;
}

/*
 * Read up to max_blocks of frame data into buf (block payload only), in the
 * 7520's layout once the SSC transport is aligned.
 */
static ssize_t read_frames(struct stubd *s, unsigned int max_blocks,
			   uint8_t *buf, size_t cap, unsigned int timeout_ms)
{
	int fd = s->fd;
	size_t block = s->slots * 2u * s->fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *tmp = malloc(slot * 16u);
	size_t got = 0;
	unsigned int blocks = 0;

	if (!tmp)
		return -1;
	while (blocks < max_blocks && !stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		size_t off;
		int pr = poll(&pfd, 1, (int)timeout_ms);

		if (pr < 0 && errno == EINTR)
			break;
		if (pr <= 0)
			break;
		n = read(fd, tmp, slot * 16u);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			free(tmp);
			return -1;
		}
		for (off = 0; off + slot <= (size_t)n && blocks < max_blocks;
		     off += slot) {
			const struct dect_pcm_block *h = (const void *)(tmp + off);

			if (!pcm_block_ok(s, h) || h->len != block)
				continue;
			if (got + block > cap)
				goto done;
			memcpy(buf + got, tmp + off + sizeof(*h), block);
			ssc_rx_fix(s, buf + got, block);
			got += block;
			blocks++;
		}
	}
done:
	free(tmp);
	return (ssize_t)got;
}

/*
 * Does the byte stream of one slot in frame data hold a contact answer?
 * Every mail in the window counts, not just the first: on a freshly booted
 * chip the answer comes right after the 0xFFF2 build stamp
 * (docs/stub-pcm-control-channel.md), and a search that looked at the
 * first mail only never found the 7362 SL's control slot after an upload
 * (2026-11-01, docs/bench-7362sl-g2.md). Pulling as we push keeps a window
 * longer than the receive buffer (DECT_RX_BUF) whole.
 */
static int slot_has_contact_ack(const uint8_t *data, size_t frames,
				unsigned int slots, unsigned int slot,
				struct dect_mail *out)
{
	struct dect_rx rx;
	uint8_t pair[2];
	size_t f;

	dect_rx_init(&rx);
	for (f = 0; f < frames; f++) {
		memcpy(pair, data + (f * slots + slot) * 2u, 2);
		dect_rx_push(&rx, pair, 2);
		while (dect_rx_pull(&rx, out))
			if (out->prim == DECT_PRIM_CONTACT_ACK1 ||
			    out->prim == DECT_PRIM_CONTACT_ACK2)
				return 1;
	}
	return 0;
}

/*
 * Find the control slot pair by probing: send the contact in each transmit
 * slot, then look for a valid STUB answer in any receive slot. Safe because
 * the STUB ignores the contact in every slot but its control slot
 * (docs/stub-pcm-control-channel.md). Sets s->tx_slot and s->rx_slot.
 */
static int find_control_slot(struct stubd *s, unsigned int fpb)
{
	size_t block = s->slots * 2u * fpb;
	size_t cap = block * PROBE_BLOCKS;
	uint8_t *data = malloc(cap);
	unsigned int tx, rx;
	int ret = -1;

	if (!data)
		return -1;
	for (tx = 0; tx < s->slots && !stop; tx++) {
		struct dect_mail cm, ans;
		ssize_t n;
		size_t frames;

		dect_session_contact_mail(&s->sess, &cm);
		if (send_mail(s, tx, &cm)) {
			if (s->verbose)
				fprintf(stderr, "probe: send in slot %u failed\n", tx);
			continue;
		}
		n = read_frames(s, PROBE_BLOCKS, data, cap, 500);
		if (n <= 0)
			continue;
		frames = (size_t)n / (s->slots * 2u);
		for (rx = 0; rx < s->slots; rx++) {
			/*
			 * Only a contact ACK proves the chip read our mail on
			 * THIS tx slot. The chip also emits unsolicited
			 * reports on its own schedule -- 0xFFF1 statistics
			 * and 0xF618 every 5 s, 0xF623 scan reports -- so
			 * accepting any mail at all lets one of those land
			 * inside the 500 ms probe window and lock the search
			 * onto a slot the chip never read. The daemon then
			 * runs with a transmit slot nothing listens to: mails
			 * go nowhere, the radio is never brought up, and the
			 * handset cannot see the base, while the log still
			 * says "control slot found" and the chip's periodic
			 * reports keep arriving. That is 2026-10-23 vF46 --
			 * "tx 0, rx 1 (answer prim fff1)", session contacts 3
			 * answers 0 -- against the working "tx 1, rx 1
			 * (answer prim f601)" of the day before.
			 */
			if (!slot_has_contact_ack(data, frames, s->slots, rx,
						  &ans))
				continue;
			s->tx_slot = (int)tx;
			s->rx_slot = (int)rx;
			printf("control slot found: tx %u, rx %u (answer prim %04x)\n",
			       tx, rx, ans.prim);
			/*
			 * This answer carries the RFPI the chip broadcasts, and
			 * the run loop never sees it again: keep it for the
			 * controller (STATUS tag 6, HELLO tag 17).
			 */
			if (ans.prim == DECT_PRIM_CONTACT_ACK1 && ans.param_len >= 5u) {
				memcpy(s->cell.rfpi, ans.param, sizeof(s->cell.rfpi));
				s->cell.have_rfpi = 1;
				printf("chip: broadcasts RFPI %02x%02x%02x%02x%02x\n",
				       s->cell.rfpi[0], s->cell.rfpi[1], s->cell.rfpi[2],
				       s->cell.rfpi[3], s->cell.rfpi[4]);
			}
			ret = 0;
			goto out;
		}
	}
out:
	free(data);
	return ret;
}

/*
 * Measure the bit offset of the chip's 16-bit lanes in the SSC's stream:
 * the offset at which the most lanes read 0xFEFE, the chip's idle pattern
 * (three such lanes per frame on the 7412's 06.88 STUB in mode 0x119,
 * observed 2026-10-29). Where two idle lanes are adjacent the pattern also
 * matches 8 bits later (two per frame there), so the control-slot search
 * retries at offset + 8 before it gives up (ssc_flip()).
 */
static int ssc_measure(struct stubd *s)
{
	size_t block = s->slots * 2u * s->fpb, cap = block * 16u;
	uint8_t *d = malloc(cap);
	unsigned int cnt[16], best;
	ssize_t n = 0;
	int tries, o;

	if (!d)
		return -1;
	/* The chip clocks the bus once its STUB runs; give it 10 s. */
	for (tries = 0; tries < 50 && n <= 0 && !stop; tries++)
		n = read_frames(s, 16, d, cap, 200);
	if (n <= 0) {
		fprintf(stderr, "ssc: no frames -- the chip is not clocking "
			"the bus\n");
		free(d);
		return -1;
	}
	o = ssc_idle_offset(d, (size_t)n, cnt);
	free(d);
	if (o < 0) {
		fprintf(stderr, "ssc: no idle lane (0xFEFE) in %zd bytes -- "
			"is the STUB running?\n", n);
		return -1;
	}
	best = (unsigned int)o;
	ssc_align_init(&s->ssc_al, best);
	s->ssc_aligned = 1;
	printf("ssc: lane bit offset %u (%u idle lanes; %u at offset %u)\n",
	       best, cnt[best], cnt[(best + 8u) % 16u], (best + 8u) % 16u);
	return 0;
}

/* The other offset at which the idle pattern reads (see ssc_measure()). */
static void ssc_flip(struct stubd *s)
{
	ssc_align_init(&s->ssc_al, s->ssc_al.off + 8u);
	printf("ssc: trying lane bit offset %u\n", s->ssc_al.off);
}

#define SSC_N_DESC	16u	/* 64 ms of blocks queued for read() */

/*
 * The generic SSC PCM transport (7412). The chip's frame has cfg->slots
 * 16-bit slots (-c; 36 on the 7412 in mode 0x119). Starting it after
 * dect-fwload, with the chip already clocking, works as well as starting
 * it before; only the bit offset is new each time (observed on a 7412,
 * 2026-10-29), and ssc_measure() finds it.
 */
static int ssc_bring_up(struct stubd *s, struct dect_pcm_config *cfg,
			const struct ssc_pcm_info *si)
{
	struct ssc_pcm_config sc;
	struct ssc_pcm_tx_idle idle;
	unsigned int step;

	memset(&sc, 0, sizeof(sc));
	sc.slots = cfg->slots;
	sc.frames_per_block = cfg->frames_per_block;
	sc.n_desc = cfg->n_desc ? cfg->n_desc : SSC_N_DESC;
	if (ioctl(s->fd, SSC_PCM_IOC_CONFIG, &sc)) {
		perror("SSC_PCM_CONFIG");
		return -1;
	}
	s->ssc = 1;
	cfg->slots = sc.slots;
	cfg->frames_per_block = sc.frames_per_block;
	cfg->n_desc = sc.n_desc;
	s->slots = sc.slots;
	s->fpb = sc.frames_per_block;
	/* a block, or the longest mail's burst (send_mail()) and its idle tail */
	s->ssc_tx_cap = (size_t)s->slots * 2u *
			(s->fpb > DECT_TX_ELEM_MAX / 2u + 1u ? s->fpb
			 : DECT_TX_ELEM_MAX / 2u + 1u);
	s->ssc_rx_t = malloc((size_t)s->slots * 2u * s->fpb + 2u);
	s->ssc_tx_t = malloc(s->ssc_tx_cap + 2u);
	s->ssc_tx_out = malloc(s->ssc_tx_cap);
	if (!s->ssc_rx_t || !s->ssc_tx_t || !s->ssc_tx_out) {
		fprintf(stderr, "ssc: out of memory\n");
		return -1;
	}
	printf("config: %.32s (uapi %u), slots %u frames %u desc %u tx\n",
	       si->name, si->version, sc.slots, sc.frames_per_block, sc.n_desc);
	/* What the driver sends when no frame is queued: all ones, idle. */
	memset(&idle, 0xff, sizeof(idle));
	if (ioctl(s->fd, SSC_PCM_IOC_TX_IDLE, &idle)) {
		perror("SSC_PCM_TX_IDLE");
		return -1;
	}
	for (step = 1; step <= SSC_PCM_STEP_DMA; step++) {
		if (ioctl(s->fd, SSC_PCM_IOC_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			return -1;
		}
	}
	return ssc_measure(s);
}

static int bring_up(struct stubd *s, struct dect_pcm_config *cfg)
{
	struct dect_ioc_info info;
	struct ssc_pcm_info si;
	unsigned int step;

	memset(&info, 0, sizeof(info));
	memset(&si, 0, sizeof(si));
	if (ioctl(s->fd, DECT_IOC_INFO, &info) &&
	    !ioctl(s->fd, SSC_PCM_IOC_INFO, &si))
		return ssc_bring_up(s, cfg, &si);
	if (info.version < 2 || !(info.features & DECT_FEATURE_PCM_RX)) {
		fprintf(stderr, "module without PCM receive (version %u)\n",
			info.version);
		return -1;
	}
	cfg->flags |= DECT_PCM_CF_TX;
	if (ioctl(s->fd, DECT_IOC_PCM_CONFIG, cfg)) {
		perror("PCM_CONFIG");
		return -1;
	}
	s->slots = cfg->slots;
	s->fpb = cfg->frames_per_block;
	printf("config: slots %u rx_delay %u tx_delay %u frames %u desc %u tx\n",
	       cfg->slots, cfg->rx_delay, cfg->tx_delay, cfg->frames_per_block,
	       cfg->n_desc);
	for (step = 1; step <= DECT_PCM_STEP_DMA; step++) {
		if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step)) {
			fprintf(stderr, "step %u: %s\n", step, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void tear_down(struct stubd *s)
{
	unsigned int step = DECT_PCM_STEP_OFF;

	if (s->ssc) {
		step = SSC_PCM_STEP_OFF;
		if (ioctl(s->fd, SSC_PCM_IOC_STEP, &step))
			perror("SSC_PCM_STEP 0");
		free(s->ssc_rx_t);
		free(s->ssc_tx_t);
		free(s->ssc_tx_out);
		return;
	}
	if (ioctl(s->fd, DECT_IOC_PCM_STEP, &step))
		perror("PCM_STEP 0");
}

/* ---- GAP registration: drive the host C-plane stack over the STUB ---- */

/* Fill buf with n random bytes (urandom, with a weak clock fallback). */
static void gen_rand(struct stubd *s, uint8_t *buf, size_t n)
{
	if (s->rand_fd >= 0 && read(s->rand_fd, buf, n) == (ssize_t)n)
		return;
	{
		uint64_t x = mono_ms() * 0x9E3779B97F4A7C15ull;
		size_t i;

		for (i = 0; i < n; i++) {
			x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
			buf[i] = (uint8_t)x;
		}
	}
}


/*
 * Host-side subscription store.
 *
 * The chip's subscription is volatile: it is lost whenever the radio is parked,
 * so AVM keeps the record host-side and reloads it into the chip at start-up
 * [CR A-0026]. We do the same with one small file, which also gives call-time
 * ciphering the UAK after a daemon restart (registration and a later call run on
 * different connections). The file holds long-term key material, so it is
 * created 0600 and must never leave the box.
 */
#define SUB_MAGIC	0x44535531u	/* "DSU1" */
#define SUB_MAGIC2	0x44535532u	/* "DSU2": adds the identities below */

/* The original layout, so files written before version 2 still load. */
struct sub_record_v1 {
	uint32_t magic;
	uint8_t  slot;
	uint8_t  uak[DECT_MM_UAK_LEN];
	uint8_t  port_id_len;
	uint8_t  port_id[DECT_MM_PORTID_MAX];
};

/*
 * Version 2 adds the two identity IEs an FP-originated SETUP must carry, so a
 * call *to* the handset does not depend on the handset having called us first
 * in this session. They are copied verbatim from a CC-SETUP the handset sent:
 * their encodings (identity type, bit length, octet padding) are easy to get
 * wrong and the handset puts the authoritative form on the wire.
 *
 * A version 1 file still loads; it simply has no identities, and they are
 * learned and saved the first time the handset places a call.
 */
struct sub_record {
	uint32_t magic;
	uint8_t  slot;
	uint8_t  uak[DECT_MM_UAK_LEN];
	uint8_t  port_id_len;
	uint8_t  port_id[DECT_MM_PORTID_MAX];
	uint8_t  cc_port_id_len;
	uint8_t  cc_port_id[DECT_CC_ID_MAX];
	uint8_t  cc_fixed_id_len;
	uint8_t  cc_fixed_id[DECT_CC_ID_MAX];
};

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
static uint8_t stubd_sub_slot(const struct stubd *s)
{
	return (uint8_t)dect_base_slot(&s->base, s->base.cur);
}

/* Handset i became current: its file, and its warnings not given yet. */
static void hs_took(struct stubd *s, unsigned i)
{
	s->sub_path = s->hs[i].path;
}

/* Handset i's record as the working copy, at start (dect_base_load()). */
static void hs_load(struct stubd *s, unsigned i)
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

static void send_subscription(struct stubd *s);

/* dect_base's install(): every handset this box serves (send_subscription()). */
static void base_install(void *ctx)
{
	send_subscription(ctx);
}

static void ctl_send_bridge(struct stubd *s, struct dect_ctl_out *o);

/* dect_base's event(): to the bridge's client. */
static void base_event(void *ctx, struct dect_ctl_out *o)
{
	ctl_send_bridge(ctx, o);
}

static void pump_lapc(struct stubd *s);
static void side_pump(struct stubd *s, struct side_link *sl);

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

static int lanes_available(const struct stubd *s, const struct side_link *me);

/* dect_base's lanes(): the audio channels a new call can count on. */
static int base_lanes(void *ctx, int p)
{
	struct stubd *s = ctx;

	return lanes_available(s, p >= 0 ? &s->side[p] : NULL);
}

static void base_forget(void *ctx, unsigned i, const char *why);
static void base_mask(void *ctx);
static int base_clock(void *ctx, struct dect_time *dt, long *off_s);
static void base_named(void *ctx, unsigned i);
static void base_switched(void *ctx, const char *why);
static void base_link_up(void *ctx, int p);
static void base_side_down(void *ctx, unsigned k);
static void base_registered(void *ctx, uint64_t now);
static void base_call_up(void *ctx, uint64_t now);
static void base_side_call(void *ctx, unsigned k, uint64_t now);
static void base_swapped(void *ctx, unsigned k);
static void base_cap_learned(void *ctx, unsigned i);
static void base_call_down(void *ctx);
static void base_wall(void *ctx, struct dect_time *dt);

static const struct dect_base_ops base_ops = {
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
static int sub_load(struct stubd *s)
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

static unsigned hs_slot(const struct stubd *s, unsigned i);
static void hs_name(const struct stubd *s, unsigned i, char *buf, size_t cap);

/*
 * holdback=MS (docs/multicell-experiments.md, test M2): every store but the
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

static void send_subscription_tail(struct stubd *s);
static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN]);

static void pump_lapc(struct stubd *s);

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
 * Test M4 (docs/multicell-experiments.md): play the controller for a
 * roamer. MM holds a {LOCATE-REQUEST} from an identity this link does not
 * know; if it is the remote= store's, its record "arrives" fetchdelay ms
 * after the request, and the request is answered. Times are from the
 * request, so the handset's patience is measured, not ours.
 */
static void fetch_tick(struct stubd *s, uint64_t now)
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

static int ic_start(struct stubd *s, int side, uint64_t now);

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

static void cap_load(struct stubd *s, unsigned i)
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
static void cap_tick(struct stubd *s, struct dect_link *l)
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
static void lapc_failed_note(struct stubd *s, struct dect_link *l,
			     const char *which)
{
	dect_base_lapc_note(&s->base, l, which);
	fflush(stdout);
}

/* -a nlr: the release after a call is the library's (dect_base.h, S3u). */
static void nlr_tick(struct stubd *s, uint64_t now)
{
	dect_base_registration_tick(&s->base, now);
	dect_base_nlr_tick(&s->base, now);
	fflush(stdout);
}

/* The main link's call rings: the name, or infoie= (dect_base.h, S3r). */
static void info_tick(struct stubd *s, uint64_t now)
{
	dect_base_info_tick(&s->base, now);
}

static int bridge_call_out(struct stubd *s, int p, uint64_t now);

/* Authenticate first (dect_base_call_auth_ok(), S3p). */
static int call_auth_ok(const struct stubd *s, const struct dect_link *l)
{
	return dect_base_call_auth_ok(&s->base, l);
}

/*
 * Overlap receiving: accept a handset-placed call once its digits have
 * been quiet for dialwait ms. Stands in for a dial plan or the
 * controller, which would decide on the number instead.
 */
static void dial_tick(struct stubd *s, uint64_t now)
{
	dect_base_main_dial_tick(&s->base, now);
	fflush(stdout);
}

/* alertms / bindfirst: send the CONNECT once its conditions hold. */
static void connect_tick(struct stubd *s, uint64_t now)
{
	dect_base_connect_tick(&s->base, now);
}

static void holdback_tick(struct stubd *s, uint64_t now)
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
static int any_uak(const struct stubd *s)
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
static void send_subscription(struct stubd *s)
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

static void send_subscription_tail(struct stubd *s)
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

/*
 * The 7412's 06.88 STUB: what a park leaves in RAM [CR A-0009, A-0011]. The
 * radio switch 0x010A reads 0 and the beacon timer's link 0x08B0 links to
 * itself (0) once 0x015B 00 has cancelled it; the beacon carrier 0x010C does
 * not tell, it keeps its last value. Both checked on a 7412, 2026-10-29.
 */
/*
 * The park done, for the service's stop (opendectcell.init, park_guard): read
 * back on the chip, or, for a build we cannot read back, the park mails on
 * the wire. Without this file the stop holds the chip in reset: the daemon
 * crashed, was killed, or the chip said the radio is still on.
 */
#define PARK_RECORD		DECT_RUN_DIR "/parked"

static FILE *fopen_private(const char *path);

static void park_record(const char *how)
{
	FILE *f = fopen_private(PARK_RECORD);

	if (!f || fprintf(f, "%s\n", how) < 0 || fclose(f))
		fprintf(stderr, "reg: %s not written\n", PARK_RECORD);
}

#define PARK_0688_RADIO		0x010Au
#define PARK_0688_TIMER		0x08B0u
#define PARK_CHECK_MS		1000u

static void park_read(struct stubd *s, uint32_t addr)
{
	chip_mem_read(s, addr, 1);
}

/* After the park mails are out: read the park back, where we know how. */
static int park_check_start(struct stubd *s, uint64_t now)
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
static int park_check_done(struct stubd *s, uint64_t now)
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
static void mem_reply(struct stubd *s, const struct dect_mail *m)
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

/*
 * Bind a connected call's voice path, in the order AVM's own bind routine uses
 * [CR A-0029 §1]: 0x0251 SET_CHANNEL_PARAM first, then 0x0230 in its full
 * six-parameter form. 0x0251's third byte is an EEPROM-derived value A-0029
 * could not recover, so -a gain=N sets it. 0x022D, which connects the path,
 * follows once the chip has answered the assign with 0x0242 (see the 0x0242
 * case in handle_reg_mail(), and audio_connect_fallback()): the assignment
 * is asynchronous, and AVM sends 0x022D from state-dependent paths rather
 * than as a fixed third mail [CR A-0029 §2, A-0030 §2 Q6].
 */
static void send_audio_bind(struct stubd *s, uint8_t handle)
{
	const struct audio_cfg *a = &s->audio;
	/* a wideband call: stock's bind, flags 0x41 and codec byte 0 */
	int wb = s->reg.call_wideband;
	uint8_t flags = wb ? (uint8_t)(a->flags | 0x40u) : a->flags;
	uint8_t codec = wb ? 0u : a->codec;

	/* selector valid 1..6 [CR A-CodecModes-3]; the STUB stores gain << 8 */
	dect_base_audio_param(&s->base, handle, a->sel, a->gain);
	chip_assign(s, handle, flags, a->mask, a->mode, codec);
	printf("call: audio assign: 0x0251 (sel %u gain 0x%02x), "
	       "0x0230 {%02x %04x %02x %02x} -- 0x022D waits for 0x0242\n",
	       a->sel, a->gain, flags, a->mask, a->mode, codec);
}

/*
 * 0x0233 AUDIO_DISCONNECT -- the clean inverse of 0x0230 [CR A-0070].
 *
 * Sent when a call's CC ends, so the next call does not start on top of the
 * last one's audio state. The chip's handler runs the configurator in mode 0
 * (release) and clears the path-on bit (record +0x116 b6).
 *
 * [CR A-0070] §4 gives the release order: 0x0233 first, then 0x0202 once the
 * DLC is quiescent. This is the audio half.
 */
static void send_audio_disconnect(struct stubd *s, uint8_t handle)
{
	dect_base_audio_release(&s->base, handle);
	printf("call: audio disconnected (0x0233 on handle %u)\n", handle);
}

/*
 * Connect the assigned channel: 0x022D with bit 7 clear. `chan` is the
 * channel the chip confirmed in 0x0242 -- A-0032 Q9: trust the returned
 * channel, not the one we asked for.
 *
 * CAVEAT [CR A-0075 §0]: "confirmed" is too strong. 0x0242 is
 * emitted on EVERY path through the handler, including the early-out that
 * does nothing -- it means "mail processed", not "matrix changed". vF418 has
 * six byte-identical 0x0242 replies to sends that changed nothing, next to
 * one that configured the matrix. The real act/no-act gate is the
 * connection's byte 0/byte 1 plus record +0x116 bit 5. We keep using the
 * returned channel because it is still the channel, but nothing here should
 * be read as evidence that a 0x0230 landed.
 */
static void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan)
{
	const struct audio_cfg *a = &s->audio;

	if (s->base.main_link.path_on)
		return;
	s->base.main_link.path_on = 1;
	s->reg.remode_handle = handle;
	if (a->remode_ms)
		s->reg.remode_at = mono_ms() + a->remode_ms;
	dect_base_audio_path(&s->base, handle);	/* AUDIO_PATH_ON_OFF */
	printf("call: audio connect: 0x022D on channel %u\n", chan);
}

/*
 * Fallback: if the chip never confirms the assignment, connect anyway after
 * half a second so the deferred order can never do worse than the old one.
 */
static void audio_connect_fallback(struct stubd *s, uint64_t now)
{
	if (!s->base.main_link.bound || s->base.main_link.path_on || !s->base.main_link.active)
		return;
	if (now - s->reg.bind_ms < 500u)
		return;
	printf("call: audio connect: no 0x0242 within 500 ms, connecting anyway\n");
	send_audio_connect(s, s->base.main_link.handle, s->audio.flags & 0x0Fu);
}

/*
 * Second 0x0230 with flag bit 6 clear, once the path is on: asks the chip's
 * channel configurator for mode 2 instead of the mode 1 our bind leaves it
 * in. [CR A-0039]
 */
static void remode_tick(struct stubd *s, uint64_t now)
{
	const struct audio_cfg *a = &s->audio;
	uint8_t flags = (uint8_t)(a->flags & ~0x40u);

	if (!s->reg.remode_at || now < s->reg.remode_at)
		return;
	s->reg.remode_at = 0;
	s->reg.remode_done = 1;

	chip_assign(s, s->reg.remode_handle, flags, a->mask, a->mode, a->codec);
	printf("call: re-assign for mode 2: 0x0230 {%02x %04x %02x %02x}\n",
	       flags, a->mask, a->mode, a->codec);
	fflush(stdout);
}

/*
 * Walk the seqstep list, one 0x0230 per window. See audio_cfg.seq_ms. The
 * handle is the call's own (remode_handle, set with its 0x022D), never a
 * literal: handle 0 is a valid handle.
 */
static void seq_tick(struct stubd *s, uint64_t now)
{
	const struct audio_cfg *a = &s->audio;
	unsigned int flags, codec;

	if (!a->seq_ms || !s->reg.seq_t0 || s->reg.seq_idx >= a->n_seq)
		return;
	if (now < s->reg.seq_t0 +
		  (uint64_t)(s->reg.seq_idx + 1u) * a->seq_ms)
		return;

	if (sscanf(a->seq_step[s->reg.seq_idx], "%x:%x", &flags, &codec) != 2) {
		printf("seq: step %u (%s) malformed, skipped\n",
		       s->reg.seq_idx, a->seq_step[s->reg.seq_idx]);
		s->reg.seq_idx++;
		return;
	}

	chip_assign(s, s->reg.remode_handle, (uint8_t)flags, a->mask, a->mode,
		    (uint8_t)codec);
	printf("call: seq window %u begins (t+%u ms): 0x0230 {%02x %04x %02x %02x}\n",
	       s->reg.seq_idx + 1u, (unsigned)((s->reg.seq_idx + 1u) * a->seq_ms),
	       flags & 0xFFu, a->mask, a->mode, codec & 0xFFu);
	fflush(stdout);
	s->reg.seq_idx++;
}

/*
 * -a memdump: one chip-memory read per tick, cycling through the configured
 * ranges. The reply (0xFFF1 sub 0x08) carries the address and the bytes and
 * is printed by the ordinary mail log (tools/dect_memdump.py reads it), so
 * no decoding lives here.
 */
static void mem_dump_tick(struct stubd *s, uint64_t now)
{
	uint32_t addr, left;
	uint16_t count;

	if (!s->audio.n_memdump)
		return;
	if (s->md_at && now - s->md_at < s->audio.memdump_ms)
		return;
	s->md_at = now;

	addr = s->audio.memdump[s->md_idx].addr + s->md_off;
	left = s->audio.memdump[s->md_idx].len - s->md_off;
	count = (uint16_t)(left > 32u ? 32u : left);

	chip_mem_read(s, addr, count);

	s->md_off += count;
	if (s->md_off >= s->audio.memdump[s->md_idx].len) {
		s->md_off = 0;
		if (++s->md_idx >= s->audio.n_memdump) {
			s->md_idx = 0;
			printf("mem: sweep %u complete at t=%llu ms%s\n",
			       ++s->md_pass,
			       (unsigned long long)(now - g_start),
			       s->base.main_link.bound ? " (call bound)" : " (idle)");
			fflush(stdout);
		}
	}
}

/*
 * Ask the STUB for a link's connection status: 0xF624 {link, 0x0A}, answered
 * by 0xF625 with a 0x2D-byte status record (report_conn_status()). AVM's host
 * driver polls this every 30 s for its connection monitor [CR the
 * hardware-analysis notes §6 1.5, §5 host mail 0xF611]. A pure read, so it
 * changes nothing on the air and is safe during a call.
 */
static void send_conn_status_one(struct stubd *s, uint8_t handle)
{
	/* 0x0A: the selector AVM's monitor uses */
	const uint8_t p[2] = { handle, 0x0Au };

	chip_mail(s, 0xF624u, p, sizeof(p));
}

/* -a status=MS: poll every speech call's link, the main one and the sides. */
static void send_conn_status_req(struct stubd *s, uint64_t now)
{
	unsigned k;

	/* a side link's speech call too, for the Monitor (round 4, L3) */
	for (k = 0; s->audio.status_ms && k < SIDE_MAX; k++) {
		struct side_link *sl = &s->side[k];

		if (!sl->bl->active || sl->bl->lane < 0 ||
		    (sl->status_ms && now - sl->status_ms < s->audio.status_ms))
			continue;
		sl->status_ms = now;
		send_conn_status_one(s, sl->bl->handle);
	}
	if (!s->audio.status_ms || !s->base.main_link.bound || !s->base.main_link.active)
		return;
	if (s->reg.status_ms && now - s->reg.status_ms < s->audio.status_ms)
		return;
	s->reg.status_ms = now;
	send_conn_status_one(s, s->base.main_link.handle);
}

/*
 * Page the handset so it brings up a link we can send a CC-SETUP on.
 *
 * 0x0300 carries {page info, 0x11, slot}. Our LOCATE-ACCEPT packs the TPUI as
 * {0, 0x11, slot}, so the last two octets are also its low 16 bits -- but note
 * that in stock the 0x11 is a **hardcoded constant** in the stock paging routine
 * (`mov r3,#17`), not a byte taken from the identity [CR A-StockTrace-5 §21].
 * The two agree today because our TPUI is built with the same prefix. We take
 * them from dect_mm_tpui() anyway, so that a pool-assigned TPUI (#68) is paged
 * with the value it was actually given rather than with a constant. The page info byte is
 * 0x0C for a CC page, plus 0x20 when the SETUP will carry <<BASIC-SERVICE>>
 * with service 8, i.e. wideband [CR the hardware-analysis notes §6; COMMAND_SET §6.3].
 *
 * Stock derives it exactly this way, in its paging routine: it reads the
 * <<BASIC-SERVICE>> element of the setup it is about to send and uses
 * 0x0C | (low nibble == 8 ? 0x20 : 0) [CR A-StockTrace-5 §10]. We used to
 * hardcode 0x2C, which was right for the wideband default and silently wrong
 * for any other basic service -- `-a basicsvc=0x80` paged "wideband to
 * follow" and then sent a narrowband SETUP, so that test was measuring a
 * self-contradiction rather than narrowband.
 *
 * INERT ON THE AIR, 2026-10-23 [CR A-0076]. The chip never reads mail[2] of
 * 0x0300: it takes the TPUI from bytes 3 and 4 and passes a hardcoded format
 * constant to the page builder, so 0x0C and 0x2C produce byte-identical
 * pages. Bit 0x20 changes nothing that is transmitted.
 *
 * We keep the derivation because it is what stock does and it costs nothing,
 * but it must never be treated as an experimental variable again. It already
 * cost us: today's four A540 runs varied <<BASIC-SERVICE>> across 0x88, 0x80
 * and 0x40, and because this function keys on the service nibble, three of
 * them silently changed the page byte as well. We read four experiments over
 * two variables. They were four runs of the same page.
 */
#define RING_PAGE_CC	DECT_BASE_PAGE_CC		/* CC page */
#define RING_PAGE_WIDEBAND DECT_BASE_PAGE_WIDEBAND	/* ... wideband SETUP to follow */

/* The ring's timing (dect_base.h, S3n). */
#define RING_PAGE_RESP_MS	DECT_BASE_PAGE_RESP_MS
#define RING_SETUP_TRIES	DECT_BASE_SETUP_TRIES

/*
 * Print every NWK message we send, as `reg: nwk out --> ...`.
 *
 * This line predates the #67 refactor, which moved the drain into the
 * library and removed it without anyone noticing -- including me, who wrote
 * the refactor. The first run that needed it read as "we never answered a
 * LOCATE-REQUEST", which would have been a serious protocol defect; what had
 * actually happened was that we answered and stopped saying so. Reading a log
 * for an absence only works if the thing would have been printed.
 */
static void log_nwk_out(void *ctx, const uint8_t *msg, size_t len)
{
	const struct stubd *s = ctx;
	size_t i;

	if (!s->verbose)
		return;
	printf("reg: nwk out --> ");
	for (i = 0; i < len; i++)
		printf("%02x ", msg[i]);
	printf("\n");
	fflush(stdout);
}


/*
 * The <<BASIC-SERVICE>> of our SETUP to handset i. A pure GAP handset
 * (neither NG-DECT Part 1 nor 3) gets GAP's incoming call, basic speech
 * 0x80: EN 300 444 table 21 allows service 0 only, and TS 102 527-1 §7.3.6
 * gives an internal call class 9 to NG handsets only. Not yet seen: as
 * configured. Spec sweep, finding 15.
 */
static uint8_t target_svc(const struct stubd *s, int i, uint8_t configured)
{
	return dect_base_basic_svc(&s->base, i, configured);
}

/* Stock's next step for a Part 3 handset (dect_base.h, S3p). */
static void cs_connect_tick(struct stubd *s, uint64_t now)
{
	dect_base_cs_connect_tick(&s->base, now);
}


/* The ring's sequencing is the library's (S3n). */
static void ring_tick(struct stubd *s, uint64_t now)
{
	if (s->sess.state == DECT_SESS_UP)
		dect_base_ring_tick(&s->base, now);
}

/*
 * Decode the 0xF625 answer: the per-link row AVM's DECT Monitor displays.
 *
 * The record is 45 bytes. Offsets below are indices into the parameter bytes,
 * i.e. the chip's answer[N+2] [CR A-0088]. Two independent sources agree on
 * the shape -- the clean room's read of the producer
 * [CR the hardware-analysis notes 5 4.4, CR COMMAND_SET 8.1, CR A-0088] and
 * 554 answers already sitting in our own capture corpus, which the daemon has
 * been collecting since G7 and throwing away. Where they disagree, the note
 * on [5] below says so rather than picking a winner quietly.
 *
 *   [0]        link handle, echoing the 0xF624 request
 *   [1]        0x0a in all 554 samples: the number of carrier entries
 *   [2]        MAC time slot (or slot x 2 -- only 00/02/04/06/08 observed)
 *   [3]        RF carrier in use, 0..9
 *   [5]        (record +0x116 bit 1) ? 2 : 1 [CR A-0089]; not the slot type
 *   [6..0x0F]  ten RSSI bytes, one per carrier, LARGER = STRONGER
 *   [0x10..0x23] eight u16 LE per-interval event counters, at 0x10 0x12 0x14
 *              0x16 0x18 0x1C 0x1E 0x20, plus the interval frame count at
 *              0x1A (mirrored at 0x22)
 *   [0x24..0x25] frames since this link was established, u16 LE
 *   [0x26..0x2C] zero in all 554 samples
 *
 * Two of those were settled by our own corpus rather than by a trace, because
 * they validate themselves:
 *
 *   [3] names the carrier in use. If so, the RSSI at that index must be the
 *   strongest of the ten -- and it is, in 97.2% of the 325 samples whose
 *   spectrum has a peak at all (spread >= 20), falling to chance when the
 *   spectrum is flat and "which is largest" is arbitrary. A correlation that
 *   appears exactly when there is something to correlate with is the
 *   signature of a true identification.
 *
 *   [0x24..0x25] advances at a median of 100.00 per second against wall clock
 *   over 365 consecutive poll intervals (10th pct 99.92, 90th 100.05). The
 *   DECT TDMA frame is 10 ms and nothing else here ticks at 100 Hz. It starts
 *   near zero at establishment, so it is the call duration -- free, with no
 *   timer of ours. It is 16 bits and wraps at 65536 frames (10 min 55 s), so
 *   we unwrap it here; our poll interval is far shorter than the wrap period,
 *   which makes that safe.
 *
 * The interval count at 0x1A closes the arithmetic: the running sum
 * total(n) = total(n-1) + interval(n) is exact in 347 of 365 consecutive
 * pairs, the misses all new-call restarts.
 *
 * **[5] is NOT the slot type**, and both sides now agree on that. This
 * function used to label it "0 = full slot, 1..3 = long slot" on an early
 * clean-room reading. Our corpus has [5] in {01, 02} and never 00, which
 * under that reading would make every link we have ever had a long slot --
 * while 0x11718, the independent downlink instrument, reads 0x28 (full slot)
 * in all 57 reads this project has ever taken, across nine runs. [CR A-0089]
 * then settled it from the chip: [5] is written as (+0x116 bit 1) ? 2 : 1,
 * the bit A-0077 traced and A-0087 withdrew, which is set in ordinary bearer
 * flow regardless of slot width.
 *
 * The slot type is (received bearer byte0 & 0x0E) == 0x0A, and it is **not
 * copied into 0xF625 at all** [CR A-0089] -- it lives in the per-handset
 * record at +0x0C. So our "no byte satisfies & 0x0E == 0x0A" across 554
 * samples was not a sampling limit; the byte is genuinely absent. 0x11718 is
 * and always was our instrument for Mode. See docs/diagnostics.md.
 *
 * The eight event counters are per-interval, not cumulative -- zero in 94-99%
 * of samples, otherwise 1 or 2, and they fall as well as rise. [CR A-0089]
 * confirms the shape: these are per-slot receive FIELD-INTEGRITY counters,
 * gating on a validator that runs field-length checks and a CRC-style
 * complement over the slot buffer. Each counts the rare failing slots.
 *
 * That also explains why our corpus cannot name them: a bad slot fails
 * field-length AND CRC AND sync together, so every pair of counters is
 * enriched 4.5x-29x over independence. The clustering is the signature of
 * integrity counters sharing one quality factor, not evidence about which
 * column is which. What each status BIT means (A-CRC, X-CRC, sync, ...) is a
 * recorded gap: A-0090 traced the status byte and could not pin its source,
 * and declined to label the bits rather than guess. [28] is likewise unknown.
 *
 * Naming them needs a directed bench test, designed in docs/diagnostics.md,
 * whose control -- clean link, five minutes, all eight zero -- is the room's
 * prediction as well as ours.
 */
static void report_conn_status(struct stubd *s, const struct dect_mail *m)
{
	/*
	 * The decode lives in the library now (dect_diag_decode_status()),
	 * so the LuCI page and the controller read the row the same way. The
	 * labels name each event counter's TRIGGER, not the error: "s12b6" is
	 * "receive status[0x12] bit 6 was clear", and what that bit means is
	 * still open [CR A-0089, A-0090].
	 */
	struct dect_diag_status st;
	struct dect_diag_frames f;
	struct dect_diag_link d;
	char js[768];
	unsigned i, peak;

	if (dect_diag_decode_status(m->param, m->param_len, &st) < 0) {
		printf("call: status: short 0xF625 (%u bytes)\n", m->param_len);
		return;
	}

	/* Slot, carrier, and how strong the carrier we are on actually is. */
	peak = 0u;
	for (i = 1u; i < DECT_DIAG_CARRIERS; i++)
		if (st.rssi[i] > st.rssi[peak])
			peak = i;
	printf("call: status: slot %u carrier %u", st.time_slot, st.carrier);
	/* dBm as FRITZ!OS converts it on the 7520 (clean room A-0121) */
	if (st.carrier < DECT_DIAG_CARRIERS)
		printf(" level 0x%02x (%.1f dBm)%s", st.rssi[st.carrier],
		       (st.rssi[st.carrier] - 20) * 80.0 / 37.0 - 110.0,
		       st.carrier == peak ? "" : " (NOT the strongest carrier)");
	printf(" b5 0x%02x\ncall: status: rssi", st.b5);
	for (i = 0u; i < DECT_DIAG_CARRIERS; i++)
		printf(" %02x%s", st.rssi[i], i == st.carrier ? "*" : "");
	printf("\n");

	if (!st.has_counters) {
		printf("call: status: no counters (%u bytes)\n", m->param_len);
		return;
	}

	/* reset_call_state() clears have_counters, so a new call restarts */
	f.total = s->reg.frames_total;
	f.last = s->reg.frames_last;
	f.have = (uint8_t)s->reg.have_counters;
	s->reg.frames_total = dect_diag_frames_update(&f, &st);
	s->reg.frames_last = f.last;

	printf("call: status: up %llu frames (%.2f s), %u this interval\n",
	       (unsigned long long)s->reg.frames_total,
	       (double)s->reg.frames_total / 100.0, st.interval_frames);

	printf("call: status: errors");
	for (i = 0u; i < DECT_DIAG_EVENTS; i++) {
		int delta = (int)st.events[i] - (int)s->reg.counter[i];

		printf(" %s=%u%+d", dect_diag_event_name(i), st.events[i],
		       s->reg.have_counters ? delta : 0);
	}
	printf("\n");
	for (i = 0u; i < DECT_DIAG_EVENTS; i++)
		s->reg.counter[i] = st.events[i];
	s->reg.have_counters = 1;

	/* The same row as one object, the shape the Monitor export will use. */
	s->reg.last_status = st;
	memset(&d, 0, sizeof(d));
	d.radio = st;
	d.call_frames = s->reg.frames_total;
	dect_diag_from_link(&s->base.main_link.link, &d);
	if (d.handset_tag < 0)		/* serving the current store */
		d.handset_tag = (int)s->base.cur;
	if (dect_diag_json(&d, s->show_params, js, sizeof(js)))
		printf("diag: %s\n", js);
}

/*
 * The Fon/DECT LED. The 7520 has no LED of its own for DECT: its front
 * panel shares one "Fon/DECT" LED, green:fon under OpenWrt, which nothing
 * else drives. On while the radio is up, blinking while C-plane traffic
 * moves or a call is up, off when parked.
 *
 * The blink is sampled, not counted: every LED_TICK_MS the loop asks whether
 * anything moved since the last look and toggles if so. Signalling runs at
 * about 50 segments/s but a call is 8000 voice frames/s, so a per-packet
 * divisor would blink at 5 Hz in one case and 800 Hz in the other. Sampled,
 * it is ~5 Hz either way, at most ten sysfs writes a second, written only
 * when the state changes.
 */
#define LED_DEFAULT_PATH	"/sys/class/leds/green:fon/brightness"
#define LED_TICK_MS		100u

static void led_write(struct stubd *s, int on)
{
	if (s->led_fd < 0 || s->led_state == on)
		return;
	if (pwrite(s->led_fd, on ? "1" : "0", 1, 0) == 1) {
		s->led_state = on;
		s->led_changes++;
	}
}

static void led_open(struct stubd *s)
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

static void led_tick(struct stubd *s, uint64_t now, int radio_on)
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

/*
 * The state file: what the base is doing, for LuCI or anything else that
 * wants it without speaking a protocol (ROADMAP M9, docs/diagnostics.md).
 * One JSON object, rewritten atomically (temporary file, rename) at most
 * once a second, on /tmp, which is RAM. IPUIs only with -p: the file is
 * meant to be read by a web page.
 */
#define STATE_DEFAULT_PATH	DECT_RUN_DIR "/state.json"
#define STATE_EVERY_MS		1000u

/* A JSON string: quotes, backslashes and control characters escaped. */
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

static void state_write(struct stubd *s, uint64_t now, int radio_on,
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
static void pump_lapc(struct stubd *s)
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

static struct side_link *side_find(struct stubd *s, uint8_t handle);


/*
 * Open or close the registration window: the chip's mode (0x022B 'A' /
 * 0x022C 01 [CR COMMAND_SET §6.2]) and MM's policy together. secs = 0
 * keeps it open until closed.
 */
static void reg_window(struct stubd *s, int open, unsigned secs,
		       uint64_t now, const char *why)
{
	if (!open)
		s->reg_profile[0] = '\0';	/* the choice was for this window */
	dect_base_reg_window(&s->base, open, secs, now, why);
	fflush(stdout);
}

/* SIGUSR1 opens a window; a timed one closes itself. */
static void reg_window_tick(struct stubd *s, uint64_t now)
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

static void xlane_release(struct stubd *s, struct side_link *sl);

static void log_nwk_out_side(void *ctx, const uint8_t *msg, size_t len);

/* dect_base's link_up(): our own state for a link set up afresh. */
static void base_link_up(void *ctx, int p)
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
static void base_side_down(void *ctx, unsigned k)
{
	struct stubd *s = ctx;

	xlane_release(s, &s->side[k]);
}

/* Slot and identity of handset table entry i, current or stashed. */
static unsigned hs_slot(const struct stubd *s, unsigned i)
{
	return dect_base_slot(&s->base, i);
}

/* The handset's name for the lists and the internal caller (dect_base.h). */
static void hs_name(const struct stubd *s, unsigned i, char *buf, size_t cap)
{
	dect_base_hs_name(&s->base, i, buf, cap);
}

/* <path>.name, one line of UTF-8, beside the subscription store. */
static void name_path(const struct stubd *s, unsigned i, char *buf, size_t cap)
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
static struct dect_contacts stubd_contacts;	/* every link's Contact List */
static struct dect_calls stubd_calls;		/* ... and call lists */

/*
 * The Contact List's file, once a second: read whole when its size or time
 * changed (the sync replaces it by rename); and the changes handsets made,
 * appended to FILE.changes for the sync to write back. Formats in
 * dect_contacts.h. A journal line that cannot be written stays queued.
 */
static void contacts_tick(struct stubd *s, uint64_t now)
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

/*
 * The call lists over a restart of the daemon, in a file in RAM (a reboot
 * empties them): read at the start, written once a second when the text
 * differs from the last one written, and when the daemon ends. Format in
 * dect_calls.h; numbers and names, so only root reads it.
 */
#define CALLS_PATH	DECT_RUN_DIR "/calls"
#define CALLS_TEXT_MAX	(3u * DECT_CALLS_MAX * 230u + 32u)

static char calls_text[CALLS_TEXT_MAX];
static uint64_t calls_at, calls_hash;

static uint64_t calls_fnv(const char *t, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	while (n--)
		h = (h ^ (unsigned char)*t++) * 0x100000001b3ull;
	return h;
}

static void calls_tick(uint64_t now, int force)
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
static void contacts_init(struct stubd *s)
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

static void name_load(struct stubd *s, unsigned i)
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

static void name_save(const struct stubd *s, unsigned i)
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


static int profile_load(const char *path, struct dect_profile *p);

#define PROFILE_DIR_DEFAULT	"/etc/opendectcell/profiles"

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

/*
 * A handset's own settings, from the DECT settings page (Network > DECT,
 * Handsets, General): "key = value" lines in /etc/opendectcell/handset<N>.override,
 * written by the init script from /etc/config/opendectcell, on top of the
 * chosen profile.
 */
#define OVERRIDE_DIR	"/etc/opendectcell"

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
static void reg_profile_apply(struct stubd *s)
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
static int reg_profile_ok(const struct stubd *s, const char *id)
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

static void ctl_hs(struct stubd *s, unsigned i, const char *state);

static void profiles_resolve_all(struct stubd *s, const char *why)
{
	unsigned i;

	printf("profile: resolving the handsets' profiles (%s)\n", why);
	for (i = 0; i < s->base.n_hs; i++) {
		profile_resolve(s, i);
		lines_load(s, i);	/* their line names travel with it */
	}
	fflush(stdout);
}

static void missed_changed_tick(struct stubd *s)
{
	dect_base_missed_changed_tick(&s->base);
}

/* dect_base's named(): keep the name in its .name file, tell the clients. */
static void base_named(void *ctx, unsigned i)
{
	struct stubd *s = ctx;

	name_save(s, i);
	ctl_hs(s, i, "registered");
}

static void names_tick(struct stubd *s)
{
	if (want_profiles) {
		want_profiles = 0;
		profiles_resolve_all(s, "SIGHUP");
	}
	/* the lists' hooks per link are the library's (dect_base.h, S4d) */
	dect_base_lists_tick(&s->base, mono_ms());
}

static struct side_link *side_find(struct stubd *s, uint8_t handle)
{
	unsigned i;

	for (i = 0; i < SIDE_MAX; i++)
		if (s->side[i].bl->active && s->side[i].bl->handle == handle)
			return &s->side[i];
	return NULL;
}

static void log_nwk_out_side(void *ctx, const uint8_t *msg, size_t len)
{
	const struct stubd *s = ctx;
	size_t i;

	if (!s->verbose)
		return;
	printf("side: nwk out --> ");
	for (i = 0; i < len; i++)
		printf("%02x ", msg[i]);
	printf("\n");
	fflush(stdout);
}


static int xlane_free(const struct stubd *s)
{
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++)
		if (!s->xl[i].used)
			return (int)i;
	return -1;
}

/*
 * Lanes a new side call can still count on: free ones, less those that
 * side calls accepted but not yet connected will take (review, finding 6:
 * one free lane used to admit any number of calls), and those of side
 * rings still paging their handsets.
 */
static int lanes_available(const struct stubd *s, const struct side_link *me)
{
	int n = 0;
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++)
		if (!s->xl[i].used)
			n++;
	for (i = 0; i < BC_MAX; i++)
		if (s->base.calls[i].id && s->base.calls[i].party == BC_PAGING)
			n--;
	for (i = 0; i < SIDE_MAX; i++) {
		const struct side_link *o = &s->side[i];

		if (o != me && o->bl->active && o->bl->lane < 0 &&
		    dect_cc_busy(&o->bl->link.cc))
			n--;
	}
	return n;
}

static struct xlane *xlane_of(struct stubd *s, uint8_t handle)
{
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++)
		if (s->xl[i].used && s->xl[i].handle == handle)
			return &s->xl[i];
	return NULL;
}

/*
 * Give side link sl's speech call an audio path of its own, as stock binds
 * its second call (clean room A-0124, two captures with two calls up):
 * 0x0251 {h, k, gain}, 0x0230 {h, flags | bit, 1 << k, mode, codec} where
 * channel k = 1, 2, 3 has the flags bit 0x1, 0x2, 0x4 and the mask bit k.
 * 0x022D follows on the 0x0242, as for lane 0.
 */
static void xlane_bind(struct stubd *s, struct side_link *sl, int i)
{
	struct xlane *x = &s->xl[i];
	const struct audio_cfg *a = &s->audio;
	uint8_t flags, codec;
	uint16_t mask;

	memset(x, 0, sizeof(*x));
	x->used = 1;
	x->handle = sl->bl->handle;
	x->channel = a->lane_ch[i];
	x->word = a->lane_word[i];
	x->wideband = !a->narrowband &&
		dect_cc_codec(&sl->bl->link.cc) == DECT_CC_CODEC_G722;
	sl->bl->lane = i;
	mask = (uint16_t)(1u << x->channel);

	dect_base_audio_param(&s->base, x->handle, x->channel /* selector */,
			      a->gain);
	flags = (uint8_t)((a->flags & 0xB0u) | (1u << (x->channel - 1u)) |
			  (x->wideband ? 0x40u : 0u));
	codec = x->wideband ? 0u : a->codec;
	chip_assign(s, x->handle, flags, mask, a->mode, codec);
	printf("side: second call on handle %u: lane %d, channel %u, TDM word "
	       "%u, %s -- 0x0230 {%02x %04x %02x %02x}\n", x->handle, i,
	       x->channel, x->word, x->wideband ? "wideband" : "narrowband",
	       flags, mask, a->mode, codec);
}

static void xlane_connect(struct stubd *s, struct xlane *x)
{
	if (x->connected)
		return;
	x->connected = 1;
	dect_base_audio_path(&s->base, x->handle);
	printf("side: audio connect: 0x022D on handle %u (channel %u)\n",
	       x->handle, x->channel);
}

static void xlane_release(struct stubd *s, struct side_link *sl)
{
	if (sl->bl->lane < 0)
		return;
	if (s->xl[sl->bl->lane].connected || s->xl[sl->bl->lane].used)
		send_audio_disconnect(s, sl->bl->handle);
	memset(&s->xl[sl->bl->lane], 0, sizeof(s->xl[0]));
	sl->bl->lane = -1;
	/* the Monitor's figures belong to the call that ended */
	memset(&sl->last_status, 0, sizeof(sl->last_status));
	memset(&sl->frames, 0, sizeof(sl->frames));
	sl->status_ms = 0;
}

/* A side link's number is complete: internal, to the bridge, or local. */
static void side_route(struct stubd *s, struct side_link *sl, uint64_t now)
{
	int p = (int)(sl - s->side);

	sl->bl->ic_tried = 1;
	if (ic_start(s, p, now) || bridge_call_out(s, p, now) ||
	    dect_cc_accept(&sl->bl->link.cc) == 0) {
		dect_link_drain(&sl->bl->link, now);
		side_pump(s, sl);
	}
}

/*
 * -a twocalls on a side link: take speech while a lane is free (else the
 * busy refusal stays), route a dialled call after dialwait as the main
 * link does, bind once it is connected, release when it ends.
 */
static void side_call_tick(struct stubd *s, struct side_link *sl, uint64_t now)
{
	struct dect_cc *cc = &sl->bl->link.cc;

	if (!s->base.cfg.two_calls)
		return;
	if (!dect_cc_busy(cc))	/* decided for the next SETUP only */
		dect_cc_set_busy(cc, sl->bl->lane < 0 &&
				 lanes_available(s, sl) <= 0);
	switch (dect_base_dial_tick(&s->base, (int)(sl - s->side), now)) {
	case 2:
		printf("side: dialled on handle %u -- connecting\n",
		       sl->bl->handle);
		/* fall through */
	case 1:
		side_route(s, sl, now);
		break;
	}
	if (sl->bl->lane < 0 && cc->state == DECT_CC_ST_ACTIVE &&
	    (cc->incoming || dect_cc_peer_is_speech(cc)) &&
	    call_auth_ok(s, &sl->bl->link)) {
		int i = xlane_free(s);

		if (i >= 0)
			xlane_bind(s, sl, i);
	}
	if (sl->bl->lane >= 0 && !dect_cc_busy(cc))
		xlane_release(s, sl);
}

/* -------------------------------------------------------------------------
 * Internal call
 */

static struct dect_cc *ic_cc(struct stubd *s, int side)
{
	return side < 0 ? &s->base.main_link.link.cc : &s->side[side].bl->link.cc;
}

static int ic_active(const struct stubd *s, int p)
{
	return dect_base_party_active(&s->base, p);
}

/* the party has its audio channel bound */
static int ic_has_lane(const struct stubd *s, int p)
{
	return p == -1 ? s->base.main_link.bound : p >= 0 && s->side[p].bl->lane >= 0;
}

/* The caller (main link: side = -1) finished dialling (dect_base.h, S3q). */
static int ic_start(struct stubd *s, int side, uint64_t now)
{
	return dect_base_ic_start(&s->base, side, now);
}

static int ic_wideband(const struct stubd *s, int p);

#define DEREG_IDLE	DECT_BASE_DEREG_IDLE
#define DEREG_PAGING	DECT_BASE_DEREG_PAGING
#define DEREG_REQUESTED	DECT_BASE_DEREG_REQUESTED
#define DEREG_REQ_PATH	DECT_RUN_DIR "/deregister"

/*
 * dect_base's forget() (S4b): handset i is deregistered. The store, its
 * .name, .profile and .cap move aside (<store>.deregistered), so a mistake
 * can be undone by moving them back; the clients hear it is gone.
 */
static void base_forget(void *ctx, unsigned i, const char *why)
{
	struct stubd *s = ctx;
	char from[256], to[288];

	(void)why;
	printf("dereg: DECT %u is %s\n", hs_slot(s, i), s->hs[i].path);
	ctl_hs(s, i, "gone");
	snprintf(to, sizeof(to), "%s.deregistered", s->hs[i].path);
	if (rename(s->hs[i].path, to))
		printf("dereg: could not move %s aside: %s\n", s->hs[i].path,
		       strerror(errno));
	else
		printf("dereg: %s moved to %s\n", s->hs[i].path, to);
	name_path(s, i, from, sizeof(from));
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	/* and its model and capability: the next one here is another */
	snprintf(from, sizeof(from), "%s.profile", s->hs[i].path);
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	snprintf(from, sizeof(from), "%s.cap", s->hs[i].path);
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	fflush(stdout);
}

/* dect_base's mask(): the locked-in handsets, again. */
static void base_mask(void *ctx)
{
	send_subscription_tail(ctx);
}

static void dereg_begin(struct stubd *s, unsigned slot, uint64_t now,
			const char *who);

/*
 * SIGUSR2: deregister the handsets in the slots named in DEREG_REQ_PATH
 * (one or more numbers, e.g. "1 3"), one after the other.
 */
static void dereg_start(struct stubd *s, uint64_t now)
{
	char buf[64] = "";
	const char *path = DEREG_REQ_PATH;
	FILE *f = fopen_root_only(path);
	char *p = buf, *end;
	unsigned slot, n = 0;

	if (f) {
		if (!fgets(buf, sizeof(buf), f))
			buf[0] = 0;
		fclose(f);
		(void)remove(path);
	}
	for (;;) {
		slot = (unsigned)strtoul(p, &end, 10);
		if (end == p)
			break;
		p = end;
		dereg_begin(s, slot, now, "SIGUSR2");
		n++;
	}
	if (!n)
		dereg_begin(s, 0, now, "SIGUSR2");
}

static void dereg_begin(struct stubd *s, unsigned slot, uint64_t now,
			const char *who)
{
	dect_base_dereg_begin(&s->base, slot, now, who);
}

/* The deregistration is the library's (dect_base.h, S4b); SIGUSR2 ours. */
static void dereg_tick(struct stubd *s, uint64_t now)
{
	if (want_deregister) {
		want_deregister = 0;
		dereg_start(s, now);
	}
	dect_base_dereg_tick(&s->base, now);
}

/* dect_base's wall(): the wall clock, right or not. */
static void base_wall(void *ctx, struct dect_time *dt)
{
	(void)ctx;
	clock_any(dt);
}

/* dect_base's clock(): the wall clock, when it is known to be right. */
static int base_clock(void *ctx, struct dect_time *dt, long *off_s)
{
	struct stubd *s = ctx;

	return clock_now(s->audio.timesync, dt, off_s);
}

/*
 * -a timeupdate=MIN: the date and time to every handset we hold, every MIN
 * minutes (maintainer, 2026-10-29), and the notices for handsets with no
 * link: the library's (dect_base.h, S4c), while the radio is on.
 */
static void time_tick(struct stubd *s, uint64_t now, int radio_on)
{
	if (radio_on)
		dect_base_time_tick(&s->base, now);
}

/* -------------------------------------------------------------------------
 * The control socket: docs/bridge-interface.md §2, step 1 of §5. A Unix
 * stream socket, root only (0600), up to CTL_CLIENTS clients at once (the
 * bridge stays connected; LuCI's commands come and go); `hello` and an
 * `hs` line per handset when one connects, `hs` again to every client when
 * one is renamed or deregistered; replies go to the client that asked,
 * call traffic only to the bridge; `deregister` and `register-window`
 * from it, as SIGUSR2 and SIGUSR1 do. Nothing a SIP peer says reaches MM
 * through here except these two configuration commands (§4).
 */
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
static void ctl_send_bridge(struct stubd *s, struct dect_ctl_out *o)
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

static void ctl_hs(struct stubd *s, unsigned i, const char *state)
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
static int bridge_call_out(struct stubd *s, int p, uint64_t now)
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
static struct dect_base_call *bc_active(struct stubd *s, int p)
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
static void bridge_tick(struct stubd *s, uint64_t now)
{
	dect_base_bridge_tick(&s->base, now);
}

/*
 * The audio socket: listening while the control socket is. A connection
 * names its call in its first message, "call=N"; the lane that call is
 * bound to takes it.
 */
static void ab_init(struct stubd *s)
{
	unsigned i;

	s->ab.lfd = -1;
	for (i = 0; i < AB_LANES; i++)
		s->ab.pend[i] = s->ab.l[i].cfd = -1;
}

/* The listening audio socket, for the bridge or, in cell mode, dect-cell. */
static void ab_listen(struct stubd *s)
{
	struct sockaddr_un a;
	mode_t old;
	int fd;

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, AB_PATH);
	(void)unlink(AB_PATH);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	old = umask(077);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
	    chmod(AB_PATH, 0600) || listen(fd, (int)AB_LANES)) {
		printf("ctl: %s: %s -- no audio socket\n", AB_PATH,
		       strerror(errno));
		umask(old);
		close(fd);
		return;
	}
	umask(old);
	s->ab.lfd = fd;
}

static void ab_open(struct stubd *s)
{
	ab_init(s);
	if (s->ctl.lfd >= 0)
		ab_listen(s);
}

static void ab_end(struct ab_lane *l, const char *why)
{
	if (l->cfd < 0)
		return;
	close(l->cfd);
	l->cfd = -1;
	printf("audio: call %u's socket closed (%s): %u frames up, %u down, "
	       "%u underruns, %u dropped\n", l->call, why, l->frames_up,
	       l->frames_down, l->underruns, l->dropped);
}

static void ab_close(struct stubd *s)
{
	unsigned i;

	for (i = 0; i < AB_LANES; i++) {
		ab_end(&s->ab.l[i], "exit");
		if (s->ab.pend[i] >= 0)
			close(s->ab.pend[i]);
		s->ab.pend[i] = -1;
	}
	if (s->ab.lfd < 0)
		return;
	close(s->ab.lfd);
	s->ab.lfd = -1;
	(void)unlink(AB_PATH);
}

/* The bridged call whose speech lane i carries now, or NULL. */
static const struct dect_base_call *ab_call_of(struct stubd *s, unsigned i, int *wb)
{
	const struct xlane *x;
	unsigned k;

	if (i == 0) {
		*wb = s->reg.call_wideband;
		return s->base.main_link.active && s->base.main_link.bound ? bc_active(s, -1)
							  : NULL;
	}
	x = &s->xl[i - 1u];
	*wb = x->wideband;
	if (!x->used || !x->connected)
		return NULL;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl->active && s->side[k].bl->lane == (int)(i - 1u))
			return bc_active(s, (int)k);
	return NULL;
}

/* A pending connection's first message: "call=N" names its lane. */
static void ab_hello(struct stubd *s, unsigned j, uint64_t now)
{
	char buf[32];
	unsigned i, id = 0, rate = 16000u;
	int g722 = 0;
	ssize_t n = recv(s->ab.pend[j], buf, sizeof(buf) - 1u, MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
		      errno == EINTR) && now - s->ab.pend_since[j] < AB_HELLO_MS)
		return;
	if (n > 0) {
		unsigned mid;

		buf[n] = '\0';
		if (sscanf(buf, "call=%u rate=%u", &id, &rate) < 1)
			id = 0;
		/*
		 * cell mode: "media=N codec=g722" or "media=N rate=8000",
		 * from dect-cell for the controller's media session N (any
		 * u32, 0 too): the lane that session holds
		 */
		if (s->cell_mode && sscanf(buf, "media=%u rate=%u", &mid,
					   &rate) >= 1) {
			for (i = 0; i < AB_LANES; i++)
				if (s->cell.media[i].open &&
				    s->cell.media[i].id == mid)
					id = s->ab.l[i].call;
		}
		g722 = strstr(buf, " codec=g722") != NULL;
		if (rate != 8000u || g722)
			rate = 16000u;
	}
	for (i = 0; id && i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];

		if (l->call != id)
			continue;
		/* again: the SIP codec changed, the socket's format with it */
		if (l->cfd >= 0)
			ab_end(l, "the bridge connected again");
		l->cfd = s->ab.pend[j];
		l->rate = rate;
		l->g722 = g722;
		dect_g722_init(&l->sip_enc);
		dect_g722_init(&l->sip_dec);
		dect_rs2_init(&l->rs_up);
		dect_rs2_init(&l->rs_down);
		s->ab.pend[j] = -1;
		printf("audio: the bridge connected for call %u (%s) -- %s\n",
		       id, g722 ? "G.722" : rate == 8000u ? "8 kHz" : "16 kHz",
		       l->wideband ? (g722 ? "G.722 passed as it is" :
				      rate == 8000u ?
		       "G.722 decoded and resampled here (no common codec)" :
		       "G.722 decoded here") : rate == 8000u ?
		       "narrowband, passed as it is" : g722 ?
		       "narrowband, resampled and G.722 coded here (no common "
		       "codec)" : "narrowband, resampled here (no common codec)");
		return;
	}
	close(s->ab.pend[j]);
	s->ab.pend[j] = -1;
	printf("audio: a connection named no call waiting for one -- "
	       "closed\n");
}

static void ab_tick(struct stubd *s, uint64_t now)
{
	unsigned i;
	int c;

	if (s->ab.lfd < 0)
		return;
	/* each lane: the call on it, announced once its speech is bound */
	for (i = 0; !s->cell_mode && i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];
		const struct dect_base_call *b;
		struct dect_ctl_out o;
		int wb;

		b = ab_call_of(s, i, &wb);
		if (l->call && (!b || l->call != b->id)) {
			ab_end(l, "the call ended");
			l->call = 0;
		}
		if (!b || l->call == b->id)
			continue;
		memset(l, 0, sizeof(*l));
		l->cfd = -1;
		l->call = b->id;
		l->wideband = wb;
		l->rx_prev = 0xFFFFFFFFu;
		dect_g722_init(&l->enc);
		dect_g722_init(&l->dec);
		dect_g722_init(&l->sip_enc);
		dect_g722_init(&l->sip_dec);
		dect_g722_init(&l->sil);
		dect_rs2_init(&l->rs_up);
		dect_rs2_init(&l->rs_down);
		dect_ctl_begin(&o, "audio");
		dect_ctl_add_uint(&o, "call", b->id);
		dect_ctl_add(&o, "path", AB_PATH);
		ctl_send_bridge(s, &o);
		printf("audio: call %u's speech on %s (lane %u, %s)\n", b->id,
		       AB_PATH, i, wb ? "wideband" : "narrowband");
	}
	while ((c = accept(s->ab.lfd, NULL, NULL)) >= 0) {
		for (i = 0; i < AB_LANES && s->ab.pend[i] >= 0; i++)
			;
		if (i == AB_LANES) {
			close(c);
			continue;
		}
		(void)fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		(void)fcntl(c, F_SETFD, FD_CLOEXEC);
		s->ab.pend[i] = c;
		s->ab.pend_since[i] = now;
	}
	for (i = 0; i < AB_LANES; i++)
		if (s->ab.pend[i] >= 0)
			ab_hello(s, i, now);
	for (i = 0; i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];

		while (l->cfd >= 0) {
			int16_t pcm[AB_FRAME / 2u];
			ssize_t n = recv(l->cfd, pcm, sizeof(pcm),
					 MSG_DONTWAIT);

			if (n == 0) {
				ab_end(l, "the bridge closed it");
				break;
			}
			if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK &&
				    errno != EINTR)
					ab_end(l, "read failed");
				break;
			}
			/*
			 * 20 ms in the socket's format; one octet is the
			 * bridge saying it has nothing (the fill covers it),
			 * anything else is not ours.
			 */
			if ((size_t)n == (l->g722 ? 160u :
					  l->rate / 50u * sizeof(pcm[0])))
				ab_down(l, pcm, (size_t)n);
		}
	}
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

static void ctl_open(struct stubd *s)
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
	    chmod(s->ctl.path, 0600) || listen(fd, (int)CTL_CLIENTS)) {
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

static void ctl_close(struct stubd *s)
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

static void ctl_tick(struct stubd *s, uint64_t now)
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

/*
 * The internal call's progress is the library's (S3q); the bridge of the
 * two parties' audio is ours.
 */
static void ic_tick(struct stubd *s, uint64_t now)
{
	dect_base_ic_tick(&s->base, now);
	if (s->base.ic.st != IC_UP) {
		s->ic_warned = 0;
		return;
	}
	if (!s->ic_warned &&
	    ic_wideband(s, s->base.ic.caller) != ic_wideband(s, s->base.ic.tside) &&
	    ic_has_lane(s, s->base.ic.caller) && ic_has_lane(s, s->base.ic.tside)) {
		s->ic_warned = 1;
		printf("intern: the two calls use different codecs -- no "
		       "bridge (no transcoder yet), the parties hear silence\n");
	}
}

/* Is internal-call party p's lane the coded (wideband) one? */
static int ic_wideband(const struct stubd *s, int p)
{
	if (p == IC_NONE)
		return 0;
	return p < 0 ? s->reg.call_wideband
		     : s->side[p].bl->lane >= 0 && s->xl[s->side[p].bl->lane].wideband;
}

/*
 * The TDM word of an internal call party: lane 0, or its side's xlane; -1
 * while it has none, when the word is out of the frame (review, finding
 * 10), or when the two parties' lanes differ in mode -- a coded G.722 word
 * copied into a linear lane is noise, and there is no transcoder yet
 * (finding 7).
 */
static int ic_word(const struct stubd *s, int side)
{
	int w;

	if (side == IC_NONE || (s->base.ic.st == IC_UP &&
	    ic_wideband(s, s->base.ic.caller) != ic_wideband(s, s->base.ic.tside)))
		return -1;
	if (side < 0)
		w = s->base.main_link.bound ? (int)s->audio.lane0_word : -1;
	else if (s->side[side].bl->lane < 0 ||
		 !s->xl[s->side[side].bl->lane].connected)
		w = -1;
	else
		w = (int)s->xl[s->side[side].bl->lane].word;
	return w >= 0 && (unsigned)w < s->slots &&
	       w != s->tx_slot ? w : -1;
}

static void side_pump(struct stubd *s, struct side_link *sl)
{
	struct dect_mail m;

	while (sizeof(s->txq) - s->txq_len >= DECT_TX_ELEM_MAX &&
	       dect_link_poll_append_mail(&sl->bl->link, &m)) {
		if (send_mail(s, (unsigned int)s->tx_slot, &m))
			printf("side: C-plane mail LOST (%u bytes)\n",
			       (unsigned)m.param_len);
		else
			s->led_traffic++;
	}
}

/* A side link's call ciphered as the main link's (dect_base.h, S3v). */
static void side_cipher_tick(struct stubd *s, struct side_link *sl,
			     uint64_t now)
{
	dect_base_side_cipher_tick(&s->base, (unsigned)(sl - s->side), now);
}

/*
 * One received C-plane segment.
 *
 * The protocol work -- Cs decode, Lc reassembly, LAPC, NWK routing, queuing
 * the replies -- is the library's since openDECTcell#67. What is left here is
 * policy the library deliberately does not own: when a page response may
 * release a queued {CC-SETUP}, and learning the identities an FP-originated
 * call needs.
 */

/*
 * Challenge the handset with the stored UAK so both sides derive the DCK;
 * the CIPHER-REQUEST and the chip's key (0x0208) follow once it answers.
 * Once per connection. -C keeps every call in the clear.
 */
static void start_call_cipher(struct stubd *s, uint64_t now)
{
	dect_base_main_cipher_start(&s->base, now);
}
/*
 * dect_base's registered(): the enrolment completed and the chip holds the
 * subscription.
 */
static void base_registered(void *ctx, uint64_t now)
{
	struct stubd *s = ctx;

	/*
	 * The chip now holds what the start-up restore would write: without
	 * this the restore in loop_tick() saw the new UAK and sent the whole
	 * install a second time (register.trace, 2026-10-30).
	 */
	s->sub_restored = 1;
	if (s->reg_profile[0])
		reg_profile_apply(s);
	/*
	 * Close registration once the handset is enrolled. AVM's documented
	 * sequence opens registration (0x022B 'A') and closes it afterwards
	 * with 0x022C 01, which clears the a44 "registration open" capability
	 * [CR COMMAND_SET §6.2/§9.7]. Leaving it open keeps the base
	 * advertising open enrolment and the handset re-attaching.
	 */
	reg_window(s, 0, 0, now, "handset enrolled");
	printf("reg: subscription persisted + registration closed (slot %u)\n",
	       (unsigned)stubd_sub_slot(s));
}

/* dect_base's call_up(): the main link's call gets its audio path. */
static void base_call_up(void *ctx, uint64_t now)
{
	struct stubd *s = ctx;

	/*
	 * Bind the voice path once we have answered the call (CONNECT sent). The
	 * C4 treats CONNECT as answered even though it does not send CONNECT-ACK.
	 *
	 * Only speech calls get an audio path. A CAT-iq handset opens
	 * list-access calls to read the phonebook and the call list -- call
	 * class 0010 in <<BASIC-SERVICE>>, answered over {CC-IWU-INFO}, no
	 * U-plane anywhere in it -- and the A540 placed 145 of those in our
	 * corpus against 2 voice calls. We were binding the audio path to
	 * every one of them, which is both wrong and the reason the chip
	 * state in vF413/vF418 was measured on connections that carried no
	 * speech service at all. `incoming` means we originated towards the
	 * PP; there is no peer <<BASIC-SERVICE>> to read on those and we only
	 * ever originate speech calls, so they bind as before.
	 */
	if (!s->base.main_link.bound && !s->reg.nonspeech_noted &&
	    !s->base.main_link.link.cc.incoming &&
	    !dect_cc_peer_is_speech(&s->base.main_link.link.cc) &&
	    s->base.main_link.link.cc.state == DECT_CC_ST_ACTIVE) {
		s->reg.nonspeech_noted = 1;
		printf("call: not a speech call (<<BASIC-SERVICE>> %02x) -- "
		       "no audio path bound\n",
		       s->base.main_link.link.cc.peer_basic_service);
	}

	if (!s->base.main_link.bound && call_auth_ok(s, &s->base.main_link.link) &&
	    (s->base.main_link.link.cc.state == DECT_CC_ST_ACTIVE ||
	     (s->base.cfg.bind_first && s->base.main_link.connect_staged &&
	      s->base.main_link.link.cc.state == DECT_CC_ST_PROCEEDING)) &&
	    (s->base.main_link.link.cc.incoming ||
	     dect_cc_peer_is_speech(&s->base.main_link.link.cc))) {
		s->base.main_link.path_on = 0;
		s->reg.remode_at = 0;
		s->reg.remode_done = 0;
		s->reg.seq_t0 = now;	/* seq window 0 starts now */
		s->reg.seq_idx = 0;
		s->reg.bind_ms = now;
		s->reg.status_ms = 0;
		s->reg.have_counters = 0;
		memset(&s->reg.last_status, 0, sizeof(s->reg.last_status));
		s->reg.frames_total = 0;
		s->reg.frames_last = 0;
		/*
		 * Wideband when the call's codec is G.722 -- named by the
		 * handset in its ALERTING/CONNECT, or by our CONNECT -- and
		 * the bind is ours to choose (flags= with bit 6 set is a lab
		 * run that picked its own).
		 */
		s->reg.call_wideband = !s->base.cfg.narrowband &&
			!(s->audio.flags & 0x40u) &&
			dect_cc_codec(&s->base.main_link.link.cc) == DECT_CC_CODEC_G722;
		printf("call: codec %s -- %s bind\n",
		       dect_cc_codec(&s->base.main_link.link.cc) == DECT_CC_CODEC_G722 ?
		       "G.722" : dect_cc_codec(&s->base.main_link.link.cc) ==
		       DECT_CC_CODEC_G726 ? "G.726" : "not named (basic speech)",
		       s->reg.call_wideband ? "wideband (coded G.722 lane)" :
		       (s->audio.flags & 0x40u) ? "the configured" :
		       "narrowband (mode 2, linear PCM)");
		send_audio_bind(s, s->base.main_link.handle);
		s->base.main_link.bound = 1;
		s->reg.nonspeech_noted = 0;
		/* take the phase from this call's first received transition */
		s->play_locked = 0;
		s->rx_word_prev = 0xFFFFFFFFu;
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;		/* start the voice-slot probe */
		printf("call: connected (handle %u) -- audio channel assigned\n",
		       s->base.main_link.handle);

		/*
		 * Start call ciphering: challenge the handset with the stored UAK so
		 * both sides derive the DCK, then ask it to switch on DSC. The chip
		 * cipher-enable (0x0208) only goes out when -k asks for it, its field
		 * mapping being unproven [CR A-0028 §1].
		 *
		 * -C suppresses all of this. The handset ciphers the whole bearer --
		 * voice included -- the moment it sees our CIPHER-REQUEST, and we
		 * cannot decipher, so a captured voice slot is ciphertext from ~400 ms
		 * into the call. Suppressing the request keeps the call in the clear
		 * long enough to work out the sample format, at the price of the
		 * handset dropping it with its unencrypted-connection complaint.
		 */
		start_call_cipher(s, now);
	}

}

/* dect_base's side_call(): side link k's lane and dialling. */
static void base_side_call(void *ctx, unsigned k, uint64_t now)
{
	struct stubd *s = ctx;

	side_call_tick(s, &s->side[k], now);
}

/*
 * dect_base's call_down(): the main call is over, its audio channel goes
 * (dect_base_tick(); loop_tick() does the same in its own order).
 */
static void base_call_down(void *ctx)
{
	struct stubd *s = ctx;

	send_audio_disconnect(s, s->base.main_link.handle);
	s->reg.call_wideband = 0;
}

/* dect_base's swapped(): side link k and the main link changed places. */
static void base_swapped(void *ctx, unsigned k)
{
	struct stubd *s = ctx;

	dect_link_set_tx_log(&s->base.main_link.link, log_nwk_out, s);
	dect_link_set_tx_log(&s->side[k].bl->link, log_nwk_out_side, s);
	s->reg.call_wideband = 0;
	s->reg.nonspeech_noted = 0;
}

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
static FILE *fopen_private(const char *path)
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
 * The main loop: read the receive blocks, decode the mails, answer each
 * block with a transmit block, drive the session and every timer, and
 * (re)send the contact. Parks the radio before it returns.
 */
/* -------------------------------------------------------------------------
 * Cell mode (-K): the box is a cell of an openDECTcell controller
 * (opendectcell-controller docs/cell-protocol.md). The controller runs the DECT
 * stack; here every chip mail goes up verbatim and every MAIL_DOWN goes to
 * the chip. dect-cell holds the TLS link and talks to us on a local socket
 * in the protocol's own frames (§14.2). Nothing here turns the radio on:
 * it is parked once the chip answers, and only a MAIL_DOWN from the
 * controller switches it on (§6.4, §7.4).
 */
#define CELL_PATH		"/var/run/opendectcell-cell.sock"
#define CELL_GRACE_MS		7000u	/* §6.4 grace_ms */
#define CELL_ORPHAN_MS		60000u	/* §6.4 orphan_s */
#define CELL_FLOW_HIGH		2048u	/* §9.3: FLOW state 1 above this */
#define CELL_FRAMES_PER_TICK	32	/* the rest waits for the next pass */
#define CELL_STATE_PATH		DECT_RUN_DIR "/cell.json"

static void cell_open(struct stubd *s)
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

static void cell_close(struct stubd *s)
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

static void cell_status(struct stubd *s, uint64_t now)
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
static void cell_media_tx(struct stubd *s, uint8_t *fr, const uint8_t *in)
{
	unsigned i;

	for (i = 0; s->cell.media_n && i < AB_LANES; i++) {
		unsigned w = s->cell.media[i].word;

		if (s->cell.media[i].open)
			ab_frame(&s->ab.l[i], fr, w, in ? in + w * 2u : NULL);
	}
}

/* Every mail from the chip, up as it came (§7.1), with the links it opens. */
static void cell_mail_up(struct stubd *s, const struct dect_mail *m,
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

static void cell_tick(struct stubd *s, uint64_t now)
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
static int cell_rfpi_note(struct stubd *s, const struct dect_mail *m)
{
	if (m->prim != DECT_PRIM_CONTACT_ACK1 || m->param_len < 5u)
		return 0;
	if (s->cell.have_rfpi && !memcmp(s->cell.rfpi, m->param, 5))
		return 0;
	memcpy(s->cell.rfpi, m->param, 5);
	s->cell.have_rfpi = 1;
	return 1;
}

static void rfpi_check(const struct stubd *s, const struct dect_mail *m)
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

/*
 * The -a keys whose value is a word, a path or carries a ':', not a
 * number. Each needs its value: given bare, the parser used to read
 * through a NULL '=' (security audit 2026-10-29, F5).
 */
static const char *const audio_word_keys[] = {
	"seqstep", "play", "playpcm", "playraw", "led",
	"statefile", "profiles", "ctl", "linename", "remote",
	"clip", "memdump", "latemail", "timesync", "codeclist", "infoie",
	"lane2", "lane3", "cnip", "rfpicheck",
};

static int audio_word_key(const char *key, size_t klen)
{
	size_t i;

	for (i = 0; i < sizeof(audio_word_keys) / sizeof(audio_word_keys[0]);
	     i++)
		if (strlen(audio_word_keys[i]) == klen &&
		    !memcmp(key, audio_word_keys[i], klen))
			return 1;
	return 0;
}

/* The value of an -a key=value pair as a string: eq is its '=', end its end. */
static char *audio_value(const char *eq, const char *end)
{
	size_t n = (size_t)(end - (eq + 1));
	char *v = malloc(n + 1);

	if (v) {
		memcpy(v, eq + 1, n);
		v[n] = '\0';
	}
	return v;
}

/*
 * Parse -a key[=value][,key[=value]...] into struct audio_cfg. Returns 0 on
 * success, -1 on a malformed spec.
 */
/*
 * An -a key we do not know is ignored, not fatal: a key an older release
 * took (a lab option since removed) may linger in the UCI options, and
 * must not keep the base off the air.
 */
static void audio_unknown(const char *key, size_t klen)
{
	fprintf(stderr, "dect-stubd: -a %.*s: unknown key, ignored\n",
		(int)klen, key);
}

static int parse_audio(struct audio_cfg *a, const char *spec)
{
	while (*spec) {
		const char *key = spec, *eq;
		unsigned long v = 0;
		size_t klen;
		char *end;

		while (*spec && *spec != ',')
			spec++;
		eq = memchr(key, '=', (size_t)(spec - key));
		klen = eq ? (size_t)(eq - key) : (size_t)(spec - key);
		if (!eq && audio_word_key(key, klen))
			return -1;
		if (eq) {
			v = strtoul(eq + 1, &end, 0);
			/* These carry a ':', a path or a word, not a number. */
			if (end != spec && !audio_word_key(key, klen))
				return -1;
		}
		if (eq && klen == 8 && !memcmp(key, "slotmask", 8)) {
			a->slot_mask = (uint8_t)v;
			a->slot_mask_fixed = 1;
		}
		else if (!eq && klen == 9 && !memcmp(key, "noslotcfg", 9))
			a->no_slot_cfg = 1;
		else if (eq && klen == 6 && !memcmp(key, "attcfg", 6))
			a->att_cfg = (uint8_t)v;
		else if (eq && klen == 7 && !memcmp(key, "attflag", 7))
			a->att_flag = (uint8_t)v;
		else if (eq && klen == 5 && !memcmp(key, "ipuio", 5))
			a->ipuio = (uint32_t)v, a->ipuio_set = 1;
		else if (klen == 7 && !memcmp(key, "regopen", 7))
			a->reg_open = 1, a->reg_window_s = eq ? (unsigned)v : 0u;
		else if (klen == 9 && !memcmp(key, "regwindow", 9))
			a->reg_signal_s = (unsigned)v;
		else if (!eq && klen == 8 && !memcmp(key, "twocalls", 8))
			a->two_calls = 1;
		else if (!eq && klen == 7 && !memcmp(key, "locauth", 7))
			a->locauth = 1;
		else if (!eq && klen == 9 && !memcmp(key, "nolocauth", 9))
			a->locauth = 0;
		else if (!eq && klen == 9 && !memcmp(key, "authfirst", 9))
			a->auth_first = 1;
		else if (!eq && klen == 11 && !memcmp(key, "noauthfirst", 11))
			a->auth_first = 0;
		else if (!eq && klen == 5 && !memcmp(key, "noctl", 5))
			a->no_ctl = 1;
		else if (eq && klen == 5 && (!memcmp(key, "lane2", 5) ||
					     !memcmp(key, "lane3", 5))) {
			unsigned ch, w, k = key[4] == '2' ? 0u : 1u;

			/* channel 1..3 (A-0124: three voice channels) */
			if (sscanf(eq + 1, "%u:%u", &ch, &w) != 2 || !ch ||
			    ch > 3 || w > 63)
				return -1;
			a->lane_ch[k] = (uint8_t)ch;
			a->lane_word[k] = (uint8_t)w;
			a->lane_set[k] = 1;
		}
		else if (eq && klen == 7 && !memcmp(key, "subslot", 7)) {
			if (v < 1u || v > DECT_MM_MAX_SLOT)
				return -1;
			a->sub_slot = (unsigned)v;
		}
		else if (eq && klen == 9 && !memcmp(key, "codeclist", 9)) {
			const char *h = eq + 1;
			size_t n = 0;

			while (h < spec && n < sizeof(a->codeclist)) {
				char buf[3];
				char *he;

				if (spec - h < 2)
					return -1;
				buf[0] = h[0];
				buf[1] = h[1];
				buf[2] = 0;
				a->codeclist[n] = (uint8_t)strtoul(buf, &he, 16);
				if (*he)
					return -1;
				n++;
				h += 2;
			}
			if (h != spec || n < 4)
				return -1;	/* a list needs at least one codec */
			a->codeclist_len = n;
		}
		else if (eq && klen == 6 && !memcmp(key, "infoie", 6)) {
			const char *h = eq + 1;
			size_t n = 0;

			while (h < spec && n < sizeof(a->info_ie)) {
				char buf[3];
				char *he;

				if (spec - h < 2)
					return -1;
				buf[0] = h[0];
				buf[1] = h[1];
				buf[2] = 0;
				a->info_ie[n] = (uint8_t)strtoul(buf, &he, 16);
				if (*he)
					return -1;
				n++;
				h += 2;
			}
			if (h != spec || n < 2)
				return -1;	/* at least one element */
			a->info_ie_len = n;
		}
		else if (!eq && klen == 6 && !memcmp(key, "capall", 6))
			a->cap_all = 1;
		else if (!eq && klen == 8 && !memcmp(key, "loopback", 8))
			a->loopback = 1;
		else if (klen == 4 && !memcmp(key, "ring", 4))
			a->ring_ms = (unsigned)v;
		else if (klen == 10 && !memcmp(key, "ringattach", 10))
			a->ring_attach_ms = (unsigned)v;
		else if (klen == 8 && !memcmp(key, "basicsvc", 8))
			a->basic_svc = (uint8_t)v, a->basic_svc_set = 1;
		else if (!eq && klen == 10 && !memcmp(key, "narrowband", 10))
			a->narrowband = 1;
		else if (!eq && klen == 9 && !memcmp(key, "nooverlap", 9))
			a->no_overlap = 1;
		else if (klen == 8 && !memcmp(key, "dialwait", 8))
			a->dial_wait_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "alertms", 7))
			a->alert_ms = (unsigned)v;
		else if (!eq && klen == 9 && !memcmp(key, "bindfirst", 9))
			a->bind_first = 1;
		else if (!eq && klen == 3 && !memcmp(key, "nlr", 3))
			a->nlr = 1;
		else if (!eq && klen == 5 && !memcmp(key, "nonlr", 5))
			a->nlr = 0;
		else if (!eq && klen == 13 && !memcmp(key, "allcallstatus", 13))
			a->all_call_status = 1;
		else if (!eq && klen == 12 && !memcmp(key, "nocallstatus", 12))
			a->no_call_status = 1;
		else if (klen == 10 && !memcmp(key, "setuptries", 10)) {
			/*
			 * How long to keep re-offering a CC-SETUP the local CC
			 * entity refuses because the handset's own call is up.
			 *
			 * The default 8 x 2 s = 16 s is SHORTER than our own
			 * F<CC.03> setup timer (20 s), so the ring always gave
			 * up before the timer that would clear a stuck
			 * transaction could fire -- which is exactly how F4.36b
			 * came back void. The A540 also opens a CAT-iq list
			 * access call roughly every 17 s and holds it ~13 s,
			 * so a budget under ~20 s can miss every window.
			 */
			if (v < 1u || v > 200u)
				return -1;
			a->setup_tries = (unsigned)v;
		}
		else if (klen == 8 && !memcmp(key, "timesync", 8)) {
			if (!eq)
				return -1;
			if (!strncmp(eq + 1, "auto", 4))
				a->timesync = TIMESYNC_AUTO;
			else if (!strncmp(eq + 1, "force", 5))
				a->timesync = TIMESYNC_FORCE;
			else if (!strncmp(eq + 1, "off", 3))
				a->timesync = TIMESYNC_OFF;
			else
				return -1;
		}
		else if (klen == 10 && !memcmp(key, "timeupdate", 10)) {
			/* minutes, up to 30 days */
			if (!eq || v > 43200u)
				return -1;
			a->time_every_min = (unsigned)v;
		}
		else if (klen == 6 && !memcmp(key, "signal", 6)) {
			a->signal_value = (uint8_t)v;
			a->signal_on = 1;
			a->signal_set = 1;
		}
		else if (!eq && klen == 8 && !memcmp(key, "nosignal", 8))
			a->signal_on = 0;	/* control runs only, see run 18 */
		else if (!eq && klen == 8 && !memcmp(key, "playsync", 8))
			a->play_sync = 1;
		else if (klen == 9 && !memcmp(key, "memdumpms", 9))
			a->memdump_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "memdump", 7)) {
			unsigned long len = 32;

			if (a->n_memdump >= sizeof(a->memdump) /
					    sizeof(a->memdump[0])) {
				fprintf(stderr, "too many memdump= regions "
					"(max %zu)\n",
					sizeof(a->memdump) /
					sizeof(a->memdump[0]));
				return -1;
			}
			if (!eq)
				return -1;
			if (*end == ':')
				len = strtoul(end + 1, &end, 0);
			if (end != spec || !len || len > 0x10000u)
				return -1;
			a->memdump[a->n_memdump].addr = (uint32_t)v;
			a->memdump[a->n_memdump].len = (uint32_t)len;
			a->n_memdump++;
		}
		else if (klen == 6 && !memcmp(key, "lineid", 6)) {
			if (v >= DECT_CALLINFO_LINE_NONE)
				return -1;
			a->callinfo.has_line = 1;
			a->callinfo.line_id = (uint8_t)v;
		}
		else if (klen == 8 && !memcmp(key, "linetype", 8)) {
			if (v > 3u)
				return -1;
			a->callinfo.has_line_type = 1;
			a->callinfo.line_type = (uint8_t)v;
		}
		else if (klen == 6 && !memcmp(key, "callid", 6)) {
			if (v > 0x7Fu)
				return -1;
			a->callinfo.has_call_id = 1;
			a->callinfo.call_id = (uint8_t)v;
		}
		else if (klen == 10 && !memcmp(key, "callstatus", 10)) {
			if (v > 0x7Fu)
				return -1;
			a->callinfo.has_call_status = 1;
			a->callinfo.call_status = (uint8_t)v;
		}
		else if ((klen == 4 && !memcmp(key, "clip", 4))
			 || (klen == 4 && !memcmp(key, "cnip", 4))) {
			/* text, not a number: take the value verbatim */
			char *text = audio_value(eq, spec);

			if (!text)
				return -1;
			if (key[1] == 'l')
				a->clip_number = text;
			else
				a->clip_name = text;
		}
		else if ((klen == 4 && !memcmp(key, "play", 4))
			 || (klen == 7 && !memcmp(key, "playpcm", 7))
			 || (klen == 7 && !memcmp(key, "playraw", 7))) {
			/* value is a path, not a number: take it verbatim */
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->play_path = path;
			a->play_pcm = (klen == 7 && key[4] == 'p');
			a->play_raw = (klen == 7 && key[4] == 'r');
		}
		else if (klen == 8 && !memcmp(key, "toneword", 8)) {
			a->lane0_word = (uint8_t)v;
			a->lane0_word_set = 1;
		}
		else if (klen == 6 && !memcmp(key, "remode", 6)) {
			a->remode_ms = (unsigned)v;
			a->remode_set = 1;
		} else if (eq && klen == 8 && !memcmp(key, "linename", 8)) {
			char *name = audio_value(eq, spec);

			if (!name)
				return -1;
			a->line_name = name;
		} else if (eq && klen == 9 && !memcmp(key, "rfpicheck", 9)) {
			unsigned k, b;

			if (spec - (eq + 1) != 10)
				return -1;
			for (k = 0; k < 5u; k++) {
				if (sscanf(eq + 1 + 2u * k, "%2x", &b) != 1)
					return -1;
				a->rfpi_check[k] = (uint8_t)b;
			}
			a->rfpi_check_set = 1;
		} else if (eq && klen == 3 && !memcmp(key, "ctl", 3)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->ctl_path = path;
		} else if (eq && klen == 8 && !memcmp(key, "profiles", 8)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->profile_dir = path;
		} else if (eq && klen == 9 && !memcmp(key, "statefile", 9)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->state_path = path;
		} else if (eq && klen == 6 && !memcmp(key, "remote", 6)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->remote_path = path;
		} else if (klen == 10 && !memcmp(key, "fetchdelay", 10)) {
			a->fetch_delay_ms = (unsigned)v;
		} else if (klen == 8 && !memcmp(key, "latemail", 8)) {
			const char *rest = end + 1;
			size_t rl = (size_t)(spec - rest);
			struct dect_mail chk;

			if (*end != ':' || !rl || a->n_latemail >=
			    sizeof(a->latemail) / sizeof(a->latemail[0]) ||
			    rl >= sizeof(a->latemail[0].spec))
				return -1;
			memcpy(a->latemail[a->n_latemail].spec, rest, rl);
			a->latemail[a->n_latemail].spec[rl] = 0;
			if (parse_mail_spec(a->latemail[a->n_latemail].spec,
					    &chk))
				return -1;
			a->latemail[a->n_latemail].ms = (unsigned)v;
			a->n_latemail++;
		} else if (klen == 12 && !memcmp(key, "installdummy", 12)) {
			if (v < 1 || v > DECT_MM_MAX_SLOT)
				return -1;
			a->install_dummy = (unsigned)v;
		} else if (klen == 10 && !memcmp(key, "tpuioffset", 10)) {
			if (v > 200)
				return -1;
			a->tpui_offset = (unsigned)v;
		} else if (klen == 10 && !memcmp(key, "keycheckms", 10)) {
			a->keycheck_ms = (unsigned)v;
			a->keycheck_set = 1;
		} else if (klen == 8 && !memcmp(key, "locvalid", 8)) {
			if (v > 255)
				return -1;
			a->locate_validity = (unsigned)v;
		} else if (!eq && klen == 10 && !memcmp(key, "nochipkeys", 10)) {
			a->no_chip_keys = 1;
		} else if (klen == 8 && !memcmp(key, "holdback", 8)) {
			a->holdback_ms = (unsigned)v;
		} else if (!eq && klen == 11 && !memcmp(key, "nostatefile", 11)) {
			a->no_state = 1;
		} else if (eq && klen == 3 && !memcmp(key, "led", 3)) {
			char *path = audio_value(eq, spec);

			if (!path)
				return -1;
			a->led_path = path;
		} else if (!eq && klen == 5 && !memcmp(key, "noled", 5)) {
			a->no_led = 1;
		} else if (!eq && klen == 8 && !memcmp(key, "noremode", 8)) {
			a->remode_ms = 0;
			a->remode_set = 1;
		}
		else if (klen == 3 && !memcmp(key, "seq", 3))
			a->seq_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "seqstep", 7)) {
			if (a->n_seq >= sizeof(a->seq_step) /
					sizeof(a->seq_step[0]))
				return -1;
			a->seq_step[a->n_seq++] = eq + 1;
		}
		else if (!eq)
			audio_unknown(key, klen);	/* or its value missing */
		else if (klen == 3 && !memcmp(key, "sel", 3)) {
			if (v < 1u || v > 6u)	/* [CR A-CodecModes-3] */
				return -1;
			a->sel = (uint8_t)v;
		}
		else if (klen == 4 && !memcmp(key, "gain", 4))
			a->gain = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "flags", 5))
			a->flags = (uint8_t)v;
		else if (klen == 4 && !memcmp(key, "mask", 4))
			a->mask = (uint16_t)v;
		else if (klen == 4 && !memcmp(key, "mode", 4))
			a->mode = (uint8_t)v;
		else if (klen == 5 && !memcmp(key, "codec", 5))
			a->codec = (uint8_t)v;
		else if (klen == 6 && !memcmp(key, "txidle", 6))
			a->tx_idle = (uint8_t)v, a->have_tx_idle = 1;
		else if (klen == 9 && !memcmp(key, "voiceidle", 9))
			a->tx_voice_idle = (uint8_t)v, a->have_voice_idle = 1;
		else if (klen == 6 && !memcmp(key, "status", 6))
			a->status_ms = (unsigned)v;
		else
			audio_unknown(key, klen);
		if (*spec == ',')
			spec++;
	}
	return 0;
}

/* Parse -k off. Returns 0 on success, -1 on a malformed spec. */
static int parse_chip_cipher(struct chip_cipher *cc, const char *spec)
{
	if (!strcmp(spec, "off"))
		cc->disabled = 1;
	else
		return -1;
	return 0;
}


static void usage(void)
{
	fprintf(stderr,
		"usage: dect-stubd [options]\n"
		"  -d DEV   device (default /dev/dect0)\n"
		"  -c N     slots per frame (default 8)\n"
		"  -T SLOT  transmit control slot (skip probing)\n"
		"  -R SLOT  receive control slot (skip probing)\n"
		"  -t SECS  run for SECS seconds then exit (default: until signal)\n"
		"  -v       log every mail (prim and length; parameters hidden)\n"
		"  -p       also log parameter bytes (may hold identity data; do\n"
		"           not share such logs) -- implies -v\n"
		"  -M SRC:DST:PRIM[:HEX]  after contact, send this mail (repeatable,\n"
		"           one per ~100 ms; e.g. the MAC bring-up sequence)\n"
		"  -b FILE  offer the handsets a Contact List (CAT-iq phonebook) from\n"
		"           FILE, re-read when it changes; what handsets change\n"
		"           is appended to FILE.changes (see dect_contacts.h)\n"
		"  -K       cell mode: a cell of an openDECTcell controller. No stack and\n"
		"           no handsets here: every chip mail goes to dect-cell on\n"
		"           " CELL_PATH ", the radio stays parked\n"
		"           until the controller turns it on (not with -g)\n"
		"  -g       base mode: bring up the radio (registration closed\n"
		"           unless -a regopen) and run the host C-plane stack for\n"
		"           the handsets (parks the radio on exit)\n"
		"  -P PIN   registration PIN / AC, 4 to 8 digits (default 0000;\n"
		"           used with -g); -P @FILE reads it from a file only\n"
		"           root may write\n"
		"  -s PATH  subscription store, holds the handset key; reloaded into\n"
		"           the chip at start-up (default /etc/opendectcell-sub, \"\" = off).\n"
		"           Repeat for every handset the base holds: all are served\n"
		"           and installed in their chip slots; -a ring= rings the\n"
		"           first, a bridge call-in the one it names\n"
		"  -k off   the call key for the chip (0x0208, stock's layout,\n"
		"           [CR A-0126]) goes out before every CIPHER-REQUEST; -k off\n"
		"           withholds it\n"
		"  -w FILE  capture the call's voice slot to FILE (raw, 2 bytes per\n"
		"           frame, while a call is bound) -- holds call audio, keep\n"
		"           it local\n"
		"  -V SLOT  capture only this slot (default: every slot, so the\n"
		"           voice can be found offline)\n"
		"  -C       do not start call ciphering. The handset ciphers the whole\n"
		"           bearer once it sees CIPHER-REQUEST, so captured voice is\n"
		"           ciphertext; -C keeps the call in the clear for audio work\n"
		"           (the handset then drops it as unencrypted)\n"
		"  -a SPEC  audio bind parameters, comma separated [CR A-0029]:\n"
		"           sel=1..6 gain=N (0x0251), flags=N mask=N mode=N codec=N\n"
		"           = 0x0230 {link, mode, mask, master, extra}: flags is the\n"
		"           mode byte (low nibble = channel, 0x40 = wideband class;\n"
		"           0x41 works, 0x40 alone binds nothing), mode is master and\n"
		"           codec is extra\n"
		"           timesync=auto|force|off  send <<TIME-DATE>> after a\n"
		"                    location registration. auto (default) only when\n"
		"                    the kernel says the clock is disciplined;\n"
		"                    force trusts it anyway; off never sends\n"
		"           timeupdate=MIN  also send it to every handset every\n"
		"                    MIN minutes, paging the ones without a\n"
		"                    link (0, the default: only after a location\n"
		"                    registration)\n"
		"           toneword=N the TDM word of the main call's speech\n"
		"           (default 0; on the SSC transport the lane after the\n"
		"           control lane)\n"
		"           txidle=N voiceidle=N set the transmit filler bytes\n"
		"           slotmask=N pins the 0x017c byte, the locked-in handset\n"
		"                     mask (default: computed from the slots with\n"
		"                     a record, [CR A-0129]); noslotcfg omits it\n"
		"           attcfg=N  0x020A config byte after the UAK (stock 0x01\n"
		"                     per Gigaset, 0x02 C4; default 0x00) [CR A-0113]\n"
		"           attflag=N 0x0247 install flag byte (stock bit0=1,\n"
		"                     A540 0x03; default 0x00) [CR A-0113]\n"
		"           ipuio=N   assign a registering handset IPUI O number N\n"
		"                     (up to 32 bits) instead of its IPUI N (B4a)\n"
		"           ring=MS calls the handset MS ms after start-up: page\n"
		"           (0x0300) then an FP-originated CC-SETUP\n"
		"           ringattach=MS the same, but MS ms after the handset's\n"
		"                     FIRST C-plane contact (unattended runs; a lost\n"
		"                     A540 re-locks anywhere from 2 s to minutes later)\n"
		"           play=FILE transmits a raw G.722 stream (8000 bytes/s) to\n"
		"           the handset while a call is up, looping -- a clean\n"
		"           downlink source, unlike the loopback\n"
		"           playpcm=FILE does the same from 16 kHz signed 16-bit\n"
		"           little-endian PCM, encoding it to G.722 at start-up\n"
		"           signal=N <<SIGNAL>> N in the SETUP (0x40..0x47 =\n"
		"           alerting pattern 0..7; default 0x41, external call)\n"
		"           nosignal     omit it: the A540 then rings silently\n"
		"           setuptries=N re-offers a refused CC-SETUP N times\n"
		"           codeclist=HEX <<CODEC-LIST>> value octets for CC-SETUP\n"
		"           (default none: basic speech means G.726)\n"
		"           infoie=HEX   encoded IEs sent once in a CC-INFO when the\n"
		"           handset rings on our call (e.g. <<MULTI-DISPLAY>>)\n"
		"           subslot=N    chip subscription slot 1..6 (TPUI + UAK slot)\n"
		"           locauth      authenticate a handset with its UAK inside its\n"
		"                        location registration (the default;\n"
		"                        nolocauth switches it off)\n"
		"           authfirst    authenticate a handset with its UAK before\n"
		"                        its call goes anywhere or an answer is\n"
		"                        passed on, and end the call on any failure\n"
		"                        (the default; noauthfirst: the old order)\n"
		"           linename=NAME the line's name in the Line Settings\n"
		"                        List (\"Line 1\"; no commas)\n"
		"           ctl=PATH     the control socket, default\n"
		"                        " DECT_CTL_PATH " (0600; noctl: none)\n"
		"           twocalls     up to three calls at once: a handset may call\n"
		"           or be called while another is in a call. Side links get\n"
		"           their own audio channel (2 -> TDM word 3, 3 -> word 2;\n"
		"           lane2=CH:WORD, lane3= to override) and their speech goes\n"
		"           through the bridge as the main call's does\n"
		"           regopen[=SECS] open registration at start-up (enrol a\n"
		"           handset; closed after SECS, default never). Without it\n"
		"           registration is closed and subscribed handsets attach\n"
		"           regwindow=SECS how long a window opened by SIGUSR1\n"
		"           (opendectcell.init register) stays open (default 120)\n"
		"           basicsvc=N sets <<BASIC-SERVICE>> (default 0x88: high\n"
		"           nibble call class, low nibble 8 = wideband speech;\n"
		"           0x80 with narrowband)\n"
		"           nocallstatus no <<CALL-INFORMATION>> call status in our\n"
		"           SETUP-ACK/CALL-PROC/ALERTING/CONNECT to a handset's call\n"
		"           nonlr        do not release the link after its last call or\n"
		"           list session (default: release, 0x0202, a DISC first\n"
		"           after list access, as stock)\n"
		"           allcallstatus call status to every handset, not only to\n"
		"           those reporting NG-DECT Part 3 (the default since API 34)\n"
		"           alertms=N    ALERTING after CALL-PROC, CONNECT N ms later\n"
		"           bindfirst    bind the audio after CALL-PROC, CONNECT once\n"
		"           it is up (EN 300 175-5 9.3.1.8)\n"
		"           nooverlap    CONNECT a handset's call at once instead of\n"
		"           SETUP-ACK, digits, then CONNECT dialwait=MS (3000)\n"
		"           after the last digit\n"
		"           narrowband   never wideband: SETUP basic speech, CONNECT\n"
		"           chooses G.726, every call gets the mode-2 bind. By\n"
		"           default a call whose codec is G.722 gets the wideband\n"
		"           bind (flags 0x41, codec byte 0) and a phase-locked\n"
		"           G.722 downlink (play=/playpcm=); playraw= lanes are\n"
		"           format-specific, so use narrowband with linear ones\n"
		"           memdump=ADDR[:LEN] sweeps chip memory with the\n"
		"           read-address debug mail, logging each reply; repeat\n"
		"           the key for several ranges, memdumpms=N sets the\n"
		"           interval (default 50). Polling starts at link-up, so\n"
		"           one run gives an idle baseline and the live state.\n"
		"           Read only; never writes chip memory.\n"
		"           playsync locks play= to the received lane's word boundary\n"
		"           in a narrowband call too (a wideband call always locks)\n"
		"           statefile=PATH writes the base's state as JSON, at most\n"
		"           once a second (default " STATE_DEFAULT_PATH ");\n"
		"           nostatefile turns it off\n"
		"           led=PATH drives this LED brightness file (default\n"
		"           " LED_DEFAULT_PATH ", the 7520's\n"
		"           Fon/DECT LED): on while the radio is up, ~5 Hz blink on\n"
		"           C-plane traffic or a call, off when parked; noled leaves\n"
		"           it alone\n"
		"           remode=MS sends a second 0x0230 with flag bit 6 clear\n"
		"           MS ms after 0x022D, asking for mode 2 (the narrowband\n"
		"           G.726 path, which takes signed 16-bit linear PCM at one\n"
		"           sample per frame on the lane). Default: 500 if flags has\n"
		"           bit 6 set, else none -- with bit 6 clear the chip\n"
		"           reaches mode 2 from the first assign, and a re-assign\n"
		"           only adds a burst of noise. Less than 500 races the\n"
		"           chip's own pass and is overwritten\n"
		"           noremode never sends the re-assign\n"
		"           seq=MS with repeated seqstep=FLAGS:CODEC walks a list of\n"
		"           0x0230 configurations, MS ms per window, starting when\n"
		"           the call binds -- the same instant playraw restarts the\n"
		"           lane file, so a file built with matching windows keeps\n"
		"           format and mode aligned. Window 0 is left as the bind\n"
		"           left it. Pair with memdump of the route words so every\n"
		"           window carries proof of the state it was really in.\n"
		"           playraw=FILE replays a verbatim TDM lane instead (two\n"
		"           bytes per frame, 16000 B/s, as -w captures it): no\n"
		"           cadence logic, nothing interpreted\n"
		"           clip=NUMBER cnip=NAME put a calling-party number\n"
		"           (0x6C, printable IA5) and name (0x6D, UTF-8) in the\n"
		"           FP-originated CC-SETUP, both presented as allowed and\n"
		"           user-provided-not-screened. Omitted options send no IE.\n"
		"           lineid=N (0..126) linetype=N (bit 0 VoIP delay, bit 1\n"
		"           in-band second calls) callid=N callstatus=N add a\n"
		"           <<CALL-INFORMATION>> (0x7E) to the FP-originated\n"
		"           CC-SETUP; CAT-iq 2.0 marks an external call by its line\n"
		"           id. Our one SIP line is lineid=1,linetype=1, the\n"
		"           line the line list names.\n"
		"           loopback echoes the received bearer word (toneword)\n"
		"           straight back to the handset, byte for byte -- the\n"
		"           lane carries coded G.722, so this needs no encoder\n"
		"           capall makes -w capture from start-up, not only while a\n"
		"           call is bound (for radio-off TDM probing)\n"
		"           status=MS polls the chip's per-link connection status\n"
		"           (0xF624, read-only) every MS ms while a call is bound and\n"
		"           prints its slot, carrier and signal levels\n"
		"           holdback=MS nochipkeys remote=PATH fetchdelay=MS\n"
		"           locvalid=N: the multi-cell tests M2 and M4\n"
		"           (docs/multicell-experiments.md)\n"
		"           latemail=MS:SRC:DST:PRIM[:HEX] send this raw mail MS ms\n"
		"           after the subscriptions were reloaded (repeatable, up\n"
		"           to 8, in list order); -M sends before the radio is on\n"
		"           installdummy=N send 0x0247 {N, 1, 0} after the slot\n"
		"                     clears, no key written (bench test, A-0134)\n"
		"           tpuioffset=N assign TPUI {0x00,0x11,slot+N} instead of the\n"
		"           slot's own (a TPUI that is not slot-derived, docs/more-than-six.md T-C)\n"
		"           keycheckms=MS how long the UAK read-back and slot audit\n"
		"           wait after the writes (default 0; 1000 on the 7412's\n"
		"           06.88): the key-write delay sweep of docs/more-than-six.md\n"
		"           Default: sel=1,gain=0x2b,flags=1,mask=0,mode=0,codec=0\n"
		"           (flags=0x41 is the wideband bind used at G7),\n"
		"           txidle=0xff,voiceidle=0xff\n");
}

/* ---- handset profiles (docs/handset-profiles.md) ----------------------- */

static void profile_say(void *ctx, unsigned line, const char *msg)
{
	const char *path = ctx;

	if (line)
		printf("profile: %s:%u: %s\n", path, line, msg);
	else
		printf("profile: %s: %s\n", path, msg);
}

/* Read and parse one file; 0 on success. */
static int profile_load(const char *path, struct dect_profile *p)
{
	static char text[16384];
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f) {
		printf("profile: %s: %s\n", path, strerror(errno));
		return -1;
	}
	n = fread(text, 1, sizeof(text), f);
	fclose(f);
	if (n == sizeof(text)) {
		printf("profile: %s: larger than %zu octets\n", path,
		       sizeof(text));
		return -1;
	}
	return dect_profile_parse(text, n, p, profile_say, (void *)path);
}

/*
 * dect-stubd --ctl LINE...: a client for the control socket, for scripts
 * and the bench (the box has no socat). Sends each LINE, prints what the
 * daemon writes back for a second after the last one, then exits.
 * OPENDECTCELL_CTL names another socket path.
 */
static int ctl_client(int n, char **lines)
{
	const char *path = getenv("OPENDECTCELL_CTL");
	struct sockaddr_un a;
	char buf[DECT_CTL_LINE_MAX];
	uint64_t until;
	int fd, i;

	if (!path)
		path = DECT_CTL_PATH;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(a.sun_path))
		return 2;
	strcpy(a.sun_path, path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "ctl: %s: %s\n", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	for (i = 0; i < n; i++) {
		size_t len = strlen(lines[i]);

		if (len + 1 > sizeof(buf)) {
			fprintf(stderr, "ctl: line %d too long\n", i + 1);
			close(fd);
			return 2;
		}
		/* one argument, one line: an embedded break would add a
		 * command (security audit 2026-10-29, F5) */
		if (strpbrk(lines[i], "\r\n")) {
			fprintf(stderr, "ctl: line %d holds a line break\n",
				i + 1);
			close(fd);
			return 2;
		}
		memcpy(buf, lines[i], len);
		buf[len++] = '\n';
		if (write(fd, buf, len) != (ssize_t)len) {
			fprintf(stderr, "ctl: write: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
	}
	until = mono_ms() + 1000u;
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint64_t now = mono_ms();
		ssize_t r;

		if (now >= until || poll(&pfd, 1, (int)(until - now)) <= 0)
			break;
		r = read(fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		fwrite(buf, 1, (size_t)r, stdout);
	}
	close(fd);
	return 0;
}

/*
 * dect-stubd --bridge-echo [ring [NUMBER [HS]]]: a test bridge without SIP
 * (docs/bridge-interface.md §5 steps 2-3). It answers every number a
 * handset dials -- ALERTING at once, CONNECT 1.5 s later -- and plays the
 * caller's own voice back half a second late, through the audio socket.
 * "ring" first rings the first handset as a call from NUMBER (default
 * 0301234567, name "Echo test"; HS: that handset, else the first) and
 * echoes once it is answered. Stops
 * when the daemon goes, or on SIGINT/SIGTERM.
 */
#define ECHO_DELAY	25u	/* 20 ms frames: 0.5 s */

static int bridge_echo(int argc, char **argv)
{
	const char *path = getenv("OPENDECTCELL_CTL");
	struct sockaddr_un a;
	char in[DECT_CTL_LINE_MAX * 2], line[DECT_CTL_LINE_MAX];
	static int16_t ring[ECHO_DELAY][AB_FRAME / 2u];
	unsigned rw = 0, rn = 0, answer_call = 0;
	uint64_t answer_at = 0;
	size_t il = 0;
	int fd, afd = -1, n;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path ? path : DECT_CTL_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		fprintf(stderr, "echo: %s: %s\n", a.sun_path, strerror(errno));
		return 1;
	}
	n = snprintf(line, sizeof(line), "bridge\n");
	if (argc >= 1 && !strcmp(argv[0], "ring"))
		n += snprintf(line + n, sizeof(line) - (size_t)n,
			      "call-in line=1 number=%s name=\"Echo test\" "
			      "sip=echo%s%s\n", argc >= 2 ? argv[1] : "0301234567",
			      argc >= 3 ? " hs=" : "", argc >= 3 ? argv[2] : "");
	if (write(fd, line, (size_t)n) != n)
		return 1;
	printf("echo: bridge up -- dial any number on a handset\n");
	fflush(stdout);
	while (!stop) {
		struct pollfd p[2] = { { .fd = fd, .events = POLLIN },
				       { .fd = afd, .events = POLLIN } };
		uint64_t now = mono_ms();

		if (answer_call && now >= answer_at) {
			n = snprintf(line, sizeof(line),
				     "remote-answered call=%u\n", answer_call);
			if (write(fd, line, (size_t)n) != n)
				break;
			printf("echo: call %u answered\n", answer_call);
			answer_call = 0;
		}
		if (poll(p, afd >= 0 ? 2 : 1, 100) < 0 && errno != EINTR)
			break;
		if (afd >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
			ssize_t r = recv(afd, ring[rw], sizeof(ring[0]),
					 MSG_DONTWAIT);

			if (r <= 0) {
				close(afd);
				afd = -1;
				printf("echo: audio closed\n");
			} else {
				rw = (rw + 1u) % ECHO_DELAY;
				if (rn < ECHO_DELAY)
					rn++;
				/* the frame from half a second ago */
				if (rn == ECHO_DELAY)
					(void)send(afd, ring[rw], (size_t)r,
						   MSG_DONTWAIT | MSG_NOSIGNAL);
			}
		}
		if (!(p[0].revents & (POLLIN | POLLHUP)))
			continue;
		{
			ssize_t r = read(fd, in + il, sizeof(in) - il - 1u);
			char *nl;

			if (r <= 0)
				break;
			il += (size_t)r;
			while ((nl = memchr(in, '\n', il)) != NULL) {
				struct dect_ctl_msg m;
				size_t l = (size_t)(nl - in);
				unsigned id;

				printf("echo: << %.*s\n", (int)l, in);
				if (!dect_ctl_parse(in, l, &m) &&
				    !dect_ctl_get_uint(&m, "call", 1, 127, &id)) {
					if (!strcmp(m.verb, "call-out")) {
						n = snprintf(line, sizeof(line),
							"remote-ringing call=%u\n", id);
						if (write(fd, line, (size_t)n) != n)
							stop = 1;
						answer_call = id;
						answer_at = mono_ms() + 1500u;
					} else if (!strcmp(m.verb, "audio") &&
						   dect_ctl_get(&m, "path") &&
						   afd < 0) {
						struct sockaddr_un aa;

						memset(&aa, 0, sizeof(aa));
						aa.sun_family = AF_UNIX;
						snprintf(aa.sun_path,
							 sizeof(aa.sun_path), "%s",
							 dect_ctl_get(&m, "path"));
						afd = socket(AF_UNIX,
							     SOCK_SEQPACKET, 0);
						/* the connection names its call */
						n = snprintf(line, sizeof(line),
							     "call=%u", id);
						if (afd >= 0 &&
						    (connect(afd, (struct sockaddr *)&aa,
							     sizeof(aa)) ||
						     send(afd, line, (size_t)n,
							  MSG_NOSIGNAL) != n)) {
							close(afd);
							afd = -1;
						}
						rn = rw = 0;
						printf("echo: audio %s\n",
						       afd >= 0 ? "connected"
								: "FAILED");
					} else if (!strcmp(m.verb, "released") &&
						   afd >= 0) {
						close(afd);
						afd = -1;
					}
				}
				memmove(in, nl + 1, il - l - 1u);
				il -= l + 1u;
			}
			if (il == sizeof(in) - 1u)
				il = 0;
			fflush(stdout);
		}
	}
	if (afd >= 0)
		close(afd);
	close(fd);
	return 0;
}

/*
 * dect-stubd --check-profile FILE [FIRMWARE]: validate one profile without
 * the radio, and for a device profile its manufacturer layer, looked up
 * beside it as <extends>.profile; print what it resolves to.
 */
static int check_profile(const char *path, const char *fw)
{
	static struct dect_profile p, mfr;
	struct dect_hs_profile hp;
	const struct dect_profile *m = NULL;
	char desc[1024], mpath[512];
	const char *slash;

	if (profile_load(path, &p)) {
		printf("profile: %s: rejected\n", path);
		return 1;
	}
	if (p.kind == DECT_PROFILE_DEVICE) {
		slash = strrchr(path, '/');
		snprintf(mpath, sizeof(mpath), "%.*s%s.profile",
			 slash ? (int)(slash - path + 1) : 0, path, p.extends);
		if (profile_load(mpath, &mfr) ||
		    mfr.kind != DECT_PROFILE_MANUFACTURER ||
		    strcmp(mfr.id, p.extends)) {
			printf("profile: %s: its manufacturer profile %s is "
			       "missing or bad\n", path, mpath);
			return 1;
		}
		m = &mfr;
	}
	if (p.kind == DECT_PROFILE_MANUFACTURER)
		dect_profile_resolve(&p, NULL, fw, &hp);
	else
		dect_profile_resolve(m, &p, fw, &hp);
	printf("profile: %s: %s \"%s\"%s%s%s, %u firmware section(s)\n",
	       path, p.id, p.name, m ? " (via " : "", m ? m->id : "",
	       m ? ")" : "", p.n_sec - 1u);
	if (dect_profile_describe(&hp, desc, sizeof(desc)))
		printf("profile: resolved%s%s: %s\n", fw ? " for firmware " : "",
		       fw ? fw : "", desc);
	else
		printf("profile: resolved: the standard defaults\n");
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
	s.pin = "0000";		/* the lab's; the library has no default */
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
	 * while it runs is open; a handset decides (docs/bench-7412.md).
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

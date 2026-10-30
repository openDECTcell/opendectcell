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
#define STUBD_LINE_ID	1u

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
	int      loaded;
	uint64_t last_seen_ms;	/* runtime only: last segment from it */
	int      remote;	/* remote=: a roamer the cell must fetch */
	int      absent;	/* the file does not exist: free for enrolment */
	int      fetched;	/* ... and has, this run */
	/* runtime only: its <<TERMINAL-CAPABILITY>>, once seen (cap_tick) */
	int      cap_known;
	int      cap_part3;	/* NG-DECT Part 3: call status allowed */
	int      cap_ng;	/* NG-DECT Part 1 or 3: not a pure GAP handset */
	/*
	 * The name a handset gave it in the Internal Names List (TS 102 527-3
	 * §7.4.10.5.8), kept in <path>.name; empty = the default "DECT n".
	 */
	char     name[DECT_IWU_NAME_MAX];
	/* its lines' names, Line 2 only when its account is on (lines_load) */
	char     line_name[2][DECT_IWU_NAME_MAX];
	int      line2;
	/*
	 * Its handset profile (docs/handset-profiles.md): the model the user
	 * picked, in <path>.profile (the id, and optionally the firmware
	 * version on a second line), resolved at startup and on SIGHUP.
	 * Defaults when there is none.
	 */
	char     prof_id[DECT_PROFILE_ID_MAX];
	char     prof_fw[24];
	struct dect_hs_profile prof;
	/*
	 * Voice messages waiting per line (TS 102 527-3 §7.4.1.2), as the
	 * bridge reported its accounts' message summaries: the counts, the
	 * lines we have one for, and those the handset has not heard yet.
	 */
	unsigned mwi[2];
	uint8_t  mwi_known, mwi_due;	/* bit n-1: line n */
	/*
	 * Missed call notifications (TS 102 527-3 §7.4.1.3) the handset has
	 * not heard yet, per line, and those for a call just missed (subtype
	 * 01H, "a new external missed call just arrived").
	 */
	uint8_t  mc_due, mc_new;	/* bit n-1: line n */
};

/*
 * A second connection while the main link is busy. It gets a link of its own
 * for location registration and list access -- the gates and adoption work
 * as on the main link -- but no audio, no ringing and no learning: those
 * stay with the main link. Runs 25-28 refused 1-6 connection-ups per call,
 * handsets that wanted to locate or sync their lists while we were on a
 * call.
 */
#define SIDE_MAX	3u

/* control socket clients at once: the bridge and a few one-shot ones */
#define CTL_CLIENTS	4u

struct side_link {
	struct dect_base_link bl;	/* up, its handle, its C-plane stack */
	int      adopted;	/* handset table index, -1 = nobody yet */
	uint32_t rejected_seen;
	int      lane;		/* -a twocalls: its xlane, -1 = none */
	uint32_t located_seen;	/* mm.stats.located already answered */
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
#define IC_PARTY_NONE	(-2)	/* an internal-call party with no link yet */

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

/*
 * A call the bridge carries (docs/bridge-interface.md §2): its id, the link
 * it is on and the events already sent.
 */
#define BC_MAX		3u	/* the chip's audio channels */
#define BC_PAGING	(-3)	/* a side ring paging its handset, no link yet */
#define BC_NONE		(-4)	/* no link */

struct bcall {
	unsigned id;		/* 0 = free */
	int      party;		/* -1 the main link, else a side index */
	unsigned line;		/* the pool line, 1 or 2 */
	/*
	 * The SIP side is narrowband (a caller who offered no G.722, a far
	 * end that answered G.711): the handset's leg is too, so neither the
	 * box nor baresip converts.
	 */
	int      narrow;
	int      out;		/* 1: the handset called out, 0: call-in */
	int      busy_seen;	/* the CC call existed */
	int      rang, answered;	/* events sent */
	uint64_t since;
	/*
	 * A call-in rung on a side link (a side ring): its handset (table
	 * index), paged while BC_PAGING, and the caller it is shown.
	 */
	int      target;
	uint8_t  tpui[3];
	unsigned pages;
	uint64_t page_at;
	struct dect_clip clip;
	int      info_sent;	/* its display text sent */
	int      cs_sent;	/* ... and its CS call connect */
	/*
	 * Parallel calls on one link, to a Part 3 handset (TS 102 527-3
	 * §7.4.3): par, a call riding another's transaction in {CC-INFO} (a
	 * waiting call, or one placed with 1CH 15H); for every call its id on
	 * the handset and what the handset was told of it last (DECT_CS_*).
	 * The lane carries the one in CS call connect.
	 */
	int      par;
	uint8_t  dect_id, cs;
	int      dtmf;		/* the link's keypad is this call's DTMF */
	int      dialing;	/* 1CH 15H: the number still coming */
	uint64_t dial_ms;	/* its last digit */
	char     num[DECT_CC_KEYPAD_MAX + 1];
};

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
	/*
	 * An internal call (GAP N.31): a handset's SETUP with call class 9,
	 * the target's slot in its digits. We page the target, call it on
	 * the side link it answers on, and bridge the two channels on the
	 * host (clean room A-0124: stock does the same). Needs -a twocalls.
	 */
	struct {
		enum { IC_IDLE, IC_PAGING, IC_SETUP, IC_RINGING, IC_UP } st;
		int      caller;	/* -1 the main link, else a side index */
		int      target;	/* handset table index */
		int      tside;		/* the target's link: as caller, or IC_NONE */
		uint8_t  tpui[3];
		unsigned pages;
		uint64_t page_at;
		int      warned;	/* codec mismatch reported */
		int      name_info;	/* the caller's name follows in {CC-INFO} */
	} ic;
	/*
	 * Deregistering a handset (maintainer, 2026-10-28; stock's order from
	 * [CR A-0128]): ACCESS-RIGHTS-TERMINATE-REQUEST on its link, paging it
	 * first if it has none, then the erasure -- also without an answer,
	 * so a lost or broken handset still goes.
	 */
	struct {
		enum { DEREG_IDLE, DEREG_PAGING, DEREG_REQUESTED } st;
		int      target;	/* handset table index */
		uint8_t  tpui[3];
		unsigned pages;
		uint64_t page_at;
		unsigned queue[DECT_MM_MAX_SLOT];	/* slots asked meanwhile */
		unsigned nq;
	} dereg;
	/*
	 * The periodic date and time (-a timeupdate=MIN): one round over all
	 * handsets every MIN minutes, one handset at a time -- on its link
	 * if it has one, else paged first (TS 102 527-3 §7.4.2.1).
	 */
	struct {
		enum { TIME_IDLE, TIME_PAGING, TIME_SENT } st;
		uint64_t next_round;	/* 0 = not scheduled yet */
		int      in_round;
		unsigned next;		/* handset table index to visit next */
		int      target;
		uint8_t  tpui[3];
		unsigned pages;
		uint64_t page_at;
		int      answered;	/* it came up for our page: release it */
		uint64_t sent_at;
		int      with_time;	/* this round sends the date and time */
		uint32_t want;		/* handsets to visit for a notice (bit i) */
	} tu;
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
		unsigned next_call;	/* call ids, 1..127 */
	} ctl;
	/*
	 * The external calls the bridge carries (docs/bridge-interface.md
	 * §2), one per audio channel: on the main link (party -1), on a side
	 * link (its index), or paging its handset for a side ring (BC_PAGING).
	 */
	struct bcall bc[BC_MAX];
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
	int      reg_open;
	uint64_t reg_open_until;
	/* the profile the user picked for the next handset to enrol (LuCI) */
	char     reg_profile[DECT_PROFILE_ID_MAX];
	unsigned int n_xmail;
	unsigned int xmail_idx;	/* next extra mail to send (one per loop tick) */
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
		uint32_t epoch;		/* chip sessions since start */
		uint8_t  handles;	/* links up, bit per handle 0..7 */
		uint32_t up, down, rejected, lost_up;
		uint32_t txq_drops_seen;
	} cell;
	const char *pin;	/* registration PIN / AC */
	/*
	 * Wrong PINs in the System Settings List across all connections, and
	 * the lockout they earn (security audit 2026-10-29, A4): the library
	 * allows a few per connection, this bounds the connections.
	 */
	unsigned    pin_wrong;
	uint64_t    pin_wrong_since, pin_locked_until;
	int rand_fd;		/* /dev/urandom, -1 if unavailable */
	uint8_t rfpi[DECT_MM_RFPI_LEN];
	int have_rfpi;
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
	int         cc_id_foreign_warned;
	int         cc_id_unarmed_warned;
	/* rejected_identity of this connection's CC, as last reported */
	uint32_t    cc_rejected_seen;
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
	/*
	 * Ringing the handset. A page is a broadcast the handset may simply
	 * miss, and the stock host guards each one with a 3 s timer, so page
	 * again until the handset brings up a link or we give up.
	 */
	enum { RING_IDLE, RING_RELEASING, RING_PAGING, RING_WAIT_PAGE_RESP,
	       RING_DONE } ring_state;
	uint64_t ring_release_until;	/* give the link time to go down */
	uint64_t ring_wait_since;	/* a link in use held the ring back */
	uint64_t ring_resp_deadline;	/* fall back if no page response comes */
	unsigned ring_setup_tries;	/* refused CC-SETUPs, see ring_queue_setup */
	/*
	 * After the SETUP is queued: watch it until the handset alerts. A
	 * call lost to a crossing {CC-SETUP} of the handset's (its list sync)
	 * is rung again once the link is free, at most RING_REQUEUE_MAX times.
	 */
	int      ring_watch;
	int      ring_now;	/* a call-in: ring at once, no ring= timing */
	unsigned ring_hs;	/* the handset we ring (table index), 0 by
				 * default; a call-in names its own */
	uint32_t ring_coll0, ring_alert0;	/* cc stats when queued */
	unsigned ring_requeues;
	unsigned    ring_pages;		/* pages sent for this attempt */
	uint64_t    ring_last;		/* when the last one went out */
	uint64_t    first_contact_ms;	/* the handset's first C-plane segment, for ringattach= */
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
	/* 06.88: read-back and slot audit held back until this time */
	uint64_t    sub_check_at;
	int         sub_check_uak, sub_check_audit, sub_check_firing;
	uint8_t     sub_check_slot, sub_check_mine;
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
	struct dect_clip clip;		/* caller identity for an FP-originated SETUP */
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
	/*
	 * Outstanding UAK read-back, 0 = none. See uak_verify_send().
	 */
	uint32_t uak_check_addr;
	uint8_t  uak_check_slot;
	/* Which STUB is loaded, from DECT_STUB_RECORD; see stub_build_read(). */
	int      stub_build;
	char     stub_name[64];
	/*
	 * Slot audit: which slots we are still waiting to hear about, and
	 * which came back holding something. Bit (slot-1).
	 */
	uint8_t  sub_audit_pending;
	uint8_t  sub_audit_occupied;
	uint8_t  sub_audit_mine;	/* the slot we believe is ours */
	/*
	 * Traffic from handsets that are not the one this run serves.
	 * Counted rather than silently dropped: a busy neighbour is the
	 * difference between a result and a confounded one, and the run
	 * should say so instead of leaving it to be worked out from a log.
	 */
	uint32_t foreign_seg;		/* 0x0203 segments for another handle */
	uint32_t foreign_conn;		/* connection-ups we refused to re-arm on */
	uint8_t  foreign_seg_handle;	/* the last such handle seen */

	/* Per-run tally of commands sent, by verification class. */
	uint32_t verify_n[17];
	uint32_t verify_other;
	/*
	 * The main link's call and audio state; its connection and
	 * registration state is base.main_link (dect_base.h).
	 */
	struct reg_ctx {
		int call_bound;		/* audio channel bound for the active call */
		int nonspeech_noted;	/* logged "not a speech call" once */
		uint32_t time_sent_for;	/* mm.stats.located we last answered */
		int audio_connected;	/* 0x022D sent (after 0x0242, or fallback) */
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
		int cipher_started;	/* call-time authentication kicked off */
		int cipher_reject_seen;	/* CIPHER-REJECT already reported */
		int cipher_timeout_seen;	/* <MM_cipher.1> already reported */
		int call_wideband;	/* this call's bind is the G.722 one */
		int info_sent;		/* infoie= went out on this call */
		int cs_connect_sent;	/* "CS call connect" after CONNECT (Part 3) */
		int cc_was_busy;	/* nlr: the last tick had a call */
		int nlr_armed;		/* nlr: a call ended, release pending */
		int nlr_lia;		/* ... and it was list access: DISC first */
		uint64_t nlr_disc_at;	/* ... DISC queued at, 0 = not yet */
		int      connect_staged;	/* alertms/bindfirst: CONNECT due */
		uint64_t connect_at;		/* ... not before this */
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
#define SUB_UAK_STRIDE	0x11u
#define SUB_UAK_END	(SUB_UAK_BASE + DECT_MM_MAX_SLOT * SUB_UAK_STRIDE)

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

static int stub_build_read(char *name, size_t cap)
{
	FILE *f = fopen_root_only(DECT_STUB_RECORD);
	size_t n;

	name[0] = '\0';
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
 * So we check the UAK (uak_verify_send) and deliberately do not build general
 * command verification. This table stays because it keeps the assumption
 * visible -- a quiet log should not read as a verified one.
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
	if (s->foreign_seg || s->foreign_conn)
		printf("reg: *** OTHER HANDSETS WERE ON AIR *** %lu C-plane "
		       "segment(s) for another handle (last: %u) and %lu "
		       "connection-up(s) refused. This link serves one "
		       "handset; a run with competing traffic is NOT a clean "
		       "measurement. Power the others off.\n",
		       (unsigned long)s->foreign_seg,
		       (unsigned)s->foreign_seg_handle,
		       (unsigned long)s->foreign_conn);
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

/*
 * Read n bytes of chip memory at addr: 0xFFF1 sub 0x07 to the debug task
 * [CR COMMAND_SET 7.1], answered by 0xFFF1 sub 0x08 with the address and
 * the bytes. Read only.
 */
static void chip_mem_read(struct stubd *s, uint32_t addr, uint16_t n)
{
	const uint8_t p[7] = {
		0x07u,				/* MEM_READ */
		(uint8_t)addr, (uint8_t)(addr >> 8),
		(uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
		(uint8_t)n, (uint8_t)(n >> 8),
	};

	chip_mail_to(s, DECT_TASK_PCM_REMOTE, 0xFFF1u, p, sizeof(p));
}

/*
 * 0x0230 ASSIGN_AUDIO_CHANNEL in its full form: {link, mode byte, TDM slot
 * mask (LE), master, extra} [CR A-CodecModes-1]; the -a keys call the mode
 * byte flags, the master byte mode and the extra byte codec.
 */
static void chip_assign(struct stubd *s, uint8_t handle, uint8_t flags,
			uint16_t mask, uint8_t mode, uint8_t codec)
{
	const uint8_t p[6] = {
		handle, flags, (uint8_t)mask, (uint8_t)(mask >> 8), mode, codec,
	};

	chip_mail(s, 0x0230u, p, sizeof(p));
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
	int c = s->ic.caller, t = s->ic.tside;

	if (s->ic.st != IC_UP)
		return -1;
	if (t >= 0 && s->side[t].lane >= 0 && &s->xl[s->side[t].lane] == x)
		return ic_word(s, c);
	if (c >= 0 && s->side[c].lane >= 0 && &s->xl[s->side[c].lane] == x)
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

static enum lane0_src lane0_source(const struct stubd *s, const uint8_t *rx,
				   unsigned int *peer)
{
	unsigned int w = s->audio.lane0_word;
	int mode2 = s->reg.call_wideband || !s->audio.remode_ms ||
		    s->reg.remode_done;

	if (!s->reg.call_bound || w >= s->slots ||
	    w == (unsigned int)s->tx_slot)
		return L0_IDLE;
	if (s->ab.l[0].cfd >= 0 && s->ab.l[0].call)
		return mode2 ? L0_BRIDGE : L0_IDLE;
	if (s->ic.st == IC_UP && rx) {
		int p = ic_word(s, s->ic.caller == -1 ? s->ic.tside :
				   s->ic.tside == -1 ? s->ic.caller :
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
	tmpl = s->tx_tmpl + (s->reg.call_bound ? fb : 0u);
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

/* Does the byte stream of one slot in frame data hold a valid 0xAA mail? */
static int slot_has_mail(const uint8_t *data, size_t frames, unsigned int slots,
			 unsigned int slot, struct dect_mail *out)
{
	struct dect_rx rx;
	uint8_t pair[2];
	size_t f;

	dect_rx_init(&rx);
	for (f = 0; f < frames; f++) {
		memcpy(pair, data + (f * slots + slot) * 2u, 2);
		dect_rx_push(&rx, pair, 2);
	}
	return dect_rx_pull(&rx, out);
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
			if (!slot_has_mail(data, frames, s->slots, rx, &ans))
				continue;
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
			if (ans.prim != DECT_PRIM_CONTACT_ACK1 &&
			    ans.prim != DECT_PRIM_CONTACT_ACK2)
				continue;
			s->tx_slot = (int)tx;
			s->rx_slot = (int)rx;
			printf("control slot found: tx %u, rx %u (answer prim %04x)\n",
			       tx, rx, ans.prim);
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
	s->cc_id_foreign_warned = 0;
	s->cc_id_unarmed_warned = 0;
}

/* Handset i's record as the working copy, at start (dect_base_load()). */
static void hs_load(struct stubd *s, unsigned i)
{
	dect_base_load(&s->base, i);
	hs_took(s, i);
}

static void iwu_lines(struct stubd *s, struct dect_iwu *iwu, unsigned slot);

/* The library made another handset current: its file, and say so. */
static void hs_switched(struct stubd *s, const char *why)
{
	hs_took(s, s->base.cur);
	printf("sub: now serving %s (slot %u)%s\n", s->sub_path,
	       stubd_sub_slot(s), why);
}

/* Make handset i the current one; a no-op if it already is. */
static void hs_switch(struct stubd *s, unsigned i, const char *why)
{
	if (!dect_base_switch(&s->base, i))
		return;
	hs_switched(s, why);
}

/*
 * The identities of the handset we ring, wherever they are: in the fields
 * while it is current, in the table while the link serves another one.
 */
static size_t ring_target_fixed_len(const struct stubd *s)
{
	return s->base.cur == s->ring_hs || !s->base.n_hs ? s->base.rec.cc_fixed_id_len
		: s->base.hs[s->ring_hs].cc_fixed_id_len;
}

static const uint8_t *ring_target_port_id(const struct stubd *s, size_t *len)
{
	if (s->base.cur == s->ring_hs || !s->base.n_hs) {
		*len = s->base.rec.cc_port_id_len;
		return s->base.rec.cc_port_id;
	}
	*len = s->base.hs[s->ring_hs].cc_port_id_len;
	return s->base.hs[s->ring_hs].cc_port_id;
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
			s->hs[s->base.cur].loaded = 1;	/* it exists now */
			s->hs[s->base.cur].absent = 0;	/* and is no longer free */
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

static const struct dect_base_ops base_ops = {
	.save = base_save, .send = base_send, .random = base_random,
	.log = base_log, .install = base_install,
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

static void uak_verify_send(struct stubd *s, uint8_t slot);

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
	if (i < s->base.n_hs && s->hs[i].remote && !s->hs[i].fetched)
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
	struct handset *h;

	if (s->fetch_idx < 0)
		return;
	h = &s->hs[s->fetch_idx];
	h->fetched = 1;		/* the next link holds it from hs[] */
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
			if (s->hs[i].remote && s->base.hs[i].port_id_len == len &&
			    !memcmp(s->base.hs[i].port_id, id, len))
				s->fetch_idx = (int)i;
		/*
		 * A record fetched earlier this run is here already: a
		 * {CC-SETUP} held behind the {LOCATE-REQUEST} that fetched
		 * it is served at once. Waiting again let the A540's
		 * <CC.03> (20 s) run out (M4-30000-x-054928).
		 */
		delay = s->fetch_idx < 0 ? 0u :
			s->hs[s->fetch_idx].fetched ? 0u :
			s->audio.fetch_delay_ms;
		s->fetch_at = now + delay;
		printf("fetch: %s from a handset this cell does not hold -- %s "
		       "(simulated, %u ms)\n",
		       setup ? "CC-SETUP" : "LOCATE-REQUEST",
		       s->fetch_idx < 0 ? "unknown to the controller too" :
		       s->hs[s->fetch_idx].fetched ? "record already fetched" :
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

/* The handset table entry with this IPUI, or -1. */
static int hs_by_ipui(const struct stubd *s, const uint8_t *id, size_t len)
{
	unsigned i;

	for (i = 0; i < s->base.n_hs; i++)
		if (s->base.hs[i].port_id_len == len && len &&
		    !memcmp(s->base.hs[i].port_id, id, len))
			return (int)i;
	return -1;
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
	fprintf(f, "part3 %d\nng %d\n", s->hs[i].cap_part3, s->hs[i].cap_ng);
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
			s->hs[i].cap_part3 = v != 0, got |= 1;
		else if (!strcmp(key, "ng"))
			s->hs[i].cap_ng = v != 0, got |= 2;
	}
	fclose(f);
	if (got != 3)
		return;
	s->hs[i].cap_known = 1;
	printf("cap: %s reported %s (remembered)\n", s->hs[i].path,
	       s->hs[i].cap_part3 ? "NG-DECT Part 3" :
	       s->hs[i].cap_ng ? "NG-DECT Part 1" : "GAP only");
}

/*
 * Learn each handset's <<TERMINAL-CAPABILITY>> from whichever link carried
 * its {LOCATE-REQUEST}, matched by the IPUI that link adopted, log it once
 * and keep it beside its store (cap_save()).
 */
static void cap_tick(struct stubd *s, struct dect_link *l)
{
	const struct dect_term_cap *tc = &l->mm.peer_cap;
	struct handset *h;
	int i;

	/* what a handset says of itself counts once it is proved (A3) */
	if (!l->mm.port_id_len || !l->mm.authenticated)
		return;
	i = hs_by_ipui(s, l->mm.port_id, l->mm.port_id_len);
	if (i < 0)
		return;
	h = &s->hs[i];
	/* the profile says the handset's own report is wrong (§5.3) */
	if (!h->prof.trust_terminal_capability && h->prof.cap_ngdect) {
		if (!h->cap_known)
			printf("cap: %s: the profile sets %s (the handset's "
			       "report is not trusted)\n", h->path,
			       h->prof.cap_ngdect == 3 ? "NG-DECT Part 3" :
			       h->prof.cap_ngdect == 2 ? "NG-DECT Part 1" :
			       "GAP only");
		h->cap_known = 1;
		h->cap_part3 = h->prof.cap_ngdect == 3;
		h->cap_ng = h->prof.cap_ngdect >= 2;
		return;
	}
	if (!tc->present)
		return;
	if (h->cap_known && h->cap_part3 == tc->ng_part3 &&
	    h->cap_ng == (tc->ng_part1 || tc->ng_part3))
		return;
	h->cap_known = 1;
	h->cap_part3 = tc->ng_part3;
	h->cap_ng = tc->ng_part1 || tc->ng_part3;
	cap_save(s, (unsigned)i);
	printf("cap: %s reports %s%s\n", h->path,
	       tc->ng_part3 ? "NG-DECT Part 3" :
	       tc->ng_part1 ? "NG-DECT Part 1" : "GAP only",
	       tc->ng_part3 || s->audio.all_call_status ?
	       "" : " -- no call status to it");
	fflush(stdout);
}

/*
 * A call id no other live call uses (TS 102 527-3 §7.4.6.1: unique in the
 * system, never shared by two calls at a time): the lowest of 1..127 not
 * held by a busy CC entity on another link. 0 when -a callid= pins one, or
 * when all are taken (keep the current id). Before 2026-10-28 every link
 * used id 1, so bench test 18's two calls shared it.
 */
static uint8_t alloc_call_id(const struct stubd *s, const struct dect_link *me)
{
	unsigned id, k;

	if (s->audio.callinfo.has_call_id)
		return 0;
	for (id = 1; id <= 127u; id++) {
		int used = 0;

		if (s->base.main_link.active && &s->base.main_link.link != me &&
		    dect_cc_busy(&s->base.main_link.link.cc) &&
		    s->base.main_link.link.cc.status_call_id == id)
			used = 1;
		for (k = 0; !used && k < SIDE_MAX; k++)
			if (s->side[k].bl.active && &s->side[k].bl.link != me &&
			    dect_cc_busy(&s->side[k].bl.link.cc) &&
			    s->side[k].bl.link.cc.status_call_id == id)
				used = 1;
		for (k = 0; !used && k < BC_MAX; k++)
			if (s->bc[k].id && s->bc[k].dect_id == id)
				used = 1;	/* a parallel call's */
		if (!used)
			return (uint8_t)id;
	}
	return 0;
}

/* dect_link's question for a handset's new call: its call id. */
static uint8_t call_id_for(void *ctx, const struct dect_link *l)
{
	uint8_t id = alloc_call_id(ctx, l);

	if (id)
		printf("call: call id %u for the handset's call on handle %u\n",
		       (unsigned int)id, (unsigned int)l->handle);
	return id;
}

/*
 * -a locauth: the UAK stored for an identity, for MM's authentication of a
 * handset inside its location registration (dect_mm_set_uak_lookup()).
 */
static int uak_lookup(void *ctx, const uint8_t *id, size_t len,
		      uint8_t uak[DECT_MM_UAK_LEN])
{
	const struct stubd *s = ctx;
	int i = hs_by_ipui(s, id, len);

	if (i < 0)
		return -1;
	if ((unsigned)i == s->base.cur && s->base.rec.uak_valid) {
		memcpy(uak, s->base.rec.uak, DECT_MM_UAK_LEN);
		return 0;
	}
	if ((unsigned)i != s->base.cur && s->base.hs[i].uak_valid) {
		memcpy(uak, s->base.hs[i].uak, DECT_MM_UAK_LEN);
		return 0;
	}
	return -1;
}

/*
 * dect_link's question when a handset's {CC-SETUP} arrives: call status
 * for it? Only an NG-DECT Part 3 handset gets it (TS 102 527-3 §7.4.6.4.1;
 * stock sends it to the A540 alone, A-0125). Not yet seen: no opinion, so
 * the default stays and the A540's call timer (test 15) cannot be lost to
 * a missing location registration.
 */
static int call_status_for(void *ctx, const uint8_t *ipui, size_t len)
{
	const struct stubd *s = ctx;
	int i = hs_by_ipui(s, ipui, len);

	if (i >= 0 && s->hs[i].prof.cc_call_status != DECT_PROF_AUTO)
		return s->hs[i].prof.cc_call_status == DECT_PROF_ON;
	if (i < 0 || !s->hs[i].cap_known || s->audio.all_call_status)
		return -1;
	return !s->audio.no_call_status && s->hs[i].cap_part3;
}

/*
 * Say so, once, when LAPC gave up after N250 retransmissions and released
 * the link (EN 300 175-4 §9.2.3.6; new 2026-10-28). Loud on purpose: the
 * corpus does not count retransmissions, so whether a working link ever
 * needs more than three is unmeasured, and a false release must show.
 */
static void lapc_failed_note(struct dect_link *l, const char *which)
{
	if (l->lapc.failed != 1)
		return;
	l->lapc.failed = 2;		/* noted */
	printf("lapc: *** %s link: no acknowledgement after 3 retransmissions "
	       "-- link released (DISC, abnormal) ***\n", which);
	fflush(stdout);
}

/*
 * -a nlr: once the main link's call or list session has ended and LAPC has
 * nothing outstanding (the last {CC-RELEASE-COM} is acknowledged), release
 * the link: a DISC first if it was list access, then `0x0202 <link>`
 * [CR A-0127]. The chip's 0x0201 then runs the usual release path. A new
 * call before that disarms it.
 */
static void nlr_tick(struct stubd *s, uint64_t now)
{
	struct dect_link *l = &s->base.main_link.link;
	int busy = s->base.main_link.active && dect_cc_busy(&l->cc);

	if (busy || !s->base.main_link.active) {
		s->reg.nlr_armed = 0;
		s->reg.nlr_disc_at = 0;
	} else if (s->reg.cc_was_busy) {
		s->reg.nlr_armed = 1;
		s->reg.nlr_lia = (l->cc.peer_basic_service >> 4) == 0x2u;
		s->reg.nlr_disc_at = 0;
	}
	s->reg.cc_was_busy = busy;
	if (!s->audio.nlr || !s->reg.nlr_armed ||
	    l->lapc.outstanding || l->lapc.txq_count)
		return;
	{
		/* lce.release_after_call = no in the handset's profile */
		int i = l->mm.adopted >= 0 ? l->mm.adopted : (int)s->base.cur;

		if (i >= 0 && (unsigned)i < s->base.n_hs &&
		    !s->hs[i].prof.lce_release_after_call) {
			s->reg.nlr_armed = 0;
			printf("link: kept after the call (profile: "
			       "lce.release_after_call = no)\n");
			return;
		}
	}
	if (s->reg.nlr_lia && !s->reg.nlr_disc_at) {
		dect_lapc_disconnect(&l->lapc);	/* a CO link: DISC on air */
		pump_lapc(s);
		s->reg.nlr_disc_at = now ? now : 1;
		return;
	}
	if (s->reg.nlr_lia && now - s->reg.nlr_disc_at < 100u)
		return;			/* let the DISC reach the air */
	s->reg.nlr_armed = 0;
	chip_mail(s, 0x0202u, &s->base.main_link.handle, 1);	/* release [CR A-0127] */
	printf("link: released after the %s (0x0202 on handle %u, nlr)\n",
	       s->reg.nlr_lia ? "list session, DISC first" : "call",
	       (unsigned int)s->base.main_link.handle);
	fflush(stdout);
}

/* -a infoie=: one {CC-INFO} once our call to the handset rings. */
/*
 * The caller's name, else number, as the handset's profile's display text
 * (name method 1 or 2) in {CC-INFO}, for handset hs ringing on cc. 1 sent,
 * -1 could not queue it, 0 nothing to send.
 */
static int name_display_info(struct stubd *s, struct dect_cc *cc, unsigned hs,
			     const struct dect_clip *clip)
{
	const struct dect_hs_profile *hp = &s->hs[hs].prof;
	uint8_t ie[96];
	size_t n;

	if (s->audio.info_ie_len ||
	    (hp->cc_name_method != 1 && hp->cc_name_method != 2) ||
	    (!clip->name[0] && !clip->number[0]))
		return 0;
	n = dect_profile_display(hp, clip->name[0] ? clip->name : clip->number,
				 ie, sizeof(ie));
	if (!n || dect_cc_info(cc, ie, n)) {
		printf("call: profile display text: could not queue it\n");
		return -1;
	}
	printf("call: profile display text sent in CC-INFO (%zu octets) on "
	       "ALERTING\n", n);
	return 1;
}

static void info_tick(struct stubd *s, uint64_t now)
{
	struct dect_cc *cc = &s->base.main_link.link.cc;
	int r;

	if (!s->base.main_link.active || !dect_cc_busy(cc)) {
		s->reg.info_sent = 0;
		return;
	}
	if (s->reg.info_sent || !cc->incoming ||
	    cc->state != DECT_CC_ST_RINGING)
		return;
	r = s->base.n_hs ? name_display_info(s, cc, s->ring_hs, &s->clip) : 0;
	if (r) {
		s->reg.info_sent = 1;
		if (r > 0) {
			dect_link_drain(&s->base.main_link.link, now);
			pump_lapc(s);
		}
		return;
	}
	if (!s->audio.info_ie_len)
		return;
	s->reg.info_sent = 1;
	if (dect_cc_info(cc, s->audio.info_ie, s->audio.info_ie_len)) {
		printf("call: infoie: could not queue the CC-INFO\n");
		return;
	}
	printf("call: infoie: CC-INFO with %zu bytes of IEs sent on ALERTING\n",
	       s->audio.info_ie_len);
	dect_link_drain(&s->base.main_link.link, now);
	pump_lapc(s);
}

static int bridge_call_out(struct stubd *s, int p, uint64_t now);

/*
 * Route the main link's handset call once its number is complete: an
 * internal call if it names a handset, else the routed steps (alertms,
 * bindfirst) or CONNECT at once.
 */
static void dial_route(struct stubd *s, uint64_t now)
{
	s->base.main_link.ic_tried = 1;
	if (ic_start(s, -1, now)) {
		dect_link_drain(&s->base.main_link.link, now);
		pump_lapc(s);
		return;
	}
	if (bridge_call_out(s, -1, now))
		return;
	if (s->audio.alert_ms || s->audio.bind_first) {
		/* the steps a routed call takes; connect_tick() finishes */
		if (dect_cc_proceed(&s->base.main_link.link.cc) == 0) {
			if (s->audio.alert_ms) {
				(void)dect_cc_alert(&s->base.main_link.link.cc);
				printf("call: ALERTING sent, CONNECT in %u ms\n",
				       s->audio.alert_ms);
			}
			s->reg.connect_staged = 1;
			s->reg.connect_at = now + s->audio.alert_ms;
			dect_link_drain(&s->base.main_link.link, now);
			pump_lapc(s);
		}
		return;
	}
	if (dect_cc_accept(&s->base.main_link.link.cc) == 0) {
		dect_link_drain(&s->base.main_link.link, now);
		pump_lapc(s);
	}
}

/*
 * Authenticate first (security audit 2026-10-29, A1). A handset is taken
 * for the one it names, and an IPUI travels in clear: until the handset on
 * this connection has answered our challenge with the key we hold for that
 * IPUI, its call goes nowhere -- not to SIP, not to another handset -- and
 * its answer to our call is not passed on. The challenge itself starts as
 * soon as the call exists (the authfirst block in the call tick). -C (no
 * cipher, a lab option) keeps the old order, as does -a noauthfirst.
 */
static int call_auth_ok(const struct stubd *s, const struct dect_link *l)
{
	return !s->audio.auth_first || s->no_cipher || l->mm.authenticated;
}

/*
 * A handset's call whose number came complete and waits in PROCEEDING to be
 * routed: an internal call, a number picked from a list (1C15H, TS 102
 * 527-3 7.4.10.6.2), or one sent with <<SENDING-COMPLETE>> (redial, the
 * phonebook: dect_cc_set_route_complete()).
 */
static int number_complete(const struct dect_cc *cc)
{
	unsigned cls = cc->peer_basic_service >> 4;

	return cc->state == DECT_CC_ST_PROCEEDING && !cc->incoming &&
	       !cc->busy_notified &&	/* §7.4.8.3: the busy tone */
	       (cls == 0x8u || cls == 0x9u || cc->list_number);
}

/*
 * Overlap receiving: accept a handset-placed call once its digits have
 * been quiet for dialwait ms. Stands in for a dial plan or the
 * controller, which would decide on the number instead.
 */
static void dial_tick(struct stubd *s, uint64_t now)
{
	const uint8_t *k;
	size_t len;

	if (s->base.main_link.active && !s->base.main_link.ic_tried &&
	    number_complete(&s->base.main_link.link.cc) && call_auth_ok(s, &s->base.main_link.link)) {
		k = dect_cc_keypad(&s->base.main_link.link.cc, &len);
		printf("call: dialled \"%.*s\", complete -- connecting\n",
		       (int)len, (const char *)k);
		fflush(stdout);
		dial_route(s, now);
	}
	if (!s->base.main_link.active || !dect_cc_busy(&s->base.main_link.link.cc))
		s->base.main_link.ic_tried = 0;
	if (!s->base.main_link.active || s->base.main_link.link.cc.state != DECT_CC_ST_OVERLAP) {
		s->base.main_link.dial_len = 0;
		s->base.main_link.dial_change_ms = 0;
		return;
	}
	k = dect_cc_keypad(&s->base.main_link.link.cc, &len);
	if (!s->base.main_link.dial_change_ms || len != s->base.main_link.dial_len) {
		s->base.main_link.dial_len = len;
		s->base.main_link.dial_change_ms = now;
		return;
	}
	if (!len || now - s->base.main_link.dial_change_ms < s->audio.dial_wait_ms)
		return;		/* no digit yet: F<CC.01> covers it */
	if (!call_auth_ok(s, &s->base.main_link.link))
		return;		/* the number waits for the proof */
	printf("call: dialled \"%.*s\" -- complete after %u ms without a "
	       "digit, connecting\n", (int)len, (const char *)k,
	       s->audio.dial_wait_ms);
	fflush(stdout);
	dial_route(s, now);
}

/* alertms / bindfirst: send the CONNECT once its conditions hold. */
static void connect_tick(struct stubd *s, uint64_t now)
{
	if (!s->reg.connect_staged)
		return;
	if (!s->base.main_link.active ||
	    s->base.main_link.link.cc.state != DECT_CC_ST_PROCEEDING) {
		s->reg.connect_staged = 0;	/* released meanwhile */
		return;
	}
	if (now < s->reg.connect_at ||
	    (s->audio.bind_first && !s->reg.audio_connected))
		return;
	s->reg.connect_staged = 0;
	printf("call: CONNECT now%s%s\n",
	       s->audio.alert_ms ? ", after ALERTING" : "",
	       s->audio.bind_first ? ", the U-plane is up (0x022D sent)" : "");
	fflush(stdout);
	if (dect_cc_connect(&s->base.main_link.link.cc) == 0) {
		dect_link_drain(&s->base.main_link.link, now);
		pump_lapc(s);
	}
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

		if (!s->hs[i].loaded || !valid)
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
		if (i != s->base.cur && s->hs[i].loaded && s->base.hs[i].uak_valid)
			return 1;
	return 0;
}

static void sub_audit_reply(struct stubd *s, uint32_t addr,
			    const uint8_t *rec, size_t len);
static void sub_audit_send(struct stubd *s, uint8_t mine);

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
static void clear_foreign_slots(struct stubd *s, uint8_t keep,
				unsigned keep_mask)
{
	dect_base_slot_clear(&s->base, keep_mask);
	/*
	 * ...and now check it actually happened. The clear is 0x0247, which
	 * has no reply and no error path, so until today "cleared" meant
	 * "asked". vF426 is what asking without checking looks like.
	 */
	sub_audit_send(s, keep);
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

		if (i != s->base.cur && s->hs[i].loaded && s->base.hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && !held_back(s, i))
			keep |= 1u << hsl;
	}
	clear_foreign_slots(s, slot, keep);
	for (i = 0; i < s->base.n_hs; i++) {
		unsigned hsl = hs_slot(s, i);

		if (i != s->base.cur && s->hs[i].loaded && s->base.hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && hsl != slot &&
		    !held_back(s, i)) {
			install_subscription(s, (uint8_t)hsl, s->base.hs[i].uak);
			printf("sub: %s installed in chip slot %u\n",
			       s->hs[i].path, hsl);
		}
	}
	if (s->base.rec.uak_valid && !held_back(s, s->base.cur)) {
		install_subscription(s, slot, s->base.rec.uak);
		uak_verify_send(s, slot);	/* [CR A-0085] the only check */
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
					: s->hs[i].loaded && s->base.hs[i].uak_valid;

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
 * On the 7412's 06.88 STUB a memory read (task 0x23) sent right behind
 * 0x020A/0x0247 (task 0x02) is answered before they take effect: the read 40
 * ms after a 0x020A found the record missing, a sweep 1 s later found it in
 * place (observed 2026-10-29). There, the read-back and the slot audit wait.
 */
#define SUB_CHECK_DELAY_0688_MS	1000u

static int sub_check_defer(struct stubd *s)
{
	if (!s->uak_0688 || s->sub_check_firing)
		return 0;
	s->sub_check_at = mono_ms() + SUB_CHECK_DELAY_0688_MS;
	return 1;
}

/*
 * Read back the subscription record we just wrote, and say whether it landed.
 *
 * `[CR A-0085]`: 0x020A copies the 16-byte UAK to
 *
 *	0x9518 + (slot - 1) * 0x11
 *
 * with a flag at +0x10, six slots, ending at 0x957E. The handler is a direct
 * memcpy that returns immediately, and the chip processes mails in order, so
 * a read issued after the write sees the finished result.
 *
 * `[CR A-0086]` is why this one command gets a check when we decided the
 * others do not need one:
 *
 *   - The chip has **no rejection path and no error indication at all.** Not
 *     for this command, not for any of them. Accepted and dropped look
 *     identical, so a read-back is the only verification that can exist.
 *   - Stock does not verify either -- but stock periodically **re-asserts**
 *     most subscription state, so a lost write self-heals. The UAK is the
 *     exception: written once at enrolment and never re-asserted. Stock has
 *     our exact failure mode here and no way to notice it.
 *
 * So: one write-once, unprotected, silently-droppable command, carrying the
 * credential the handset needs to attach. That is worth one read.
 *
 * The UAK is key material. We compare it and report the verdict; the bytes
 * are never printed, not even under -p.
 */
static void uak_verify_send(struct stubd *s, uint8_t slot)
{
	uint32_t addr;

	if (slot < 1u || slot > DECT_MM_MAX_SLOT)
		return;
	if (sub_check_defer(s)) {
		s->sub_check_uak = 1;
		s->sub_check_slot = slot;
		return;
	}
	if (s->stub_build == STUB_OTHER && !s->uak_0688) {
		printf("sub: UAK read-back skipped -- STUB %s is loaded, and the "
		       "table address we hold is the 08.25 build's [CR A-0085, "
		       "Q-0059]\n", s->stub_name);
		return;
	}
	addr = SUB_UAK_BASE + (uint32_t)(slot - 1u) * SUB_UAK_STRIDE;

	s->uak_check_addr = addr;
	s->uak_check_slot = slot;
	chip_mem_read(s, addr, SUB_UAK_STRIDE);	/* 16 key bytes + the flag */
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

/* Match a 0xFFF1 sub 0x08 reply against an outstanding read-back. */
static void uak_verify_reply(struct stubd *s, const struct dect_mail *m)
{
	uint32_t addr;

	if (m->param_len < 7u || m->param[0] != 0x08u)
		return;
	addr = (uint32_t)m->param[1] | ((uint32_t)m->param[2] << 8)
	     | ((uint32_t)m->param[3] << 16) | ((uint32_t)m->param[4] << 24);
	if (s->park_check_at && m->param_len > 7u)
		park_check_reply(s, addr, m->param[7]);
	if (s->sub_audit_pending)
		sub_audit_reply(s, addr, &m->param[7],
				(size_t)(m->param_len - 7u));
	if (!s->uak_check_addr || addr != s->uak_check_addr)
		return;			/* some other read */
	s->uak_check_addr = 0;

	if (m->param_len < 7u + DECT_MM_UAK_LEN) {
		printf("sub: UAK read-back for slot %u came back short "
		       "(%u bytes) -- CANNOT VERIFY\n",
		       s->uak_check_slot, (unsigned)m->param_len);
		return;
	}
	if (!memcmp(&m->param[7], s->base.rec.uak, DECT_MM_UAK_LEN)) {
		printf("sub: UAK read-back slot %u at 0x%05X: MATCHES "
		       "(16 bytes, flag %02x)\n", s->uak_check_slot, addr,
		       m->param_len > 7u + DECT_MM_UAK_LEN
			       ? m->param[7 + DECT_MM_UAK_LEN] : 0u);
	} else {
		/*
		 * The chip took the mail and did not store it, or stored it
		 * somewhere else. There is no error indication to have
		 * missed: this is the only way the failure is visible.
		 */
		printf("sub: *** UAK READ-BACK MISMATCH *** slot %u at "
		       "0x%05X -- the subscription did NOT land. The handset "
		       "will not attach. [CR A-0085/A-0086]%s\n",
		       s->uak_check_slot, addr,
		       s->stub_build == STUB_UNKNOWN
			       ? " Unless the STUB is not 08.25: no record in "
				 DECT_STUB_RECORD " says which is loaded" : "");
	}
	fflush(stdout);
}

/*
 * Audit every subscription slot the chip holds, and say so out loud.
 *
 * This is the one that has actually bitten us. The chip's subscription table
 * **survives our daemon**: we write one slot, and a handset enrolled in some
 * earlier session stays reachable in another slot for ever. `vF426` measured
 * it -- 51 connections, every one from a handset our store did not contain.
 * We now clear foreign slots at bring-up, and until today we never checked
 * that the clear worked.
 *
 * `[CR A-0086]` says stock gets away without verifying because it
 * periodically **re-asserts** subscription state, so a lost write self-heals.
 * We re-assert at start-up and send 0x017C after every change [CR A-0129],
 * but a slot cleared or written once can still be lost, and only a read
 * shows it.
 *
 * Occupancy only -- a slot holds key material or it does not. We never print
 * the key, and we cannot yet say WHICH handset a slot belongs to: that is the
 * per-handset config above 0x957E, whose layout we do not have (Q-0054).
 */
static void sub_audit_send(struct stubd *s, uint8_t mine)
{
	uint8_t slot;

	if (sub_check_defer(s)) {
		s->sub_check_audit = 1;
		s->sub_check_mine = mine;
		return;
	}
	s->sub_audit_pending = 0;
	s->sub_audit_occupied = 0;
	s->sub_audit_mine = mine;
	if (s->stub_build == STUB_OTHER && !s->uak_0688) {
		printf("sub: slot audit skipped -- STUB %s is loaded, and the "
		       "table address we hold is the 08.25 build's\n",
		       s->stub_name);
		return;
	}
	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++) {
		uint32_t addr = SUB_UAK_BASE
			      + (uint32_t)(slot - 1u) * SUB_UAK_STRIDE;

		s->sub_audit_pending |= (uint8_t)(1u << (slot - 1u));
		chip_mem_read(s, addr, SUB_UAK_STRIDE);
	}
}

static void sub_audit_reply(struct stubd *s, uint32_t addr,
			    const uint8_t *rec, size_t len)
{
	uint8_t slot, bit, i;
	int occupied = 0;

	if (addr < SUB_UAK_BASE)
		return;
	if ((addr - SUB_UAK_BASE) % SUB_UAK_STRIDE)
		return;
	slot = (uint8_t)((addr - SUB_UAK_BASE) / SUB_UAK_STRIDE + 1u);
	if (slot < 1u || slot > DECT_MM_MAX_SLOT)
		return;
	bit = (uint8_t)(1u << (slot - 1u));
	if (!(s->sub_audit_pending & bit))
		return;
	s->sub_audit_pending &= (uint8_t)~bit;

	for (i = 0; i < DECT_MM_UAK_LEN && i < len; i++)
		if (rec[i])
			occupied = 1;
	if (occupied)
		s->sub_audit_occupied |= bit;

	if (s->sub_audit_pending)
		return;			/* wait for the rest */

	{
		uint8_t mine_bit = (uint8_t)(s->sub_audit_mine >= 1u &&
				   s->sub_audit_mine <= DECT_MM_MAX_SLOT
			? 1u << (s->sub_audit_mine - 1u) : 0u);
		/* our other handsets' slots are kept, not foreign */
		uint8_t kept = (uint8_t)(live_slot_mask(s) & ~mine_bit);
		uint8_t foreign = (uint8_t)(s->sub_audit_occupied &
					    ~(mine_bit | kept));

		printf("sub: chip slot audit:");
		for (i = 1u; i <= DECT_MM_MAX_SLOT; i++)
			printf(" %u=%s", i,
			       !(s->sub_audit_occupied & (1u << (i - 1u))) ? "-" :
			       i == s->sub_audit_mine ? "OURS" :
			       kept & (1u << (i - 1u)) ? "ours" : "held");
		printf("\n");
		if (mine_bit && !(s->sub_audit_occupied & mine_bit))
			printf("sub: *** OUR OWN SLOT %u IS EMPTY *** the "
			       "subscription did not land; the handset will "
			       "not attach\n", s->sub_audit_mine);
		if (foreign)
			printf("sub: *** %u FOREIGN SLOT(S) STILL HELD *** "
			       "the clear did not take. A handset we hold no "
			       "record for can attach [vF426]\n",
			       (unsigned)__builtin_popcount(foreign));
		if (s->stub_build == STUB_UNKNOWN)
			printf("sub: (the audit reads the 08.25 build's "
			       "address; no record in " DECT_STUB_RECORD
			       " says which STUB is loaded)\n");
		if (!foreign && (!mine_bit || (s->sub_audit_occupied & mine_bit)))
			printf("sub: slot table is as we intend it\n");
		fflush(stdout);
	}
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
	/* selector valid 1..6 [CR A-CodecModes-3]; the STUB stores gain << 8 */
	const uint8_t p[3] = { handle, a->sel, a->gain };
	/* a wideband call: stock's bind, flags 0x41 and codec byte 0 */
	int wb = s->reg.call_wideband;
	uint8_t flags = wb ? (uint8_t)(a->flags | 0x40u) : a->flags;
	uint8_t codec = wb ? 0u : a->codec;

	chip_mail(s, 0x0251u, p, sizeof(p));	/* SET_CHANNEL_PARAM */
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
	const uint8_t p[2] = { handle, 0x01u /* [3] must be non-zero */ };

	chip_mail(s, 0x0233u, p, sizeof(p));
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
	const uint8_t p[2] = { handle, 0x00u };	/* bit 7 clear: connect */

	if (s->reg.audio_connected)
		return;
	s->reg.audio_connected = 1;
	s->reg.remode_handle = handle;
	if (a->remode_ms)
		s->reg.remode_at = mono_ms() + a->remode_ms;
	chip_mail(s, 0x022Du, p, sizeof(p));	/* AUDIO_PATH_ON_OFF */
	printf("call: audio connect: 0x022D on channel %u\n", chan);
}

/*
 * Fallback: if the chip never confirms the assignment, connect anyway after
 * half a second so the deferred order can never do worse than the old one.
 */
static void audio_connect_fallback(struct stubd *s, uint64_t now)
{
	if (!s->reg.call_bound || s->reg.audio_connected || !s->base.main_link.active)
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
			       s->reg.call_bound ? " (call bound)" : " (idle)");
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

		if (!sl->bl.active || sl->lane < 0 ||
		    (sl->status_ms && now - sl->status_ms < s->audio.status_ms))
			continue;
		sl->status_ms = now;
		send_conn_status_one(s, sl->bl.handle);
	}
	if (!s->audio.status_ms || !s->reg.call_bound || !s->base.main_link.active)
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
#define RING_PAGE_CC	0x0Cu		/* CC page */
#define RING_PAGE_WIDEBAND 0x20u	/* ... wideband SETUP to follow */

/* The page for an external call: wideband to follow, unless narrow. */
static uint8_t ring_page_info(const struct stubd *s, int narrow)
{
	return (uint8_t)(RING_PAGE_CC |
			 ((s->audio.basic_svc & 0x0Fu) == 0x08u && !narrow
			  ? RING_PAGE_WIDEBAND : 0u));
}

/* The main link's ring is for a narrowband call (struct bcall). */
static int main_ring_narrow(const struct stubd *s)
{
	unsigned i;

	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == -1)
			return s->bc[i].narrow;
	return 0;
}

#define RING_RELEASE_WAIT_MS	3000u	/* [CR A-0053] DLC arms a ~5 s guard */
#define RING_IN_USE_WAIT_MS	30000u	/* a list session or MM, at most */

/*
 * What a link is doing, for the ring: 0 nothing the NWK layer holds (the
 * ring may release it), 1 a list-access session or an MM procedure (wait
 * for it), 2 a voice call (the handset is busy). EN 300 175-5 §14.2.3 pages
 * only when no suitable link exists, and the release before paging that
 * the chip needs ([CR A-0053]) cut whatever the link carried: a ring during
 * a call ended the call (spec sweep F-2, LCE1).
 */
static int link_in_use(const struct dect_link *l)
{
	if (dect_cc_busy(&l->cc))
		return (l->cc.peer_basic_service >> 4) == 0x2u && !l->cc.incoming
		       ? 1 : 2;
	if (l->mm.state == DECT_MM_WAIT_AUTH || l->mm.ident_pending ||
	    l->mm.cipher_state == DECT_MM_CIPHER_WAIT_REPLY ||
	    dect_mm_locate_pending(&l->mm, NULL, NULL))
		return 1;
	return 0;
}

/*
 * Pages before giving up: enough to cover a handset still re-synchronising
 * after the base was off the air (it shows "Synchronisiere"), which can take
 * over half a minute. An unanswered page costs nothing.
 */
#define RING_PAGE_RETRIES	20u	/* pages before giving up (60 s) */
#define RING_PAGE_INTERVAL	3000u	/* ms between them: <LCE.03> (EN 300 175-5 A.6) */
#define RING_PAGE_RESP_MS	1500u	/* wait this long for the page response */
#define RING_SETUP_RETRY_MS	2000u	/* re-try a refused CC-SETUP this often */
#define RING_SETUP_TRIES	8u	/* ... this many times before giving up */
#define RING_REQUEUE_MAX	2u	/* rings again after a lost collision */

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
 * The TPUI of the handset we ring, by MM's own rule for its slot. Not the
 * link's MM: that is whichever handset the link last served. Measured
 * 2026-10-25 (run 32): the A540 had placed a call on the link, MM had
 * adopted it, and the page for the C430A went out as 0x1101 -- the A540's.
 * The A540 answered, correctly, and got {LCE-PAGE-REJECT}.
 */
static void ring_target_tpui(const struct stubd *s, uint8_t tpui[3])
{
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	memset(&cfg, 0, sizeof(cfg));
	if (s->base.cur == s->ring_hs || !s->base.n_hs)
		cfg.sub_slot = stubd_sub_slot(s);
	else
		cfg.sub_slot = (uint8_t)(s->audio.sub_slot && !s->ring_hs
					 ? s->audio.sub_slot
					 : s->base.hs[s->ring_hs].slot);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, tpui);
}

static void send_page(struct stubd *s)
{
	uint8_t tpui[3], p[3];

	/*
	 * The page carries the low 16 bits of the TPUI we assigned, so take
	 * them from MM rather than rebuilding them from the slot. With a
	 * pool-assigned TPUI the two are no longer the same number, and a page
	 * built from the slot would address the wrong handset.
	 */
	ring_target_tpui(s, tpui);
	p[0] = ring_page_info(s, main_ring_narrow(s));
	p[1] = tpui[1];
	p[2] = tpui[2];
	chip_mail(s, 0x0300u, p, sizeof(p));
	printf("ring: paged the handset, attempt %u of %u "
	       "(0x0300 {%02x %02x %02x}, basic service %02x)%s\n",
	       s->ring_pages, RING_PAGE_RETRIES, p[0],
	       tpui[1], tpui[2], s->audio.basic_svc,
	       /*
		* We page BY the TPUI we assigned. If the handset never
		* acknowledged the assignment (EN 300 175-5 §13.2.2,
		* {TEMPORARY-IDENTITY-ASSIGN-ACK}) it may not be listening for
		* it, and the page is silent in a way that looks exactly like a
		* handset that will not answer -- which we have chased more
		* than once. Say so rather than letting it look normal.
		*/
	       dect_mm_tpui_confirmed(&s->base.main_link.link.mm)
		       ? "" : " -- WARNING: TPUI never acknowledged");
}

static struct dect_clip ring_clip;	/* s->clip as the target's profile wants it */

/*
 * The <<BASIC-SERVICE>> of our SETUP to handset i. A pure GAP handset
 * (neither NG-DECT Part 1 nor 3) gets GAP's incoming call, basic speech
 * 0x80: EN 300 444 table 21 allows service 0 only, and TS 102 527-1 §7.3.6
 * gives an internal call class 9 to NG handsets only. Not yet seen: as
 * configured. Spec sweep, finding 15.
 */
static uint8_t target_svc(const struct stubd *s, int i, uint8_t configured)
{
	if (i >= 0 && (unsigned)i < s->base.n_hs && s->hs[i].prof.cc_basic_service)
		/* the profile: 1 wideband, 2 basic speech; the class stays */
		return (uint8_t)((configured & 0xF0u) |
				 (s->hs[i].prof.cc_basic_service == 1 ? 0x08u
								      : 0x00u));
	if (i >= 0 && (unsigned)i < s->base.n_hs && s->hs[i].cap_known &&
	    !s->hs[i].cap_ng)
		return 0x80u;
	return configured;
}

/*
 * The basic service of our SETUP to handset t for an external call:
 * basic speech (G.726) when the SIP side is narrowband, so the handset
 * codes what the far end hears, else what target_svc() gives it.
 */
static uint8_t ring_svc(const struct stubd *s, unsigned t, int narrow)
{
	uint8_t svc = target_svc(s, (int)t, s->audio.basic_svc);

	return narrow ? (uint8_t)(svc & 0xF0u) : svc;
}

/*
 * The ring target is a Part 3 handset whose call information we choose:
 * no lab option set any <<CALL-INFORMATION>> field.
 */
static int ring_target_part3(const struct stubd *s)
{
	const struct dect_callinfo *o = &s->audio.callinfo;

	return !o->has_line && !o->has_call_id && !o->has_call_status &&
	       s->base.n_hs && s->hs[s->ring_hs].cap_known &&
	       s->hs[s->ring_hs].cap_part3;
}

/*
 * on = 1: the call information of an external incoming call, for the next
 * SETUP only. A Part 3 handset gets stock's block: our line, line type
 * "significant delay" (VoIP), call id, CS call setup [CR A-0125]; TS 102
 * 527-3 §7.4.6.4.3 makes CS call setup mandatory in that SETUP. Bench
 * 2026-10-28: with it the A540 rings aloud and shows "via Line 1
 * <number>"; with a bare line id it rings silently. Part 1 handsets get
 * none (stock sends them none; §7.4.6.4.1). on = 0 restores the link's
 * setting, so later answers keep their bytes.
 */
static void ring_callinfo(struct stubd *s, int on)
{
	struct dect_callinfo ci;
	unsigned i;

	if (!on || !ring_target_part3(s)) {
		dect_cc_set_callinfo(&s->base.main_link.link.cc, &s->audio.callinfo);
		if (on && s->base.n_hs && !s->hs[s->ring_hs].cap_known)
			printf("ring: the target's terminal capability is not "
			       "known yet -- no call information (an A540 "
			       "then rings silently)\n");
		return;
	}
	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = STUBD_LINE_ID;
	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == -1)
			ci.line_id = (uint8_t)s->bc[i].line;	/* the called one */
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	ci.has_call_id = 1;
	ci.call_id = s->base.main_link.link.cc.status_call_id;
	ci.has_call_status = 1;
	ci.call_status = DECT_CS_CALL_SETUP;
	dect_cc_set_callinfo(&s->base.main_link.link.cc, &ci);
	printf("ring: full call information for a Part 3 handset "
	       "(line %u, VoIP, call id %u, CS call setup)\n",
	       (unsigned int)ci.line_id, (unsigned int)ci.call_id);
}

/*
 * Stock's next step for a Part 3 handset [CR A-0125]: once our call is
 * answered, a {CC-INFO} with the call id and "CS call connect".
 */
static void cs_connect_tick(struct stubd *s, uint64_t now)
{
	struct dect_cc *cc = &s->base.main_link.link.cc;
	struct dect_callinfo ci;
	uint8_t ie[16];
	size_t n;

	if (!s->base.main_link.active || !dect_cc_busy(cc)) {
		s->reg.cs_connect_sent = 0;
		return;
	}
	if (s->reg.cs_connect_sent || !cc->incoming ||
	    cc->state != DECT_CC_ST_ACTIVE || !ring_target_part3(s) ||
	    s->audio.no_call_status)
		return;
	s->reg.cs_connect_sent = 1;
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = cc->status_call_id;
	ci.has_call_status = 1;
	ci.call_status = DECT_CS_CALL_CONNECT;
	n = dect_callinfo_put(ie, sizeof(ie), 0, &ci);
	if (!n || dect_cc_info(cc, ie, n)) {
		printf("call: could not queue the CS call connect\n");
		return;
	}
	printf("call: CS call connect sent in CC-INFO (Part 3)\n");
	dect_link_drain(&s->base.main_link.link, now);
	pump_lapc(s);
}

/*
 * Queue the FP-originated {CC-SETUP} on the main link, once the handset has
 * answered our page or the page-response deadline has passed. Not the
 * moment the chip reports the link up: a SETUP sent before the handset's
 * page response was lost as an I-frame it did not expect, and the
 * retransmission a second later drew 0x32 "insufficient resources".
 * Waiting for the page response is also the order the paging procedure
 * prescribes.
 */
static void ring_queue_setup(struct stubd *s, uint64_t now, const char *why)
{
	/*
	 * No link to send it on: the handsets that woke for the page took
	 * their links and dropped them again (12d, 2026-10-25: the SETUP was
	 * queued at 45.0 s with every link gone, and nothing rang). Page
	 * again; the page budget still bounds it.
	 */
	if (!s->base.main_link.active) {
		printf("ring: no link up at the page-response deadline -- "
		       "paging again\n");
		s->ring_state = RING_PAGING;
		return;
	}
	/*
	 * Our call goes to the handset we ring (ring_hs). A link that has taken on
	 * another handset is that handset's; a {CC-SETUP} on it would address
	 * the wrong one. Wait for it to clear, as for a busy CC entity.
	 */
	if (s->base.cur != s->ring_hs) {
		printf("ring: this link serves %s, not the handset we ring -- "
		       "waiting\n", s->sub_path);
		s->ring_resp_deadline = now + RING_SETUP_RETRY_MS;
		return;
	}
	/*
	 * EN 300 444 §6.9.2: the lowest free transaction value, which for our
	 * one-call CC entity is always 0 (see dect_cc_set_next_ti()). The
	 * Gigasets refuse an incoming call with any other.
	 */
	dect_cc_set_next_ti(&s->base.main_link.link.cc, 0);
	{
		uint8_t id = alloc_call_id(s, &s->base.main_link.link);

		if (id)
			dect_cc_set_call_status(&s->base.main_link.link.cc,
						s->base.main_link.link.cc.call_status, id);
	}
	ring_callinfo(s, 1);
	{
		/* the ring target's profile: its alerting pattern, unless
		 * -a signal= chose one, and whether the name goes as CNIP */
		const struct dect_hs_profile *hp = s->base.n_hs ?
			&s->hs[s->ring_hs].prof : NULL;

		ring_clip = s->clip;
		if (hp && !s->audio.signal_set &&
		    hp->cc_signal_external != DECT_SIGNAL_ALERT_PATTERN1)
			dect_cc_set_signal(&s->base.main_link.link.cc, 1,
					   hp->cc_signal_external);
		if (hp && (hp->cc_name_method == 1 || hp->cc_name_method == 3))
			ring_clip.name[0] = '\0';	/* display or none */
	}
	/*
	 * dect_cc_setup_id() refuses unless the CC entity is idle, and that is
	 * not rare: the A540 opens a CAT-iq service call of its own roughly
	 * every 17 s and holds it for about 13 s. Retry while the paging window
	 * is still open rather than lose the ring on the first refusal.
	 */
	if (!dect_cc_setup_id(&s->base.main_link.link.cc, s->base.rec.cc_port_id, s->base.rec.cc_port_id_len,
			      s->base.rec.cc_fixed_id, s->base.rec.cc_fixed_id_len,
			      ring_svc(s, s->ring_hs, main_ring_narrow(s)),
			      &ring_clip)) {
		ring_callinfo(s, 0);
		s->ring_state = RING_DONE;
		s->ring_watch = 1;
		s->ring_coll0 = s->base.main_link.link.cc.stats.collisions;
		s->ring_alert0 = s->base.main_link.link.cc.stats.alerting;
		printf("ring: CC-SETUP queued (%s) -- the handset should ring\n",
		       why);
		return;
	}
	if (++s->ring_setup_tries >= (s->audio.setup_tries ?
				      s->audio.setup_tries : RING_SETUP_TRIES)) {
		s->ring_state = RING_DONE;
		printf("ring: CC-SETUP refused %u times, the handset's own call "
		       "never cleared; giving up\n", s->ring_setup_tries);
		return;
	}
	s->ring_resp_deadline = now + RING_SETUP_RETRY_MS;
	printf("ring: CC-SETUP refused (a call is in progress), retry %u of %u "
	       "in %u ms\n", s->ring_setup_tries,
	       s->audio.setup_tries ? s->audio.setup_tries : RING_SETUP_TRIES,
	       RING_SETUP_RETRY_MS);
}


/*
 * The queued SETUP is gone without an ALERTING: if a {CC-SETUP} of the
 * handset's crossed it (EN 300 175-5 §9.5.2.3), ring again from the start
 * once the link is free. The A540 opens a list call right after a location
 * registration with a list change notice, and does not wait P<CC.06> for
 * our re-presentation, so the ring was lost (bench, 2026-10-28).
 */
static void ring_watch_tick(struct stubd *s)
{
	const struct dect_cc *cc = &s->base.main_link.link.cc;

	if (s->ring_state != RING_DONE || !s->ring_watch)
		return;
	if (s->base.main_link.active && cc->stats.alerting != s->ring_alert0) {
		s->ring_watch = 0;		/* it rings */
		return;
	}
	if (s->base.main_link.active && cc->incoming && dect_cc_busy(cc))
		return;				/* still presented */
	s->ring_watch = 0;
	if (!s->base.main_link.active || cc->stats.collisions == s->ring_coll0)
		return;				/* refused or ended otherwise */
	if (s->ring_requeues >= RING_REQUEUE_MAX) {
		printf("ring: lost to the handset's own call again -- giving "
		       "up after %u new attempts\n", s->ring_requeues);
		return;
	}
	s->ring_requeues++;
	s->ring_state = RING_IDLE;
	s->ring_setup_tries = 0;
	s->ring_pages = 0;
	s->ring_wait_since = 0;
	printf("ring: our call was lost to the handset's own call (collision, "
	       "9.5.2.3) -- ringing again when the link is free (%u of %u)\n",
	       s->ring_requeues, RING_REQUEUE_MAX);
}

static void ring_tick(struct stubd *s, uint64_t now)
{
	if ((!s->audio.ring_ms && !s->audio.ring_attach_ms && !s->ring_now) ||
	    s->sess.state != DECT_SESS_UP)
		return;
	ring_watch_tick(s);
	if (s->ring_state == RING_DONE)
		return;

	/*
	 * Waiting for the page response. Send the SETUP anyway once the
	 * deadline passes, so a handset that never answers the page the way
	 * we expect still gets a call rather than silence.
	 */
	if (s->ring_state == RING_WAIT_PAGE_RESP) {
		if (now >= s->ring_resp_deadline)
			ring_queue_setup(s, now, s->ring_setup_tries
					 ? "retrying after a refusal"
					 : "no page response, deadline");
		return;
	}

	if (s->ring_state == RING_IDLE) {
		if (s->ring_now) {
			/* a call-in from the bridge: now */
		} else if (s->audio.ring_attach_ms) {
			/* Count from the handset's first contact, not ours. */
			if (!s->first_contact_ms ||
			    now - s->first_contact_ms < s->audio.ring_attach_ms)
				return;
		} else if (now - g_start < s->audio.ring_ms) {
			return;
		}
		if (!ring_target_fixed_len(s)) {
			printf("ring: no call identities -- the handset is not "
			       "registered here, or -s is off so nothing was "
			       "stored\n");
			s->ring_state = RING_DONE;
			return;
		}
		/*
		 * A link in use is not released under the handset (§14.2.3,
		 * see link_in_use()): a voice call on it -- the ring target's
		 * or, on the main link, another handset's -- makes this ring
		 * "busy" (call waiting is not built yet, lines-and-calls.md
		 * step 5); a list session or an MM procedure is waited out,
		 * for at most RING_IN_USE_WAIT_MS, then released as before.
		 */
		{
			int use = s->base.main_link.active ? link_in_use(&s->base.main_link.link) : 0;
			unsigned k;

			for (k = 0; k < SIDE_MAX; k++)
				if (s->side[k].bl.active &&
				    s->side[k].adopted == (int)s->ring_hs &&
				    link_in_use(&s->side[k].bl.link) == 2)
					use = 2;	/* the target, elsewhere */
			if (use == 2) {
				printf("ring: the handset (or the main link) is in "
				       "a call -- busy, nothing released, ring "
				       "abandoned\n");
				s->ring_state = RING_DONE;
				return;
			}
			if (use == 1 && !s->ring_wait_since) {
				s->ring_wait_since = now;
				printf("ring: the link carries a list session or "
				       "an MM procedure -- waiting for it to end\n");
			}
			if (use == 1 &&
			    now - s->ring_wait_since < RING_IN_USE_WAIT_MS)
				return;
			if (use == 1)
				printf("ring: still in use after %u ms -- releasing "
				       "it all the same\n", RING_IN_USE_WAIT_MS);
			else if (s->ring_wait_since)
				printf("ring: the link is free after %llu ms\n",
				       (unsigned long long)(now -
							    s->ring_wait_since));
			s->ring_wait_since = 0;
		}
		/*
		 * **Page for every call, even when the handset is attached.**
		 *
		 * This used to take a shortcut: a link is already up, so the
		 * handset does not need paging and the SETUP can go straight
		 * out. That shortcut was the whole of defect 2's intermittency.
		 *
		 * Measured 2026-10-23 over seven runs, a clean 7/7 split:
		 *
		 *   "page response received" -> the mode-2 re-assign takes ->
		 *       audio works
		 *   "a link was already up"  -> the enable word never reaches 1,
		 *       however often mode 2 is asked for (eight times in vF415)
		 *       -> no audio at all
		 *
		 * [CR A-0052] names the mechanism. The link that is already up
		 * is the ATTACHMENT/signalling link, not a traffic bearer. Stock
		 * sets up a call's traffic bearer by PAGING (0x0300, page type
		 * 0x11), and the causal chain is: page -> the handset answers
		 * over the air -> the chip allocates the traffic bearer -> MAC
		 * bearer event 0x20 -> event 0x70 -> the configurator binds the
		 * audio channel. Skip the page and nothing ever asks the chip for
		 * a traffic bearer, so 0x20/0x70 never fire, the configurator
		 * never runs, and the channel keeps its unconfigured default
		 * (enable = 0, dest 6 = the silence source) no matter what we
		 * assign afterwards.
		 *
		 * Paging an idle-locked attached handset is the ordinary DECT
		 * mechanism and does not un-attach it, so this is light, not
		 * heavy: no release, no re-attach. An earlier fix here waited up
		 * to 20 s for the link to "clear" -- vF420/vF420b showed the C4
		 * attaches a second after the base comes up and holds the link
		 * indefinitely, so the wait never fired and could not have.
		 */
		/*
		 * [CR A-0053]: stock never pages a linked handset. It releases
		 * the link first and always pages from idle-locked, and the
		 * release does NOT un-attach the handset -- attachment and
		 * location registration are persistent and separate from the
		 * CC/DLC link, so the handset returns to idle-locked and
		 * pageable without re-registering. Release once per call.
		 *
		 * vF423 is the measurement behind this: with the link up, 16
		 * pages went unanswered, where vF411 -- same build, same mails,
		 * no link up -- got a page response on attempt 1.
		 */
		if (s->base.main_link.active) {
			printf("ring: releasing the link before paging -- a "
			       "linked handset does not answer pages, and the "
			       "page is what makes the chip allocate a traffic "
			       "bearer\n");
			dect_lapc_disconnect(&s->base.main_link.link.lapc);
			pump_lapc(s);
			s->ring_release_until = now + RING_RELEASE_WAIT_MS;
			s->ring_state = RING_RELEASING;
			return;
		}
		s->ring_state = RING_PAGING;
	}

	/*
	 * Wait for the link to actually go down before paging. reg.active is
	 * cleared on the chip's CONN_DOWN, so this is the chip telling us the
	 * link is gone, not a guess. Page anyway when the wait expires: a page
	 * that goes unanswered costs one ring attempt, and the fallback below
	 * still rings the handset over the old link.
	 */
	if (s->ring_state == RING_RELEASING) {
		if (s->base.main_link.active && now < s->ring_release_until)
			return;
		printf("ring: link %s -- paging now\n",
		       s->base.main_link.active ? "did not go down in time, paging anyway"
				     : "released");
		s->ring_state = RING_PAGING;
	}

	if (s->ring_pages && now - s->ring_last < RING_PAGE_INTERVAL)
		return;
	if (s->ring_pages >= RING_PAGE_RETRIES) {
		/*
		 * An attached handset does not answer our page. Measured
		 * 2026-10-23 (vF423): with the attachment link up, 16 pages
		 * went unanswered, where vF411 -- same build, same mails, no
		 * link up -- got a page response on attempt 1. So [CR A-0052]'s
		 * "paging an idle-locked attached handset is routine" does not
		 * hold for this handset in this state: the C4 ignores the page
		 * while it holds a link.
		 *
		 * Fall back to the SETUP on the existing link so the handset
		 * still rings. That call will have no audio -- the chip never
		 * allocates a traffic bearer without a page response, so the
		 * configurator never runs -- but a call that rings and is
		 * silent beats a handset that does not ring at all, which is
		 * what paging alone gives us here.
		 */
		if (s->base.main_link.active) {
			printf("ring: %u pages unanswered -- the handset is "
			       "attached and ignores them; falling back to the "
			       "SETUP on the existing link; EXPECT NO AUDIO\n",
			       s->ring_pages);
			ring_queue_setup(s, now, "a link was already up");
			return;
		}
		printf("ring: the handset did not answer %u pages, giving up\n",
		       s->ring_pages);
		s->ring_state = RING_DONE;
		return;
	}
	s->ring_pages++;
	s->ring_last = now;
	send_page(s);
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
 * Load the call's key into the chip (0x0208) before our over-air
 * {CIPHER-REQUEST}, as stock does [CR A-0126]; docs/stub-api.md §2.1:
 *
 *   param[0]      the link (connection handle)
 *   param[1..8]   the 8-byte call key: our DCK in its own byte order
 *                 (bench 2026-10-28; [CR A-0028]'s reversal is wrong here)
 *   param[9..23]  00
 *   param[24..25] FF FF
 *   param[26..27] 00 00
 *
 * Without it the chip's MAC switches to ciphering on the handset's signal
 * with no key and drops the link ~200 ms later (bench test 17). The chip
 * answers with a one-byte 0x0209 <link>. Our earlier layout carried a
 * guessed selector and RES/RAND_F in bytes 9..20, and was inert
 * (2026-10-19).
 */
static void send_chip_cipher(struct stubd *s)
{
	(void)dect_base_chip_key(&s->base, &s->base.main_link);
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
	if (s->led_traffic != s->led_traffic_seen || s->reg.call_bound) {
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
		radio_on && s->reg_open ? "open" : "closed");
	if (radio_on && s->reg_open && s->reg_open_until > now)
		fprintf(f, ",\"registration_left_s\":%llu",
			(unsigned long long)((s->reg_open_until - now + 999) /
					     1000));
	/* what the base is set to, for the Monitor's base section */
	fprintf(f, ",\"api\":%u,\"cipher\":%s,\"locauth\":%s,"
		"\"wideband\":%s,\"twocalls\":%s,\"line\":",
		OPENDECTCELL_API_VERSION, s->no_cipher ? "false" : "true",
		s->audio.locauth ? "true" : "false",
		s->audio.narrowband ? "false" : "true",
		s->audio.two_calls ? "true" : "false");
	json_str(f, s->audio.line_name ? s->audio.line_name : "Line 1");
	fprintf(f, ",\"handsets\":[");
	first = 1;
	for (i = 0; i < s->base.n_hs; i++) {
		unsigned k, linked = 0;
		uint64_t seen = s->hs[i].last_seen_ms;

		/* the handsets the base holds: not a store just deregistered */
		if ((i != s->base.cur && !s->hs[i].loaded) || s->hs[i].absent)
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
			if (s->side[k].bl.active && s->side[k].adopted == (int)i)
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
		if (s->hs[i].cap_known)
			fprintf(f, ",\"part3\":%s",
				s->hs[i].cap_part3 ? "true" : "false");
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
		if (s->side[i].bl.active)
			state_link(s, f, &s->side[i].bl.link,
				   s->side[i].lane >= 0 &&
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

static const uint8_t *hs_port_id(const struct stubd *s, unsigned i,
				 size_t *len);

/* A store that does not exist yet: somewhere a new handset can enrol. */
static int hs_free(const struct stubd *s)
{
	unsigned i;

	for (i = 0; i < s->base.n_hs; i++)
		if (!s->hs[i].loaded && !s->hs[i].remote && s->hs[i].absent)
			return (int)i;
	return -1;
}

/*
 * Who may enrol on the main link. Nobody unless the window is open; then
 * anyone with the PIN when a free store can take a new handset (the
 * enrolment is routed there, enrol_route()), else only the current
 * store's own handset re-enrolling.
 */
static uint8_t reg_policy(const struct stubd *s)
{
	if (!s->reg_open)
		return DECT_MM_REG_CLOSED;
	if (hs_free(s) >= 0)
		return DECT_MM_REG_OPEN;
	return s->base.rec.port_id_len ? DECT_MM_REG_SAME : DECT_MM_REG_OPEN;
}

/*
 * Open or close the registration window: the chip's mode (0x022B 'A' /
 * 0x022C 01 [CR COMMAND_SET §6.2]) and MM's policy together. secs = 0
 * keeps it open until closed.
 */
static void reg_window(struct stubd *s, int open, unsigned secs,
		       uint64_t now, const char *why)
{
	if (send_mail_spec(s, open ? "0x22:0x02:0x022b:41"
				   : "0x22:0x02:0x022c:01"))
		printf("reg: could not send the registration mode mail\n");
	s->reg_open = open;
	s->reg_open_until = open && secs ? now + secs * 1000ull : 0;
	if (!open)
		s->reg_profile[0] = '\0';	/* the choice was for this window */
	if (s->base.main_link.active)
		s->base.main_link.link.mm.cfg.registration = reg_policy(s);
	if (open && secs)
		printf("reg: registration OPEN for %u s (%s)\n", secs, why);
	else if (open)
		printf("reg: registration OPEN until closed (%s)\n", why);
	if (!open)
		printf("reg: registration closed (%s)\n", why);
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
	if (s->reg_open && s->reg_open_until && now >= s->reg_open_until)
		reg_window(s, 0, 0, now, "window elapsed");
}

/*
 * A handset is enrolling (MM sent KEY-ALLOCATE and waits for its PIN
 * answer). Serve it from the right store before ACCESS-RIGHTS-ACCEPT goes
 * out: its own, if we hold it, else a free one -- so a new handset no
 * longer lands in the first store and overwrites the ring target.
 */
static void enrol_route(struct stubd *s)
{
	const uint8_t *id = s->base.main_link.link.mm.pend_id;
	size_t len = s->base.main_link.link.mm.pend_id_len, l;
	unsigned i;
	int target = -1;
	char name[DECT_IWU_NAME_MAX];

	for (i = 0; i < s->base.n_hs && target < 0; i++) {
		const uint8_t *p = hs_port_id(s, i, &l);

		if (len && l == len && !memcmp(p, id, len))
			target = (int)i;
	}
	if (target < 0)
		target = hs_free(s);
	if (target < 0 || (unsigned)target == s->base.cur) {
		printf("reg: enrolment into %s (slot %u)\n", s->sub_path,
		       (unsigned)stubd_sub_slot(s));
		return;
	}
	hs_switch(s, (unsigned)target, " (enrolment)");
	s->base.main_link.enrol_switched = 1;	/* undone if the PIN fails */
	s->base.main_link.link.mm.cfg.sub_slot = stubd_sub_slot(s);
	snprintf(name, sizeof(name), "DECT %u", (unsigned)stubd_sub_slot(s));
	dect_iwu_add_handset(&s->base.main_link.link.cc.iwu, stubd_sub_slot(s), name);
	dect_iwu_set_own(&s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
	iwu_lines(s, &s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
	printf("reg: enrolment routed to %s (slot %u)\n", s->sub_path,
	       (unsigned)stubd_sub_slot(s));
}

/*
 * The main link's CC entity, fresh: what reg_start() installs per
 * connection, and side_promote() when a side link becomes the main one.
 */
static void main_cc_setup(struct stubd *s)
{
	unsigned i;

	dect_cc_init(&s->base.main_link.link.cc);
	s->cc_rejected_seen = 0;
	/*
	 * The lists a CAT-iq handset reads before it calls itself synchronised
	 * (TS 102 527-3 7.4.10): one line, and the Internal Names List with
	 * the handset this daemon serves -- terminal identity number = its
	 * chip subscription slot, which is also the low octet of the TPUI we
	 * assigned it, and the default name the standard suggests. Installed
	 * per connection because dect_link_init() zeroes the responder.
	 */
	{
		uint8_t tid = stubd_sub_slot(s) ? stubd_sub_slot(s) : 1u;
		char hsname[DECT_IWU_NAME_MAX];

		dect_iwu_set_line(&s->base.main_link.link.cc.iwu, STUBD_LINE_ID,
				  s->audio.line_name ? s->audio.line_name
						     : "Line 1");
		if (s->base.n_hs && s->base.cur < s->base.n_hs)
			hs_name(s, s->base.cur, hsname, sizeof(hsname));
		else
			snprintf(hsname, sizeof(hsname), "DECT %u",
				 (unsigned)tid);
		dect_iwu_add_handset(&s->base.main_link.link.cc.iwu, tid, hsname);
		for (i = 0; i < s->base.n_hs; i++) {
			if (i == s->base.cur || !s->hs[i].loaded)
				continue;
			hs_name(s, i, hsname, sizeof(hsname));
			dect_iwu_add_handset(&s->base.main_link.link.cc.iwu,
					     (uint8_t)s->base.hs[i].slot, hsname);
		}
		dect_iwu_set_own(&s->base.main_link.link.cc.iwu, tid);
		iwu_lines(s, &s->base.main_link.link.cc.iwu, tid);
	}
	/*
	 * No <<CODEC-LIST>> in FP-originated setups by default: basic speech
	 * (`e0 80`) already means G.726, the codec our audio path carries.
	 * TS 102 527-1 §7.3.3 lets a list in {CC-SETUP} only if it holds at
	 * least G.722 and G.726; the G.726-only list we sent from 2026-10-22
	 * ("give it something to reserve against", written when the A540's
	 * refusal was misread as a resource problem -- it was the transaction
	 * value) was non-conformant. Runs 31-33 (2026-10-25): all three
	 * Gigasets ring without it. codeclist=HEX still sends one, e.g.
	 * codeclist=90020084 for the old list.
	 */
	if (s->audio.codeclist_len)
		dect_cc_set_codec_list(&s->base.main_link.link.cc, s->audio.codeclist,
				       s->audio.codeclist_len);
	else
		dect_cc_set_codec_list(&s->base.main_link.link.cc, NULL, 0);
	dect_cc_set_signal(&s->base.main_link.link.cc, s->audio.signal_on,
			   s->audio.signal_value);
	dect_cc_set_wideband(&s->base.main_link.link.cc, !s->audio.narrowband);
	dect_cc_set_overlap(&s->base.main_link.link.cc, !s->audio.no_overlap);
	dect_cc_set_route_complete(&s->base.main_link.link.cc, 1);
	dect_cc_set_call_status(&s->base.main_link.link.cc, !s->audio.no_call_status,
				s->audio.callinfo.has_call_id ?
				s->audio.callinfo.call_id : 1u);
	s->base.main_link.link.call_status_for = call_status_for;
	s->base.main_link.link.call_id_for = call_id_for;
	s->base.main_link.link.call_status_ctx = s;
	/* our one line for FP-managed selection ('None', §7.4.5.2.4) */
	dect_cc_set_default_line(&s->base.main_link.link.cc, STUBD_LINE_ID,
				 DECT_CALLINFO_NDT_DELAY);
	dect_cc_set_callinfo(&s->base.main_link.link.cc, &s->audio.callinfo);
	/* the lowest free transaction value, 0 (EN 300 444 §6.9.2) */
	dect_cc_set_next_ti(&s->base.main_link.link.cc, 0);
}

static void xlane_release(struct stubd *s, struct side_link *sl);

static void reg_start(struct stubd *s, uint8_t handle)
{
	struct dect_mm_cfg mcfg;
	unsigned i, hold = 0;

	/* a new connection is nobody's yet; default to the ring target */
	hs_switch(s, s->ring_state == RING_IDLE || s->ring_state == RING_DONE
		  ? 0u : s->ring_hs, " (new connection)");
	{
		struct side_link *sl = side_find(s, handle);

		if (sl) {
			xlane_release(s, sl);	/* never leak its lane */
			sl->bl.active = 0;		/* the main link takes it */
		}
	}
	memset(&mcfg, 0, sizeof(mcfg));
	/* subslot=N wins, else the slot the store was saved with, else 0 --
	 * resolved once, in stubd_sub_slot(). */
	mcfg.sub_slot = stubd_sub_slot(s);
	if (s->pin)
		snprintf(mcfg.pin, sizeof(mcfg.pin), "%s", s->pin);
	mcfg.authenticate = 1;
	mcfg.narrowband_only = (uint8_t)s->audio.narrowband;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);
	/*
	 * Who may enrol. Closed unless regopen. With the first store already
	 * holding a handset, only that handset may re-enrol into it (or a held
	 * one, once adopted, into its own): a new handset used to land in the
	 * first store and overwrite the ring target (review, 2026-10-25). A
	 * new handset needs a store of its own as the first -s.
	 */
	mcfg.defer_unknown = s->audio.remote_path != NULL;
	mcfg.auth_locate = (uint8_t)s->audio.locauth;
	mcfg.locate_validity = (uint8_t)s->audio.locate_validity;
	s->fetch_retrans_seen = 0;	/* MM's counter restarts with the link */
	mcfg.registration = reg_policy(s);
	if (s->audio.ipuio_set) {
		/* <<PORTABLE-IDENTITY>>: IPUI, 40 bits: PUT 1 (O), the number */
		const uint32_t n = s->audio.ipuio;

		mcfg.assign_id[0] = 0x80u;
		mcfg.assign_id[1] = 0x80u | 40u;
		mcfg.assign_id[2] = 0x10u;
		mcfg.assign_id[3] = (uint8_t)(n >> 24);
		mcfg.assign_id[4] = (uint8_t)(n >> 16);
		mcfg.assign_id[5] = (uint8_t)(n >> 8);
		mcfg.assign_id[6] = (uint8_t)n;
		mcfg.assign_id_len = 7;
	}

	s->reg.call_bound = 0;
	s->reg.call_wideband = 0;
	s->reg.connect_staged = 0;
	s->reg.nonspeech_noted = 0;
	s->reg.cipher_started = 0;
	s->base.main_link.cipher_done = 0;
	s->reg.cipher_reject_seen = 0;
	s->base.main_link.auth_refused = 0;
	s->reg.cipher_timeout_seen = 0;
	/* the other handsets this link may turn out to be: loaded ones, and a
	 * roamer once fetched */
	for (i = 0; i < s->base.n_hs; i++)
		if (s->hs[i].loaded && !(s->hs[i].remote && !s->hs[i].fetched))
			hold |= 1u << i;
	dect_base_link_open(&s->base, &s->base.main_link, handle, &mcfg, hold);
	dect_mm_set_uak_lookup(&s->base.main_link.link.mm, uak_lookup, s);
	dect_link_set_tx_log(&s->base.main_link.link, log_nwk_out, s);
	s->reg.time_sent_for = 0;
	main_cc_setup(s);
	printf("reg: connection up (handle %u), C-plane armed\n", handle);
}

/* Slot and identity of handset table entry i, current or stashed. */
static unsigned hs_slot(const struct stubd *s, unsigned i)
{
	return dect_base_slot(&s->base, i);
}

/* The handset's name for the lists and the internal caller: its own, or
 * "DECT n" -- the default the daemon has always used. */
static void hs_name(const struct stubd *s, unsigned i, char *buf, size_t cap)
{
	if (s->hs[i].name[0])
		snprintf(buf, cap, "%s", s->hs[i].name);
	else
		snprintf(buf, cap, "DECT %u", hs_slot(s, i));
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

	s->hs[i].line_name[0][0] = s->hs[i].line_name[1][0] = '\0';
	s->hs[i].line2 = 0;
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
		snprintf(s->hs[i].line_name[buf[0] - '1'],
			 sizeof(s->hs[i].line_name[0]), "%.15s", nm);
		if (buf[0] == '2')
			s->hs[i].line2 = 1;
	}
	fclose(f);
	printf("sub: DECT %u's lines: 1 \"%s\"%s%s%s\n", hs_slot(s, i),
	       s->hs[i].line_name[0], s->hs[i].line2 ? ", 2 \"" : "",
	       s->hs[i].line2 ? s->hs[i].line_name[1] : "",
	       s->hs[i].line2 ? "\"" : "");
}

/* The lines a link's Line Settings List offers: those of its handset. */
static struct dect_contacts stubd_contacts;	/* every link's Contact List */
static struct dect_calls stubd_calls;		/* ... and call lists */

static void iwu_lines(struct stubd *s, struct dect_iwu *iwu, unsigned slot)
{
	const char *def = s->audio.line_name ? s->audio.line_name : "Line 1";
	uint32_t att = slot >= 1u && slot <= 32u ? 1u << (slot - 1u) : 0u;
	unsigned i;

	iwu->contacts = s->contacts_path ? &stubd_contacts : NULL;
	iwu->calls = &stubd_calls;

	for (i = 0; i < s->base.n_hs; i++)
		if (hs_slot(s, i) == slot)
			break;
	if (!slot || i == s->base.n_hs)
		return;
	dect_iwu_clear_lines(iwu);
	(void)dect_iwu_add_line(iwu, STUBD_LINE_ID, s->hs[i].line_name[0][0]
				? s->hs[i].line_name[0] : def, att);
	if (s->hs[i].line2)
		(void)dect_iwu_add_line(iwu, STUBD_LINE_ID + 1u,
					s->hs[i].line_name[1][0]
					? s->hs[i].line_name[1] : "Line 2", att);
}

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
#define CALLS_TEXT_MAX	(3u * DECT_CALLS_MAX * 160u + 32u)

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
	n = fread(s->hs[i].name, 1, sizeof(s->hs[i].name) - 1u, f);
	fclose(f);
	s->hs[i].name[n] = '\0';
	s->hs[i].name[strcspn(s->hs[i].name, "\r\n")] = '\0';
	if (s->hs[i].name[0])
		printf("sub: %s is called \"%s\"\n", s->hs[i].path,
		       s->hs[i].name);
}

static void name_save(const struct stubd *s, unsigned i)
{
	char path[256];
	FILE *f;

	name_path(s, i, path, sizeof(path));
	if (!s->hs[i].name[0]) {
		(void)remove(path);		/* back to the default */
		return;
	}
	f = fopen(path, "w");
	if (!f) {
		printf("names: cannot write %s\n", path);
		return;
	}
	fprintf(f, "%s\n", s->hs[i].name);
	fclose(f);
}

static void names_spread(struct stubd *s, unsigned i);

/* The handset table index a link serves, or -1. */
static int hs_of_link(const struct stubd *s, const struct dect_link *l)
{
	int i = l->mm.adopted;

	if (i < 0 && l == &s->base.main_link.link)
		i = (int)s->base.cur;
	return i >= 0 && (unsigned)i < s->base.n_hs ? i : -1;
}

static int profile_load(const char *path, struct dect_profile *p);

#define PROFILE_DIR_DEFAULT	"/etc/opendectcell/profiles"

/*
 * Handset i's profile: <path>.profile names the model's id (and on a
 * second line its firmware version); the device file and its manufacturer
 * file come from the profile directory. Resolved into hs[i].prof and
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
	struct dect_hs_profile *hp = &s->hs[i].prof, def;
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
	if (dect_profile_override(text, n, &s->hs[i].prof, ovr_say, path)) {
		printf("profile: DECT %u's own settings (%s) not applied\n",
		       hs_slot(s, i), path);
		return;
	}
	if (dect_profile_describe(&s->hs[i].prof, desc, sizeof(desc)))
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

/*
 * List change notifications (TS 102 527-3 §7.4.10.2), a CISS {FACILITY}
 * with <<EVENTS-NOTIFICATION>> on a link that is up. Only to Part 3
 * handsets: the notifications are a CAT-iq Part 3 procedure, and a Part 1
 * or GAP handset was never sent one by us; what it would make of one is
 * untested. A handset with no link up learns at its next location
 * registration (the CLSS paging path is not built).
 */
static int notify_ok(const struct stubd *s, const struct dect_link *l)
{
	int i = hs_of_link(s, l);

	if (i >= 0 && s->hs[i].prof.notify != DECT_PROF_AUTO)
		return s->hs[i].prof.notify == DECT_PROF_ON;
	if (l->mm.peer_cap.present)
		return l->mm.peer_cap.ng_part3;
	/* a link up for a page or a call: what the handset said before */
	return i >= 0 && s->hs[i].cap_known && s->hs[i].cap_part3;
}

/*
 * After a location registration: one indication for the Line Settings
 * List per line the handset is attached to, to that handset only, with
 * the line "relating to" and the entries for that line, 1 (§7.4.10.2.1,
 * note 1; spec sweep M1.3).
 */
static void locate_notify(const struct stubd *s, struct dect_link *l)
{
	const struct dect_iwu *iwu = &l->cc.iwu;
	unsigned i;

	if (!notify_ok(s, l))
		return;
	for (i = 0; i < iwu->n_lines; i++) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_LINE_SETTINGS, 1 };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t n;

		if (!dect_iwu_attached(iwu, iwu->lines[i].id,
				       iwu->own_terminal_id))
			continue;
		n = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
					       iwu->lines[i].id,
					       DECT_EVENT_LINE_RELATING_TO);
		if (n && dect_link_send_ciss(l, msg, n) == 0)
			printf("reg: list change notice sent for line %u "
			       "(Line Settings List)\n", iwu->lines[i].id);
	}
	/*
	 * One for the List of Supported Lists too (no line field, so no line;
	 * one entry). A handset keeps the copy it read once: the A540
	 * re-reads only 06 and 08 when it reconnects, so it never learned of
	 * the Contact List or the call lists until told (2026-10-30).
	 */
	if (iwu->contacts || iwu->calls) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_SUPPORTED, 1 };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t n = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						      DECT_EVENT_NO_LINE, 0);

		if (n && dect_link_send_ciss(l, msg, n) == 0)
			printf("reg: list change notice sent (List of Supported "
			       "Lists)\n");
	}
}

/*
 * Voice message waiting notifications (TS 102 527-3 §7.4.1.2, table 14)
 * to handset i on link l, for the lines in its mwi_due: the count with
 * the line, "external call" subtype. Part 3 handsets only, as the other
 * notifications (notify_ok()). Returns the number queued.
 */
static unsigned mwi_send(struct stubd *s, struct dect_link *l, unsigned i)
{
	unsigned n, sent = 0;

	if (!notify_ok(s, l)) {
		s->hs[i].mwi_due = 0;
		return 0;
	}
	for (n = 1; n <= 2u; n++) {
		struct dect_event ev = { DECT_EVENT_MESSAGE_WAITING,
					 DECT_EVENT_VOICE, s->hs[i].mwi[n - 1u] };
		uint8_t msg[DECT_LAPC_MSG_MAX], bit = (uint8_t)(1u << (n - 1u));
		size_t len;

		if (!(s->hs[i].mwi_due & bit))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						 (uint8_t)n,
						 DECT_EVENT_LINE_EXTERNAL);
		if (!len || dect_link_send_ciss(l, msg, len))
			continue;	/* the queue is full: next time */
		s->hs[i].mwi_due &= (uint8_t)~bit;
		sent++;
		printf("mwi: DECT %u, line %u: %u message(s) waiting\n",
		       hs_slot(s, i), n, ev.multiplicity);
	}
	return sent;
}

/*
 * After a location registration: the waiting messages again, once per
 * line we know them for (§7.4.1.2, "notification update").
 */
static void mwi_locate(struct stubd *s, struct dect_link *l)
{
	int i = hs_of_link(s, l);

	if (i < 0 || !s->hs[i].mwi_known)
		return;
	s->hs[i].mwi_due = s->hs[i].mwi_known;
	(void)mwi_send(s, l, (unsigned)i);
}

/*
 * Missed call notifications (TS 102 527-3 §7.4.1.3, table 15) to handset i
 * on link l, for the lines in its mc_due: the unread missed calls on that
 * line, and with it the list change indication for the Missed Calls List
 * (its entries on the line), "line identifier for external call". Subtype
 * 01H only for a call just missed. Part 3 handsets only (notify_ok()).
 */
static unsigned missed_send(struct stubd *s, struct dect_link *l, unsigned i)
{
	unsigned n, sent = 0, tid = hs_slot(s, i);

	if (!notify_ok(s, l)) {
		s->hs[i].mc_due = s->hs[i].mc_new = 0;
		return 0;
	}
	for (n = 1; n <= 2u; n++) {
		uint8_t bit = (uint8_t)(1u << (n - 1u));
		struct dect_event ev[2] = {
			{ DECT_EVENT_MISSED_CALL, s->hs[i].mc_new & bit ? 1u : 2u,
			  dect_calls_missed_unread(&stubd_calls, tid, n) },
			{ DECT_EVENT_LIST_CHANGE, DECT_LIA_LIST_MISSED,
			  dect_calls_missed_total(&stubd_calls, tid, n) },
		};
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t len;

		if (!(s->hs[i].mc_due & bit))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), ev, 2,
						 (uint8_t)n,
						 DECT_EVENT_LINE_EXTERNAL);
		if (!len || dect_link_send_ciss(l, msg, len))
			continue;	/* the queue is full: next time */
		s->hs[i].mc_due &= (uint8_t)~bit;
		s->hs[i].mc_new &= (uint8_t)~bit;
		sent++;
		printf("missed: DECT %u, line %u: %u unread of %u (%s)\n",
		       tid, n, ev[0].multiplicity, ev[1].multiplicity,
		       ev[0].subtype == 1u ? "a new one" : "update");
	}
	return sent;
}

/* The lines handset i has: 1, and 2 if it has a second. */
static uint8_t hs_lines(const struct stubd *s, unsigned i)
{
	return (uint8_t)(s->hs[i].line2 ? 3u : 1u);
}

/*
 * After a location registration: the missed calls once per line the
 * handset has (§7.4.1.3, subtype 02H).
 */
static void missed_locate(struct stubd *s, struct dect_link *l)
{
	int i = hs_of_link(s, l);

	if (i < 0)
		return;
	s->hs[i].mc_due = hs_lines(s, (unsigned)i);
	(void)missed_send(s, l, (unsigned)i);
}

static struct dect_link *link_known_hs(struct stubd *s, int i);

/* Could handset i hear notifications, from what it told us before? */
static int notify_known(const struct stubd *s, unsigned i)
{
	if (s->hs[i].prof.notify != DECT_PROF_AUTO)
		return s->hs[i].prof.notify == DECT_PROF_ON;
	return s->hs[i].cap_known && s->hs[i].cap_part3;
}

/*
 * Due notifications to handset i: now if it has a link, else at the next
 * paging round (tu.want, time_tick()).
 */
static void missed_due(struct stubd *s, unsigned i, uint8_t lines, int fresh)
{
	struct dect_link *l;

	lines &= hs_lines(s, i);
	if (!lines || !s->hs[i].loaded || !notify_known(s, i))
		return;
	s->hs[i].mc_due |= lines;
	if (fresh)
		s->hs[i].mc_new |= lines;
	l = link_known_hs(s, (int)i);
	if (l)
		(void)missed_send(s, l, i);
	else
		s->tu.want |= 1u << i;
}

/*
 * The Missed Calls List changed on a handset (read, deleted): every handset
 * hears the new counts (§7.4.1.3, "entry modified or deleted", subtype
 * 02H) -- one in a link at once, the others at the next paging round.
 */
static void missed_changed_tick(struct stubd *s)
{
	unsigned i;

	if (!stubd_calls.missed_changed)
		return;
	stubd_calls.missed_changed = 0;
	for (i = 0; i < s->base.n_hs; i++)
		missed_due(s, i, hs_lines(s, i), 0);
}

/*
 * The Internal Names List changed -- a name saved, a handset added: tell
 * every handset that is up (§7.4.10.2.2: "at least ... that other PP", and
 * to all on an addition), no line, the number of entries.
 */
static void names_notify(struct stubd *s)
{
	struct dect_link *links[1 + SIDE_MAX];
	unsigned k, n = 0;

	if (s->base.main_link.active)
		links[n++] = &s->base.main_link.link;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active)
			links[n++] = &s->side[k].bl.link;
	for (k = 0; k < n; k++) {
		struct dect_event ev = { DECT_EVENT_LIST_CHANGE,
					 DECT_LIA_LIST_INTERNAL_NAMES,
					 links[k]->cc.iwu.n_handsets };
		uint8_t msg[DECT_LAPC_MSG_MAX];
		size_t len;

		if (!notify_ok(s, links[k]))
			continue;
		len = dect_events_build_facility(msg, sizeof(msg), &ev, 1,
						 DECT_EVENT_NO_LINE, 0);
		if (len && dect_link_send_ciss(links[k], msg, len) == 0)
			printf("names: list change notice sent to DECT %u\n",
			       links[k]->cc.iwu.own_terminal_id);
	}
}

/*
 * A handset saved a name in the Internal Names List (dect_iwu sets
 * names_changed): keep it for that handset, in its .name file, and show
 * it on every link's list. An empty name is the reset to "DECT n".
 * TS 102 527-3 also wants a list change notification to the other
 * handsets (§7.4.10.2.2); that comes with the {FACILITY} builder.
 */
static void names_take(struct stubd *s, struct dect_link *l)
{
	struct dect_iwu *iwu = &l->cc.iwu;
	unsigned b, j;

	for (b = 0; iwu->names_changed && b < iwu->n_handsets; b++) {
		const struct dect_iwu_handset *h = &iwu->handsets[b];

		if (!(iwu->names_changed & (1u << b)))
			continue;
		iwu->names_changed &= ~(1u << b);
		for (j = 0; j < s->base.n_hs; j++)
			if (hs_slot(s, j) == h->terminal_id)
				break;
		if (j == s->base.n_hs)
			continue;
		snprintf(s->hs[j].name, sizeof(s->hs[j].name), "%s", h->name);
		name_save(s, j);
		printf("names: DECT %u is now %s%s%s\n", h->terminal_id,
		       h->name[0] ? "\"" : "", h->name[0] ? h->name :
		       "back to its default name", h->name[0] ? "\"" : "");
		names_spread(s, j);
		names_notify(s);
		ctl_hs(s, j, "registered");
	}
	iwu->names_changed = 0;
}

static void names_spread(struct stubd *s, unsigned i)
{
	char name[DECT_IWU_NAME_MAX];
	uint8_t tid = (uint8_t)hs_slot(s, i);
	unsigned k;

	hs_name(s, i, name, sizeof(name));
	if (s->base.main_link.active)
		(void)dect_iwu_add_handset(&s->base.main_link.link.cc.iwu, tid, name);
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active)
			(void)dect_iwu_add_handset(&s->side[k].bl.link.cc.iwu,
						   tid, name);
}

/*
 * The UAK of the handset a link serves, so MM can answer the handset's
 * authentication of the base (EN 300 444 §8.23; mandatory inside access
 * rights termination, §8.31). The main link serves the current store
 * unless it adopted another; a side link only the one it adopted.
 */
static void uak_for_link(struct stubd *s, struct dect_link *l, int main)
{
	int i = l->mm.adopted >= 0 ? l->mm.adopted
				   : main ? (int)s->base.cur : -1;

	if (i < 0 || (unsigned)i >= s->base.n_hs)
		return;
	if ((unsigned)i == s->base.cur) {
		if (s->base.rec.uak_valid)
			dect_mm_set_uak(&l->mm, s->base.rec.uak);
	} else if (s->base.hs[i].uak_valid) {
		dect_mm_set_uak(&l->mm, s->base.hs[i].uak);
	}
}

/* Is the handset in this slot in a voice call on any link? */
static int slot_in_call(const struct stubd *s, unsigned slot)
{
	const struct dect_link *ls[1 + SIDE_MAX];
	unsigned k, n = 0;

	if (s->base.main_link.active)
		ls[n++] = &s->base.main_link.link;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active)
			ls[n++] = &s->side[k].bl.link;
	for (k = 0; k < n; k++) {
		int i = hs_of_link(s, ls[k]);

		if (i >= 0 && hs_slot(s, (unsigned)i) == slot &&
		    dect_cc_busy(&ls[k]->cc) &&
		    (ls[k]->cc.peer_basic_service >> 4) != 0x2u)
			return 1;
	}
	return 0;
}

static void dereg_begin(struct stubd *s, unsigned slot, uint64_t now,
			const char *who);

/*
 * What the lists need from the daemon (7.4.10.4.6, 7.4.11.1): the base PIN
 * to check an entered one against, whether the link is ciphered (a PIN
 * only travels ciphered), which handsets are in a call (no deletion then),
 * and the deletion a handset asked for, which is a deregistration.
 */
#define PIN_WRONG_MAX		10u
#define PIN_WRONG_WINDOW_MS	3600000u	/* the count's window */
#define PIN_LOCK_MS		3600000u	/* no PIN check after that */

static void lists_tick(struct stubd *s, struct dect_link *l)
{
	struct dect_iwu *iwu = &l->cc.iwu;
	uint64_t now = mono_ms();
	unsigned j;

	if (iwu->stats.pin_wrong != iwu->pin_wrong_seen) {
		if (!s->pin_wrong_since ||
		    now - s->pin_wrong_since > PIN_WRONG_WINDOW_MS) {
			s->pin_wrong = 0;
			s->pin_wrong_since = now;
		}
		s->pin_wrong += iwu->stats.pin_wrong - iwu->pin_wrong_seen;
		iwu->pin_wrong_seen = iwu->stats.pin_wrong;
		printf("list: wrong PIN entered (%u within the hour)\n",
		       s->pin_wrong);
		if (s->pin_wrong >= PIN_WRONG_MAX && now >= s->pin_locked_until) {
			s->pin_locked_until = now + PIN_LOCK_MS;
			printf("list: %u wrong PINs -- no PIN is accepted for "
			       "the next hour\n", s->pin_wrong);
		}
		fflush(stdout);
	}
	if (now < s->pin_locked_until) {
		/* nothing to compare against: every entry is wrong */
		iwu->pin_known = 0;
		iwu->pin_ok = 0;
	} else if (!iwu->pin_known && s->pin) {
		uint8_t ac[4];

		if (!dect_dsaa_pin_to_ac(s->pin, ac, sizeof(ac)))
			dect_iwu_set_pin(iwu, ac);
	}
	iwu->ciphered = l->mm.cipher_on;
	for (j = 0; j < iwu->n_handsets; j++)
		iwu->handsets[j].in_call =
			(uint8_t)slot_in_call(s, iwu->handsets[j].terminal_id);
	if (iwu->delete_request) {
		char who[48];
		unsigned slot = iwu->delete_request;

		iwu->delete_request = 0;
		snprintf(who, sizeof(who), "DECT %u's handset list (PIN given)",
			 iwu->own_terminal_id);
		dereg_begin(s, slot, mono_ms(), who);
	}
}

static void names_tick(struct stubd *s)
{
	unsigned k;

	if (want_profiles) {
		want_profiles = 0;
		profiles_resolve_all(s, "SIGHUP");
	}
	if (s->base.main_link.active) {
		names_take(s, &s->base.main_link.link);
		uak_for_link(s, &s->base.main_link.link, 1);
		lists_tick(s, &s->base.main_link.link);
	}
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active) {
			names_take(s, &s->side[k].bl.link);
			uak_for_link(s, &s->side[k].bl.link, 0);
			lists_tick(s, &s->side[k].bl.link);
		}
}

static const uint8_t *hs_port_id(const struct stubd *s, unsigned i,
				 size_t *len)
{
	if (i == s->base.cur) {
		*len = s->base.rec.port_id_len;
		return s->base.rec.port_id;
	}
	*len = s->base.hs[i].port_id_len;
	return s->base.hs[i].port_id;
}

static struct side_link *side_find(struct stubd *s, uint8_t handle)
{
	unsigned i;

	for (i = 0; i < SIDE_MAX; i++)
		if (s->side[i].bl.active && s->side[i].bl.handle == handle)
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

/* Give connection `handle` a side link. Returns 0, or -1 if none is free. */
static int side_start(struct stubd *s, uint8_t handle)
{
	struct dect_mm_cfg mcfg;
	struct side_link *sl = side_find(s, handle);
	unsigned i, hold = 0;

	if (sl) {
		printf("side: connection up (handle %u) repeated -- keeping "
		       "its side link\n", handle);
		return 0;
	}
	for (i = 0; i < SIDE_MAX && !sl; i++)
		if (!s->side[i].bl.active)
			sl = &s->side[i];
	if (!sl)
		return -1;

	memset(&mcfg, 0, sizeof(mcfg));
	mcfg.sub_slot = DECT_MM_REG_SLOT;	/* adoption sets the real one */
	/*
	 * A side link cannot keep a subscription -- the persist path lives
	 * on the main link -- so it accepts no registration at all. It used
	 * to run the whole enrolment and the key was never stored (review,
	 * 2026-10-25).
	 */
	mcfg.registration = DECT_MM_REG_CLOSED;
	if (s->pin)
		snprintf(mcfg.pin, sizeof(mcfg.pin), "%s", s->pin);
	mcfg.authenticate = 1;
	mcfg.narrowband_only = (uint8_t)s->audio.narrowband;
	mcfg.auth_locate = (uint8_t)s->audio.locauth;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);
	/*
	 * Every handset we hold, the current one included; strangers meet
	 * the gates as usual. A roamer only once fetched.
	 */
	for (i = 0; i < s->base.n_hs; i++)
		if ((i == s->base.cur || s->hs[i].loaded) &&
		    !(s->hs[i].remote && !s->hs[i].fetched))
			hold |= 1u << i;
	dect_base_side_open(&s->base, &sl->bl, handle, &mcfg, hold);
	dect_mm_set_uak_lookup(&sl->bl.link.mm, uak_lookup, s);
	dect_link_set_tx_log(&sl->bl.link, log_nwk_out_side, s);

	dect_iwu_set_line(&sl->bl.link.cc.iwu, STUBD_LINE_ID,
			  s->audio.line_name ? s->audio.line_name : "Line 1");
	for (i = 0; i < s->base.n_hs; i++) {
		size_t len;
		char name[DECT_IWU_NAME_MAX];

		(void)hs_port_id(s, i, &len);
		if (!len || !(hold & (1u << i)))
			continue;
		hs_name(s, i, name, sizeof(name));
		dect_iwu_add_handset(&sl->bl.link.cc.iwu, (uint8_t)hs_slot(s, i),
				     name);
	}
	dect_cc_set_signal(&sl->bl.link.cc, s->audio.signal_on,
			   s->audio.signal_value);
	/* no audio path here, unless -a twocalls gives it a lane */
	dect_cc_set_busy(&sl->bl.link.cc, !s->audio.two_calls);
	dect_cc_set_wideband(&sl->bl.link.cc, !s->audio.narrowband);
	dect_cc_set_overlap(&sl->bl.link.cc, !s->audio.no_overlap);
	dect_cc_set_route_complete(&sl->bl.link.cc, 1);
	dect_cc_set_call_status(&sl->bl.link.cc, !s->audio.no_call_status,
				s->audio.callinfo.has_call_id ?
				s->audio.callinfo.call_id : 1u);
	sl->bl.link.call_status_for = call_status_for;
	sl->bl.link.call_id_for = call_id_for;
	sl->bl.link.call_status_ctx = s;
	dect_cc_set_default_line(&sl->bl.link.cc, STUBD_LINE_ID,
				 DECT_CALLINFO_NDT_DELAY);
	sl->lane = -1;
	sl->bl.ic_tried = 0;
	sl->bl.dial_len = 0;
	sl->bl.dial_change_ms = 0;
	sl->adopted = -1;
	sl->bl.cipher_done = 0;
	sl->located_seen = 0;
	sl->bl.auth_refused = 0;
	memset(&sl->last_status, 0, sizeof(sl->last_status));
	memset(&sl->frames, 0, sizeof(sl->frames));
	sl->status_ms = 0;
	sl->rejected_seen = 0;
	printf("side: connection up (handle %u) -- handle %u is busy, so it "
	       "gets a side link (location registration and list access "
	       "only)\n", handle, s->base.main_link.handle);
	return 0;
}

static void side_pump(struct stubd *s, struct side_link *sl);

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
		if (s->bc[i].id && s->bc[i].party == BC_PAGING)
			n--;
	for (i = 0; i < SIDE_MAX; i++) {
		const struct side_link *o = &s->side[i];

		if (o != me && o->bl.active && o->lane < 0 &&
		    dect_cc_busy(&o->bl.link.cc))
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
	uint8_t p[3], flags, codec;
	uint16_t mask;

	memset(x, 0, sizeof(*x));
	x->used = 1;
	x->handle = sl->bl.handle;
	x->channel = a->lane_ch[i];
	x->word = a->lane_word[i];
	x->wideband = !a->narrowband &&
		dect_cc_codec(&sl->bl.link.cc) == DECT_CC_CODEC_G722;
	sl->lane = i;
	mask = (uint16_t)(1u << x->channel);

	p[0] = x->handle;
	p[1] = x->channel;		/* selector */
	p[2] = a->gain;
	chip_mail(s, 0x0251u, p, sizeof(p));
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
	uint8_t p[2] = { 0, 0x00u };

	if (x->connected)
		return;
	x->connected = 1;
	p[0] = x->handle;
	chip_mail(s, 0x022Du, p, sizeof(p));	/* bit 7 clear: connect */
	printf("side: audio connect: 0x022D on handle %u (channel %u)\n",
	       x->handle, x->channel);
}

static void xlane_release(struct stubd *s, struct side_link *sl)
{
	if (sl->lane < 0)
		return;
	if (s->xl[sl->lane].connected || s->xl[sl->lane].used)
		send_audio_disconnect(s, sl->bl.handle);
	memset(&s->xl[sl->lane], 0, sizeof(s->xl[0]));
	sl->lane = -1;
	/* the Monitor's figures belong to the call that ended */
	memset(&sl->last_status, 0, sizeof(sl->last_status));
	memset(&sl->frames, 0, sizeof(sl->frames));
	sl->status_ms = 0;
}

/* A side link's number is complete: internal, to the bridge, or local. */
static void side_route(struct stubd *s, struct side_link *sl, uint64_t now)
{
	int p = (int)(sl - s->side);

	sl->bl.ic_tried = 1;
	if (ic_start(s, p, now) || bridge_call_out(s, p, now) ||
	    dect_cc_accept(&sl->bl.link.cc) == 0) {
		dect_link_drain(&sl->bl.link, now);
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
	struct dect_cc *cc = &sl->bl.link.cc;

	if (!s->audio.two_calls)
		return;
	if (!dect_cc_busy(cc))	/* decided for the next SETUP only */
		dect_cc_set_busy(cc, sl->lane < 0 &&
				 lanes_available(s, sl) <= 0);
	if (!sl->bl.ic_tried && number_complete(cc) && call_auth_ok(s, &sl->bl.link))
		side_route(s, sl, now);
	if (!dect_cc_busy(cc))
		sl->bl.ic_tried = 0;
	if (cc->state == DECT_CC_ST_OVERLAP) {
		size_t len;

		(void)dect_cc_keypad(cc, &len);
		if (!sl->bl.dial_change_ms || len != sl->bl.dial_len) {
			sl->bl.dial_len = len;
			sl->bl.dial_change_ms = now;
		} else if (len && now - sl->bl.dial_change_ms >=
			   s->audio.dial_wait_ms && call_auth_ok(s, &sl->bl.link)) {
			printf("side: dialled on handle %u -- connecting\n",
			       sl->bl.handle);
			side_route(s, sl, now);
		}
	} else {
		sl->bl.dial_len = 0;
		sl->bl.dial_change_ms = 0;
	}
	if (sl->lane < 0 && cc->state == DECT_CC_ST_ACTIVE &&
	    (cc->incoming || dect_cc_peer_is_speech(cc)) &&
	    call_auth_ok(s, &sl->bl.link)) {
		int i = xlane_free(s);

		if (i >= 0)
			xlane_bind(s, sl, i);
	}
	if (sl->lane >= 0 && !dect_cc_busy(cc))
		xlane_release(s, sl);
}

/* -------------------------------------------------------------------------
 * Internal call
 */
#define IC_NONE		IC_PARTY_NONE	/* -1 is the main link */
#define IC_PAGES	20u
#define IC_PAGE_MS	3000u
#define IC_BASIC_SVC	0x98u	/* internal call, wideband speech */

static struct dect_cc *ic_cc(struct stubd *s, int side)
{
	return side < 0 ? &s->base.main_link.link.cc : &s->side[side].bl.link.cc;
}

/* The CC identities of handset i, current or stashed. */
static void hs_cc_ids(const struct stubd *s, unsigned i, const uint8_t **port,
		      size_t *plen, const uint8_t **fixed, size_t *flen)
{
	if (i == s->base.cur) {
		*port = s->base.rec.cc_port_id; *plen = s->base.rec.cc_port_id_len;
		*fixed = s->base.rec.cc_fixed_id; *flen = s->base.rec.cc_fixed_id_len;
	} else {
		*port = s->base.hs[i].cc_port_id; *plen = s->base.hs[i].cc_port_id_len;
		*fixed = s->base.hs[i].cc_fixed_id; *flen = s->base.hs[i].cc_fixed_id_len;
	}
}

static int ic_active(const struct stubd *s, int p)
{
	return p == -1 ? s->base.main_link.active : p >= 0 && s->side[p].bl.active;
}

static struct dect_link *ic_link(struct stubd *s, int p)
{
	return p == -1 ? &s->base.main_link.link : &s->side[p].bl.link;
}

static void ic_drain(struct stubd *s, int p, uint64_t now)
{
	dect_link_drain(ic_link(s, p), now);
	if (p == -1)
		pump_lapc(s);
	else
		side_pump(s, &s->side[p]);
}

/* the party has its audio channel bound */
static int ic_has_lane(const struct stubd *s, int p)
{
	return p == -1 ? s->reg.call_bound : p >= 0 && s->side[p].lane >= 0;
}

static void ic_page(struct stubd *s)
{
	const uint8_t p[3] = {
		RING_PAGE_CC | RING_PAGE_WIDEBAND, s->ic.tpui[1], s->ic.tpui[2],
	};

	chip_mail(s, 0x0300u, p, sizeof(p));
	s->ic.pages++;
	printf("intern: paged %s, attempt %u of %u\n", s->hs[s->ic.target].path,
	       s->ic.pages, IC_PAGES);
}

/*
 * The caller (main link: side = -1) finished dialling an internal call:
 * its digits name the target's slot ("2" for DECT 2).
 */
static int ic_start(struct stubd *s, int side, uint64_t now)
{
	struct dect_cc *cc = ic_cc(s, side);
	const uint8_t *k;
	size_t len, j;
	unsigned slot = 0, i;
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	if ((cc->peer_basic_service >> 4) != 0x9u)
		return 0;			/* not an internal call */
	k = dect_cc_keypad(cc, &len);
	for (j = 0; k && j < len; j++)
		if (k[j] >= '0' && k[j] <= '9')
			slot = slot * 10u + (unsigned)(k[j] - '0');
	if (!s->audio.two_calls || s->ic.st != IC_IDLE) {
		/*
		 * With a reason the handset can show (EN 300 175-5 §7.7.18):
		 * another internal call in the way is the system busy (TS 102
		 * 527-3 §7.4.8.3 for a Part 3 handset), the feature switched
		 * off "service not implemented". It went without one.
		 */
		printf("intern: internal call refused (%s)\n",
		       s->audio.two_calls ? "one at a time" :
		       "needs -a twocalls");
		if (s->audio.two_calls)
			(void)dect_cc_busy_system(cc);
		else
			(void)dect_cc_release_reason(cc,
				DECT_CC_REASON_SERVICE_NOT_IMPLEMENTED);
		return 1;
	}
	/* lanes: the target lands on a side link; a side caller needs its own */
	if (lanes_available(s, side >= 0 ? &s->side[side] : NULL) <
	    (side >= 0 ? 2 : 1)) {
		printf("intern: no free audio channel -- system busy\n");
		(void)dect_cc_busy_system(cc);
		return 1;
	}
	for (i = 0; i < s->base.n_hs; i++)
		if (s->hs[i].loaded && hs_slot(s, i) == slot)
			break;
	if (i >= s->base.n_hs || (side < 0 && i == s->base.cur) ||
	    (side >= 0 && (int)i == s->side[side].adopted)) {
		printf("intern: no other handset in slot %u -- releasing, "
		       "\"user unknown\"\n", slot);
		(void)dect_cc_release_reason(cc, DECT_CC_REASON_USER_UNKNOWN);
		return 1;
	}
	(void)dect_cc_proceed(cc);
	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)hs_slot(s, i);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, s->ic.tpui);
	s->ic.st = IC_PAGING;
	s->ic.caller = side;
	s->ic.target = (int)i;
	s->ic.tside = IC_NONE;
	s->ic.pages = 0;
	s->ic.page_at = now;
	s->ic.warned = 0;
	printf("intern: %s calls DECT %u (%s)\n",
	       side < 0 ? "the main link's handset" : "a side link's handset",
	       slot, s->hs[i].path);
	return 1;
}

static void ic_setup(struct stubd *s, int p, uint64_t now, const char *how);
static int page_adopt(struct stubd *s, struct side_link *sl,
		      const struct dect_link_ind *ind, int i, const char *why);

/* The target answered our page on side link sl (NULL: the main link). */
static void ic_target_link(struct stubd *s, struct side_link *sl,
			   const struct dect_link_ind *ind, uint64_t now)
{
	/*
	 * The link adopts the target, as for a deregistration's or the date
	 * and time's page (page_adopt()): authfirst authenticates the call
	 * with the key of the handset the link adopted, and a link that
	 * adopted no one was released with "no key" -- the target rang for
	 * a moment and stopped (7412, 2026-10-29).
	 */
	if (s->ic.st != IC_PAGING ||
	    !page_adopt(s, sl, ind, s->ic.target, " (internal call)"))
		return;
	ic_setup(s, sl ? (int)(sl - s->side) : -1, now, "answered the page");
}

/*
 * Place our SETUP to the target on its side link -- after its page
 * response, or once the link has adopted it (the A540 answers a page with
 * a location registration, M2' and 12b).
 */
static struct dect_clip ic_clip;	/* the internal caller, kept for re-presentation */

static void ic_setup(struct stubd *s, int p, uint64_t now, const char *how)
{
	const uint8_t *port, *fixed;
	size_t plen, flen;
	struct dect_link *l = ic_link(s, p);

	hs_cc_ids(s, (unsigned)s->ic.target, &port, &plen, &fixed, &flen);
	if (!plen || p == s->ic.caller || dect_cc_busy(&l->cc))
		return;
	dect_cc_set_next_ti(&l->cc, 0);
	/* one call, one id on both handsets (TS 102 527-3 §7.4.6.1 note 2) */
	dect_cc_set_call_status(&l->cc, l->cc.call_status,
				ic_cc(s, s->ic.caller)->status_call_id);
	/*
	 * EN 300 444 table 23: <<SIGNAL>> 40H for an internal call, 41H for
	 * an external one, so the handset rings the internal pattern (stock
	 * does the same, A-StockTrace-1). The SETUP is built here, so the
	 * link's configured value is restored at once.
	 */
	dect_cc_set_signal(&l->cc, s->audio.signal_on,
			   DECT_SIGNAL_ALERT_PATTERN0);
	/*
	 * Who is calling (EN 300 444 §8.43/§8.44): the caller's terminal
	 * number, network-specific type in the private plan (table 82), and
	 * its handset name in the DECT alphabet (table 83), both "verified
	 * and passed". Bench test 5 (2026-10-28): the C430A showed no caller.
	 */
	{
		int ci_hs = s->ic.caller < 0 ? (int)s->base.cur
			    : s->side[s->ic.caller].adopted;
		unsigned slot = ci_hs >= 0 ? hs_slot(s, (unsigned)ci_hs) : 0u;
		char num[12], name[DECT_IWU_NAME_MAX + 12];
		struct dect_callinfo prev = l->cc.callinfo, ci;
		int r;

		dect_clip_init(&ic_clip);
		if (slot) {
			snprintf(num, sizeof(num), "%u", slot);
			hs_name(s, (unsigned)ci_hs, name, sizeof(name));
			(void)dect_clip_set_number(&ic_clip, num,
					DECT_CLIP_TON_NETWORK_SPECIFIC,
					DECT_CLIP_NPI_PRIVATE,
					DECT_CLIP_PRES_ALLOWED,
					DECT_CLIP_SCR_USER_PASSED);
			(void)dect_clip_set_name(&ic_clip, name,
					DECT_CLIP_ALPHABET_DECT,
					DECT_CLIP_PRES_ALLOWED,
					DECT_CLIP_SCR_USER_PASSED);
		}
		/*
		 * A Part 3 target: the call id and CS call setup, no line (an
		 * internal call has none), as stock sends the A540: 7e 06 01
		 * 00 81 02 01 81 [CR A-0125]; TS 102 527-3 §7.4.6.3.
		 */
		dect_callinfo_init(&ci);
		if (s->hs[s->ic.target].cap_known &&
		    s->hs[s->ic.target].cap_part3 && !s->audio.no_call_status) {
			ci.has_call_id = 1;
			ci.call_id = l->cc.status_call_id;
			ci.has_call_status = 1;
			ci.call_status = DECT_CS_CALL_SETUP;
		}
		dect_cc_set_callinfo(&l->cc, &ci);
		/*
		 * A Part 3 target gets the number in the SETUP and the name in a
		 * {CC-INFO} once it rings, as stock does [CR A-0130 §1]: the A540
		 * took a name in the SETUP for "via Intern DECT" (bench test 11).
		 */
		s->ic.name_info = slot && ci.has_call_status && ic_clip.name[0];
		{
			struct dect_clip num_only = ic_clip;

			if (s->ic.name_info)
				num_only.name[0] = '\0';
			r = dect_cc_setup_id(&l->cc, port, plen, fixed, flen,
					     target_svc(s, s->ic.target,
						s->audio.narrowband ? 0x90u
								    : IC_BASIC_SVC),
					     slot ? &num_only : NULL);
		}
		dect_cc_set_callinfo(&l->cc, &prev);
		if (r) {
			dect_cc_set_signal(&l->cc, s->audio.signal_on,
					   s->audio.signal_value);
			printf("intern: could not queue the SETUP to the target\n");
			return;
		}
		if (slot)
			printf("intern: caller shown as %s / \"%s\"%s\n", num,
			       name, ci.has_call_status ?
			       ", call id and CS call setup (Part 3)" : "");
	}
	dect_cc_set_signal(&l->cc, s->audio.signal_on, s->audio.signal_value);
	s->ic.tside = p;
	s->ic.st = IC_SETUP;
	printf("intern: the target %s on %s link %u -- SETUP sent\n", how,
	       p == -1 ? "the main" : "side", p == -1 ? s->base.main_link.handle :
	       s->side[p].bl.handle);
	ic_drain(s, p, now);
}

static void ic_end(struct stubd *s, const char *why)
{
	printf("intern: call ended (%s)\n", why);
	s->ic.st = IC_IDLE;
	s->ic.tside = IC_NONE;
}

static int ic_wideband(const struct stubd *s, int p);

#define DEREG_PAGES	10u		/* 3 s apart: 30 s to reach it */
#define DEREG_PAGE_MS	3000u
#define DEREG_REQ_PATH	DECT_RUN_DIR "/deregister"

/*
 * Erase handset i from the base, as stock does after the handset's
 * ACCEPT [CR A-0128]: the slot's 0x0247 entry cleared, its 0x020A record
 * overwritten with zeros, then 0x017C without its bit ([CR A-0129]: the
 * locked-in handset mask; stock rewrites it on every deregistration).
 * The store, its
 * .name, .profile and .cap move aside (<store>.deregistered), so a mistake
 * can be undone by moving them back. Then the lists lose the entry and say so.
 */
static void dereg_erase(struct stubd *s, int i, const char *why)
{
	unsigned slot = hs_slot(s, (unsigned)i), k;
	uint8_t p[1 + DECT_MM_UAK_LEN + 1];
	char from[256], to[288];

	printf("dereg: DECT %u (%s): %s\n", slot, s->hs[i].path, why);
	ctl_hs(s, (unsigned)i, "gone");
	if (slot >= 1u && slot <= DECT_MM_MAX_SLOT) {
		memset(p, 0, sizeof(p));
		p[0] = (uint8_t)slot;
		chip_mail(s, 0x0247u, p, 3);		/* slot, 00, 00 */
		chip_mail(s, 0x020Au, p, sizeof(p));	/* slot, 17 zeros */
		printf("dereg: chip slot %u cleared (0x0247, 0x020A zeros)\n",
		       slot);
	}
	snprintf(to, sizeof(to), "%s.deregistered", s->hs[i].path);
	if (rename(s->hs[i].path, to))
		printf("dereg: could not move %s aside: %s\n", s->hs[i].path,
		       strerror(errno));
	else
		printf("dereg: %s moved to %s\n", s->hs[i].path, to);
	name_path(s, (unsigned)i, from, sizeof(from));
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	/* and its model and capability: the next one here is another */
	snprintf(from, sizeof(from), "%s.profile", s->hs[i].path);
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	snprintf(from, sizeof(from), "%s.cap", s->hs[i].path);
	snprintf(to, sizeof(to), "%s.deregistered", from);
	(void)rename(from, to);
	s->hs[i].loaded = 0;
	s->base.hs[i].uak_valid = 0;
	s->hs[i].absent = 1;		/* free for a new enrolment */
	s->hs[i].name[0] = '\0';
	s->hs[i].cap_known = 0;
	memset(s->hs[i].mwi, 0, sizeof(s->hs[i].mwi));
	s->hs[i].mwi_known = s->hs[i].mwi_due = 0;
	s->tu.want &= ~(1u << i);
	if ((unsigned)i == s->base.cur) {
		s->base.rec.uak_valid = 0;
		s->base.rec.port_id_len = 0;
		s->base.rec.cc_port_id_len = 0;
		s->base.rec.cc_fixed_id_len = 0;
	}
	if (s->base.main_link.active)
		(void)dect_iwu_remove_handset(&s->base.main_link.link.cc.iwu,
					      (uint8_t)slot);
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active)
			(void)dect_iwu_remove_handset(&s->side[k].bl.link.cc.iwu,
						      (uint8_t)slot);
	send_subscription_tail(s);	/* the mask without it */
	names_notify(s);		/* §7.4.10.2.2: one removed */
	fflush(stdout);
}

/*
 * The link that carries handset i by its own word: MM adopted it (its
 * LOCATE-REQUEST, a call it placed, or its page response below). Not the
 * presumption that a main link which has adopted no one serves s->base.cur (as
 * ic_tick still makes) -- a deregistration sent on that went to whichever
 * handset connected next
 * (bench, 2026-10-28: the C430A's location registration got the AS690H's
 * TERMINATE-REQUEST).
 */
static struct dect_link *link_known_hs(struct stubd *s, int i)
{
	unsigned k;

	if (s->base.main_link.active && s->base.main_link.link.mm.adopted == i)
		return &s->base.main_link.link;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active && s->side[k].bl.link.mm.adopted == i)
			return &s->side[k].bl.link;
	return NULL;
}

/*
 * A page response from handset i, which we paged without a call: if it
 * carries that handset's identity, the link adopts it, so link_known_hs()
 * finds it. Returns 1 when it was handset i's.
 */
static int page_adopt(struct stubd *s, struct side_link *sl,
		      const struct dect_link_ind *ind, int i, const char *why)
{
	struct dect_link *l = sl ? &sl->bl.link : &s->base.main_link.link;
	unsigned k;

	for (k = 0; k < l->mm.n_held; k++)
		if (l->mm.held[k].tag == i)
			break;
	if (k == l->mm.n_held || l->mm.held[k].id_len != ind->page_id_len ||
	    memcmp(l->mm.held[k].id, ind->page_id, ind->page_id_len))
		return 0;
	(void)dect_mm_adopt(&l->mm, ind->page_id, ind->page_id_len);
	if (sl) {
		sl->adopted = i;
	} else if ((unsigned)i != s->base.cur) {
		hs_switch(s, (unsigned)i, why);
		dect_iwu_set_own(&s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
		iwu_lines(s, &s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
	}
	return 1;
}

/*
 * A page response while we page a handset to deregister it: if it carries
 * that handset's identity, the link adopts it and dereg_tick() sends the
 * request there. Returns 1 when it was the target's (consumed).
 */
static int dereg_page_response(struct stubd *s, struct side_link *sl,
			       const struct dect_link_ind *ind)
{
	int i = s->dereg.target;

	if (s->dereg.st != DEREG_PAGING || !ind->page_response ||
	    !page_adopt(s, sl, ind, i, " (deregistration)"))
		return 0;
	printf("dereg: DECT %u answered the page with its own identity\n",
	       hs_slot(s, (unsigned)i));
	return 1;
}

/* The same for a handset paged for the date and time (time_tick). */
static int time_page_response(struct stubd *s, struct side_link *sl,
			      const struct dect_link_ind *ind)
{
	int i = s->tu.target;

	if (s->tu.st != TIME_PAGING || !ind->page_response ||
	    !page_adopt(s, sl, ind, i, " (date and time)"))
		return 0;
	s->tu.answered = 1;
	printf("time: DECT %u answered the page\n", hs_slot(s, (unsigned)i));
	return 1;
}

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
	unsigned i;
	struct dect_link *l;
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	for (i = 0; i < s->base.n_hs; i++)
		if (s->hs[i].loaded && hs_slot(s, i) == slot)
			break;
	if (!slot || i == s->base.n_hs) {
		printf("dereg: %s, but no handset we hold is in slot %u\n", who,
		       slot);
		return;
	}
	printf("dereg: DECT %u, asked by %s\n", slot, who);
	if (s->dereg.st != DEREG_IDLE) {
		unsigned k;

		for (k = 0; k < s->dereg.nq; k++)
			if (s->dereg.queue[k] == slot)
				break;
		if (k == s->dereg.nq && s->dereg.nq < DECT_MM_MAX_SLOT &&
		    (int)i != s->dereg.target)
			s->dereg.queue[s->dereg.nq++] = slot;
		printf("dereg: one at a time -- DECT %u waits its turn (%u "
		       "waiting)\n", slot, s->dereg.nq);
		return;
	}
	s->dereg.target = (int)i;
	s->dereg.pages = 0;
	s->dereg.page_at = now;
	l = link_known_hs(s, (int)i);
	if (l && dect_mm_terminate(&l->mm) == 0) {
		s->dereg.st = DEREG_REQUESTED;
		printf("dereg: DECT %u: ACCESS-RIGHTS-TERMINATE-REQUEST on its "
		       "link\n", slot);
		return;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)slot;
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, s->dereg.tpui);
	s->dereg.st = DEREG_PAGING;
	printf("dereg: DECT %u has no link -- paging it\n", slot);
}

static void dereg_tick(struct stubd *s, uint64_t now)
{
	struct dect_link *l;
	unsigned k;
	int i;

	/* a handset that deregistered itself: erase it the same way */
	for (k = 0; k <= SIDE_MAX; k++) {
		struct dect_link *pl = k < SIDE_MAX
			? (s->side[k].bl.active ? &s->side[k].bl.link : NULL)
			: (s->base.main_link.active ? &s->base.main_link.link : NULL);

		if (!pl || !pl->mm.term_by_pt)
			continue;
		pl->mm.term_by_pt = 0;
		i = pl->mm.adopted >= 0 ? pl->mm.adopted
		    : (pl == &s->base.main_link.link ? (int)s->base.cur : -1);
		if (i >= 0 && (unsigned)i < s->base.n_hs && s->hs[i].loaded)
			dereg_erase(s, i, "the handset deregistered itself "
				    "(accepted)");
	}
	if (want_deregister) {
		want_deregister = 0;
		dereg_start(s, now);
	}
	if (s->dereg.st == DEREG_IDLE && s->dereg.nq) {
		unsigned slot = s->dereg.queue[0];

		s->dereg.nq--;
		memmove(s->dereg.queue, s->dereg.queue + 1,
			s->dereg.nq * sizeof(s->dereg.queue[0]));
		dereg_begin(s, slot, now, "the queue");
	}
	if (s->dereg.st == DEREG_IDLE)
		return;
	i = s->dereg.target;
	l = link_known_hs(s, i);
	if (s->dereg.st == DEREG_PAGING) {
		if (l && dect_mm_terminate(&l->mm) == 0) {
			s->dereg.st = DEREG_REQUESTED;
			printf("dereg: DECT %u came up -- "
			       "ACCESS-RIGHTS-TERMINATE-REQUEST\n",
			       hs_slot(s, (unsigned)i));
			return;
		}
		if (now < s->dereg.page_at)
			return;
		if (s->dereg.pages >= DEREG_PAGES) {
			s->dereg.st = DEREG_IDLE;
			dereg_erase(s, i, "not reachable (paged 10 times): "
				    "deleted WITHOUT its confirmation");
			return;
		}
		{
			/* stock's page for this: C-plane only, the TPUI */
			uint8_t p[3] = { 0x08, s->dereg.tpui[1],
					 s->dereg.tpui[2] };

			chip_mail(s, 0x0300u, p, 3);
		}
		s->dereg.pages++;
		s->dereg.page_at = now + DEREG_PAGE_MS;
		return;
	}
	/* REQUESTED */
	if (!l) {
		s->dereg.st = DEREG_IDLE;
		dereg_erase(s, i, "its link went down before it answered: "
			    "deleted WITHOUT its confirmation");
		return;
	}
	switch (l->mm.term_state) {
	case DECT_MM_TERM_ACCEPTED:
		s->dereg.st = DEREG_IDLE;
		dereg_erase(s, i, "the handset confirmed (TERMINATE-ACCEPT)");
		break;
	case DECT_MM_TERM_REJECTED:
		s->dereg.st = DEREG_IDLE;
		dereg_erase(s, i, "the handset REFUSED (TERMINATE-REJECT): "
			    "deleted from the base anyway");
		break;
	case DECT_MM_TERM_FAILED:
		s->dereg.st = DEREG_IDLE;
		dereg_erase(s, i, "no answer to the request, sent twice "
			    "(<MM_access.2>): deleted WITHOUT its "
			    "confirmation");
		break;
	default:
		break;
	}
}

#define TIME_PAGES	5u		/* 3 s apart, as a ring's */
#define TIME_PAGE_MS	3000u
#define TIME_SENT_MS	5000u		/* the FACILITY's ack, at most */

static void drain_nwk(struct stubd *s, uint64_t now);

/*
 * Anything a page of ours could get in the way of: a ring, an internal
 * call, a deregistration, a call or list session on any link. The date and
 * time can wait for all of them.
 */
static int time_base_busy(struct stubd *s)
{
	unsigned k;

	if (s->dereg.st != DEREG_IDLE || s->ic.st != IC_IDLE ||
	    (s->ring_state != RING_IDLE && s->ring_state != RING_DONE))
		return 1;
	if (s->base.main_link.active && dect_cc_busy(&s->base.main_link.link.cc))
		return 1;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active && dect_cc_busy(&s->side[k].bl.link.cc))
			return 1;
	return 0;
}

/* Push what a link has queued towards the chip. */
static void time_pump(struct stubd *s, struct dect_link *l, uint64_t now)
{
	unsigned k;

	if (l == &s->base.main_link.link) {
		drain_nwk(s, now);
		return;
	}
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active && &s->side[k].bl.link == l)
			side_pump(s, &s->side[k]);
}

/* Done with the current handset: on to the next one. */
static void time_next(struct stubd *s)
{
	s->tu.st = TIME_IDLE;
	s->tu.next = (unsigned)s->tu.target + 1u;
}

/*
 * -a timeupdate=MIN: the date and time to every handset we hold, every MIN
 * minutes (maintainer, 2026-10-29). One interval for the base, not one per
 * handset: a handset that attached a minute before a round gets it twice,
 * which is harmless.
 *
 * TS 102 527-3 §7.4.2.0 makes it "the responsibility of the entity to
 * ensure that time and date information within the peer entity is up to
 * date", and §7.4.2.1 says how to reach a handset with no link: "the FP
 * shall initiate indirect link establishment" -- a page, as for a call, with
 * the LCE header "000" (no U-plane). That is the C-plane page the
 * deregistration sends. Then the {FACILITY} (dect_link_send_time), and once
 * it is acknowledged we release the link we brought up, as after a call
 * (EN 300 175-5 §14.2.7). A handset that already has a link gets it there
 * and keeps its link.
 *
 * One handset at a time, and only while the base is otherwise idle
 * (time_base_busy); a handset that does not answer TIME_PAGES pages is off
 * or out of range and is skipped until the next round. The same clock gate
 * as after a location registration: no trustworthy clock, no time.
 *
 * The same round visits a handset with no link that has a voice message
 * waiting notification to hear (tu.want, mwi_send(): §7.4.1.2 wants it
 * sent to every attached handset); outside a time round, only those.
 */
static void time_tick(struct stubd *s, uint64_t now, int radio_on)
{
	uint64_t every = (uint64_t)s->audio.time_every_min * 60000u;
	struct dect_link *l;
	struct dect_time dt;
	long off_s;
	int i;

	if (!radio_on)
		return;
	if (every && !s->tu.next_round)
		s->tu.next_round = now + every;
	if (!s->tu.in_round) {
		int time_due = every && now >= s->tu.next_round;

		if (!time_due && !s->tu.want)
			return;
		s->tu.with_time = 0;
		if (time_due) {
			s->tu.next_round = now + every;
			if (clock_now(s->audio.timesync, &dt, &off_s))
				s->tu.with_time = 1;
			else
				printf("time: update round skipped -- the "
				       "clock is not known to be right (no "
				       "NTP sync reported)\n");
			if (!s->tu.with_time && !s->tu.want)
				return;
		}
		s->tu.in_round = 1;
		s->tu.next = 0;
		s->tu.st = TIME_IDLE;
		if (s->tu.with_time)
			printf("time: update round, every %u min\n",
			       s->audio.time_every_min);
	}

	if (s->tu.st == TIME_IDLE) {
		while (s->tu.next < s->base.n_hs &&
		       (!s->hs[s->tu.next].loaded ||
			(!s->tu.with_time &&
			 !(s->tu.want & (1u << s->tu.next)))))
			s->tu.next++;
		if (s->tu.next >= s->base.n_hs) {
			s->tu.in_round = 0;
			if (s->tu.with_time)
				printf("time: update round done\n");
			return;
		}
		if (time_base_busy(s))
			return;
		s->tu.target = (int)s->tu.next;
		s->tu.pages = 0;
		s->tu.page_at = now;
		s->tu.answered = 0;
		s->tu.st = TIME_PAGING;
		{
			struct dect_mm_cfg cfg;
			struct dect_mm mm;

			memset(&cfg, 0, sizeof(cfg));
			cfg.sub_slot = (uint8_t)hs_slot(s,
						(unsigned)s->tu.target);
			dect_mm_init(&mm, &cfg);
			dect_mm_tpui(&mm, s->tu.tpui);
		}
	}
	i = s->tu.target;
	if (!s->hs[i].loaded) {		/* deregistered meanwhile */
		time_next(s);
		return;
	}
	l = link_known_hs(s, i);

	if (s->tu.st == TIME_PAGING) {
		if (l) {
			s->tu.want &= ~(1u << i);
			(void)mwi_send(s, l, (unsigned)i);
			(void)missed_send(s, l, (unsigned)i);
			if (s->tu.with_time &&
			    clock_now(s->audio.timesync, &dt, &off_s) &&
			    dect_link_send_time(l, &dt) == 0) {
				printf("time: DECT %u: sent the date and time "
				       "(20%02u-%02u-%02u %02u:%02u:%02u, "
				       "GMT%+ld:%02ld)%s\n",
				       hs_slot(s, (unsigned)i), dt.year,
				       dt.month, dt.day, dt.hour, dt.minute,
				       dt.second, off_s / 3600,
				       labs(off_s % 3600) / 60,
				       s->tu.answered ? "" : " on its link");
			} else if (s->tu.with_time) {
				printf("time: DECT %u: could not queue the "
				       "date and time\n",
				       hs_slot(s, (unsigned)i));
			}
			time_pump(s, l, now);
			/* a link we paged up is ours to release, sent or not */
			if (!s->tu.answered) {
				time_next(s);
				return;
			}
			s->tu.st = TIME_SENT;
			s->tu.sent_at = now;
			return;
		}
		/* a call or a ring came first: page again later */
		if (time_base_busy(s)) {
			s->tu.st = TIME_IDLE;
			return;
		}
		if (now < s->tu.page_at)
			return;
		if (s->tu.pages >= TIME_PAGES) {
			printf("time: DECT %u did not answer %u pages -- "
			       "skipped until it reports in\n",
			       hs_slot(s, (unsigned)i), TIME_PAGES);
			s->tu.want &= ~(1u << i);	/* mwi_locate() then */
			time_next(s);
			return;
		}
		{
			/* C-plane only, the TPUI: the deregistration's page */
			uint8_t p[3] = { 0x08, s->tu.tpui[1], s->tu.tpui[2] };

			chip_mail(s, 0x0300u, p, 3);
		}
		if (!s->tu.pages)
			printf("time: DECT %u has no link -- paging it\n",
			       hs_slot(s, (unsigned)i));
		s->tu.pages++;
		s->tu.page_at = now + TIME_PAGE_MS;
		return;
	}

	/* TIME_SENT: release the link we brought up, once the ack is in */
	if (!l) {
		time_next(s);		/* the handset released it itself */
		return;
	}
	if ((l->ciss_n || l->lapc.outstanding || l->lapc.txmsg_len) &&
	    now - s->tu.sent_at < TIME_SENT_MS)
		return;
	if (!dect_cc_busy(&l->cc)) {
		chip_mail(s, 0x0202u, &l->handle, 1);	/* [CR A-0127] */
		printf("time: link released (0x0202 on handle %u)%s\n",
		       (unsigned)l->handle,
		       l->ciss_n || l->lapc.outstanding
		       ? ", the FACILITY unacknowledged" : "");
	}
	time_next(s);
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
static void bc_clear(struct stubd *s, struct bcall *b);

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
		for (i = 0; i < BC_MAX; i++) {
			struct bcall *b = &s->bc[i];
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
	return i < s->base.n_hs && (i == s->base.cur || s->hs[i].loaded) &&
	       !s->hs[i].absent;
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
		if (s->hs[i].cap_known)
			dect_ctl_add_uint(&o, "part3", s->hs[i].cap_part3 ? 1 : 0);
		/* 0: we call it narrowband, so its account may answer G.711 */
		dect_ctl_add_uint(&o, "wideband", !s->audio.narrowband &&
				  (target_svc(s, (int)i, s->audio.basic_svc) &
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
 * The bridge (docs/bridge-interface.md §2, step 2 of §5): a client that
 * sent "bridge" carries the external calls, up to BC_MAX at once, on the
 * main link or on side links (struct bcall). Out: a handset's number goes
 * out as call-out, and remote-ringing, remote-answered and remote-released
 * drive ALERTING, CONNECT and the release. In: call-in rings the handset
 * hs= names (the first without it), on the main link or, while that is
 * busy, on a side link of its own; ringing, answered and released come
 * back. Without a bridge nothing changes: a dialled call is connected
 * locally (the lab's test sound or loop-back).
 */
static unsigned bridge_new_id(struct stubd *s)
{
	unsigned i;

	do {
		s->ctl.next_call = s->ctl.next_call % 127u + 1u;
		for (i = 0; i < BC_MAX; i++)
			if (s->bc[i].id == s->ctl.next_call)
				break;
	} while (i < BC_MAX);
	return s->ctl.next_call;
}

static unsigned main_hs_slot(struct stubd *s)
{
	int a = s->base.main_link.link.mm.adopted;

	return hs_slot(s, a >= 0 ? (unsigned)a : s->base.cur);
}

/* The handset a call's party serves, as its slot; 0 = nobody yet. */
static unsigned party_hs_slot(struct stubd *s, int p)
{
	if (p == -1)
		return main_hs_slot(s);
	if (p >= 0 && s->side[p].adopted >= 0)
		return hs_slot(s, (unsigned)s->side[p].adopted);
	return 0;
}

/* The handset on link p has a Line 2 (an account for it). */
static int party_has_line2(const struct stubd *s, int p)
{
	int a = p == -1 ? (s->base.main_link.link.mm.adopted >= 0 ?
			   s->base.main_link.link.mm.adopted : (int)s->base.cur)
			: p >= 0 ? s->side[p].adopted : -1;

	return a >= 0 && (unsigned)a < s->base.n_hs && s->hs[a].line2;
}

static struct bcall *bc_by_id(struct stubd *s, unsigned id)
{
	unsigned i;

	for (i = 0; id && i < BC_MAX; i++)
		if (s->bc[i].id == id)
			return &s->bc[i];
	return NULL;
}

/* The bridged call on link p (-1 the main link), or NULL. */
static struct bcall *bc_on(struct stubd *s, int p)
{
	unsigned i;

	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == p)
			return &s->bc[i];
	return NULL;
}

/* A free entry, zeroed; it is taken once its id is set. NULL: all taken. */
static struct bcall *bc_new(struct stubd *s)
{
	unsigned i;

	for (i = 0; i < BC_MAX; i++)
		if (!s->bc[i].id) {
			memset(&s->bc[i], 0, sizeof(s->bc[i]));
			return &s->bc[i];
		}
	return NULL;
}

/*
 * The call is over for the bridge. On the main link its ring stops too,
 * whatever state it was in: with ring_ms=0 ring_tick() runs only while
 * ring_now is set, so a ring left in PAGING would hold the main link
 * "busy" for good (code review, 2026-10-29).
 */
static void bc_clear(struct stubd *s, struct bcall *b)
{
	if (b->party == -1) {
		s->ring_now = 0;
		s->ring_watch = 0;
		if (s->ring_state != RING_IDLE)
			s->ring_state = RING_DONE;
	}
	b->id = 0;
}

static void bridge_event(struct stubd *s, const struct bcall *b,
			 const char *verb, const char *k2, const char *v2)
{
	struct dect_ctl_out o;
	unsigned hs = b->party == BC_PAGING ? hs_slot(s, (unsigned)b->target)
					    : party_hs_slot(s, b->party);

	dect_ctl_begin(&o, verb);
	dect_ctl_add_uint(&o, "call", b->id);
	if (hs)
		dect_ctl_add_uint(&o, "hs", hs);
	if (k2)
		dect_ctl_add(&o, k2, v2);
	ctl_send_bridge(s, &o);
}

/* The handset on link p dialled a number: hand it to the bridge. */
static int bridge_call_out(struct stubd *s, int p, uint64_t now)
{
	struct dect_cc *cc = ic_cc(s, p);
	struct dect_ctl_out o;
	struct bcall *b;
	const uint8_t *k;
	size_t len;
	char num[DECT_CTL_VAL_MAX];

	if (s->ctl.bridge < 0)
		return 0;
	k = dect_cc_keypad(cc, &len);
	if (len >= sizeof(num))
		len = sizeof(num) - 1;
	memcpy(num, k, len);
	num[len] = '\0';
	b = bc_new(s);
	if (!b) {
		(void)dect_cc_busy_system(cc);
		printf("bridge: %u calls already -- busy\n", BC_MAX);
		ic_drain(s, p, now);
		return 1;
	}
	if (dect_cc_proceed(cc))
		return 0;
	b->id = bridge_new_id(s);
	b->party = p;
	b->out = 1;
	b->since = now;
	/* the line the handset chose (§7.4.5.2), if it has it; else Line 1 */
	b->line = dect_cc_line(cc);
	if (b->line != STUBD_LINE_ID + 1u || !party_has_line2(s, p))
		b->line = STUBD_LINE_ID;
	dect_ctl_begin(&o, "call-out");
	dect_ctl_add_uint(&o, "call", b->id);
	dect_ctl_add_uint(&o, "hs", party_hs_slot(s, p));
	dect_ctl_add_uint(&o, "line", b->line);
	/* nb: the handset's call is narrowband, so the INVITE offers G.711 */
	dect_ctl_add(&o, "codec", dect_cc_codec(cc) == DECT_CC_CODEC_G722 ?
		     "wb" : "nb");
	dect_ctl_add(&o, "number", num);
	ctl_send_bridge(s, &o);
	printf("bridge: call %u out from DECT %u to \"%s\" -- waiting for the "
	       "far end\n", b->id, party_hs_slot(s, p), num);
	ic_drain(s, p, now);
	return 1;
}

/* remote-released's reason, as a CC release reason (§2.2 of the page) */
static uint8_t bridge_reason(const char *r)
{
	if (!r || !strcmp(r, "normal"))
		return DECT_CC_REASON_NONE;
	if (!strcmp(r, "busy"))
		return DECT_CC_REASON_USER_BUSY;
	if (!strcmp(r, "unreachable"))
		return DECT_CC_REASON_USER_UNKNOWN;
	if (!strcmp(r, "rejected"))
		return 0x15u;			/* user rejection */
	return DECT_CC_REASON_NONE;
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

/* The calls the bridge carries on link p. */
static unsigned bc_live(const struct stubd *s, int p)
{
	unsigned i, n = 0;

	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == p)
			n++;
	return n;
}

/* The call link p's lane carries -- in CS call connect -- or NULL. */
static struct bcall *bc_active(struct stubd *s, int p)
{
	unsigned i;

	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == p &&
		    s->bc[i].cs == DECT_CS_CALL_CONNECT)
			return &s->bc[i];
	return NULL;
}

/* Link p's call the handset knows by call id id, or NULL. */
static struct bcall *bc_by_dect(struct stubd *s, int p, uint8_t id)
{
	unsigned i;

	for (i = 0; id && i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == p &&
		    s->bc[i].dect_id == id)
			return &s->bc[i];
	return NULL;
}

static const char *cs_name(uint8_t cs)
{
	switch (cs) {
	case DECT_CS_CALL_SETUP:	return "waiting";
	case DECT_CS_CALL_SETUP_ACK:	return "being dialled";
	case DECT_CS_CALL_PROC:		return "dialled";
	case DECT_CS_CALL_ALERTING:	return "ringing at the far end";
	case DECT_CS_CALL_CONNECT:	return "connected";
	case DECT_CS_CALL_HOLD:		return "on hold";
	default:			return "released";
	}
}

/* Tell the handset call b's new status (§7.4.6.4) in {CC-INFO}. */
static void par_status(struct stubd *s, struct bcall *b, uint8_t cs,
		       uint64_t now)
{
	struct dect_cc *cc = ic_cc(s, b->party);

	b->cs = cs;
	if (!b->dect_id)
		b->dect_id = cc->status_call_id;
	if (dect_cc_call_status(cc, b->dect_id, cs, DECT_CC_NO_REASON,
				DECT_CC_NO_SIGNAL) == 0)
		ic_drain(s, b->party, now);
	printf("bridge: call %u %s on DECT %u (call id %u)\n", b->id,
	       cs_name(cs), party_hs_slot(s, b->party), b->dect_id);
}

/* §7.4.3.4: a request on call id id failed -- its status again, a reason. */
static void par_nack(struct stubd *s, int p, uint8_t id, uint8_t reason,
		     uint64_t now)
{
	const struct bcall *b = bc_by_dect(s, p, id);

	if (dect_cc_call_status(ic_cc(s, p), id, b ? b->cs : DECT_CS_IDLE,
				reason, DECT_SIGNAL_NEGATIVE_ACK_TONE) == 0)
		ic_drain(s, p, now);
	printf("bridge: DECT %u's request on call id %u refused\n",
	       party_hs_slot(s, p), id);
}

/* The active call on link p goes on hold, at the far end too. */
static void par_hold(struct stubd *s, int p, uint64_t now)
{
	struct bcall *a = bc_active(s, p);

	if (!a)
		return;
	par_status(s, a, DECT_CS_CALL_HOLD, now);
	bridge_event(s, a, "hold", NULL, NULL);
}

/* Call b becomes the active one; verb, if any, tells the bridge. */
static void par_connect(struct stubd *s, struct bcall *b, const char *verb,
			uint64_t now)
{
	par_status(s, b, DECT_CS_CALL_CONNECT, now);
	b->dtmf = 1;
	if (verb)
		bridge_event(s, b, verb, NULL, NULL);
}

/* The handset table index link p serves, or -1. */
static int party_hs_index(const struct stubd *s, int p)
{
	int a;

	if (p == -1) {
		a = s->base.main_link.link.mm.adopted;
		return a >= 0 ? a : (int)s->base.cur;
	}
	return p >= 0 ? s->side[p].adopted : -1;
}

/*
 * The link a call to handset t can wait on: its one call bridged and
 * connected, to a handset that takes call status (Part 3). BC_NONE if
 * there is none.
 */
static int waiting_party(struct stubd *s, unsigned t)
{
	int p;

	for (p = -1; p < (int)SIDE_MAX; p++) {
		const struct dect_cc *cc;

		if (!ic_active(s, p) || party_hs_index(s, p) != (int)t)
			continue;
		cc = ic_cc(s, p);
		if (cc->state == DECT_CC_ST_ACTIVE && cc->call_status &&
		    bc_active(s, p) && bc_live(s, p) == 1)
			return p;
	}
	return BC_NONE;
}

/* Call waiting (§7.4.3.5.2) for call-in b on link p. 0, or -1. */
static int cw_start(struct stubd *s, struct bcall *b, int p,
		    const struct dect_clip *clip, uint64_t now)
{
	struct dect_callinfo ci;
	uint8_t id = alloc_call_id(s, NULL);

	if (!id)
		return -1;
	dect_callinfo_init(&ci);
	ci.has_line = 1;
	ci.line_id = (uint8_t)b->line;
	ci.has_line_type = 1;
	ci.line_type = DECT_CALLINFO_NDT_DELAY;
	ci.has_call_id = 1;
	ci.call_id = id;
	if (dect_cc_call_waiting(ic_cc(s, p), &ci, clip))
		return -1;
	b->party = p;
	b->par = 1;
	b->dect_id = id;
	b->cs = DECT_CS_CALL_SETUP;
	b->busy_seen = 1;
	b->rang = 1;
	ic_drain(s, p, now);
	bridge_event(s, b, "ringing", NULL, NULL);
	return 0;
}

/* A parallel call's number is complete: to the bridge. */
static void par_dial_out(struct stubd *s, struct bcall *d, uint64_t now)
{
	struct dect_ctl_out o;

	d->dialing = 0;
	if (s->ctl.bridge < 0) {
		par_status(s, d, DECT_CS_IDLE, now);
		bc_clear(s, d);
		return;
	}
	dect_ctl_begin(&o, "call-out");
	dect_ctl_add_uint(&o, "call", d->id);
	dect_ctl_add_uint(&o, "hs", party_hs_slot(s, d->party));
	dect_ctl_add_uint(&o, "line", d->line);
	/* the link's codec: a parallel call cannot change it */
	dect_ctl_add(&o, "codec", dect_cc_codec(ic_cc(s, d->party)) ==
		     DECT_CC_CODEC_G722 ? "wb" : "nb");
	dect_ctl_add(&o, "number", d->num);
	ctl_send_bridge(s, &o);
	printf("bridge: call %u out from DECT %u to \"%s\" (a parallel "
	       "call)\n", d->id, party_hs_slot(s, d->party), d->num);
	par_status(s, d, DECT_CS_CALL_PROC, now);
}

/* A control code from the handset on link p (table 17). */
static void par_control(struct stubd *s, int p, uint8_t code, uint8_t id,
			uint64_t now)
{
	struct bcall *b = bc_by_dect(s, p, id), *a = bc_active(s, p);

	switch (code) {
	case 0x35u:			/* accept the waiting call ... */
	case 0x38u:			/* ... releasing the active one */
		if (!b || !b->par || b->cs != DECT_CS_CALL_SETUP || b->out)
			break;
		if (a && code == 0x38u) {
			bridge_event(s, a, "released", "by", "hs");
			par_status(s, a, DECT_CS_IDLE, now);
			bc_clear(s, a);
		} else {
			par_hold(s, p, now);
		}
		b->answered = 1;
		par_connect(s, b, "answered", now);
		return;
	case 0x36u:			/* reject the waiting call */
		if (!b || !b->par || b->cs != DECT_CS_CALL_SETUP || b->out)
			break;
		bridge_event(s, b, "released", "by", "hs");
		par_status(s, b, DECT_CS_IDLE, now);
		bc_clear(s, b);
		return;
	case 0x31u:			/* toggle to b */
	case 0x42u:			/* resume b */
		if (!b || b->cs != DECT_CS_CALL_HOLD)
			break;
		par_hold(s, p, now);
		par_connect(s, b, "resume", now);
		return;
	case 0x41u:			/* hold b, the active call */
		if (!b || b != a)
			break;
		par_hold(s, p, now);
		return;
	case 0x33u:			/* release b */
		if (!b || b->cs == DECT_CS_CALL_SETUP)
			break;		/* a waiting call is rejected */
		bridge_event(s, b, "released", "by", "hs");
		par_status(s, b, DECT_CS_IDLE, now);
		bc_clear(s, b);
		return;
	case 0x15u:			/* a parallel call out, its number next */
		/* a call id of its own, none while -a callid= pins one */
		b = bc_live(s, p) < 2u && alloc_call_id(s, NULL) ?
		    bc_new(s) : NULL;
		if (!b) {
			uint8_t nid = alloc_call_id(s, NULL);
			struct dect_cc *cc = ic_cc(s, p);

			if (nid) {
				/* §7.4.8.3: busy, then the call id freed */
				(void)dect_cc_call_status(cc, nid,
					DECT_CS_CALL_DISCONNECTING,
					DECT_CSR_SYSTEM_BUSY,
					DECT_SIGNAL_BUSY_TONE);
				(void)dect_cc_call_status(cc, nid,
					DECT_CS_IDLE, DECT_CC_NO_REASON,
					DECT_CC_NO_SIGNAL);
				ic_drain(s, p, now);
			} else {
				par_nack(s, p, a ? a->dect_id :
					 cc->status_call_id,
					 DECT_CSR_SYSTEM_BUSY, now);
			}
			dect_cc_clear_keypad(cc);	/* its number */
			return;
		}
		par_hold(s, p, now);
		b->id = bridge_new_id(s);
		b->party = p;
		b->par = 1;
		b->out = 1;
		b->busy_seen = 1;
		b->since = now;
		b->line = STUBD_LINE_ID;
		b->dect_id = alloc_call_id(s, NULL);
		b->dialing = 1;
		b->dial_ms = now;
		par_status(s, b, DECT_CS_CALL_SETUP_ACK, now);
		return;
	default:			/* internal, conference, transfer */
		par_nack(s, p, id ? id : a ? a->dect_id :
			 ic_cc(s, p)->status_call_id,
			 DECT_CSR_CODE_NOT_SUPPORTED, now);
		if (code == 0x17u)		/* the internal number */
			dect_cc_clear_keypad(ic_cc(s, p));
		return;
	}
	par_nack(s, p, id, DECT_CSR_CODE_FAILED, now);
}

/*
 * Link p's control codes and keypad: the number of a parallel call being
 * dialled (complete after the dial pause), else DTMF for the active call.
 * The keypad of a call still being set up is its number: left alone.
 */
static void par_tick(struct stubd *s, int p, uint64_t now)
{
	struct dect_cc *cc = ic_cc(s, p);
	struct bcall *d = NULL, *a;
	const uint8_t *k;
	uint8_t code, id;
	size_t len, i, n;

	if (dect_cc_take_control(cc, &code, &id))
		par_control(s, p, code, id, now);
	for (i = 0; i < BC_MAX; i++)
		if (s->bc[i].id && s->bc[i].party == p && s->bc[i].dialing)
			d = &s->bc[i];
	k = dect_cc_keypad(cc, &len);
	if (d) {
		n = strlen(d->num);
		for (i = 0; i < len && n + 1u < sizeof(d->num); i++)
			if ((k[i] >= '0' && k[i] <= '9') || k[i] == '*' ||
			    k[i] == '#')
				d->num[n++] = (char)k[i];
		d->num[n] = '\0';
		if (len) {
			d->dial_ms = now;
			dect_cc_clear_keypad(cc);
		} else if (n && now - d->dial_ms >= s->audio.dial_wait_ms) {
			par_dial_out(s, d, now);
		}
		return;
	}
	a = bc_active(s, p);
	if (!len)
		return;
	if (!a) {
		/*
		 * A call with a status here is past its own setup: every call
		 * is held or waiting, and the digits are for none of them.
		 */
		for (i = 0; i < BC_MAX; i++)
			if (s->bc[i].id && s->bc[i].party == p && s->bc[i].cs) {
				dect_cc_clear_keypad(cc);
				break;
			}
		return;
	}
	if (!a->dtmf)
		return;
	for (i = 0; i < len; i++) {
		char dig[2] = { (char)k[i], '\0' };

		if ((k[i] >= '0' && k[i] <= '9') || k[i] == '*' ||
		    k[i] == '#')
			bridge_event(s, a, "digit", "d", dig);
	}
	dect_cc_clear_keypad(cc);
}

/* Handset t is in a call, or being rung for one. */
static int hs_in_call(const struct stubd *s, unsigned t)
{
	unsigned k;
	int a = s->base.main_link.link.mm.adopted;

	if (s->base.main_link.active && dect_cc_busy(&s->base.main_link.link.cc) &&
	    (a >= 0 ? (unsigned)a : s->base.cur) == t)
		return 1;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active && s->side[k].adopted == (int)t &&
		    dect_cc_busy(&s->side[k].bl.link.cc))
			return 1;
	for (k = 0; k < BC_MAX; k++)
		if (s->bc[k].id && !s->bc[k].out &&
		    ((s->bc[k].party == -1 && s->ring_hs == t) ||
		     (s->bc[k].party == BC_PAGING && s->bc[k].target == (int)t)))
			return 1;
	return 0;
}

/* The main link can ring: no call, no ring of its own. */
static int main_free(struct stubd *s)
{
	return !bc_on(s, -1) &&
	       !(s->base.main_link.active && dect_cc_busy(&s->base.main_link.link.cc)) &&
	       (s->ring_state == RING_IDLE || s->ring_state == RING_DONE);
}

/* Ring handset t on the main link for b; ring_tick() pages it. */
static void ring_main(struct stubd *s, struct bcall *b, unsigned t)
{
	b->party = -1;
	s->ring_hs = t;
	s->ring_state = RING_IDLE;
	s->ring_now = 1;
	s->ring_requeues = 0;
	s->ring_setup_tries = 0;
	s->ring_pages = 0;
	s->ring_wait_since = 0;
}

/* -------------------------------------------------------------------------
 * A side ring: a call-in for a handset while the main link is in another
 * call or ringing. The handset is paged as an internal call's target is
 * (ic_page()) and rung on the side link it comes up on, with what
 * ring_queue_setup() gives the main link's: its profile's alerting pattern
 * and name method, and to a Part 3 handset the line, call id and CS call
 * setup [CR A-0125]. Each call pages its own handset.
 */
static void sr_page(struct stubd *s, struct bcall *b)
{
	const uint8_t p[3] = { ring_page_info(s, b->narrow), b->tpui[1],
			       b->tpui[2] };

	chip_mail(s, 0x0300u, p, sizeof(p));
	b->pages++;
	printf("bridge: paged DECT %u for call %u, attempt %u of %u\n",
	       hs_slot(s, (unsigned)b->target), b->id, b->pages, IC_PAGES);
}

static void sr_start(struct stubd *s, struct bcall *b, unsigned t,
		     uint64_t now)
{
	struct dect_mm_cfg cfg;
	struct dect_mm mm;

	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = (uint8_t)hs_slot(s, t);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, b->tpui);
	b->party = BC_PAGING;
	b->target = (int)t;
	b->pages = 0;
	b->page_at = now;
}

static int sr_part3(const struct stubd *s, unsigned t)
{
	const struct dect_callinfo *o = &s->audio.callinfo;

	return !o->has_line && !o->has_call_id && !o->has_call_status &&
	       !s->audio.no_call_status && s->hs[t].cap_known &&
	       s->hs[t].cap_part3;
}

/* Side ring b's SETUP to its handset, on side link k. */
static void sr_setup(struct stubd *s, struct bcall *b, unsigned k,
		     uint64_t now, const char *how)
{
	struct dect_link *l = &s->side[k].bl.link;
	unsigned t = (unsigned)b->target;
	const struct dect_hs_profile *hp = &s->hs[t].prof;
	struct dect_callinfo prev = l->cc.callinfo, ci;
	struct dect_clip clip = b->clip;
	const uint8_t *port, *fixed;
	size_t plen, flen;
	uint8_t id;
	int r;

	hs_cc_ids(s, t, &port, &plen, &fixed, &flen);
	if (!plen || dect_cc_busy(&l->cc))
		return;
	dect_cc_set_next_ti(&l->cc, 0);
	id = alloc_call_id(s, l);
	if (id)
		dect_cc_set_call_status(&l->cc, l->cc.call_status, id);
	dect_callinfo_init(&ci);
	if (sr_part3(s, t)) {
		ci.has_line = 1;
		ci.line_id = (uint8_t)b->line;
		ci.has_line_type = 1;
		ci.line_type = DECT_CALLINFO_NDT_DELAY;
		ci.has_call_id = 1;
		ci.call_id = l->cc.status_call_id;
		ci.has_call_status = 1;
		ci.call_status = DECT_CS_CALL_SETUP;
	}
	dect_cc_set_callinfo(&l->cc, &ci);
	if (!s->audio.signal_set &&
	    hp->cc_signal_external != DECT_SIGNAL_ALERT_PATTERN1)
		dect_cc_set_signal(&l->cc, 1, hp->cc_signal_external);
	if (hp->cc_name_method == 1 || hp->cc_name_method == 3)
		clip.name[0] = '\0';		/* display text or none */
	r = dect_cc_setup_id(&l->cc, port, plen, fixed, flen,
			     ring_svc(s, t, b->narrow), &clip);
	dect_cc_set_callinfo(&l->cc, &prev);
	dect_cc_set_signal(&l->cc, s->audio.signal_on, s->audio.signal_value);
	if (r) {
		printf("bridge: could not queue the SETUP to DECT %u -- "
		       "trying again\n", hs_slot(s, t));
		return;
	}
	b->party = (int)k;
	printf("bridge: call %u: DECT %u %s on side link %u -- SETUP sent%s\n",
	       b->id, hs_slot(s, t), how, s->side[k].bl.handle,
	       ci.has_call_status ? " (full call information, Part 3)" : "");
	ic_drain(s, (int)k, now);
}

/* A page response on side link sl: the handset a side ring pages? */
static int sr_target_link(struct stubd *s, struct side_link *sl,
			  const struct dect_link_ind *ind, uint64_t now)
{
	unsigned i;

	for (i = 0; i < BC_MAX; i++) {
		struct bcall *b = &s->bc[i];

		if (!b->id || b->party != BC_PAGING ||
		    !page_adopt(s, sl, ind, b->target, " (external call)"))
			continue;
		sr_setup(s, b, (unsigned)(sl - s->side), now,
			 "answered the page");
		return 1;
	}
	return 0;
}

static void sr_tick(struct stubd *s, struct bcall *b, uint64_t now)
{
	unsigned k;

	/* the main link's call ended meanwhile: ring there, as usual */
	if (main_free(s)) {
		printf("bridge: call %u: the main link is free -- ringing DECT "
		       "%u there\n", b->id, hs_slot(s, (unsigned)b->target));
		s->clip = b->clip;
		ring_main(s, b, (unsigned)b->target);
		return;
	}
	for (k = 0; k < SIDE_MAX && b->party == BC_PAGING; k++)
		if (s->side[k].bl.active && s->side[k].adopted == b->target &&
		    !dect_cc_busy(&s->side[k].bl.link.cc))
			sr_setup(s, b, k, now, "came up (adopted, no page "
				 "response)");
	if (b->party != BC_PAGING || now < b->page_at)
		return;
	if (b->pages >= IC_PAGES) {
		printf("bridge: call %u in -- DECT %u did not answer the "
		       "page\n", b->id, hs_slot(s, (unsigned)b->target));
		bridge_event(s, b, "released", "by", "base");
		bc_clear(s, b);
		return;
	}
	sr_page(s, b);
	b->page_at = now + IC_PAGE_MS;
}

/*
 * A side ring's call on its link: the profile's display text once it rings
 * (info_tick() for the main link), CS call connect to a Part 3 handset
 * once it answered (cs_connect_tick()).
 */
static void sr_call_tick(struct stubd *s, struct bcall *b, uint64_t now)
{
	struct dect_cc *cc = ic_cc(s, b->party);
	unsigned t = (unsigned)b->target;

	if (!b->info_sent && cc->state == DECT_CC_ST_RINGING) {
		b->info_sent = 1;
		if (name_display_info(s, cc, t, &b->clip) > 0)
			ic_drain(s, b->party, now);
	}
	if (!b->cs_sent && cc->state == DECT_CC_ST_ACTIVE && sr_part3(s, t)) {
		struct dect_callinfo ci;
		uint8_t ie[16];
		size_t n;

		b->cs_sent = 1;
		dect_callinfo_init(&ci);
		ci.has_call_id = 1;
		ci.call_id = cc->status_call_id;
		ci.has_call_status = 1;
		ci.call_status = DECT_CS_CALL_CONNECT;
		n = dect_callinfo_put(ie, sizeof(ie), 0, &ci);
		if (n && !dect_cc_info(cc, ie, n))
			ic_drain(s, b->party, now);
	}
}

/* A bridge command about a call. Returns 1 when the verb was one of ours. */
static int bridge_command(struct stubd *s, const struct dect_ctl_msg *m,
			  uint64_t now)
{
	struct dect_ctl_out o;
	struct bcall *b;
	unsigned id;
	int p;

	if (!strcmp(m->verb, "bridge")) {
		if (s->ctl.bridge >= 0 && s->ctl.bridge != s->ctl.cur) {
			ctl_reply(s, m->verb, "busy", NULL, 0);
			return 1;
		}
		s->ctl.bridge = s->ctl.cur;
		printf("bridge: client %d carries external calls\n",
		       s->ctl.cur + 1);
		ctl_reply(s, m->verb, NULL, NULL, 0);
		return 1;
	}
	if (!strcmp(m->verb, "mwi")) {
		unsigned want, line, count, t;
		uint8_t bit;

		if (s->ctl.cur != s->ctl.bridge) {
			ctl_reply(s, m->verb, "not-bridge", NULL, 0);
			return 1;
		}
		if (dect_ctl_get_uint(m, "hs", 1, DECT_MM_MAX_SLOT, &want) ||
		    dect_ctl_get_uint(m, "line", 1, 2, &line) ||
		    dect_ctl_get_uint(m, "count", 0, 16383, &count)) {
			ctl_reply(s, m->verb, "malformed", NULL, 0);
			return 1;
		}
		for (t = 0; t < s->base.n_hs; t++)
			if (ctl_listed(s, t) && hs_slot(s, t) == want)
				break;
		if (t == s->base.n_hs) {
			ctl_reply(s, m->verb, "no-handset", "hs", want);
			return 1;
		}
		bit = (uint8_t)(1u << (line - 1u));
		if (!(s->hs[t].mwi_known & bit) || s->hs[t].mwi[line - 1u] != count) {
			struct dect_link *l = link_known_hs(s, (int)t);

			s->hs[t].mwi[line - 1u] = count;
			s->hs[t].mwi_known |= bit;
			s->hs[t].mwi_due |= bit;
			if (l) {
				(void)mwi_send(s, l, t);
				time_pump(s, l, now);
			} else {
				s->tu.want |= 1u << t;	/* time_tick() pages it */
			}
		}
		ctl_reply(s, m->verb, NULL, "hs", want);
		return 1;
	}
	/*
	 * calllog: a finished call for the call lists (the bridge knows a
	 * call's outcome over all the handsets it rang): kind=missed |
	 * accepted | outgoing, number, name, legs=HS:LINE,... the handsets
	 * whose line it was and which of their lines.
	 */
	if (!strcmp(m->verb, "calllog")) {
		const char *kind = dect_ctl_get(m, "kind");
		const char *num = dect_ctl_get(m, "number");
		const char *name = dect_ctl_get(m, "name");
		const char *legs = dect_ctl_get(m, "legs");
		uint8_t line[DECT_CALLS_TERMINALS];
		char book_name[2u * DECT_CONTACT_NAME_MAX];
		struct dect_time when;
		unsigned list, i;

		if (s->ctl.cur != s->ctl.bridge) {
			ctl_reply(s, m->verb, "not-bridge", NULL, 0);
			return 1;
		}
		list = !kind ? 0u : !strcmp(kind, "missed") ? DECT_CALLS_MISSED :
		       !strcmp(kind, "outgoing") ? DECT_CALLS_OUTGOING :
		       !strcmp(kind, "accepted") ? DECT_CALLS_ACCEPTED : 0u;
		memset(line, 0, sizeof(line));
		while (legs && *legs) {
			unsigned hs = 0, ln = 0;

			if (sscanf(legs, "%u:%u", &hs, &ln) == 2 &&
			    hs >= 1u && hs < DECT_CALLS_TERMINALS &&
			    ln >= 1u && ln <= 2u)
				line[hs] = (uint8_t)ln;
			legs = strchr(legs, ',');
			legs = legs ? legs + 1 : NULL;
		}
		if (!list) {
			ctl_reply(s, m->verb, "malformed", NULL, 0);
			return 1;
		}
		/*
		 * The Contact List's name before the network's, as
		 * TS 102 527-3 7.4.10.5.1.3 note 1 recommends.
		 */
		if (num && *num && s->contacts_path) {
			const struct dect_contact *e =
				dect_contacts_find_number(&stubd_contacts, num);

			if (e) {
				snprintf(book_name, sizeof(book_name), "%s%s%s",
					 e->first, e->first[0] && e->name[0]
					 ? " " : "", e->name);
				name = book_name;
			}
		}
		clock_any(&when);
		(void)dect_calls_add(&stubd_calls, list, num, name, &when, line);
		printf("calls: %s call %s%s\n", kind,
		       list == DECT_CALLS_OUTGOING ? "to " : "from ",
		       num && *num ? num : "(no number)");
		if (list == DECT_CALLS_MISSED)
			for (i = 0; i < s->base.n_hs; i++) {
				unsigned tid = hs_slot(s, i);

				if (tid < DECT_CALLS_TERMINALS && line[tid])
					missed_due(s, i, (uint8_t)(1u << (line[tid] - 1u)),
						   1);
			}
		ctl_reply(s, m->verb, NULL, NULL, 0);
		return 1;
	}
	if (!strcmp(m->verb, "call-in")) {
		const char *num = dect_ctl_get(m, "number");
		const char *name = dect_ctl_get(m, "name");
		char book_name[2u * DECT_CONTACT_NAME_MAX];
		int book = 0;
		const char *sip = dect_ctl_get(m, "sip");
		const char *why = NULL, *codec;
		struct dect_clip clip;
		int wp;
		unsigned want = 0, t = 0, line = STUBD_LINE_ID;

		/* hs=N: the handset whose line was called (its own account) */
		if (dect_ctl_get(m, "hs")) {
			if (dect_ctl_get_uint(m, "hs", 1, DECT_MM_MAX_SLOT, &want)) {
				ctl_reply(s, m->verb, "bad-hs", NULL, 0);
				return 1;
			}
			for (t = 0; t < s->base.n_hs; t++)
				if (ctl_listed(s, t) && hs_slot(s, t) == want)
					break;
			if (t == s->base.n_hs) {
				ctl_reply(s, m->verb, "no-handset", "hs", want);
				return 1;
			}
		}
		if (s->ctl.cur != s->ctl.bridge) {
			ctl_reply(s, m->verb, "not-bridge", NULL, 0);
			return 1;
		}
		/* line=N: which of the handset's lines was called */
		if (dect_ctl_get(m, "line") &&
		    dect_ctl_get_uint(m, "line", 1, 2, &line))
			line = STUBD_LINE_ID;
		/* codec=nb: the caller offered no G.722 */
		codec = dect_ctl_get(m, "codec");
		b = bc_new(s);
		/* a Part 3 handset in a bridged call: call waiting */
		wp = BC_NONE;
		if (s->base.n_hs && hs_in_call(s, t)) {
			wp = b ? waiting_party(s, t) : BC_NONE;
			if (wp == BC_NONE)
				why = "in-call";
		} else if (!b || (!main_free(s) && (!s->audio.two_calls ||
			   lanes_available(s, NULL) <= 0))) {
			why = "channels";
		}
		if (why) {
			dect_ctl_begin(&o, "busy");
			if (sip)
				dect_ctl_add(&o, "sip", sip);
			dect_ctl_add(&o, "why", why);
			ctl_send_bridge(s, &o);
			return 1;
		}
		/*
		 * A SIP From is whatever the caller wrote: user provided, not
		 * screened -- "network provided" told the handset the
		 * network vouched for it (security audit 2026-10-29, S1).
		 */
		dect_clip_init(&clip);
		if (num && *num)
			(void)dect_clip_set_number(&clip, num,
					DECT_CLIP_TON_UNKNOWN,
					DECT_CLIP_NPI_UNKNOWN,
					DECT_CLIP_PRES_ALLOWED,
					DECT_CLIP_SCR_USER_UNSCREENED);
		/*
		 * No name from SIP: the Contact List's, as TS 102 527-3
		 * 7.4.10.5.1.3 note 1 recommends ("First Name"), screened
		 * "user-provided, verified and passed" as EN 300 175-5
		 * 7.7.53 note 1 asks for a name from the FP's contact list.
		 */
		if ((!name || !*name) && num && *num && s->contacts_path) {
			const struct dect_contact *e =
				dect_contacts_find_number(&stubd_contacts, num);

			if (e) {
				snprintf(book_name, sizeof(book_name), "%s%s%s",
					 e->first, e->first[0] && e->name[0]
					 ? " " : "", e->name);
				name = book_name;
				book = 1;
			}
		}
		if (name && *name)
			(void)dect_clip_set_name(&clip, name,
					DECT_CLIP_ALPHABET_UTF8,
					DECT_CLIP_PRES_ALLOWED,
					book ? DECT_CLIP_SCR_USER_PASSED
					     : DECT_CLIP_SCR_USER_UNSCREENED);
		b->id = bridge_new_id(s);
		b->line = line;
		b->narrow = codec && !strcmp(codec, "nb");
		b->since = now;
		b->target = (int)t;
		b->clip = clip;
		if (wp != BC_NONE) {
			if (cw_start(s, b, wp, &clip, now)) {
				b->id = 0;
				dect_ctl_begin(&o, "busy");
				if (sip)
					dect_ctl_add(&o, "sip", sip);
				dect_ctl_add(&o, "why", "in-call");
				ctl_send_bridge(s, &o);
				return 1;
			}
		} else if (main_free(s)) {
			s->clip = clip;
			ring_main(s, b, t);
		} else {
			sr_start(s, b, t, now);
		}
		printf("bridge: call %u in from \"%s\" -- %s DECT %u%s\n",
		       b->id, num ? num : "", b->par ? "waiting on" : "ringing",
		       hs_slot(s, t), b->party == BC_PAGING ? " on a side link "
		       "(the main link is in a call)" : "");
		dect_ctl_begin(&o, "call-in-ok");
		dect_ctl_add_uint(&o, "call", b->id);
		if (sip)
			dect_ctl_add(&o, "sip", sip);
		ctl_send_bridge(s, &o);
		return 1;
	}
	if (strcmp(m->verb, "remote-ringing") &&
	    strcmp(m->verb, "remote-answered") &&
	    strcmp(m->verb, "remote-released"))
		return 0;
	if (s->ctl.cur != s->ctl.bridge) {
		ctl_reply(s, m->verb, "not-bridge", NULL, 0);
		return 1;
	}
	if (dect_ctl_get_uint(m, "call", 1, 127, &id) ||
	    !(b = bc_by_id(s, id))) {
		ctl_reply(s, m->verb, "no-call", NULL, 0);
		return 1;
	}
	p = b->party;
	if (!strcmp(m->verb, "remote-released")) {
		const char *r = dect_ctl_get(m, "reason");

		printf("bridge: call %u released by the far end (%s)\n", id,
		       r ? r : "normal");
		if (p != BC_PAGING && bc_live(s, p) > 1u) {
			/* the link carries another call: this one goes alone */
			par_status(s, b, DECT_CS_IDLE, now);
			bc_clear(s, b);
			ctl_reply(s, m->verb, NULL, "call", id);
			return 1;
		}
		bc_clear(s, b);			/* no "released" back */
		if (p == BC_PAGING) {
			ctl_reply(s, m->verb, NULL, "call", id);
			return 1;		/* paging stops */
		}
		if (ic_active(s, p) && dect_cc_busy(ic_cc(s, p)))
			(void)dect_cc_release_reason(ic_cc(s, p),
						     bridge_reason(r));
	} else if (!b->out || !ic_active(s, p)) {
		ctl_reply(s, m->verb, "no-call", NULL, 0);
		return 1;
	} else if (b->par) {
		/* a parallel call out: its progress as call status */
		if (!strcmp(m->verb, "remote-ringing")) {
			if (b->cs == DECT_CS_CALL_PROC)
				par_status(s, b, DECT_CS_CALL_ALERTING, now);
		} else if (!bc_active(s, p)) {
			par_connect(s, b, NULL, now);
		} else {
			par_status(s, b, DECT_CS_CALL_HOLD, now);
			bridge_event(s, b, "hold", NULL, NULL);
		}
	} else if (!strcmp(m->verb, "remote-ringing")) {
		if (ic_cc(s, p)->state == DECT_CC_ST_PROCEEDING)
			(void)dect_cc_alert(ic_cc(s, p));
	} else {
		const char *codec = dect_ctl_get(m, "codec");

		/*
		 * codec=nb: the far end speaks narrowband, so the handset
		 * does too (G.726, if it offered it) rather than the box
		 * converting its G.722.
		 */
		if (codec && !strcmp(codec, "nb") &&
		    dect_cc_prefer_narrowband(ic_cc(s, p)))
			b->narrow = 1;
		printf("bridge: call %u answered by the far end%s\n", id,
		       b->narrow ? " -- narrowband, G.726 to the handset" : "");
		(void)dect_cc_connect(ic_cc(s, p));
	}
	if (ic_active(s, p))
		ic_drain(s, p, now);
	ctl_reply(s, m->verb, NULL, "call", id);
	return 1;
}

/* The bridged calls' progress, as events. */
static void bridge_tick(struct stubd *s, uint64_t now)
{
	unsigned i;
	int p;

	/*
	 * Control codes and keypad, once per link with a call of ours. A
	 * link without one is not in parallel mode: the library refuses its
	 * codes (§7.4.3.4), and none is left over for a later call.
	 */
	for (p = -1; p < (int)SIDE_MAX; p++) {
		int ours;

		if (!ic_active(s, p))
			continue;
		ours = bc_live(s, p) != 0;
		dect_cc_set_parallel(ic_cc(s, p), ours);
		if (ours && dect_cc_busy(ic_cc(s, p)))
			par_tick(s, p, now);
	}
	for (i = 0; i < BC_MAX; i++) {
		struct bcall *b = &s->bc[i];
		struct dect_cc *cc;
		int busy;

		if (b->id && b->party == BC_PAGING)
			sr_tick(s, b, now);
		if (!b->id)
			continue;
		busy = b->party != BC_PAGING && ic_active(s, b->party) &&
		       dect_cc_busy(ic_cc(s, b->party));
		cc = busy ? ic_cc(s, b->party) : NULL;
		/*
		 * A main-link call-in exists once the handset alerts: a SETUP
		 * lost to the handset's own call before that is rung again
		 * (ring_watch_tick()), not ended.
		 */
		if (cc && (b->out || b->party != -1 ||
			   (cc->incoming && (cc->state == DECT_CC_ST_RINGING ||
					     cc->state == DECT_CC_ST_ACTIVE))))
			b->busy_seen = 1;
		/*
		 * The link's own call connected: it is the active one, and
		 * what the keypad brings from now on is its DTMF (the number
		 * it was dialled with is dropped).
		 */
		if (cc && !b->par && !b->cs && cc->state == DECT_CC_ST_ACTIVE &&
		    call_auth_ok(s, ic_link(s, b->party))) {
			b->cs = DECT_CS_CALL_CONNECT;
			b->dect_id = cc->status_call_id;
			b->dtmf = 1;
			dect_cc_clear_keypad(cc);
		}
		if (cc && !b->par && !b->out && cc->incoming) {
			/*
			 * The main link's ring_now stays set until bc_clear():
			 * ring_tick() then runs ring_watch_tick(), which rings
			 * again after a collision with the handset's own call.
			 */
			if (b->party >= 0)
				sr_call_tick(s, b, now);
			if (!b->rang && (cc->state == DECT_CC_ST_RINGING ||
					 cc->state == DECT_CC_ST_ACTIVE)) {
				b->rang = 1;
				bridge_event(s, b, "ringing", NULL, NULL);
			}
			if (!b->answered && cc->state == DECT_CC_ST_ACTIVE &&
			    call_auth_ok(s, ic_link(s, b->party))) {
				b->answered = 1;
				printf("bridge: call %u answered on DECT %u\n",
				       b->id, party_hs_slot(s, b->party));
				bridge_event(s, b, "answered", NULL, NULL);
			}
		}
		if (b->busy_seen && !busy) {
			printf("bridge: call %u ended on the handset\n", b->id);
			bridge_event(s, b, "released", "by", "hs");
			bc_clear(s, b);
			continue;
		}
		/* a call-in that never got a handset: the ring gave up */
		if (!b->out && !b->busy_seen &&
		    (now - b->since > 75000u ||
		     (b->party == -1 && s->ring_state == RING_DONE &&
		      !s->ring_watch && now - b->since > 5000u))) {
			printf("bridge: call %u in -- no handset answered the "
			       "page\n", b->id);
			bridge_event(s, b, "released", "by", "base");
			bc_clear(s, b);
		}
	}
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

static void ab_open(struct stubd *s)
{
	struct sockaddr_un a;
	mode_t old;
	int fd;

	ab_init(s);
	if (s->ctl.lfd < 0)
		return;
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
static const struct bcall *ab_call_of(struct stubd *s, unsigned i, int *wb)
{
	const struct xlane *x;
	unsigned k;

	if (i == 0) {
		*wb = s->reg.call_wideband;
		return s->base.main_link.active && s->reg.call_bound ? bc_active(s, -1)
							  : NULL;
	}
	x = &s->xl[i - 1u];
	*wb = x->wideband;
	if (!x->used || !x->connected)
		return NULL;
	for (k = 0; k < SIDE_MAX; k++)
		if (s->side[k].bl.active && s->side[k].lane == (int)(i - 1u))
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
		buf[n] = '\0';
		if (sscanf(buf, "call=%u rate=%u", &id, &rate) < 1)
			id = 0;
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
	for (i = 0; i < AB_LANES; i++) {
		struct ab_lane *l = &s->ab.l[i];
		const struct bcall *b;
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

/*
 * A handset name from the socket: at most DECT_IWU_NAME_MAX - 1 octets of
 * well-formed UTF-8 (it goes on the air in the Internal Names List); the
 * line format has already refused control characters. Empty resets it.
 */
static int ctl_name_ok(const char *nm)
{
	const unsigned char *p = (const unsigned char *)nm;
	size_t n = strlen(nm);

	if (n >= DECT_IWU_NAME_MAX)
		return 0;
	while (*p) {
		unsigned k = *p < 0x80u ? 0 : (*p & 0xE0u) == 0xC0u ? 1
			     : (*p & 0xF0u) == 0xE0u ? 2
			     : (*p & 0xF8u) == 0xF0u ? 3 : 4;

		if (k == 4 || (k == 1 && *p < 0xC2u))
			return 0;
		for (p++; k; k--, p++)
			if ((*p & 0xC0u) != 0x80u)
				return 0;
	}
	return 1;
}

static void ctl_command(struct stubd *s, const char *line, size_t len,
			uint64_t now)
{
	struct dect_ctl_msg m;
	const char *pr;
	unsigned v, i;

	if (dect_ctl_parse(line, len, &m)) {
		printf("ctl: a malformed line -- ignored\n");
		ctl_reply(s, "?", "malformed", NULL, 0);
		return;
	}
	if (!strcmp(m.verb, "deregister")) {
		if (dect_ctl_get_uint(&m, "hs", 1, DECT_MM_MAX_SLOT, &v)) {
			ctl_reply(s, m.verb, "bad-hs", NULL, 0);
			return;
		}
		for (i = 0; i < s->base.n_hs; i++)
			if (ctl_listed(s, i) && hs_slot(s, i) == v)
				break;
		if (i == s->base.n_hs) {
			ctl_reply(s, m.verb, "no-handset", "hs", v);
			return;
		}
		dereg_begin(s, v, now, "the control socket");
		ctl_reply(s, m.verb, NULL, "hs", v);
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
	} else if (!strcmp(m.verb, "rename")) {
		const char *nm = dect_ctl_get(&m, "name");

		if (dect_ctl_get_uint(&m, "hs", 1, DECT_MM_MAX_SLOT, &v)) {
			ctl_reply(s, m.verb, "bad-hs", NULL, 0);
			return;
		}
		for (i = 0; i < s->base.n_hs; i++)
			if (ctl_listed(s, i) && hs_slot(s, i) == v)
				break;
		if (i == s->base.n_hs) {
			ctl_reply(s, m.verb, "no-handset", "hs", v);
			return;
		}
		if (!nm || !ctl_name_ok(nm)) {
			ctl_reply(s, m.verb, "bad-name", "hs", v);
			return;
		}
		/* as a handset's own save does (names_take()) */
		snprintf(s->hs[i].name, sizeof(s->hs[i].name), "%s", nm);
		name_save(s, i);
		printf("names: DECT %u is now %s%s%s (the control socket)\n", v,
		       nm[0] ? "\"" : "", nm[0] ? nm : "back to its default name",
		       nm[0] ? "\"" : "");
		names_spread(s, i);
		names_notify(s);
		ctl_reply(s, m.verb, NULL, "hs", v);
		ctl_hs(s, i, "registered");
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
 * The internal caller's identity for a Part 3 target, after the SETUP:
 * {CC-INFO} with <<CALLING-PARTY-NUMBER>>, <<CALLING-PARTY-NAME>> in UTF-8
 * (stock's octet 3 is 05H) and <<CALL-INFORMATION>> with the call id only,
 * as stock sends the A540 [CR A-0130 §1].
 */
static void ic_name_info(struct stubd *s, struct dect_cc *t, uint64_t now)
{
	struct dect_clip id = ic_clip;
	struct dect_callinfo ci;
	uint8_t ies[96];
	size_t off = 0, next;

	s->ic.name_info = 0;
	if (dect_clip_set_name(&id, ic_clip.name, DECT_CLIP_ALPHABET_UTF8,
			       DECT_CLIP_PRES_ALLOWED,
			       DECT_CLIP_SCR_USER_PASSED))
		id = ic_clip;			/* keep the DECT alphabet */
	next = dect_clip_put_number(ies, sizeof(ies), off, &id);
	if (next)
		off = next;
	next = dect_clip_put_name(ies, sizeof(ies), off, &id, 0);
	if (next)
		off = next;
	dect_callinfo_init(&ci);
	ci.has_call_id = 1;
	ci.call_id = t->status_call_id;
	next = dect_callinfo_put(ies, sizeof(ies), off, &ci);
	if (next)
		off = next;
	if (!off || dect_cc_info(t, ies, off)) {
		printf("intern: could not queue the caller's name\n");
		return;
	}
	ic_drain(s, s->ic.tside, now);
	printf("intern: caller's name \"%s\" sent in CC-INFO (Part 3, "
	       "A-0130)\n", ic_clip.name);
}

static void ic_tick(struct stubd *s, uint64_t now)
{
	struct dect_cc *c, *t;

	if (s->ic.st == IC_IDLE)
		return;
	c = ic_cc(s, s->ic.caller);
	if (!ic_active(s, s->ic.caller) || !dect_cc_busy(c)) {
		if (s->ic.tside != IC_NONE && ic_active(s, s->ic.tside) &&
		    dect_cc_busy(ic_cc(s, s->ic.tside))) {
			(void)dect_cc_release(ic_cc(s, s->ic.tside));
			ic_drain(s, s->ic.tside, now);
		}
		ic_end(s, "the caller hung up");
		return;
	}
	if (s->ic.st == IC_PAGING) {
		unsigned k;

		for (k = 0; k < SIDE_MAX && s->ic.st == IC_PAGING; k++)
			if (s->side[k].bl.active &&
			    s->side[k].adopted == s->ic.target &&
			    !dect_cc_busy(&s->side[k].bl.link.cc))
				ic_setup(s, (int)k, now,
					 "came up (adopted, no page response)");
		/*
		 * or the main link, if the caller is not on it (finding 5) --
		 * only once it has adopted the target. A main link that has
		 * adopted no one is not presumed to serve s->base.cur: whichever
		 * handset connects next would get the SETUP (the deregistration
		 * did exactly that on the bench, 2026-10-28). The target's
		 * page response is matched by identity in ic_target_link().
		 */
		if (s->ic.st == IC_PAGING && s->ic.caller != -1 &&
		    s->base.main_link.active && !dect_cc_busy(&s->base.main_link.link.cc) &&
		    s->base.main_link.link.mm.adopted == s->ic.target)
			ic_setup(s, -1, now,
				 "came up (adopted, no page response)");
		if (s->ic.st != IC_PAGING)
			return;
		if (now >= s->ic.page_at) {
			if (s->ic.pages >= IC_PAGES) {
				(void)dect_cc_release(c);
				ic_end(s, "the target did not answer the page");
				return;
			}
			ic_page(s);
			s->ic.page_at = now + IC_PAGE_MS;
		}
		return;
	}
	if (s->ic.tside == IC_NONE || !ic_active(s, s->ic.tside) ||
	    !dect_cc_busy(ic_cc(s, s->ic.tside))) {
		(void)dect_cc_release(c);
		ic_end(s, "the target hung up or refused");
		return;
	}
	t = ic_cc(s, s->ic.tside);
	if (s->ic.st == IC_SETUP && t->state == DECT_CC_ST_RINGING) {
		(void)dect_cc_alert(c);
		s->ic.st = IC_RINGING;
		printf("intern: the target rings -- ringback to the caller\n");
		if (s->ic.name_info)
			ic_name_info(s, t, now);
	}
	if (s->ic.st != IC_UP && t->state == DECT_CC_ST_ACTIVE &&
	    call_auth_ok(s, s->ic.tside < 0 ? &s->base.main_link.link
					    : &s->side[s->ic.tside].bl.link)) {
		(void)dect_cc_connect(c);
		s->ic.st = IC_UP;
		printf("intern: the target answered -- connected, bridging\n");
	}
	if (s->ic.st == IC_UP && !s->ic.warned &&
	    ic_wideband(s, s->ic.caller) != ic_wideband(s, s->ic.tside) &&
	    ic_has_lane(s, s->ic.caller) && ic_has_lane(s, s->ic.tside)) {
		s->ic.warned = 1;
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
		     : s->side[p].lane >= 0 && s->xl[s->side[p].lane].wideband;
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

	if (side == IC_NONE || (s->ic.st == IC_UP &&
	    ic_wideband(s, s->ic.caller) != ic_wideband(s, s->ic.tside)))
		return -1;
	if (side < 0)
		w = s->reg.call_bound ? (int)s->audio.lane0_word : -1;
	else if (s->side[side].lane < 0 ||
		 !s->xl[s->side[side].lane].connected)
		w = -1;
	else
		w = (int)s->xl[s->side[side].lane].word;
	return w >= 0 && (unsigned)w < s->slots &&
	       w != s->tx_slot ? w : -1;
}

static void side_pump(struct stubd *s, struct side_link *sl)
{
	struct dect_mail m;

	while (sizeof(s->txq) - s->txq_len >= DECT_TX_ELEM_MAX &&
	       dect_link_poll_append_mail(&sl->bl.link, &m)) {
		if (send_mail(s, (unsigned int)s->tx_slot, &m))
			printf("side: C-plane mail LOST (%u bytes)\n",
			       (unsigned)m.param_len);
		else
			s->led_traffic++;
	}
}

/*
 * Swap side link sl with the main link: the ring target answered our page
 * here while the main link carries another handset that is not in a call.
 * Both are C-plane state only -- struct dect_link holds no pointer into
 * itself but the tx-log hook, set again below -- so the swap is a copy.
 * Returns 1 when promoted (the SETUP is then queued on the new main).
 */
static int side_promote(struct stubd *s, struct side_link *sl,
			const struct dect_link_ind *ind, uint64_t now)
{
	struct dect_link tmp;
	size_t tgt_len;
	const uint8_t *tgt = ring_target_port_id(s, &tgt_len);
	int old_main;
	uint8_t h;

	/* not mid-enrolment either: a side link cannot persist a key */
	if (s->ring_state != RING_WAIT_PAGE_RESP || !s->base.main_link.active ||
	    !tgt_len || ind->page_id_len != tgt_len ||
	    memcmp(ind->page_id, tgt, tgt_len) != 0 ||
	    s->reg.call_bound || dect_cc_busy(&s->base.main_link.link.cc) ||
	    s->base.main_link.link.mm.state == DECT_MM_WAIT_AUTH ||
	    dect_cc_busy(&sl->bl.link.cc))
		return 0;
	old_main = s->base.main_link.link.mm.adopted >= 0 ? s->base.main_link.link.mm.adopted :
		   s->base.main_link.link.mm.port_id_len ? (int)s->base.cur : -1;
	tmp = s->base.main_link.link;
	s->base.main_link.link = sl->bl.link;
	sl->bl.link = tmp;
	h = s->base.main_link.handle;
	s->base.main_link.handle = sl->bl.handle;
	sl->bl.handle = h;
	sl->adopted = old_main;
	dect_link_set_tx_log(&s->base.main_link.link, log_nwk_out, s);
	dect_link_set_tx_log(&sl->bl.link, log_nwk_out_side, s);

	/* the old main is a side link now: no enrolment, no speech */
	sl->bl.link.mm.cfg.registration = DECT_MM_REG_CLOSED;
	dect_cc_set_busy(&sl->bl.link.cc, 1);
	sl->rejected_seen = sl->bl.link.cc.stats.rejected_identity;

	/*
	 * The new main serves the target: the page response matched the ring
	 * target (hs[0]), whether or not its link has adopted anyone yet --
	 * a page response does not adopt (review, finding 2).
	 */
	hs_switch(s, s->base.main_link.link.mm.adopted >= 0 ?
		  (unsigned)s->base.main_link.link.mm.adopted : s->ring_hs,
		  " (promoted side link)");
	s->base.main_link.link.mm.cfg.registration = reg_policy(s);
	s->base.main_link.link.mm.cfg.defer_unknown = s->audio.remote_path != NULL;
	s->base.main_link.link.mm.cfg.locate_validity =
		(uint8_t)s->audio.locate_validity;
	main_cc_setup(s);
	s->reg.call_wideband = 0;
	s->reg.connect_staged = 0;
	s->reg.nonspeech_noted = 0;
	s->base.main_link.enrol_routed = 0;
	s->base.main_link.enrol_switched = 0;
	/* what reg_start() resets for a new connection (review, finding 8) */
	s->base.main_link.persisted = 0;
	/* a side link's cipher (side_cipher_tick) came along with it */
	s->reg.cipher_started =
		s->base.main_link.link.mm.cipher_state != DECT_MM_CIPHER_NONE;
	s->base.main_link.cipher_done =
		s->base.main_link.link.mm.cipher_state == DECT_MM_CIPHER_READY ||
		s->base.main_link.link.mm.cipher_state == DECT_MM_CIPHER_FAILED;
	sl->bl.cipher_done = 1;	/* the old main's was the main link's to do */
	s->reg.cipher_reject_seen = 0;
	s->base.main_link.auth_refused = 0;
	s->reg.cipher_timeout_seen = 0;
	s->reg.audio_connected = 0;
	s->reg.time_sent_for = 0;
	printf("ring: page response on side link %u -- promoted to the main "
	       "link; handle %u (%s) is a side link now\n", s->base.main_link.handle,
	       sl->bl.handle, (unsigned)old_main < s->base.n_hs ?
	       s->hs[old_main].path : "?");
	ring_queue_setup(s, now, "page response received on a side link");
	dect_link_drain(&s->base.main_link.link, now);
	pump_lapc(s);
	side_pump(s, sl);
	return 1;
}

/*
 * Cipher a side link's call as the main link's (start_call_cipher()): a
 * speech call once connected, a list-access call at once (TS 102 527-3
 * C.7.2). The second handset's call in bench test 10 ran in the clear,
 * and a list session here could not take the PIN. The UAK is the one of
 * the handset the link adopted; a link that adopted no one stays clear.
 */
static void side_cipher_tick(struct stubd *s, struct side_link *sl,
			     uint64_t now)
{
	struct dect_link *l = &sl->bl.link;
	const struct dect_cc *cc = &l->cc;
	int a = l->mm.adopted;

	if (l->mm.cipher_state == DECT_MM_CIPHER_NONE && !s->no_cipher &&
	    dect_cc_busy(cc) && (a >= 0 || s->audio.auth_first) &&
	    (s->audio.auth_first ||
	     ((cc->peer_basic_service >> 4) == 0x2u && !cc->incoming) ||
	     cc->state == DECT_CC_ST_ACTIVE)) {
		const uint8_t *uak = a < 0 ? NULL :
				     dect_base_uak(&s->base, (unsigned)a);

		if (!uak) {
			/* authfirst: nothing to prove it against (A1) */
			if (s->audio.auth_first && !sl->bl.auth_refused) {
				sl->bl.auth_refused = 1;
				(void)dect_cc_release_reason(&l->cc, 0x09u);
				printf("side: released -- no key to "
				       "authenticate the handset on handle "
				       "%u with\n", sl->bl.handle);
				dect_link_drain(l, now);
				side_pump(s, sl);
			}
			return;
		}
		dect_base_cipher_start(&s->base, &sl->bl, uak);
		sl->bl.cipher_done = 0;
		printf("side: cipher: authenticating the handset on handle %u\n",
		       sl->bl.handle);
		dect_link_drain(l, now);
		side_pump(s, sl);
		return;
	}
	if (sl->bl.cipher_done)
		return;
	if (l->mm.cipher_state == DECT_MM_CIPHER_READY) {
		sl->bl.cipher_done = 1;
		if (!s->chip_cipher.disabled)
			(void)dect_base_chip_key(&s->base, &sl->bl);
		printf("side: cipher: DCK derived, CIPHER-REQUEST sent on "
		       "handle %u\n", sl->bl.handle);
	} else if (l->mm.cipher_state == DECT_MM_CIPHER_FAILED) {
		sl->bl.cipher_done = 1;
		printf("side: cipher: authentication FAILED on handle %u (%s)\n",
		       sl->bl.handle, l->mm.stats.auth_timeouts ? "no reply in 10 s"
		       : l->mm.stats.auth_mismatch ? "WRONG RES"
		       : "the handset refused");
		/* EN 300 444 8.23.2.3, as on the main link */
		if ((l->mm.stats.auth_mismatch || s->audio.auth_first) &&
		    dect_cc_busy(cc)) {
			(void)dect_cc_release_reason(&l->cc, 0x09u);
			printf("side: released -- the handset failed "
			       "authentication (EN 300 444 8.23.2.3)\n");
			dect_link_drain(l, now);
			side_pump(s, sl);
		}
	}
}

static void side_segment(struct stubd *s, struct side_link *sl,
			 const struct dect_mail *m, uint64_t now)
{
	struct dect_link_ind ind;
	int a;

	side_call_tick(s, sl, now);	/* busy or not, before a SETUP lands */
	if (dect_base_link_segment(&s->base, &sl->bl, m, now, &ind) < 0) {
		side_pump(s, sl);
		return;
	}
	side_call_tick(s, sl, now);
	a = sl->bl.link.mm.adopted;
	if (a >= 0 && (unsigned)a < s->base.n_hs)
		s->hs[a].last_seen_ms = now;
	if (a >= 0 && a != sl->adopted && (unsigned)a < s->base.n_hs) {
		sl->adopted = a;
		dect_iwu_set_own(&sl->bl.link.cc.iwu,
				 (uint8_t)hs_slot(s, (unsigned)a));
		iwu_lines(s, &sl->bl.link.cc.iwu, (uint8_t)hs_slot(s, (unsigned)a));
		printf("side: handle %u serves %s (slot %u)\n", sl->bl.handle,
		       s->hs[a].path, hs_slot(s, (unsigned)a));
	}
	/* a location registration here: the waiting messages again */
	if (sl->bl.link.mm.stats.located != sl->located_seen) {
		sl->located_seen = sl->bl.link.mm.stats.located;
		mwi_locate(s, &sl->bl.link);
		missed_locate(s, &sl->bl.link);
	}
	if (sl->bl.link.cc.stats.rejected_identity != sl->rejected_seen) {
		sl->rejected_seen = sl->bl.link.cc.stats.rejected_identity;
		printf("side: turned away a call from a handset we are not "
		       "subscribed to (handle %u, release reason 0x0a)\n",
		       sl->bl.handle);
	}
	/*
	 * Ringing has started and a locating handset took the main link
	 * first, so the target's page response lands here (12d, 2026-10-25:
	 * the ring timed out). Promote this link to the main one and ring on
	 * it; anything else that answers here is not ours to ring.
	 */
	if (dereg_page_response(s, sl, &ind) ||
	    time_page_response(s, sl, &ind)) {
		side_pump(s, sl);
		return;
	}
	if (ind.page_response && s->ic.st == IC_PAGING) {
		ic_target_link(s, sl, &ind, now);
		if (s->ic.st == IC_SETUP)
			return;
	}
	if (ind.page_response && sr_target_link(s, sl, &ind, now))
		return;
	if (ind.page_response && side_promote(s, sl, &ind, now))
		return;
	if (ind.page_response)
		printf("side: page response on handle %u while the main link "
		       "is busy -- not ours to ring here, ignored\n",
		       sl->bl.handle);
	side_pump(s, sl);
}

/* Drain queued NWK responses (MM and CC) into LAPC, one per free window. */
static void drain_nwk(struct stubd *s, uint64_t now)
{
	dect_link_drain(&s->base.main_link.link, now);
	pump_lapc(s);
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
	if (s->reg.cipher_started)
		return;
	if (s->no_cipher) {
		s->reg.cipher_started = 1;
		printf("call: cipher: suppressed (-C), call stays in the clear\n");
	} else if (s->base.rec.uak_valid) {
		dect_base_cipher_start(&s->base, &s->base.main_link,
				       s->base.rec.uak);
		s->reg.cipher_started = 1;
		printf("call: cipher: authenticating handset\n");
		drain_nwk(s, now);
	} else {
		printf("call: cipher: no UAK (handset must enrol this session)\n");
	}
}
static void reg_segment(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	struct dect_link_ind ind;
	uint32_t collisions = s->base.main_link.link.cc.stats.collisions;
	uint32_t represented = s->base.main_link.link.cc.stats.represented;
	int switched;

	if (dect_base_main_segment(&s->base, m, now, &ind, &switched) < 0) {
		if (ind.foreign) {
			/*
			 * Another handset's connection. The library refused to
			 * let it touch this link's Lc/LAPC/NWK state; we only
			 * count it, and say so the first time so a confounded
			 * run is visible while it is running.
			 */
			if (!s->foreign_seg)
				printf("reg: ignoring C-plane traffic for "
				       "handle %u -- this link serves handle "
				       "%u. Another handset is on air.\n",
				       ind.seg_handle, s->base.main_link.handle);
			s->foreign_seg++;
			s->foreign_seg_handle = ind.seg_handle;
		}
		return;
	}
	pump_lapc(s);			/* at least the LAPC ack */
	if (s->base.main_link.link.mm.adopted >= 0 &&
	    (unsigned)s->base.main_link.link.mm.adopted < s->base.n_hs)
		s->hs[s->base.main_link.link.mm.adopted].last_seen_ms = now;

	/*
	 * MM took on one of the other handsets we hold (its LOCATE-REQUEST,
	 * or a call it placed). Serve it as itself: its store, its call
	 * identities, and its own entry in the Internal Names List, set before
	 * the list-access session that follows its {CC-SETUP} asks for it.
	 */
	if (switched) {
		hs_switched(s, "");
		dect_iwu_set_own(&s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
		iwu_lines(s, &s->base.main_link.link.cc.iwu, stubd_sub_slot(s));
	}

	if (!s->first_contact_ms) {
		s->first_contact_ms = now;
		printf("reg: first C-plane contact from the handset at t=%llu ms\n",
		       (unsigned long long)(now - g_start));
	}

	/*
	 * EN 300 175-5 §9.5.2.3, in the log while it happens. Every refusal
	 * of an FP-originated call in this project's corpus (60 of 60, from
	 * 93 logs tabulated on 2026-10-24) coincided with the handset's own
	 * list-access {CC-SETUP} being on the wire, active, or pending; the
	 * library now resolves the collision, and these two lines are how a
	 * log shows which half of it ran.
	 */
	if (s->base.main_link.link.cc.stats.collisions != collisions)
		printf("ring: {CC-SETUP} COLLISION -- the handset opened a call "
		       "of its own while ours was pending; releasing its call "
		       "with \"insufficient resources\" (EN 300 175-5 9.5.2.3)\n");
	if (s->base.main_link.link.cc.stats.represented != represented)
		printf("ring: the handset released our call for the collision; "
		       "RE-PRESENTING the {CC-SETUP} now (9.5.2.3), %u left\n",
		       s->base.main_link.link.cc.represent_left);

	if (s->verbose) {
		printf("reg: cs in %02x %02x %02x (addr/ctrl/li), lc_rc=%d\n",
		       ind.seg_head[0], ind.seg_head[1], ind.seg_head[2],
		       ind.lc_rc);
		if (ind.lapc_parse_failed)
			printf("reg: lapc parse failed\n");
		else if (ind.lc_rc > 0)
			printf("reg: lapc type=%d ns=%u nr=%u more=%u infolen=%zu\n",
			       ind.frame.type, ind.frame.ns, ind.frame.nr,
			       ind.frame.more, ind.frame.info_len);
		if (ind.nwk_len) {
			size_t i;

			/* local only: carries IPUI, never commit */
			printf("reg: nwk in  <-- ");
			for (i = 0; i < ind.nwk_len; i++)
				printf("%02x ", ind.nwk[i]);
			printf("\n");
		}
	}

	/*
	 * The handset's answer to our page. This is the point at which an
	 * FP-originated {CC-SETUP} can be sent: see ring_queue_setup() for the
	 * run that showed sending it on link-up loses the frame.
	 */
	/*
	 * TS 102 527-1 §7.3.8.2.1: only a page response with the IPUI we
	 * paged stops the paging. Another handset's answer is not ours to
	 * ring: §7.3.8.3.1 answers it with {LCE-PAGE-REJECT} carrying its full
	 * IPUI, and releases the unwanted link at once. We keep waiting for
	 * the right one. The release goes out only once the reject is in LAPC
	 * ahead of it; otherwise the link is left for its own timers.
	 */
	size_t tgt_len;
	const uint8_t *tgt = ring_target_port_id(s, &tgt_len);

	/*
	 * A handset offering its clock (TS 102 527-3 §7.4.2.2). Our 'Clock
	 * master' is the FP (the System Settings List says so), and the box
	 * keeps time by NTP: logged, not used. An unidentified one is ignored,
	 * as the clause requires.
	 */
	if (ind.pp_time || ind.pp_time_unknown) {
		const struct dect_time *t = &ind.pp_time_value;

		printf("time: %s handset sent its clock (20%02u-%02u-%02u "
		       "%02u:%02u:%02u) -- %s\n",
		       ind.pp_time ? "the" : "an UNIDENTIFIED",
		       t->year, t->month, t->day, t->hour, t->minute,
		       t->second, ind.pp_time
		       ? "not used: the base is the clock master"
		       : "ignored (7.4.2.2)");
	}

	if (dereg_page_response(s, NULL, &ind) ||
	    time_page_response(s, NULL, &ind))
		ind.page_response = 0;

	/* an internal call's target answering on the main link (finding 5) */
	if (ind.page_response && s->ic.st == IC_PAGING && s->ic.caller != -1) {
		ic_target_link(s, NULL, &ind, now);
		if (s->ic.st == IC_SETUP)
			ind.page_response = 0;
	}

	if (ind.page_response && s->ring_state == RING_WAIT_PAGE_RESP &&
	    tgt_len && (ind.page_id_len != tgt_len ||
			memcmp(ind.page_id, tgt, tgt_len) != 0)) {
		int rejected = dect_link_page_reject(&s->base.main_link.link, ind.page_id,
						     ind.page_id_len,
						     ind.page_ti) == 0;

		if (rejected) {
			dect_link_drain(&s->base.main_link.link, now);
			rejected = !s->base.main_link.link.lce_len;
		}
		if (rejected)
			dect_lapc_disconnect(&s->base.main_link.link.lapc);
		pump_lapc(s);
		printf("ring: page response from a handset we did not page "
		       "(identity differs) -- %s, still waiting\n",
		       rejected ? "LCE-PAGE-REJECT sent, link released"
				: "could not send LCE-PAGE-REJECT, ignored");
		ind.page_response = 0;
	}
	if (ind.page_response && s->ring_state == RING_WAIT_PAGE_RESP) {
		if (s->base.cur != s->ring_hs)
			hs_switch(s, s->ring_hs, " (the ring target answered)");
		ring_queue_setup(s, now, "page response received");
		/*
		 * Push it out in the same breath. The main loop would now get
		 * it within POLL_MS anyway, but the handset has just answered
		 * the page and is listening: the whole point of waiting for
		 * the page response is to hit that window.
		 */
		drain_nwk(s, now);
	}

	/*
	 * The handset dialled. GAP N.4 is mandatory for an FT and we did not
	 * implement it until 2026-10-23 -- seven runs in the capture corpus
	 * hold a handset sending "12345" past a base that dropped it.
	 *
	 * Printing is all the daemon does with it: routing a dialled number
	 * is the controller's job, and this build has no line to dial out on.
	 * The digits stay on the CC entity until someone takes them.
	 *
	 * GAP's basic set is 0-9, * and #. Anything else is shown as a hex
	 * escape rather than written raw to a terminal -- a handset may send
	 * control codes (pause, register recall, go-to-pulse), and those are
	 * data, not formatting.
	 */
	if (ind.keypad) {
		size_t n, i;
		const uint8_t *kp = dect_cc_keypad(&s->base.main_link.link.cc, &n);
		char show[DECT_CC_KEYPAD_MAX * 4 + 1];
		size_t o = 0;

		for (i = 0; i < n && o + 5 < sizeof(show); i++) {
			uint8_t c = kp[i];

			if ((c >= '0' && c <= '9') || c == '*' || c == '#')
				show[o++] = (char)c;
			else
				o += (size_t)snprintf(show + o,
						      sizeof(show) - o,
						      "\\x%02x", c);
		}
		show[o] = '\0';
		printf("call: handset dialled: %s (%zu character%s)%s\n",
		       show, n, n == 1 ? "" : "s",
		       s->base.main_link.link.cc.keypad_overflow ? " [TRUNCATED]" : "");
		fflush(stdout);
	}

	if (s->base.main_link.link.cc.stats.rejected_identity != s->cc_rejected_seen) {
		s->cc_rejected_seen = s->base.main_link.link.cc.stats.rejected_identity;
		printf("call: turned away a call from a handset we are not "
		       "subscribed to (release reason 0x0a, %u on this "
		       "link)\n", (unsigned)s->cc_rejected_seen);
		fflush(stdout);
	}

	/*
	 * A call the handset places carries the two identities an
	 * FP-originated SETUP needs, in the handset's own encoding. Keep the
	 * first pair we see and persist it, so calling the handset later needs
	 * no guesswork about identity types, bit lengths or padding.
	 */
	if (ind.cc_message && !s->base.rec.cc_id_learned &&
	    s->base.main_link.link.cc.peer_fixed_id_len &&
	    !dect_cc_peer_is(&s->base.main_link.link.cc, s->base.rec.port_id,
			     s->base.rec.port_id_len)) {
		/*
		 * A CC transaction from a handset we are not subscribed to.
		 * Learning from it would persist ANOTHER handset's identities
		 * into this handset's store, and every later FP-originated
		 * {CC-SETUP} would address the wrong handset -- which is
		 * exactly what happened on 2026-10-24: a second handset runs a
		 * CAT-iq list-access call every ~22 s, so it reliably spoke
		 * first and won the race for `cc_id_learned`.
		 *
		 * `cc_id_learned` is deliberately NOT set here: the subscribed
		 * handset may still speak later in this run and should be
		 * allowed to.
		 */
		if (!s->cc_id_foreign_warned) {
			s->cc_id_foreign_warned = 1;
			printf("call: ignoring call identities from a handset "
			       "we are not subscribed to -- not learning, "
			       "not saving\n");
			fflush(stdout);
		}
	} else if (ind.cc_message && !s->base.rec.cc_id_learned &&
		   s->base.main_link.link.cc.peer_fixed_id_len && !s->base.rec.port_id_len) {
		/*
		 * No handset registered yet, so nothing to check the caller
		 * against, and dect_cc_peer_is() says yes to anyone. On
		 * 2026-10-24 the A540 placed its list-access call in the
		 * AS690H's enrolment window, before the AS690H had registered;
		 * its identities went into the AS690H's store, and the next
		 * call to the AS690H addressed the A540 and was refused with
		 * "invalid identity". Learn only once an identity is armed.
		 */
		if (!s->cc_id_unarmed_warned) {
			s->cc_id_unarmed_warned = 1;
			printf("call: not learning call identities -- no "
			       "handset registered yet to check them "
			       "against\n");
			fflush(stdout);
		}
	} else if (ind.cc_message && !s->base.rec.cc_id_learned &&
		   s->base.main_link.link.cc.peer_fixed_id_len &&
		   /* not from whoever named its IPUI (audit A3) */
		   s->base.main_link.link.mm.authenticated) {
		memcpy(s->base.rec.cc_port_id, s->base.main_link.link.cc.peer_port_id,
		       sizeof(s->base.rec.cc_port_id));
		s->base.rec.cc_port_id_len = s->base.main_link.link.cc.peer_port_id_len;
		memcpy(s->base.rec.cc_fixed_id, s->base.main_link.link.cc.peer_fixed_id,
		       sizeof(s->base.rec.cc_fixed_id));
		s->base.rec.cc_fixed_id_len = s->base.main_link.link.cc.peer_fixed_id_len;
		s->base.rec.cc_id_learned = 1;
		printf("call: learned the identities for calling the handset "
		       "(%zu + %zu bytes), saving\n",
		       s->base.rec.cc_port_id_len, s->base.rec.cc_fixed_id_len);
		dect_base_changed(&s->base);
	}

	/*
	 * Derive them instead, if the handset has not placed a call.
	 *
	 * Both identities are ours to build the moment the handset is known:
	 * the portable one is the IPUI it gave us during registration, and the
	 * fixed one is our own PARK, which we already send it in
	 * {ACCESS-RIGHTS-ACCEPT}. Measured on the A540, our accept and the
	 * {CC-SETUP} the handset later places carry byte-identical IEs.
	 *
	 * Waiting for the handset to dial first was never a DECT requirement
	 * -- EN 300 444 table 21 specifies both elements exactly -- it was an
	 * artefact of the PARK encoding living only inside dect_mm.c. Stock
	 * does not need it either. A handset that never places a call, or a
	 * freshly registered one, could not be rung at all.
	 */
	if (!s->base.rec.cc_fixed_id_len && s->have_rfpi && s->base.rec.port_id_len &&
	    s->base.rec.port_id_len <= sizeof(s->base.rec.cc_port_id)) {
		uint8_t fid[DECT_CC_ID_MAX];
		size_t n = dect_mm_fixed_identity(&s->base.main_link.link.mm, fid,
						  sizeof(fid));

		if (n) {
			/* this handset's identity, not the link MM's */
			memcpy(s->base.rec.cc_port_id, s->base.rec.port_id,
			       s->base.rec.port_id_len);
			s->base.rec.cc_port_id_len = s->base.rec.port_id_len;
			memcpy(s->base.rec.cc_fixed_id, fid, n);
			s->base.rec.cc_fixed_id_len = n;
			printf("call: derived the identities for calling the "
			       "handset (%zu + %zu bytes) from the "
			       "subscription, saving\n",
			       s->base.rec.cc_port_id_len, s->base.rec.cc_fixed_id_len);
			dect_base_changed(&s->base);
		}
	}

	/*
	 * Once the handset is authenticated (UAK valid) and we have answered its
	 * LOCATE-REQUEST, install the subscription in the chip, once per connection.
	 */
	if (dect_base_main_registered(&s->base)) {
		/*
		 * The chip now holds what the start-up restore would write:
		 * without this the restore in loop_tick() saw the new UAK and
		 * sent the whole install a second time (register.trace,
		 * 2026-10-30).
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

	/*
	 * A base is where a handset gets its clock, and we never sent one --
	 * so every timestamp a handset showed, call lists above all, was
	 * whatever it had invented.
	 *
	 * TS 102 527-3 §7.4.20.2: "If the system Date and Time on FT side is
	 * valid, the FT shall systematically use the 'FT initiated Date and
	 * Time synchronization procedure' after successful PT location
	 * registration". It adds that we send it "even if the PT does not
	 * indicate support" and "independently of the 'Clock master' setting
	 * value" -- so there is nothing to negotiate and nothing to check.
	 *
	 * Our clock is the box's. If the box has no valid time (no RTC on
	 * this hardware, so this means NTP has not landed yet) we send
	 * nothing: the same clause makes the duty conditional on the FT's own
	 * time being valid, and a confidently wrong clock is worse than none.
	 */
	if (s->base.main_link.link.mm.stats.located != s->reg.time_sent_for) {
		struct dect_time dt;
		long off_s;

		s->reg.time_sent_for = s->base.main_link.link.mm.stats.located;
		if (clock_now(s->audio.timesync, &dt, &off_s)) {
			if (dect_link_send_time(&s->base.main_link.link, &dt) == 0)
				printf("reg: sent the date and time "
				       "(20%02u-%02u-%02u %02u:%02u:%02u, "
				       "GMT%+ld:%02ld)\n",
				       dt.year, dt.month, dt.day, dt.hour,
				       dt.minute, dt.second,
				       off_s / 3600, labs(off_s % 3600) / 60);
			else
				printf("reg: could not queue the date and "
				       "time\n");
		} else {
			printf("reg: the clock is not known to be right (no "
			       "NTP sync reported) -- sending no date and "
			       "time. TS 102 527-3 7.4.20.2 makes the duty "
			       "conditional on ours being valid, and a "
			       "confidently wrong clock is worse than none\n");
		}
		locate_notify(s, &s->base.main_link.link);
		mwi_locate(s, &s->base.main_link.link);
		missed_locate(s, &s->base.main_link.link);
	}

	if (s->base.main_link.link.mm.state != DECT_MM_WAIT_AUTH)
		s->base.main_link.enrol_routed = 0;
	else if (!s->base.main_link.enrol_routed) {
		s->base.main_link.enrol_routed = 1;
		enrol_route(s);
	}

	/* once per outcome, not on every message that follows it */
	if (s->base.main_link.link.mm.state != s->base.main_link.reported_state &&
	    s->base.main_link.link.mm.peer_auth_len >= 2 &&
	    (s->base.main_link.link.mm.state == DECT_MM_REGISTERED ||
	     s->base.main_link.link.mm.state == DECT_MM_FAILED)) {
		const uint8_t *at = s->base.main_link.link.mm.peer_auth;

		printf("reg: it proposed %s with %s %u (we run DSAA from the "
		       "PIN)\n",
		       at[0] == 0x01 ? "DSAA" : at[0] == 0x02 ? "DSAA2" :
		       "another algorithm",
		       (at[1] >> 4) == 0x4 ? "the authentication code" :
		       (at[1] >> 4) == 0x1 ? "user authentication key" :
		       "key type", (unsigned)(at[1] & 0x0Fu));
	}
	if (s->base.main_link.link.mm.state != s->base.main_link.reported_state &&
	    s->base.main_link.link.mm.state == DECT_MM_REGISTERED) {
		printf("reg: handset registered\n");
		if (s->base.n_hs && s->base.cur < s->base.n_hs)
			names_spread(s, s->base.cur);  /* into every link's list */
		names_notify(s);	/* §7.4.10.2.2: one added */
	}
	else if (s->base.main_link.link.mm.state != s->base.main_link.reported_state &&
		 s->base.main_link.link.mm.state == DECT_MM_FAILED) {
		printf("reg: registration rejected%s\n",
		       s->base.n_hs && s->base.cur < s->base.n_hs &&
		       s->hs[s->base.cur].prof.reg_tries_0000_first &&
		       s->base.main_link.link.mm.stats.auth_fail
		       ? " (expected: this model tries PIN 0000 by itself "
			 "first, profile reg.tries_0000_first)" : "");
		/* back to the ring target's store (review, finding 13) */
		if (s->base.main_link.enrol_switched) {
			s->base.main_link.enrol_switched = 0;
			hs_switch(s, 0, " (enrolment failed)");
			s->base.main_link.link.mm.cfg.sub_slot = stubd_sub_slot(s);
		}
	}
	if (s->base.main_link.link.mm.state == DECT_MM_REGISTERED)
		s->base.main_link.enrol_switched = 0;
	s->base.main_link.reported_state = s->base.main_link.link.mm.state;

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
	if (!s->reg.call_bound && !s->reg.nonspeech_noted &&
	    !s->base.main_link.link.cc.incoming &&
	    !dect_cc_peer_is_speech(&s->base.main_link.link.cc) &&
	    s->base.main_link.link.cc.state == DECT_CC_ST_ACTIVE) {
		s->reg.nonspeech_noted = 1;
		printf("call: not a speech call (<<BASIC-SERVICE>> %02x) -- "
		       "no audio path bound\n",
		       s->base.main_link.link.cc.peer_basic_service);
	}

	if (!s->reg.call_bound && call_auth_ok(s, &s->base.main_link.link) &&
	    (s->base.main_link.link.cc.state == DECT_CC_ST_ACTIVE ||
	     (s->audio.bind_first && s->reg.connect_staged &&
	      s->base.main_link.link.cc.state == DECT_CC_ST_PROCEEDING)) &&
	    (s->base.main_link.link.cc.incoming ||
	     dect_cc_peer_is_speech(&s->base.main_link.link.cc))) {
		s->reg.audio_connected = 0;
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
		s->reg.call_wideband = !s->audio.narrowband &&
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
		s->reg.call_bound = 1;
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

	/*
	 * Authenticate first (A1, call_auth_ok()): challenge the handset as
	 * soon as a call exists on the link, in either direction, as a
	 * list-access call always was (below). Nothing is routed or passed on
	 * until it answers. Without a key for the identity it claims there is
	 * nothing to check it against, so the call ends.
	 */
	if (s->audio.auth_first && !s->no_cipher && !s->reg.cipher_started &&
	    !s->base.main_link.auth_refused && s->base.main_link.active &&
	    dect_cc_busy(&s->base.main_link.link.cc)) {
		if (s->base.rec.uak_valid) {
			printf("call: authenticating the handset before the "
			       "call goes anywhere\n");
			start_call_cipher(s, now);
		} else {
			s->base.main_link.auth_refused = 1;
			(void)dect_cc_release_reason(&s->base.main_link.link.cc,
						     0x09u /* auth failed */);
			printf("call: released -- no key to authenticate this "
			       "handset with\n");
			dect_link_drain(&s->base.main_link.link, now);
			pump_lapc(s);
		}
	}

	/*
	 * A list-access call is ciphered as well. TS 102 527-3 C.7.2 shows the
	 * FP's {CIPHER-REQUEST} right after the LiA {CC-SETUP}, and the PIN
	 * fields of the System Settings List travel only on a ciphered link
	 * (7.4.11.1): without this the PIN, the PIN-guarded delete and the
	 * A540's "Mobilteil abmelden" were refused with "procedure not
	 * allowed" (bench, 2026-10-28). The call stays in PROCEEDING (no
	 * CONNECT on a list call), so this does not wait for the connect.
	 */
	if (!s->reg.cipher_started && (s->base.rec.uak_valid || s->no_cipher) &&
	    !s->base.main_link.link.cc.incoming && dect_cc_busy(&s->base.main_link.link.cc) &&
	    (s->base.main_link.link.cc.peer_basic_service >> 4) == 0x2u /* LiA */) {
		printf("call: list-access call -- ciphering it "
		       "(TS 102 527-3 C.7.2)\n");
		start_call_cipher(s, now);
	}

	/* Never log key material: state only. */
	if (s->base.main_link.link.mm.cipher_state == DECT_MM_CIPHER_READY && !s->base.main_link.cipher_done) {
		s->base.main_link.cipher_done = 1;
		/*
		 * Key material first, then the over-air request: A-0027 Q5 makes
		 * loading the chip and negotiating the cipher distinct steps in
		 * that order. The CIPHER-REQUEST is only queued here; it still has
		 * to travel through LAPC, so the mail below wins the race.
		 */
		if (!s->chip_cipher.disabled)
			send_chip_cipher(s);
		printf("call: cipher: DCK derived, CIPHER-REQUEST sent\n");
	} else if (s->base.main_link.link.mm.cipher_state == DECT_MM_CIPHER_FAILED &&
		   !s->base.main_link.cipher_done) {
		s->base.main_link.cipher_done = 1;
		printf("call: cipher: authentication FAILED (%s)\n",
		       s->base.main_link.link.mm.stats.auth_timeouts ? "no reply in 10 s"
		       : s->base.main_link.link.mm.stats.auth_mismatch ? "WRONG RES"
		       : "the handset refused");
		/*
		 * A wrong RES means the handset is not the one subscribed
		 * under that identity: EN 300 444 §8.23.2.3, "any failure ...
		 * shall result on the interruption of any call in progress".
		 * We let the call go on unciphered (spec sweep, N.9) -- and
		 * so did anyone who named a held IPUI and then ignored or
		 * refused the challenge. With authfirst any failure ends it:
		 * no reply, a refusal or a missing RES as well (A1).
		 */
		if ((s->base.main_link.link.mm.stats.auth_mismatch ||
		     s->audio.auth_first) &&
		    dect_cc_busy(&s->base.main_link.link.cc)) {
			(void)dect_cc_release_reason(&s->base.main_link.link.cc,
						     0x09u /* auth failed */);
			printf("call: released -- the handset failed "
			       "authentication (EN 300 444 8.23.2.3)\n");
		}
	}
	if (s->base.main_link.link.mm.stats.cipher_timeouts && !s->reg.cipher_timeout_seen) {
		s->reg.cipher_timeout_seen = 1;
		printf("call: cipher: no cipher-on (0x0209) 10 s after "
		       "CIPHER-REQUEST -- <MM_cipher.1>, the call is kept\n");
	}
	if (s->base.main_link.link.mm.cipher_rejected && !s->reg.cipher_reject_seen) {
		s->reg.cipher_reject_seen = 1;
		printf("call: cipher: the handset sent CIPHER-REJECT -- the call "
		       "stays in the clear\n");
	}

	pump_lapc(s);
}

/* Route a received mail into the registration stack (when -g). */
static void handle_reg_mail(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	switch (m->prim) {
	case 0x0209:
		/*
		 * {link}: the chip has switched the link to ciphered -- the
		 * cipher-on indication, absent from every unciphered call
		 * [CR A-0128] -- which ends <MM_cipher.1>.
		 */
		if (m->param_len >= 1 && s->base.main_link.active &&
		    m->param[0] == s->base.main_link.handle &&
		    !s->base.main_link.link.mm.cipher_on) {
			dect_mm_cipher_on(&s->base.main_link.link.mm);
			if (s->base.main_link.link.mm.cipher_on)
				printf("call: cipher: the chip reports link %u "
				       "ciphered (0x0209)\n",
				       (unsigned)m->param[0]);
		}
		{
			unsigned k;

			for (k = 0; m->param_len >= 1 && k < SIDE_MAX; k++) {
				struct side_link *sl = &s->side[k];

				if (!sl->bl.active || sl->bl.handle != m->param[0] ||
				    sl->bl.link.mm.cipher_on)
					continue;
				dect_mm_cipher_on(&sl->bl.link.mm);
				if (sl->bl.link.mm.cipher_on)
					printf("side: cipher: the chip reports "
					       "link %u ciphered (0x0209)\n",
					       (unsigned)m->param[0]);
			}
		}
		break;
	case DECT_PRIM_CONTACT_ACK1:	/* 0xF601 carries the base RFPI */
		if (!s->have_rfpi && m->param_len >= DECT_MM_RFPI_LEN) {
			memcpy(s->rfpi, m->param, DECT_MM_RFPI_LEN);
			s->have_rfpi = 1;
		}
		break;
	case DECT_PRIM_CONN_UP: {
		uint8_t h = m->param_len ? m->param[0] : 0;

		/*
		 * The chip can repeat connection-up for a handle that is
		 * already up -- observed in v18, ~150 ms after a call had
		 * bound, which re-armed the C-plane and dropped the call state
		 * underneath a live call.
		 *
		 * Guarding on call_bound alone was not enough. An incoming call
		 * binds audio only when it connects, so between our CC-SETUP
		 * and the user pressing accept the flag is still clear, and a
		 * repeat in that window re-initialised LAPC, MM and CC. Run A2
		 * (2026-10-20) caught it: the chip re-established 1.2 s after
		 * the handset's CC-ALERTING, so the CC-CONNECT two seconds
		 * later arrived at a state machine with no transaction, no
		 * CONNECT-ACK went out, and the handset released the call --
		 * and wedged badly enough to need its battery pulled.
		 *
		 * The re-establishment is the same event that wires the chip's
		 * audio route, so it is expected and must not be disruptive.
		 * Keep the state whenever that handle has a call in any phase;
		 * a different handle, or a genuinely idle one, still starts a
		 * fresh link.
		 */
		/*
		 * A connection-up re-arms the link: reg_start() re-initialises
		 * LAPC, MM and CC. That is right for a genuinely new link and
		 * destructive for one that is in the middle of something, and
		 * until 2026-10-24 the only thing protected was a live call on
		 * the SAME handle.
		 *
		 * With several handsets in range that is not enough. Every
		 * handset that connects -- including one we hold no record for
		 * -- re-armed our link. Measured with three handsets and a
		 * FRITZ!Fon C4 on air: 26-30 connection-ups in 170 s, and an
		 * FP-originated {CC-SETUP} sent onto a link that was re-armed
		 * underneath it, retransmitted unacknowledged, then abandoned.
		 * Nothing answered, and the run read as a refusal.
		 *
		 * So: while this link has work in progress, no connection-up
		 * re-arms it -- least of all one for a different handle.
		 *
		 * Paging is deliberately NOT in the busy set. RING_PAGING is
		 * exactly when we want the handset's connection to come up and
		 * arm the link; it is only from the page response onwards that
		 * a re-arm would lose the call we are placing.
		 */
		{
			int ring_busy = s->ring_state == RING_WAIT_PAGE_RESP;
			int busy = s->base.main_link.active &&
				   (s->reg.call_bound ||
				    dect_cc_busy(&s->base.main_link.link.cc) ||
				    s->base.main_link.link.mm.state == DECT_MM_WAIT_AUTH ||
				    ring_busy);

			if (busy && h == s->base.main_link.handle) {
				printf("reg: connection up (handle %u) repeated "
				       "while this link is busy -- keeping "
				       "state\n", h);
				break;
			}
			/*
			 * A repeat for a side link that carries a call (or holds
			 * a lane): keep it there. reg_start() would take the
			 * handle over and drop the call with its lane (review,
			 * 2026-10-25, finding 1).
			 */
			{
				struct side_link *sl = side_find(s, h);

				if (sl && (sl->lane >= 0 ||
					   dect_cc_busy(&sl->bl.link.cc))) {
					printf("side: connection up (handle %u) "
					       "repeated during its call -- "
					       "keeping its side link\n", h);
					break;
				}
			}
			if (busy && side_start(s, h) == 0)
				break;
			if (busy) {
				s->foreign_conn++;
				printf("reg: connection up (handle %u) IGNORED "
				       "-- handle %u is busy. Another handset "
				       "is competing for the link; re-arming "
				       "now would drop what it is doing.\n",
				       h, s->base.main_link.handle);
				break;
			}
		}
		reg_start(s, h);
		if (s->ring_state == RING_PAGING) {
			s->ring_state = RING_WAIT_PAGE_RESP;
			s->ring_resp_deadline = now + RING_PAGE_RESP_MS;
			printf("ring: link up -- waiting for the handset's "
			       "page response before the CC-SETUP\n");
		}
		break;
	}
	case DECT_PRIM_CONN_DOWN: {
		/*
		 * {handle}. Only the release of OUR connection ends our link.
		 * The chip also reports the release of an older connection
		 * just after the new one is up -- vF418: up on handle 0, then
		 * down for handle 1 eight ms later -- and that used to clear
		 * reg.active, so the live connection's next C-plane segment
		 * was dropped. 102 such releases in the 2026-10-24 corpus.
		 * Found by the external review (the clean-room record/
		 * an external review-24102026.md, finding 10).
		 */
		uint8_t h = m->param_len ? m->param[0] : 0;
		struct side_link *sl = m->param_len ? side_find(s, h) : NULL;

		if (sl && !(s->base.main_link.active && h == s->base.main_link.handle)) {
			xlane_release(s, sl);
			sl->bl.active = 0;
			printf("side: connection released (handle %u)\n", h);
			break;
		}
		if (s->base.main_link.active && m->param_len && h != s->base.main_link.handle) {
			printf("reg: release of handle %u ignored -- our "
			       "connection is handle %u\n", (unsigned)h,
			       (unsigned)s->base.main_link.handle);
			break;
		}
		if (s->base.main_link.active)
			printf("reg: connection released\n");
		s->base.main_link.active = 0;
		hs_switch(s, 0, " (link released)");
		break;
	}
	case DECT_PRIM_SIGNALLING: {
		struct side_link *sl = m->param_len ? side_find(s, m->param[0])
						    : NULL;

		s->led_traffic++;
		if (sl && !(s->base.main_link.active && m->param[0] == s->base.main_link.handle)) {
			side_segment(s, sl, m, now);
			break;
		}
		if (s->base.main_link.active)
			reg_segment(s, m, now);
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
		if (s->reg.call_bound && !s->reg.audio_connected &&
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
		uak_verify_reply(s, m);
		break;
	case 0xF625:
		/* [0] echoes the request's link: the main call or a side one */
		if (m->param_len >= 1 && s->base.main_link.active &&
		    m->param[0] != s->base.main_link.handle) {
			struct side_link *sl = side_find(s, m->param[0]);
			struct dect_diag_status st;

			if (sl && sl->bl.active &&
			    !dect_diag_decode_status(m->param, m->param_len,
						     &st)) {
				sl->last_status = st;
				if (st.has_counters)
					(void)dect_diag_frames_update(
						&sl->frames, &st);
			}
			break;
		}
		if (s->reg.call_bound)
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

	if (!s->reg.call_bound || (!s->verbose && !s->voice_fp))
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
	if (!s->voice_fp || (!s->reg.call_bound && !s->audio.cap_all))
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

/* The 06.88 read-back and slot audit, once their delay is up. */
static void sub_check_tick(struct stubd *s, uint64_t now)
{
	if (!s->sub_check_at || now < s->sub_check_at)
		return;
	s->sub_check_at = 0;
	s->sub_check_firing = 1;
	if (s->sub_check_audit)
		sub_audit_send(s, s->sub_check_mine);
	if (s->sub_check_uak)
		uak_verify_send(s, s->sub_check_slot);
	s->sub_check_audit = s->sub_check_uak = 0;
	s->sub_check_firing = 0;
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
	uint8_t b[sizeof(c) / sizeof(c[0]) * 12u];
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
	if (!t.err)
		(void)cell_send(s, DECT_CELL_STATUS, 0, b, t.len);
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

/* Every mail from the chip, up as it came (§7.1), with the links it opens. */
static void cell_mail_up(struct stubd *s, const struct dect_mail *m,
			 uint64_t now)
{
	struct dect_cell_mail cm = { (uint32_t)(now * 1000u), m->src, m->dst,
				     m->prim, m->param, m->param_len };
	uint8_t b[9u + DECT_CELL_MAIL_PARAM_MAX];
	size_t n = dect_cell_mail_up(b, sizeof(b), &cm);
	uint8_t h = m->param_len ? m->param[0] : 0xFFu;

	if (h < 8u && m->prim == DECT_PRIM_CONN_UP)
		s->cell.handles |= (uint8_t)(1u << h);
	else if (h < 8u && m->prim == DECT_PRIM_CONN_DOWN)
		s->cell.handles &= (uint8_t)~(1u << h);
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

	if (dect_cell_mail_parse(f->body, f->len, 0, &cm)) {
		cell_reject(s, f->corr, DECT_CELL_REJ_CHIP_REFUSED);
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
	uint8_t b[16];
	struct dect_cell_tlv t;

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
	case DECT_CELL_MEDIA_OPEN: {
		/* media is not built yet (§14.3): status 4 */
		uint32_t id = 0;

		if (!dect_cell_tlv_check(f.body, f.len))
			(void)dect_cell_tlv_get_u32(f.body, f.len, 1, &id);
		dect_cell_tlv_init(&t, b, sizeof(b));
		dect_cell_tlv_u32(&t, 1, id);
		dect_cell_tlv_u16(&t, 2, 4);
		(void)cell_send(s, DECT_CELL_MEDIA_OPEN_ACK, f.corr, b, t.len);
		break;
	}
	default:
		dect_cell_tlv_init(&t, b, sizeof(b));
		dect_cell_tlv_u16(&t, 1, DECT_CELL_ERR_UNSUPPORTED);
		dect_cell_tlv_u16(&t, 2, f.type);
		dect_cell_tlv_u16(&t, 3, f.corr);
		(void)cell_send(s, DECT_CELL_ERROR, 0, b, t.len);
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
		"\"not_forwarded\":%u,\"queue_bytes\":%zu,\"queue_drops\":%u}\n",
		s->cell.cfd >= 0 ? "true" : "false",
		s->cell.lost_at ? "false" : "true",
		(unsigned)__builtin_popcount(s->cell.handles), s->cell.up,
		s->cell.down, s->cell.rejected, s->cell.lost_up, s->txq_len,
		s->txq_drops);
	if (fclose(f) == 0)
		(void)rename(tmp, CELL_STATE_PATH);
}

static void cell_tick(struct stubd *s, uint64_t now)
{
	uint8_t buf[DECT_CELL_FRAME_MAX];
	int c;
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
		printf("cell: dect-cell connected\n");
		cell_status(s, now);
	}
	while (s->cell.cfd >= 0) {
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
		printf("cell: dect-cell went away\n");
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
	led_tick(s, now, s->do_register && s->sess.state == DECT_SESS_UP);
	state_write(s, now, s->do_register &&
		    s->sess.state == DECT_SESS_UP, 0);
	holdback_tick(s, now);
	sub_check_tick(s, now);
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
			if (s->side[k].bl.active)
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
			if (!s->side[k].bl.active)
				continue;
			dect_link_tick(&s->side[k].bl.link, now);
			lapc_failed_note(&s->side[k].bl.link, "side");
			cap_tick(s, &s->side[k].bl.link);
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
		lapc_failed_note(&s->base.main_link.link, "main");
		cap_tick(s, &s->base.main_link.link);
		names_tick(s);		/* a handset renamed one */
		/*
		 * The call has gone away: release the audio channel.
		 * Without this it stays armed for the life of the link
		 * and the next call starts on top of the last one's
		 * state.
		 */
		if (s->reg.call_bound && !dect_cc_busy(&s->base.main_link.link.cc)) {
			s->reg.call_bound = 0;
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
					if (s->do_register)
						handle_reg_mail(s, &m, now);
					else if (s->cell_mode) {
						cell_mail_up(s, &m, now);
						/* our own park's read-back */
						if (m.prim == 0xFFF1u &&
						    s->park_check_at)
							uak_verify_reply(s, &m);
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
	"clip", "memdump", "timesync", "codeclist", "infoie",
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
	s.reg_open = s.audio.reg_open;
	s.reg_open_until = s.reg_open && s.audio.reg_window_s ?
			   mono_ms() + s.audio.reg_window_s * 1000ull : 0;
	if (!s.audio.basic_svc_set)
		s.audio.basic_svc = s.audio.narrowband ? RING_BASIC_SVC
						       : RING_BASIC_SVC_WB;

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
			s.hs[i].loaded = sub_load(&s);
			dect_base_stash(&s.base);
			/*
			 * Only a store that does not exist yet may take an
			 * enrolment. One that exists but did not load (wrong
			 * size or magic, a slot clash) holds someone's record;
			 * routing a new handset there would overwrite it
			 * (review, finding 9).
			 */
			s.hs[i].absent = s.hs[i].path &&
				stat(s.hs[i].path, &sst) != 0 && errno == ENOENT;
			for (j = 0; s.hs[i].loaded && j < i; j++) {
				if (!s.hs[j].loaded ||
				    s.base.hs[j].slot != s.base.hs[i].slot)
					continue;
				printf("sub: %s claims slot %u, which %s "
				       "holds -- not loaded\n", s.hs[i].path,
				       s.base.hs[i].slot, s.hs[j].path);
				s.hs[i].loaded = 0;
			}
		}
		/* remote=: one more store, marked as a roamer (test M4) */
		if (s.audio.remote_path && s.base.n_hs < DECT_MM_MAX_SLOT) {
			i = s.base.n_hs++;
			s.hs[i].path = s.audio.remote_path;
			s.base.cur = i;
			hs_load(&s, i);
			s.hs[i].loaded = sub_load(&s);
			dect_base_stash(&s.base);
			s.hs[i].remote = 1;
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
		if (!s.hs[0].loaded && !s.audio.sub_slot) {
			unsigned used = 0, slot;

			for (i = 1; i < s.base.n_hs; i++)
				if (s.hs[i].loaded)
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

			if (s.hs[i].loaded || s.hs[i].remote ||
			    !s.hs[i].absent)
				continue;
			for (k = 0; k < s.base.n_hs; k++) {
				unsigned ks = k == 0 && s.audio.sub_slot ?
					      s.audio.sub_slot : s.base.hs[k].slot;

				if (k != i && (s.hs[k].loaded ||
				    (k < i && !s.hs[k].remote)))
					used |= 1u << ks;
			}
			for (slot = 1; slot <= DECT_MM_MAX_SLOT &&
			     (used & (1u << slot)); slot++)
				;
			if (slot > DECT_MM_MAX_SLOT) {
				printf("sub: no chip slot left for %s; it cannot "
				       "take an enrolment\n", s.hs[i].path);
				s.hs[i].remote = 1;	/* never a free store */
				continue;
			}
			s.base.hs[i].slot = slot;
			printf("sub: %s is free for an enrolment, chip slot %u\n",
			       s.hs[i].path, slot);
		}
		if (!s.hs[0].loaded && s.audio.sub_slot) {
			for (i = 1; i < s.base.n_hs; i++)
				if (s.hs[i].loaded &&
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

	dect_clip_init(&s.clip);
	if (s.audio.clip_number
	    && dect_clip_set_number(&s.clip, s.audio.clip_number,
				    DECT_CLIP_TON_UNKNOWN, DECT_CLIP_NPI_E164,
				    DECT_CLIP_PRES_ALLOWED,
				    DECT_CLIP_SCR_USER_UNSCREENED)) {
		fprintf(stderr, "clip: number rejected (printable IA5, "
			"at most %u bytes)\n", DECT_CLIP_NUMBER_MAX);
		return 1;
	}
	if (s.audio.clip_name
	    && dect_clip_set_name(&s.clip, s.audio.clip_name,
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
		 * answered only 30 to 60 s after its upload (2026-10-29), so
		 * the SSC transport makes ten, ~7 s each.
		 */
		int pass;

		passes = s.ssc ? 10 : 3;
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

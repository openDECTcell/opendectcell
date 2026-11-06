/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dect-stubd: what the source files of the daemon share (src/stubd/README.md).
 *
 * Types, constants and the functions and variables that more than one file uses.
 * What only one file uses stays `static` in that file.
 */
#ifndef STUBD_INTERNAL_H
#define STUBD_INTERNAL_H

#include <ctype.h>

#include <errno.h>

#include <sys/random.h>

#include <fcntl.h>

#include <grp.h>

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

#include "dect_g722.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u

#define POLL_MS		100

#define DECT_CTL_PATH	"/var/run/opendectcell.sock"
/* End-of-run park: how long to wait for the queue, then for the driver. */
#define PARK_DRAIN_MS	2000u

#define PARK_FLUSH_MS	200u

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
	/*
	 * How loud each direction was, for the log at the end of a call: the frames that carried signal
	 * (a sample above AB_SIGNAL) and the highest sample, in the lane's own PCM before any resampling.
	 * It tells a silent direction from a lost one (the O201 test, 2026-11-05). Narrowband and linear
	 * wideband only: G.722 octets passed through are not looked at.
	 */
	uint32_t up_sig, down_sig;
	int16_t  up_peak, down_peak;
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
		 * docs/journals/call-g7.md stay reproducible -- `flags` is the mode
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
		const char *sock_group;	/* sockgroup=NAME: the bridge sockets belong to this group, 0660 */
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
		 * the link it brings up. Milestone B; see docs/journals/call-g7.md.
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

#define VERIFY_TBL_N	(sizeof(verify_tbl) / sizeof(verify_tbl[0]))

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

/*
 * One word from the handset; every 20 ms a frame to the bridge in the
 * socket's format. Nothing is decoded while no bridge is connected.
 */
#define AB_SIGNAL 32	/* a sample above this (about -60 dBFS) counts as signal */

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

#define SSC_N_DESC	16u	/* 64 ms of blocks queued for read() */


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

#define PARK_0688_RADIO		0x010Au

#define PARK_0688_TIMER		0x08B0u

#define PARK_CHECK_MS		1000u

/*
 * Page the handset so it brings up a link we can send a CC-SETUP on.
 *
 * 0x0300 carries {page info, 0x11, slot}. Our LOCATE-ACCEPT packs the TPUI as
 * {0, 0x11, slot}, so the last two octets are also its low 16 bits -- but note
 * that in stock the 0x11 is a **hardcoded constant** in the stock paging routine
 *, not a byte taken from the identity [CR A-StockTrace-5 §21].
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

/*
 * The state file: what the base is doing, for LuCI or anything else that
 * wants it without speaking a protocol (ROADMAP M9, docs/diagnostics.md).
 * One JSON object, rewritten atomically (temporary file, rename) at most
 * once a second, on /tmp, which is RAM. IPUIs only with -p: the file is
 * meant to be read by a web page.
 */
#define STATE_DEFAULT_PATH	DECT_RUN_DIR "/state.json"

#define STATE_EVERY_MS		1000u

/*
 * The call lists over a restart of the daemon, in a file in RAM (a reboot
 * empties them): read at the start, written once a second when the text
 * differs from the last one written, and when the daemon ends. Format in
 * dect_calls.h; numbers and names, so only root reads it.
 */
#define CALLS_PATH	DECT_RUN_DIR "/calls"

#define CALLS_TEXT_MAX	(3u * DECT_CALLS_MAX * 230u + 32u)

#define PROFILE_DIR_DEFAULT	"/etc/opendectcell/profiles"

/*
 * A handset's own settings, from the DECT settings page (Network > DECT,
 * Handsets, General): "key = value" lines in /etc/opendectcell/handset<N>.override,
 * written by the init script from /etc/config/opendectcell, on top of the
 * chosen profile.
 */
#define OVERRIDE_DIR	"/etc/opendectcell"

#define DEREG_IDLE	DECT_BASE_DEREG_IDLE

#define DEREG_PAGING	DECT_BASE_DEREG_PAGING

#define DEREG_REQUESTED	DECT_BASE_DEREG_REQUESTED

#define DEREG_REQ_PATH	DECT_RUN_DIR "/deregister"

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

/* ---- defined in dect-stubd.c ---- */
extern volatile sig_atomic_t stop;
extern volatile sig_atomic_t want_register;
extern uint64_t g_start;
extern volatile sig_atomic_t want_deregister;
extern volatile sig_atomic_t want_profiles;
extern uint32_t sub_uak_base;
void clock_any(struct dect_time *dt);
int clock_now(uint8_t mode, struct dect_time *dt, long *off_s);
void on_signal(int sig);
uint64_t mono_ms(void);
FILE *fopen_private(const char *path);

/* ---- defined in stubd_link.c ---- */
FILE *fopen_root_only(const char *path);
int stub_build_read(char *name, size_t cap);
void log_mail(const struct dect_mail *m, const char *dir, int show_params);
void verify_report(const struct stubd *s);
int pcm_block_ok(const struct stubd *s, const void *hdr);
void ssc_rx_fix(struct stubd *s, uint8_t *fr, size_t len);
void lanes_str(char *out, size_t cap, uint64_t mask);
void ssc_watch(struct stubd *s, const uint8_t *raw, size_t len, uint64_t now);
ssize_t pcm_write(struct stubd *s, const uint8_t *frames, size_t len, int burst);
int send_mail(struct stubd *s, unsigned int tx_slot, const struct dect_mail *m);
void chip_mail_to(struct stubd *s, uint8_t dst, uint16_t prim, const uint8_t *p, uint16_t n);
void chip_mail(struct stubd *s, uint16_t prim, const uint8_t *p, uint16_t n);
void chip_mem_read(struct stubd *s, uint32_t addr, uint16_t n);
void chip_assign(struct stubd *s, uint8_t handle, uint8_t flags, uint16_t mask, uint8_t mode, uint8_t codec);
int lane_word_live(uint32_t v);
int parse_mail_spec(const char *spec, struct dect_mail *m);
int send_mail_spec(struct stubd *s, const char *spec);
int find_control_slot(struct stubd *s, unsigned int fpb);
void ssc_flip(struct stubd *s);
int bring_up(struct stubd *s, struct dect_pcm_config *cfg);
void tear_down(struct stubd *s);

/* ---- defined in stubd_audio.c ---- */
void ab_frame(struct ab_lane *l, uint8_t *fr, unsigned int w, const uint8_t *src);
void tx_templates(const struct stubd *s, uint8_t *t);
void tx_block(struct stubd *s, unsigned int fpb, uint8_t *frames, const uint8_t *rx);
void ab_init(struct stubd *s);
int sock_perms(const struct stubd *s, const char *path);
void ab_listen(struct stubd *s);
void ab_open(struct stubd *s);
void ab_end(struct ab_lane *l, const char *why);
void ab_close(struct stubd *s);
void ab_tick(struct stubd *s, uint64_t now);

/* ---- defined in stubd_host.c ---- */
extern const struct dect_base_ops base_ops;
uint8_t stubd_sub_slot(const struct stubd *s);
void hs_load(struct stubd *s, unsigned i);
int sub_load(struct stubd *s);
void fetch_tick(struct stubd *s, uint64_t now);
void cap_load(struct stubd *s, unsigned i);
void cap_tick(struct stubd *s, struct dect_link *l);
void lapc_failed_note(struct stubd *s, struct dect_link *l, const char *which);
void nlr_tick(struct stubd *s, uint64_t now);
void info_tick(struct stubd *s, uint64_t now);
int call_auth_ok(const struct stubd *s, const struct dect_link *l);
void dial_tick(struct stubd *s, uint64_t now);
void connect_tick(struct stubd *s, uint64_t now);
void holdback_tick(struct stubd *s, uint64_t now);
int any_uak(const struct stubd *s);
void send_subscription(struct stubd *s);
void send_subscription_tail(struct stubd *s);
void park_record(const char *how);
int park_check_start(struct stubd *s, uint64_t now);
int park_check_done(struct stubd *s, uint64_t now);
void mem_reply(struct stubd *s, const struct dect_mail *m);

/* ---- defined in stubd_calls.c ---- */
void send_audio_disconnect(struct stubd *s, uint8_t handle);
void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan);
void audio_connect_fallback(struct stubd *s, uint64_t now);
void remode_tick(struct stubd *s, uint64_t now);
void seq_tick(struct stubd *s, uint64_t now);
void mem_dump_tick(struct stubd *s, uint64_t now);
void send_conn_status_req(struct stubd *s, uint64_t now);
void log_nwk_out(void *ctx, const uint8_t *msg, size_t len);
uint8_t target_svc(const struct stubd *s, int i, uint8_t configured);
void cs_connect_tick(struct stubd *s, uint64_t now);
void ring_tick(struct stubd *s, uint64_t now);
void report_conn_status(struct stubd *s, const struct dect_mail *m);
struct side_link *side_find(struct stubd *s, uint8_t handle);
void log_nwk_out_side(void *ctx, const uint8_t *msg, size_t len);
int lanes_available(const struct stubd *s, const struct side_link *me);
struct xlane *xlane_of(struct stubd *s, uint8_t handle);
void xlane_connect(struct stubd *s, struct xlane *x);
void xlane_release(struct stubd *s, struct side_link *sl);
void side_call_tick(struct stubd *s, struct side_link *sl, uint64_t now);
struct dect_cc *ic_cc(struct stubd *s, int side);
int ic_active(const struct stubd *s, int p);
void base_forget(void *ctx, unsigned i, const char *why);
void base_mask(void *ctx);
void dereg_tick(struct stubd *s, uint64_t now);
void base_wall(void *ctx, struct dect_time *dt);
int base_clock(void *ctx, struct dect_time *dt, long *off_s);
void time_tick(struct stubd *s, uint64_t now, int radio_on);
void ic_tick(struct stubd *s, uint64_t now);
int ic_word(const struct stubd *s, int side);
void side_pump(struct stubd *s, struct side_link *sl);
void side_cipher_tick(struct stubd *s, struct side_link *sl, uint64_t now);
void base_registered(void *ctx, uint64_t now);
void base_call_up(void *ctx, uint64_t now);
void base_side_call(void *ctx, unsigned k, uint64_t now);
void base_call_down(void *ctx);
void base_swapped(void *ctx, unsigned k);

/* ---- defined in stubd_handsets.c ---- */
extern struct dect_contacts stubd_contacts;
extern struct dect_calls stubd_calls;
void led_write(struct stubd *s, int on);
void led_open(struct stubd *s);
void led_tick(struct stubd *s, uint64_t now, int radio_on);
void json_str(FILE *f, const char *v);
void state_write(struct stubd *s, uint64_t now, int radio_on, int force);
void pump_lapc(struct stubd *s);
void reg_window(struct stubd *s, int open, unsigned secs, uint64_t now, const char *why);
void reg_window_tick(struct stubd *s, uint64_t now);
void base_link_up(void *ctx, int p);
void base_side_down(void *ctx, unsigned k);
unsigned hs_slot(const struct stubd *s, unsigned i);
void hs_name(const struct stubd *s, unsigned i, char *buf, size_t cap);
void name_path(const struct stubd *s, unsigned i, char *buf, size_t cap);
void contacts_tick(struct stubd *s, uint64_t now);
void calls_tick(uint64_t now, int force);
void contacts_init(struct stubd *s);
void name_load(struct stubd *s, unsigned i);
void name_save(const struct stubd *s, unsigned i);
void reg_profile_apply(struct stubd *s);
int reg_profile_ok(const struct stubd *s, const char *id);
void profiles_resolve_all(struct stubd *s, const char *why);
void missed_changed_tick(struct stubd *s);
void base_named(void *ctx, unsigned i);
void names_tick(struct stubd *s);

/* ---- defined in stubd_ctl.c ---- */
void ctl_send_bridge(struct stubd *s, struct dect_ctl_out *o);
void ctl_hs(struct stubd *s, unsigned i, const char *state);
int bridge_call_out(struct stubd *s, int p, uint64_t now);
struct dect_base_call *bc_active(struct stubd *s, int p);
void bridge_tick(struct stubd *s, uint64_t now);
void ctl_open(struct stubd *s);
void ctl_close(struct stubd *s);
void ctl_tick(struct stubd *s, uint64_t now);

/* ---- defined in stubd_cell.c ---- */
void cell_open(struct stubd *s);
void cell_close(struct stubd *s);
void cell_status(struct stubd *s, uint64_t now);
void cell_media_tx(struct stubd *s, uint8_t *fr, const uint8_t *in);
void cell_mail_up(struct stubd *s, const struct dect_mail *m, uint64_t now);
void cell_tick(struct stubd *s, uint64_t now);
int cell_rfpi_note(struct stubd *s, const struct dect_mail *m);
void rfpi_check(const struct stubd *s, const struct dect_mail *m);

/* ---- defined in stubd_opts.c ---- */
int parse_audio(struct audio_cfg *a, const char *spec);
int parse_chip_cipher(struct chip_cipher *cc, const char *spec);
void usage(void);
int profile_load(const char *path, struct dect_profile *p);
int ctl_client(int n, char **lines);
int bridge_echo(int argc, char **argv);
int check_profile(const char *path, const char *fw);

#endif /* STUBD_INTERNAL_H */

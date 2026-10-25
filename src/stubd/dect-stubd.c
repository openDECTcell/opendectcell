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
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include "opendectcell.h"
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
#include "dect_callinfo.h"
#include "dect_clip.h"

/* Longest -a loopdelay ring, in frames (1 s at 8 kHz). */
/* How long to wait for a lingering link to clear before ringing anyway. */

#define LOOP_DELAY_MAX 8000u

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
 */
#define TIMESYNC_AUTO	0u
#define TIMESYNC_FORCE	1u
#define TIMESYNC_OFF	2u

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
	if (adjtimex(&tx) < 0)
		return 0;		/* cannot tell: assume not */
	return (tx.status & STA_UNSYNC) == 0;
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
#include "dect_g722.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u
#define POLL_MS		100
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

static uint64_t mono_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
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
	unsigned slot;
	uint8_t  uak[DECT_MM_UAK_LEN];
	int      uak_valid;
	uint8_t  port_id[DECT_MM_PORTID_MAX];
	size_t   port_id_len;
	uint8_t  cc_port_id[DECT_CC_ID_MAX];
	size_t   cc_port_id_len;
	uint8_t  cc_fixed_id[DECT_CC_ID_MAX];
	size_t   cc_fixed_id_len;
	int      cc_id_learned;
	uint64_t last_seen_ms;	/* runtime only: last segment from it */
	int      remote;	/* remote=: a roamer the cell must fetch */
	int      absent;	/* the file does not exist: free for enrolment */
	int      fetched;	/* ... and has, this run */
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

struct side_link {
	int      active;
	uint8_t  handle;
	int      adopted;	/* handset table index, -1 = nobody yet */
	uint32_t rejected_seen;
	int      lane;		/* -a twocalls: its xlane, -1 = none */
	int      ic_tried;	/* this call was checked for internal */
	size_t   dial_len;	/* overlap receiving, as on the main link */
	uint64_t dial_change_ms;
	struct dect_link link;
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
	uint8_t  ring[LOOP_DELAY_MAX * 2u];
	unsigned wr;
	int      primed;
};

struct stubd {
	int fd;
	unsigned int slots;
	unsigned int fpb;		/* frames per DMA block (32 = 4 ms) */
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
	} ic;
	/* The registration window: open now, and until when (0 = no limit). */
	int      reg_open;
	uint64_t reg_open_until;
	unsigned int n_xmail;
	unsigned int xmail_idx;	/* next extra mail to send (one per loop tick) */
	/*
	 * Mails to send once a CALL is bound, rather than once the session is
	 * up (-N). The DSP probes need this: a cross-connect naming a wideband
	 * channel is meaningless until the chip has allocated one for a call.
	 * Re-armed on every call, so a sweep can be repeated by redialling.
	 */
	const char *cmail[20];
	unsigned int n_cmail;
	unsigned int cmail_idx;
	struct dect_rx rx;
	struct dect_session sess;

	/* GAP registration (-g): the host-side C-plane stack per connection. */
	int do_register;
	const char *pin;	/* registration PIN / AC */
	int rand_fd;		/* /dev/urandom, -1 if unavailable */
	uint8_t rfpi[DECT_MM_RFPI_LEN];
	int have_rfpi;
	/*
	 * The handset's long-term key, kept across connections: registration and a
	 * later call run on different connections (each re-inits the MM FSM), and
	 * call-time ciphering needs the UAK. Persisted in the subscription store
	 * (-s) so a call after a daemon restart needs no fresh enrolment.
	 */
	uint8_t uak[DECT_MM_UAK_LEN];
	int uak_valid;
	const char *sub_path;	/* host-side subscription store (holds the UAK) */
	struct handset hs[DECT_MM_MAX_SLOT];	/* every -s store, [0] = ring target */
	unsigned n_hs;
	unsigned cur;		/* which of them the fields here belong to */
	/* Identities for an FP-originated SETUP, learned and persisted. */
	/*
	 * The subscribed handset's MM <<PORTABLE-IDENTITY>>, restored from the
	 * store. reg_start() pushes it into the MM state machine so a location
	 * registration from a handset we do not hold can be rejected -- the
	 * chip admits anything locked to our RFPI, so this is the only gate.
	 */
	uint8_t     sub_port_id[DECT_MM_PORTID_MAX];
	size_t      sub_port_id_len;
	uint8_t     cc_port_id[DECT_CC_ID_MAX];
	size_t      cc_port_id_len;
	uint8_t     cc_fixed_id[DECT_CC_ID_MAX];
	size_t      cc_fixed_id_len;
	/*
	 * 1 once the pair came from a call the handset actually placed.
	 * A derived pair is good enough to ring with; a measured one is
	 * still better, so it is allowed to overwrite a derived one.
	 */
	int         cc_id_learned;
	int         cc_id_foreign_warned;
	int         cc_id_unarmed_warned;
	/* rejected_identity of this connection's CC, as last reported */
	uint32_t    cc_rejected_seen;
	unsigned    page_rejects;	/* {LCE-PAGE-REJECT}s sent */
	struct side_link side[SIDE_MAX];
	unsigned    side_started;
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
	       RING_SETUP_DELAY, RING_DONE } ring_state;
	uint64_t ring_setup_at;		/* when setupdelay=N expires */
	uint64_t ring_release_until;	/* give the link time to go down */
	uint64_t ring_resp_deadline;	/* fall back if no page response comes */
	unsigned ring_setup_tries;	/* refused CC-SETUPs, see ring_queue_setup */
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
	 */
	unsigned sub_slot;
	/*
	 * Experimental chip cipher-enable (0x0208). Its field layout is only
	 * statically derived and the key offsets are unproven [CR A-0027 Q1], so
	 * it is never sent unless -k asks for a specific selector and layout.
	 */
	struct chip_cipher {
		int     enabled;
		uint8_t selector;
		int     use_rs;		/* 1 = RS in the input field, else RAND_F */
		int     no_reverse;	/* 1 = send DCK/RES unreversed */
		int     long_form;	/* 1 = 33-byte mail variant */
	} chip_cipher;
	/* Voice-slot probe: which TDM slot carries a call's audio (mapping open). */
	uint16_t vs_last[8];
	uint32_t vs_changes[8];
	uint32_t vs_frames;
	/*
	 * Uplink hold statistics for the bearer lane (tone_word): how many
	 * live words were held longer than the nominal two frames in the
	 * last second, how many extra frames that cost, and how many of
	 * those holds started at the same position within the 80-frame
	 * DECT frame as the previous one -- the frame-locked signature
	 * that codec 0 showed in v22 and codec 3 did not in C4.
	 */
	uint32_t vh_run;		/* frames the current word has been held */
	uint32_t vh_holds, vh_extra, vh_locked;
	uint32_t vh_frame;		/* running frame index since bind */
	uint32_t vh_last_phase;
	/*
	 * Voice capture (-w): dump the call's TDM slot raw, two bytes per frame,
	 * so the sample format can be worked out offline. The slot is measured,
	 * the encoding inside it is not: 8000 frames/s x 2 bytes could be 16-bit
	 * linear at 8 kHz, A-law at 16 kHz, or A-law in one byte of each pair.
	 */
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
	uint32_t    tx_write_short;	/* block writes the driver did not take whole */
	int         parked;		/* run_loop() drained the park mails */
	uint8_t     tx_idle;		/* filler for unmapped slots (-a txidle) */
	uint8_t     tx_voice_idle;	/* filler for the voice lanes */
	uint32_t    tx_blocks;
	unsigned    tone_phase;		/* index into tone_sine, advanced per frame */
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
		int      no_gain;	/* skip 0x0251 entirely */
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
		int      send_f626;	/* also send 0xF626 (absent from AVM's bind) */
		/*
		 * nbcodec: advertise only the full-slot codec in <<CODEC-LIST>>,
		 * dropping the G.722 entry that asks for a long slot. Tests
		 * whether the A540's RELEASE-REASON 0x32 is a long-slot bearer
		 * it cannot allocate (docs/tasks-2026-10-22.md T3).
		 */
		int      nbcodec;
		int      nowait;	/* send 0x022D immediately (old behaviour) */
		/*
		 * remode=MS: MS ms after 0x022D, send a SECOND 0x0230 for the
		 * same handle and channel with flag bit 6 CLEAR.
		 *
		 * [CR A-0039]: the chip's channel configurator picks its mode
		 * from the assign's flags and the path state. A first 0x0230,
		 * and the 0x022D connect, both run mode 1 -- which routes PCM
		 * out(k) from the silence constant, parks PCM in(k) in a sink,
		 * and writes a skip over the channel's transmit chain, i.e.
		 * disables it. Our whole sequence runs mode 1 twice, so nothing
		 * we write to the lane can reach the air and nothing from the
		 * air reaches us through the matrix. That is the decoupling
		 * measured on 2026-10-23 (3.8 dB of envelope range against the
		 * source's 52.9 dB, no correlation at any offset).
		 *
		 * A second 0x0230 with bit 6 clear while the path is already on
		 * selects mode 2: PCM out(k) <- channel k, channel k <- PCM
		 * in(k), and a NOP over the chain, i.e. enabled.
		 */
		int nocodeclist;	/* omit <<CODEC-LIST>> from CC-SETUP */
		/*
		 * earlybind: assign the audio channel the moment the handset
		 * answers the page, BEFORE the {CC-SETUP}, instead of waiting
		 * for {CC-CONNECT}.
		 *
		 * COMMAND_SET §9.2 puts the audio assignment straight after
		 * "radio connection up", and [CR A-StockTrace-5 §15] measured a
		 * connection bearer on stock ~360 ms BEFORE its {CC-SETUP}. We
		 * assign on {CC-CONNECT}, which for an incoming call never
		 * arrives -- so the handset is offered speech on a connection
		 * that has no U-plane, and "insufficient resources" is the
		 * honest answer. The chip says as much: a page-response
		 * connection reports 0x0200 {handle, 0} / 0x0242 {handle, 0x00}
		 * where a handset-originated one reports {handle, 1} /
		 * {handle, 0x40}.
		 */
		int earlybind;
		/*
		 * nostockcfg: omit the three config mails stock sends before
		 * radio-on. The control for the bring-up-gap test.
		 */
		int no_stock_cfg;
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
		 * slotmask=N / noslotcfg: the traffic-bearer slot-availability
		 * mask the chip searches when allocating a bearer for an
		 * INCOMING call. [CR A-0114] found THE cause of GAP handsets
		 * refusing with 0x32: the chip inits 0xdb3 = 0x3f (all six slots
		 * marked in-use) and the CC state machine's free-slot search
		 * finds nothing, so no bearer slot is allocated and the handset
		 * releases "insufficient bearers available". Stock sends
		 * 0x017c 00 09 at attach -> 0xdb3 = 0x09 (keeps slots 0 and 3
		 * reserved, frees 1,2,4,5). We never sent 0x017c at all.
		 *
		 * 0xdb3 is global, so sending it once per subscription load is
		 * enough. Default 0x09 matches stock exactly; do NOT use 0x00
		 * (frees the reserved dummy-bearer/scan slots). noslotcfg omits
		 * the mail entirely (the pre-A-0114 behaviour, as a control).
		 * The C4 does not depend on it -- CAT-iq opens its own bearer
		 * PP-side -- which is why it rang without this and the three
		 * Gigasets did not.
		 */
		uint8_t  slot_mask;
		int      no_slot_cfg;
		/*
		 * setupdelay=N: wait N ms after the page response before the
		 * {CC-SETUP}. [CR A-StockTrace-5 §15] measured a connection
		 * bearer on stock ~360 ms before its setup; we send ours
		 * ~120 ms after the connection comes up, which may simply be
		 * before the MAC has finished bringing the bearer up.
		 */
		unsigned setup_delay_ms;
		/*
		 * codeclist=HEX replaces the <<CODEC-LIST>> contents (the IE
		 * value, without tag or length). EN 300 175-5 §7.7.54: one
		 * negotiation octet then 3 octets per codec
		 * {identifier, direction+MAC/DLC service, routing+slot size}.
		 */
		uint8_t  codeclist[31];
		size_t   codeclist_len;
		unsigned xmail_ms;	/* hold -M mails until this many ms in */
		unsigned remode_ms;
		const char *led_path;	/* led=PATH, NULL = the default */
		const char *state_path;	/* statefile=PATH, NULL = the default */
		int      no_state;	/* nostatefile */
		/*
		 * Registration closed unless regopen (since 2026-10-25, the
		 * maintainer: "we don't want anything to register unless we
		 * tell it to"). noregopen is still accepted, and is the default.
		 */
		int      no_reg_open;
		unsigned reg_window_s;	/* regopen=SECS: close after; 0 = never */
		int      two_calls;	/* twocalls: speech on side links */
		uint8_t  lane_ch[XLANE_MAX], lane_word[XLANE_MAX];
		unsigned reg_signal_s;	/* regwindow=SECS for SIGUSR1, 120 */
		unsigned holdback_ms;	/* holdback=MS, multi-cell test M2 */
		int      no_chip_keys;	/* nochipkeys, multi-cell test M2 */
		const char *remote_path;	/* remote=PATH, test M4 */
		unsigned fetch_delay_ms;	/* fetchdelay=MS, test M4 */
		unsigned locate_validity;	/* locvalid=N x 40.96 s, M4 */
		int      no_led;	/* noled */
		int      remode_set;	/* remode= or noremode given */
		/*
		 * remodebit6 keeps flag bit 6 SET in the re-assign, which asks
		 * for mode 9 rather than mode 2: the channel's codec-side input
		 * variant B (dest 11+k-1) instead of variant A (dest k).
		 *
		 * [CR A-0039] marks variant A as the likely G.726 encoder input
		 * and variant B as the likely transparent (already-coded) path.
		 * A wideband call cannot use the encoder: [CR A-DSP-8] found no
		 * G.722 encoder in either STUB build, so wideband downlink audio
		 * has to arrive on the lane already coded. Measured 2026-10-23:
		 * with variant A routed and a G.722 lane we got white noise,
		 * which is what an encoder fed coded octets would produce.
		 */
		int      remode_bit6;
		/*
		 * remodecodec=N overrides the codec byte in the re-assign only.
		 * [CR A-0040] §5: with bit 6 set and the path on, the codec byte
		 * picks the mode -- 1 gives mode 8, 2-3 give mode 9. Mode 8 is
		 * the only mode that configures the 80-byte B-field our long-slot
		 * bearer actually carries: transparent downlink on, uplink
		 * deliberately off, enable word 0. A-0033 rejected codec 1
		 * because the UPLINK lane stayed 0x0000, which is what mode 8
		 * does by design, and never checked the downlink acoustically.
		 */
		int      remode_codec;	/* -1 = keep a->codec */
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
		 * noconnect: never send 0x022D. The stock connect sequence is
		 * exactly 0x0251 then 0x0230 [CR A-0034]; 0x022D is the stock
		 * mute control (0x80 = mute), never sent at connect. Whether
		 * the chip reads more than bit 7 of its byte is not
		 * established, so omitting it is the clean comparison.
		 */
		int      no_connect;
		unsigned tone_hz;	/* transmit test tone (0 = off); multiple of 125 */
		uint8_t  tone_word;	/* raw DMA word to write it into (default 0) */
		/*
		 * playslot=MS walks the lane across every transmit slot, MS ms
		 * each, during a call. Which transmit slot the chip reads as the
		 * downlink bearer is not established: the v22 capture shows the
		 * bearer in slot 0 of the chip's OWN frames, which says nothing
		 * about the direction we write. On 2026-10-23 a correctly rated
		 * G.722 lane in slot 0 produced handset audio with 3.8 dB of
		 * envelope range against the source's 52.9 dB, and no envelope
		 * correlation at any offset -- the chip was not reading our data
		 * at all, which is a slot question, not a format question.
		 */
		unsigned play_slot_ms;
		int      tone_high;	/* high byte lane instead of low */
		int      tone16;	/* 16-bit linear in the whole word (default profile) */
		int      tone16be;	/* ... big-endian within the word */
		/*
		 * GenDSP routes to establish once the call's audio is connected:
		 * 0xFFF1 sub 0x0A {dst, src} to task 0x23 [CR COMMAND_SET §7.1,
		 * docs/stub-pcm-control-channel.md]. dst/src: ADPCM 0..3, PCM 4..6,
		 * wideband 7..9; src may also be silence 12 or test tone 15.
		 * Measured at G4: PCM 6 drives raw word 0, PCM 4 drives raw word 5.
		 */
		uint8_t  dsp_dst[8], dsp_src[8];
		unsigned dsp_n;
		uint8_t  tx_idle;	/* transmit filler for unmapped slots */
		int      have_tx_idle;
		uint8_t  tx_voice_idle;	/* transmit filler for the voice lanes */
		int      have_voice_idle;
		unsigned status_ms;	/* poll 0xF624 connection status (0 = off) */
		int      cap_all;	/* -w captures from start-up, not just calls */
		/*
		 * dspsweep=DST[:MS]: during a bound call, walk the GenDSP source
		 * numbers 0..22 into destination DST, one per MS ms (default
		 * 1000). The daemon already prints per-slot change counts once a
		 * second, so the log lines up source against lane activity and a
		 * single call covers the whole source range. Off by default; the
		 * destination's default route is restored when the sweep ends.
		 */
		int      sweep_dst;	/* -1 = off */
		unsigned sweep_ms;
		/*
		 * bindearly: assign the audio channel as soon as the chip
		 * reports a connection, not only when a call connects.
		 *
		 * v19 is the one run in which call audio ever reached the TDM,
		 * and the one thing that distinguished it was that the chip
		 * reported connection-up *again* 120 ms after the bind, then
		 * re-confirmed the channel. Every silent run had the opposite
		 * order: the connection came up long before the call, its only
		 * 0x0242 carried audio type 0x40 with no channel, and the bind
		 * arrived later. The theory this tests is that the chip wires
		 * its audio route when a connection is established, using the
		 * channel assignment that exists at that moment.
		 */
		int      bind_early;
		int      loopback;	/* echo the received bearer word back */
		/*
		 * loopdelay=N: echo the lane through an N-frame ring instead
		 * of in the same frame. The cadence stays locked to the
		 * chip's own pair boundaries -- each transmitted word still
		 * changes exactly when a received one does -- but the content
		 * is N frames old, so the handset cannot be hearing a live
		 * side-tone. Every downlink source that worked so far was
		 * live-derived, and every one that failed was free-running
		 * from a file; this separates the cadence from the source.
		 */
		unsigned loop_delay;
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
		 * playlog=FILE: append every octet pair as it is handed to the
		 * lane, and count frames, advances and received transitions.
		 * Judging a codec by ear through a DECT bearer localises
		 * nothing; this makes the transmitted stream decodable offline
		 * and the cadence countable.
		 */
		const char *play_log;
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
		 * The cadence is right (8000 octets/s, phase taken from the
		 * chip's own boundary) and the stream decodes to speech, yet
		 * the handset hears loud noise -- which is what a G.722
		 * decoder makes of structurally wrong octets. Two bits are
		 * left: which parity of frame starts a word, and the order of
		 * the two octets within it. playinv and playswap set them;
		 * playsweep=MS walks all four every MS ms so one call can
		 * find the right one instead of four.
		 */
		int      play_inv;
		int      play_swap;
		unsigned play_sweep_ms;
		/*
		 * playbyte=N chooses how octets are laid into the lane:
		 *   0  one 16-bit word (two octets) held across two frames,
		 *      which is how the chip's own uplink looks
		 *   1  one octet per frame in the low byte, advancing every
		 *      frame, the other byte left as the transmit filler
		 *   2  the same in the high byte
		 *
		 * Mode 0's four parity/order variants were all swept on
		 * hardware on 2026-10-20 and all sound like noise. A-0030
		 * then showed that mode 0 *is* the stock host's convention on
		 * this box, and that modes 1 and 2 are the convention of a
		 * denser slot map this box does not run [CR A-0030 §3, §9].
		 * They stay selectable, but the fault is not the layout.
		 */
		unsigned play_byte;
		/*
		 * playslow=N: hold every N-th coded word for four frames
		 * instead of two, so the lane carries 8000 * (1 - 1/N)
		 * octets/s. Every downlink source that was heard clean so far
		 * supplied the chip at or below its own rate (the loopback
		 * exactly, A4 and v32 below it), and every one that was heard
		 * as noise supplied a steady 8000 octets/s -- which is above
		 * the chip's measured uplink rate, 7976 octets/s in v22. The
		 * uplink's holds are 4, 10 and 40 frames, so a 4-frame hold
		 * is a shape the chip itself produces. N=100 under-supplies
		 * by 1 %, three times the measured deficit.
		 */
		unsigned play_slow;
		/*
		 * playphase=MS: alternate the held-word frame parity every
		 * MS ms, keeping octet order and layout fixed. Two variants,
		 * not six: a falsification run that should hear one thing
		 * throughout, or hear it alternate.
		 */
		unsigned play_phase_ms;
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
		 * pagehdr=N: a FOURTH payload byte on the 0x0300 page.
		 *
		 * [CR A-0091] corrects [CR A-0076] §2. The page builder does
		 * not compile the LCE header in: it builds page+4 as
		 * bit0 | ((mail[5] & 0x0F) << 1) | (bits 5-7 preserved), and
		 * mail[5] is the 0x0300 body byte AFTER the TPUI -- our
		 * param[3]. (A-0076 was still right that mail[2], our page
		 * info byte, is never read.)
		 *
		 * Neither we nor stock has ever written mail[5]: stock's
		 * sender emits three payload bytes exactly as ours does. So
		 * the builder has been reading a byte past what the sender
		 * wrote -- whatever chip buffer state happens to be there.
		 * That means the on-air LCE header is not known to be stable,
		 * let alone correct, and it is NOT a constant we are stuck
		 * with.
		 *
		 * -1 keeps the three-byte page we have always sent, so the
		 * default changes nothing. 0..15 appends the byte.
		 */
		int	 page_hdr;
		unsigned setup_tries;	/* 0 = RING_SETUP_TRIES */
		/*
		 * pagemail=SPEC: raw mails to send IMMEDIATELY after each
		 * 0x0300 page, in the order given. -M cannot do this -- its
		 * mails fire once, when the session comes up, not per page.
		 *
		 * This exists for 0x015E. docs/mac-bringup.md has recorded the
		 * stock sequence since the MAC bring-up work:
		 *
		 *   0x022C -> 0x015C -> 0x0300 -> 0x015E x2 -> 0x0300
		 *
		 * Stock sends 0x015E twice around every page; we have never
		 * sent it once. [CR A-0076] notes it carries a bearer-type
		 * field read by the MAC config -- mentioned only to rule it
		 * out as a page builder, so nobody asked what it does as a
		 * BEARER REQUEST. Our whole primitive inventory has no
		 * connection- or bearer-setup mail in it, which is the shape
		 * of a hole that "insufficient resources" would fit.
		 *
		 * The payload is deliberately NOT built in: we have no
		 * observed bytes for 0x015E, only that it exists and has a
		 * bearer-type field. Q-0055 asks the room for the layout. A
		 * wrong bearer request is likelier to wedge the MAC than a
		 * missing one, and the chip has no error indication to catch
		 * it [A-0086], so the bytes come from the answer, not from us.
		 */
		const char *page_mail[4];
		unsigned n_page_mail;
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
		 * signal=N: send <<SIGNAL>> N in the SETUP. v33 showed a
		 * call-class-3 SETUP displays the caller but never alerts,
		 * so the alerting pattern has to be stated rather than left
		 * to the handset. 0x40..0x47 are alerting patterns 0..7.
		 */
		int      signal_on;
		uint8_t  signal_value;
		/*
		 * ti=N: pin the CC transaction identifier of an FP-originated
		 * SETUP; -1 (the default) uses the lowest free value, 0.
		 * Written 2026-10-22 because stock uses TI 0 in every
		 * base->handset SETUP we have bytes for, every call this
		 * project ever completed used TI 0 (v28, v30, v34, A2, A3, B2)
		 * and every refused one used TI 1. The only ti=0 test then ran
		 * under the drain defect and was void, and the lab scripts
		 * pinned ti=1 from then on. Settled 2026-10-24: run 15 (ti=0)
		 * rang the A540, run 16 (ti=1, same session) was refused.
		 */
		int      ti_fixed;
		/*
		 * handover=MS: MS ms after the audio bind, ask the chip to move
		 * the call's bearer.
		 *
		 * Audio has only ever flowed in runs where the chip
		 * re-established the connection *after* the channel was
		 * assigned -- a repeated 0x0200 plus a 0x0243 parameter change.
		 * We cannot assign early enough to catch the first
		 * establishment (v23: bindearly produced no repeat and no
		 * audio), so instead provoke a second one. `0x016A {link}` is
		 * a stock handover hook, a hook for forcing a bearer
		 * handover [CR the hardware-analysis notes §4, §5 2.5;
		 * the hardware-analysis notes §5 0x2fea]. Our own COMMAND_SET §6.4 guessed
		 * this id was a frame-credit counter; the stock daemon's
		 * primitive-name table corrects that.
		 */
		unsigned handover_ms;
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
		 * The descriptor fields, settable for the same reason every
		 * 0x0230 field is: v28 sent a byte-exact 0x6C and 0x6D and the
		 * C4 displayed neither, so which field the handset gates on is
		 * now the open question and one run should be able to sweep it.
		 * Defaults are the honest ones (not screened); the stock
		 * producer's ordinary branch sends clipscr=1, i.e. `01 81`.
		 */
		uint8_t  clip_ton, clip_npi, clip_scr;
		uint8_t  cnip_scr, cnip_alpha;
		int      clip_first;	/* identity IEs before BASIC-SERVICE */
		/*
		 * lineid=N linetype=N callid=N callstatus=N: the
		 * <<CALL-INFORMATION>> subrecords of an FP-originated SETUP.
		 * TS 102 527-3 7.4.5.3.2 requires a line identifier in every
		 * incoming external call and none in an internal one, and we
		 * have never sent it -- the leading reason the C4 says
		 * "Intern". Each is opt-in; none means no IE.
		 */
		struct dect_callinfo callinfo;
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
	/* Ring for -a loopdelay=N, two bytes per frame. */
	uint8_t     loop_ring[LOOP_DELAY_MAX * 2u];
	unsigned    loop_wr;
	int         loop_primed;
	uint32_t    rx_word_prev;	/* last received lane word, for playsync */
	FILE       *play_log_fp;	/* -a playlog: the octets we transmit */
	uint32_t    pl_frames, pl_adv, pl_rxchg;	/* cadence counters */
	uint32_t    pl_held;		/* advances deferred by -a playslow */
	unsigned    md_idx;		/* memdump: range being swept */
	uint32_t    md_off;		/* memdump: offset within that range */
	uint64_t    md_at;		/* memdump: last request */
	unsigned    md_pass;		/* memdump: completed sweeps */
	unsigned    play_slow_ctr;
	int         play_locked;	/* playsync has seen its first transition */
	int         sweep_inv, sweep_swap;	/* active combination */
	unsigned    sweep_byte;			/* 0 = held word, 1 = low, 2 = high */
	unsigned    sweep_idx;
	uint64_t    sweep_at;
	uint64_t    pl_last_ms;
	size_t      play_pos;		/* byte offset into audio.play */
	uint32_t    slot_sweep_frames;	/* frames elapsed in a playslot sweep */
	int         slot_sweep_cur;	/* slot being written now, -1 = none yet */
	int         play_phase;		/* 0 = load a new word, 1 = hold it */
	struct dect_clip clip;		/* caller identity for an FP-originated SETUP */
	uint8_t     next_ti;		/* FP-originated TI value: 0, or -a ti= */
	const char *voice_path;
	FILE       *voice_fp;
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
	struct reg_ctx {
		int active;
		int persisted;		/* chip subscription written this connection */
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
		int handover_sent;	/* -a handover fired for this call */
		unsigned sweep_idx;	/* next GenDSP source for -a dspsweep */
		uint64_t sweep_ms;	/* when the current source was routed */
		int sweep_done;
		int cipher_started;	/* call-time authentication kicked off */
		int cipher_done;	/* cipher outcome already reported */
		int call_wideband;	/* this call's bind is the G.722 one */
		int enrol_routed;	/* this enrolment's store is chosen */
		int enrol_switched;	/* ... and it moved off store 0 */
		int ic_tried;		/* this call was checked for internal */
		size_t   dial_len;	/* digits seen so far in OVERLAP */
		uint64_t dial_change_ms;	/* when the last ones arrived */
		int      connect_staged;	/* alertms/bindfirst: CONNECT due */
		uint64_t connect_at;		/* ... not before this */
		enum dect_mm_state reported_state;	/* last one logged */
		uint8_t handle;
		/*
		 * The C-plane stack, one per connection, from libopendectcell
		 * (openDECTcell#67). The glue that used to live here -- segment
		 * decode, LAPC drive, NWK routing -- is protocol behaviour and
		 * belongs in the library where the controller can use the same
		 * code rather than a second copy written from prose.
		 */
		struct dect_link link;
	} reg;
};

/*
 * The chip's UAK table [CR A-0085]: six 17-byte records, 16 key bytes and a
 * config byte each. The address is the 08.25 STUB build's, which is the one
 * A-0085 was read from. On 07.56 this range holds something else and the
 * table's address is not known yet (Q-0059).
 */
#define SUB_UAK_BASE	0x9518u
#define SUB_UAK_STRIDE	0x11u
#define SUB_UAK_END	(SUB_UAK_BASE + DECT_MM_MAX_SLOT * SUB_UAK_STRIDE)

/*
 * Which second-stage STUB the chip runs. The chip says so itself only in the
 * 0xFFF2 build stamp right after a boot, which a run reusing a loaded STUB
 * never sees, so fwload.sh records the blob it uploaded here. The UAK table
 * address above is known for 08.25 only: on 2026-10-24 the box ran 07.56 for
 * seven hours, every read-back said MISMATCH and every audit five foreign
 * slots, and all of it was a read of unrelated memory.
 */
#define DECT_STUB_RECORD	"/tmp/dect-stub.second"
#define DECT_STUB_0825		"dectfw_secondlevel_441.hex"

enum { STUB_UNKNOWN, STUB_0825, STUB_OTHER };

static int stub_build_read(char *name, size_t cap)
{
	FILE *f = fopen(DECT_STUB_RECORD, "r");
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
 * Send a mail: encode the transmit element (libopendectcell) and spread its two
 * bytes per frame into the transmit control slot, idle (0xFF) elsewhere.
 */
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
 *     **That reassurance does not transfer to us.** `0x017C`, the re-assert,
 *     is on the list of mails we deliberately do not send. Stock does not
 *     verify but does re-assert; we did neither. So the subscription table --
 *     the thing that has actually caused us trouble -- is where we were most
 *     exposed and least like stock, which is why it now gets an audit as well
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
			if (s->stub_build == STUB_0825)
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
			  s->stub_build == STUB_0825);

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
	w = write(s->fd, frames, nf * fb);
	free(frames);
	if (w != (ssize_t)(nf * fb))
		return -1;
	s->tx_seq++;
	return 0;
}

/* One cycle at 8000 frames/s / 64 = 125 Hz steps; amplitude ~ -12 dBFS. */
static const int16_t tone_sine[64] = {
	     0,    784,   1561,   2322,   3061,   3771,   4445,   5075,
	  5657,   6184,   6652,   7055,   7391,   7656,   7846,   7961,
	  8000,   7961,   7846,   7656,   7391,   7055,   6652,   6184,
	  5657,   5075,   4445,   3771,   3061,   2322,   1561,    784,
	     0,   -784,  -1561,  -2322,  -3061,  -3771,  -4445,  -5075,
	 -5657,  -6184,  -6652,  -7055,  -7391,  -7656,  -7846,  -7961,
	 -8000,  -7961,  -7846,  -7656,  -7391,  -7055,  -6652,  -6184,
	 -5657,  -5075,  -4445,  -3771,  -3061,  -2322,  -1561,   -784
};

/*
 * Write the test tone into its lane. Host -> chip uses the same raw word
 * indexing as chip -> host -- our control slot is transmitted at raw word 1
 * and received there too -- so raw word 0, low byte, is the transmit side of
 * dect1 under A-0035's mapping. Writing A-law here while a call is up is the
 * one-way TX proof A-0030 §8.4, A-0033 step 3 and A-0035 step 5 all ask for.
 */
static void tx_tone(struct stubd *s, uint8_t *fr)
{
	unsigned int w = s->audio.tone_word;
	unsigned int step = s->audio.tone_hz * 64u / 8000u;

	if (!s->audio.tone_hz || !s->reg.call_bound || w >= s->slots ||
	    w == (unsigned int)s->tx_slot)
		return;
	if (s->audio.tone16) {
		/*
		 * The stock driver's *default* (non-enhanced) profile has a
		 * separate 16-bit path [CR A-0030 §3 Q3, A-0035 §Q1]; which
		 * profile is active is not established. Byte order within the
		 * word is not to be inferred from the platform, so both are
		 * selectable.
		 */
		int16_t v = tone_sine[s->tone_phase];
		uint8_t lo = (uint8_t)(v & 0xFF), hi = (uint8_t)((v >> 8) & 0xFF);

		fr[w * 2u] = s->audio.tone16be ? hi : lo;
		fr[w * 2u + 1u] = s->audio.tone16be ? lo : hi;
	} else {
		fr[w * 2u + (s->audio.tone_high ? 1u : 0u)] =
			dect_alaw_encode(tone_sine[s->tone_phase]);
	}
	s->tone_phase = (s->tone_phase + step) & 63u;
}

/* A received lane word that carries bearer data rather than a fill. */
static int lane_word_live(uint32_t v)
{
	return v <= 0xFFFFu && v != 0x0000u && v != 0xFEFEu && v != 0xFFFFu;
}

/*
 * The extra lanes' downlink for frame f (-a twocalls). The G.722 file
 * phase-locked to the chip's two-frame words as lane 0's playsync does; a
 * raw (linear) file two bytes a frame; the loop-back's delay ring; else
 * silence. Their own positions, so two calls each hear the whole file.
 */
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

static void xlane_tx(struct stubd *s, uint8_t *fr, const uint8_t *rx,
		     unsigned int f)
{
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++) {
		struct xlane *x = &s->xl[i];
		unsigned w = x->word;
		const uint8_t *src;

		if (!x->used || !x->connected || w >= s->slots ||
		    w == (unsigned)s->tx_slot)
			continue;
		src = rx ? rx + (f * s->slots + w) * 2u : NULL;
		fr[w * 2u] = s->tx_voice_idle;
		fr[w * 2u + 1u] = s->tx_voice_idle;
		if (ic_peer_word(s, x) >= 0 && rx) {
			/* the bridge: the other handset's uplink, as it comes */
			const uint8_t *p = rx + (f * s->slots +
				(unsigned)ic_peer_word(s, x)) * 2u;

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
			unsigned d = s->audio.loop_delay;

			if (d) {
				unsigned rd = (x->wr + 1u) % d;

				x->ring[x->wr * 2u] = src[0];
				x->ring[x->wr * 2u + 1u] = src[1];
				x->wr = rd;
				if (!x->primed && rd == 0u)
					x->primed = 1;
				if (x->primed) {
					fr[w * 2u] = x->ring[rd * 2u];
					fr[w * 2u + 1u] = x->ring[rd * 2u + 1u];
				}
			} else {
				fr[w * 2u] = src[0];
				fr[w * 2u + 1u] = src[1];
			}
		}
	}
}

/*
 * Transmit one DMA block: every frame carries the control slot (the next two
 * queued mail bytes, or idle when the queue is empty) and every other slot its
 * configured idle value. Called once per received block so the transmit side
 * keeps pace with the receive side without per-frame syscalls, which is the
 * cadence A-0030 §2 Q1 describes ("refills occur per buffer, not per
 * individual 8-kHz frame").
 */
static void tx_block(struct stubd *s, unsigned int fpb, uint8_t *frames,
		     const uint8_t *rx)
{
	size_t fb = s->slots * 2u;
	unsigned int f, n;
	/*
	 * Control-slot bytes are taken from a copy of the queue position and
	 * committed only for the frames the driver accepted. They used to be
	 * dequeued as the frames were built, so a failed or short write lost
	 * part of a mail for good (second external review,
	 * the clean-room record/an external review-1-24102026.md, finding 2). Frames take
	 * the queue in pairs from the first frame on, so the bytes owed for
	 * the first k accepted frames are min(2k, bytes taken).
	 */
	size_t qh = s->txq_head, ql = s->txq_len, taken, owed;
	ssize_t wr;

	if (s->tx_slot < 0)
		return;
	for (f = 0; f < fpb; f++) {
		uint8_t *fr = frames + f * fb;

		for (n = 0; n < s->slots; n++) {
			uint8_t fill = (n == (unsigned int)s->tx_slot)
				? 0xFFu : s->tx_idle;

			fr[n * 2u] = fill;
			fr[n * 2u + 1u] = fill;
		}
		/*
		 * Voice lanes get their own filler so a silent call sends
		 * silence rather than whatever the control path happens to use.
		 */
		if (s->reg.call_bound && s->tx_voice_idle != s->tx_idle) {
			for (n = 0; n < s->slots; n++) {
				if (n == (unsigned int)s->tx_slot)
					continue;
				fr[n * 2u] = s->tx_voice_idle;
				fr[n * 2u + 1u] = s->tx_voice_idle;
			}
		}
		/*
		 * Bearer loopback: copy the received word straight back out,
		 * byte for byte, frame for frame.
		 *
		 * What the chip puts on a bound call's lane is the coded G.722
		 * bearer, not PCM -- 4000 distinct 16-bit words per second, each
		 * held for two frames, which decodes to speech once de-duplicated
		 * (v19, confirmed by ear). Copying it back verbatim therefore
		 * hands the chip a valid coded stream with no encoder on our
		 * side, and preserves the two-frame cadence for free. If the
		 * handset hears itself, the transmit direction is proved -- and
		 * it explains every inaudible test tone so far, because those
		 * wrote A-law or linear PCM into a slot that carries coded bits.
		 */
		/*
		 * File playback: one coded word per two frames, looping. Takes
		 * precedence over the loopback, which it exists to replace as a
		 * source whose glitches cannot be blamed on the uplink.
		 */
		/*
		 * Hold the lane silent until the mode-2 re-assign has gone out.
		 * Between 0x022D and the re-assign the chip is still in mode 9,
		 * where linear PCM decodes as noise -- audible as a burst of
		 * white noise at the start of every call (reported 2026-10-23,
		 * and visible in vF411 as dir1 8001 -> 0000 one second in).
		 * There is nothing useful to send in that window.
		 */
		if (s->ic.st == IC_UP && rx && s->reg.call_bound &&
		    ic_word(s, s->ic.caller == -1 ? s->ic.tside :
			    s->ic.tside == -1 ? s->ic.caller :
			    IC_PARTY_NONE) >= 0 &&
		    s->audio.tone_word < s->slots) {
			/* internal call: lane 0 hears the other party */
			const uint8_t *p = rx + (f * s->slots + (unsigned)
				ic_word(s, s->ic.caller == -1 ? s->ic.tside :
					s->ic.caller)) * 2u;

			fr[s->audio.tone_word * 2u] = p[0];
			fr[s->audio.tone_word * 2u + 1u] = p[1];
		} else if (s->audio.play && s->audio.play_raw &&
			   s->reg.call_bound &&
			   (!s->audio.remode_ms || s->reg.remode_done)) {
			unsigned int w = s->audio.tone_word;

			if (s->audio.play_slot_ms && s->slots) {
				unsigned int span = s->audio.play_slot_ms * 8u;
				unsigned int k;

				k = (s->slot_sweep_frames / span) % s->slots;
				s->slot_sweep_frames++;
				/* the control slot is not a lane */
				if (k == (unsigned int)s->tx_slot)
					k = (k + 1u) % s->slots;
				w = k;
				if ((int)k != s->slot_sweep_cur) {
					s->slot_sweep_cur = (int)k;
					printf("play: lane slot %u\n", k);
					fflush(stdout);
				}
			}

			/* Verbatim lane: two bytes every frame, no cadence. */
			if (w < s->slots && w != (unsigned int)s->tx_slot) {
				if (s->play_pos + 2u > s->audio.play_len)
					s->play_pos = 0;
				fr[w * 2u] = s->audio.play[s->play_pos];
				fr[w * 2u + 1u] = s->audio.play[s->play_pos + 1u];
				s->play_pos += 2u;
			}
		} else if (s->audio.play && s->reg.call_bound) {
			unsigned int w = s->audio.tone_word;

			if (w < s->slots && w != (unsigned int)s->tx_slot) {
				int advance;

				if ((s->audio.play_sync ||
				     s->reg.call_wideband) && rx) {
					const uint8_t *src = rx
						+ (f * s->slots + w) * 2u;
					uint32_t v = (uint32_t)src[0]
						   | ((uint32_t)src[1] << 8);

					/*
					 * Lock the phase once, then free-run
					 * at a steady two frames per word.
					 *
					 * The chip consumes the downlink at a
					 * constant 8000 octets/s -- it has to,
					 * the bearer is 64 kbit/s -- so the
					 * rate must be ours to keep. What we
					 * cannot guess is *which* of each pair
					 * of frames starts a word, and getting
					 * that wrong is what made a
					 * free-running stream noise in v28-v30
					 * while leaving a steady sine intact.
					 *
					 * The first received transition after
					 * the call binds reveals the chip's
					 * pair boundary, so take the phase
					 * from it and then ignore the receive
					 * lane entirely. There is no drift to
					 * chase: both directions run off the
					 * same 8 kHz frame clock.
					 *
					 * Two things that do not work, both
					 * measured on 2026-10-20. Advancing on
					 * every transition (v32, A3, A4)
					 * starves: the uplink carries the
					 * handset's microphone, so when the
					 * user stops talking the lane stops
					 * changing and the downlink falls from
					 * 8000 octets/s to zero -- the
					 * instrumented run recorded seven
					 * consecutive seconds of no advances.
					 * Realigning the phase on every
					 * transition (run A) is no better,
					 * because the chip's holds of 4, 10
					 * and 40 frames make the transitions
					 * unevenly spaced, so each realignment
					 * inserts or drops an octet.
					 */
					/*
					 * A transition is a change between two
					 * live words. The sentinel after a reset
					 * and the idle fills must not count, or
					 * the "lock" lands on the first frame
					 * processed -- a DMA block boundary --
					 * which is what every run up to B3
					 * (2026-10-20) actually locked to.
					 */
					if (v != s->rx_word_prev &&
					    lane_word_live(v) &&
					    lane_word_live(s->rx_word_prev)) {
						s->pl_rxchg++;
						if (!s->play_locked) {
							s->play_locked = 1;
							s->play_phase =
								s->sweep_inv
								? 1 : 0;
						}
					}
					s->rx_word_prev = v;
					advance = !s->play_phase;
					s->play_phase ^= 1;
				} else {
					advance = !s->play_phase;
					s->play_phase ^= 1;
				}
				if (!s->play_pos)
					advance = 1;	/* prime */
				else if (advance && s->audio.play_slow &&
					 ++s->play_slow_ctr >= s->audio.play_slow) {
					/*
					 * Defer this advance by one pair of
					 * frames: the word is held for four,
					 * the parity is untouched.
					 */
					s->play_slow_ctr = 0;
					s->pl_held++;
					advance = 0;
				}
				if (s->sweep_byte) {
					/*
					 * One octet per frame: the lane holds
					 * a new octet every 125 us, in one
					 * byte position, and the other byte
					 * keeps the transmit filler.
					 */
					unsigned lo = (s->sweep_byte == 1u);

					if (s->play_pos >= s->audio.play_len)
						s->play_pos = 0;
					fr[w * 2u + (lo ? 0u : 1u)] =
						s->audio.play[s->play_pos];
					if (s->play_log_fp)
						fwrite(&s->audio.play[s->play_pos],
						       1, 1, s->play_log_fp);
					s->play_pos++;
					s->pl_adv++;
					s->pl_frames++;
					goto lane_done;
				}
				if (advance) {
					if (s->play_pos + 2u > s->audio.play_len)
						s->play_pos = 0;
					s->play_pos += 2u;
					s->pl_adv++;
					if (s->play_log_fp)
						fwrite(&s->audio.play[s->play_pos - 2u],
						       1, 2, s->play_log_fp);
				}
				s->pl_frames++;
				if (s->sweep_swap) {
					fr[w * 2u] =
						s->audio.play[s->play_pos - 1u];
					fr[w * 2u + 1u] =
						s->audio.play[s->play_pos - 2u];
				} else {
					fr[w * 2u] =
						s->audio.play[s->play_pos - 2u];
					fr[w * 2u + 1u] =
						s->audio.play[s->play_pos - 1u];
				}
lane_done:
				;
			}
		} else if (s->audio.loopback && s->reg.call_bound && rx) {
			unsigned int w = s->audio.tone_word;

			if (w < s->slots && w != (unsigned int)s->tx_slot) {
				const uint8_t *src = rx + (f * s->slots + w) * 2u;
				uint16_t v = (uint16_t)(src[0] | (src[1] << 8));

				/*
				 * Only echo real bearer data. A lane that is
				 * routed but has nothing on it reads 0x0000, and
				 * an unrouted one reads the 0xFEFE idle fill;
				 * feeding either back means handing the chip's
				 * downlink a stream of zeros or fill bytes while
				 * its own path is still coming up. v20 did that
				 * and its lane, which had started to come alive,
				 * went flat within the first second -- where v22,
				 * whose lane came up at once and echoed valid
				 * coded words from the start, worked. Until there
				 * is something real to send, send the filler.
				 */
				if (s->audio.loop_delay) {
					unsigned rd = (s->loop_wr + 1u)
						    % s->audio.loop_delay;

					s->loop_ring[s->loop_wr * 2u] = src[0];
					s->loop_ring[s->loop_wr * 2u + 1u] = src[1];
					s->loop_wr = rd;
					if (!s->loop_primed && rd == 0u)
						s->loop_primed = 1;
					if (s->loop_primed) {
						fr[w * 2u] = s->loop_ring[rd * 2u];
						fr[w * 2u + 1u] =
							s->loop_ring[rd * 2u + 1u];
					}
				} else if (v != 0x0000u && v != 0xFEFEu) {
					fr[w * 2u] = src[0];
					fr[w * 2u + 1u] = src[1];
				}
			}
		}
		xlane_tx(s, fr, rx, f);
		tx_tone(s, fr);
		if (ql >= 2u) {
			fr[(unsigned int)s->tx_slot * 2u] = s->txq[qh];
			fr[(unsigned int)s->tx_slot * 2u + 1u] =
				s->txq[(qh + 1u) % sizeof(s->txq)];
			qh = (qh + 2u) % sizeof(s->txq);
			ql -= 2u;
		}
	}
	taken = s->txq_len - ql;
	wr = write(s->fd, frames, fpb * fb);
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

/* Read up to max_blocks of raw frame data into buf (block payload only). */
static ssize_t read_frames(int fd, unsigned int slots, unsigned int fpb,
			   unsigned int max_blocks, uint8_t *buf, size_t cap,
			   unsigned int timeout_ms)
{
	size_t block = slots * 2u * fpb;
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

			if (h->magic != DECT_PCM_BLOCK_MAGIC || h->len != block)
				continue;
			if (got + block > cap)
				goto done;
			memcpy(buf + got, tmp + off + sizeof(*h), block);
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
		n = read_frames(s->fd, s->slots, fpb, PROBE_BLOCKS, data, cap, 500);
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

static int bring_up(struct stubd *s, struct dect_pcm_config *cfg)
{
	struct dect_ioc_info info;
	unsigned int step;

	memset(&info, 0, sizeof(info));
	if (ioctl(s->fd, DECT_IOC_INFO, &info) || info.version < 2 ||
	    !(info.features & DECT_FEATURE_PCM_RX)) {
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
/* The original layout, so files written before version 2 still load. */
struct sub_record_v1 {
	uint32_t magic;
	uint8_t  slot;
	uint8_t  uak[DECT_MM_UAK_LEN];
	uint8_t  port_id_len;
	uint8_t  port_id[DECT_MM_PORTID_MAX];
};

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
	unsigned v = s->audio.sub_slot && s->cur == 0 ? s->audio.sub_slot
						      : s->sub_slot;

	return (v >= 1u && v <= DECT_MM_MAX_SLOT) ? (uint8_t)v
						  : (uint8_t)DECT_MM_REG_SLOT;
}

static void hs_stash(struct stubd *s, unsigned i)
{
	struct handset *h = &s->hs[i];

	h->slot = s->sub_slot;
	memcpy(h->uak, s->uak, sizeof(h->uak));
	h->uak_valid = s->uak_valid;
	memcpy(h->port_id, s->sub_port_id, sizeof(h->port_id));
	h->port_id_len = s->sub_port_id_len;
	memcpy(h->cc_port_id, s->cc_port_id, sizeof(h->cc_port_id));
	h->cc_port_id_len = s->cc_port_id_len;
	memcpy(h->cc_fixed_id, s->cc_fixed_id, sizeof(h->cc_fixed_id));
	h->cc_fixed_id_len = s->cc_fixed_id_len;
	h->cc_id_learned = s->cc_id_learned;
}

static void hs_load(struct stubd *s, unsigned i)
{
	const struct handset *h = &s->hs[i];

	s->sub_path = h->path;
	s->sub_slot = h->slot;
	memcpy(s->uak, h->uak, sizeof(s->uak));
	s->uak_valid = h->uak_valid;
	memcpy(s->sub_port_id, h->port_id, sizeof(s->sub_port_id));
	s->sub_port_id_len = h->port_id_len;
	memcpy(s->cc_port_id, h->cc_port_id, sizeof(s->cc_port_id));
	s->cc_port_id_len = h->cc_port_id_len;
	memcpy(s->cc_fixed_id, h->cc_fixed_id, sizeof(s->cc_fixed_id));
	s->cc_fixed_id_len = h->cc_fixed_id_len;
	s->cc_id_learned = h->cc_id_learned;
	s->cc_id_foreign_warned = 0;
	s->cc_id_unarmed_warned = 0;
}

/* Make handset i the current one; a no-op if it already is. */
static void hs_switch(struct stubd *s, unsigned i, const char *why)
{
	if (i == s->cur || i >= s->n_hs)
		return;
	hs_stash(s, s->cur);
	hs_load(s, i);
	s->cur = i;
	printf("sub: now serving %s (slot %u)%s\n", s->sub_path,
	       stubd_sub_slot(s), why);
}

/*
 * The identities of the handset we ring, wherever they are: in the fields
 * while it is current, in the table while the link serves another one.
 */
static const uint8_t *ring_target_port_id(const struct stubd *s, size_t *len)
{
	if (s->cur == 0 || !s->n_hs) {
		*len = s->cc_port_id_len;
		return s->cc_port_id;
	}
	*len = s->hs[0].cc_port_id_len;
	return s->hs[0].cc_port_id;
}

static void sub_save(struct stubd *s)
{
	struct sub_record r;
	int fd;

	if (!s->sub_path || !s->uak_valid)
		return;
	memset(&r, 0, sizeof(r));
	r.magic = SUB_MAGIC;
	r.slot = stubd_sub_slot(s);
	memcpy(r.uak, s->uak, DECT_MM_UAK_LEN);
	r.magic = SUB_MAGIC2;
	/* the current handset's identity; the link MM may serve another */
	if (s->sub_port_id_len) {
		r.port_id_len = (uint8_t)s->sub_port_id_len;
		memcpy(r.port_id, s->sub_port_id, sizeof(r.port_id));
	} else {
		r.port_id_len = (uint8_t)s->reg.link.mm.port_id_len;
		memcpy(r.port_id, s->reg.link.mm.port_id, sizeof(r.port_id));
	}
	r.cc_port_id_len = (uint8_t)s->cc_port_id_len;
	memcpy(r.cc_port_id, s->cc_port_id, sizeof(r.cc_port_id));
	r.cc_fixed_id_len = (uint8_t)s->cc_fixed_id_len;
	memcpy(r.cc_fixed_id, s->cc_fixed_id, sizeof(r.cc_fixed_id));

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
		if (s->cur < s->n_hs)
			s->hs[s->cur].loaded = 1;	/* it exists now */
	}
}

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
	memcpy(s->uak, r.uak, DECT_MM_UAK_LEN);
	s->uak_valid = 1;
	/*
	 * The record has always carried the MM portable identity and we have
	 * never restored it, so the identity gate in on_locate_request() could
	 * not fire: measured 2026-10-23, a handset we hold no record for had
	 * its location registration accepted anyway.
	 */
	if (r.port_id_len && r.port_id_len <= DECT_MM_PORTID_MAX) {
		memcpy(s->sub_port_id, r.port_id, r.port_id_len);
		s->sub_port_id_len = r.port_id_len;
	}
	if (r.magic == SUB_MAGIC2 && r.cc_port_id_len <= DECT_CC_ID_MAX &&
	    r.cc_fixed_id_len <= DECT_CC_ID_MAX) {
		memcpy(s->cc_port_id, r.cc_port_id, sizeof(s->cc_port_id));
		s->cc_port_id_len = r.cc_port_id_len;
		memcpy(s->cc_fixed_id, r.cc_fixed_id, sizeof(s->cc_fixed_id));
		s->cc_fixed_id_len = r.cc_fixed_id_len;
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
		if (!s->sub_port_id_len && r.cc_port_id_len &&
		    r.cc_port_id_len <= DECT_MM_PORTID_MAX) {
			memcpy(s->sub_port_id, r.cc_port_id, r.cc_port_id_len);
			s->sub_port_id_len = r.cc_port_id_len;
		}
		/*
		 * Call identities that are not the subscribed handset's were
		 * learned from another handset's call (see the learning gate
		 * where they are kept) and would address every incoming call
		 * to it. GAP table 10 makes a PT's {CC-SETUP} carry its IPUI,
		 * the identity it registered with, so the two must agree.
		 * Drop them; the handset's next call teaches the right pair.
		 */
		if (s->cc_port_id_len &&
		    (s->cc_port_id_len != s->sub_port_id_len ||
		     memcmp(s->cc_port_id, s->sub_port_id,
			    s->sub_port_id_len))) {
			memset(s->cc_port_id, 0, sizeof(s->cc_port_id));
			memset(s->cc_fixed_id, 0, sizeof(s->cc_fixed_id));
			s->cc_port_id_len = 0;
			s->cc_fixed_id_len = 0;
			printf("sub: *** stored call identities belong to "
			       "ANOTHER handset *** -- dropped; they are "
			       "relearned from this handset's next call\n");
		}
	}
	printf("sub: identity gate %s (%zu bytes)\n",
	       s->sub_port_id_len ? "armed" : "DISABLED -- no stored identity",
	       s->sub_port_id_len);
	/*
	 * Honour the slot the record was saved with, unless -a subslot=N said
	 * otherwise. Without this a handset enrolled in slot 3 came back as
	 * slot 1 on the next run, taking another handset's UAK slot and TPUI
	 * with it.
	 */
	if ((!s->audio.sub_slot || s->cur != 0) &&
	    r.slot >= 1u && r.slot <= DECT_MM_MAX_SLOT)
		s->sub_slot = r.slot;	/* subslot= is the ring target's only */
	printf("sub: restored subscription for slot %u from %s%s\n",
	       r.slot, s->sub_path,
	       s->cc_fixed_id_len ? " (with call identities)" : "");
	return 1;
}

static void uak_verify_send(struct stubd *s, uint8_t slot);

static unsigned hs_slot(const struct stubd *s, unsigned i);

/*
 * holdback=MS (docs/multicell-experiments.md, test M2): every store but the
 * first stays out of the chip until MS after start, as if a controller
 * pushed it then. The host still holds them; only the chip lacks the key.
 */
static int held_back(const struct stubd *s, unsigned i)
{
	if (s->audio.no_chip_keys)	/* every slot cleared, no key at all */
		return 1;
	if (i < s->n_hs && s->hs[i].remote && !s->hs[i].fetched)
		return 1;		/* a roamer: not in this cell's chip */
	return s->audio.holdback_ms && !s->holdback_done && i != 0;
}

static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN]);

/*
 * Test M4 (docs/multicell-experiments.md): play the controller for a
 * roamer. MM holds a {LOCATE-REQUEST} from an identity this link does not
 * know; if it is the remote= store's, its record "arrives" fetchdelay ms
 * after the request, and the request is answered. Times are from the
 * request, so the handset's patience is measured, not ours.
 */
static void pump_lapc(struct stubd *s);

/* The controller's answer: hold the fetched record on the main link. */
static void fetch_keep(struct stubd *s)
{
	struct handset *h;

	if (s->fetch_idx < 0)
		return;
	h = &s->hs[s->fetch_idx];
	h->fetched = 1;		/* the next link holds it from hs[] */
	if (s->reg.active)
		(void)dect_mm_hold(&s->reg.link.mm, h->port_id,
				   h->port_id_len, (uint8_t)h->slot, NULL,
				   s->fetch_idx);
}

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
	if (s->reg.active &&
	    dect_mm_locate_pending(&s->reg.link.mm, &id, &len))
		setup = 0;
	else if (s->reg.active &&
		 dect_link_setup_pending(&s->reg.link, &id, &len))
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
		for (i = 0; i < s->n_hs; i++)
			if (s->hs[i].remote && s->hs[i].port_id_len == len &&
			    !memcmp(s->hs[i].port_id, id, len))
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
	if (s->reg.link.mm.stats.locate_retrans != s->fetch_retrans_seen) {
		s->fetch_retrans_seen = s->reg.link.mm.stats.locate_retrans;
		printf("fetch: the handset repeated its LOCATE-REQUEST at +%llu "
		       "ms\n", (unsigned long long)(now - s->fetch_req_ms));
	}
	if (now < s->fetch_at)
		return;
	fetch_keep(s);
	r = setup ? dect_link_setup_resume(&s->reg.link, now)
		  : dect_mm_locate_resume(&s->reg.link.mm);
	printf("fetch: %s at +%llu ms after the request\n",
	       setup ? (r > 0 ? "CC-SETUP served" : "CC-SETUP turned away")
		     : (r > 0 ? "LOCATE-ACCEPT sent" : "LOCATE-REJECT sent"),
	       (unsigned long long)(now - s->fetch_req_ms));
	fflush(stdout);
	s->fetch_req_ms = 0;
	s->fetch_at = 0;
	dect_link_drain(&s->reg.link, now);
	pump_lapc(s);
}

/*
 * Overlap receiving: accept a handset-placed call once its digits have
 * been quiet for dialwait ms. Stands in for a dial plan or the
 * controller, which would decide on the number instead.
 */
static int ic_start(struct stubd *s, int side, uint64_t now);

static void dial_tick(struct stubd *s, uint64_t now)
{
	const uint8_t *k;
	size_t len;

	/* an internal call that sent its number complete: route it now */
	if (s->reg.active && s->reg.link.cc.state == DECT_CC_ST_PROCEEDING &&
	    !s->reg.link.cc.incoming && !s->reg.ic_tried &&
	    (s->reg.link.cc.peer_basic_service >> 4) == 0x9u) {
		s->reg.ic_tried = 1;
		(void)ic_start(s, -1, now);
		dect_link_drain(&s->reg.link, now);
		pump_lapc(s);
	}
	if (!s->reg.active || !dect_cc_busy(&s->reg.link.cc))
		s->reg.ic_tried = 0;
	if (!s->reg.active || s->reg.link.cc.state != DECT_CC_ST_OVERLAP) {
		s->reg.dial_len = 0;
		s->reg.dial_change_ms = 0;
		return;
	}
	k = dect_cc_keypad(&s->reg.link.cc, &len);
	if (!s->reg.dial_change_ms || len != s->reg.dial_len) {
		s->reg.dial_len = len;
		s->reg.dial_change_ms = now;
		return;
	}
	if (!len || now - s->reg.dial_change_ms < s->audio.dial_wait_ms)
		return;		/* no digit yet: F<CC.01> covers it */
	printf("call: dialled \"%.*s\" -- complete after %u ms without a "
	       "digit, connecting\n", (int)len, (const char *)k,
	       s->audio.dial_wait_ms);
	fflush(stdout);
	s->reg.ic_tried = 1;
	if (ic_start(s, -1, now)) {
		dect_link_drain(&s->reg.link, now);
		pump_lapc(s);
		return;
	}
	if (s->audio.alert_ms || s->audio.bind_first) {
		/* the steps a routed call takes; connect_tick() finishes */
		if (dect_cc_proceed(&s->reg.link.cc) == 0) {
			if (s->audio.alert_ms) {
				(void)dect_cc_alert(&s->reg.link.cc);
				printf("call: ALERTING sent, CONNECT in %u ms\n",
				       s->audio.alert_ms);
			}
			s->reg.connect_staged = 1;
			s->reg.connect_at = now + s->audio.alert_ms;
			dect_link_drain(&s->reg.link, now);
			pump_lapc(s);
		}
		return;
	}
	if (dect_cc_accept(&s->reg.link.cc) == 0) {
		dect_link_drain(&s->reg.link, now);
		pump_lapc(s);
	}
}

/* alertms / bindfirst: send the CONNECT once its conditions hold. */
static void connect_tick(struct stubd *s, uint64_t now)
{
	if (!s->reg.connect_staged)
		return;
	if (!s->reg.active ||
	    s->reg.link.cc.state != DECT_CC_ST_PROCEEDING) {
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
	if (dect_cc_connect(&s->reg.link.cc) == 0) {
		dect_link_drain(&s->reg.link, now);
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
	for (i = 1; i < s->n_hs; i++) {
		const uint8_t *uak = i == s->cur ? s->uak : s->hs[i].uak;
		int valid = i == s->cur ? s->uak_valid : s->hs[i].uak_valid;

		if (!s->hs[i].loaded || !valid)
			continue;
		install_subscription(s, (uint8_t)hs_slot(s, i), uak);
		printf("sub: holdback over: %s installed in chip slot %u at "
		       "t=%llu ms\n", s->hs[i].path, hs_slot(s, i),
		       (unsigned long long)(now - g_start));
	}
	fflush(stdout);
}

/* Does any handset we hold have a key to put in the chip? */
static int any_uak(const struct stubd *s)
{
	unsigned i;

	if (s->uak_valid)
		return 1;
	for (i = 0; i < s->n_hs; i++)
		if (i != s->cur && s->hs[i].loaded && s->hs[i].uak_valid)
			return 1;
	return 0;
}

static void sub_audit_reply(struct stubd *s, uint32_t addr,
			    const uint8_t *rec, size_t len);
static void sub_audit_send(struct stubd *s, uint8_t mine);

/*
 * Persist the just-enrolled handset in the chip so it holds standby and is
 * reachable: 0x0247 {slot, op=1, status} then 0x020A {slot, UAK[16], extra},
 * to STUB task 0x02, in that order, after LOCATE-ACCEPT [CR A-0026]. The 16-byte
 * 0x020A record is the allocated UAK. The `status`/`extra` security-mode bytes
 * have no known enumeration [CR A-0026]; we send 0x00 and validate on hardware.
 * 0xF605 / 0x015F / 0x017C are intentionally NOT sent (not on AVM's ordinary
 * voice-handset path).
 */
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
	struct dect_mail m;
	uint8_t slot;

	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++) {
		if (keep_mask & (1u << slot))
			continue;
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0x0247u;
		m.param[0] = slot;
		m.param[1] = 0x00u;	/* op: clear */
		m.param[2] = 0x00u;
		m.param_len = 3;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}
	printf("sub: cleared chip subscription slots other than");
	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++)
		if (keep_mask & (1u << slot))
			printf(" %u", slot);
	printf(" -- the chip keeps them across runs and would otherwise still "
	       "admit a handset we have no record of\n");
	/*
	 * ...and now check it actually happened. The clear is 0x0247, which
	 * has no reply and no error path, so until today "cleared" meant
	 * "asked". vF426 is what asking without checking looks like.
	 */
	sub_audit_send(s, keep);
}

static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN]);
static void send_subscription_tail(struct stubd *s);

/*
 * Every handset we hold goes into its chip slot, the current one last so its
 * read-back is the one outstanding; only slots no store uses are cleared.
 * With one store loaded this is what it always was.
 */
static void send_subscription(struct stubd *s)
{
	uint8_t slot = stubd_sub_slot(s);
	unsigned keep = s->uak_valid && !held_back(s, s->cur) ? 1u << slot : 0u;
	unsigned i;

	for (i = 0; i < s->n_hs; i++) {
		unsigned hsl = hs_slot(s, i);

		if (i != s->cur && s->hs[i].loaded && s->hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && !held_back(s, i))
			keep |= 1u << hsl;
	}
	clear_foreign_slots(s, slot, keep);
	for (i = 0; i < s->n_hs; i++) {
		unsigned hsl = hs_slot(s, i);

		if (i != s->cur && s->hs[i].loaded && s->hs[i].uak_valid &&
		    hsl >= 1u && hsl <= DECT_MM_MAX_SLOT && hsl != slot &&
		    !held_back(s, i)) {
			install_subscription(s, (uint8_t)hsl, s->hs[i].uak);
			printf("sub: %s installed in chip slot %u\n",
			       s->hs[i].path, hsl);
		}
	}
	if (s->uak_valid && !held_back(s, s->cur)) {
		install_subscription(s, slot, s->uak);
		uak_verify_send(s, slot);	/* [CR A-0085] the only check */
	}
	send_subscription_tail(s);
}

static void install_subscription(struct stubd *s, uint8_t slot,
				 const uint8_t uak[DECT_MM_UAK_LEN])
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0247u;
	m.param[0] = slot;		/* slot */
	m.param[1] = 0x01u;		/* op: install/refresh */
	/*
	 * Third byte: [CR A-0113] stock sends bit 0 = 1 (A540 = 0x03); the
	 * op=1 handler tests bit 0 and clears a per-slot mask bit on 1, sets
	 * it on 0. Historical default 0x00; -a attflag= overrides.
	 */
	m.param[2] = s->audio.att_flag;
	m.param_len = 3;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

	/*
	 * Slot 7 would address the start of the 0xF605 table and the chip-side
	 * writer does not range-check it [CR A-SubscriberCapacity-7]. Refuse
	 * rather than trust the caller; see DECT_MM_MAX_SLOT.
	 */
	if (slot < 1u || slot > DECT_MM_MAX_SLOT) {
		fprintf(stderr, "sub: refusing 0x020A for slot %u "
			"(valid 1..%u)\n", (unsigned)slot, DECT_MM_MAX_SLOT);
		return;
	}

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x020Au;
	m.param[0] = slot;		/* one-based, 17-byte record stride */
	memcpy(&m.param[1], uak, DECT_MM_UAK_LEN);		/* 16-byte credential */
	/*
	 * Config byte after the UAK: [CR A-0113] stock sends 0x01 per Gigaset
	 * (0x02 for the C4). The 0x020A handler stores it at UAK-slot +0x10
	 * and the CC state machine copies it into conn+0x21, the per-connection
	 * auth/capability word the ring progression uses. Our historical 0x00
	 * seeds the wrong security class and a GAP handset then refuses the
	 * call with 0x32. -a attcfg= sets it.
	 */
	m.param[1 + DECT_MM_UAK_LEN] = s->audio.att_cfg;	/* [CR A-0113] */
	m.param_len = (uint16_t)(2 + DECT_MM_UAK_LEN);
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)			/* log_mail() masks the UAK */
		log_mail(&m, "tx M", s->show_params);
}

static void send_subscription_tail(struct stubd *s)
{
	struct dect_mail m;

	/*
	 * [CR A-0114] 0x017c sets the traffic-bearer slot-availability mask
	 * 0xdb3. Without it 0xdb3 stays at its init 0x3f (all slots in-use)
	 * and the CC state machine allocates no bearer slot for an incoming
	 * call, so a GAP handset refuses with 0x32 "insufficient bearers".
	 * Stock sends this right after 0x020A; 0xdb3 is global so once is
	 * enough. The handler takes the second payload byte into 0xdb3.
	 */
	if (!s->audio.no_slot_cfg) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0x017Cu;
		m.param[0] = 0x00u;		/* unused by the handler */
		m.param[1] = s->audio.slot_mask;	/* -> 0xdb3; stock 0x09 */
		m.param_len = 2;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
		printf("sub: slot-availability mask 0x017c 00 %02x "
		       "(0xdb3; frees bearer slots) [CR A-0114]\n",
		       s->audio.slot_mask);
	}
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
	struct dect_mail m;
	uint32_t addr;

	if (slot < 1u || slot > DECT_MM_MAX_SLOT)
		return;
	if (s->stub_build == STUB_OTHER) {
		printf("sub: UAK read-back skipped -- STUB %s is loaded, and the "
		       "table address we hold is the 08.25 build's [CR A-0085, "
		       "Q-0059]\n", s->stub_name);
		return;
	}
	addr = SUB_UAK_BASE + (uint32_t)(slot - 1u) * SUB_UAK_STRIDE;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_PCM_REMOTE;
	m.prim = 0xFFF1u;
	m.param[0] = 0x07u;			/* MEM_READ */
	m.param[1] = (uint8_t)(addr & 0xFFu);
	m.param[2] = (uint8_t)((addr >> 8) & 0xFFu);
	m.param[3] = (uint8_t)((addr >> 16) & 0xFFu);
	m.param[4] = (uint8_t)((addr >> 24) & 0xFFu);
	m.param[5] = SUB_UAK_STRIDE;		/* 16 key bytes + the flag */
	m.param[6] = 0x00u;
	m.param_len = 7;
	s->uak_check_addr = addr;
	s->uak_check_slot = slot;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
}

/* Match a 0xFFF1 sub 0x08 reply against an outstanding read-back. */
static void uak_verify_reply(struct stubd *s, const struct dect_mail *m)
{
	uint32_t addr;

	if (m->param_len < 7u || m->param[0] != 0x08u)
		return;
	addr = (uint32_t)m->param[1] | ((uint32_t)m->param[2] << 8)
	     | ((uint32_t)m->param[3] << 16) | ((uint32_t)m->param[4] << 24);
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
	if (!memcmp(&m->param[7], s->uak, DECT_MM_UAK_LEN)) {
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
 * **That reassurance does not transfer to us**: `0x017C`, the re-assert, is
 * on the list of mails we deliberately do not send. Stock does not verify but
 * does re-assert; we do neither. Of the two of us, we are the one exposed.
 *
 * Occupancy only -- a slot holds key material or it does not. We never print
 * the key, and we cannot yet say WHICH handset a slot belongs to: that is the
 * per-handset config above 0x957E, whose layout we do not have (Q-0054).
 */
static void sub_audit_send(struct stubd *s, uint8_t mine)
{
	uint8_t slot;

	s->sub_audit_pending = 0;
	s->sub_audit_occupied = 0;
	s->sub_audit_mine = mine;
	if (s->stub_build == STUB_OTHER) {
		printf("sub: slot audit skipped -- STUB %s is loaded, and the "
		       "table address we hold is the 08.25 build's\n",
		       s->stub_name);
		return;
	}
	for (slot = 1u; slot <= DECT_MM_MAX_SLOT; slot++) {
		struct dect_mail m;
		uint32_t addr = SUB_UAK_BASE
			      + (uint32_t)(slot - 1u) * SUB_UAK_STRIDE;

		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_PCM_REMOTE;
		m.prim = 0xFFF1u;
		m.param[0] = 0x07u;		/* MEM_READ */
		m.param[1] = (uint8_t)(addr & 0xFFu);
		m.param[2] = (uint8_t)((addr >> 8) & 0xFFu);
		m.param[3] = (uint8_t)((addr >> 16) & 0xFFu);
		m.param[4] = (uint8_t)((addr >> 24) & 0xFFu);
		m.param[5] = SUB_UAK_STRIDE;
		m.param[6] = 0x00u;
		m.param_len = 7;
		s->sub_audit_pending |= (uint8_t)(1u << (slot - 1u));
		send_mail(s, (unsigned int)s->tx_slot, &m);
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
		uint8_t mine_bit = s->sub_audit_mine >= 1u &&
				   s->sub_audit_mine <= DECT_MM_MAX_SLOT
			? (uint8_t)(1u << (s->sub_audit_mine - 1u)) : 0u;
		uint8_t foreign = (uint8_t)(s->sub_audit_occupied & ~mine_bit);

		printf("sub: chip slot audit:");
		for (i = 1u; i <= DECT_MM_MAX_SLOT; i++)
			printf(" %u=%s", i,
			       (s->sub_audit_occupied & (1u << (i - 1u)))
				       ? (i == s->sub_audit_mine ? "OURS" : "held")
				       : "-");
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
 * Bind a connected call's voice path to DSP audio channel 1 [CR COMMAND_SET
 * §6.5]: assign the channel (0x0230), program the DSP route (0xF626 on), then
 * connect the audio path (0x022D). Reply 0x0242 confirms the channel. This sets
 * up the bearer; feeding actual voice (echo/tone) is a later step.
 */
/*
 * Bind a connected call's voice path, in the order AVM's own bind routine uses
 * [CR A-0029 §1]: 0x0251 SET_CHANNEL_PARAM *first*, then 0x0230 in its full
 * six-parameter form. Our earlier sequence had three faults, all from A-0029:
 *
 *  - 0x0251 was never sent at all. AVM sends it before the channel is assigned,
 *    not after; its third byte is an EEPROM-derived value that A-0029 could not
 *    recover, so -a gain=N makes it settable.
 *  - 0x0230 was sent in the two-byte short form. The STUB defaults the missing
 *    fields, so it is not invalid, but it does not reproduce AVM's setup.
 *  - 0xF626 was sent between the two. A-0029 found it absent from the normal
 *    binding producer -- it lives in the transport's serialisation logic -- so
 *    it is off unless -a f626 asks for it.
 *
 * 0x022D stays last: A-0029 found it produced by separate state-dependent
 * paths (mute/unmute, audio-state events) rather than as a fixed third step,
 * but connecting the path explicitly is still what we want.
 */
static void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan);

static void send_audio_bind(struct stubd *s, uint8_t handle)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;
	int wb;

	if (!a->no_gain) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0x0251u;	/* SET_CHANNEL_PARAM, before the assign */
		m.param[0] = handle;
		m.param[1] = a->sel;	/* selector, valid 1..6 [CR A-CodecModes-3] */
		m.param[2] = a->gain;	/* stored as value << 8 in the DSP block */
		m.param_len = 3;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0230u;		/* ASSIGN_AUDIO_CHANNEL, full form */
	/* a wideband call: stock's bind, flags 0x41 and codec byte 0 */
	wb = s->reg.call_wideband;
	m.param[0] = handle;
	m.param[1] = wb ? (uint8_t)(a->flags | 0x40u) : a->flags;
	m.param[2] = (uint8_t)(a->mask & 0xFFu);	/* slot mask (LE) */
	m.param[3] = (uint8_t)(a->mask >> 8);
	m.param[4] = a->mode;		/* master */
	m.param[5] = wb ? 0u : a->codec;	/* extra */
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

	printf("call: audio assign: 0x0251 %s(sel %u gain 0x%02x), "
	       "0x0230 {%02x %04x %02x %02x}%s\n",
	       a->no_gain ? "SKIPPED " : "", a->sel, a->gain,
	       m.param[1], a->mask, a->mode, m.param[5],
	       a->nowait ? "" : " -- 0x022D waits for 0x0242");
	if (a->tone_hz)
		printf("call: tx tone: %u Hz %s into raw word %u%s\n",
		       a->tone_hz,
		       a->tone16 ? (a->tone16be ? "pcm16-be" : "pcm16-le") : "A-law",
		       a->tone_word,
		       a->tone16 ? "" : (a->tone_high ? " high byte" : " low byte"));

	/*
	 * 0x022D connects the path of a connection "whose channel is already
	 * assigned" [CR COMMAND_SET §6.5]. The assignment is asynchronous: the
	 * 0x0242 confirmation arrives tens of milliseconds after 0x0230, while a
	 * 0x022D queued here is on the wire within a few. AVM sends 0x022D from
	 * state-dependent paths rather than as a fixed third mail [CR A-0029 §2,
	 * A-0030 §2 Q6], so we wait for the confirmation too -- see the 0x0242
	 * case in handle_reg_mail() -- unless -a nowait asks for the old order.
	 */
	if (a->nowait)
		send_audio_connect(s, handle, a->flags & 0x0Fu);
}

/*
 * Connect the assigned channel: optionally route it (0xF626, not part of
 * AVM's bind), then 0x022D with bit 7 clear. `chan` is the channel the chip
 * confirmed in 0x0242 -- A-0032 Q9: trust the returned channel, not the one
 * we asked for.
 *
 * CAVEAT, 2026-10-23 [CR A-0075 §0]: "confirmed" is too strong. 0x0242 is
 * emitted on EVERY path through the handler, including the early-out that
 * does nothing -- it means "mail processed", not "matrix changed". vF418 has
 * six byte-identical 0x0242 replies to sends that changed nothing, next to
 * one that configured the matrix. The real act/no-act gate is the
 * connection's byte 0/byte 1 plus record +0x116 bit 5. We keep using the
 * returned channel because it is still the channel, but nothing here should
 * be read as evidence that a 0x0230 landed.
 */
/*
 * 0x0233 AUDIO_DISCONNECT -- the clean inverse of 0x0230 [CR A-0070].
 *
 * We have never sent this. `call_bound` was set when a call connected and
 * cleared only when the LINK restarted, so the audio channel stayed armed
 * after every call ended and the next call inherited whatever state the last
 * one left. The chip's handler runs the configurator in mode 0 (release) and
 * clears the path-on bit (record +0x116 b6), which is exactly the teardown we
 * have been missing.
 *
 * [CR A-0070] §4 gives the release order: 0x0233 first, then 0x0202 once the
 * DLC is quiescent. This is the audio half.
 */
static void send_audio_disconnect(struct stubd *s, uint8_t handle)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0233u;
	m.param[0] = handle;
	m.param[1] = 0x01u;	/* [3] must be non-zero */
	m.param_len = 2;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: audio disconnected (0x0233 on handle %u)\n", handle);
}

static void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;

	if (s->reg.audio_connected)
		return;
	s->reg.audio_connected = 1;
	if (a->no_connect) {
		printf("call: audio connect: 0x022D omitted (-a noconnect)\n");
		return;
	}

	if (a->send_f626) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0xF626u;	/* SET_AUDIO_ROUTE */
		m.param[0] = handle;
		m.param[1] = 0x01u;	/* enable */
		m.param[2] = chan;	/* chan_cfg */
		m.param[3] = 0x00u;	/* param (LE) */
		m.param[4] = 0x00u;
		m.param_len = 5;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x022Du;		/* AUDIO_PATH_ON_OFF */
	m.param[0] = handle;
	m.param[1] = 0x00u;		/* bit 7 = 0: connect */
	s->reg.remode_handle = handle;
	if (a->remode_ms)
		s->reg.remode_at = mono_ms() + a->remode_ms;
	m.param_len = 2;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: audio connect: %s0x022D on channel %u\n",
	       a->send_f626 ? "0xF626, " : "", chan);

	/*
	 * GenDSP connects (-a dsp=DST:SRC,...). A debug path, so only on the
	 * bench, and reversible: route the destination to silence 12 or send
	 * 0xFFF1 sub 0x09 n to restore channel n's default [CR A-0029 §6].
	 */
	for (unsigned i = 0; i < a->dsp_n; i++) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = 0x23u;		/* debug/management task */
		m.prim = 0xFFF1u;
		m.param[0] = 0x0Au;	/* DSP_ROUTE */
		m.param[1] = a->dsp_dst[i];
		m.param[2] = a->dsp_src[i];
		m.param_len = 3;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
		printf("call: dsp route: dst %u <- src %u\n",
		       a->dsp_dst[i], a->dsp_src[i]);
	}
}

/*
 * Fallback: if the chip never confirms the assignment, connect anyway after
 * half a second so the deferred order can never do worse than the old one.
 */
static void audio_connect_fallback(struct stubd *s, uint64_t now)
{
	if (!s->reg.call_bound || s->reg.audio_connected || !s->reg.active)
		return;
	if (now - s->reg.bind_ms < 500u)
		return;
	printf("call: audio connect: no 0x0242 within 500 ms, connecting anyway\n");
	send_audio_connect(s, s->reg.handle, s->audio.flags & 0x0Fu);
}

/*
 * Ask the STUB for the per-link connection status: 0xF624 {link, 0x0A},
 * answered by 0xF625 with a 0x2D-byte status record. AVM's host driver polls
 * this every 30 s for its connection monitor [CR the hardware-analysis notes §6 1.5,
 * §5 host mail 0xF611]. It is a pure read, so it changes nothing about what
 * the radio transmits -- which is why it is usable during a live call.
 *
 * The record's first bytes are the ones we are after: MAC slot, RF carrier,
 * the codec/slot-type byte and the link type. The codec byte doubles as the
 * bearer's slot type: 0 = full slot (G.726 narrowband), 1..3 = long slot
 * (G.722 or the AVM wideband codecs) [CR the hardware-analysis notes §8 3.3,
 * §5 4.4]. A G.722 call that is running on a full-slot bearer would explain
 * silence on an otherwise correctly bound audio channel.
 */
/*
 * -a playsweep=MS: walk the four (parity, byte-order) combinations while a
 * call is bound, so one call can identify the right one. Each change
 * re-locks the phase from the next received transition.
 */
static void play_sweep(struct stubd *s, uint64_t now)
{
	static const char *const name[6] = {
		"held word, phase A, octets in order",
		"held word, phase B, octets in order",
		"held word, phase A, octets swapped",
		"held word, phase B, octets swapped",
		"one octet per frame, low byte",
		"one octet per frame, high byte"
	};

	if (!s->reg.call_bound)
		return;
	if (s->audio.play_phase_ms) {
		if (s->sweep_at && now - s->sweep_at < s->audio.play_phase_ms)
			return;
		s->sweep_at = now;
		s->sweep_inv ^= 1;
		s->play_phase ^= 1;	/* shift the pair boundary by a frame */
		printf("call: play phase %c at t=%llu ms\n",
		       s->sweep_inv ? 'B' : 'A',
		       (unsigned long long)(now - g_start));
		fflush(stdout);
		return;
	}
	if (!s->audio.play_sweep_ms)
		return;
	if (s->sweep_at && now - s->sweep_at < s->audio.play_sweep_ms)
		return;
	s->sweep_at = now;
	s->sweep_idx %= 6u;
	s->sweep_inv = (int)(s->sweep_idx & 1u);
	s->sweep_swap = (int)((s->sweep_idx >> 1) & 1u);
	s->sweep_byte = s->sweep_idx >= 4u ? s->sweep_idx - 3u : 0u;
	printf("call: play combination %u/6 at t=%llu ms -- %s\n",
	       s->sweep_idx + 1u,
	       (unsigned long long)(now - g_start), name[s->sweep_idx]);
	fflush(stdout);
	s->sweep_idx = (s->sweep_idx + 1u) % 6u;
	s->play_locked = 0;		/* re-lock the phase for this variant */
	s->rx_word_prev = 0xFFFFFFFFu;
}

/*
 * One chip-memory read per tick, cycling through the configured ranges.
 * The reply (0xFFF1 sub 0x08) carries the address and the bytes and is
 * printed by the ordinary mail log, so no extra decoding lives here.
 */
/*
 * Second 0x0230 with flag bit 6 clear, once the path is on: asks the chip's
 * channel configurator for mode 2 instead of the mode 1 our bind leaves it
 * in. [CR A-0039]
 */
static void remode_tick(struct stubd *s, uint64_t now)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;

	if (!s->reg.remode_at || now < s->reg.remode_at)
		return;
	s->reg.remode_at = 0;
	s->reg.remode_done = 1;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0230u;
	m.param[0] = s->reg.remode_handle;
	m.param[1] = a->remode_bit6 ? a->flags
				    : (uint8_t)(a->flags & ~0x40u);
	m.param[2] = (uint8_t)(a->mask & 0xFFu);
	m.param[3] = (uint8_t)(a->mask >> 8);
	m.param[4] = a->mode;
	m.param[5] = a->remode_codec >= 0 ? (uint8_t)a->remode_codec
					  : a->codec;
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: re-assign for mode %d: 0x0230 {%02x %04x %02x %02x}\n",
	       a->remode_bit6 ? (m.param[5] == 1 ? 8 : 9) : 2,
	       m.param[1], a->mask, a->mode, m.param[5]);
	fflush(stdout);
}

/*
 * Walk the seqstep list, one 0x0230 per window. See audio_cfg.seq_ms.
 *
 * The handle comes from the bind, not from a literal. vF418 sent every window
 * to handle 1 while the call was on handle 0 -- handle 0 is a valid handle and
 * the old fallback could not tell it from "no call" -- and the chip answered
 * 0x0242 with channel 0x0f each time. Two rows of that run's codec table are
 * unusable because of it. seq_t0 is only nonzero while a call is bound, so by
 * the time we get here remode_handle is always the handle of that call.
 */
static void seq_tick(struct stubd *s, uint64_t now)
{
	const struct audio_cfg *a = &s->audio;
	struct dect_mail m;
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

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0230u;
	m.param[0] = s->reg.remode_handle;
	m.param[1] = (uint8_t)flags;
	m.param[2] = (uint8_t)(a->mask & 0xFFu);
	m.param[3] = (uint8_t)(a->mask >> 8);
	m.param[4] = a->mode;
	m.param[5] = (uint8_t)codec;
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: seq window %u begins (t+%u ms): 0x0230 {%02x %04x %02x %02x}\n",
	       s->reg.seq_idx + 1u, (unsigned)((s->reg.seq_idx + 1u) * a->seq_ms),
	       m.param[1], a->mask, a->mode, m.param[5]);
	fflush(stdout);
	s->reg.seq_idx++;
}

static void mem_dump_tick(struct stubd *s, uint64_t now)
{
	struct dect_mail m;
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

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_PCM_REMOTE;	/* debug/management task, as for sub 0x09/0x0A */
	m.prim = 0xFFF1u;		/* DEBUG_REQUEST */
	m.param[0] = 0x07u;		/* MEM_READ */
	m.param[1] = (uint8_t)(addr & 0xFFu);
	m.param[2] = (uint8_t)((addr >> 8) & 0xFFu);
	m.param[3] = (uint8_t)((addr >> 16) & 0xFFu);
	m.param[4] = (uint8_t)((addr >> 24) & 0xFFu);
	m.param[5] = (uint8_t)(count & 0xFFu);
	m.param[6] = (uint8_t)(count >> 8);
	m.param_len = 7;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

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

static void send_conn_status_req(struct stubd *s, uint64_t now)
{
	struct dect_mail m;

	if (!s->audio.status_ms || !s->reg.call_bound || !s->reg.active)
		return;
	if (s->reg.status_ms && now - s->reg.status_ms < s->audio.status_ms)
		return;
	s->reg.status_ms = now;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0xF624u;		/* connection status request */
	m.param[0] = s->reg.handle;
	m.param[1] = 0x0Au;		/* the selector AVM's monitor uses */
	m.param_len = 2;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
}

/* Send one GenDSP command: 0xFFF1 sub, with one or two argument bytes. */
static void send_gendsp(struct stubd *s, uint8_t sub, uint8_t a, uint8_t b,
			unsigned len)
{
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_PCM_REMOTE;
	m.prim = 0xFFF1u;
	m.param[0] = sub;
	m.param[1] = a;
	m.param[2] = b;
	m.param_len = (uint16_t)len;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
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
 */
/*
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

static uint8_t ring_page_info(const struct stubd *s)
{
	return (uint8_t)(RING_PAGE_CC |
			 ((s->audio.basic_svc & 0x0Fu) == 0x08u
			  ? RING_PAGE_WIDEBAND : 0u));
}

/*
 * Pages before giving up. 4 (12 s) was far too short: run F1 (2026-10-22)
 * paged at 45/48/51/54 s, gave up at 57 s, and the handset's first connection
 * came at 75.8 s -- it was still re-synchronising after the base had been off
 * the air between runs, which the handset shows as "Synchronisiere". Cover a
 * realistic re-sync instead, since an unanswered page costs nothing.
 */
static void pump_lapc(struct stubd *s);

#define RING_RELEASE_WAIT_MS	3000u	/* [CR A-0053] DLC arms a ~5 s guard */
#define RING_PAGE_RETRIES	20u	/* pages before giving up (60 s) */
#define RING_PAGE_INTERVAL	3000u	/* ms between them; the stock guard */
#define RING_PAGE_RESP_MS	1500u	/* wait this long for the page response */
#define RING_SETUP_RETRY_MS	2000u	/* re-try a refused CC-SETUP this often */
#define RING_SETUP_TRIES	8u	/* ... this many times before giving up */

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
	if (s->cur == 0 || !s->n_hs)
		cfg.sub_slot = stubd_sub_slot(s);
	else
		cfg.sub_slot = (uint8_t)(s->audio.sub_slot ? s->audio.sub_slot
							   : s->hs[0].slot);
	dect_mm_init(&mm, &cfg);
	dect_mm_tpui(&mm, tpui);
}

static void send_page(struct stubd *s)
{
	struct dect_mail m;
	uint8_t tpui[3];
	unsigned i;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0300u;
	/*
	 * The page carries the low 16 bits of the TPUI we assigned, so take
	 * them from MM rather than rebuilding them from the slot. With a
	 * pool-assigned TPUI the two are no longer the same number, and a page
	 * built from the slot would address the wrong handset.
	 */
	ring_target_tpui(s, tpui);
	m.param[0] = ring_page_info(s);
	m.param[1] = tpui[1];
	m.param[2] = tpui[2];
	m.param_len = 3;
	/*
	 * The optional fourth byte -- mail[5] -- from which [CR A-0091] says
	 * the builder takes bits 1-4 of the page header byte at 0x96ad+4.
	 * Off by default: the three-byte page is what every run so far has
	 * sent and what stock sends.
	 */
	if (s->audio.page_hdr >= 0) {
		m.param[3] = (uint8_t)s->audio.page_hdr;
		m.param_len = 4;
	}
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	if (s->audio.page_hdr >= 0)
		/*
		 * [CR A-0093]: the byte at 0x96ad+4 is composed, not copied --
		 * bit 0 is always set, bits 1-4 carry mail[5] bits 0-3, and
		 * bits 5-7 are a fixed init constant. So the header to compare
		 * against a reading is (byte >> 1) & 7, and the low five bits
		 * we expect are ((hdr & 0x0F) << 1) | 1.
		 */
		printf("ring: page carries mail[5] = 0x%02x -> expect LCE "
		       "header %u, 0x96ad+4 low bits 0x%02x (A-0091/93)\n",
		       (unsigned)s->audio.page_hdr,
		       (unsigned)(s->audio.page_hdr & 0x07),
		       (unsigned)(((s->audio.page_hdr & 0x0F) << 1) | 1));
	/*
	 * Whatever the caller wants sent straight after the page -- see the
	 * comment on page_mail. Sent here rather than from the poll loop so
	 * the ordering against 0x0300 is exact, which is the whole point.
	 */
	for (i = 0; i < s->audio.n_page_mail; i++) {
		printf("ring: page mail %u/%u: %s\n", i + 1u,
		       s->audio.n_page_mail, s->audio.page_mail[i]);
		if (send_mail_spec(s, s->audio.page_mail[i]))
			printf("ring: page mail %u FAILED to parse/send: %s\n",
			       i + 1u, s->audio.page_mail[i]);
	}
	printf("ring: paged the handset, attempt %u of %u "
	       "(0x0300 {%02x %02x %02x}, basic service %02x)%s\n",
	       s->ring_pages, RING_PAGE_RETRIES, ring_page_info(s),
	       tpui[1], tpui[2], s->audio.basic_svc,
	       /*
		* We page BY the TPUI we assigned. If the handset never
		* acknowledged the assignment (EN 300 175-5 §13.2.2,
		* {TEMPORARY-IDENTITY-ASSIGN-ACK}) it may not be listening for
		* it, and the page is silent in a way that looks exactly like a
		* handset that will not answer -- which we have chased more
		* than once. Say so rather than letting it look normal.
		*/
	       dect_mm_tpui_confirmed(&s->reg.link.mm)
		       ? "" : " -- WARNING: TPUI never acknowledged");
}

/*
 * Queue the FP-originated CC-SETUP.
 *
 * Run E1b (2026-10-22) showed why this must not be done the moment the chip
 * reports the link up: we sent the SETUP as an I-frame at t=90166 and the
 * handset's LCE page response was only parsed at t=90246, 80 ms later. The
 * handset answered RR with N(R)=0 -- it had not received the frame -- so the
 * I-frame was lost, LAPC retransmitted it a full second later, and the A540
 * released the call with RELEASE-REASON 0x32 "Insufficient resources" as soon
 * as the retransmission arrived. The SETUP therefore waits for the handset to
 * answer the page, which is also the order the paging procedure prescribes.
 */
static void ring_queue_setup(struct stubd *s, uint64_t now, const char *why)
{
	/*
	 * No link to send it on: the handsets that woke for the page took
	 * their links and dropped them again (12d, 2026-10-25: the SETUP was
	 * queued at 45.0 s with every link gone, and nothing rang). Page
	 * again; the page budget still bounds it.
	 */
	if (!s->reg.active) {
		printf("ring: no link up at the page-response deadline -- "
		       "paging again\n");
		s->ring_state = RING_PAGING;
		return;
	}
	/*
	 * Our call goes to the first store's handset. A link that has taken on
	 * another handset is that handset's; a {CC-SETUP} on it would address
	 * the wrong one. Wait for it to clear, as for a busy CC entity.
	 */
	if (s->cur != 0) {
		printf("ring: this link serves %s, not the handset we ring -- "
		       "waiting\n", s->sub_path);
		s->ring_resp_deadline = now + RING_SETUP_RETRY_MS;
		return;
	}
	/*
	 * dect_cc_setup_id() refuses unless the CC entity is idle, and this is
	 * a real collision rather than a rare one: the A540 CAT opens a CAT-iq
	 * service call of its own roughly every 17 s and holds it for about
	 * 13 s. Run F4 caught it exactly -- the handset's call ran 48.6 s to
	 * 61.6 s and our ring fired at 58.5 s -- so abandoning the ring on the
	 * first refusal loses the call about as often as not. Retry while the
	 * paging window is still open.
	 */
	/*
	 * EN 300 444 §6.9.2: the lowest free transaction value, which for our
	 * one-call CC entity is always 0. This used to rotate per call, and
	 * every Gigaset refused every incoming call for it: run 15 (value 0)
	 * rang the A540, run 16 (value 1, same session) was refused with 0x32.
	 * See dect_cc_set_next_ti(). -a ti=N still overrides, for tests.
	 */
	s->next_ti = s->audio.ti_fixed >= 0 ? (uint8_t)s->audio.ti_fixed : 0u;
	dect_cc_set_next_ti(&s->reg.link.cc, s->next_ti);
	if (!dect_cc_setup_id(&s->reg.link.cc, s->cc_port_id, s->cc_port_id_len,
			      s->cc_fixed_id, s->cc_fixed_id_len,
			      s->audio.basic_svc, &s->clip)) {
		s->ring_state = RING_DONE;
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


static void ring_tick(struct stubd *s, uint64_t now)
{
	if ((!s->audio.ring_ms && !s->audio.ring_attach_ms) ||
	    s->ring_state == RING_DONE || s->sess.state != DECT_SESS_UP)
		return;

	/*
	 * Waiting for the page response. Send the SETUP anyway once the
	 * deadline passes, so a handset that never answers the page the way
	 * we expect still gets a call rather than silence.
	 */
	if (s->ring_state == RING_SETUP_DELAY) {
		if (now >= s->ring_setup_at) {
			ring_queue_setup(s, now, "setupdelay elapsed");
			/*
			 * ring_queue_setup() leaves the state alone when the
			 * CC entity was busy, so without this we would come
			 * straight back here on the next tick with the
			 * deadline already past and burn the whole retry
			 * budget in a few hundred milliseconds. Hand the
			 * retry back to RING_WAIT_PAGE_RESP, which owns the
			 * RING_SETUP_RETRY_MS spacing.
			 */
			if (s->ring_state == RING_SETUP_DELAY)
				s->ring_state = RING_WAIT_PAGE_RESP;
		}
		return;
	}

	if (s->ring_state == RING_WAIT_PAGE_RESP) {
		if (now >= s->ring_resp_deadline)
			ring_queue_setup(s, now, s->ring_setup_tries
					 ? "retrying after a refusal"
					 : "no page response, deadline");
		return;
	}

	if (s->ring_state == RING_IDLE) {
		if (s->audio.ring_attach_ms) {
			/* Count from the handset's first contact, not ours. */
			if (!s->first_contact_ms ||
			    now - s->first_contact_ms < s->audio.ring_attach_ms)
				return;
		} else if (now - g_start < s->audio.ring_ms) {
			return;
		}
		if (!s->cc_fixed_id_len) {
			printf("ring: no call identities -- the handset is not "
			       "registered here, or -s is off so nothing was "
			       "stored\n");
			s->ring_state = RING_DONE;
			return;
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
		if (s->reg.active) {
			printf("ring: releasing the link before paging -- a "
			       "linked handset does not answer pages, and the "
			       "page is what makes the chip allocate a traffic "
			       "bearer\n");
			dect_lapc_disconnect(&s->reg.link.lapc);
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
		if (s->reg.active && now < s->ring_release_until)
			return;
		printf("ring: link %s -- paging now\n",
		       s->reg.active ? "did not go down in time, paging anyway"
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
		if (s->reg.active) {
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

/* Ask the chip to move this call's bearer (see audio_cfg.handover_ms). */
static void send_handover(struct stubd *s, uint64_t now)
{
	struct dect_mail m;

	if (!s->audio.handover_ms || s->reg.handover_sent ||
	    !s->reg.call_bound || !s->reg.active)
		return;
	if (now - s->reg.bind_ms < s->audio.handover_ms)
		return;
	s->reg.handover_sent = 1;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x016Au;		/* MAC trigger handover */
	m.param[0] = s->reg.handle;
	m.param_len = 1;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("call: handover: asked the chip to move the bearer "
	       "(0x016A on handle %u)\n", s->reg.handle);
}

/*
 * Walk the GenDSP source numbers into one destination during a live call, one
 * per interval, so a single call covers the whole source range: the daemon
 * already prints per-slot change counts once a second, so the log lines up
 * "which source" against "which lane moved".
 *
 * Measured with the radio off and no call [observed, 2026-10-19]: into
 * destination 6, which drives raw DMA word 0, sources 0, 21 and 22 leave the
 * lane at the 0xFEFE idle fill, 19 and 20 drive it to 0x7FFF and 0x8000 --
 * the extremes of 16-bit linear -- and every other source holds it at a
 * constant 0x0000 or 0xFFFF. So the lane carries 16-bit linear, it can reach
 * full scale, and a connected-but-silent source reads 0x0000, which is what a
 * bound call leaves there.
 *
 * When the sweep finishes it restores the default routes of channels 1..6
 * (sub 0x09, the documented range) rather than leaving the chip re-routed
 * [CR A-0038 §2].
 */
static void dsp_sweep(struct stubd *s, uint64_t now)
{
	const struct audio_cfg *a = &s->audio;

	if (a->sweep_dst < 0 || !s->reg.call_bound || !s->reg.active ||
	    s->reg.sweep_done)
		return;
	if (s->reg.sweep_ms && now - s->reg.sweep_ms < a->sweep_ms)
		return;
	s->reg.sweep_ms = now;

	if (s->reg.sweep_idx > 22u) {
		unsigned n;

		for (n = 1; n <= 6u; n++)
			send_gendsp(s, 0x09u, (uint8_t)n, 0, 2);
		printf("call: dsp sweep: done, default routes restored\n");
		s->reg.sweep_done = 1;
		return;
	}
	send_gendsp(s, 0x0Au, (uint8_t)a->sweep_dst,
		    (uint8_t)s->reg.sweep_idx, 3);
	printf("call: dsp sweep: dest %d <- src %u\n",
	       a->sweep_dst, s->reg.sweep_idx);
	s->reg.sweep_idx++;
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
	dect_diag_from_link(&s->reg.link, &d);
	if (d.handset_tag < 0)		/* serving the current store */
		d.handset_tag = (int)s->cur;
	if (dect_diag_json(&d, s->show_params, js, sizeof(js)))
		printf("diag: %s\n", js);
}

/*
 * Hand the call's security material to the chip (0x0208), so the STUB can
 * switch the radio to DSC. Layout traced to its producer in [CR A-0028 §1],
 * which corrects A-0027: offsets below are into the complete mail, so they are
 * two higher than the parameter indices used here.
 *
 *   mail[2]      selector (see below)
 *   mail[3..10]  the authentication worker's 8-byte computed result, BYTE-
 *                REVERSED as a group before the event that feeds this mail
 *   mail[11..14] its 4-byte computed result, likewise byte-reversed
 *   mail[15..22] the last 8 bytes of a 16-byte authentication *input*, copied
 *                intact -- an input, not a derived value
 *   mail[23..25] zeroed, mail[26..27] FF FF
 *   mail[28..29] EEPROM 0x51/0x52, whose defaults are 00 00
 *   mail[30..32] only in the 33-byte form: 0x40 and two per-terminal bytes
 *
 * Mapping those onto DSAA1: the two computed results are the 64-bit DCK and
 * the 32-bit RES, and the 16-byte input is the pair of 8-byte randoms, so its
 * last half is RAND_F (",rs" swaps in RS, in case the halves are the other way
 * round). A-0028 is explicit that the DCK identification is a candidate, not a
 * proven fact, so ",norev" keeps the unreversed order available too.
 *
 * Selector: A-0028 §2 found two producer branches. The live-call one takes a
 * context identifier from the authentication event's routing nibble -- which is
 * NOT proven equal to our MAC handle -- while the subscription-oriented branch
 * uses terminal index + 7, i.e. 7..12. It also withdraws A-0027's implication
 * that low selectors are safe: neither family is certified harmless without a
 * consumer audit, though the state lives in RAM-loaded firmware. So -k still
 * has to name the selector; the daemon never picks one.
 */
static void send_chip_cipher(struct stubd *s)
{
	struct dect_mail m;
	uint8_t dck[DECT_MM_DCK_LEN];
	size_t n = 0, i;

	if (!dect_mm_cipher_dck(&s->reg.link.mm, dck))
		return;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0208u;		/* chip security/cipher material */
	m.param[n++] = s->chip_cipher.selector;

	/* mail[3..10]: computed 8-byte result (DCK candidate), reversed. */
	for (i = 0; i < DECT_MM_DCK_LEN; i++)
		m.param[n + i] = s->chip_cipher.no_reverse
			? dck[i] : dck[DECT_MM_DCK_LEN - 1u - i];
	n += DECT_MM_DCK_LEN;

	/* mail[11..14]: computed 4-byte result (RES), reversed. */
	for (i = 0; i < 4u; i++)
		m.param[n + i] = s->chip_cipher.no_reverse
			? s->reg.link.mm.cipher_res[i] : s->reg.link.mm.cipher_res[3u - i];
	n += 4u;

	/* mail[15..22]: second half of the 16-byte input, copied intact. */
	memcpy(&m.param[n], s->chip_cipher.use_rs ? s->reg.link.mm.rs
						  : s->reg.link.mm.rand_f, 8);
	n += 8u;

	m.param[n++] = 0x00u;		/* zeroed by the producer */
	m.param[n++] = 0x00u;
	m.param[n++] = 0x00u;
	m.param[n++] = 0xFFu;		/* FF FF initialised by the producer */
	m.param[n++] = 0xFFu;
	m.param[n++] = 0x00u;		/* EEPROM 0x51, default 00 [CR A-0028] */
	m.param[n++] = 0x00u;		/* EEPROM 0x52, default 00 */
	if (s->chip_cipher.long_form) {
		m.param[n++] = 0x40u;	/* fixed in the 33-byte form */
		m.param[n++] = 0x00u;	/* per-terminal status/config */
		m.param[n++] = 0x00u;
	}
	m.param_len = (uint16_t)n;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	memset(dck, 0, sizeof(dck));
	memset(&m, 0, sizeof(m));	/* the mail held the DCK */
	/* Never log this mail's parameters: they carry the cipher key. */
	printf("call: cipher: chip command 0x0208 sent (selector %u, %u bytes, "
	       "%s order, input %s)\n",
	       s->chip_cipher.selector, (unsigned int)(n + 2),
	       s->chip_cipher.no_reverse ? "plain" : "reversed",
	       s->chip_cipher.use_rs ? "RS" : "RAND_F");
}

/* Flush any LAPC frames the FSM wants to transmit. */
/*
 * Move whatever the link has queued onto the wire. The framing, the Lc
 * signature and the 0x0244 mail are the library's (openDECTcell#67); ours is only
 * the transport.
 */
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
#define STATE_DEFAULT_PATH	"/tmp/opendectcell-state.json"
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
	if (d.handset_tag < 0 && l == &s->reg.link)
		d.handset_tag = (int)s->cur;	/* the current store */
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
	fprintf(f, ",\"handsets\":[");
	first = 1;
	for (i = 0; i < s->n_hs; i++) {
		unsigned k, linked = 0;
		uint64_t seen = s->hs[i].last_seen_ms;

		if (i != s->cur && !s->hs[i].loaded)
			continue;
		/*
		 * parked: no handset has a link, whatever was up at exit.
		 * adopted -1 means the link serves the current store itself
		 * (a call we placed to it): that handset is linked too, or the
		 * Monitor shows it "idle" mid-call (bench test 12a).
		 */
		if (radio_on && s->reg.active &&
		    (s->reg.link.mm.adopted == (int)i ||
		     (s->reg.link.mm.adopted < 0 && i == s->cur &&
		      s->reg.link.mm.port_id_len)))
			linked = 1;
		for (k = 0; radio_on && k < SIDE_MAX; k++)
			if (s->side[k].active && s->side[k].adopted == (int)i)
				linked = 1;
		fprintf(f, "%s{\"slot\":%u,\"name\":\"DECT %u\",\"store\":",
			first ? "" : ",", hs_slot(s, i), hs_slot(s, i));
		json_str(f, s->hs[i].path);
		fprintf(f, ",\"link\":%s", linked ? "true" : "false");
		/* in contact right now while it holds a link */
		if (seen || linked)
			fprintf(f, ",\"seen_ms_ago\":%llu", linked ? 0ull :
				(unsigned long long)(now - seen));
		fprintf(f, "}");
		first = 0;
	}
	fprintf(f, "],\"links\":[");
	first = 1;
	if (radio_on && s->reg.active)
		state_link(s, f, &s->reg.link,
			   s->reg.last_status.valid ? &s->reg.last_status
						    : NULL,
			   s->reg.frames_total, &first);
	for (i = 0; radio_on && i < SIDE_MAX; i++)
		if (s->side[i].active)
			state_link(s, f, &s->side[i].link, NULL, 0, &first);
	fprintf(f, "]}\n");
	if (fclose(f) == 0)
		(void)rename(tmp, path);
	else
		unlink(tmp);
}

static void pump_lapc(struct stubd *s)
{
	struct dect_mail m;

	/*
	 * Take a frame out of LAPC only when the control queue can hold a
	 * whole mail: once dequeued it is gone, and LAPC retransmits a lost
	 * I-frame by timer but never a lost RR or UA. send_mail() used to be
	 * called with its result ignored (second external review, finding 2).
	 */
	while (sizeof(s->txq) - s->txq_len >= DECT_TX_ELEM_MAX &&
	       dect_link_poll_append_mail(&s->reg.link, &m)) {
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

	for (i = 0; i < s->n_hs; i++)
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
	return s->sub_port_id_len ? DECT_MM_REG_SAME : DECT_MM_REG_OPEN;
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
	if (s->reg.active)
		s->reg.link.mm.cfg.registration = reg_policy(s);
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
	const uint8_t *id = s->reg.link.mm.pend_id;
	size_t len = s->reg.link.mm.pend_id_len, l;
	unsigned i;
	int target = -1;
	char name[DECT_IWU_NAME_MAX];

	for (i = 0; i < s->n_hs && target < 0; i++) {
		const uint8_t *p = hs_port_id(s, i, &l);

		if (len && l == len && !memcmp(p, id, len))
			target = (int)i;
	}
	if (target < 0)
		target = hs_free(s);
	if (target < 0 || (unsigned)target == s->cur) {
		printf("reg: enrolment into %s (slot %u)\n", s->sub_path,
		       (unsigned)stubd_sub_slot(s));
		return;
	}
	hs_switch(s, (unsigned)target, " (enrolment)");
	s->reg.enrol_switched = 1;	/* undone if the PIN fails */
	s->reg.link.mm.cfg.sub_slot = stubd_sub_slot(s);
	snprintf(name, sizeof(name), "DECT %u", (unsigned)stubd_sub_slot(s));
	dect_iwu_add_handset(&s->reg.link.cc.iwu, stubd_sub_slot(s), name);
	dect_iwu_set_own(&s->reg.link.cc.iwu, stubd_sub_slot(s));
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

	dect_cc_init(&s->reg.link.cc);
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

		dect_iwu_set_line(&s->reg.link.cc.iwu, 1, "Line 1");
		snprintf(hsname, sizeof(hsname), "DECT %u", (unsigned)tid);
		dect_iwu_add_handset(&s->reg.link.cc.iwu, tid, hsname);
		for (i = 0; i < s->n_hs; i++) {
			if (i == s->cur || !s->hs[i].loaded)
				continue;
			snprintf(hsname, sizeof(hsname), "DECT %u",
				 s->hs[i].slot);
			dect_iwu_add_handset(&s->reg.link.cc.iwu,
					     (uint8_t)s->hs[i].slot, hsname);
		}
		dect_iwu_set_own(&s->reg.link.cc.iwu, tid);
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
	if (s->audio.codeclist_len && !s->audio.nocodeclist)
		dect_cc_set_codec_list(&s->reg.link.cc, s->audio.codeclist,
				       s->audio.codeclist_len);
	else
		dect_cc_set_codec_list(&s->reg.link.cc, NULL, 0);
	dect_cc_set_signal(&s->reg.link.cc, s->audio.signal_on,
			   s->audio.signal_value);
	dect_cc_set_wideband(&s->reg.link.cc, !s->audio.narrowband);
	dect_cc_set_overlap(&s->reg.link.cc, !s->audio.no_overlap);
	dect_cc_set_call_status(&s->reg.link.cc, !s->audio.no_call_status,
				s->audio.callinfo.has_call_id ?
				s->audio.callinfo.call_id : 1u);
	dect_cc_set_callinfo(&s->reg.link.cc, &s->audio.callinfo);
	/* the lowest free value, 0, unless -a ti= says otherwise */
	dect_cc_set_next_ti(&s->reg.link.cc, s->next_ti);
}

static void xlane_release(struct stubd *s, struct side_link *sl);

static void reg_start(struct stubd *s, uint8_t handle)
{
	struct dect_mm_cfg mcfg;
	unsigned i;

	/* a new connection is nobody's yet; default to the ring target */
	hs_switch(s, 0, " (new connection)");
	{
		struct side_link *sl = side_find(s, handle);

		if (sl) {
			xlane_release(s, sl);	/* never leak its lane */
			sl->active = 0;		/* the main link takes it */
		}
	}
	memset(&mcfg, 0, sizeof(mcfg));
	/* subslot=N wins, else the slot the store was saved with, else 0 --
	 * resolved once, in stubd_sub_slot(). */
	mcfg.sub_slot = stubd_sub_slot(s);
	if (s->pin)
		snprintf(mcfg.pin, sizeof(mcfg.pin), "%s", s->pin);
	mcfg.authenticate = 1;
	mcfg.narrowband_only = (uint8_t)s->audio.nbcodec;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);
	/*
	 * Who may enrol. Closed with noregopen. With the first store already
	 * holding a handset, only that handset may re-enrol into it (or a held
	 * one, once adopted, into its own): a new handset used to land in the
	 * first store and overwrite the ring target (review, 2026-10-25). A
	 * new handset needs a store of its own as the first -s.
	 */
	mcfg.defer_unknown = s->audio.remote_path != NULL;
	mcfg.locate_validity = (uint8_t)s->audio.locate_validity;
	s->fetch_retrans_seen = 0;	/* MM's counter restarts with the link */
	mcfg.registration = reg_policy(s);

	s->reg.active = 1;
	s->reg.persisted = 0;
	s->reg.call_bound = 0;
	s->reg.call_wideband = 0;
	s->reg.connect_staged = 0;
	s->reg.nonspeech_noted = 0;
	s->reg.enrol_routed = 0;
	s->reg.enrol_switched = 0;
	s->reg.cipher_started = 0;
	s->reg.cipher_done = 0;
	s->reg.handle = handle;
	dect_link_init(&s->reg.link, handle, &mcfg);
	/*
	 * Re-arm after every init: dect_link_init() zeroes the whole struct,
	 * so the hook has to be set per connection, not once at startup.
	 */
	dect_link_set_tx_log(&s->reg.link, log_nwk_out, s);
	/*
	 * dect_link_init() also zeroed mm.stats.located. Without resetting
	 * our own watermark the comparison below reads 0 != N and fires a
	 * date-and-time send on every connection setup, with no location
	 * registration behind it -- which is exactly what vF439 did: 14 sends
	 * for 7 LOCATE-ACCEPTs, the extras landing right after
	 * "connection up".
	 *
	 * Found by running it. A counter compared across a re-init is a trap
	 * no unit test of either side would have caught.
	 */
	s->reg.time_sent_for = 0;
	/*
	 * Hand the MM state machine the identity we are subscribed to, so a
	 * location registration from a handset we do not hold is rejected.
	 * Must come AFTER dect_mm_init(), which clears it.
	 */
	if (s->sub_port_id_len) {
		memcpy(s->reg.link.mm.port_id, s->sub_port_id, s->sub_port_id_len);
		s->reg.link.mm.port_id_len = s->sub_port_id_len;
	}
	/*
	 * ...and every other handset we hold, so its location registration
	 * and its calls are served rather than turned away. MM adopts
	 * whichever one this link turns out to carry; reg_segment() follows.
	 */
	for (i = 0; i < s->n_hs; i++) {
		const struct handset *h = &s->hs[i];

		if (i == s->cur || !h->loaded || !h->port_id_len ||
		    (h->remote && !h->fetched))
			continue;
		if (dect_mm_hold(&s->reg.link.mm, h->port_id, h->port_id_len,
				 (uint8_t)h->slot, NULL, (int)i) < 0)
			printf("sub: could not hold %s on this link\n",
			       h->path);
	}
	if (s->sub_port_id_len)
		(void)dect_mm_hold(&s->reg.link.mm, s->sub_port_id,
				   s->sub_port_id_len, stubd_sub_slot(s), NULL,
				   (int)s->cur);
	main_cc_setup(s);
	printf("reg: connection up (handle %u), C-plane armed\n", handle);
}

/* Slot and identity of handset table entry i, current or stashed. */
static unsigned hs_slot(const struct stubd *s, unsigned i)
{
	if (i == s->cur)
		return stubd_sub_slot(s);
	if (i == 0 && s->audio.sub_slot)	/* subslot= is the target's */
		return s->audio.sub_slot;
	return s->hs[i].slot;
}

static const uint8_t *hs_port_id(const struct stubd *s, unsigned i,
				 size_t *len)
{
	if (i == s->cur) {
		*len = s->sub_port_id_len;
		return s->sub_port_id;
	}
	*len = s->hs[i].port_id_len;
	return s->hs[i].port_id;
}

static struct side_link *side_find(struct stubd *s, uint8_t handle)
{
	unsigned i;

	for (i = 0; i < SIDE_MAX; i++)
		if (s->side[i].active && s->side[i].handle == handle)
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
	unsigned i;

	if (sl) {
		printf("side: connection up (handle %u) repeated -- keeping "
		       "its side link\n", handle);
		return 0;
	}
	for (i = 0; i < SIDE_MAX && !sl; i++)
		if (!s->side[i].active)
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
	mcfg.narrowband_only = (uint8_t)s->audio.nbcodec;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);
	dect_link_init(&sl->link, handle, &mcfg);
	dect_link_set_tx_log(&sl->link, log_nwk_out_side, s);

	/* every handset we hold; strangers meet the gates as usual */
	dect_iwu_set_line(&sl->link.cc.iwu, 1, "Line 1");
	for (i = 0; i < s->n_hs; i++) {
		size_t len;
		const uint8_t *id = hs_port_id(s, i, &len);
		char name[DECT_IWU_NAME_MAX];

		if (!len || (i != s->cur && !s->hs[i].loaded) ||
		    (s->hs[i].remote && !s->hs[i].fetched))
			continue;
		(void)dect_mm_hold(&sl->link.mm, id, len,
				   (uint8_t)hs_slot(s, i), NULL, (int)i);
		snprintf(name, sizeof(name), "DECT %u", hs_slot(s, i));
		dect_iwu_add_handset(&sl->link.cc.iwu, (uint8_t)hs_slot(s, i),
				     name);
	}
	dect_cc_set_signal(&sl->link.cc, s->audio.signal_on,
			   s->audio.signal_value);
	/* no audio path here, unless -a twocalls gives it a lane */
	dect_cc_set_busy(&sl->link.cc, !s->audio.two_calls);
	dect_cc_set_wideband(&sl->link.cc, !s->audio.narrowband);
	dect_cc_set_overlap(&sl->link.cc, !s->audio.no_overlap);
	dect_cc_set_call_status(&sl->link.cc, !s->audio.no_call_status,
				s->audio.callinfo.has_call_id ?
				s->audio.callinfo.call_id : 1u);
	sl->lane = -1;
	sl->ic_tried = 0;
	sl->dial_len = 0;
	sl->dial_change_ms = 0;
	sl->active = 1;
	sl->handle = handle;
	sl->adopted = -1;
	sl->rejected_seen = 0;
	s->side_started++;
	printf("side: connection up (handle %u) -- handle %u is busy, so it "
	       "gets a side link (location registration and list access "
	       "only)\n", handle, s->reg.handle);
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
 * one free lane used to admit any number of calls).
 */
static int lanes_available(const struct stubd *s, const struct side_link *me)
{
	int n = 0;
	unsigned i;

	for (i = 0; i < XLANE_MAX; i++)
		if (!s->xl[i].used)
			n++;
	for (i = 0; i < SIDE_MAX; i++) {
		const struct side_link *o = &s->side[i];

		if (o != me && o->active && o->lane < 0 &&
		    dect_cc_busy(&o->link.cc))
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
	struct dect_mail m;
	uint16_t mask;

	memset(x, 0, sizeof(*x));
	x->used = 1;
	x->handle = sl->handle;
	x->channel = a->lane_ch[i];
	x->word = a->lane_word[i];
	x->wideband = !a->narrowband &&
		dect_cc_codec(&sl->link.cc) == DECT_CC_CODEC_G722;
	sl->lane = i;
	mask = (uint16_t)(1u << x->channel);

	if (!a->no_gain) {
		memset(&m, 0, sizeof(m));
		m.src = DECT_TASK_PCM;
		m.dst = DECT_TASK_CONTACT_DST;
		m.prim = 0x0251u;
		m.param[0] = x->handle;
		m.param[1] = x->channel;	/* selector */
		m.param[2] = a->gain;
		m.param_len = 3;
		send_mail(s, (unsigned int)s->tx_slot, &m);
		if (s->verbose)
			log_mail(&m, "tx M", s->show_params);
	}
	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0230u;
	m.param[0] = x->handle;
	m.param[1] = (uint8_t)((a->flags & 0xB0u) |
			       (1u << (x->channel - 1u)) |
			       (x->wideband ? 0x40u : 0u));
	m.param[2] = (uint8_t)(mask & 0xFFu);
	m.param[3] = (uint8_t)(mask >> 8);
	m.param[4] = a->mode;
	m.param[5] = x->wideband ? 0u : a->codec;
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("side: second call on handle %u: lane %d, channel %u, TDM word "
	       "%u, %s -- 0x0230 {%02x %04x %02x %02x}\n", x->handle, i,
	       x->channel, x->word, x->wideband ? "wideband" : "narrowband",
	       m.param[1], mask, m.param[4], m.param[5]);
}

static void xlane_connect(struct stubd *s, struct xlane *x)
{
	struct dect_mail m;

	if (x->connected)
		return;
	x->connected = 1;
	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x022Du;
	m.param[0] = x->handle;
	m.param[1] = 0x00u;
	m.param_len = 2;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("side: audio connect: 0x022D on handle %u (channel %u)\n",
	       x->handle, x->channel);
}

static void xlane_release(struct stubd *s, struct side_link *sl)
{
	if (sl->lane < 0)
		return;
	if (s->xl[sl->lane].connected || s->xl[sl->lane].used)
		send_audio_disconnect(s, sl->handle);
	memset(&s->xl[sl->lane], 0, sizeof(s->xl[0]));
	sl->lane = -1;
}

/*
 * -a twocalls on a side link: take speech while a lane is free (else the
 * busy refusal stays), accept a dialled call after dialwait as the main
 * link does, bind once it is connected, release when it ends.
 */
static int ic_start(struct stubd *s, int side, uint64_t now);

static void side_call_tick(struct stubd *s, struct side_link *sl, uint64_t now)
{
	struct dect_cc *cc = &sl->link.cc;

	if (!s->audio.two_calls)
		return;
	if (!dect_cc_busy(cc))	/* decided for the next SETUP only */
		dect_cc_set_busy(cc, sl->lane < 0 &&
				 lanes_available(s, sl) <= 0);
	if (cc->state == DECT_CC_ST_PROCEEDING && !cc->incoming &&
	    !sl->ic_tried && (cc->peer_basic_service >> 4) == 0x9u) {
		sl->ic_tried = 1;	/* sent complete: route it now */
		(void)ic_start(s, (int)(sl - s->side), now);
		dect_link_drain(&sl->link, now);
		side_pump(s, sl);
	}
	if (!dect_cc_busy(cc))
		sl->ic_tried = 0;
	if (cc->state == DECT_CC_ST_OVERLAP) {
		size_t len;

		(void)dect_cc_keypad(cc, &len);
		if (!sl->dial_change_ms || len != sl->dial_len) {
			sl->dial_len = len;
			sl->dial_change_ms = now;
		} else if (len && now - sl->dial_change_ms >=
			   s->audio.dial_wait_ms) {
			printf("side: dialled on handle %u -- connecting\n",
			       sl->handle);
			sl->ic_tried = 1;
			if (ic_start(s, (int)(sl - s->side), now) ||
			    dect_cc_accept(cc) == 0) {
				dect_link_drain(&sl->link, now);
				side_pump(s, sl);
			}
		}
	} else {
		sl->dial_len = 0;
		sl->dial_change_ms = 0;
	}
	if (sl->lane < 0 && cc->state == DECT_CC_ST_ACTIVE &&
	    (cc->incoming || dect_cc_peer_is_speech(cc))) {
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
	return side < 0 ? &s->reg.link.cc : &s->side[side].link.cc;
}

/* The CC identities of handset i, current or stashed. */
static void hs_cc_ids(const struct stubd *s, unsigned i, const uint8_t **port,
		      size_t *plen, const uint8_t **fixed, size_t *flen)
{
	if (i == s->cur) {
		*port = s->cc_port_id; *plen = s->cc_port_id_len;
		*fixed = s->cc_fixed_id; *flen = s->cc_fixed_id_len;
	} else {
		*port = s->hs[i].cc_port_id; *plen = s->hs[i].cc_port_id_len;
		*fixed = s->hs[i].cc_fixed_id; *flen = s->hs[i].cc_fixed_id_len;
	}
}

static int ic_active(const struct stubd *s, int p)
{
	return p == -1 ? s->reg.active : p >= 0 && s->side[p].active;
}

static struct dect_link *ic_link(struct stubd *s, int p)
{
	return p == -1 ? &s->reg.link : &s->side[p].link;
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
	struct dect_mail m;

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0300u;
	m.param[0] = RING_PAGE_CC | RING_PAGE_WIDEBAND;
	m.param[1] = s->ic.tpui[1];
	m.param[2] = s->ic.tpui[2];
	m.param_len = 3;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
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
		printf("intern: internal call refused (%s)\n",
		       s->audio.two_calls ? "one at a time" :
		       "needs -a twocalls");
		(void)dect_cc_release(cc);
		return 1;
	}
	/* lanes: the target lands on a side link; a side caller needs its own */
	if (lanes_available(s, side >= 0 ? &s->side[side] : NULL) <
	    (side >= 0 ? 2 : 1)) {
		printf("intern: no free audio channel -- releasing\n");
		(void)dect_cc_release(cc);
		return 1;
	}
	for (i = 0; i < s->n_hs; i++)
		if (s->hs[i].loaded && hs_slot(s, i) == slot)
			break;
	if (i >= s->n_hs || (side < 0 && i == s->cur) ||
	    (side >= 0 && (int)i == s->side[side].adopted)) {
		printf("intern: no other handset in slot %u -- releasing\n",
		       slot);
		(void)dect_cc_release(cc);
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

/* The target answered our page on side link sl (NULL: the main link). */
static void ic_target_link(struct stubd *s, struct side_link *sl,
			   const struct dect_link_ind *ind, uint64_t now)
{
	const uint8_t *port, *fixed;
	size_t plen, flen;

	if (s->ic.st != IC_PAGING)
		return;
	hs_cc_ids(s, (unsigned)s->ic.target, &port, &plen, &fixed, &flen);
	if (!plen || ind->page_id_len != plen ||
	    memcmp(ind->page_id, port, plen))
		return;
	ic_setup(s, sl ? (int)(sl - s->side) : -1, now, "answered the page");
}

/*
 * Place our SETUP to the target on its side link -- after its page
 * response, or once the link has adopted it (the A540 answers a page with
 * a location registration, M2' and 12b).
 */
static void ic_setup(struct stubd *s, int p, uint64_t now, const char *how)
{
	const uint8_t *port, *fixed;
	size_t plen, flen;
	struct dect_link *l = ic_link(s, p);

	hs_cc_ids(s, (unsigned)s->ic.target, &port, &plen, &fixed, &flen);
	if (!plen || p == s->ic.caller || dect_cc_busy(&l->cc))
		return;
	dect_cc_set_next_ti(&l->cc, 0);
	if (dect_cc_setup_id(&l->cc, port, plen, fixed, flen,
			     s->audio.narrowband ? 0x90u : IC_BASIC_SVC, NULL)) {
		printf("intern: could not queue the SETUP to the target\n");
		return;
	}
	s->ic.tside = p;
	s->ic.st = IC_SETUP;
	printf("intern: the target %s on %s link %u -- SETUP sent\n", how,
	       p == -1 ? "the main" : "side", p == -1 ? s->reg.handle :
	       s->side[p].handle);
	ic_drain(s, p, now);
}

static void ic_end(struct stubd *s, const char *why)
{
	printf("intern: call ended (%s)\n", why);
	s->ic.st = IC_IDLE;
	s->ic.tside = IC_NONE;
}

static int ic_wideband(const struct stubd *s, int p);

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
			if (s->side[k].active &&
			    s->side[k].adopted == s->ic.target &&
			    !dect_cc_busy(&s->side[k].link.cc))
				ic_setup(s, (int)k, now,
					 "came up (adopted, no page response)");
		/* or the main link, if the caller is not on it (finding 5) */
		if (s->ic.st == IC_PAGING && s->ic.caller != -1 &&
		    s->reg.active && !dect_cc_busy(&s->reg.link.cc) &&
		    (s->reg.link.mm.adopted == s->ic.target ||
		     (s->reg.link.mm.adopted < 0 &&
		      s->cur == (unsigned)s->ic.target &&
		      s->reg.link.mm.port_id_len)))
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
	}
	if (s->ic.st != IC_UP && t->state == DECT_CC_ST_ACTIVE) {
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
		w = s->reg.call_bound ? (int)s->audio.tone_word : -1;
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
	       dect_link_poll_append_mail(&sl->link, &m)) {
		if (send_mail(s, (unsigned int)s->tx_slot, &m))
			printf("side: C-plane mail LOST (%u bytes)\n",
			       (unsigned)m.param_len);
		else
			s->led_traffic++;
	}
}

static void ring_queue_setup(struct stubd *s, uint64_t now, const char *why);
static void main_cc_setup(struct stubd *s);
static void ic_target_link(struct stubd *s, struct side_link *sl,
			   const struct dect_link_ind *ind, uint64_t now);

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
	if (s->ring_state != RING_WAIT_PAGE_RESP || !s->reg.active ||
	    !tgt_len || ind->page_id_len != tgt_len ||
	    memcmp(ind->page_id, tgt, tgt_len) != 0 ||
	    s->reg.call_bound || dect_cc_busy(&s->reg.link.cc) ||
	    s->reg.link.mm.state == DECT_MM_WAIT_AUTH ||
	    dect_cc_busy(&sl->link.cc))
		return 0;
	old_main = s->reg.link.mm.adopted >= 0 ? s->reg.link.mm.adopted :
		   s->reg.link.mm.port_id_len ? (int)s->cur : -1;
	tmp = s->reg.link;
	s->reg.link = sl->link;
	sl->link = tmp;
	h = s->reg.handle;
	s->reg.handle = sl->handle;
	sl->handle = h;
	sl->adopted = old_main;
	dect_link_set_tx_log(&s->reg.link, log_nwk_out, s);
	dect_link_set_tx_log(&sl->link, log_nwk_out_side, s);

	/* the old main is a side link now: no enrolment, no speech */
	sl->link.mm.cfg.registration = DECT_MM_REG_CLOSED;
	dect_cc_set_busy(&sl->link.cc, 1);
	sl->rejected_seen = sl->link.cc.stats.rejected_identity;

	/*
	 * The new main serves the target: the page response matched the ring
	 * target (hs[0]), whether or not its link has adopted anyone yet --
	 * a page response does not adopt (review, finding 2).
	 */
	hs_switch(s, s->reg.link.mm.adopted >= 0 ?
		  (unsigned)s->reg.link.mm.adopted : 0u,
		  " (promoted side link)");
	s->reg.link.mm.cfg.registration = reg_policy(s);
	s->reg.link.mm.cfg.defer_unknown = s->audio.remote_path != NULL;
	s->reg.link.mm.cfg.locate_validity =
		(uint8_t)s->audio.locate_validity;
	main_cc_setup(s);
	s->reg.call_wideband = 0;
	s->reg.connect_staged = 0;
	s->reg.nonspeech_noted = 0;
	s->reg.enrol_routed = 0;
	s->reg.enrol_switched = 0;
	/* what reg_start() resets for a new connection (review, finding 8) */
	s->reg.persisted = 0;
	s->reg.cipher_started = 0;
	s->reg.cipher_done = 0;
	s->reg.audio_connected = 0;
	s->reg.time_sent_for = 0;
	printf("ring: page response on side link %u -- promoted to the main "
	       "link; handle %u (%s) is a side link now\n", s->reg.handle,
	       sl->handle, (unsigned)old_main < s->n_hs ?
	       s->hs[old_main].path : "?");
	ring_queue_setup(s, now, "page response received on a side link");
	dect_link_drain(&s->reg.link, now);
	pump_lapc(s);
	side_pump(s, sl);
	return 1;
}

static void side_segment(struct stubd *s, struct side_link *sl,
			 const struct dect_mail *m, uint64_t now)
{
	struct dect_link_ind ind;
	uint8_t rand_f[8], rs[16];
	int a;

	gen_rand(s, rand_f, sizeof(rand_f));
	gen_rand(s, rs, sizeof(rs));
	side_call_tick(s, sl, now);	/* busy or not, before a SETUP lands */
	if (dect_link_rx_segment(&sl->link, m, now, rand_f, rs, &ind) < 0) {
		side_pump(s, sl);
		return;
	}
	side_call_tick(s, sl, now);
	a = sl->link.mm.adopted;
	if (a >= 0 && (unsigned)a < s->n_hs)
		s->hs[a].last_seen_ms = now;
	if (a >= 0 && a != sl->adopted && (unsigned)a < s->n_hs) {
		sl->adopted = a;
		dect_iwu_set_own(&sl->link.cc.iwu,
				 (uint8_t)hs_slot(s, (unsigned)a));
		printf("side: handle %u serves %s (slot %u)\n", sl->handle,
		       s->hs[a].path, hs_slot(s, (unsigned)a));
	}
	if (sl->link.cc.stats.rejected_identity != sl->rejected_seen) {
		sl->rejected_seen = sl->link.cc.stats.rejected_identity;
		printf("side: turned away a call from a handset we are not "
		       "subscribed to (handle %u, release reason 0x0a)\n",
		       sl->handle);
	}
	/*
	 * Ringing has started and a locating handset took the main link
	 * first, so the target's page response lands here (12d, 2026-10-25:
	 * the ring timed out). Promote this link to the main one and ring on
	 * it; anything else that answers here is not ours to ring.
	 */
	if (ind.page_response && s->ic.st == IC_PAGING) {
		ic_target_link(s, sl, &ind, now);
		if (s->ic.st == IC_SETUP)
			return;
	}
	if (ind.page_response && side_promote(s, sl, &ind, now))
		return;
	if (ind.page_response)
		printf("side: page response on handle %u while the main link "
		       "is busy -- not ours to ring here, ignored\n",
		       sl->handle);
	side_pump(s, sl);
}

/* Drain queued NWK responses (MM and CC) into LAPC, one per free window. */
static void drain_nwk(struct stubd *s, uint64_t now)
{
	dect_link_drain(&s->reg.link, now);
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
static void reg_segment(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	uint8_t rand_f[8], rs[16];
	struct dect_link_ind ind;
	uint32_t collisions = s->reg.link.cc.stats.collisions;
	uint32_t represented = s->reg.link.cc.stats.represented;

	gen_rand(s, rand_f, sizeof(rand_f));
	gen_rand(s, rs, sizeof(rs));

	if (dect_link_rx_segment(&s->reg.link, m, now, rand_f, rs, &ind) < 0) {
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
				       ind.seg_handle, s->reg.handle);
			s->foreign_seg++;
			s->foreign_seg_handle = ind.seg_handle;
		}
		return;
	}
	pump_lapc(s);			/* at least the LAPC ack */
	if (s->reg.link.mm.adopted >= 0 &&
	    (unsigned)s->reg.link.mm.adopted < s->n_hs)
		s->hs[s->reg.link.mm.adopted].last_seen_ms = now;

	/*
	 * MM took on one of the other handsets we hold (its LOCATE-REQUEST,
	 * or a call it placed). Serve it as itself: its store, its call
	 * identities, and its own entry in the Internal Names List, set before
	 * the list-access session that follows its {CC-SETUP} asks for it.
	 */
	if (s->reg.link.mm.adopted >= 0 &&
	    (unsigned)s->reg.link.mm.adopted != s->cur) {
		hs_switch(s, (unsigned)s->reg.link.mm.adopted, "");
		dect_iwu_set_own(&s->reg.link.cc.iwu, stubd_sub_slot(s));
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
	if (s->reg.link.cc.stats.collisions != collisions)
		printf("ring: {CC-SETUP} COLLISION -- the handset opened a call "
		       "of its own while ours was pending; releasing its call "
		       "with \"insufficient resources\" (EN 300 175-5 9.5.2.3)\n");
	if (s->reg.link.cc.stats.represented != represented)
		printf("ring: the handset released our call for the collision; "
		       "RE-PRESENTING the {CC-SETUP} now (9.5.2.3), %u left\n",
		       s->reg.link.cc.represent_left);

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

	/* an internal call's target answering on the main link (finding 5) */
	if (ind.page_response && s->ic.st == IC_PAGING && s->ic.caller != -1) {
		ic_target_link(s, NULL, &ind, now);
		if (s->ic.st == IC_SETUP)
			ind.page_response = 0;
	}

	if (ind.page_response && s->ring_state == RING_WAIT_PAGE_RESP &&
	    tgt_len && (ind.page_id_len != tgt_len ||
			memcmp(ind.page_id, tgt, tgt_len) != 0)) {
		int rejected = dect_link_page_reject(&s->reg.link, ind.page_id,
						     ind.page_id_len,
						     ind.page_ti) == 0;

		if (rejected) {
			dect_link_drain(&s->reg.link, now);
			rejected = !s->reg.link.lce_len;
		}
		if (rejected) {
			dect_lapc_disconnect(&s->reg.link.lapc);
			s->page_rejects++;
		}
		pump_lapc(s);
		printf("ring: page response from a handset we did not page "
		       "(identity differs) -- %s, still waiting\n",
		       rejected ? "LCE-PAGE-REJECT sent, link released"
				: "could not send LCE-PAGE-REJECT, ignored");
		ind.page_response = 0;
	}
	if (ind.page_response && s->ring_state == RING_WAIT_PAGE_RESP) {
		/*
		 * Give the connection a U-plane before offering speech over
		 * it. See the earlybind comment in struct audio_cfg.
		 */
		if (s->audio.earlybind && !s->reg.call_bound) {
			printf("ring: early audio bind on handle %u, before "
			       "the CC-SETUP\n", s->reg.handle);
			send_audio_bind(s, s->reg.handle);
			s->reg.call_bound = 1;
		}
		if (s->audio.setup_delay_ms) {
			s->ring_state = RING_SETUP_DELAY;
			s->ring_setup_at = now + s->audio.setup_delay_ms;
			printf("ring: page answered -- holding the CC-SETUP "
			       "for %u ms to let the MAC finish the bearer\n",
			       s->audio.setup_delay_ms);
		} else {
			ring_queue_setup(s, now, "page response received");
			/*
			 * Push it out in the same breath. The main loop would
			 * now get it within POLL_MS anyway, but the handset
			 * has just answered the page and is listening: the
			 * whole point of waiting for the page response is to
			 * hit that window.
			 */
			drain_nwk(s, now);
		}
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
		const uint8_t *kp = dect_cc_keypad(&s->reg.link.cc, &n);
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
		       s->reg.link.cc.keypad_overflow ? " [TRUNCATED]" : "");
		fflush(stdout);
	}

	if (s->reg.link.cc.stats.rejected_identity != s->cc_rejected_seen) {
		s->cc_rejected_seen = s->reg.link.cc.stats.rejected_identity;
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
	if (ind.cc_message && !s->cc_id_learned &&
	    s->reg.link.cc.peer_fixed_id_len &&
	    !dect_cc_peer_is(&s->reg.link.cc, s->sub_port_id,
			     s->sub_port_id_len)) {
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
	} else if (ind.cc_message && !s->cc_id_learned &&
		   s->reg.link.cc.peer_fixed_id_len && !s->sub_port_id_len) {
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
	} else if (ind.cc_message && !s->cc_id_learned &&
		   s->reg.link.cc.peer_fixed_id_len) {
		memcpy(s->cc_port_id, s->reg.link.cc.peer_port_id,
		       sizeof(s->cc_port_id));
		s->cc_port_id_len = s->reg.link.cc.peer_port_id_len;
		memcpy(s->cc_fixed_id, s->reg.link.cc.peer_fixed_id,
		       sizeof(s->cc_fixed_id));
		s->cc_fixed_id_len = s->reg.link.cc.peer_fixed_id_len;
		s->cc_id_learned = 1;
		printf("call: learned the identities for calling the handset "
		       "(%zu + %zu bytes), saving\n",
		       s->cc_port_id_len, s->cc_fixed_id_len);
		sub_save(s);
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
	if (!s->cc_fixed_id_len && s->have_rfpi && s->sub_port_id_len &&
	    s->sub_port_id_len <= sizeof(s->cc_port_id)) {
		uint8_t fid[DECT_CC_ID_MAX];
		size_t n = dect_mm_fixed_identity(&s->reg.link.mm, fid,
						  sizeof(fid));

		if (n) {
			/* this handset's identity, not the link MM's */
			memcpy(s->cc_port_id, s->sub_port_id,
			       s->sub_port_id_len);
			s->cc_port_id_len = s->sub_port_id_len;
			memcpy(s->cc_fixed_id, fid, n);
			s->cc_fixed_id_len = n;
			printf("call: derived the identities for calling the "
			       "handset (%zu + %zu bytes) from the "
			       "subscription, saving\n",
			       s->cc_port_id_len, s->cc_fixed_id_len);
			sub_save(s);
		}
	}

	/*
	 * Once the handset is authenticated (UAK valid) and we have answered its
	 * LOCATE-REQUEST, install the subscription in the chip, once per connection.
	 */
	if (!s->reg.persisted && s->reg.link.mm.subscribed && s->reg.link.mm.stats.located) {
		/* Keep the UAK for the later call's ciphering, and across restarts. */
		memcpy(s->uak, s->reg.link.mm.uak, DECT_MM_UAK_LEN);
		s->uak_valid = 1;
		/*
		 * Arm the identity gate for every later link of this run, the
		 * way sub_load() arms it for a stored handset. Since 7e7e5f4 a
		 * {LOCATE-REQUEST} is rejected when nothing is armed, and a
		 * freshly registered handset coming back on a new link would
		 * have been told "IPUI unknown" -- which a handset may take as
		 * losing its registration.
		 */
		if (s->reg.link.mm.port_id_len &&
		    s->reg.link.mm.port_id_len <= sizeof(s->sub_port_id)) {
			memcpy(s->sub_port_id, s->reg.link.mm.port_id,
			       s->reg.link.mm.port_id_len);
			s->sub_port_id_len = s->reg.link.mm.port_id_len;
			printf("sub: identity gate armed for the newly "
			       "registered handset (%zu bytes)\n",
			       s->sub_port_id_len);
		}
		send_subscription(s);
		sub_save(s);
		/*
		 * Close registration once the handset is enrolled. AVM's documented
		 * sequence opens registration (0x022B 'A') and closes it afterwards
		 * with 0x022C 01, which clears the a44 "registration open" capability
		 * [CR COMMAND_SET §6.2/§9.7]. Leaving it open keeps the base
		 * advertising open enrolment and the handset re-attaching.
		 */
		reg_window(s, 0, 0, now, "handset enrolled");
		s->reg.persisted = 1;
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
	if (s->reg.link.mm.stats.located != s->reg.time_sent_for) {
		struct dect_time dt;
		time_t now_s = time(NULL);
		struct tm tm;

		s->reg.time_sent_for = s->reg.link.mm.stats.located;
		if (clock_is_trustworthy(now_s, s->audio.timesync)
		    && localtime_r(&now_s, &tm)) {
			long off_s = tz_offset_seconds(now_s);

			memset(&dt, 0, sizeof(dt));
			dt.year   = (uint8_t)(tm.tm_year % 100);
			dt.month  = (uint8_t)(tm.tm_mon + 1);
			dt.day    = (uint8_t)tm.tm_mday;
			dt.hour   = (uint8_t)tm.tm_hour;
			dt.minute = (uint8_t)tm.tm_min;
			dt.second = (uint8_t)tm.tm_sec;
			dt.tz_quarters = (int16_t)(off_s / 900);
			dt.tz_valid = 1;
			if (dect_link_send_time(&s->reg.link, &dt) == 0)
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
			printf("reg: clock not disciplined (STA_UNSYNC or "
			       "pre-2020) -- sending no date and time. "
			       "TS 102 527-3 7.4.20.2 makes the duty "
			       "conditional on ours being valid, and a "
			       "confidently wrong clock is worse than none\n");
		}
	}

	if (s->reg.link.mm.state != DECT_MM_WAIT_AUTH)
		s->reg.enrol_routed = 0;
	else if (!s->reg.enrol_routed) {
		s->reg.enrol_routed = 1;
		enrol_route(s);
	}

	/* once per outcome, not on every message that follows it */
	if (s->reg.link.mm.state != s->reg.reported_state &&
	    s->reg.link.mm.state == DECT_MM_REGISTERED)
		printf("reg: handset registered\n");
	else if (s->reg.link.mm.state != s->reg.reported_state &&
		 s->reg.link.mm.state == DECT_MM_FAILED) {
		printf("reg: registration rejected\n");
		/* back to the ring target's store (review, finding 13) */
		if (s->reg.enrol_switched) {
			s->reg.enrol_switched = 0;
			hs_switch(s, 0, " (enrolment failed)");
			s->reg.link.mm.cfg.sub_slot = stubd_sub_slot(s);
		}
	}
	if (s->reg.link.mm.state == DECT_MM_REGISTERED)
		s->reg.enrol_switched = 0;
	s->reg.reported_state = s->reg.link.mm.state;

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
	    !s->reg.link.cc.incoming &&
	    !dect_cc_peer_is_speech(&s->reg.link.cc) &&
	    (s->reg.link.cc.state == DECT_CC_ST_CONNECT_PENDING ||
	     s->reg.link.cc.state == DECT_CC_ST_ACTIVE)) {
		s->reg.nonspeech_noted = 1;
		printf("call: not a speech call (<<BASIC-SERVICE>> %02x) -- "
		       "no audio path bound\n",
		       s->reg.link.cc.peer_basic_service);
	}

	if (!s->reg.call_bound &&
	    (s->reg.link.cc.state == DECT_CC_ST_CONNECT_PENDING ||
	     s->reg.link.cc.state == DECT_CC_ST_ACTIVE ||
	     (s->audio.bind_first && s->reg.connect_staged &&
	      s->reg.link.cc.state == DECT_CC_ST_PROCEEDING)) &&
	    (s->reg.link.cc.incoming ||
	     dect_cc_peer_is_speech(&s->reg.link.cc))) {
		s->reg.audio_connected = 0;
		s->reg.remode_at = 0;
		s->reg.remode_done = 0;
		s->reg.seq_t0 = 0;
		s->reg.seq_idx = 0;
		s->reg.bind_ms = now;
		s->reg.status_ms = 0;
		s->reg.have_counters = 0;
		memset(&s->reg.last_status, 0, sizeof(s->reg.last_status));
		s->reg.frames_total = 0;
		s->reg.frames_last = 0;
		/*
		 * Source 0 leaves the destination disconnected (measured with
		 * the radio off), so it carries no information during a call and
		 * only drops whatever route the bind installed. Start at 1.
		 */
		s->reg.handover_sent = 0;
		s->cmail_idx = 0;	/* re-arm the -N sweep for this call */
		s->reg.seq_t0 = mono_ms();	/* window 0 starts now */
		s->reg.seq_idx = 0;
		s->reg.sweep_idx = 1;
		s->reg.sweep_ms = now;	/* let the call settle for one step */
		s->reg.sweep_done = 0;
		/*
		 * Wideband when the call's codec is G.722 -- named by the
		 * handset in its ALERTING/CONNECT, or by our CONNECT -- and
		 * the bind is ours to choose (flags= with bit 6 set is a lab
		 * run that picked its own).
		 */
		s->reg.call_wideband = !s->audio.narrowband &&
			!(s->audio.flags & 0x40u) &&
			dect_cc_codec(&s->reg.link.cc) == DECT_CC_CODEC_G722;
		printf("call: codec %s -- %s bind\n",
		       dect_cc_codec(&s->reg.link.cc) == DECT_CC_CODEC_G722 ?
		       "G.722" : dect_cc_codec(&s->reg.link.cc) ==
		       DECT_CC_CODEC_G726 ? "G.726" : "not named (basic speech)",
		       s->reg.call_wideband ? "wideband (coded G.722 lane)" :
		       (s->audio.flags & 0x40u) ? "the configured" :
		       "narrowband (mode 2, linear PCM)");
		send_audio_bind(s, s->reg.handle);
		s->reg.call_bound = 1;
		s->reg.nonspeech_noted = 0;
		/* take the phase from this call's first received transition */
		s->play_locked = 0;
		s->rx_word_prev = 0xFFFFFFFFu;
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;		/* start the voice-slot probe */
		printf("call: connected (handle %u) -- audio channel assigned\n",
		       s->reg.handle);

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
		if (s->no_cipher && !s->reg.cipher_started) {
			s->reg.cipher_started = 1;
			printf("call: cipher: suppressed (-C), call stays in the clear\n");
		} else if (s->uak_valid && !s->reg.cipher_started) {
			uint8_t crand[8], crs[16];

			gen_rand(s, crand, sizeof(crand));
			gen_rand(s, crs, sizeof(crs));
			dect_mm_start_cipher(&s->reg.link.mm, s->uak, crand, crs);
			s->reg.cipher_started = 1;
			printf("call: cipher: authenticating handset\n");
			drain_nwk(s, now);
		} else if (!s->uak_valid) {
			printf("call: cipher: no UAK (handset must enrol this session)\n");
		}
	}

	/* Never log key material: state only. */
	if (s->reg.link.mm.cipher_state == DECT_MM_CIPHER_READY && !s->reg.cipher_done) {
		s->reg.cipher_done = 1;
		/*
		 * Key material first, then the over-air request: A-0027 Q5 makes
		 * loading the chip and negotiating the cipher distinct steps in
		 * that order. The CIPHER-REQUEST is only queued here; it still has
		 * to travel through LAPC, so the mail below wins the race.
		 */
		if (s->chip_cipher.enabled)
			send_chip_cipher(s);
		printf("call: cipher: DCK derived, CIPHER-REQUEST sent\n");
	} else if (s->reg.link.mm.cipher_state == DECT_MM_CIPHER_FAILED &&
		   !s->reg.cipher_done) {
		s->reg.cipher_done = 1;
		printf("call: cipher: authentication FAILED (RES mismatch)\n");
	}

	pump_lapc(s);
}

/* Route a received mail into the registration stack (when -g). */
static void handle_reg_mail(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	switch (m->prim) {
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
			int ring_busy = s->ring_state == RING_WAIT_PAGE_RESP ||
					s->ring_state == RING_SETUP_DELAY;
			int busy = s->reg.active &&
				   (s->reg.call_bound ||
				    dect_cc_busy(&s->reg.link.cc) ||
				    s->reg.link.mm.state == DECT_MM_WAIT_AUTH ||
				    ring_busy);

			if (busy && h == s->reg.handle) {
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
					   dect_cc_busy(&sl->link.cc))) {
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
				       h, s->reg.handle);
				break;
			}
		}
		reg_start(s, h);
		/*
		 * -a bindearly: assign the audio channel now, on the bare link,
		 * rather than waiting for a call to connect. The evidence says
		 * the chip wires its audio route when a connection is
		 * established, using the channel assignment that exists at that
		 * moment: the runs where audio flowed are exactly the ones where
		 * the chip re-established the connection *after* our bind. On a
		 * link that is already assigned, the next establishment should
		 * wire it without needing that second chance.
		 *
		 * Only the assignment is sent. 0x022D stays behind the call, as
		 * its 0x0242 handler is gated on a bound call.
		 */
		if (s->ring_state == RING_PAGING) {
			s->ring_state = RING_WAIT_PAGE_RESP;
			s->ring_resp_deadline = now + RING_PAGE_RESP_MS;
			printf("ring: link up -- waiting for the handset's "
			       "page response before the CC-SETUP\n");
		}
		if (s->audio.bind_early) {
			printf("call: bindearly: assigning the audio channel "
			       "on handle %u before any call\n", h);
			s->reg.bind_ms = now;
			send_audio_bind(s, h);
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

		if (sl && !(s->reg.active && h == s->reg.handle)) {
			xlane_release(s, sl);
			sl->active = 0;
			printf("side: connection released (handle %u)\n", h);
			break;
		}
		if (s->reg.active && m->param_len && h != s->reg.handle) {
			printf("reg: release of handle %u ignored -- our "
			       "connection is handle %u\n", (unsigned)h,
			       (unsigned)s->reg.handle);
			break;
		}
		if (s->reg.active)
			printf("reg: connection released\n");
		s->reg.active = 0;
		hs_switch(s, 0, " (link released)");
		break;
	}
	case DECT_PRIM_SIGNALLING: {
		struct side_link *sl = m->param_len ? side_find(s, m->param[0])
						    : NULL;

		s->led_traffic++;
		if (sl && !(s->reg.active && m->param[0] == s->reg.handle)) {
			side_segment(s, sl, m, now);
			break;
		}
		if (s->reg.active)
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
		    m->param_len >= 4 && m->param[0] == s->reg.handle) {
			uint8_t chan = m->param[1] & 0x0Fu;

			/*
			 * Valid STUB channels are 0..9 (ADPCM 0..3, PCM 4..6,
			 * wideband 7..9); 0xF means "not connected"
			 * [CR the hardware-analysis notes §6 1.6]. We used to stop at 6,
			 * which silently skipped the connect for a wideband
			 * channel.
			 */
			if (chan >= 1u && chan <= 9u)
				send_audio_connect(s, s->reg.handle, chan);
		} else if (m->param_len >= 2 && xlane_of(s, m->param[0])) {
			xlane_connect(s, xlane_of(s, m->param[0]));
		}
		break;
	case 0xFFF1u:
		uak_verify_reply(s, m);
		break;
	case 0xF625:
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
	unsigned int f, n, nslots = s->slots < 8u ? s->slots : 8u;

	if (!s->reg.call_bound)
		return;
	for (f = 0; f < fpb; f++) {
		for (n = 0; n < nslots; n++) {
			uint16_t v;

			memcpy(&v, fr + (f * s->slots + n) * 2u, 2);
			if (s->vs_frames && v != s->vs_last[n])
				s->vs_changes[n]++;
			if (n == s->audio.tone_word) {
				int live = v != 0x0000u && v != 0xFEFEu &&
					   v != 0xFFFFu;

				if (v == s->vs_last[n]) {
					s->vh_run++;
				} else {
					if (live && s->vh_run > 2u) {
						uint32_t ph = (s->vh_frame -
							       s->vh_run) % 80u;

						s->vh_holds++;
						s->vh_extra += s->vh_run - 2u;
						if (ph == s->vh_last_phase)
							s->vh_locked++;
						s->vh_last_phase = ph;
					}
					s->vh_run = 1;
				}
				s->vh_frame++;
			}
			s->vs_last[n] = v;
		}
		s->vs_frames++;
	}
	if (s->vs_frames >= 8000u) {		/* 8 kHz frame rate: about one second */
		printf("call: slot activity (changes/s):");
		for (n = 0; n < nslots; n++)
			printf(" %u:%u", n, s->vs_changes[n]);
		printf("\n");
		printf("call: uplink holds: %u longer than 2 frames, %u extra "
		       "frames, %u frame-locked (same phase mod 80 as the "
		       "previous hold)\n",
		       s->vh_holds, s->vh_extra, s->vh_locked);
		s->vh_holds = 0;
		s->vh_extra = 0;
		s->vh_locked = 0;
		report_tx_starvation(s);
		if (s->audio.play && !s->audio.play_raw && s->reg.call_bound) {
			/*
			 * Expect frames ~8000, advances ~4000 and, with
			 * playsync, rx_changes == advances. A rate far from
			 * 4000 means we are over- or under-supplying the lane.
			 */
			printf("call: play cadence: %u frames, %u advances, "
			       "%u rx transitions, %u held -> %u octets/s%s\n",
			       s->pl_frames, s->pl_adv, s->pl_rxchg, s->pl_held,
			       s->pl_adv * 2u,
			       (s->audio.play_sync || s->reg.call_wideband)
			       ? (s->play_locked ? " (phase locked)"
						 : " (phase NOT yet locked)")
			       : "");
			s->pl_frames = 0;
			s->pl_adv = 0;
			s->pl_rxchg = 0;
			s->pl_held = 0;
		}
		memset(s->vs_changes, 0, sizeof(s->vs_changes));
		s->vs_frames = 0;
	}
}

/*
 * Dump the call's voice slot while it is bound: the raw two bytes per frame,
 * in frame order, with nothing interpreted. Decoding happens offline -- see
 * tools/voice-decode.py, which tries the candidate formats against the file.
 */
static void voice_capture(struct stubd *s, const uint8_t *fr, unsigned int fpb)
{
	unsigned int f;

	/*
	 * Normally only a bound call is captured, which keeps the files small.
	 * `-a capall` captures from start-up instead, so the TDM lanes can be
	 * watched with the radio off -- which is how the chip's DSP routing is
	 * probed without transmitting anything.
	 */
	if (!s->voice_fp || (!s->reg.call_bound && !s->audio.cap_all))
		return;

	/*
	 * Default (-V absent) dumps every slot of every frame, so one capture
	 * can be searched offline for the voice: A-0029 §4 warns that a 16-bit
	 * TDM word may hold two independent 8-bit channels, that the lane
	 * matters, and that a slot picked by index in the DMA buffer is not
	 * necessarily the logical channel. Guessing a slot per run would cost a
	 * dial each; capturing all of them costs disk.
	 */
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
		size_t n = (size_t)fpb * s->slots * 2u;

		if (fwrite(fr, 1, n, s->voice_fp) != n)
			goto fail;
		s->voice_bytes += (uint32_t)n;
		return;
	}

	if ((unsigned int)s->voice_slot >= s->slots)
		return;
	for (f = 0; f < fpb; f++) {
		if (fwrite(fr + (f * s->slots + (unsigned int)s->voice_slot) * 2u,
			   2, 1, s->voice_fp) != 1)
			goto fail;
		s->voice_bytes += 2u;
	}
	return;
fail:
	perror("voice capture");
	fclose(s->voice_fp);
	s->voice_fp = NULL;
}

/* Main loop: decode mails, drive the session, (re)send the contact. */
static int run_loop(struct stubd *s, unsigned int fpb, unsigned int run_seconds)
{
	size_t block = s->slots * 2u * fpb;
	size_t slot = (sizeof(struct dect_pcm_block) + block + 7u) & ~(size_t)7u;
	uint8_t *buf = malloc(slot * 16u);
	uint8_t *txbuf = malloc(block);
	uint64_t start = mono_ms();

	if (!buf || !txbuf) {
		free(buf);
		free(txbuf);
		return -1;
	}
	uint64_t park_at = 0, drained_at = 0;

	s->tx_ring = 1;		/* from here on, mails are paced by tx_block() */
	g_start = start;
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
			if (!s->do_register || s->tx_slot < 0)
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
			if (drained_at && now - drained_at >= PARK_FLUSH_MS) {
				s->parked = 1;
				printf("reg: radio parked -- park mails on the "
				       "wire %llu ms after queueing\n",
				       (unsigned long long)(drained_at -
							    park_at));
				break;
			}
			if (now - park_at >= PARK_DRAIN_MS) {
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
				const uint8_t *fr = buf + off + sizeof(*h);
				struct dect_mail m;
				unsigned int f;

				if (h->magic != DECT_PCM_BLOCK_MAGIC)
					continue;
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
					if (s->do_register)
						handle_reg_mail(s, &m, now);
				}
			}
		}

		/* While parking, nothing may queue behind the park mails. */
		if (park_at) {
			led_write(s, 0);
			continue;
		}

		now = mono_ms();
		led_tick(s, now, s->do_register && s->sess.state == DECT_SESS_UP);
		state_write(s, now, s->do_register &&
			    s->sess.state == DECT_SESS_UP, 0);
		holdback_tick(s, now);
		fetch_tick(s, now);
		dial_tick(s, now);
		connect_tick(s, now);
		reg_window_tick(s, now);
		{
			unsigned k;

			for (k = 0; k < SIDE_MAX; k++)
				if (s->side[k].active)
					side_call_tick(s, &s->side[k], now);
			ic_tick(s, now);
		}
		if (s->do_register) {
			audio_connect_fallback(s, now);
			send_conn_status_req(s, now);
			play_sweep(s, now);
			send_handover(s, now);
			ring_tick(s, now);
			dsp_sweep(s, now);
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
		/* Call-bound mails (-N), one per tick, re-armed per call. */
		if (s->reg.call_bound && s->cmail_idx < s->n_cmail) {
			if (send_mail_spec(s, s->cmail[s->cmail_idx]))
				fprintf(stderr, "call mail %u (%s) failed\n",
					s->cmail_idx, s->cmail[s->cmail_idx]);
			else
				printf("call: sent call-bound mail %u: %s\n",
				       s->cmail_idx, s->cmail[s->cmail_idx]);
			s->cmail_idx++;
		}
		/*
		 * Once up, send the extra mails in order, one per tick
		 * (~100 ms) -- but not before xmail_ms, if one is given.
		 *
		 * Extra mails fire as soon as the session is up, which is far
		 * too early for anything aimed at a HANDSET: in vF424 they went
		 * out at t = 28..36 ms, before the handset had locked to the
		 * base and before the stored subscription was reloaded into the
		 * chip. xmailms holds them until the given point instead.
		 */
		if (s->sess.state == DECT_SESS_UP && s->xmail_idx < s->n_xmail &&
		    now - g_start >= s->audio.xmail_ms) {
			if (send_mail_spec(s, s->xmail[s->xmail_idx]))
				fprintf(stderr, "extra mail %u (%s) failed\n",
					s->xmail_idx, s->xmail[s->xmail_idx]);
			s->xmail_idx++;
			if (s->xmail_idx == s->n_xmail)
				printf("all %u extra mails sent\n", s->n_xmail);
		}
		/*
		 * Reload a stored subscription into the chip once the radio is up,
		 * the way AVM restores saved handsets at boot [CR A-0026], so the
		 * handset stays known across daemon restarts.
		 */
		/*
		 * Normally this waits for the extra mails, so a -M sequence can
		 * run before the chip knows any handset. With xmailms the order
		 * is deliberately the other way round: the mails are being held
		 * BACK so the handset is already known and locked when they go
		 * out, so the reload must not wait for them.
		 */
		if (s->sess.state == DECT_SESS_UP && any_uak(s) &&
		    !s->sub_restored &&
		    (s->xmail_idx >= s->n_xmail || s->audio.xmail_ms)) {
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
				if (!s->side[k].active)
					continue;
				dect_link_tick(&s->side[k].link, now);
				side_pump(s, &s->side[k]);
			}
		}
		/* Drive LAPC retransmission timers for the registration link. */
		if (s->do_register && s->reg.active) {
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
			dect_link_tick(&s->reg.link, now);
			/*
			 * The call has gone away: release the audio channel.
			 * Without this it stays armed for the life of the link
			 * and the next call starts on top of the last one's
			 * state.
			 */
			if (s->reg.call_bound && !dect_cc_busy(&s->reg.link.cc)) {
				s->reg.call_bound = 0;
				send_audio_disconnect(s, s->reg.handle);
				s->reg.call_wideband = 0;
			}
			if (!dect_cc_busy(&s->reg.link.cc))
				s->reg.nonspeech_noted = 0;
			pump_lapc(s);
		}
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
	       s->reg.link.cc.stats.setup, s->reg.link.cc.stats.alerting,
	       s->reg.link.cc.stats.connected, s->reg.link.cc.stats.released,
	       s->reg.link.cc.stats.collisions, s->reg.link.cc.stats.represented,
	       s->reg.link.cc.stats.setup_timeouts,
	       s->reg.link.cc.stats.closed_unknown);
	printf("rx: mails %u seq_gaps %u csum_errors %u framing_errors %u resyncs %u\n",
	       s->rx.stats.mails, s->rx.stats.seq_gaps, s->rx.stats.csum_errors,
	       s->rx.stats.framing_errors, s->rx.stats.resyncs);
	/*
	 * Transport bookkeeping from the driver. `dropped` counts receive
	 * blocks lost because read() was too slow, which would show up in a
	 * capture as words held over -- the audio stalls we are chasing. If it
	 * is zero while the capture still stalls, the gaps are the chip's.
	 */
	{
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
	return 0;
}

/*
 * Parse -a key=value[,key=value...]: the audio bind fields (see struct
 * audio_cfg). Bare keys "f626" and "nogain" take no value. Returns 0 on
 * success, -1 on a malformed spec.
 */
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
		if (eq) {
			v = strtoul(eq + 1, &end, 0);
			/* These carry a ':', a path or a word, not a number. */
			if (end != spec &&
			    !(klen == 3 && !memcmp(key, "dsp", 3)) &&
			    !(klen == 8 && !memcmp(key, "dspsweep", 8)) &&
			    !(klen == 7 && !memcmp(key, "seqstep", 7)) &&
			    !(klen == 4 && !memcmp(key, "play", 4)) &&
			    !(klen == 7 && !memcmp(key, "playpcm", 7)) &&
			    !(klen == 7 && !memcmp(key, "playraw", 7)) &&
			    !(klen == 3 && !memcmp(key, "led", 3)) &&
			    !(klen == 9 && !memcmp(key, "statefile", 9)) &&
			    !(klen == 6 && !memcmp(key, "remote", 6)) &&
			    !(klen == 7 && !memcmp(key, "playlog", 7)) &&
			    !(klen == 4 && !memcmp(key, "clip", 4)) &&
			    !(klen == 7 && !memcmp(key, "memdump", 7)) &&
			    !(klen == 8 && !memcmp(key, "timesync", 8)) &&
			    !(klen == 8 && !memcmp(key, "pagemail", 8)) &&
			    !(klen == 9 && !memcmp(key, "codeclist", 9)) &&
			    !(klen == 5 && !memcmp(key, "lane2", 5)) &&
			    !(klen == 5 && !memcmp(key, "lane3", 5)) &&
			    !(klen == 4 && !memcmp(key, "cnip", 4)))
				return -1;
		}
		if (eq && klen == 8 && !memcmp(key, "slotmask", 8))
			a->slot_mask = (uint8_t)v;
		else if (!eq && klen == 9 && !memcmp(key, "noslotcfg", 9))
			a->no_slot_cfg = 1;
		else if (eq && klen == 6 && !memcmp(key, "attcfg", 6))
			a->att_cfg = (uint8_t)v;
		else if (eq && klen == 7 && !memcmp(key, "attflag", 7))
			a->att_flag = (uint8_t)v;
		else if (!eq && klen == 4 && !memcmp(key, "f626", 4))
			a->send_f626 = 1;
		else if (!eq && klen == 7 && !memcmp(key, "nbcodec", 7))
			a->nbcodec = 1;
		else if (!eq && klen == 6 && !memcmp(key, "nogain", 6))
			a->no_gain = 1;
		else if (!eq && klen == 11 && !memcmp(key, "nocodeclist", 11))
			a->nocodeclist = 1;
		else if (!eq && klen == 9 && !memcmp(key, "earlybind", 9))
			a->earlybind = 1;
		else if (!eq && klen == 9 && !memcmp(key, "noregopen", 9))
			a->no_reg_open = 1;
		else if (klen == 7 && !memcmp(key, "regopen", 7))
			a->no_reg_open = 0, a->reg_window_s = eq ? (unsigned)v : 0u;
		else if (klen == 9 && !memcmp(key, "regwindow", 9))
			a->reg_signal_s = (unsigned)v;
		else if (!eq && klen == 8 && !memcmp(key, "twocalls", 8))
			a->two_calls = 1;
		else if (eq && klen == 5 && (!memcmp(key, "lane2", 5) ||
					     !memcmp(key, "lane3", 5))) {
			unsigned ch, w, k = key[4] == '2' ? 0u : 1u;

			/* channel 1..3 (A-0124: three voice channels) */
			if (sscanf(eq + 1, "%u:%u", &ch, &w) != 2 || !ch ||
			    ch > 3 || w > 31)
				return -1;
			a->lane_ch[k] = (uint8_t)ch;
			a->lane_word[k] = (uint8_t)w;
		}
		else if (!eq && klen == 10 && !memcmp(key, "nostockcfg", 10))
			a->no_stock_cfg = 1;
		else if (eq && klen == 7 && !memcmp(key, "subslot", 7)) {
			if (v < 1u || v > DECT_MM_MAX_SLOT)
				return -1;
			a->sub_slot = (unsigned)v;
		}
		else if (eq && klen == 10 && !memcmp(key, "setupdelay", 10)) {
			if (v > 5000u)
				return -1;
			a->setup_delay_ms = (unsigned)v;
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
		else if (!eq && klen == 9 && !memcmp(key, "noconnect", 9))
			a->no_connect = 1;
		else if (!eq && klen == 6 && !memcmp(key, "nowait", 6))
			a->nowait = 1;
		else if (!eq && klen == 6 && !memcmp(key, "capall", 6))
			a->cap_all = 1;
		else if (!eq && klen == 8 && !memcmp(key, "loopback", 8))
			a->loopback = 1;
		else if (klen == 9 && !memcmp(key, "loopdelay", 9)) {
			if (v < 2u || v > LOOP_DELAY_MAX)
				return -1;
			a->loop_delay = (unsigned)v;
			a->loopback = 1;
		}
		else if (!eq && klen == 9 && !memcmp(key, "bindearly", 9))
			a->bind_early = 1;
		else if (klen == 8 && !memcmp(key, "handover", 8))
			a->handover_ms = (unsigned)v;
		else if (klen == 4 && !memcmp(key, "ring", 4))
			a->ring_ms = (unsigned)v;
		else if (klen == 10 && !memcmp(key, "ringattach", 10))
			a->ring_attach_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "clipton", 7))
			a->clip_ton = (uint8_t)v;
		else if (klen == 7 && !memcmp(key, "clipnpi", 7))
			a->clip_npi = (uint8_t)v;
		else if (klen == 7 && !memcmp(key, "clipscr", 7))
			a->clip_scr = (uint8_t)v;
		else if (klen == 7 && !memcmp(key, "playlog", 7)) {
			size_t n = (size_t)(spec - (eq + 1));
			char *path = malloc(n + 1);

			if (!path)
				return -1;
			memcpy(path, eq + 1, n);
			path[n] = '\0';
			a->play_log = path;
		}
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
		else if (!eq && klen == 12 && !memcmp(key, "nocallstatus", 12))
			a->no_call_status = 1;
		else if (klen == 8 && !memcmp(key, "pagemail", 8)) {
			/* text, not a number: a mail spec, taken verbatim */
			size_t n = (size_t)(spec - (eq + 1));
			char *text;

			if (a->n_page_mail >= sizeof(a->page_mail) /
					      sizeof(a->page_mail[0]))
				return -1;
			text = malloc(n + 1);
			if (!text)
				return -1;
			memcpy(text, eq + 1, n);
			text[n] = '\0';
			a->page_mail[a->n_page_mail++] = text;
		}
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
		else if (klen == 7 && !memcmp(key, "pagehdr", 7)) {
			/*
			 * Only the low nibble reaches the page, and anything
			 * above 0x0F is rejected rather than masked so a typo
			 * is not silently accepted as something else.
			 *
			 * [CR A-0093]: the builder also has a second path that
			 * forces the header to 0 when mail[5] & 0xC0 != 0.
			 * Capping at 0x0F means we can never trip it by
			 * accident.
			 */
			if (v > 0x0Fu)
				return -1;
			a->page_hdr = (int)v;
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
		else if (klen == 2 && !memcmp(key, "ti", 2))
			a->ti_fixed = (int)v;
		else if (klen == 6 && !memcmp(key, "signal", 6)) {
			a->signal_value = (uint8_t)v;
			a->signal_on = 1;
		}
		else if (!eq && klen == 8 && !memcmp(key, "nosignal", 8))
			a->signal_on = 0;	/* control runs only, see run 18 */
		else if (!eq && klen == 8 && !memcmp(key, "playsync", 8))
			a->play_sync = 1;
		else if (!eq && klen == 7 && !memcmp(key, "playinv", 7))
			a->play_inv = 1;
		else if (!eq && klen == 8 && !memcmp(key, "playswap", 8))
			a->play_swap = 1;
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
		else if (klen == 9 && !memcmp(key, "playsweep", 9))
			a->play_sweep_ms = (unsigned)v;
		else if (klen == 8 && !memcmp(key, "playbyte", 8)) {
			if (v > 2u)
				return -1;
			a->play_byte = (unsigned)v;
		}
		else if (klen == 8 && !memcmp(key, "playslow", 8)) {
			if (v < 2u)
				return -1;
			a->play_slow = (unsigned)v;
		}
		else if (klen == 9 && !memcmp(key, "playphase", 9))
			a->play_phase_ms = (unsigned)v;
		else if (!eq && klen == 9 && !memcmp(key, "clipfirst", 9))
			a->clip_first = 1;
		else if (klen == 7 && !memcmp(key, "cnipscr", 7))
			a->cnip_scr = (uint8_t)v;
		else if (klen == 9 && !memcmp(key, "cnipalpha", 9))
			a->cnip_alpha = (uint8_t)v;
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
			size_t n = (size_t)(spec - (eq + 1));
			char *text = malloc(n + 1);

			if (!text)
				return -1;
			memcpy(text, eq + 1, n);
			text[n] = '\0';
			if (key[1] == 'l')
				a->clip_number = text;
			else
				a->clip_name = text;
		}
		else if ((klen == 4 && !memcmp(key, "play", 4))
			 || (klen == 7 && !memcmp(key, "playpcm", 7))
			 || (klen == 7 && !memcmp(key, "playraw", 7))) {
			/* value is a path, not a number: take it verbatim */
			size_t n = (size_t)(spec - (eq + 1));
			char *path = malloc(n + 1);

			if (!path)
				return -1;
			memcpy(path, eq + 1, n);
			path[n] = '\0';
			a->play_path = path;
			a->play_pcm = (klen == 7 && key[4] == 'p');
			a->play_raw = (klen == 7 && key[4] == 'r');
		}
		else if (!eq && klen == 8 && !memcmp(key, "tonehigh", 8))
			a->tone_high = 1;
		else if (!eq && klen == 6 && !memcmp(key, "tone16", 6))
			a->tone16 = 1;
		else if (!eq && klen == 8 && !memcmp(key, "tone16be", 8))
			a->tone16 = a->tone16be = 1;
		else if (klen == 4 && !memcmp(key, "tone", 4)) {
			if (v % 125u || v > 3875u)
				return -1;
			a->tone_hz = (unsigned)v;
		} else if (klen == 8 && !memcmp(key, "toneword", 8))
			a->tone_word = (uint8_t)v;
		else if (klen == 8 && !memcmp(key, "playslot", 8))
			a->play_slot_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "xmailms", 7))
			a->xmail_ms = (unsigned)v;
		else if (klen == 6 && !memcmp(key, "remode", 6)) {
			a->remode_ms = (unsigned)v;
			a->remode_set = 1;
		} else if (eq && klen == 9 && !memcmp(key, "statefile", 9)) {
			size_t n = (size_t)(spec - (eq + 1));
			char *path = malloc(n + 1);

			if (!path)
				return -1;
			memcpy(path, eq + 1, n);
			path[n] = '\0';
			a->state_path = path;
		} else if (eq && klen == 6 && !memcmp(key, "remote", 6)) {
			size_t n = (size_t)(spec - (eq + 1));
			char *path = malloc(n + 1);

			if (!path)
				return -1;
			memcpy(path, eq + 1, n);
			path[n] = '\0';
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
			size_t n = (size_t)(spec - (eq + 1));
			char *path = malloc(n + 1);

			if (!path)
				return -1;
			memcpy(path, eq + 1, n);
			path[n] = '\0';
			a->led_path = path;
		} else if (!eq && klen == 5 && !memcmp(key, "noled", 5)) {
			a->no_led = 1;
		} else if (!eq && klen == 8 && !memcmp(key, "noremode", 8)) {
			a->remode_ms = 0;
			a->remode_set = 1;
		}
		else if (!eq && klen == 10 && !memcmp(key, "remodebit6", 10))
			a->remode_bit6 = 1;
		else if (klen == 11 && !memcmp(key, "remodecodec", 11))
			a->remode_codec = (int)v;
		else if (klen == 3 && !memcmp(key, "seq", 3))
			a->seq_ms = (unsigned)v;
		else if (klen == 7 && !memcmp(key, "seqstep", 7)) {
			if (a->n_seq >= sizeof(a->seq_step) /
					sizeof(a->seq_step[0]))
				return -1;
			a->seq_step[a->n_seq++] = eq + 1;
		}
		else if (klen == 3 && !memcmp(key, "dsp", 3)) {
			/* dsp=DST:SRC -- the generic check above let the ':' through */
			unsigned long src;
			const char *colon = memchr(eq + 1, ':', (size_t)(spec - eq - 1));

			if (!colon || a->dsp_n >= 8u)
				return -1;
			v = strtoul(eq + 1, &end, 0);
			if (end != colon)
				return -1;
			src = strtoul(colon + 1, &end, 0);
			if (end != spec || v > 17u || src > 22u)
				return -1;
			a->dsp_dst[a->dsp_n] = (uint8_t)v;
			a->dsp_src[a->dsp_n] = (uint8_t)src;
			a->dsp_n++;
		} else if (!eq)
			return -1;
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
		else if (klen == 8 && !memcmp(key, "dspsweep", 8)) {
			const char *colon = strchr(eq + 1, ':');

			if (v > 17u)
				return -1;
			a->sweep_dst = (int)v;
			if (colon)
				a->sweep_ms = (unsigned)strtoul(colon + 1,
								NULL, 0);
			if (!a->sweep_ms)
				a->sweep_ms = 1000u;
		}
		else
			return -1;
		if (*spec == ',')
			spec++;
	}
	return 0;
}

/* Parse -k SEL[,rs][,33]. Returns 0 on success, -1 on a malformed spec. */
static int parse_chip_cipher(struct chip_cipher *cc, const char *spec)
{
	char *end;
	unsigned long sel;

	sel = strtoul(spec, &end, 0);
	if (end == spec || sel > 0xFFu)
		return -1;
	cc->enabled = 1;
	cc->selector = (uint8_t)sel;
	while (*end == ',') {
		const char *opt = ++end;

		while (*end && *end != ',')
			end++;
		size_t len = (size_t)(end - opt);

		if (len == 2 && memcmp(opt, "rs", 2) == 0)
			cc->use_rs = 1;
		else if (len == 2 && memcmp(opt, "33", 2) == 0)
			cc->long_form = 1;
		else if (len == 5 && memcmp(opt, "norev", 5) == 0)
			cc->no_reverse = 1;
		else
			return -1;
	}
	return *end ? -1 : 0;
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
		"  -N SRC:DST:PRIM[:HEX]  send this mail once a CALL is bound\n"
		"           (repeatable, one per ~100 ms tick, re-armed on every call).\n"
		"           For DSP probes that name a channel the chip only allocates\n"
		"           when a call exists, e.g. the Testton cross-connect.\n"
		"           one per ~100 ms; e.g. the MAC bring-up sequence)\n"
		"  -g       GAP registration mode: bring up the radio + open\n"
		"           registration, then run the host C-plane stack so a\n"
		"           handset can register (parks the radio on exit)\n"
		"  -P PIN   registration PIN / AC, 4 to 8 digits (default 0000;\n"
		"           used with -g)\n"
		"  -s PATH  subscription store, holds the handset key; reloaded into\n"
		"           the chip at start-up (default /etc/opendectcell-sub, \"\" = off).\n"
		"           Repeat for every handset the base holds: all are served\n"
		"           and installed in their chip slots; the first is rung\n"
		"  -k SPEC  EXPERIMENTAL: hand the call's cipher material to the chip\n"
		"           (0x0208) once the DCK is derived. SPEC is\n"
		"           SEL[,rs][,norev][,33]: SEL = selector (7..12 is the\n"
		"           subscription-oriented branch), rs = put RS rather than\n"
		"           RAND_F in the input field, norev = do not byte-reverse the\n"
		"           DCK/RES, 33 = long mail variant. Layout per [CR A-0028];\n"
		"           the DCK identification is a candidate only. Off by default\n"
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
		"           codec is extra. nogain = skip 0x0251, f626 = send 0xF626,\n"
		"           nowait = send 0x022D at once instead of after 0x0242\n"
		"           timesync=auto|force|off  send <<TIME-DATE>> after a\n"
		"                    location registration. auto (default) only when\n"
		"                    the kernel says the clock is disciplined;\n"
		"                    force trusts it anyway; off never sends\n"
		"           noconnect = never send 0x022D (the stock connect is\n"
		"           0x0251 then 0x0230 and nothing else)\n"
		"           tone=HZ (multiple of 125) transmits an A-law test tone to\n"
		"           the handset while a call is up, into raw word toneword=N\n"
		"           (default 0 = dect1), low byte, or the high byte with tonehigh;\n"
		"           tone16 / tone16be write 16-bit linear into the whole word;\n"
		"           dsp=DST:SRC (repeatable) sends GenDSP connect 0xFFF1/0A after\n"
		"           the audio path is up: ADPCM 0..3, PCM 4..6, wideband 7..9,\n"
		"           silence 12, test tone 15 (PCM 6 = raw word 0, PCM 4 = raw 5)\n"
		"           txidle=N voiceidle=N set the transmit filler bytes\n"
		"           slotmask=N 0x017c byte -> 0xdb3 bearer-slot mask (stock\n"
		"                     0x09; default 0x09; THE incoming-call fix,\n"
		"                     [CR A-0114]); noslotcfg omits it (control)\n"
		"           attcfg=N  0x020A config byte after the UAK (stock 0x01\n"
		"                     per Gigaset, 0x02 C4; default 0x00) [CR A-0113]\n"
		"           attflag=N 0x0247 install flag byte (stock bit0=1,\n"
		"                     A540 0x03; default 0x00) [CR A-0113]\n"
		"           ringattach=MS same, but MS ms after the handset's FIRST\n"
		"                     C-plane contact (unattended runs; a lost A540\n"
		"                     re-locks anywhere from 2 s to minutes later)\n"
		"           ring=MS calls the handset MS ms after start-up: page\n"
		"           (0x0300) then an FP-originated CC-SETUP\n"
		"           play=FILE transmits a raw G.722 stream (8000 bytes/s) to\n"
		"           the handset while a call is up, looping -- a clean\n"
		"           downlink source, unlike the loopback\n"
		"           playpcm=FILE does the same from 16 kHz signed 16-bit\n"
		"           little-endian PCM, encoding it to G.722 at start-up\n"
		"           signal=N <<SIGNAL>> N in the SETUP (0x40..0x47 =\n"
		"           alerting pattern 0..7; default 0x41, external call)\n"
		"           nosignal     omit it: the A540 then rings silently\n"
		"           pagemail=SPEC sends a raw mail (same syntax as -M)\n"
		"           IMMEDIATELY after each page; repeatable, up to 4, in\n"
		"           order. For 0x015E, which stock sends twice around\n"
		"           every page and we never send at all.\n"
		"           setuptries=N re-offers a refused CC-SETUP N times\n"
		"           codeclist=HEX <<CODEC-LIST>> value octets for CC-SETUP\n"
		"           (default none: basic speech means G.726)\n"
		"           earlybind    assign audio on the page response\n"
		"           subslot=N    chip subscription slot 1..6 (TPUI + UAK slot)\n"
		"           nostockcfg   omit stock's 0153/0169/0157 bring-up config\n"
		"           twocalls     a second (third) handset may call while one\n"
		"           is in a call: side links get their own audio channel\n"
		"           (2 -> TDM word 3, 3 -> word 2; lane2=CH:WORD to override)\n"
		"           with the test file or loop-back, as the main call\n"
		"           regopen      open registration while running (enrol a\n"
		"           handset; default closed, subscribed handsets attach)\n"
		"           noregopen    closed, the default\n"
		"           setupdelay=N hold the CC-SETUP N ms after the page\n"
		"           (default 8; the default 16 s budget is SHORTER than\n"
		"           our own F<CC.03> timer, so it gives up first)\n"
		"           pagehdr=N appends a FOURTH byte to the 0x0300 page\n"
		"           (mail[5], 0..15). [CR A-0091]: the chip builds the\n"
		"           page header byte at 0x96ad+4 from it, so the low 3\n"
		"           bits select the on-air LCE header -- 4 is the GAP\n"
		"           voice code, 0 is C-plane only. Neither we nor stock\n"
		"           has ever set this byte, so the builder has been\n"
		"           reading whatever was in the buffer. OFF by default.\n"
		"           basicsvc=N sets <<BASIC-SERVICE>> (default 0x88: high\n"
		"           nibble call class, low nibble 8 = wideband speech;\n"
		"           0x80 with narrowband)\n"
		"           nocallstatus no <<CALL-INFORMATION>> call status in our\n"
		"           SETUP-ACK/CALL-PROC/ALERTING/CONNECT to a handset's call\n"
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
		"           playbyte=0|1|2 lane layout: 0 = a 16-bit word held\n"
		"           two frames, 1 = one octet per frame in the low byte,\n"
		"           2 = the high byte\n"
		"           playsweep=MS walks all six lane layouts every MS ms\n"
		"           during a call and prints which is active;\n"
		"           playinv / playswap / playbyte pin one\n"
		"           memdump=ADDR[:LEN] sweeps chip memory with the\n"
		"           read-address debug mail, logging each reply; repeat\n"
		"           the key for several ranges, memdumpms=N sets the\n"
		"           interval (default 50). Polling starts at link-up, so\n"
		"           one run gives an idle baseline and the live state.\n"
		"           Read only; never writes chip memory.\n"
		"           playslow=N holds every N-th word four frames, so the\n"
		"           lane carries 1/N less than 8000 octets/s (N=100: 1 %%)\n"
		"           playphase=MS alternates only the held-word parity\n"
		"           every MS ms (two variants, not six)\n"
		"           playlog=FILE records the octets actually handed to\n"
		"           the lane, so the stream can be decoded offline\n"
		"           playsync advances the coded stream on the received\n"
		"           lane's word boundary instead of a cadence of our own;\n"
		"           required for anything but a steady tone\n"
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
		"           remodecodec=N overrides the codec byte in the re-assign;\n"
		"           with bit 6 set, N=1 selects mode 8 (the only mode with\n"
		"           the 80-byte B-field a long slot carries: transparent\n"
		"           downlink on, uplink off by design) [CR A-0040]\n"
		"           remodebit6 keeps bit 6 set instead: mode 9, which uses\n"
		"           the channel's input variant B (the transparent path a\n"
		"           wideband call needs, since the chip has no G.722\n"
		"           encoder) rather than variant A (the G.726 encoder)\n"
		"           playslot=MS walks the lane across every transmit slot,\n"
		"           MS ms each, logging 'play: lane slot N'. Which slot the\n"
		"           chip reads as the downlink bearer is not established.\n"
		"           playraw=FILE replays a verbatim TDM lane instead (two\n"
		"           bytes per frame, 16000 B/s, as -w captures it): no\n"
		"           cadence logic, nothing interpreted\n"
		"           clip=NUMBER cnip=NAME put a calling-party number\n"
		"           (0x6C, printable IA5) and name (0x6D, UTF-8) in the\n"
		"           FP-originated CC-SETUP, both presented as allowed and\n"
		"           user-provided-not-screened. Omitted options send no IE.\n"
		"           clipton=N clipnpi=N clipscr=N cnipscr=N cnipalpha=N\n"
		"           sweep the descriptor fields; the stock producer's\n"
		"           ordinary branch is clipscr=1, giving `01 81`.\n"
		"           clipfirst puts both IEs before <<BASIC-SERVICE>>\n"
		"           lineid=N (0..126) linetype=N (bit 0 VoIP delay, bit 1\n"
		"           in-band second calls) callid=N callstatus=N add a\n"
		"           <<CALL-INFORMATION>> (0x7E) to the FP-originated\n"
		"           CC-SETUP; CAT-iq 2.0 marks an external call by its line\n"
		"           id. Our one SIP line is lineid=0,linetype=1.\n"
		"           handover=MS asks the chip to move the call's bearer MS ms\n"
		"           after the bind (0x016A), which is the event that has\n"
		"           accompanied every run where audio flowed\n"
		"           bindearly assigns the audio channel when the chip reports a\n"
		"           connection, not when a call connects\n"
		"           loopdelay=N echoes it through an N-frame ring (2..%u)\n"
		"           instead: same cadence, older content\n"
		"           loopback echoes the received bearer word (toneword)\n"
		"           straight back to the handset, byte for byte -- the\n"
		"           lane carries coded G.722, so this needs no encoder\n"
		"           capall makes -w capture from start-up, not only while a\n"
		"           call is bound (for radio-off TDM probing)\n"
		"           dspsweep=DST[:MS] walks GenDSP sources 0..22 into DST\n"
		"           during a call, one per MS ms (default 1000), then\n"
		"           restores the default routes of channels 1..6\n"
		"           status=MS polls the chip's per-link connection status\n"
		"           (0xF624, read-only) every MS ms while a call is bound and\n"
		"           prints the MAC slot, carrier and bearer slot type\n"
		"           Default: sel=1,gain=0x2b,flags=1,mask=0,mode=0,codec=0\n"
		"           (flags=0x41 is the wideband bind used at G7),\n"
		"           txidle=0xff,voiceidle=0xff\n", LOOP_DELAY_MAX);
}

int main(int argc, char **argv)
{
	struct stubd s;
	struct dect_pcm_config cfg;
	struct dect_sess_cfg scfg = {
		.src_task = DECT_TASK_PCM,
		.dst_task = DECT_TASK_CONTACT_DST,
	};
	const char *dev = "/dev/dect0";
	unsigned int run_seconds = 0, fpb;
	struct dect_mail spec_check;	/* -M / -N validated at startup */
	int c, ret = 1, sub_off = 0;

	memset(&s, 0, sizeof(s));
	s.tx_slot = s.rx_slot = -1;
	s.rand_fd = -1;
	memset(&cfg, 0, sizeof(cfg));
	s.sub_path = "/etc/opendectcell-sub";	/* overridden by -s */
	s.slot_sweep_cur = -1;
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
	s.audio.clip_ton = DECT_CLIP_TON_UNKNOWN;
	s.audio.clip_npi = DECT_CLIP_NPI_E164;
	s.audio.clip_scr = DECT_CLIP_SCR_USER_UNSCREENED;
	s.audio.cnip_scr = DECT_CLIP_SCR_USER_UNSCREENED;
	s.audio.cnip_alpha = DECT_CLIP_ALPHABET_UTF8;
	s.sweep_byte = 0;
	s.rx_word_prev = 0xFFFFFFFFu;	/* no word seen yet */
	s.audio.memdump_ms = 50u;	/* default chip-memory poll interval */
	s.audio.basic_svc = RING_BASIC_SVC;
	s.audio.dial_wait_ms = 3000u;
	s.audio.no_reg_open = 1;	/* closed unless -a regopen */
	s.audio.reg_signal_s = 120u;
	s.audio.lane_ch[0] = 2; s.audio.lane_word[0] = 3;	/* Q-0063 */
	s.audio.lane_ch[1] = 3; s.audio.lane_word[1] = 2;
	s.audio.slot_mask = 0x09u;	/* [CR A-0114] stock's 0x017c 00 09 */
	/*
	 * -1, not 0: the struct is memset to zero above, and a zero here
	 * would append a fourth page byte of 0x00 on every run -- changing
	 * what we transmit by default on the strength of one trace. Off
	 * unless asked.
	 */
	s.audio.page_hdr = -1;
	s.audio.ti_fixed = -1;
	s.audio.remode_codec = -1;	/* keep the bind's codec byte unless asked */
	/*
	 * The mode-2 re-assign. Since 2026-10-25 it is sent only when the
	 * first 0x0230 had flag bit 6 set (the 0x41 wideband bind every lab
	 * script used); see the resolution after option parsing. With bit 6
	 * clear in the first assign -- our default flags=1 -- the chip reaches
	 * mode 2 by itself once 0x022D turns the path on, as A-0046/A-0047
	 * said, and the re-assign only adds a 500 ms mode-9 window: a burst of
	 * noise in both directions at the start of every call (runs 25-27,
	 * A540 and AS690H).
	 *
	 * When it is sent: the 2026-10-23 history.
	 * Without it the 0x41 bind stays in mode 9 and no downlink audio of
	 * any format reaches the handset. With it, and linear PCM on the lane,
	 * a whole call is audible (vF411).
	 *
	 * 500 ms, and not less. This was briefly defaulted to 150 ms on the
	 * reasoning that it is comfortably after the 0x0242 which triggers
	 * 0x022D; vF414 then failed with the enable word never reaching 1 and
	 * dest 6 falling back to silence 4 s in, while vF411 at 500 ms held
	 * for a whole call. The two runs differ in nothing else. The MAC-side
	 * bearer handler evidently runs its own configurator pass after
	 * 0x022D, and a re-assign that arrives before it is simply
	 * overwritten. 500 ms is the only value with evidence behind it.
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
	s.audio.sweep_dst = -1;		/* no GenDSP source sweep */
	s.tx_idle = 0xFFu;
	s.tx_voice_idle = 0xFFu;
	s.pin = "0000";		/* the lab's; the library has no default */
	while ((c = getopt(argc, argv, "d:c:T:R:t:vpM:N:gP:s:k:w:V:Ca:h")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'c': cfg.slots = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'T': s.tx_slot = (int)strtol(optarg, NULL, 0); break;
		case 'R': s.rx_slot = (int)strtol(optarg, NULL, 0); break;
		case 't': run_seconds = (unsigned int)strtoul(optarg, NULL, 0); break;
		case 'v': s.verbose = 1; break;
		case 'p': s.verbose = 1; s.show_params = 1; break;
		case 'N':
			if (s.n_cmail >= sizeof(s.cmail) / sizeof(s.cmail[0])) {
				fprintf(stderr, "too many -N mails\n");
				return 2;
			}
			if (parse_mail_spec(optarg, &spec_check)) {
				fprintf(stderr, "bad -N mail spec: %s "
					"(SRC:DST:PRIM[:HEX], hex in pairs)\n",
					optarg);
				return 2;
			}
			s.cmail[s.n_cmail++] = optarg;
			break;
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
		case 'P':
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
			if (s.n_hs >= DECT_MM_MAX_SLOT) {
				fprintf(stderr, "-s: at most %u stores\n",
					DECT_MM_MAX_SLOT);
				return 2;
			}
			s.hs[s.n_hs++].path = optarg;
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
		 *
		 * -a nostockcfg restores the old two-mail bring-up, so the
		 * difference is one flag.
		 */
		/* src/dst/prim parse with base 0, so hex needs the 0x prefix. */
		if (!s.audio.no_stock_cfg) {
			s.xmail[s.n_xmail++] = "0x14:0x02:0x0153:00";	/* ECO_CONFIG */
			s.xmail[s.n_xmail++] = "0x14:0x02:0x0169:07";	/* FEATURE_FLAGS */
			s.xmail[s.n_xmail++] = "0x14:0x02:0x0157:00";	/* CAPABILITY_PROFILE */
		}
		s.xmail[s.n_xmail++] = "0x22:0x02:0x015b:01";	/* radio/beacon on */
		/*
		 * Registration open ('A') only when asked (-a regopen): nothing
		 * enrols unless we mean it to. Closed is 0x022C 01.
		 */
		if (s.audio.no_reg_open)
			s.xmail[s.n_xmail++] = "0x22:0x02:0x022c:01";	/* closed */
		else
			s.xmail[s.n_xmail++] = "0x22:0x02:0x022b:41";	/* 'A': open reg */
	}
	if (optind != argc) {
		usage();
		return 2;
	}
	if (sub_off) {			/* -s "" */
		s.sub_path = NULL;
		s.n_hs = 0;
	} else if (!s.n_hs && s.sub_path) {
		s.hs[0].path = s.sub_path;	/* the default store */
		s.n_hs = 1;
	}
	if (!s.audio.remode_set && !(s.audio.flags & 0x40u))
		s.audio.remode_ms = 0;	/* mode 2 from the first assign */
	/* the window the start-up mail opens (0x022B), if regopen asked */
	s.reg_open = !s.audio.no_reg_open;
	s.reg_open_until = s.reg_open && s.audio.reg_window_s ?
			   mono_ms() + s.audio.reg_window_s * 1000ull : 0;
	if (!s.audio.basic_svc_set)
		s.audio.basic_svc = s.audio.narrowband ? RING_BASIC_SVC
						       : RING_BASIC_SVC_WB;

	s.stub_build = stub_build_read(s.stub_name, sizeof(s.stub_name));
	printf("chip: STUB second stage %s\n",
	       s.stub_build == STUB_UNKNOWN ? "unknown (no " DECT_STUB_RECORD
					      ", run fwload.sh)"
					    : s.stub_name);
	if (s.do_register && s.n_hs) {
		unsigned i, j;

		/* restores the UAKs; chip reload once up */
		for (i = 0; i < s.n_hs; i++) {
			struct stat sst;

			s.cur = i;
			hs_load(&s, i);		/* path set, the rest empty */
			s.hs[i].loaded = sub_load(&s);
			hs_stash(&s, i);
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
				    s.hs[j].slot != s.hs[i].slot)
					continue;
				printf("sub: %s claims slot %u, which %s "
				       "holds -- not loaded\n", s.hs[i].path,
				       s.hs[i].slot, s.hs[j].path);
				s.hs[i].loaded = 0;
			}
		}
		/* remote=: one more store, marked as a roamer (test M4) */
		if (s.audio.remote_path && s.n_hs < DECT_MM_MAX_SLOT) {
			i = s.n_hs++;
			s.hs[i].path = s.audio.remote_path;
			s.cur = i;
			hs_load(&s, i);
			s.hs[i].loaded = sub_load(&s);
			hs_stash(&s, i);
			s.hs[i].remote = 1;
			printf("sub: %s is a roamer: not held, not in the chip, "
			       "fetched on demand after %u ms\n",
			       s.audio.remote_path, s.audio.fetch_delay_ms);
		}
		/*
		 * A first store that does not exist yet (an enrolment) has no
		 * slot; it used to default to 1 and silently share it with
		 * whichever store held 1 (review, 2026-10-25). Give it the
		 * lowest free one, unless subslot= chose.
		 */
		if (!s.hs[0].loaded && !s.audio.sub_slot) {
			unsigned used = 0, slot;

			for (i = 1; i < s.n_hs; i++)
				if (s.hs[i].loaded)
					used |= 1u << s.hs[i].slot;
			for (slot = 1; slot <= DECT_MM_MAX_SLOT &&
			     (used & (1u << slot)); slot++)
				;
			if (slot > DECT_MM_MAX_SLOT) {
				fprintf(stderr, "sub: every chip slot is taken "
					"by the other stores\n");
				return 2;
			}
			s.hs[0].slot = slot;
			printf("sub: %s is new; it gets chip slot %u\n",
			       s.hs[0].path, slot);
		}
		/*
		 * Every other store that does not exist yet is a place a new
		 * handset can enrol (enrol_route()): it needs a slot of its
		 * own as well, distinct from the loaded stores and from each
		 * other.
		 */
		for (i = 1; i < s.n_hs; i++) {
			unsigned used = 0, slot, k;

			if (s.hs[i].loaded || s.hs[i].remote ||
			    !s.hs[i].absent)
				continue;
			for (k = 0; k < s.n_hs; k++) {
				unsigned ks = k == 0 && s.audio.sub_slot ?
					      s.audio.sub_slot : s.hs[k].slot;

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
			s.hs[i].slot = slot;
			printf("sub: %s is free for an enrolment, chip slot %u\n",
			       s.hs[i].path, slot);
		}
		if (!s.hs[0].loaded && s.audio.sub_slot) {
			for (i = 1; i < s.n_hs; i++)
				if (s.hs[i].loaded &&
				    s.hs[i].slot == s.audio.sub_slot)
					printf("sub: *** subslot=%u is %s's slot "
					       "-- enrolling here overwrites it ***\n",
					       s.audio.sub_slot, s.hs[i].path);
		}
		s.cur = 0;
		hs_load(&s, 0);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_usr1);
	dect_rx_init(&s.rx);
	dect_session_init(&s.sess, &scfg);

	dect_clip_init(&s.clip);
	if (s.audio.clip_number
	    && dect_clip_set_number(&s.clip, s.audio.clip_number,
				    (enum dect_clip_ton)s.audio.clip_ton,
				    (enum dect_clip_npi)s.audio.clip_npi,
				    DECT_CLIP_PRES_ALLOWED,
				    (enum dect_clip_screening)s.audio.clip_scr)) {
		fprintf(stderr, "clip: number rejected (printable IA5, "
			"at most %u bytes)\n", DECT_CLIP_NUMBER_MAX);
		return 1;
	}
	if (s.audio.clip_name
	    && dect_clip_set_name(&s.clip, s.audio.clip_name,
				  (enum dect_clip_alphabet)s.audio.cnip_alpha,
				  DECT_CLIP_PRES_ALLOWED,
				  (enum dect_clip_screening)s.audio.cnip_scr)) {
		fprintf(stderr, "cnip: name rejected (well-formed UTF-8 "
			"without control characters, at most %u bytes)\n",
			DECT_CLIP_NAME_MAX);
		return 1;
	}
	s.clip.emit_before_basic_service = (uint8_t)(s.audio.clip_first != 0);
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
	}
	if (s.audio.clip_number || s.audio.clip_name)
		printf("clip: ton=%u npi=%u clipscr=%u cnipscr=%u "
		       "cnipalpha=%u -- CC-SETUP will carry %s%s%s\n",
		       s.audio.clip_ton, s.audio.clip_npi, s.audio.clip_scr,
		       s.audio.cnip_scr, s.audio.cnip_alpha,
		       s.audio.clip_number ? "a number" : "",
		       (s.audio.clip_number && s.audio.clip_name) ? " and " : "",
		       s.audio.clip_name ? "a name" : "");
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
		 * passes, ~4 s each, before giving up.
		 */
		int pass;

		for (pass = 1; find_control_slot(&s, fpb); pass++) {
			if (pass >= 3 || stop) {
				fprintf(stderr, "control slot not found\n");
				goto out;
			}
			fprintf(stderr, "control slot not found yet (pass %d of "
				"3) -- the chip may still be starting\n", pass);
			sleep(1);
		}
	} else {
		printf("control slot given: tx %d, rx %d\n", s.tx_slot, s.rx_slot);
	}
	if (s.tx_slot >= (int)s.slots || s.rx_slot >= (int)s.slots) {
		fprintf(stderr, "slot out of range\n");
		goto out;
	}
	if (s.audio.play_log) {
		s.play_log_fp = fopen(s.audio.play_log, "wb");
		if (!s.play_log_fp) {
			fprintf(stderr, "cannot write %s\n", s.audio.play_log);
			return 1;
		}
		printf("playlog: recording transmitted octets to %s\n",
		       s.audio.play_log);
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
		s.voice_fp = fopen(s.voice_path, "wb");
		if (!s.voice_fp) {
			perror(s.voice_path);
			goto out;
		}
	}
	led_open(&s);
	led_write(&s, 0);	/* a park or probe run leaves it dark */
	ret = run_loop(&s, fpb, run_seconds);
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
		printf("voice: captured %u bytes from slot %d (%u frames)\n",
		       s.voice_bytes, s.voice_slot, s.voice_bytes / 2u);
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
	if (s.play_log_fp) {
		printf("playlog: %ld octets written\n", ftell(s.play_log_fp));
		fclose(s.play_log_fp);
	}
	if (s.rand_fd >= 0)
		close(s.rand_fd);
	tear_down(&s);
	close(s.fd);
	return ret;
}

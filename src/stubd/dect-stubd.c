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
#include "dect_cc.h"
#include "dect_audio.h"
#include "dect_callinfo.h"
#include "dect_clip.h"

/* Longest -a loopdelay ring, in frames (1 s at 8 kHz). */
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


#define RING_BASIC_SVC	0x88u		/* normal call class, wideband speech */
#include "dect_g722.h"

/* Short read window per candidate slot while probing (blocks ~4 ms each). */
#define PROBE_BLOCKS	40u
#define POLL_MS		100

static volatile sig_atomic_t stop;
static uint64_t g_start;		/* for relative timestamps in the log */

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static uint64_t mono_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

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
	/* Identities for an FP-originated SETUP, learned and persisted. */
	uint8_t     cc_port_id[DECT_CC_ID_MAX];
	size_t      cc_port_id_len;
	uint8_t     cc_fixed_id[DECT_CC_ID_MAX];
	size_t      cc_fixed_id_len;
	/*
	 * Ringing the handset. A page is a broadcast the handset may simply
	 * miss, and the stock host guards each one with a 3 s timer, so page
	 * again until the handset brings up a link or we give up.
	 */
	enum { RING_IDLE, RING_PAGING, RING_WAIT_PAGE_RESP,
	       RING_DONE } ring_state;
	uint64_t ring_resp_deadline;	/* fall back if no page response comes */
	unsigned ring_setup_tries;	/* refused CC-SETUPs, see ring_queue_setup */
	unsigned    ring_pages;		/* pages sent for this attempt */
	uint64_t    ring_last;		/* when the last one went out */
	int sub_restored;	/* stored subscription already pushed to the chip */
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
	uint32_t    txq_drops;
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
		 * noconnect: never send 0x022D. The stock connect sequence is
		 * exactly 0x0251 then 0x0230 [CR A-0034]; 0x022D is the stock
		 * mute control (0x80 = mute), never sent at connect. Whether
		 * the chip reads more than bit 7 of its byte is not
		 * established, so omitting it is the clean comparison.
		 */
		int      no_connect;
		unsigned tone_hz;	/* transmit test tone (0 = off); multiple of 125 */
		uint8_t  tone_word;	/* raw DMA word to write it into (default 0) */
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
		struct { uint32_t addr; uint32_t len; } memdump[8];
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
		 * SETUP instead of rotating it. Stock uses TI 0 in every
		 * base->handset SETUP we have bytes for, and every call this
		 * project ever completed used TI 0 (v28, v30, v34, A2, A3, B2);
		 * every refused one used TI 1. Those are confounded with the
		 * handset -- the TI-0 runs are all the C4 -- so this exists to
		 * separate them. -1 keeps the rotation.
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
	int         play_phase;		/* 0 = load a new word, 1 = hold it */
	struct dect_clip clip;		/* caller identity for an FP-originated SETUP */
	uint8_t     next_ti;		/* rotates per FP-originated call */
	const char *voice_path;
	FILE       *voice_fp;
	int         voice_slot;		/* -V: one slot only; <0 = every slot */
	uint32_t    voice_bytes;
	uint32_t    voice_space_checked;
	struct reg_ctx {
		int active;
		int persisted;		/* chip subscription written this connection */
		int call_bound;		/* audio channel bound for the active call */
		int audio_connected;	/* 0x022D sent (after 0x0242, or fallback) */
		uint64_t bind_ms;	/* when 0x0230 went out, for the fallback */
		uint64_t status_ms;	/* when the last 0xF624 poll went out */
		uint16_t counter[10];	/* previous 0xF625 counters, for deltas */
		int have_counters;
		int handover_sent;	/* -a handover fired for this call */
		unsigned sweep_idx;	/* next GenDSP source for -a dspsweep */
		uint64_t sweep_ms;	/* when the current source was routed */
		int sweep_done;
		int cipher_started;	/* call-time authentication kicked off */
		int cipher_done;	/* cipher outcome already reported */
		uint8_t handle;
		struct dect_lc_rx lc;
		struct dect_lapc lapc;
		struct dect_mm mm;
		struct dect_cc cc;
	} reg;
};

/*
 * Log a mail. Parameter bytes are hidden by default: the 0xF601/0xF602
 * answers carry box identity (RFPI-like) data that must not leak into
 * shared logs or issues. Pass -p to show them for local debugging only.
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
	for (i = 0; i < m->param_len && i < sizeof(m->param); i++)
		printf(" %02x", m->param[i]);
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
static int send_mail(struct stubd *s, unsigned int tx_slot,
		     const struct dect_mail *m)
{
	uint8_t el[DECT_TX_ELEM_MAX], *frames;
	size_t fb = s->slots * 2u, nf, k;
	int el_len = dect_mail_encode(m, s->tx_seq, el, sizeof(el));
	ssize_t w;

	if (el_len <= 0 || tx_slot >= s->slots)
		return -1;

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
		if (s->audio.play && s->audio.play_raw && s->reg.call_bound) {
			unsigned int w = s->audio.tone_word;

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

				if (s->audio.play_sync && rx) {
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
		tx_tone(s, fr);
		if (s->txq_len >= 2u) {
			fr[(unsigned int)s->tx_slot * 2u] =
				s->txq[s->txq_head];
			fr[(unsigned int)s->tx_slot * 2u + 1u] =
				s->txq[(s->txq_head + 1u) % sizeof(s->txq)];
			s->txq_head = (s->txq_head + 2u) % sizeof(s->txq);
			s->txq_len -= 2u;
		}
	}
	if (write(s->fd, frames, fpb * fb) != (ssize_t)(fpb * fb))
		s->txq_drops++;
	s->tx_blocks++;
}

/* Parse "SRC:DST:PRIM[:HEX]" and send it as a mail in the control slot. */
static int send_mail_spec(struct stubd *s, const char *spec)
{
	struct dect_mail m;
	unsigned long src, dst, prim;
	const char *par = NULL;
	char *end;

	memset(&m, 0, sizeof(m));
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
	m.src = (uint8_t)src;
	m.dst = (uint8_t)dst;
	m.prim = (uint16_t)prim;
	for (; par && *par; par += 2) {
		unsigned int v;

		if (sscanf(par, "%2x", &v) != 1 ||
		    m.param_len >= DECT_MAIL_PARAM_MAX)
			return -1;
		m.param[m.param_len++] = (uint8_t)v;
	}
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
 * Wrap a LAPC frame in an Lc Cs frame and send it via APPEND_CONNECTION_DATA
 * (0x0244) only. AVM's host driver transmits C-plane replies this way and does
 * NOT follow with 0x0202; in AVM's code 0x0202 is sent on an abnormal link
 * release, which matches the ~40 ms connection drop we saw when we triggered it
 * [CR AVM]. LSIG = 0: the PP has no assigned PMID during registration.
 */
static void send_cplane(struct stubd *s, uint8_t handle,
			const uint8_t *lapc, size_t lapc_len)
{
	uint8_t cs[DECT_LC_FRAME_MAX];
	struct dect_mail m;
	/* Sign with the link's signature (0 while unregistered, the PP's PMID-
	 * derived LSIG once learned from its establishment frame). */
	size_t flen = dect_lc_build(lapc, lapc_len, s->reg.lc.lsig, cs, sizeof(cs));

	if (flen == 0)
		return;
	if (dect_cs_append_mail(&m, DECT_TASK_PCM, handle, cs, flen) == 0)
		send_mail(s, (unsigned int)s->tx_slot, &m);
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

static void sub_save(struct stubd *s)
{
	struct sub_record r;
	int fd;

	if (!s->sub_path || !s->uak_valid)
		return;
	memset(&r, 0, sizeof(r));
	r.magic = SUB_MAGIC;
	r.slot = dect_mm_sub_slot(&s->reg.mm);
	memcpy(r.uak, s->uak, DECT_MM_UAK_LEN);
	r.magic = SUB_MAGIC2;
	r.port_id_len = (uint8_t)s->reg.mm.port_id_len;
	memcpy(r.port_id, s->reg.mm.port_id, sizeof(r.port_id));
	r.cc_port_id_len = (uint8_t)s->cc_port_id_len;
	memcpy(r.cc_port_id, s->cc_port_id, sizeof(r.cc_port_id));
	r.cc_fixed_id_len = (uint8_t)s->cc_fixed_id_len;
	memcpy(r.cc_fixed_id, s->cc_fixed_id, sizeof(r.cc_fixed_id));

	fd = open(s->sub_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("sub: cannot write %s: %s\n", s->sub_path, strerror(errno));
		return;
	}
	if (write(fd, &r, sizeof(r)) != (ssize_t)sizeof(r))
		printf("sub: short write to %s\n", s->sub_path);
	else
		printf("sub: subscription saved to %s\n", s->sub_path);
	close(fd);
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
	if (r.magic == SUB_MAGIC2 && r.cc_port_id_len <= DECT_CC_ID_MAX &&
	    r.cc_fixed_id_len <= DECT_CC_ID_MAX) {
		memcpy(s->cc_port_id, r.cc_port_id, sizeof(s->cc_port_id));
		s->cc_port_id_len = r.cc_port_id_len;
		memcpy(s->cc_fixed_id, r.cc_fixed_id, sizeof(s->cc_fixed_id));
		s->cc_fixed_id_len = r.cc_fixed_id_len;
	}
	printf("sub: restored subscription for slot %u from %s%s\n",
	       r.slot, s->sub_path,
	       s->cc_fixed_id_len ? " (with call identities)" : "");
	return 1;
}

/*
 * Persist the just-enrolled handset in the chip so it holds standby and is
 * reachable: 0x0247 {slot, op=1, status} then 0x020A {slot, UAK[16], extra},
 * to STUB task 0x02, in that order, after LOCATE-ACCEPT [CR A-0026]. The 16-byte
 * 0x020A record is the allocated UAK. The `status`/`extra` security-mode bytes
 * have no known enumeration [CR A-0026]; we send 0x00 and validate on hardware.
 * 0xF605 / 0x015F / 0x017C are intentionally NOT sent (not on AVM's ordinary
 * voice-handset path).
 */
static void send_subscription(struct stubd *s)
{
	struct dect_mail m;
	uint8_t slot = dect_mm_sub_slot(&s->reg.mm);

	memset(&m, 0, sizeof(m));
	m.src = DECT_TASK_PCM;
	m.dst = DECT_TASK_CONTACT_DST;
	m.prim = 0x0247u;
	m.param[0] = dect_mm_sub_slot(&s->reg.mm);	/* slot */
	m.param[1] = 0x01u;		/* op: install/refresh */
	m.param[2] = 0x00u;		/* status (unknown enumeration) */
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
	memcpy(&m.param[1], s->uak, DECT_MM_UAK_LEN);		/* 16-byte credential */
	m.param[1 + DECT_MM_UAK_LEN] = 0x00u;			/* extra (unknown) */
	m.param_len = (uint16_t)(2 + DECT_MM_UAK_LEN);
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)			/* params carry the UAK: -p is local only */
		log_mail(&m, "tx M", s->show_params);
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
	m.param[0] = handle;
	m.param[1] = a->flags;		/* mode: low nibble = channel, 0x40 = wideband */
	m.param[2] = (uint8_t)(a->mask & 0xFFu);	/* slot mask (LE) */
	m.param[3] = (uint8_t)(a->mask >> 8);
	m.param[4] = a->mode;		/* master */
	m.param[5] = a->codec;		/* extra */
	m.param_len = 6;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);

	printf("call: audio assign: 0x0251 %s(sel %u gain 0x%02x), "
	       "0x0230 {%02x %04x %02x %02x}%s\n",
	       a->no_gain ? "SKIPPED " : "", a->sel, a->gain,
	       a->flags, a->mask, a->mode, a->codec,
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
 */
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
#define RING_PAGE_RETRIES	20u	/* pages before giving up (60 s) */
#define RING_PAGE_INTERVAL	3000u	/* ms between them; the stock guard */
#define RING_PAGE_RESP_MS	1500u	/* wait this long for the page response */
#define RING_SETUP_RETRY_MS	2000u	/* re-try a refused CC-SETUP this often */
#define RING_SETUP_TRIES	8u	/* ... this many times before giving up */

static void send_page(struct stubd *s)
{
	struct dect_mail m;
	uint8_t tpui[3];

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
	dect_mm_tpui(&s->reg.mm, tpui);
	m.param[0] = ring_page_info(s);
	m.param[1] = tpui[1];
	m.param[2] = tpui[2];
	m.param_len = 3;
	send_mail(s, (unsigned int)s->tx_slot, &m);
	if (s->verbose)
		log_mail(&m, "tx M", s->show_params);
	printf("ring: paged the handset, attempt %u of %u "
	       "(0x0300 {%02x %02x %02x}, basic service %02x)\n",
	       s->ring_pages, RING_PAGE_RETRIES, ring_page_info(s),
	       tpui[1], tpui[2], s->audio.basic_svc);
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
	 * dect_cc_setup_id() refuses unless the CC entity is idle, and this is
	 * a real collision rather than a rare one: the A540 CAT opens a CAT-iq
	 * service call of its own roughly every 17 s and holds it for about
	 * 13 s. Run F4 caught it exactly -- the handset's call ran 48.6 s to
	 * 61.6 s and our ring fired at 58.5 s -- so abandoning the ring on the
	 * first refusal loses the call about as often as not. Retry while the
	 * paging window is still open.
	 */
	if (s->audio.ti_fixed >= 0)
		s->next_ti = (uint8_t)s->audio.ti_fixed;
	else
		s->next_ti = (uint8_t)((s->next_ti + 1u) % 7u);
	dect_cc_set_next_ti(&s->reg.cc, s->next_ti);
	if (!dect_cc_setup_id(&s->reg.cc, s->cc_port_id, s->cc_port_id_len,
			      s->cc_fixed_id, s->cc_fixed_id_len,
			      s->audio.basic_svc, &s->clip)) {
		s->ring_state = RING_DONE;
		printf("ring: CC-SETUP queued (%s) -- the handset should ring\n",
		       why);
		return;
	}
	if (++s->ring_setup_tries >= RING_SETUP_TRIES) {
		s->ring_state = RING_DONE;
		printf("ring: CC-SETUP refused %u times, the handset's own call "
		       "never cleared; giving up\n", s->ring_setup_tries);
		return;
	}
	s->ring_resp_deadline = now + RING_SETUP_RETRY_MS;
	printf("ring: CC-SETUP refused (a call is in progress), retry %u of %u "
	       "in %u ms\n", s->ring_setup_tries, RING_SETUP_TRIES,
	       RING_SETUP_RETRY_MS);
}


static void ring_tick(struct stubd *s, uint64_t now)
{
	if (!s->audio.ring_ms || s->ring_state == RING_DONE ||
	    s->sess.state != DECT_SESS_UP)
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
		if (now - g_start < s->audio.ring_ms)
			return;
		if (!s->cc_fixed_id_len) {
			printf("ring: no stored call identities -- let the "
			       "handset place one call first (with -s), then "
			       "retry\n");
			s->ring_state = RING_DONE;
			return;
		}
		s->ring_state = RING_PAGING;
		/*
		 * A link may already be up, in which case the handset does not
		 * need paging at all and the SETUP can go straight out.
		 */
		if (s->reg.active) {
			ring_queue_setup(s, now, "a link was already up");
			return;
		}
	}

	if (s->ring_pages && now - s->ring_last < RING_PAGE_INTERVAL)
		return;
	if (s->ring_pages >= RING_PAGE_RETRIES) {
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
 * Decode the 0xF625 answer. The ten RSSI bytes are the anchor that fixes the
 * offsets: they sit at the same place in the chip's DECT-status answer
 * 0xF623, and the whole record is 45 bytes [CR the hardware-analysis notes
 * §5 4.4]:
 *
 *   [0]        link handle      [1]        the selector we asked with
 *   [2]        MAC slot         [3]        RF carrier
 *   [5]        codec / bearer slot type (0 = full slot, 1..3 = long slot)
 *   [6..0x0F]  ten RSSI bytes, one per carrier
 *   [0x10..]   ten u16 counters [0x28..] u32 BER
 *
 * The host's column order for this record is duration, received B-field
 * frames, Q1/Q2 errors, slide errors, sync errors, handovers and the A/X/Z
 * CRC counts -- but the same source says the host *reorders* them when it
 * builds that view [CR the hardware-analysis notes §5 4.4], so the wire order
 * is not known. We print them by index and interpret them in the doc rather
 * than baking a guessed mapping into the output.
 *
 * The B-field frame counter is the one that matters: it separates "a bearer
 * exists" from "voice frames are actually arriving", which is the distinction
 * A-0038 §5 names as the crucial missing stage. Deltas are what count, so we
 * print the change since the previous poll.
 */
static void report_conn_status(struct stubd *s, const struct dect_mail *m)
{
	const uint8_t *p = m->param;
	unsigned n = m->param_len;
	uint16_t c[10];
	uint32_t ber;
	unsigned i;

	if (n < 6u) {
		printf("call: status: short 0xF625 (%u bytes)\n", n);
		return;
	}
	printf("call: status: mac slot %u carrier %u codec/slottype 0x%02x (%s)",
	       p[2], p[3], p[5],
	       p[5] == 0u ? "full slot, G.726" :
	       (p[5] <= 3u ? "long slot, wideband" : "unknown"));
	if (n < 0x2Cu) {
		printf(" -- no counters (%u bytes)\n", n);
		return;
	}
	for (i = 0; i < 10u; i++)
		c[i] = (uint16_t)(p[0x10u + 2u * i] | (p[0x11u + 2u * i] << 8));
	ber = (uint32_t)p[0x28] | ((uint32_t)p[0x29] << 8) |
	      ((uint32_t)p[0x2A] << 16) | ((uint32_t)p[0x2B] << 24);

	printf("\ncall: status: counters");
	for (i = 0; i < 10u; i++) {
		int d = (int)c[i] - (int)s->reg.counter[i];
		printf(" c%u=%u%+d", i, c[i],
		       s->reg.have_counters ? d : 0);
	}
	printf(" ber=%u\n", ber);
	memcpy(s->reg.counter, c, sizeof(c));
	s->reg.have_counters = 1;
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

	if (!dect_mm_cipher_dck(&s->reg.mm, dck))
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
			? s->reg.mm.cipher_res[i] : s->reg.mm.cipher_res[3u - i];
	n += 4u;

	/* mail[15..22]: second half of the 16-byte input, copied intact. */
	memcpy(&m.param[n], s->chip_cipher.use_rs ? s->reg.mm.rs
						  : s->reg.mm.rand_f, 8);
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
static void pump_lapc(struct stubd *s)
{
	uint8_t frame[DECT_LC_LAPC_MAX];
	size_t n;

	while ((n = dect_lapc_poll_tx(&s->reg.lapc, frame, sizeof(frame))) > 0) {
		if (s->verbose)
			printf("[%5llu] reg: tx cplane %zu bytes (addr %02x ctrl %02x)\n",
			       (unsigned long long)(mono_ms() - g_start), n,
			       frame[0], frame[1]);
		send_cplane(s, s->reg.handle, frame, n);
	}
}

static void reg_start(struct stubd *s, uint8_t handle)
{
	struct dect_mm_cfg mcfg;

	memset(&mcfg, 0, sizeof(mcfg));
	if (s->pin)
		snprintf(mcfg.pin, sizeof(mcfg.pin), "%s", s->pin);
	mcfg.authenticate = 1;
	mcfg.narrowband_only = (uint8_t)s->audio.nbcodec;
	if (s->have_rfpi)
		memcpy(mcfg.rfpi, s->rfpi, DECT_MM_RFPI_LEN);

	s->reg.active = 1;
	s->reg.persisted = 0;
	s->reg.call_bound = 0;
	s->reg.cipher_started = 0;
	s->reg.cipher_done = 0;
	s->reg.handle = handle;
	dect_lc_rx_init(&s->reg.lc, 0);
	dect_lapc_init(&s->reg.lapc, 0);
	dect_mm_init(&s->reg.mm, &mcfg);
	dect_cc_init(&s->reg.cc);
	dect_cc_set_signal(&s->reg.cc, s->audio.signal_on,
			   s->audio.signal_value);
	dect_cc_set_callinfo(&s->reg.cc, &s->audio.callinfo);
	/* a fresh transaction per call; see dect_cc_set_next_ti() */
	dect_cc_set_next_ti(&s->reg.cc, s->next_ti);
	printf("reg: connection up (handle %u), C-plane armed\n", handle);
}

/* Drain queued NWK responses (MM and CC) into LAPC, one per free window. */
static void drain_nwk(struct stubd *s, uint64_t now)
{
	uint8_t msg[DECT_LAPC_MSG_MAX];
	size_t mlen;

	while (!s->reg.lapc.outstanding) {
		mlen = dect_mm_poll(&s->reg.mm, msg, sizeof(msg));
		if (mlen == 0)
			mlen = dect_cc_poll(&s->reg.cc, msg, sizeof(msg));
		if (mlen == 0)
			break;
		if (s->verbose) {
			size_t i;

			printf("reg: nwk out --> ");
			for (i = 0; i < mlen; i++)
				printf("%02x ", msg[i]);
			printf("\n");
		}
		if (dect_lapc_send(&s->reg.lapc, msg, mlen, now) != 0)
			break;
		pump_lapc(s);
	}
}

/* Feed one 0x0203 signalling segment up the stack; send any responses. */
static void reg_segment(struct stubd *s, const struct dect_mail *m, uint64_t now)
{
	struct dect_cs_seg seg;
	uint8_t lapcbuf[DECT_LC_LAPC_MAX], nwk[DECT_LAPC_MSG_MAX];
	struct dect_lapc_frame f;
	int lapc_len, nwk_len;

	if (dect_cs_decode_segment(m, &seg) < 0)
		return;
	lapc_len = dect_lc_rx_fragment(&s->reg.lc, seg.data, lapcbuf, sizeof(lapcbuf));
	if (s->verbose)
		printf("reg: cs in %02x %02x %02x (addr/ctrl/li), lc_rc=%d\n",
		       seg.data[0], seg.data[1], seg.data[2], lapc_len);
	/*
	 * A segment we cannot use must not stop our own queued messages: an
	 * incoming call queues the CC-SETUP before the handset's first frame
	 * arrives, and returning here left it sitting in the queue for ever.
	 */
	if (lapc_len <= 0) {
		drain_nwk(s, now);
		return;
	}
	if (dect_lapc_parse(lapcbuf, (size_t)lapc_len, &f) < 0) {
		if (s->verbose)
			printf("reg: lapc parse failed (len %d)\n", lapc_len);
		drain_nwk(s, now);
		return;
	}
	if (s->verbose)
		printf("reg: lapc type=%d ns=%u nr=%u more=%u infolen=%zu\n",
		       f.type, f.ns, f.nr, f.more, f.info_len);
	nwk_len = dect_lapc_rx(&s->reg.lapc, &f, now, nwk, sizeof(nwk));
	if (s->verbose)
		printf("reg: nwk_len=%d\n", nwk_len);
	pump_lapc(s);				/* at least the LAPC ack */
	if (nwk_len > 0) {
		struct dect_nwk_hdr h;
		const uint8_t *ies;
		size_t ies_len;
		int parsed;

		if (s->verbose) {		/* local only: carries IPUI, never commit */
			int i;

			printf("reg: nwk in  <-- ");
			for (i = 0; i < nwk_len; i++)
				printf("%02x ", nwk[i]);
			printf("\n");
		}
		/* Route by protocol discriminator: LCE, CC, else MM. */
		parsed = dect_nwk_parse(nwk, (size_t)nwk_len,
					&h, &ies, &ies_len) == 0;
		if (parsed && h.pd == DECT_PD_LCE) {
			/*
			 * The handset's answer to our page. This is the point
			 * at which an FP-originated CC-SETUP can be sent: see
			 * ring_queue_setup() for the run that showed sending it
			 * on link-up loses the frame.
			 */
			if (s->ring_state == RING_WAIT_PAGE_RESP)
				ring_queue_setup(s, now, "page response received");
		} else if (parsed && h.pd == DECT_PD_CC) {
			dect_cc_rx(&s->reg.cc, nwk, (size_t)nwk_len);
			/*
			 * A call the handset places carries the two identities
			 * an FP-originated SETUP needs, in the handset's own
			 * encoding. Keep the first pair we see and persist it,
			 * so calling the handset later needs no guesswork about
			 * identity types, bit lengths or padding.
			 */
			if (!s->cc_fixed_id_len && s->reg.cc.peer_fixed_id_len) {
				memcpy(s->cc_port_id, s->reg.cc.peer_port_id,
				       sizeof(s->cc_port_id));
				s->cc_port_id_len = s->reg.cc.peer_port_id_len;
				memcpy(s->cc_fixed_id, s->reg.cc.peer_fixed_id,
				       sizeof(s->cc_fixed_id));
				s->cc_fixed_id_len = s->reg.cc.peer_fixed_id_len;
				printf("call: learned the identities for calling "
				       "the handset (%zu + %zu bytes), saving\n",
				       s->cc_port_id_len, s->cc_fixed_id_len);
				sub_save(s);
			}
		} else {
			uint8_t rand_f[8], rs[16];

			gen_rand(s, rand_f, sizeof(rand_f));
			gen_rand(s, rs, sizeof(rs));
			dect_mm_rx(&s->reg.mm, nwk, (size_t)nwk_len, rand_f, rs);
		}
	}

	/* The next message goes out when the handset acknowledges the previous one. */
	drain_nwk(s, now);

	/*
	 * Once the handset is authenticated (UAK valid) and we have answered its
	 * LOCATE-REQUEST, install the subscription in the chip, once per connection.
	 */
	if (!s->reg.persisted && s->reg.mm.subscribed && s->reg.mm.stats.located) {
		/* Keep the UAK for the later call's ciphering, and across restarts. */
		memcpy(s->uak, s->reg.mm.uak, DECT_MM_UAK_LEN);
		s->uak_valid = 1;
		send_subscription(s);
		sub_save(s);
		/*
		 * Close registration once the handset is enrolled. AVM's documented
		 * sequence opens registration (0x022B 'A') and closes it afterwards
		 * with 0x022C 01, which clears the a44 "registration open" capability
		 * [CR COMMAND_SET §6.2/§9.7]. Leaving it open keeps the base
		 * advertising open enrolment and the handset re-attaching.
		 */
		send_mail_spec(s, "0x22:0x02:0x022c:01");
		s->reg.persisted = 1;
		printf("reg: subscription persisted + registration closed (slot %u)\n",
		       (unsigned)dect_mm_sub_slot(&s->reg.mm));
	}

	if (s->reg.mm.state == DECT_MM_REGISTERED)
		printf("reg: handset registered\n");
	else if (s->reg.mm.state == DECT_MM_FAILED)
		printf("reg: registration rejected\n");

	/*
	 * Bind the voice path once we have answered the call (CONNECT sent). The
	 * C4 treats CONNECT as answered even though it does not send CONNECT-ACK.
	 */
	if (!s->reg.call_bound &&
	    (s->reg.cc.state == DECT_CC_ST_CONNECT_PENDING ||
	     s->reg.cc.state == DECT_CC_ST_ACTIVE)) {
		s->reg.audio_connected = 0;
		s->reg.bind_ms = now;
		s->reg.status_ms = 0;
		s->reg.have_counters = 0;
		/*
		 * Source 0 leaves the destination disconnected (measured with
		 * the radio off), so it carries no information during a call and
		 * only drops whatever route the bind installed. Start at 1.
		 */
		s->reg.handover_sent = 0;
		s->cmail_idx = 0;	/* re-arm the -N sweep for this call */
		s->reg.sweep_idx = 1;
		s->reg.sweep_ms = now;	/* let the call settle for one step */
		s->reg.sweep_done = 0;
		send_audio_bind(s, s->reg.handle);
		s->reg.call_bound = 1;
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
			uint8_t rand_f[8], rs[16];

			gen_rand(s, rand_f, sizeof(rand_f));
			gen_rand(s, rs, sizeof(rs));
			dect_mm_start_cipher(&s->reg.mm, s->uak, rand_f, rs);
			s->reg.cipher_started = 1;
			printf("call: cipher: authenticating handset\n");
			drain_nwk(s, now);
		} else if (!s->uak_valid) {
			printf("call: cipher: no UAK (handset must enrol this session)\n");
		}
	}

	/* Never log key material: state only. */
	if (s->reg.mm.cipher_state == DECT_MM_CIPHER_READY && !s->reg.cipher_done) {
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
	} else if (s->reg.mm.cipher_state == DECT_MM_CIPHER_FAILED &&
		   !s->reg.cipher_done) {
		s->reg.cipher_done = 1;
		printf("call: cipher: authentication FAILED (RES mismatch)\n");
	}
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
		if (s->reg.active && h == s->reg.handle
		    && (s->reg.call_bound || dect_cc_busy(&s->reg.cc))) {
			printf("reg: connection up (handle %u) repeated while "
			       "a call is in progress -- keeping call state\n",
			       h);
			break;
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
	case DECT_PRIM_CONN_DOWN:
		if (s->reg.active)
			printf("reg: connection released\n");
		s->reg.active = 0;
		break;
	case DECT_PRIM_SIGNALLING:
		if (s->reg.active)
			reg_segment(s, m, now);
		break;
	case 0x0242:
		/*
		 * Channel assignment confirmed: {handle, chan_flags, mask}. The
		 * low nibble is the channel the chip actually gave us; 0 or 0xF
		 * means none. Only the long form answers 0x0230 [CR COMMAND_SET
		 * §8.4]; the short one reports later state changes.
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
		}
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
			       s->audio.play_sync
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
	s->tx_ring = 1;		/* from here on, mails are paced by tx_block() */
	g_start = start;
	while (!stop) {
		struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
		uint64_t now;
		int pr = poll(&pfd, 1, POLL_MS);

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

		now = mono_ms();
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
		 * Reload a stored subscription into the chip once the radio is up,
		 * the way AVM restores saved handsets at boot [CR A-0026], so the
		 * handset stays known across daemon restarts.
		 */
		if (s->sess.state == DECT_SESS_UP && s->xmail_idx >= s->n_xmail &&
		    s->uak_valid && !s->sub_restored) {
			send_subscription(s);
			s->sub_restored = 1;
			printf("sub: stored subscription reloaded into the chip\n");
		}
		/* Drive LAPC retransmission timers for the registration link. */
		if (s->do_register && s->reg.active) {
			dect_lapc_tick(&s->reg.lapc, now);
			dect_cc_tick(&s->reg.cc, now);
			pump_lapc(s);
		}
		if (run_seconds && now - start >= (uint64_t)run_seconds * 1000u)
			break;
	}
	printf("session: state %d contacts %u answers %u status %u deaths %u\n",
	       s->sess.state, s->sess.stats.contacts_sent, s->sess.stats.answers,
	       s->sess.stats.status_mails, s->sess.stats.deaths);
	printf("tx: blocks %u queue_drops %u idle %02x voice_idle %02x\n",
	       s->tx_blocks, s->txq_drops, s->tx_idle, s->tx_voice_idle);
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
			/* "dsp" and "dspsweep" carry a ':' in their value. */
			if (end != spec &&
			    !(klen == 3 && !memcmp(key, "dsp", 3)) &&
			    !(klen == 8 && !memcmp(key, "dspsweep", 8)) &&
			    !(klen == 4 && !memcmp(key, "play", 4)) &&
			    !(klen == 7 && !memcmp(key, "playpcm", 7)) &&
			    !(klen == 7 && !memcmp(key, "playraw", 7)) &&
			    !(klen == 7 && !memcmp(key, "playlog", 7)) &&
			    !(klen == 4 && !memcmp(key, "clip", 4)) &&
			    !(klen == 7 && !memcmp(key, "memdump", 7)) &&
			    !(klen == 4 && !memcmp(key, "cnip", 4)))
				return -1;
		}
		if (!eq && klen == 4 && !memcmp(key, "f626", 4))
			a->send_f626 = 1;
		else if (!eq && klen == 7 && !memcmp(key, "nbcodec", 7))
			a->nbcodec = 1;
		else if (!eq && klen == 6 && !memcmp(key, "nogain", 6))
			a->no_gain = 1;
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
			a->basic_svc = (uint8_t)v;
		else if (klen == 2 && !memcmp(key, "ti", 2))
			a->ti_fixed = (int)v;
		else if (klen == 6 && !memcmp(key, "signal", 6)) {
			a->signal_value = (uint8_t)v;
			a->signal_on = 1;
		}
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

			if (a->n_memdump >= 8u || !eq)
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
		"  -P PIN   registration PIN / AC (default 0000; used with -g)\n"
		"  -s PATH  subscription store, holds the handset key; reloaded into\n"
		"           the chip at start-up (default /etc/opendectcell-sub, \"\" = off)\n"
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
		"           ring=MS calls the handset MS ms after start-up: page\n"
		"           (0x0300) then an FP-originated CC-SETUP\n"
		"           play=FILE transmits a raw G.722 stream (8000 bytes/s) to\n"
		"           the handset while a call is up, looping -- a clean\n"
		"           downlink source, unlike the loopback\n"
		"           playpcm=FILE does the same from 16 kHz signed 16-bit\n"
		"           little-endian PCM, encoding it to G.722 at start-up\n"
		"           signal=N adds <<SIGNAL>> N to the SETUP (0x40..0x47 =\n"
		"           alerting pattern 0..7); omitted, the handset picks\n"
		"           basicsvc=N sets <<BASIC-SERVICE>> (default 0x88: high\n"
		"           nibble call class, low nibble 8 = wideband speech)\n"
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
	int c, ret = 1;

	memset(&s, 0, sizeof(s));
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
	s.audio.clip_ton = DECT_CLIP_TON_UNKNOWN;
	s.audio.clip_npi = DECT_CLIP_NPI_E164;
	s.audio.clip_scr = DECT_CLIP_SCR_USER_UNSCREENED;
	s.audio.cnip_scr = DECT_CLIP_SCR_USER_UNSCREENED;
	s.audio.cnip_alpha = DECT_CLIP_ALPHABET_UTF8;
	s.sweep_byte = 0;
	s.rx_word_prev = 0xFFFFFFFFu;	/* no word seen yet */
	s.audio.memdump_ms = 50u;	/* default chip-memory poll interval */
	s.audio.basic_svc = RING_BASIC_SVC;
	s.audio.ti_fixed = -1;
	s.audio.signal_value = DECT_SIGNAL_ALERT_PATTERN0;
	s.audio.sweep_dst = -1;		/* no GenDSP source sweep */
	s.tx_idle = 0xFFu;
	s.tx_voice_idle = 0xFFu;
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
			s.cmail[s.n_cmail++] = optarg;
			break;
		case 'M':
			if (s.n_xmail >= sizeof(s.xmail) / sizeof(s.xmail[0])) {
				usage();
				return 2;
			}
			s.xmail[s.n_xmail++] = optarg;
			break;
		case 'g': s.do_register = 1; break;
		case 'P': s.pin = optarg; break;
		case 's': s.sub_path = optarg[0] ? optarg : NULL; break;
		case 'C': s.no_cipher = 1; break;
		case 'a':
			if (parse_audio(&s.audio, optarg)) {
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
		/* src/dst/prim parse with base 0, so hex needs the 0x prefix. */
		s.xmail[s.n_xmail++] = "0x22:0x02:0x015b:01";	/* radio/beacon on */
		s.xmail[s.n_xmail++] = "0x22:0x02:0x022b:41";	/* 'A': open reg */
	}
	if (optind != argc) {
		usage();
		return 2;
	}

	if (s.do_register)
		sub_load(&s);		/* restores the UAK; chip reload once up */

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
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
		if (find_control_slot(&s, fpb)) {
			fprintf(stderr, "control slot not found\n");
			goto out;
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
	ret = run_loop(&s, fpb, run_seconds);
	if (s.voice_fp) {
		fclose(s.voice_fp);
		printf("voice: captured %u bytes from slot %d (%u frames)\n",
		       s.voice_bytes, s.voice_slot, s.voice_bytes / 2u);
	}

	/* Park the radio so the base does not keep beaconing with registration
	 * open after we exit. [CR COMMAND_SET §6.2/§6.1] */
	if (s.do_register && s.tx_slot >= 0) {
		send_mail_spec(&s, "0x22:0x02:0x022c:01");	/* close registration */
		send_mail_spec(&s, "0x22:0x02:0x015b:00");	/* radio off */
		printf("reg: radio parked\n");
	}
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

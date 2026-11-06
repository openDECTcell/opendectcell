// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dect-stubd, calls: binding the chip's audio to a call, connection status, the ring state
 * machine's host side, side links (second and third call), internal calls, deregistration, and
 * the library's call callbacks. Moved out of dect-stubd.c unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "stubd_internal.h"

/* functions used before their definition below */
static int ic_start(struct stubd *s, int side, uint64_t now);

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
void send_audio_disconnect(struct stubd *s, uint8_t handle)
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
void send_audio_connect(struct stubd *s, uint8_t handle, uint8_t chan)
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
void audio_connect_fallback(struct stubd *s, uint64_t now)
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
void remode_tick(struct stubd *s, uint64_t now)
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
void seq_tick(struct stubd *s, uint64_t now)
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
void mem_dump_tick(struct stubd *s, uint64_t now)
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
void send_conn_status_req(struct stubd *s, uint64_t now)
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
 * Print every NWK message we send, as `reg: nwk out --> ...`.
 *
 * This line predates the #67 refactor, which moved the drain into the
 * library and removed it without anyone noticing -- including me, who wrote
 * the refactor. The first run that needed it read as "we never answered a
 * LOCATE-REQUEST", which would have been a serious protocol defect; what had
 * actually happened was that we answered and stopped saying so. Reading a log
 * for an absence only works if the thing would have been printed.
 */
void log_nwk_out(void *ctx, const uint8_t *msg, size_t len)
{
	const struct stubd *s = ctx;
	size_t i;

	if (!s->verbose)
		return;
	printf("reg: nwk out --> ");
	if (!s->show_params) {
		char red[3 * 256 + 64];

		(void)dect_nwk_format_redacted(msg, len, red, sizeof(red));
		printf("%s\n", red);
		fflush(stdout);
		return;
	}
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
uint8_t target_svc(const struct stubd *s, int i, uint8_t configured)
{
	return dect_base_basic_svc(&s->base, i, configured);
}

/* Stock's next step for a Part 3 handset (dect_base.h, S3p). */
void cs_connect_tick(struct stubd *s, uint64_t now)
{
	dect_base_cs_connect_tick(&s->base, now);
}


/* The ring's sequencing is the library's (S3n). */
void ring_tick(struct stubd *s, uint64_t now)
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
void report_conn_status(struct stubd *s, const struct dect_mail *m)
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

struct side_link *side_find(struct stubd *s, uint8_t handle)
{
	unsigned i;

	for (i = 0; i < SIDE_MAX; i++)
		if (s->side[i].bl->active && s->side[i].bl->handle == handle)
			return &s->side[i];
	return NULL;
}

void log_nwk_out_side(void *ctx, const uint8_t *msg, size_t len)
{
	const struct stubd *s = ctx;
	size_t i;

	if (!s->verbose)
		return;
	printf("side: nwk out --> ");
	if (!s->show_params) {
		char red[3 * 256 + 64];

		(void)dect_nwk_format_redacted(msg, len, red, sizeof(red));
		printf("%s\n", red);
		fflush(stdout);
		return;
	}
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
int lanes_available(const struct stubd *s, const struct side_link *me)
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

struct xlane *xlane_of(struct stubd *s, uint8_t handle)
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

void xlane_connect(struct stubd *s, struct xlane *x)
{
	if (x->connected)
		return;
	x->connected = 1;
	dect_base_audio_path(&s->base, x->handle);
	printf("side: audio connect: 0x022D on handle %u (channel %u)\n",
	       x->handle, x->channel);
}

void xlane_release(struct stubd *s, struct side_link *sl)
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
void side_call_tick(struct stubd *s, struct side_link *sl, uint64_t now)
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

struct dect_cc *ic_cc(struct stubd *s, int side)
{
	return side < 0 ? &s->base.main_link.link.cc : &s->side[side].bl->link.cc;
}

int ic_active(const struct stubd *s, int p)
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

/*
 * dect_base's forget() (S4b): handset i is deregistered. The store, its
 * .name, .profile and .cap move aside (<store>.deregistered), so a mistake
 * can be undone by moving them back; the clients hear it is gone.
 */
void base_forget(void *ctx, unsigned i, const char *why)
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
void base_mask(void *ctx)
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
void dereg_tick(struct stubd *s, uint64_t now)
{
	if (want_deregister) {
		want_deregister = 0;
		dereg_start(s, now);
	}
	dect_base_dereg_tick(&s->base, now);
}

/* dect_base's wall(): the wall clock, right or not. */
void base_wall(void *ctx, struct dect_time *dt)
{
	(void)ctx;
	clock_any(dt);
}

/* dect_base's clock(): the wall clock, when it is known to be right. */
int base_clock(void *ctx, struct dect_time *dt, long *off_s)
{
	struct stubd *s = ctx;

	return clock_now(s->audio.timesync, dt, off_s);
}

/*
 * -a timeupdate=MIN: the date and time to every handset we hold, every MIN
 * minutes (maintainer, 2026-10-29), and the notices for handsets with no
 * link: the library's (dect_base.h, S4c), while the radio is on.
 */
void time_tick(struct stubd *s, uint64_t now, int radio_on)
{
	if (radio_on)
		dect_base_time_tick(&s->base, now);
}

/*
 * The internal call's progress is the library's (S3q); the bridge of the
 * two parties' audio is ours.
 */
void ic_tick(struct stubd *s, uint64_t now)
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
int ic_word(const struct stubd *s, int side)
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

void side_pump(struct stubd *s, struct side_link *sl)
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
void side_cipher_tick(struct stubd *s, struct side_link *sl,
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
void base_registered(void *ctx, uint64_t now)
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
void base_call_up(void *ctx, uint64_t now)
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
void base_side_call(void *ctx, unsigned k, uint64_t now)
{
	struct stubd *s = ctx;

	side_call_tick(s, &s->side[k], now);
}

/*
 * dect_base's call_down(): the main call is over, its audio channel goes
 * (dect_base_tick(); loop_tick() does the same in its own order).
 */
void base_call_down(void *ctx)
{
	struct stubd *s = ctx;

	send_audio_disconnect(s, s->base.main_link.handle);
	s->reg.call_wideband = 0;
}

/* dect_base's swapped(): side link k and the main link changed places. */
void base_swapped(void *ctx, unsigned k)
{
	struct stubd *s = ctx;

	dect_link_set_tx_log(&s->base.main_link.link, log_nwk_out, s);
	dect_link_set_tx_log(&s->side[k].bl->link, log_nwk_out_side, s);
	s->reg.call_wideband = 0;
	s->reg.nonspeech_noted = 0;
}

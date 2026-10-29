// SPDX-License-Identifier: GPL-2.0-or-later
/* Diagnostics as data. See dect_diag.h and docs/diagnostics.md. */
#include "dect_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Where the eight event counters sit, and the condition that drives each. */
static const uint8_t event_off[DECT_DIAG_EVENTS] = {
	0x10u, 0x12u, 0x14u, 0x16u, 0x18u, 0x1Cu, 0x1Eu, 0x20u
};
static const char *const event_name[DECT_DIAG_EVENTS] = {
	"acrc", "xcrc", "zcrc", "qbit1", "qbit2", "handover", "slideerr",
	"syncerr"
};

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

void dect_diag_from_link(const struct dect_link *l, struct dect_diag_link *d)
{
	const struct dect_mm *mm = &l->mm;
	const struct dect_cc *cc = &l->cc;
	struct dect_diag_status radio = d->radio;
	uint64_t frames = d->call_frames;

	memset(d, 0, sizeof(*d));
	d->radio = radio;
	d->call_frames = frames;
	d->handle = l->handle;
	if (mm->port_id_len <= sizeof(d->identity)) {
		memcpy(d->identity, mm->port_id, mm->port_id_len);
		d->identity_len = mm->port_id_len;
	}
	d->handset_tag = mm->adopted;
	d->slot = dect_mm_sub_slot(mm);
	dect_mm_tpui(mm, d->tpui);
	d->located = mm->stats.located != 0;
	d->tpui_confirmed = (uint8_t)dect_mm_tpui_confirmed(mm);
	d->registered = mm->subscribed;
	memcpy(d->model, mm->peer_model, mm->peer_model_len);
	d->model_len = mm->peer_model_len;
	memcpy(d->codecs, mm->peer_codecs, mm->peer_codecs_len);
	d->codecs_len = mm->peer_codecs_len;
	d->call_state = cc->state;
	d->call_incoming = cc->incoming;
	d->basic_service = cc->peer_basic_service;
	d->chosen_codec = cc->chosen_codec;
	d->ciphered = mm->cipher_state == DECT_MM_CIPHER_READY;
}

int dect_diag_decode_status(const uint8_t *p, size_t n,
			    struct dect_diag_status *st)
{
	unsigned i;

	memset(st, 0, sizeof(*st));
	if (!p || n < 0x10u)
		return -1;
	st->valid = 1;
	st->link = p[0];
	st->time_slot = p[2];
	st->carrier = p[3];
	st->b5 = p[5];
	memcpy(st->rssi, &p[6], DECT_DIAG_CARRIERS);
	if (n < 0x26u)
		return 0;
	st->has_counters = 1;
	for (i = 0; i < DECT_DIAG_EVENTS; i++)
		st->events[i] = le16(&p[event_off[i]]);
	st->interval_frames = le16(&p[0x1Au]);
	st->frames = le16(&p[0x24u]);
	st->connect_frames = le16(&p[0x22u]);
	st->uptime = st->frames;
	if (n >= 0x28u) {
		st->has_long = 1;
		st->uptime = (uint32_t)st->frames |
			     ((uint32_t)le16(&p[0x26u]) << 16);
	}
	return 0;
}

/*
 * The raw RSSI octet in tenths of a dBm, as FRITZ!OS converts it on the
 * 7520 (clean room A-0121): (raw - 20) * 80/37 - 110 dBm. Raw 0 and 255
 * carry no value: NO_DBM.
 */
#define NO_DBM	(-32768)

static int rssi_dbm10(uint8_t raw)
{
	if (raw == 0u || raw == 0xFFu)
		return NO_DBM;
	/* (raw - 20) * 80/37 dB, in tenths, truncated toward zero */
	return ((int)raw - 20) * 800 / 37 - 1100;
}

/*
 * FRITZ!OS's "Quality %" of a link (clean room A-0121): from the dBm of
 * the call's carrier, discounted by ACRC + XCRC + ZCRC + QBIT1 + QBIT2
 * over CONNECTTIME. -1 when there is nothing to judge (no counters, or
 * no frames this interval).
 */
static int quality(const struct dect_diag_status *st)
{
	int dbm10, q, good;
	unsigned errs, dur, i;

	if (!st->valid || !st->has_counters || st->carrier >= DECT_DIAG_CARRIERS)
		return -1;
	dur = st->connect_frames;
	if (!dur)
		return -1;
	dbm10 = rssi_dbm10(st->rssi[st->carrier]);
	if (dbm10 == NO_DBM)
		q = 100;
	else if (dbm10 > -400) q = 100;
	else if (dbm10 > -450) q = 90;
	else if (dbm10 > -500) q = 80;
	else if (dbm10 > -550) q = 70;
	else if (dbm10 > -600) q = 60;
	else if (dbm10 > -650) q = 50;
	else if (dbm10 > -700) q = 40;
	else if (dbm10 > -750) q = 30;
	else q = 20;
	for (errs = 0, i = 0; i < 5u; i++)	/* ACRC .. QBIT2 */
		errs += st->events[i];
	if (!errs)
		return q;
	good = 100 - (int)(100u * (errs < dur ? errs : dur) / dur);
	if (good < 70)
		return 1;
	if (good < 80)
		return 2;
	if (good < 90)
		return 5;
	/* A-0121 as given, including its step to 0 at exactly 90 % good */
	return ((q * (good - 90) * 10 / 100) + 2) / 5 * 5;
}

uint64_t dect_diag_frames_update(struct dect_diag_frames *f,
				 const struct dect_diag_status *st)
{
	if (!st->has_counters)
		return f->total;
	/*
	 * 16 bits at 100 Hz wraps every 10 min 55 s; a poll interval far
	 * shorter than that makes a modular difference safe.
	 */
	if (!f->have)
		f->total = st->frames;
	else
		f->total += (uint16_t)(st->frames - f->last);
	f->last = st->frames;
	f->have = 1;
	return f->total;
}

const char *dect_diag_event_name(unsigned i)
{
	return i < DECT_DIAG_EVENTS ? event_name[i] : "?";
}

/* A short name for a CC state: "idle", "ringing", "active", ... */
static const char *call_state_name(enum dect_cc_state st)
{
	switch (st) {
	case DECT_CC_ST_NULL:		 return "idle";
	case DECT_CC_ST_PROCEEDING:	 return "proceeding";
	case DECT_CC_ST_ACTIVE:		 return "active";
	case DECT_CC_ST_RELEASED:	 return "released";
	case DECT_CC_ST_CALL_PRESENT:	 return "calling";
	case DECT_CC_ST_RINGING:	 return "ringing";
	case DECT_CC_ST_OVERLAP:	 return "dialling";
	}
	return "unknown";
}

/* Bounded appends; *ok drops to 0 on the first overflow. */
static void put(char *buf, size_t cap, size_t *off, int *ok,
		const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!*ok)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *off, cap - *off, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= cap - *off) {
		*ok = 0;
		return;
	}
	*off += (size_t)n;
}

static void put_hex(char *buf, size_t cap, size_t *off, int *ok,
		    const uint8_t *p, size_t n)
{
	size_t i;

	put(buf, cap, off, ok, "\"");
	for (i = 0; i < n; i++)
		put(buf, cap, off, ok, "%02x", p[i]);
	put(buf, cap, off, ok, "\"");
}

size_t dect_diag_json(const struct dect_diag_link *d, int with_identity,
		      char *buf, size_t cap)
{
	size_t off = 0;
	int ok = cap > 0;
	unsigned i;

	if (!buf)
		return 0;
	put(buf, cap, &off, &ok, "{\"handle\":%u,\"handset\":%d,\"slot\":%u",
	    d->handle, d->handset_tag, d->slot);
	if (with_identity && d->identity_len) {
		put(buf, cap, &off, &ok, ",\"ipui\":");
		put_hex(buf, cap, &off, &ok, d->identity, d->identity_len);
	}
	put(buf, cap, &off, &ok, ",\"tpui\":");
	put_hex(buf, cap, &off, &ok, d->tpui, sizeof(d->tpui));
	put(buf, cap, &off, &ok,
	    ",\"located\":%s,\"tpui_confirmed\":%s,\"registered\":%s",
	    d->located ? "true" : "false",
	    d->tpui_confirmed ? "true" : "false",
	    d->registered ? "true" : "false");
	if (d->model_len) {
		put(buf, cap, &off, &ok, ",\"model\":");
		put_hex(buf, cap, &off, &ok, d->model, d->model_len);
	}
	if (d->codecs_len) {
		put(buf, cap, &off, &ok, ",\"codecs\":");
		put_hex(buf, cap, &off, &ok, d->codecs, d->codecs_len);
	}
	put(buf, cap, &off, &ok, ",\"call\":\"%s\",\"incoming\":%s",
	    call_state_name(d->call_state),
	    d->call_incoming ? "true" : "false");
	if (d->basic_service != DECT_CC_SVC_NONE)
		put(buf, cap, &off, &ok, ",\"basic_service\":%u",
		    d->basic_service);
	if (d->chosen_codec)
		put(buf, cap, &off, &ok, ",\"codec\":%u", d->chosen_codec);
	put(buf, cap, &off, &ok, ",\"ciphered\":%s",
	    d->ciphered ? "true" : "false");
	if (d->radio.valid) {
		put(buf, cap, &off, &ok,
		    ",\"radio\":{\"slot\":%u,\"carrier\":%u,\"rssi\":[",
		    d->radio.time_slot, d->radio.carrier);
		for (i = 0; i < DECT_DIAG_CARRIERS; i++)
			put(buf, cap, &off, &ok, "%s%u", i ? "," : "",
			    d->radio.rssi[i]);
		put(buf, cap, &off, &ok, "]");
		if (d->radio.has_counters) {
			put(buf, cap, &off, &ok, ",\"events\":{");
			for (i = 0; i < DECT_DIAG_EVENTS; i++)
				put(buf, cap, &off, &ok, "%s\"%s\":%u",
				    i ? "," : "", event_name[i],
				    d->radio.events[i]);
			put(buf, cap, &off, &ok, "},\"call_frames\":%llu",
			    (unsigned long long)d->call_frames);
			int q = quality(&d->radio);

			if (q >= 0)
				put(buf, cap, &off, &ok, ",\"quality\":%d", q);
		}
		if (d->radio.carrier < DECT_DIAG_CARRIERS &&
		    rssi_dbm10(d->radio.rssi[d->radio.carrier]) != NO_DBM) {
			int t = rssi_dbm10(d->radio.rssi[d->radio.carrier]);

			put(buf, cap, &off, &ok, ",\"dbm\":%s%d.%d",
			    t < 0 ? "-" : "", (t < 0 ? -t : t) / 10,
			    (t < 0 ? -t : t) % 10);
		}
		put(buf, cap, &off, &ok, "}");
	}
	put(buf, cap, &off, &ok, "}");
	if (!ok) {
		buf[0] = '\0';
		return 0;
	}
	return off;
}

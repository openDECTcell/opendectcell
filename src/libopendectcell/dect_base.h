/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base station above the links: what dect-stubd holds today and the
 * library takes over stage by stage (docs/stack-in-library.md), so the box
 * and the controller run one implementation.
 *
 * Stage S1a: a handset's record, the part of a subscription that outlives a
 * connection and a restart. The host stores it (dect-stubd in its -s files,
 * the controller in its database); the stack reads and changes it.
 *
 * No I/O and no allocation.
 */
#ifndef OPENDECTCELL_DECT_BASE_H
#define OPENDECTCELL_DECT_BASE_H

#include <stddef.h>
#include <stdint.h>

#include "dect_callinfo.h"
#include "dect_cc.h"
#include "dect_clip.h"
#include "dect_ctl.h"
#include "dect_link.h"
#include "dect_mail.h"
#include "dect_mm.h"
#include "dect_profile.h"
#include "dect_proto.h"

struct dect_hs_rec {
	unsigned slot;				/* chip subscription slot, 1..6 */
	uint8_t  uak[DECT_MM_UAK_LEN];		/* the long-term key */
	int      uak_valid;
	/*
	 * The MM <<PORTABLE-IDENTITY>> content: the identity gate for location
	 * registration and access rights (the chip admits anything locked to
	 * our RFPI, so this is the only gate).
	 */
	uint8_t  port_id[DECT_MM_PORTID_MAX];
	size_t   port_id_len;
	/* The identities an FP-originated {CC-SETUP} carries. */
	uint8_t  cc_port_id[DECT_CC_ID_MAX];
	size_t   cc_port_id_len;
	uint8_t  cc_fixed_id[DECT_CC_ID_MAX];
	size_t   cc_fixed_id_len;
	/*
	 * 1 once the pair came from a call the handset placed. A pair derived
	 * from the subscription is good enough to ring with; a measured one
	 * may overwrite it.
	 */
	int      cc_id_learned;
};

/*
 * Stage S1b: the handsets the base holds. Records live in a table; one of
 * them is current, the one the main link serves, and the stack works on a
 * copy of it (rec) that goes back into the table when another becomes
 * current. The host loads the table from its store at start and saves the
 * current record when the stack says it changed.
 */
#define DECT_BASE_HS_MAX	DECT_MM_MAX_SLOT
#define DECT_BASE_SIDES		3u

/* What the host does for the stack (S1c, S2.1). Any may be NULL. */
struct dect_base_ops {
	/* Store record i, which changed (r is it, the working copy). */
	void (*save)(void *ctx, unsigned i, const struct dect_hs_rec *r);
	/* Hand a mail to the chip: 0, or -1 if it cannot take it now. */
	int  (*send)(void *ctx, const struct dect_mail *m);
	/* n random bytes for keys and challenges: a source fit for that. */
	void (*random)(void *ctx, uint8_t *out, size_t n);
	/* One line for the log, without its newline. */
	void (*log)(void *ctx, const char *line);
	/*
	 * Put the handsets the host serves into the chip's slots (with the
	 * dect_base_slot_*() writers): a record's key changed (S2.3d).
	 */
	void (*install)(void *ctx);
	/* A line for the bridge (docs/bridge-interface.md §2.1) (S3e). */
	void (*event)(void *ctx, struct dect_ctl_out *o);
};

/*
 * Stage S2.2: the chip's subscription slots. Which handsets go into the
 * chip is the host's policy (a cell holds the ones it serves); how they go
 * in is the chip's, here. Mails to the STUB's contact task 0x02
 * [CR A-0026].
 */
struct dect_base_chip {
	uint8_t att_flag;	/* 0x0247 op 1's flag; stock sets bit 0 [CR A-0113] */
	uint8_t att_cfg;	/* 0x020A's byte after the UAK [CR A-0113] */
	int     no_mask;	/* send no 0x017C at all */
};

/*
 * Stage S2.3: a radio connection the stack runs, with the registration's
 * progress on it. The chip names it by a handle.
 */
struct dect_base_link {
	int      active;		/* the connection is up */
	uint8_t  handle;
	/*
	 * The C-plane stack of this connection: segments, Lc, LAPC, NWK
	 * routing, MM and CC (dect_link.h).
	 */
	struct dect_link link;
	int      persisted;		/* the subscription went into the chip */
	enum dect_mm_state reported_state;	/* MM's state last logged */
	int      enrol_routed;		/* this enrolment's record is chosen */
	int      enrol_switched;	/* ... and it moved off record 0 */
	int      adopted;		/* side link: the record it serves, -1 */
	/* Stage S3b, the call on this link: */
	size_t   dial_len;		/* digits seen so far, overlap sending */
	uint64_t dial_change_ms;	/* when the last ones arrived */
	int      ic_tried;		/* checked for an internal call */
	int      cipher_done;		/* the cipher's outcome handled */
	int      auth_refused;		/* authfirst: ended, the key unproven */
};

/*
 * Stage S3c: the external calls the bridge carries
 * (docs/bridge-interface.md §2), one per audio channel of the chip: on the
 * main link (party -1), on a side link (its index), or paging its handset
 * for a side ring (DECT_BASE_PARTY_PAGING).
 */
#define DECT_BASE_CALLS		3u
#define DECT_BASE_PARTY_PAGING	(-3)	/* a side ring paging, no link yet */
#define DECT_BASE_PARTY_NONE	(-4)	/* no link */

struct dect_base_call {
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
	 * index), paged while DECT_BASE_PARTY_PAGING, and the caller it
	 * is shown.
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
 * Stage S3f: ringing a handset. A page is a broadcast the handset may
 * simply miss, and the stock host guards each one with a 3 s timer, so the
 * base pages again until the handset brings up a link or gives up.
 */
enum dect_base_ring_state {
	DECT_BASE_RING_IDLE,
	DECT_BASE_RING_RELEASING,	/* the link is being released first */
	DECT_BASE_RING_PAGING,
	DECT_BASE_RING_WAIT_PAGE_RESP,	/* link up, the page response next */
	DECT_BASE_RING_DONE,
};

struct dect_base_ring {
	enum dect_base_ring_state state;
	uint64_t release_until;		/* give the link time to go down */
	uint64_t wait_since;		/* a link in use held the ring back */
	uint64_t resp_deadline;		/* fall back if no page response comes */
	unsigned setup_tries;		/* refused CC-SETUPs */
	/*
	 * After the SETUP is queued: watch it until the handset alerts. A call
	 * lost to a crossing {CC-SETUP} of the handset's (its list sync) is
	 * rung again once the link is free, a few times at most.
	 */
	int      watch;
	int      now;			/* a call-in: ring at once */
	unsigned hs;			/* the record rung (table index) */
	uint32_t coll0, alert0;		/* CC's counters when queued */
	unsigned requeues;
	unsigned pages;			/* pages sent for this attempt */
	uint64_t last;			/* when the last one went out */
};

/*
 * Stage S3i: how the base runs calls, the host's options (dect-stubd's -a
 * keys of the same names). Set once at start.
 */
struct dect_base_cfg {
	int      narrowband;	/* the speech is narrowband: G.726, no G.722 */
	int      two_calls;	/* up to three calls (twocalls) */
	int      auth_first;	/* a handset proves its key before its call goes anywhere */
	int      locauth;	/* authenticate on location registration */
	int      no_overlap;	/* no overlap sending: the number en bloc */
	unsigned dial_wait_ms;	/* the dial pause that ends a number */
	unsigned alert_ms;	/* ALERTING this long before CONNECT (alertms=) */
	int      bind_first;	/* the audio bound before CONNECT (bindfirst) */
	int      no_call_status;	/* no TS 102 527-3 call status at all */
	int      all_call_status;	/* call status to every handset */
	int      nlr;	/* the base releases the link after a call */
	unsigned setup_tries;	/* refused CC-SETUPs before a ring gives up, 0 = default */
	int      signal_on;	/* <<SIGNAL>> in FP-originated SETUPs */
	uint8_t  signal_value;	/* ... its value */
	int      signal_set;	/* ... given as an option, not the profile's */
	uint8_t  basic_svc;	/* <<BASIC-SERVICE>> of an incoming call */
	int      basic_svc_set;	/* ... given as an option */
	unsigned ring_ms;	/* ring= test timing */
	unsigned ring_attach_ms;	/* ringattach= test timing */
	unsigned time_every_min;	/* the periodic date and time */
	/* callinfo keys: a fixed <<CALL-INFORMATION>> for every SETUP (S3k) */
	struct dect_callinfo callinfo;
};

/* What a handset said of itself in <<TERMINAL-CAPABILITY>> (S3k). */
struct dect_base_cap {
	int known;			/* seen (cap_tick) */
	int part3;			/* NG-DECT Part 3: call status allowed */
	int ng;				/* NG-DECT Part 1 or 3: not pure GAP */
};

struct dect_base {
	const struct dect_base_ops *ops;	/* NULL: nothing is stored */
	void *ctx;
	struct dect_base_chip chip;
	struct dect_base_link main_link;	/* calls, enrolment */
	/*
	 * Side links: connections while the main link is busy, for location
	 * registration, list access and a second call (S2.4, S3e).
	 */
	struct dect_base_link side[DECT_BASE_SIDES];
	/* subslot=: record 0's chip slot instead of its own, 0 = none */
	unsigned slot0;
	struct dect_base_call calls[DECT_BASE_CALLS];
	struct dect_base_ring ring;
	struct dect_base_cfg cfg;
	struct dect_hs_rec hs[DECT_BASE_HS_MAX];
	/*
	 * Each record's handset profile (docs/handset-profiles.md), resolved
	 * by the host from the model it was given (S3h): defaults otherwise.
	 */
	struct dect_hs_profile prof[DECT_BASE_HS_MAX];
	struct dect_base_cap cap[DECT_BASE_HS_MAX];	/* runtime, per record */
	unsigned n_hs;			/* records in use, hs[0] first */
	unsigned cur;			/* the current one */
	struct dect_hs_rec rec;		/* hs[cur], being worked on */
};

void dect_base_init(struct dect_base *b);

/* hs[cur] = rec: the working copy back into the table. */
void dect_base_stash(struct dect_base *b);

/* rec = hs[i], without stashing first (loading the table at start). */
void dect_base_load(struct dect_base *b, unsigned i);

/*
 * Make record i the current one: the working copy goes back first.
 * 1 if the current record changed, 0 if i already was current or is out
 * of range.
 */
int dect_base_switch(struct dect_base *b, unsigned i);

/* The current record changed: the host's save(). */
void dect_base_changed(struct dect_base *b);

/* The host's send(); -1 without one. */
int dect_base_send(struct dect_base *b, const struct dect_mail *m);

/*
 * The host's random(). Without one the bytes are zero: a host that keys
 * handsets must set it.
 */
void dect_base_random(struct dect_base *b, uint8_t *out, size_t n);

/*
 * Clear every chip slot 1..6 whose bit (1 << slot) is not in keep_mask:
 * 0x0247 {slot, op 0, 0}. The chip keeps its slots across runs and would
 * otherwise still admit a handset the host holds no record of.
 */
void dect_base_slot_clear(struct dect_base *b, unsigned keep_mask);

/*
 * Put a handset's key into chip slot `slot`: 0x0247 {slot, op 1, flag},
 * then 0x020A {slot, UAK, config} [CR A-0026]. 0, or -1 for a slot outside
 * 1..6 (the 0x020A writer does not range-check; only 0x0247 goes out).
 */
int dect_base_slot_install(struct dect_base *b, unsigned slot,
			   const uint8_t uak[DECT_MM_UAK_LEN]);

/* 0x017C {00, mask}: the handsets locked in, bit n-1 for slot n [CR A-0129]. */
void dect_base_slot_mask(struct dect_base *b, uint8_t mask);

/*
 * A connection came up on `handle` for link bl: its C-plane stack starts
 * afresh with cfg (cfg->sub_slot is the current record's chip slot), the
 * current record's identity is the one MM expects, and every record in
 * hold_mask (bit i for hs[i]; the host's choice, the current one aside)
 * is held on the link too, so MM adopts whichever handset this is.
 */
void dect_base_link_open(struct dect_base *b, struct dect_base_link *bl,
			 uint8_t handle, const struct dect_mm_cfg *cfg,
			 unsigned hold_mask);

/*
 * Record i's chip slot: its own, or slot0 for record 0; the current one
 * falls back to DECT_MM_REG_SLOT when its slot is not 1..6.
 */
unsigned dect_base_slot(const struct dect_base *b, unsigned i);

/*
 * A side link: a connection while the main link is busy, for location
 * registration, list access and a second call (S2.4). Its stack starts
 * with cfg; every record in hold_mask (the current one included) is held
 * in table order, so MM adopts whichever handset this is, without the
 * current record changing.
 */
void dect_base_side_open(struct dect_base *b, struct dect_base_link *bl,
			 uint8_t handle, const struct dect_mm_cfg *cfg,
			 unsigned hold_mask);

/*
 * A C-plane segment (0x0203) on link bl: the challenges MM may need drawn
 * from the host's random(), then dect_link_rx_segment() (its result, the
 * indication in *ind).
 */
int dect_base_link_segment(struct dect_base *b, struct dect_base_link *bl,
			   const struct dect_mail *m, uint64_t now,
			   struct dect_link_ind *ind);

/*
 * A C-plane segment (0x0203) on the main link: the challenges MM may need
 * drawn from the host's random(), the segment through the link
 * (dect_link_rx_segment(), its indication in *ind), and when MM adopted a
 * held record other than the current one, that one becomes current
 * (*switched = 1). dect_link_rx_segment()'s result.
 */
int dect_base_main_segment(struct dect_base *b, const struct dect_mail *m,
			   uint64_t now, struct dect_link_ind *ind,
			   int *switched);

/*
 * After a segment on the main link: if the handset registered on it and
 * has now located (its subscription is complete), the current record
 * takes its new key and its identity (the gate for every later link), the
 * host installs and saves it, and the link counts as persisted. 1 when
 * that happened now, else 0.
 */
int dect_base_main_registered(struct dect_base *b);

/*
 * Stage S3a, a call's ciphering. Record i's key (the working copy for the
 * current one), or NULL if it has none.
 */
const uint8_t *dect_base_uak(const struct dect_base *b, unsigned i);

/*
 * Challenge the handset on link bl with key uak, so both sides derive the
 * call's cipher key (DCK); the challenges come from the host's random().
 */
void dect_base_cipher_start(struct dect_base *b, struct dect_base_link *bl,
			    const uint8_t uak[DECT_MM_UAK_LEN]);

/*
 * The call key to the chip before the over-air {CIPHER-REQUEST}, as stock
 * does [CR A-0126]: 0x0208 {link, DCK[8] in its own order, 00 x15, FF FF,
 * 00 00}. Without it the chip switches to ciphering with no key and drops
 * the link. 0, or -1 when MM holds no DCK. The mail carries a key: the host
 * must not log its parameters.
 */
int dect_base_chip_key(struct dect_base *b, const struct dect_base_link *bl);

/*
 * The call table (S3d). party -1 is the main link, a side link's index,
 * or DECT_BASE_PARTY_PAGING.
 */
struct dect_base_call *dect_base_call_by_id(struct dect_base *b,
					    unsigned id);
/* The call on link `party`, or NULL. */
struct dect_base_call *dect_base_call_on(struct dect_base *b, int party);
/* A free entry, zeroed; it is taken once its id is set. NULL: all taken. */
struct dect_base_call *dect_base_call_new(struct dect_base *b);
/* How many calls link `party` carries. */
unsigned dect_base_calls_on(const struct dect_base *b, int party);
/* The call link `party`'s lane carries (in CS call connect), or NULL. */
struct dect_base_call *dect_base_call_active(struct dect_base *b, int party);
/* Link `party`'s call the handset knows by call id `id`, or NULL. */
struct dect_base_call *dect_base_call_by_dect(struct dect_base *b, int party,
					      uint8_t id);

/*
 * The chip slot of the handset link `party` serves (S3e): the main link's
 * adopted record or the current one, a side link's adopted record; 0 for
 * nobody yet.
 */
unsigned dect_base_party_slot(const struct dect_base *b, int party);

/* The same for a call; a side ring's paging target counts. */
unsigned dect_base_call_slot(const struct dect_base *b,
			     const struct dect_base_call *c);

/*
 * An event about call c to the bridge through the host's event(): `verb
 * call=ID hs=SLOT [k2=v2]`, hs left out for nobody yet.
 */
void dect_base_call_event(struct dect_base *b, const struct dect_base_call *c,
			  const char *verb, const char *k2, const char *v2);

/*
 * Record i's TPUI by MM's own rule for its chip slot (S3g). Not a link's
 * MM: that is whichever handset the link last served (run 32, 2026-10-25:
 * a page for the C430A went out with the A540's TPUI).
 */
void dect_base_tpui(const struct dect_base *b, unsigned i, uint8_t tpui[3]);

/*
 * Page a handset: 0x0300 {info, the TPUI's low 16 bits}. info is the page
 * information byte the host chose for the call (its basic service).
 */
void dect_base_page(struct dect_base *b, uint8_t info, const uint8_t tpui[3]);

/*
 * The audio binds (S3j), the chip's mails for a call's speech path; the
 * values (selector, gain, the TDM mask, mode and codec bytes) are the
 * host's, which knows its bus [CR A-CodecModes-1].
 */
/* 0x0251 SET_CHANNEL_PARAM {link, selector, gain}. */
void dect_base_audio_param(struct dect_base *b, uint8_t handle, uint8_t sel,
			   uint8_t gain);
/* 0x0230 ASSIGN_AUDIO_CHANNEL {link, flags, mask (LE), mode, codec}. */
void dect_base_audio_assign(struct dect_base *b, uint8_t handle,
			    uint8_t flags, uint16_t mask, uint8_t mode,
			    uint8_t codec);
/* 0x022D AUDIO_PATH_ON_OFF {link, 00}: connect. */
void dect_base_audio_path(struct dect_base *b, uint8_t handle);
/* 0x0233 {link, 01}: the audio channel released [CR A-0127]. */
void dect_base_audio_release(struct dect_base *b, uint8_t handle);

/* A log line, printf-style, cut at 255 octets; nothing without log(). */
void dect_base_log(struct dect_base *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#endif

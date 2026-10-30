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
/*
 * The base's one line, as the line list (TS 102 527-3 7.4.11) names it; a
 * <<CALL-INFORMATION>> line identifier must name the same line (bench 21c:
 * line 0 against the list's line 1, and the A540 rang silently).
 */
#define DECT_BASE_LINE_ID	1u
/* The page information byte: a CC page, and a wideband SETUP to follow. */
#define DECT_BASE_PAGE_CC	0x0Cu
#define DECT_BASE_PAGE_WIDEBAND	0x20u
#define DECT_BASE_SIDES		3u

struct dect_base_link;

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
	/*
	 * C-plane frames are queued on bl's link: hand the chip what it can
	 * take now (S3n).
	 */
	void (*pump)(void *ctx, struct dect_base_link *bl);
	/*
	 * Audio channels a new call on link p (-1 the main link) can still
	 * count on (S3q): the host's lanes. NULL: as many as needed.
	 */
	int  (*lanes)(void *ctx, int p);
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
	/* Stage S3p, the main link's call: */
	int      bound;			/* its audio channel bound */
	int      path_on;		/* ... and connected (0x022D sent) */
	int      connect_staged;	/* alertms/bindfirst: CONNECT due */
	uint64_t connect_at;		/* ... not before this */
	int      cs_connect_sent;	/* "CS call connect" after CONNECT (Part 3) */
	int      info_sent;		/* the name or infoie= went out on ALERTING */
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
	int      no_cipher;	/* -C, a lab option: no ciphering, no proof */
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
	/* infoie=: IEs in a {CC-INFO} on ALERTING, a lab option (S3r) */
	uint8_t  info_ie[64];
	size_t   info_ie_len;
};

/* What a handset said of itself in <<TERMINAL-CAPABILITY>> (S3k). */
struct dect_base_cap {
	int known;			/* seen (cap_tick) */
	int part3;			/* NG-DECT Part 3: call status allowed */
	int ng;				/* NG-DECT Part 1 or 3: not pure GAP */
};

/*
 * Stage S3q: an internal call (GAP N.31), a handset's SETUP with call
 * class 9, the target's slot in its digits. The base pages the target,
 * calls it on the link it answers on, and the host bridges the two
 * channels (clean room A-0124: stock does the same). Needs cfg.two_calls.
 */
enum dect_base_ic_state {
	DECT_BASE_IC_IDLE,
	DECT_BASE_IC_PAGING,
	DECT_BASE_IC_SETUP,
	DECT_BASE_IC_RINGING,
	DECT_BASE_IC_UP,
};

#define DECT_BASE_IC_NONE	(-2)	/* a party with no link yet */
#define DECT_BASE_IC_PAGES	20u
#define DECT_BASE_IC_PAGE_MS	3000u
#define DECT_BASE_IC_SVC	0x98u	/* internal call, wideband speech */

struct dect_base_ic {
	enum dect_base_ic_state st;
	int      caller;	/* -1 the main link, else a side index */
	int      target;	/* the record called */
	int      tside;		/* the target's link: as caller, or IC_NONE */
	uint8_t  tpui[3];
	unsigned pages;
	uint64_t page_at;
	int      name_info;	/* the caller's name follows in {CC-INFO} */
	struct dect_clip clip;	/* the caller, kept for re-presentation */
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
	/*
	 * The bridge (docs/bridge-interface.md): a client takes the calls out,
	 * and the ids it knows them by (S3o).
	 */
	int      bridge;		/* a bridge client is connected */
	unsigned next_call;		/* the last call id given, 1..127 */
	/* record i has a Line 2, an account of its own (lines-and-calls.md) */
	int      line2[DECT_BASE_HS_MAX];
	uint64_t start_ms;		/* the host's start, for ring= */
	uint64_t first_contact_ms;	/* a handset's first C-plane segment, for ringattach= */
	struct dect_base_cfg cfg;
	/* the caller's identity for the next FP-originated SETUP (S3m) */
	struct dect_clip clip;
	struct dect_base_ic ic;
	struct dect_hs_rec hs[DECT_BASE_HS_MAX];
	/* record i holds a subscription (the host's store had one) */
	int      loaded[DECT_BASE_HS_MAX];
	/*
	 * The name a handset gave record i in the Internal Names List (TS 102
	 * 527-3 §7.4.10.5.8); empty = the default "DECT n".
	 */
	char     name[DECT_BASE_HS_MAX][DECT_IWU_NAME_MAX];
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

/*
 * The ring's SETUP (S3l). A call id 1..127 no other call uses, for a call
 * on link `me`; 0 when the call information option fixes it.
 */
uint8_t dect_base_call_id(const struct dect_base *b,
			  const struct dect_link *me);
/* The handset rung gets stock's full call information: Part 3, known. */
int dect_base_ring_part3(const struct dect_base *b);
/*
 * The main link's <<CALL-INFORMATION>> for its next SETUP: on, the external
 * call's for a Part 3 handset (our line, VoIP, call id, CS call setup
 * [CR A-0125]); off or not Part 3, the fixed option.
 */
void dect_base_ring_callinfo(struct dect_base *b, int on);
/*
 * The <<BASIC-SERVICE>> of a SETUP to record i: the profile's, a pure GAP
 * handset's (0x80: EN 300 444 table 21), else `configured`.
 */
uint8_t dect_base_basic_svc(const struct dect_base *b, int i,
			    uint8_t configured);
/* ... of the ring's SETUP to record t; narrow drops the wideband bit. */
uint8_t dect_base_ring_svc(const struct dect_base *b, unsigned t, int narrow);
/* The main link's bridged call is narrowband. */
int dect_base_ring_narrow(const struct dect_base *b);
/* The page information byte for a call (narrow: no wideband SETUP). */
uint8_t dect_base_page_info(const struct dect_base *b, int narrow);

/*
 * The ring's {CC-SETUP} on the main link, once the handset rung answered
 * its page (S3m): its call id, the call information, the signal and the
 * name as its profile wants them, the identities from its record. A
 * SETUP the link refuses (a call of the handset's in progress) is tried
 * again every DECT_BASE_SETUP_RETRY_MS, cfg.setup_tries times (default
 * DECT_BASE_SETUP_TRIES). The ring's state says what happened.
 */
#define DECT_BASE_SETUP_RETRY_MS	2000u
#define DECT_BASE_SETUP_TRIES		8u
void dect_base_ring_setup(struct dect_base *b, uint64_t now, const char *why);

/*
 * The ring's sequencing (S3n). Release a link that is up first, page
 * every DECT_BASE_PAGE_INTERVAL_MS until the handset brings up a link
 * (DECT_BASE_PAGE_RETRIES at most), then the SETUP once it answered the
 * page or DECT_BASE_PAGE_RESP_MS passed. A ring lost to a crossing
 * {CC-SETUP} of the handset's starts again, DECT_BASE_REQUEUE_MAX times.
 */
/*
 * Pages before giving up: enough to cover a handset still re-synchronising
 * after the base was off the air (it shows "Synchronisiere"), which can
 * take over half a minute. An unanswered page costs nothing.
 */
#define DECT_BASE_PAGE_RETRIES		20u	/* pages before giving up (60 s) */
#define DECT_BASE_PAGE_INTERVAL_MS	3000u	/* <LCE.03> (EN 300 175-5 A.6) */
#define DECT_BASE_PAGE_RESP_MS		1500u	/* wait this long for the page response */
#define DECT_BASE_REQUEUE_MAX		2u	/* rings again after a lost collision */
#define DECT_BASE_RELEASE_WAIT_MS	3000u	/* [CR A-0053] DLC arms a ~5 s guard */
#define DECT_BASE_IN_USE_WAIT_MS	30000u	/* a list session or MM, at most */
/*
 * What a link is doing, for the ring: 0 nothing the NWK layer holds, 1 a
 * list-access session or an MM procedure (wait for it), 2 a voice call
 * (the handset is busy).
 */
int dect_base_link_use(const struct dect_link *l);
/* The ring target's fixed identity length: 0, it has none to ring with. */
size_t dect_base_ring_fixed_len(const struct dect_base *b);
/* Page the handset rung, by its TPUI, with the call's page byte. */
void dect_base_ring_page(struct dect_base *b);
/* After the SETUP: ring again if a crossing call of the handset's took it. */
void dect_base_ring_watch(struct dect_base *b);
/* The ring, driven by time; the host calls it while the chip is up. */
void dect_base_ring_tick(struct dect_base *b, uint64_t now);

/*
 * The calls out (S3o). Link p: -1 the main link, else side link p.
 * dect_base_drain() sends what p's stack queued.
 */
struct dect_base_link *dect_base_party_link(struct dect_base *b, int p);
void dect_base_drain(struct dect_base *b, int p, uint64_t now);
/* A call id for the bridge, 1..127, that no call holds. */
unsigned dect_base_call_new_id(struct dect_base *b);
/* The handset on link p has a Line 2. */
int dect_base_party_line2(const struct dect_base *b, int p);
/*
 * The handset on link p dialled a number: hand it to the bridge as
 * `call-out` (bridge-interface.md §2.1), the call proceeding. 0: no bridge
 * client, or CC refused; 1: done (busy if the call table is full).
 */
int dect_base_call_out(struct dect_base *b, int p, uint64_t now);

/*
 * A handset's call and its number (S3p). Authenticate first (security
 * audit 2026-10-29, A1): with cfg.auth_first, a call goes nowhere until
 * the handset on its link proved the key we hold for the IPUI it named.
 */
int dect_base_call_auth_ok(const struct dect_base *b,
			   const struct dect_link *l);
/*
 * A handset's call whose number came complete and waits in PROCEEDING to
 * be routed: an internal call, a number picked from a list (1C15H, TS 102
 * 527-3 7.4.10.6.2), or one sent with <<SENDING-COMPLETE>>.
 */
int dect_base_number_complete(const struct dect_cc *cc);
/*
 * Link p's dialling, once a tick: 1 the number is complete, 2 overlap
 * digits were quiet for cfg.dial_wait_ms (a dial plan's stand-in), 0 not
 * yet. Either way the host routes the call (and sets ic_tried).
 */
int dect_base_dial_tick(struct dect_base *b, int p, uint64_t now);
/*
 * The main link's call, routed nowhere else: the base connects it,
 * through the routed steps when cfg.alert_ms or cfg.bind_first ask for
 * them (dect_base_connect_tick() finishes).
 */
void dect_base_dial_connect(struct dect_base *b, uint64_t now);
/* alertms / bindfirst: the CONNECT once its conditions hold. */
void dect_base_connect_tick(struct dect_base *b, uint64_t now);
/*
 * Stock's next step for a Part 3 handset [CR A-0125]: once our call is
 * answered, a {CC-INFO} with the call id and "CS call connect".
 */
void dect_base_cs_connect_tick(struct dect_base *b, uint64_t now);

/* Record i's name: its own, or "DECT n" (S3q). */
void dect_base_hs_name(const struct dect_base *b, unsigned i, char *buf,
		       size_t cap);
/* The CC identities of record i, current or stashed. */
void dect_base_cc_ids(const struct dect_base *b, unsigned i,
		      const uint8_t **port, size_t *plen,
		      const uint8_t **fixed, size_t *flen);
/* Link p is up. */
int dect_base_party_active(const struct dect_base *b, int p);
/*
 * The handset on link p finished dialling: an internal call if its basic
 * service says so (0: not one). Refused busy, "service not implemented"
 * or "user unknown" as the case is, else proceeding and the target paged.
 */
int dect_base_ic_start(struct dect_base *b, int p, uint64_t now);
/*
 * Our SETUP to the target on link p, once it answered the page or came
 * up adopted: the caller's number and name, the internal signal, the
 * call id both handsets share.
 */
void dect_base_ic_setup(struct dect_base *b, int p, uint64_t now,
			const char *how);
void dect_base_ic_end(struct dect_base *b, const char *why);
/* The internal call's progress, once a tick. */
void dect_base_ic_tick(struct dect_base *b, uint64_t now);

/*
 * Calls in (S3r). A call is over for the bridge: on the main link its ring
 * stops too, whatever state it was in.
 */
void dect_base_call_clear(struct dect_base *b, struct dect_base_call *c);
/* The main link can ring: no call, no ring of its own. */
int dect_base_main_free(struct dect_base *b);
/* Ring record t on the main link for call c; dect_base_ring_tick() pages. */
void dect_base_ring_main(struct dect_base *b, struct dect_base_call *c,
			 unsigned t);
/*
 * A side ring: a call-in for a handset while the main link is in another
 * call or ringing. The handset is paged as an internal call's target is
 * and rung on the side link it comes up on, with what the main link's
 * ring gives it: its profile's alerting pattern and name method, and to a
 * Part 3 handset the line, call id and CS call setup [CR A-0125].
 */
void dect_base_side_ring_start(struct dect_base *b, struct dect_base_call *c,
			       unsigned t, uint64_t now);
void dect_base_side_ring_setup(struct dect_base *b, struct dect_base_call *c,
			       unsigned k, uint64_t now, const char *how);
void dect_base_side_ring_tick(struct dect_base *b, struct dect_base_call *c,
			      uint64_t now);
/* The side ring's call on its link: display text, CS call connect. */
void dect_base_side_ring_call_tick(struct dect_base *b,
				   struct dect_base_call *c, uint64_t now);
/*
 * The caller's name as the profile's display text in {CC-INFO}, for a
 * profile whose name method is 1 or 2: 1 queued, 0 not this profile's way
 * (or infoie= set), -1 it could not be queued.
 */
int dect_base_name_info(struct dect_base *b, struct dect_cc *cc, unsigned hs,
			const struct dect_clip *clip);
/* The main link's call rings: the name, or infoie=, once. */
void dect_base_info_tick(struct dect_base *b, uint64_t now);

/*
 * Parallel calls on one link, for a Part 3 handset (TS 102 527-3 §7.4.3),
 * S3s. Call c's new status (§7.4.6.4) to its handset in {CC-INFO}.
 */
void dect_base_par_status(struct dect_base *b, struct dect_base_call *c,
			  uint8_t cs, uint64_t now);
/* Call c becomes the active one; verb, if any, tells the bridge. */
void dect_base_par_connect(struct dect_base *b, struct dect_base_call *c,
			   const char *verb, uint64_t now);
/* The record link p serves, or -1. */
int dect_base_party_index(const struct dect_base *b, int p);
/*
 * The link a call to record t can wait on: its one call bridged and
 * connected, to a handset that takes call status (Part 3).
 * DECT_BASE_PARTY_NONE if there is none.
 */
int dect_base_waiting_party(struct dect_base *b, unsigned t);
/* Call waiting (§7.4.3.5.2) for call-in c on link p. 0, or -1. */
int dect_base_call_waiting(struct dect_base *b, struct dect_base_call *c,
			   int p, const struct dect_clip *clip, uint64_t now);
/*
 * Link p's control codes (table 17) and keypad: the number of a parallel
 * call being dialled (complete after the dial pause), else DTMF for the
 * active call.
 */
void dect_base_par_tick(struct dect_base *b, int p, uint64_t now);
/* Record t is in a call, or being rung for one. */
int dect_base_hs_in_call(const struct dect_base *b, unsigned t);

/*
 * The bridged calls' progress, once a tick (S3t): parallel calls' control
 * codes, side rings, and the events ringing, answered and released.
 */
void dect_base_bridge_tick(struct dect_base *b, uint64_t now);

/* A log line, printf-style, cut at 255 octets; nothing without log(). */
void dect_base_log(struct dect_base *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#endif

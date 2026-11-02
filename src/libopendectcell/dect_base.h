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
#include "dect_calls.h"
#include "dect_cc.h"
#include "dect_clip.h"
#include "dect_contacts.h"
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
	/*
	 * 1 + the index of the registration PIN the handset proved when this
	 * record was created by a registration in this run (cfg.reg_pins, or 1
	 * for cfg.pin); 0 for a record that was stored or loaded. The host maps
	 * it to the user whose window issued that PIN. API 139.
	 */
	unsigned reg_pin1;
	/*
	 * The TPUI this handset holds: {byte 0, 0x11, byte 2}, all zero = derive
	 * it from the slot (and cfg.tpui_offset). A host that keeps a pool sets
	 * it on a record before the handset locates (a free record taken by an
	 * enrolment too); a registration records the TPUI it assigned here.
	 * A handset takes a new TPUI only at its next location registration, so
	 * do not change one that is in use (bench 2026-10-31, T-C). API 141.
	 */
	uint8_t  tpui[3];
};

/*
 * Stage S1b: the handsets the base holds. Records live in a table; one of
 * them is current, the one the main link serves, and the stack works on a
 * copy of it (rec) that goes back into the table when another becomes
 * current. The host loads the table from its store at start and saves the
 * current record when the stack says it changed.
 */
#define DECT_BASE_HS_MAX	DECT_HS_MAX
/* remote directory entries (API 152): numbers DECT_BASE_REMOTE_FIRST..127 */
#define DECT_BASE_REMOTE_MAX	32u
#define DECT_BASE_REMOTE_FIRST	64u
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
	/*
	 * Record i is deregistered (S4b): the host moves its store aside and
	 * tells its clients, before the chip's slot is cleared.
	 */
	void (*forget)(void *ctx, unsigned i, const char *why);
	/* The records in the chip changed: write the locked-in mask (0x017C). */
	void (*mask)(void *ctx);
	/*
	 * The date and time now, with the zone's offset in seconds, when the
	 * host's clock is known to be right (S4c): 1, else 0.
	 */
	int  (*clock)(void *ctx, struct dect_time *dt, long *off_s);
	/* The date and time now, right or not, for a call list entry (S5b). */
	void (*wall)(void *ctx, struct dect_time *dt);
	/*
	 * A handset renamed record i in the Internal Names List (S4d): the
	 * host keeps the name (b->name[i]) and tells its clients.
	 */
	void (*named)(void *ctx, unsigned i);
	/*
	 * May the handset in slot `viewer` see the handset in `slot` in its
	 * Internal Names List (API 138)? 1 yes, 0 no; NULL: always (a
	 * standalone base). A handset always sees itself. The controller
	 * answers by the user's account. After a change of the answer call
	 * dect_base_names_refresh().
	 */
	int  (*name_visible)(void *ctx, unsigned viewer, unsigned slot);
	/*
	 * The stack made record b->cur the current one (S3w): the host serves
	 * its store from now on; why says what made it so (log text).
	 */
	void (*switched)(void *ctx, const char *why);
	/*
	 * Link p (-1 the main link) was set up afresh (S6a): the host's own
	 * per-link state (its NWK log hook, its audio and diagnostics).
	 */
	void (*link_up)(void *ctx, int p);
	/* Side link k goes: the host releases its audio lane first (S6a). */
	void (*side_down)(void *ctx, unsigned k);
	/*
	 * The main link's enrolment completed and the subscription went into
	 * the chip (S6b): the host's follow-up (a profile chosen for the
	 * window, the window closed).
	 */
	void (*registered)(void *ctx, uint64_t now);
	/*
	 * After each main-link segment, before the call's cipher steps (S6b):
	 * the host binds the call's audio path when it is due.
	 */
	void (*call_up)(void *ctx, uint64_t now);
	/*
	 * Side link k's call, before and after each of its segments (S6b):
	 * the host's audio lane and the routing of a dialled number.
	 */
	void (*side_call)(void *ctx, unsigned k, uint64_t now);
	/* Side link k and the main link swapped (S6b): the host's per-link state. */
	void (*swapped)(void *ctx, unsigned k);
	/* Record i's capability changed (S6c): the host keeps it (b->cap[i]). */
	void (*cap_learned)(void *ctx, unsigned i);
	/*
	 * The main link's call is over and its audio channel was bound (S6c,
	 * dect_base_tick()): the host releases its audio path.
	 */
	void (*call_down)(void *ctx);
	/*
	 * An enrolment on the main link was rejected (optional, API 142):
	 * `pin` is 1 when the handset's reply matched no registration PIN (the
	 * number the host counts for a per-cell attempt limit), 0 for another
	 * failure (a malformed request, an identity not accepted); `expected`
	 * is 1 when the handset's profile says this model tries PIN 0000 by
	 * itself first, so the failure is not a guess at the PIN. Once per
	 * rejected attempt; the `reg: registration rejected` log line stays.
	 */
	void (*registration_rejected)(void *ctx, int pin, int expected);
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
	/*
	 * S2.2b: where the loaded STUB build keeps the UAK table in its RAM,
	 * DECT_MM_MAX_SLOT records of DECT_BASE_UAK_STRIDE bytes from slot 1
	 * [CR A-0085]; 0: a build whose table we have no address for, so no
	 * read-back and no audit. The host knows the build, the library not.
	 */
	uint32_t    uak_table;
	int         uak_guess;	/* uak_table is the 08.25 build's, for a build nobody named */
	uint32_t    check_delay_ms;	/* reads wait this long after the writes (06.88) */
	const char *stub;	/* the build's name, for the log; NULL: unknown */
};

/* One UAK table record: 16 key bytes and a flag [CR A-0085]. */
#define DECT_BASE_UAK_STRIDE	0x11u

/*
 * S2.2b: the chip's subscription table read back. The writes have no reply
 * and no error path, so a memory read is the only way to see them land.
 */
#define DECT_BASE_UAK_RETRY_MS	500u	/* ask the read-back again after this long */

struct dect_base_sub_check {
	uint32_t uak_addr;	/* the outstanding UAK read-back, 0 = none */
	uint8_t  uak_slot;
	uint8_t  uak_tries;	/* the read-back was asked this often (max 2) */
	uint64_t uak_asked;	/* when it was last asked, 0 = answered or none */
	uint8_t  pending;	/* audit: reads still out, bit n-1 for slot n */
	uint8_t  occupied;	/* audit: slots that came back holding a key */
	uint8_t  mine;		/* audit: the slot we believe is ours */
	uint8_t  kept;		/* audit: our other records' slots */
	/* chip.check_delay_ms: the reads held back until this time, 0 = none */
	uint64_t at;
	int      want_uak, want_audit, firing;
	uint8_t  want_slot, want_mine, want_kept;
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
	uint64_t reg_done_ms;		/* the key allocation completed (0 = not yet) */
	int      persist_force;		/* persist without a location registration */
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
	int      cipher_started;	/* main link: the call's challenge sent (S3v) */
	int      cipher_reject_seen;	/* ... CIPHER-REJECT reported */
	int      cipher_timeout_seen;	/* ... <MM_cipher.1> reported */
	uint32_t time_sent_for;		/* main link: mm.stats.located answered (S3w) */
	uint32_t located_seen;		/* side link: mm.stats.located answered (S3z) */
	uint32_t rejected_seen;		/* cc.stats.rejected_identity reported (S3z) */
	int      lane;			/* side link: the host's audio lane, -1 none (S6a) */
	/* Stage S3p, the main link's call: */
	int      bound;			/* its audio channel bound */
	int      path_on;		/* ... and connected (0x022D sent) */
	int      connect_staged;	/* alertms/bindfirst: CONNECT due */
	uint64_t connect_at;		/* ... not before this */
	int      cs_connect_sent;	/* "CS call connect" after CONNECT (Part 3) */
	int      info_sent;		/* the name or infoie= went out on ALERTING */
	/* Stage S3u, nlr: the release after a call */
	int      cc_was_busy;		/* the last tick had a call */
	int      nlr_armed;		/* a call ended, release pending */
	int      nlr_lia;		/* ... and it was list access: DISC first */
	uint64_t nlr_disc_at;		/* ... DISC queued at, 0 = not yet */
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
	int      names_readonly; /* no handset may rename an entry (API 138) */
	int      auth_first;	/* a handset proves its key before its call goes anywhere */
	int      no_cipher;	/* -C, a lab option: no ciphering, no proof */
	int      no_chip_key;	/* -k off: the call key withheld from the chip */
	int      verbose;	/* -v: each segment and NWK message in the log */
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
	const char *pin;	/* the base PIN, decimal; NULL: none (S4d) */
	/*
	 * Registration PINs, several at once (API 139): each a decimal string of
	 * 4 to 8 digits, the array owned by the host and valid while the base
	 * runs. A handset proves one of them; the record's reg_pin1 says which.
	 * n_reg_pins 0: `pin` alone, as before. `pin` stays the PIN of the list
	 * access service (deleting a handset) either way.
	 */
	const char *const *reg_pins;
	unsigned n_reg_pins;
	const char *line_name;	/* Line 1's default name, NULL: "Line 1" (S3x) */
	/* codeclist=: a <<CODEC-LIST>> for FP-originated SETUPs (S3z) */
	uint8_t  codeclist[31];
	size_t   codeclist_len;
	unsigned locate_validity;	/* locvalid=N x 40.96 s */
	/* tpuioffset=N: TPUI = {0x00, 0x11, slot + N}, off the slot (T-C, 0 = off) */
	unsigned tpui_offset;
	int      defer_unknown;	/* hold an unknown LOCATE-REQUEST (the host fetches) */
	int      ipuio_set;	/* ipuio=N: assign IPUI O N at registration */
	uint32_t ipuio;
	unsigned park_pli;	/* the PARK length in the access-rights accept, 0: the class's */
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

/*
 * Stage S4a: a record's notifications. Voice messages waiting per line
 * (TS 102 527-3 §7.4.1.2), as the bridge reported its accounts' message
 * summaries: the counts, the lines we have one for, and those the handset
 * has not heard yet. Missed call notifications (§7.4.1.3) the handset has
 * not heard yet, per line, and those for a call just missed (subtype 01H,
 * "a new external missed call just arrived"). Bit n-1: line n.
 */
struct dect_base_notice {
	unsigned mwi[2];
	uint8_t  mwi_known, mwi_due;
	uint8_t  mc_due, mc_new;
};

/*
 * Stage S4b: deregistering a handset (maintainer, 2026-10-28; stock's
 * order from [CR A-0128]): ACCESS-RIGHTS-TERMINATE-REQUEST on its link,
 * paging it first if it has none, then the erasure -- also without an
 * answer, so a lost or broken handset still goes. One at a time; slots
 * asked for meanwhile wait in the queue.
 */
enum dect_base_dereg_state {
	DECT_BASE_DEREG_IDLE,
	DECT_BASE_DEREG_PAGING,
	DECT_BASE_DEREG_REQUESTED,
};

#define DECT_BASE_DEREG_PAGES	10u	/* 3 s apart: 30 s to reach it */
#define DECT_BASE_DEREG_PAGE_MS	3000u

struct dect_base_dereg {
	enum dect_base_dereg_state st;
	int      target;	/* the record */
	uint8_t  tpui[3];
	unsigned pages;
	uint64_t page_at;
	unsigned queue[DECT_BASE_HS_MAX];	/* slots asked meanwhile */
	unsigned nq;
};

/*
 * Stage S4c: the periodic date and time (cfg.time_every_min): one round
 * over all records every so many minutes, one handset at a time -- on its
 * link if it has one, else paged first (TS 102 527-3 §7.4.2.1). The same
 * round visits a handset with no link that has a notice due (page_want).
 */
enum dect_base_time_state {
	DECT_BASE_TIME_IDLE,
	DECT_BASE_TIME_PAGING,
	DECT_BASE_TIME_SENT,
};

#define DECT_BASE_TIME_PAGES	5u	/* 3 s apart, as a ring's */
#define DECT_BASE_TIME_PAGE_MS	3000u
#define DECT_BASE_TIME_SENT_MS	5000u	/* the FACILITY's ack, at most */

struct dect_base_tu {
	enum dect_base_time_state st;
	uint64_t next_round;	/* 0 = not scheduled yet */
	int      in_round;
	unsigned next;		/* the record to visit next */
	int      target;
	uint8_t  tpui[3];
	unsigned pages;
	uint64_t page_at;
	int      answered;	/* it came up for our page: release it */
	uint64_t sent_at;
	int      with_time;	/* this round sends the date and time */
};

struct dect_base {
	const struct dect_base_ops *ops;	/* NULL: nothing is stored */
	void *ctx;
	struct dect_base_chip chip;
	struct dect_base_sub_check check;	/* S2.2b */
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
	/* its lines' names, empty: the default (S3x) */
	char     line_name[DECT_BASE_HS_MAX][2][DECT_IWU_NAME_MAX];
	/* the chip's RFPI, from its contact answer 0xF601 (S3w) */
	uint8_t  rfpi[DECT_MM_RFPI_LEN];
	int      have_rfpi;
	/* identities not learned for the current record, said once (S3w) */
	int      cc_id_foreign_warned, cc_id_unarmed_warned;
	uint64_t start_ms;		/* the host's start, for ring= */
	uint64_t first_contact_ms;	/* a handset's first C-plane segment, for ringattach= */
	struct dect_base_cfg cfg;
	/* the caller's identity for the next FP-originated SETUP (S3m) */
	struct dect_clip clip;
	struct dect_base_ic ic;
	struct dect_base_dereg dereg;
	struct dect_base_tu tu;
	/* wrong PINs entered in list sessions, and the lock they earn (S4d) */
	unsigned pin_wrong;
	uint64_t pin_wrong_since, pin_locked_until;
	struct dect_hs_rec hs[DECT_BASE_HS_MAX];
	/* record i holds a subscription (the host's store had one) */
	int      loaded[DECT_BASE_HS_MAX];
	/* record i's store does not exist: free for enrolment (S5a) */
	int      absent[DECT_BASE_HS_MAX];
	/* record i is a roamer's the host must fetch: never free (S3y) */
	int      remote[DECT_BASE_HS_MAX];
	int      fetched[DECT_BASE_HS_MAX];	/* ... and it has, this run (S3z) */
	uint32_t foreign_conn;	/* connection-ups not allowed to re-arm a busy link */
	uint32_t foreign_seg;	/* segments for another handle than the main link's */
	uint8_t  foreign_seg_handle;	/* ... the last one */
	uint64_t last_seen_ms[DECT_BASE_HS_MAX];	/* record i's last segment */
	/* the registration window (S3z) */
	int      reg_open;
	uint64_t reg_open_until;	/* 0: until closed */
	/* the Contact List the host keeps, NULL: none (S5a) */
	struct dect_contacts *contacts;
	/* the call lists the host keeps, NULL: none (S4a) */
	struct dect_calls *call_lists;
	/* record i's own Contact List (a controller: each handset reads its owner's), NULL: contacts above */
	struct dect_contacts *rec_contacts[DECT_BASE_HS_MAX];
	/* handsets of this account that are on other bases: Internal Names entries only (API 152) */
	uint8_t  remote_id[DECT_BASE_REMOTE_MAX];
	char     remote_name[DECT_BASE_REMOTE_MAX][DECT_IWU_NAME_MAX];
	unsigned n_remote;
	/* what each record is due to hear, and what it heard (S4a) */
	struct dect_base_notice note[DECT_BASE_HS_MAX];
	/* records with a notice due and no link: to page (bit i) */
	uint32_t page_want;
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

/*
 * No link, call, ring, internal call, deregistration or time round is going:
 * nothing refers to a record by its table index.
 */
int dect_base_quiet(const struct dect_base *b);

/*
 * Take record i out of the table (a roamer that stopped renewing, API 146): its
 * subscription, name, profile and notices are gone and the index is free for
 * another record (dect_base_load() and the host's own AddRecord). The record
 * must hold a subscription (not a free store). 0, or -1 when it is not a
 * record, or the base is not quiet (try again later). The current record
 * is moved off first to another one; with none to move to it stays. The host
 * has the record's data in its own store: nothing is saved.
 */
int dect_base_record_drop(struct dect_base *b, unsigned i);

/*
 * Turn a record that holds a subscription in one of the chip's slots (1 to 6)
 * into a free store again, with the same chip slot and `tpui` as the TPUI the
 * next handset enrolling there gets (API 148): the slot is cleared in the chip
 * like a deregistration does, the key and everything else the base kept for the
 * record is wiped, and the mask goes out without it. Nothing is saved or
 * forgotten towards the host: it keeps the handset in its own store (a pooled
 * handset enrols into a chip slot and then leaves it). 0; 1 when the base is
 * not quiet (try again later); -1 when it is not a record with a chip slot.
 */
int dect_base_record_recycle(struct dect_base *b, unsigned i,
			     const uint8_t tpui[3]);

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
 * Read n bytes of chip memory at addr: 0xFFF1 sub 0x07 to the debug task
 * [CR COMMAND_SET 7.1]. Read only; 0xFFF1 sub 0x08 answers.
 */
void dect_base_mem_read(struct dect_base *b, uint32_t addr, uint16_t n);

/*
 * Read back the UAK just written to chip slot `slot` (S2.2b): the reply
 * says whether it landed, the bytes never logged. Held back by
 * chip.check_delay_ms; skipped when chip.uak_table is 0.
 */
void dect_base_uak_verify(struct dect_base *b, uint8_t slot, uint64_t now);

/*
 * Read every chip slot and say which hold a key (S2.2b): `mine` is the
 * slot we believe is ours, `kept` our other records' slots (bit n-1 for
 * slot n); any other slot holding a key is foreign. Held back and skipped
 * as dect_base_uak_verify().
 */
void dect_base_slot_audit(struct dect_base *b, uint8_t mine, uint8_t kept,
			  uint64_t now);

/* The held-back reads, once chip.check_delay_ms is up. */
void dect_base_sub_check_tick(struct dect_base *b, uint64_t now);

/* A 0xFFF1 mail from the chip: a read-back's or the audit's answer. */
void dect_base_mem_reply(struct dect_base *b, const struct dect_mail *m);

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
 * A handset that registers does a location registration on the same link,
 * and the subscription is persisted after it (above). A handset that already
 * counts itself located at this base (a re-registration over its own record)
 * may skip that step, and the new key was then never stored while the handset
 * held it (2026-10-31, C430A on the 7412). Called with the time every tick:
 * DECT_BASE_LOCATE_GRACE_MS after the key allocation completed without a
 * location registration, the subscription is persisted anyway (API 140).
 */
#define DECT_BASE_LOCATE_GRACE_MS	2000u
void dect_base_registration_tick(struct dect_base *b, uint64_t now);

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

/*
 * The bridge (docs/bridge-interface.md §2, step 2 of §5): a client that
 * sent "bridge" carries the external calls, up to DECT_BASE_CALLS at
 * once, on the main link or on side links (struct dect_base_call). Out:
 * a handset's number goes out as call-out, and remote-ringing,
 * remote-answered and remote-released drive ALERTING, CONNECT and the
 * release. In: call-in rings the handset hs= names (the first without
 * it), on the main link or, while that is busy, on a side link of its
 * own; ringing, answered and released come back. Without a bridge nothing
 * changes: a dialled call is connected locally (the lab's test sound or
 * loop-back).
 *
 * Its verbs (§2.2), S5a. What the host answers the client: an error (err
 * set), or ok with key=val (key NULL: none); nothing at all when `sent`,
 * as an event went to the bridge.
 */
struct dect_base_reply {
	const char *err;
	const char *key;
	unsigned    val;
	int         sent;
	/*
	 * rename: the record renamed, -1 none. After the reply the host keeps
	 * the name and tells its clients, as named() does (S5c).
	 */
	int         named;
};

/* The records the base lists to its clients (`hs` lines). */
int dect_base_listed(const struct dect_base *b, unsigned i);
/*
 * The Contact List's name for a number, "First Last" (TS 102 527-3
 * 7.4.10.5.1.3 note 1): 1 found, 0 not.
 */
int dect_base_contact_name(const struct dect_base *b, const char *num,
			   char *buf, size_t cap);
/*
 * call-in, remote-ringing, remote-answered, remote-released, mwi and
 * calllog (S5b), from a client that is (from_bridge) or is not the
 * bridge: 0 not one of these verbs, 1 handled, with what to answer in r.
 */
int dect_base_ctl_call(struct dect_base *b, const struct dect_ctl_msg *m,
		       int from_bridge, uint64_t now,
		       struct dect_base_reply *r);
/*
 * A handset name for the Internal Names List: at most DECT_IWU_NAME_MAX -
 * 1 octets of well-formed UTF-8 (it goes on the air). Empty resets it.
 */
int dect_base_name_ok(const char *nm);
/*
 * The administrator's verbs (S5c): deregister hs= (as SIGUSR2) and rename
 * hs= name= (as a handset's own save). 0 not one of these, 1 handled.
 */
int dect_base_ctl_admin(struct dect_base *b, const struct dect_ctl_msg *m,
			uint64_t now, struct dect_base_reply *r);

/*
 * Notifications (S4a), TS 102 527-3 §7.4.10.2, §7.4.1.2, §7.4.1.3: a CISS
 * {FACILITY} with <<EVENTS-NOTIFICATION>> on a link that is up, to Part 3
 * handsets only (the profile may say otherwise). A handset with no link
 * learns at its next location registration, or when the host pages it
 * (page_want).
 */
int dect_base_hs_of_link(const struct dect_base *b, const struct dect_link *l);
struct dect_link *dect_base_link_known(struct dect_base *b, int i);
int dect_base_notify_ok(const struct dect_base *b, const struct dect_link *l);
/* Could record i hear notifications, from what it told us before? */
int dect_base_notify_known(const struct dect_base *b, unsigned i);
/* After a location registration: the Line Settings and Supported Lists. */
void dect_base_locate_notify(struct dect_base *b, struct dect_link *l);
/* Messages waiting to record i on l, for its due lines; queued count. */
unsigned dect_base_mwi_send(struct dect_base *b, struct dect_link *l,
			    unsigned i);
/* After a location registration: the waiting messages again. */
void dect_base_mwi_locate(struct dect_base *b, struct dect_link *l);
/* Missed calls to record i on l, for its due lines; queued count. */
unsigned dect_base_missed_send(struct dect_base *b, struct dect_link *l,
			       unsigned i);
/* The lines record i has: 1, and 2 if it has a second (a mask). */
uint8_t dect_base_hs_lines(const struct dect_base *b, unsigned i);
/* After a location registration: the missed calls once per line. */
void dect_base_missed_locate(struct dect_base *b, struct dect_link *l);
/* Missed-call notices due to record i: now on its link, else paged. */
void dect_base_missed_due(struct dect_base *b, unsigned i, uint8_t lines,
			  int fresh);
/* The Missed Calls List changed: every record hears the counts. */
void dect_base_missed_changed_tick(struct dect_base *b);
/* The Internal Names List changed: every handset that is up hears it. */
void dect_base_names_notify(struct dect_base *b);

/*
 * Deregistration (S4b). Erase record i from the base, as stock does after
 * the handset's ACCEPT [CR A-0128]: the host forgets it (forget()), the
 * slot's 0x0247 entry cleared, its 0x020A record overwritten with zeros,
 * then 0x017C without its bit (mask(); [CR A-0129]: stock rewrites it on
 * every deregistration). Then the lists lose the entry and say so.
 */
void dect_base_dereg_erase(struct dect_base *b, int i, const char *why);
/* Deregister the handset in slot `slot`, asked by `who`. */
void dect_base_dereg_begin(struct dect_base *b, unsigned slot, uint64_t now,
			   const char *who);
/* The deregistration's progress, and handsets that deregistered themselves. */
void dect_base_dereg_tick(struct dect_base *b, uint64_t now);

/*
 * The date and time, and the notices for handsets with no link (S4c).
 * cfg.time_every_min (-a timeupdate=MIN): the date and time to every
 * handset we hold, every MIN minutes (maintainer, 2026-10-29). One interval
 * for the base, not one per handset: a handset that attached a minute
 * before a round gets it twice, which is harmless.
 *
 * TS 102 527-3 §7.4.2.0 makes it "the responsibility of the entity to
 * ensure that time and date information within the peer entity is up to
 * date", and §7.4.2.1 says how to reach a handset with no link: "the FP
 * shall initiate indirect link establishment" -- a page, as for a call,
 * with the LCE header "000" (no U-plane). That is the C-plane page the
 * deregistration sends. Then the {FACILITY} (dect_link_send_time), and
 * once it is acknowledged the link we brought up is released, as after a
 * call (EN 300 175-5 §14.2.7). A handset that already has a link gets it
 * there and keeps its link.
 *
 * One handset at a time, and only while the base is otherwise idle
 * (dect_base_time_busy()); a handset that does not answer
 * DECT_BASE_TIME_PAGES pages is off or out of range and is skipped until
 * the next round. No trustworthy clock (clock()), no time. The same round
 * visits a handset with no link that has a notice to hear (page_want,
 * §7.4.1.2 wants it sent to every attached handset); outside a time round,
 * only those. The host calls the tick while the radio is on.
 */
int dect_base_time_busy(const struct dect_base *b);
/* Push what link l has queued towards the chip. */
void dect_base_link_flush(struct dect_base *b, struct dect_link *l,
			  uint64_t now);
void dect_base_time_tick(struct dect_base *b, uint64_t now);

/*
 * What the lists need from the base (TS 102 527-3 7.4.10.4.6, 7.4.11.1),
 * once a tick per link that is up (S4d): the name a handset saved, the
 * UAK of the handset the link serves (so MM can answer the handset's
 * authentication of the base, EN 300 444 §8.23), the base PIN to check an
 * entered one against (no PIN after DECT_BASE_PIN_WRONG_MAX wrong ones in
 * the hour), whether the link is ciphered, which handsets are in a call
 * (no deletion then), and the deletion a handset asked for, which is a
 * deregistration.
 */
#define DECT_BASE_PIN_WRONG_MAX		10u
#define DECT_BASE_PIN_WRONG_WINDOW_MS	3600000u	/* the count's window */
#define DECT_BASE_PIN_LOCK_MS		3600000u	/* no PIN check after that */
/* Record i's name into every link's Internal Names List. */
void dect_base_names_spread(struct dect_base *b, unsigned i);
/* Is the handset in this slot in a voice call on any link? */
int dect_base_slot_in_call(const struct dect_base *b, unsigned slot);
void dect_base_lists_tick(struct dect_base *b, uint64_t now);

/*
 * cfg.nlr: once the main link's call or list session has ended and LAPC
 * has nothing outstanding (the last {CC-RELEASE-COM} is acknowledged),
 * release the link: a DISC first if it was list access, then `0x0202
 * <link>` [CR A-0127]. The chip's 0x0201 then runs the usual release
 * path. A new call before that disarms it; a profile with
 * lce.release_after_call = no keeps the link (S3u).
 */
void dect_base_nlr_tick(struct dect_base *b, uint64_t now);

/*
 * A call's ciphering, when and what follows (S3v). The main link:
 * challenge the handset with the stored UAK so both sides derive the DCK;
 * the CIPHER-REQUEST and the chip's key (0x0208) follow once it answers.
 * Once per connection; cfg.no_cipher keeps every call in the clear.
 */
void dect_base_main_cipher_start(struct dect_base *b, uint64_t now);
/*
 * After each main-link segment: authenticate first (cfg.auth_first), a
 * list-access call ciphered at once (TS 102 527-3 C.7.2), the outcome
 * (the chip's key, or the release EN 300 444 §8.23.2.3 asks for), and the
 * cipher's timeout and rejection reported once.
 */
void dect_base_main_cipher_tick(struct dect_base *b, uint64_t now);
/*
 * Side link k's call ciphered as the main link's: a speech call once
 * connected, a list-access call at once. The UAK is the one of the
 * handset the link adopted; a link that adopted no one stays clear.
 */
void dect_base_side_cipher_tick(struct dect_base *b, unsigned k,
				uint64_t now);

/*
 * The main link after a segment (S3w). Make record i the current one and
 * tell the host (switched()); 0 when it already was.
 */
int dect_base_serve(struct dect_base *b, unsigned i, const char *why);
/*
 * The identities an FP-originated {CC-SETUP} needs, for the current
 * record: learned from a call the handset placed (once proven, audit A3;
 * never another handset's), else derived from the subscription and our
 * PARK (EN 300 444 table 21 specifies both). Saved through save().
 */
void dect_base_main_ids(struct dect_base *b, int cc_message);
/*
 * After each location registration on the main link: the date and time
 * when the host's clock is right (TS 102 527-3 §7.4.20.2), the list
 * change notices, the messages waiting and the missed calls.
 */
void dect_base_main_located(struct dect_base *b);

/*
 * Page responses (S3x). The lines a link's Line Settings List offers: those
 * of the handset in `slot`, and the Contact List and call lists the host
 * keeps.
 */
void dect_base_iwu_lines(const struct dect_base *b, struct dect_iwu *iwu,
			 unsigned slot);
/*
 * A link adopted the record in `slot` (a LOCATE-REQUEST or a call of a handset
 * other than the one the link was built for, one fetched after it came up
 * included): its Internal Names List is rebuilt from the table as it is now,
 * the handset is its own terminal, and the lines and the account's filter apply
 * (API 149).
 */
void dect_base_iwu_adopted(const struct dect_base *b, struct dect_iwu *iwu,
			   unsigned slot);
/*
 * The Internal Names Lists after name_visible() changed its answers (API
 * 138): every link's list gains the entries its handset may now see, loses
 * the others, and the handsets are told.
 */
void dect_base_names_refresh(struct dect_base *b);
/*
 * The remote directory (API 152): a handset of the same account that is on
 * another base still belongs in a handset's Internal Names List. The host
 * gives it a number `id` (DECT_BASE_REMOTE_FIRST..127, never a record's slot)
 * and a name; name_visible(viewer, id) filters it like a local entry. An
 * empty or NULL name removes the entry. Returns 0, or -1 for a bad number or
 * a full directory. The links' lists change at once; tell the handsets with
 * dect_base_names_refresh() after the last change. dect_base_remote_name()
 * returns the name for a number, NULL if none.
 */
int dect_base_remote_set(struct dect_base *b, unsigned id, const char *name);
const char *dect_base_remote_name(const struct dect_base *b, unsigned id);
/*
 * A page response on link p (-1 the main link) from record i, which we
 * paged without a call: if it carries that handset's identity, the link
 * adopts it, so dect_base_link_known() finds it. 1 when it was i's.
 */
int dect_base_page_adopt(struct dect_base *b, int p,
			 const struct dect_link_ind *ind, int i,
			 const char *why);
/*
 * The page responses the base waits for besides a ring's, on link p: a
 * deregistration's or the date and time's target (1: consumed), an
 * internal call's target (its SETUP follows), a side ring's handset on a
 * side link (1: its SETUP follows).
 */
int dect_base_dereg_page_response(struct dect_base *b, int p,
				  const struct dect_link_ind *ind);
int dect_base_time_page_response(struct dect_base *b, int p,
				 const struct dect_link_ind *ind);
void dect_base_ic_target_link(struct dect_base *b, int p,
			      const struct dect_link_ind *ind, uint64_t now);
int dect_base_side_ring_answer(struct dect_base *b, unsigned k,
			       const struct dect_link_ind *ind, uint64_t now);

/*
 * The main link after a segment, continued (S3y). The ring target's
 * portable identity for a SETUP, wherever its record is.
 */
const uint8_t *dect_base_ring_port_id(const struct dect_base *b,
				      size_t *len);
/* A record free for a new enrolment, or -1. */
int dect_base_free_record(const struct dect_base *b);
/*
 * A page response on the main link (ind->page_response): a
 * deregistration's or the date and time's target, an internal call's, or
 * the ring's. TS 102 527-1 §7.3.8.2.1: only the IPUI we paged stops the
 * paging; another handset's answer gets {LCE-PAGE-REJECT} with its IPUI
 * and the link is released (§7.3.8.3.1). The ring target's answer brings
 * the SETUP. ind->page_response is cleared once consumed.
 */
void dect_base_main_page_response(struct dect_base *b,
				  struct dect_link_ind *ind, uint64_t now);
/*
 * The registration on the main link: an enrolment routed to the record
 * whose IPUI it names, else to a free one (enrol_switched, undone if the
 * PIN fails), and the outcome, said once.
 */
void dect_base_main_enrolment(struct dect_base *b);

/*
 * Links set up (S3z). The record whose stored IPUI this is, or -1.
 */
int dect_base_hs_by_ipui(const struct dect_base *b, const uint8_t *id,
			 size_t len);
/*
 * Who may enrol on the main link: nobody unless the window is open; then
 * anyone with the PIN when a free record can take a new handset, else
 * only the current record's own handset re-enrolling.
 */
uint8_t dect_base_reg_policy(const struct dect_base *b);
/*
 * Open or close the registration window: the chip's mode (0x022B 'A' /
 * 0x022C 01 [CR COMMAND_SET §6.2]) and MM's policy together. secs = 0
 * keeps it open until closed.
 */
void dect_base_reg_window(struct dect_base *b, int open, unsigned secs,
			  uint64_t now, const char *why);
/* The main link's CC entity, fresh: per connection, and on a promotion. */
void dect_base_main_cc_setup(struct dect_base *b);
/*
 * A connection on `handle` becomes the main link: MM configured for the
 * current record (the PIN, the window's policy, the RFPI), the records it
 * may turn out to be held, the call state reset, CC set up.
 */
void dect_base_main_open(struct dect_base *b, uint8_t handle);
/*
 * A connection on `handle` while the main link is busy gets a side link:
 * location registration and list access, a call only with
 * cfg.two_calls. Its index, the one it already has when the chip repeats
 * connection-up, or -1 when none is free.
 */
int dect_base_side_start(struct dect_base *b, uint8_t handle);
/*
 * Swap side link k with the main link: the ring target answered our page
 * there while the main link carries another handset that is not in a call
 * nor enrolling. Both are C-plane state only, so the swap is a copy. 1
 * when promoted (the SETUP is then queued on the new main link).
 */
int dect_base_side_promote(struct dect_base *b, unsigned k,
			   const struct dect_link_ind *ind, uint64_t now);

/*
 * The chip's mails about connections (S6a). 0x0209 {link}: the chip has
 * switched the link to ciphered -- the cipher-on indication, absent from
 * every unciphered call [CR A-0128] -- which ends <MM_cipher.1>.
 */
void dect_base_cipher_on(struct dect_base *b, uint8_t handle);
/* 0xF601, the contact answer: it carries the base's RFPI. */
void dect_base_contact(struct dect_base *b, const struct dect_mail *m);
/*
 * Connection up on `handle`: a fresh main link, unless the main link is
 * busy (a call in any phase, an enrolment, the ring's page response
 * awaited) -- then a repeat keeps its state, and another handle gets a
 * side link, or is refused if none is free. A side link in a call keeps
 * its handle. Paging is not busy: that is when the handset's connection
 * should come up, and the ring then waits for its page response.
 */
void dect_base_conn_up(struct dect_base *b, uint8_t handle, uint64_t now);
/*
 * Connection down: a side link's, or the main link's own -- the chip also
 * reports the release of an older connection just after a new one is up,
 * and that must not end the live one (vF418; review finding 10). known:
 * the mail named a handle.
 */
void dect_base_conn_down(struct dect_base *b, uint8_t handle, int known);

/*
 * A C-plane segment from the chip (S6b), for the main link or a side
 * link: the stack's work on it, the page responses, identities, the
 * enrolment, notices, the call's cipher; the host's parts through
 * registered(), call_up() and side_call(). Returns 0, or -1 when no link
 * took it.
 */
int dect_base_segment(struct dect_base *b, const struct dect_mail *m,
		      uint64_t now);

/*
 * Once a tick (S6c). What a handset says of itself in
 * <<TERMINAL-CAPABILITY>> on link l, kept for its record once the handset
 * proved its key (audit A3); a profile that does not trust the report
 * sets it instead.
 */
void dect_base_cap_tick(struct dect_base *b, const struct dect_link *l);
/*
 * Said once, loudly, when LAPC gave up after N250 retransmissions and
 * released link l (EN 300 175-4 §9.2.3.6): a false release must show.
 */
void dect_base_lapc_note(struct dect_base *b, struct dect_link *l,
			 const char *which);
/*
 * The main link's number, once complete: an internal call if it names a
 * handset, else to the bridge, else the base's own connect.
 */
void dect_base_main_dial_tick(struct dect_base *b, uint64_t now);
/*
 * Every stack step of a tick, in dect-stubd's order, for a host that runs
 * no steps of its own in between (dect-stubd interleaves its own: the
 * control and audio sockets, diagnostics). radio_on: the chip's session
 * is up (dect-stubd: sess.state UP), for the ring and the date and time.
 */
void dect_base_tick(struct dect_base *b, uint64_t now, int radio_on);

/* A log line, printf-style, cut at 1023 octets; nothing without log(). */
void dect_base_log(struct dect_base *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*
 * Give record i a Contact List of its own: the handset that record serves reads
 * it instead of the base's (dect_base.contacts, which stays for the records that
 * have none). The host owns c, loads it (dect_contacts_load) and takes what the
 * handset changes (dect_contacts_take_change); c must outlive the attachment.
 * c NULL takes it off. A record that is dropped, recycled or wiped lets go of it
 * by itself, so the host can free c after that. A link that is up keeps what it
 * had until it adopts a record again or the handset's next link; the handset is
 * told of a changed list by the list change notice at its next location
 * registration. No effect on a record that does not exist (i >= DECT_BASE_HS_MAX).
 */
void dect_base_record_contacts(struct dect_base *b, unsigned i,
			       struct dect_contacts *c);

#endif

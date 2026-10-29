/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CAT-iq list access (LiA) responder: TS 102 527-3 V1.7.1 clause 7.4.10.
 *
 * A CAT-iq handset reads the base's lists over {CC-IWU-INFO} messages that
 * carry one <<IWU-TO-IWU>> element each (EN 300 175-5 7.7.23), protocol
 * discriminator 03H "List Access". The handset opens a session on a list,
 * reads entries, ends the session. Three lists are what a Gigaset asks for
 * before it considers itself synchronised with a base: the List of Supported
 * Lists (00H), the Internal Names List (06H) and the Line Settings List
 * (08H). They are small, static, and defined to the byte in clauses
 * 7.4.10.5.2, 7.4.10.5.8 and 7.4.11.4; this file serves them.
 *
 * History, because it explains the defect this replaces. The first version
 * of this responder copied stock's reply bytes without decoding them. Its
 * "read entries confirm" announced three entries and never sent the data
 * packets that TS 102 527-3 7.4.10.4.3.2 says must follow ("Content of list
 * entry is transmitted in data packets"), so every Gigaset ran its sync,
 * waited ~3 s for data that never came, ended the session, tried the next
 * list, released, and started over -- "Synchronisiere" for ever, a
 * list-access call on every link that came up, and an incoming call refused
 * with "insufficient resources" while it was at it. Measured 2026-10-24
 * across 93 logs; see docs/call-g7.md.
 *
 * Coding conventions used throughout (7.4.10.4.10 table 62, 7.4.10.5.1.0,
 * and note 1 at table 39): session identifiers, indices, entry identifiers
 * and lengths are "extensible": values below 128 are one octet with bit 8
 * set; larger values take two octets, high 7 bits first with bit 8 clear.
 * Every field is [identifier][length][properties][value...], the length
 * counting the properties octet.
 *
 * No I/O, no allocation. Feed one <<IWU-TO-IWU>> content in, get up to
 * DECT_IWU_OUT_MAX reply contents out, each to be carried in its own
 * {CC-IWU-INFO} on the same transaction. Re-implemented from the public
 * ETSI text as GPL-2.0-or-later.
 */
#ifndef OPENDECTCELL_DECT_IWU_H
#define OPENDECTCELL_DECT_IWU_H

#include <stddef.h>
#include <stdint.h>

/* Octet 3 of <<IWU-TO-IWU>>: bit 8 fixed 1, S/R = 1 "transmission", PD 03H. */
#define DECT_IWU_DISC		0xC3u
#define DECT_IWU_PD_LIA		0x03u

/* List access commands, TS 102 527-3 7.4.10.4.0. */
#define DECT_LIA_START_SESSION		0x00u
#define DECT_LIA_START_SESSION_CFM	0x01u
#define DECT_LIA_END_SESSION		0x02u
#define DECT_LIA_END_SESSION_CFM	0x03u
#define DECT_LIA_QUERY_FIELDS		0x04u
#define DECT_LIA_QUERY_FIELDS_CFM	0x05u
#define DECT_LIA_READ_ENTRIES		0x06u
#define DECT_LIA_READ_ENTRIES_CFM	0x07u
#define DECT_LIA_EDIT_ENTRY		0x08u
#define DECT_LIA_EDIT_ENTRY_CFM		0x09u
#define DECT_LIA_SAVE_ENTRY		0x0Au
#define DECT_LIA_SAVE_ENTRY_CFM		0x0Bu
#define DECT_LIA_DELETE_ENTRY		0x0Cu
#define DECT_LIA_DELETE_ENTRY_CFM	0x0Du
#define DECT_LIA_DELETE_LIST		0x0Eu
#define DECT_LIA_DELETE_LIST_CFM	0x0Fu
#define DECT_LIA_SEARCH_ENTRIES		0x10u
#define DECT_LIA_SEARCH_ENTRIES_CFM	0x11u
#define DECT_LIA_NACK			0x12u
#define DECT_LIA_DATA			0x13u
#define DECT_LIA_DATA_LAST		0x14u

/* Legacy names kept for the daemon's log vocabulary. */
#define DECT_IWU_OPEN		DECT_LIA_START_SESSION
#define DECT_IWU_OPEN_RESP	DECT_LIA_START_SESSION_CFM
#define DECT_IWU_CLOSE		DECT_LIA_END_SESSION
#define DECT_IWU_CLOSE_ACK	DECT_LIA_END_SESSION_CFM

/* List identifiers, 7.4.10.3. */
#define DECT_LIA_LIST_SUPPORTED		0x00u
#define DECT_LIA_LIST_MISSED		0x01u
#define DECT_LIA_LIST_OUTGOING		0x02u
#define DECT_LIA_LIST_ACCEPTED		0x03u
#define DECT_LIA_LIST_ALL_CALLS		0x04u
#define DECT_LIA_LIST_CONTACTS		0x05u
#define DECT_LIA_LIST_INTERNAL_NAMES	0x06u
#define DECT_LIA_LIST_SYSTEM_SETTINGS	0x07u
#define DECT_LIA_LIST_LINE_SETTINGS	0x08u
#define DECT_LIA_LIST_ALL_INCOMING	0x09u

/* Start session reject reasons, 7.4.10.4.1.2. */
#define DECT_LIA_REJ_NO_RESOURCES	0x00u
#define DECT_LIA_REJ_LIST_IN_USE	0x01u
#define DECT_LIA_REJ_LIST_UNSUPPORTED	0x02u
#define DECT_LIA_REJ_MAX_SESSIONS	0x03u

/* Negative acknowledgement reasons, 7.4.10.4.9. */
#define DECT_LIA_NACK_INVALID_RANGE	0x00u
#define DECT_LIA_NACK_NO_ENTRY		0x01u
#define DECT_LIA_NACK_INVALID_SESSION	0x02u
#define DECT_LIA_NACK_TEMP_NOT_POSSIBLE	0x03u
#define DECT_LIA_NACK_FORMAT		0x04u
#define DECT_LIA_NACK_INVALID_START	0x05u
#define DECT_LIA_NACK_NOT_SUPPORTED	0x06u
#define DECT_LIA_NACK_NOT_ALLOWED	0x07u
#define DECT_LIA_NACK_INCORRECT_PIN	0x0Au
#define DECT_LIA_NACK_PIN_REQUIRED	0x0Bu

/*
 * One <<IWU-TO-IWU>> content: EN 300 175-4 caps a LAPC UI frame at 63
 * octets of information, and TS 102 527-3 7.4.10.4.10 derives 56 octets of
 * data content per data packet from that.
 */
#define DECT_IWU_MSG_MAX	63u
#define DECT_IWU_DATA_PER_PKT	56u
/* Replies one received record can produce: a confirm plus data packets. */
#define DECT_IWU_OUT_MAX	12u
/* Largest data content we assemble before segmenting it into packets. */
#define DECT_IWU_CONTENT_MAX	(DECT_IWU_DATA_PER_PKT * (DECT_IWU_OUT_MAX - 1u))

#define DECT_IWU_NAME_MAX	16u	/* UTF-8 bytes, NUL-terminated */
#define DECT_IWU_PIN_TRIES	3u	/* wrong PIN entries per call */
#define DECT_IWU_SAVE_MAX	128u	/* the data of one 'save entry' */
#define DECT_IWU_HANDSETS	8u	/* Internal Names List capacity */
#define DECT_IWU_SESSIONS	2u	/* "at least two sessions at a time" */

struct dect_iwu_out {
	uint8_t  msg[DECT_IWU_OUT_MAX][DECT_IWU_MSG_MAX];
	size_t   len[DECT_IWU_OUT_MAX];
	unsigned count;
};

struct dect_iwu_handset {
	uint8_t terminal_id;			/* 1..127, the Internal Names "Number" */
	char    name[DECT_IWU_NAME_MAX];
	uint8_t in_call;	/* the owner keeps it: no deletion meanwhile */
};

struct dect_iwu_stats {
	uint32_t opened;	/* sessions started */
	uint32_t closed;	/* sessions ended on request */
	uint32_t replied;	/* typed commands answered positively */
	uint32_t rejected;	/* start rejects and negative acknowledgements */
	uint32_t entries_sent;	/* list entries delivered in data packets */
	uint32_t ignored;	/* records we have no reply for */
	uint32_t malformed;	/* too short, wrong discriminator, bad coding */
	uint32_t saved;		/* entries saved by a handset */
	uint32_t pin_wrong;	/* 'Current PIN code' entries that did not match */
};

/*
 * One line of the Line Settings List (TS 102 527-3 §7.4.11.4): a SIP line
 * from the pool. `attached` holds terminal t in bit t-1; all_handsets
 * attaches every registered handset (the single-line default).
 */
#define DECT_IWU_LINES	8u

struct dect_iwu_line {
	uint8_t  id;			/* 0..126, unique */
	char     name[DECT_IWU_NAME_MAX];
	uint8_t  all_handsets;
	uint32_t attached;		/* used when !all_handsets */
};

struct dect_iwu {
	/* The data behind the three lists; the owner fills these in. */
	struct dect_iwu_handset handsets[DECT_IWU_HANDSETS];
	unsigned n_handsets;
	uint8_t  own_terminal_id;	/* the PP talking to us: 'Own' property */
	/* The line pool, sorted by line id (the list's sort field). */
	struct dect_iwu_line lines[DECT_IWU_LINES];
	unsigned n_lines;
	/* 'FP version / Firmware version' (7.4.11.3.9): IA5, 1..20 chars */
	char     fw_version[21];

	/* Open sessions; session identifier = index + 1. */
	struct {
		uint8_t list;
		uint8_t in_use;
		uint8_t locked;		/* entry an 'edit entry' locked, 0 = none */
	} sessions[DECT_IWU_SESSIONS];
	/* A 'save entry' whose data packets are still arriving (7.4.10.4.9). */
	uint8_t  save_pending;
	uint8_t  save_session;
	uint8_t  save_entry;
	uint8_t  save_overflow;
	size_t   save_len;
	uint8_t  save_data[DECT_IWU_SAVE_MAX];
	/*
	 * Internal Names entries whose name a handset saved (bit i = handsets[i]),
	 * for the owner to keep and to show on the other links; it clears it.
	 * An empty name asks for the owner's default (a field reset, table 52a).
	 */
	uint32_t names_changed;
	/*
	 * The base PIN as 'Current PIN code' compares it (7.4.11.3.1: BCD,
	 * F-padded, 4 octets; dect_dsaa_pin_to_ac() gives exactly that), set
	 * by the owner; pin_ok once a handset entered it on this link, until
	 * the call ends; ciphered as the owner reports the link (a PIN only
	 * travels ciphered, 7.4.11.1).
	 */
	uint8_t  pin[4];
	uint8_t  pin_known;
	uint8_t  pin_ok;
	/*
	 * Wrong PINs on this link. After DECT_IWU_PIN_TRIES no entry is
	 * compared any more while the connection lasts, so guessing costs a
	 * new connection per few tries (security audit 2026-10-29, A4); the
	 * owner limits those. pin_wrong_seen is the owner's, for counting
	 * stats.pin_wrong.
	 */
	uint8_t  pin_tries;
	uint32_t pin_wrong_seen;
	uint8_t  ciphered;
	/* "delete entry" on the Internal Names List: the terminal whose
	 * deregistration a handset asked for (7.4.10.4.6.1); the owner runs it
	 * and clears this */
	uint8_t  delete_request;

	struct dect_iwu_stats stats;
};

void dect_iwu_init(struct dect_iwu *iwu);

/* One line only, every handset attached: the single-line shortcut. */
void dect_iwu_set_line(struct dect_iwu *iwu, uint8_t line_id, const char *name);

/*
 * Add a line to the pool, attached to the terminals in `attached` (bit t-1
 * = terminal t). 0 on success; -1 if the id is taken or > 126, or the
 * pool is full. Kept sorted by line id.
 */
int dect_iwu_add_line(struct dect_iwu *iwu, uint8_t line_id, const char *name,
		      uint32_t attached);

/* Remove every line (then add_line them). */
void dect_iwu_clear_lines(struct dect_iwu *iwu);

/*
 * Is the handset with this terminal id attached to the line? 0 when the
 * line does not exist; 1 for an all-handsets line or terminal 0 (unknown:
 * no refusal without knowledge).
 */
int dect_iwu_attached(const struct dect_iwu *iwu, uint8_t line_id,
		      uint8_t terminal_id);

/* Add a registered handset to the Internal Names List. Keeps the list
 * sorted by terminal id (7.4.10.3 default sorting); a repeated id replaces
 * the entry. Returns 0, or -1 when the list is full or the id invalid. */
int dect_iwu_add_handset(struct dect_iwu *iwu, uint8_t terminal_id,
			 const char *name);

/* Which handset is on the other end of this transaction (its 'Own' bit). */
void dect_iwu_set_own(struct dect_iwu *iwu, uint8_t terminal_id);

/* The base PIN for 'Current PIN code' checks (pin_known set). */
void dect_iwu_set_pin(struct dect_iwu *iwu, const uint8_t pin[4]);

/* Take a deregistered handset out of the list; 0, or -1 if not there. */
int dect_iwu_remove_handset(struct dect_iwu *iwu, uint8_t terminal_id);

/* Forget every open session, e.g. when the call carrying them is released:
 * "The session(s) between a PP and FP shall at latest be terminated with
 * call release" (7.4.10.4.1.3). */
void dect_iwu_reset_sessions(struct dect_iwu *iwu);

/*
 * Handle one received <<IWU-TO-IWU>> content (starting at the discriminator
 * octet). Replies, if any, are appended to `out` -- call with out->count = 0
 * for a fresh set. A record we do not recognise, or that needs no reply,
 * produces nothing: silence beats a guess, which is how this dialog used to
 * end up desynchronised.
 */
void dect_iwu_handle(struct dect_iwu *iwu, const uint8_t *in, size_t in_len,
		     struct dect_iwu_out *out);

#endif /* OPENDECTCELL_DECT_IWU_H */

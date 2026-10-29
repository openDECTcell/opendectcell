/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CAT-iq list access responder. TS 102 527-3 V1.7.1 clause 7.4.10; the
 * clause of each rule is cited where it is applied. See dect_iwu.h.
 */

#include <string.h>

#include "dect_iwu.h"

/* The lists this base serves, ascending (7.4.10.5.1.1 wants them ordered). */
static const uint8_t supported_lists[] = {
	DECT_LIA_LIST_SUPPORTED,
	DECT_LIA_LIST_INTERNAL_NAMES,
	DECT_LIA_LIST_SYSTEM_SETTINGS,
	DECT_LIA_LIST_LINE_SETTINGS,
};

/* Field identifiers per list; also the "Query supported entry fields" reply. */
static const uint8_t fields_supported[] = { 0x01 };		/* 7.4.10.5.2 */
static const uint8_t fields_internal[] = { 0x01, 0x02, 0x03 };	/* 7.4.10.5.8 */
/* 7.4.11.4; table 9 makes 08H and 0BH-0DH mandatory for the FP too */
static const uint8_t fields_line[] = { 0x01, 0x02, 0x03, 0x08,
				       0x0B, 0x0C, 0x0D };
/* 7.4.11.3: the fields table 9 makes mandatory for the FP */
static const uint8_t fields_system[] = { 0x01, 0x02, 0x03, 0x09, 0x0A,
					 0x0D };

void dect_iwu_init(struct dect_iwu *iwu)
{
	memset(iwu, 0, sizeof(*iwu));
	dect_iwu_set_line(iwu, 1, "Line 1");
	strcpy(iwu->fw_version, "openDECTcell");
}

static void line_name(struct dect_iwu_line *l, const char *name)
{
	memset(l->name, 0, sizeof(l->name));
	if (name)
		strncpy(l->name, name, sizeof(l->name) - 1u);
	if (!l->name[0])			/* 7.4.11.4.1: never empty */
		memcpy(l->name, "Line", 5);
}

void dect_iwu_clear_lines(struct dect_iwu *iwu)
{
	memset(iwu->lines, 0, sizeof(iwu->lines));
	iwu->n_lines = 0;
}

void dect_iwu_set_line(struct dect_iwu *iwu, uint8_t line_id, const char *name)
{
	struct dect_iwu_line *l;

	dect_iwu_clear_lines(iwu);
	l = &iwu->lines[0];
	l->id = (uint8_t)(line_id & 0x7Fu);
	l->all_handsets = 1;
	line_name(l, name);
	iwu->n_lines = 1;
}

int dect_iwu_add_line(struct dect_iwu *iwu, uint8_t line_id, const char *name,
		      uint32_t attached)
{
	unsigned i, at;

	if (line_id > 126u || iwu->n_lines >= DECT_IWU_LINES ||
	    dect_iwu_line(iwu, line_id))
		return -1;
	for (at = 0; at < iwu->n_lines && iwu->lines[at].id < line_id; at++)
		;
	for (i = iwu->n_lines; i > at; i--)
		iwu->lines[i] = iwu->lines[i - 1u];
	memset(&iwu->lines[at], 0, sizeof(iwu->lines[at]));
	iwu->lines[at].id = line_id;
	iwu->lines[at].attached = attached;
	line_name(&iwu->lines[at], name);
	iwu->n_lines++;
	return 0;
}

int dect_iwu_attached(const struct dect_iwu *iwu, uint8_t line_id,
		      uint8_t terminal_id)
{
	const struct dect_iwu_line *l = dect_iwu_line(iwu, line_id);

	if (!l)
		return 0;
	if (l->all_handsets || !terminal_id || terminal_id > 32u)
		return 1;
	return (l->attached >> (terminal_id - 1u)) & 1u;
}

const struct dect_iwu_line *dect_iwu_line(const struct dect_iwu *iwu,
					  uint8_t line_id)
{
	unsigned i;

	for (i = 0; i < iwu->n_lines; i++)
		if (iwu->lines[i].id == line_id)
			return &iwu->lines[i];
	return NULL;
}

int dect_iwu_remove_handset(struct dect_iwu *iwu, uint8_t terminal_id)
{
	unsigned i;

	for (i = 0; i < iwu->n_handsets; i++)
		if (iwu->handsets[i].terminal_id == terminal_id)
			break;
	if (i == iwu->n_handsets)
		return -1;
	memmove(&iwu->handsets[i], &iwu->handsets[i + 1u],
		(iwu->n_handsets - i - 1u) * sizeof(iwu->handsets[0]));
	iwu->n_handsets--;
	iwu->names_changed = 0;		/* the bits named positions */
	return 0;
}

int dect_iwu_add_handset(struct dect_iwu *iwu, uint8_t terminal_id,
			 const char *name)
{
	unsigned i, pos;

	if (!terminal_id || terminal_id > 127u)
		return -1;
	for (i = 0; i < iwu->n_handsets; i++)
		if (iwu->handsets[i].terminal_id == terminal_id)
			break;
	if (i == iwu->n_handsets) {
		if (iwu->n_handsets >= DECT_IWU_HANDSETS)
			return -1;
		/* keep ascending order by terminal id */
		for (pos = 0; pos < iwu->n_handsets; pos++)
			if (iwu->handsets[pos].terminal_id > terminal_id)
				break;
		memmove(&iwu->handsets[pos + 1u], &iwu->handsets[pos],
			(iwu->n_handsets - pos) * sizeof(iwu->handsets[0]));
		iwu->n_handsets++;
		i = pos;
	}
	iwu->handsets[i].terminal_id = terminal_id;
	memset(iwu->handsets[i].name, 0, sizeof(iwu->handsets[i].name));
	if (name)
		strncpy(iwu->handsets[i].name, name,
			sizeof(iwu->handsets[i].name) - 1u);
	return 0;
}

void dect_iwu_set_own(struct dect_iwu *iwu, uint8_t terminal_id)
{
	iwu->own_terminal_id = terminal_id;
}

void dect_iwu_reset_sessions(struct dect_iwu *iwu)
{
	memset(iwu->sessions, 0, sizeof(iwu->sessions));
	iwu->save_pending = 0;
	iwu->pin_ok = 0;		/* "in the same call" (7.4.11.1) */
}

void dect_iwu_set_pin(struct dect_iwu *iwu, const uint8_t pin[4])
{
	memcpy(iwu->pin, pin, sizeof(iwu->pin));
	iwu->pin_known = 1;
}

/* ---- coding helpers ---------------------------------------------------- */

/*
 * Read one extensible value (note 1 at table 39): bit 8 set = last octet.
 * Returns the number of octets consumed, 0 on a truncated or over-long value.
 */
static size_t get_ext(const uint8_t *p, size_t n, unsigned *v)
{
	if (n < 1u)
		return 0;
	if (p[0] & 0x80u) {
		*v = p[0] & 0x7Fu;
		return 1;
	}
	if (n < 2u || !(p[1] & 0x80u))
		return 0;
	*v = ((unsigned)(p[0] & 0x7Fu) << 7) | (p[1] & 0x7Fu);
	return 2;
}

struct buf {
	uint8_t *p;
	size_t cap, len;
	int overflow;
};

static void put(struct buf *b, uint8_t v)
{
	if (b->len < b->cap)
		b->p[b->len++] = v;
	else
		b->overflow = 1;
}

static void put_ext(struct buf *b, unsigned v)
{
	if (v < 128u) {
		put(b, (uint8_t)(0x80u | v));
	} else {
		put(b, (uint8_t)((v >> 7) & 0x7Fu));
		put(b, (uint8_t)(0x80u | (v & 0x7Fu)));
	}
}

static void put_bytes(struct buf *b, const void *src, size_t n)
{
	const uint8_t *s = src;

	while (n--)
		put(b, *s++);
}

/*
 * One field: [id][length][properties][value] (7.4.10.5.1.0). The length
 * counts the properties octet; the properties octet carries its own
 * extension bit, set because one octet is all we ever need.
 */
static void put_field(struct buf *b, uint8_t id, uint8_t props,
		      const void *value, size_t vlen)
{
	put(b, id);
	put_ext(b, (unsigned)(vlen + 1u));
	put(b, (uint8_t)(0x80u | props));
	put_bytes(b, value, vlen);
}

/* The common head of every reply: discriminator, command. */
static void begin(struct buf *b, uint8_t *msg, uint8_t cmd)
{
	b->p = msg;
	b->cap = DECT_IWU_MSG_MAX;
	b->len = 0;
	b->overflow = 0;
	put(b, DECT_IWU_DISC);
	put(b, cmd);
}

static void emit(struct dect_iwu_out *out, const struct buf *b)
{
	if (!out || out->count >= DECT_IWU_OUT_MAX || b->overflow)
		return;
	out->len[out->count] = b->len;
	out->count++;
}

/* Build the reply straight into the next output slot. */
static int reply_begin(struct dect_iwu_out *out, struct buf *b, uint8_t cmd)
{
	if (!out || out->count >= DECT_IWU_OUT_MAX)
		return -1;
	begin(b, out->msg[out->count], cmd);
	return 0;
}

static void nack(struct dect_iwu *iwu, struct dect_iwu_out *out,
		 unsigned session, uint8_t reason)
{
	struct buf b;

	if (reply_begin(out, &b, DECT_LIA_NACK))
		return;
	put_ext(&b, session);
	put(&b, reason);
	emit(out, &b);
	iwu->stats.rejected++;
}

/* ---- the lists ----------------------------------------------------------- */

static int list_supported(uint8_t list)
{
	size_t i;

	for (i = 0; i < sizeof(supported_lists); i++)
		if (supported_lists[i] == list)
			return 1;
	return 0;
}

static unsigned entry_count(const struct dect_iwu *iwu, uint8_t list)
{
	switch (list) {
	case DECT_LIA_LIST_SUPPORTED:		return 1;
	case DECT_LIA_LIST_INTERNAL_NAMES:	return iwu->n_handsets;
	case DECT_LIA_LIST_LINE_SETTINGS:	return iwu->n_lines;
	case DECT_LIA_LIST_SYSTEM_SETTINGS:	return 1;	/* singular */
	default:				return 0;
	}
}

static const uint8_t *list_fields(uint8_t list, size_t *n)
{
	switch (list) {
	case DECT_LIA_LIST_SUPPORTED:
		*n = sizeof(fields_supported); return fields_supported;
	case DECT_LIA_LIST_INTERNAL_NAMES:
		*n = sizeof(fields_internal); return fields_internal;
	case DECT_LIA_LIST_LINE_SETTINGS:
		*n = sizeof(fields_line); return fields_line;
	case DECT_LIA_LIST_SYSTEM_SETTINGS:
		*n = sizeof(fields_system); return fields_system;
	default:
		*n = 0; return NULL;
	}
}

/* Default sorting field per list (7.4.10.3), 0 = the list is not sorted. */
static uint8_t default_sort_field(uint8_t list)
{
	switch (list) {
	case DECT_LIA_LIST_INTERNAL_NAMES:	return 0x01;	/* Number */
	case DECT_LIA_LIST_LINE_SETTINGS:	return 0x02;	/* Line id */
	default:				return 0;
	}
}

/*
 * Append one field of one entry, if we hold it. Entries are addressed by
 * list index (1-based) and double as entry identifiers: the lists are
 * static, so the identity of an entry is its position.
 */
static void put_entry_field(const struct dect_iwu *iwu, struct buf *b,
			    uint8_t list, unsigned idx, uint8_t field)
{
	switch (list) {
	case DECT_LIA_LIST_SUPPORTED:
		/* 7.4.10.5.1.1 'List identifiers': properties all reserved. */
		if (field == 0x01)
			put_field(b, field, 0x00, supported_lists,
				  sizeof(supported_lists));
		break;

	case DECT_LIA_LIST_INTERNAL_NAMES: {
		const struct dect_iwu_handset *h;
		char digits[4];
		size_t n = 0;
		unsigned v;

		if (idx < 1u || idx > iwu->n_handsets)
			break;
		h = &iwu->handsets[idx - 1u];
		switch (field) {
		case 0x01:
			/*
			 * 7.4.10.5.1.2 'Number' as a terminal identity number
			 * (7.4.10.5.1.2.1): decimal digits in IA5. Properties:
			 * bit 7 editable = 0 (note 3), bit 6 'internal' = 1
			 * for every Internal Names entry, bit 5 'own' = 1 for
			 * the PP that is reading, bit 1 PIN protected = 0.
			 */
			v = h->terminal_id;
			if (v >= 100u)
				digits[n++] = (char)('0' + v / 100u);
			if (v >= 10u)
				digits[n++] = (char)('0' + (v / 10u) % 10u);
			digits[n++] = (char)('0' + v % 10u);
			/* PIN protected (bit 1): deleting the entry deregisters
			 * the handset, which needs the base PIN (7.4.11.1) */
			put_field(b, field,
				  (uint8_t)(0x21u |
					    (h->terminal_id == iwu->own_terminal_id
					     ? 0x10u : 0u)),
				  digits, n);
			break;
		case 0x02:
			/* 7.4.10.5.1.3 'Name', UTF-8, editable (bit 7, annex H.7) */
			put_field(b, field, 0x40, h->name, strlen(h->name));
			break;
		case 0x03: {
			/* 7.4.10.5.1.13 'Call interception': 31H = allowed. */
			static const uint8_t allowed = 0x31;

			put_field(b, field, 0x00, &allowed, 1);
			break;
		}
		default:
			break;
		}
		break;
	}

	case DECT_LIA_LIST_LINE_SETTINGS: {
		const struct dect_iwu_line *l;

		if (idx < 1u || idx > iwu->n_lines)
			break;
		l = &iwu->lines[idx - 1u];
		switch (field) {
		case 0x01:
			/* 7.4.11.4.1 / 7.4.10.5.1.6 'Line name'. */
			put_field(b, field, 0x00, l->name, strlen(l->name));
			break;
		case 0x02: {
			/*
			 * 7.4.11.4.2 / 7.4.10.5.1.7 'Line id': identifier
			 * subtype 03H "Relating to", then the value, extensible.
			 */
			uint8_t v[2] = { 0x03, (uint8_t)(0x80u | l->id) };

			put_field(b, field, 0x00, v, sizeof(v));
			break;
		}
		case 0x03: {
			/*
			 * 7.4.11.4.3 'Attached handsets': the count, then a
			 * bitmap with terminal 1 in bit 1, 7 terminals per
			 * octet, extension bit set on the last octet only, and
			 * only as many octets as the highest terminal needs.
			 * Only registered handsets count, so a pool line names
			 * no terminal the base does not hold.
			 */
			uint8_t v[1 + 19];
			size_t n = 0, octets = 0, i;
			unsigned hi = 0, count = 0;

			for (i = 0; i < iwu->n_handsets; i++) {
				unsigned t = iwu->handsets[i].terminal_id;

				if (!t || t > 32u || (!l->all_handsets &&
				    !(l->attached & (1u << (t - 1u)))))
					continue;
				count++;
				if (t > hi)
					hi = t;
			}
			v[n++] = (uint8_t)(0x80u | (count & 0x7Fu));
			if (count) {
				octets = (hi + 6u) / 7u;
				memset(v + 1, 0, octets);
				for (i = 0; i < iwu->n_handsets; i++) {
					unsigned t = iwu->handsets[i].terminal_id;

					if (!t || t > 32u || (!l->all_handsets &&
					    !(l->attached & (1u << (t - 1u)))))
						continue;
					v[1 + (t - 1u) / 7u] |=
						(uint8_t)(1u << ((t - 1u) % 7u));
				}
				v[octets] |= 0x80u;
				n += octets;
			}
			put_field(b, field, 0x00, v, n);
			break;
		}
		case 0x08: {
			/*
			 * 7.4.11.4.8 'Multiple calls mode': 30H single call,
			 * 31H multiple call. We hold one call per handset and
			 * have no call waiting, so every line is single call --
			 * "for non-multiple call lines, the value 'Single call
			 * mode' shall always be used". Not editable.
			 */
			static const uint8_t single = 0x30;

			put_field(b, field, 0x00, &single, 1);
			break;
		}
		case 0x0B:
		case 0x0D: {
			/*
			 * 7.4.11.4.11 / .13 'Call forwarding unconditional' and
			 * 'on busy': value 30H (off), then the activation code,
			 * deactivation code and forwarding number lengths, all 0
			 * -- the field's default. Not editable: forwarding needs
			 * the network side (the bridge), which does not exist.
			 */
			static const uint8_t off[4] = { 0x30, 0, 0, 0 };

			put_field(b, field, 0x00, off, sizeof(off));
			break;
		}
		case 0x0C: {
			/* 7.4.11.4.12 'on no answer': seconds 0 = the FP's default */
			static const uint8_t off[5] = { 0x30, 0, 0, 0, 0 };

			put_field(b, field, 0x00, off, sizeof(off));
			break;
		}
		default:
			break;
		}
		break;
	}

	case DECT_LIA_LIST_SYSTEM_SETTINGS: {
		/*
		 * The DECT System Settings List, one entry (7.4.11.3), read
		 * only for now: nothing is marked editable, so an edit or save
		 * is 'procedure not allowed'. Missing before (spec sweep, H21).
		 */
		static const uint8_t hidden_pin[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
		static const uint8_t fp = 0x30, yes = 0x31;

		if (idx != 1u)
			break;
		switch (field) {
		case 0x01:
			/* 7.4.11.3.1: "always ... 'FF'H 'FF'H 'FF'H 'FF'H";
			 * editable, to check a PIN the user enters */
			put_field(b, field, 0x40, hidden_pin, sizeof(hidden_pin));
			break;
		case 0x02:
			/* 7.4.11.3.2 'Clock master': 30H, the FP sets the time */
			put_field(b, field, 0x00, &fp, 1);
			break;
		case 0x03:
			/* 7.4.11.3.3 'Base reset': 31H while nothing was changed
			 * from its default, which is all a PP can see here */
			put_field(b, field, 0x00, &yes, 1);
			break;
		case 0x09: {
			/* 7.4.11.3.9: IA5, 1..20 characters, never empty */
			size_t len = strlen(iwu->fw_version);

			put_field(b, field, 0x00, iwu->fw_version,
				  len > 20u ? 20u : len);
			break;
		}
		case 0x0A:
			/* 7.4.11.3.10: may be empty when there is no EEPROM */
			put_field(b, field, 0x00, NULL, 0);
			break;
		case 0x0D:
			/* 7.4.11.3.13: PIN protected always; hidden like 01H */
			put_field(b, field, 0x01, hidden_pin, sizeof(hidden_pin));
			break;
		default:
			break;
		}
		break;
	}

	default:
		break;
	}
}

/* ---- commands ------------------------------------------------------------ */

static void start_session(struct dect_iwu *iwu, const uint8_t *p, size_t n,
			  struct dect_iwu_out *out)
{
	struct buf b;
	uint8_t list;
	unsigned i, slot = DECT_IWU_SESSIONS;
	uint8_t reject = 0xFFu;
	uint8_t sort;

	/* 7.4.10.4.1.1 table 42: list identifier, then the sorting fields. */
	if (n < 1u) {
		iwu->stats.malformed++;
		return;
	}
	list = p[0];

	if (!list_supported(list)) {
		reject = DECT_LIA_REJ_LIST_UNSUPPORTED;
	} else {
		/* A list already open to this PP: reuse its session rather
		 * than letting a half-torn-down dialog exhaust the slots. */
		for (i = 0; i < DECT_IWU_SESSIONS; i++)
			if (iwu->sessions[i].in_use && iwu->sessions[i].list == list)
				slot = i;
		for (i = 0; slot == DECT_IWU_SESSIONS && i < DECT_IWU_SESSIONS; i++)
			if (!iwu->sessions[i].in_use)
				slot = i;
		if (slot == DECT_IWU_SESSIONS)
			reject = DECT_LIA_REJ_MAX_SESSIONS;
	}

	/*
	 * 7.4.10.4.1.2 table 43. On a reject the session identifier is 0 and
	 * the reason says why; on a confirm the reason "shall not be
	 * evaluated" -- it stays at FFH, the value a stock base sends, so the
	 * handset keeps seeing the byte it has always accepted.
	 */
	if (reply_begin(out, &b, DECT_LIA_START_SESSION_CFM))
		return;
	put(&b, list);
	if (reject != 0xFFu) {
		put_ext(&b, 0);				/* session identifier */
		put_ext(&b, 0);				/* total entries */
		put(&b, 0x00);				/* discriminator type */
		put(&b, 0x00);
		put(&b, 0x00);				/* EMC, do not care */
		put(&b, reject);
		put(&b, 0x00);				/* no sorting fields */
		emit(out, &b);
		iwu->stats.rejected++;
		return;
	}
	iwu->sessions[slot].in_use = 1;
	iwu->sessions[slot].list = list;
	iwu->sessions[slot].locked = 0;
	put_ext(&b, slot + 1u);				/* session identifier */
	put_ext(&b, entry_count(iwu, list));		/* total entries */
	put(&b, 0x00);					/* undefined: no proprietary extensions */
	put(&b, 0x00);
	put(&b, 0x00);
	put(&b, 0xFFu);					/* reject reason, not evaluated */
	/*
	 * "the FP shall confirm the sorting fields which were actually used",
	 * the defaults of 7.4.10.3 whatever the PP suggested; and 0 of them
	 * for the one-entry lists.
	 */
	sort = default_sort_field(list);
	if (sort) {
		put(&b, 0x01);
		put(&b, sort);
	} else {
		put(&b, 0x00);
	}
	emit(out, &b);
	iwu->stats.opened++;
}

/* Session identifier -> list, or -1 when no such session is open. */
static int session_list(const struct dect_iwu *iwu, unsigned session)
{
	if (session < 1u || session > DECT_IWU_SESSIONS ||
	    !iwu->sessions[session - 1u].in_use)
		return -1;
	return iwu->sessions[session - 1u].list;
}

static void end_session(struct dect_iwu *iwu, const uint8_t *p, size_t n,
			struct dect_iwu_out *out)
{
	struct buf b;
	unsigned session;

	if (!get_ext(p, n, &session)) {
		iwu->stats.malformed++;
		return;
	}
	/* 7.4.10.4.1.3: a wrong session identifier "should be ignored". */
	if (session_list(iwu, session) < 0) {
		iwu->stats.ignored++;
		return;
	}
	iwu->sessions[session - 1u].in_use = 0;
	if (reply_begin(out, &b, DECT_LIA_END_SESSION_CFM))
		return;
	put_ext(&b, session);
	emit(out, &b);
	iwu->stats.closed++;
}

static void query_fields(struct dect_iwu *iwu, const uint8_t *p, size_t n,
			 struct dect_iwu_out *out)
{
	struct buf b;
	unsigned session;
	int list;
	const uint8_t *f;
	size_t nf;

	if (!get_ext(p, n, &session)) {
		iwu->stats.malformed++;
		return;
	}
	list = session_list(iwu, session);
	if (list < 0) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_SESSION);
		return;
	}
	/* table 41 note 1: not on the List of Supported Lists */
	if (list == DECT_LIA_LIST_SUPPORTED) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	/* 7.4.10.4.2.2 table 47: editable fields first, then the rest. */
	f = list_fields((uint8_t)list, &nf);
	if (reply_begin(out, &b, DECT_LIA_QUERY_FIELDS_CFM))
		return;
	put_ext(&b, session);
	if (list == DECT_LIA_LIST_INTERNAL_NAMES ||
	    list == DECT_LIA_LIST_SYSTEM_SETTINGS) {
		/* the name (06H), or 'Current PIN code' (07H) */
		uint8_t ed = list == DECT_LIA_LIST_INTERNAL_NAMES ? 0x02 : 0x01;
		size_t i;

		put(&b, 0x01);
		put(&b, ed);
		put(&b, (uint8_t)(nf - 1u));
		for (i = 0; i < nf; i++)
			if (f[i] != ed)
				put(&b, f[i]);
	} else {
		put(&b, 0x00);
		put(&b, (uint8_t)nf);
		put_bytes(&b, f, nf);
	}
	emit(out, &b);
	iwu->stats.replied++;
}

static int assemble(const struct dect_iwu *iwu, uint8_t list, unsigned first,
		    unsigned last, const uint8_t *wanted, size_t nwanted,
		    struct buf *content, uint8_t *data, size_t cap);
static void send_data(struct dect_iwu_out *out, unsigned session,
		      const struct buf *content);

static void read_entries(struct dect_iwu *iwu, const uint8_t *p, size_t n,
			 struct dect_iwu_out *out)
{
	struct buf b, content;
	uint8_t data[DECT_IWU_CONTENT_MAX];
	uint8_t wanted[16];
	unsigned session, start, count, first, last, total;
	size_t k, nwanted = 0, i;
	uint8_t mark;
	int list, backward;

	/* 7.4.10.4.3.1 table 48. */
	k = get_ext(p, n, &session);
	if (!k) goto malformed;
	p += k; n -= k;
	k = get_ext(p, n, &start);
	if (!k) goto malformed;
	p += k; n -= k;
	if (n < 2u) goto malformed;
	backward = (p[0] & 0x80u) != 0;
	count = p[0] & 0x7Fu;
	mark = p[1];
	p += 2; n -= 2;
	(void)mark;			/* no 'Read status' field in our lists */

	list = session_list(iwu, session);
	if (list < 0) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_SESSION);
		return;
	}
	/* Requested fields, in ascending order as the data packet wants them;
	 * duplicates and fields we do not hold are simply not delivered. */
	while (n && nwanted < sizeof(wanted)) {
		uint8_t f = *p++;

		n--;
		for (i = 0; i < nwanted && wanted[i] < f; i++)
			;
		if (i < nwanted && wanted[i] == f)
			continue;
		memmove(&wanted[i + 1u], &wanted[i], nwanted - i);
		wanted[i] = f;
		nwanted++;
	}

	total = entry_count(iwu, (uint8_t)list);
	if (!total) {
		/* table 38a: "Type 2 is used ... if no entry is found" */
		nack(iwu, out, session, DECT_LIA_NACK_NO_ENTRY);
		return;
	}
	if (!start)
		start = total;			/* 0 = "last entry" */
	if (start > total) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_START);
		return;
	}
	if (!count)
		count = 1;
	if (backward) {
		first = start + 1u > count ? start + 1u - count : 1u;
		last = start;
	} else {
		first = start;
		last = start + count - 1u;
		if (last > total)
			last = total;
	}
	/* "Type 1 is used if no field was requested (mark entries request
	 * only)": a confirm with counter 0 and no data packet. */
	if (!nwanted)
		first = 1u, last = 0u;

	if (assemble(iwu, (uint8_t)list, first, last, wanted, nwanted, &content,
		     data, sizeof(data))) {
		/* Larger than we can deliver in one go: refuse the range
		 * rather than truncate an entry (7.4.10.4.3.2). */
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_RANGE);
		return;
	}

	/* 7.4.10.4.3.2 table 49: confirm, then the data packets. */
	if (reply_begin(out, &b, DECT_LIA_READ_ENTRIES_CFM))
		return;
	put_ext(&b, session);
	put_ext(&b, first);
	put(&b, (uint8_t)(last >= first ? last - first + 1u : 0u));
	emit(out, &b);
	iwu->stats.replied++;
	send_data(out, session, &content);
	if (last >= first)
		iwu->stats.entries_sent += last - first + 1u;
	return;

malformed:
	iwu->stats.malformed++;
}

/*
 * The data content of entries first..last, ascending, each [entry id]
 * [entry length][fields...] (7.4.10.4.10 table 62). -1 if it does not fit.
 */
static int assemble(const struct dect_iwu *iwu, uint8_t list, unsigned first,
		    unsigned last, const uint8_t *wanted, size_t nwanted,
		    struct buf *content, uint8_t *data, size_t cap)
{
	unsigned idx;
	size_t i;

	content->p = data;
	content->cap = cap;
	content->len = 0;
	content->overflow = 0;
	for (idx = first; idx <= last && !content->overflow; idx++) {
		struct buf e;
		uint8_t ebuf[128];

		e.p = ebuf; e.cap = sizeof(ebuf); e.len = 0; e.overflow = 0;
		for (i = 0; i < nwanted; i++)
			put_entry_field(iwu, &e, list, idx, wanted[i]);
		if (e.overflow) {
			content->overflow = 1;
			break;
		}
		put_ext(content, idx);		/* entry identifier */
		put_ext(content, (unsigned)e.len);
		put_bytes(content, ebuf, e.len);
	}
	return content->overflow ? -1 : 0;
}

/* The data content in data packets of 56 octets, the last one marked. */
static void send_data(struct dect_iwu_out *out, unsigned session,
		      const struct buf *content)
{
	struct buf b;
	size_t off;

	for (off = 0; off < content->len; off += DECT_IWU_DATA_PER_PKT) {
		size_t chunk = content->len - off;
		int is_last = chunk <= DECT_IWU_DATA_PER_PKT;

		if (!is_last)
			chunk = DECT_IWU_DATA_PER_PKT;
		if (reply_begin(out, &b, is_last ? DECT_LIA_DATA_LAST
						 : DECT_LIA_DATA))
			return;
		put_ext(&b, session);
		put_bytes(&b, content->p + off, chunk);
		emit(out, &b);
	}
}

/* A command we take the session of but do not perform. */
static void refuse(struct dect_iwu *iwu, const uint8_t *p, size_t n,
		   struct dect_iwu_out *out, uint8_t reason)
{
	unsigned session;

	if (!get_ext(p, n, &session)) {
		iwu->stats.malformed++;
		return;
	}
	if (session_list(iwu, session) < 0)
		reason = DECT_LIA_NACK_INVALID_SESSION;
	nack(iwu, out, session, reason);
}

/*
 * "Edit entry" (7.4.10.4.4): one entry, read like 'read entries' and locked
 * for this session until its 'save entry' or the session's end. Every list
 * we serve may be edited except the List of Supported Lists (table 41);
 * whether a field may then be saved is its 'editable' property. The lock
 * is per link: another handset's edit of the same entry is not seen here.
 */
static void edit_entry(struct dect_iwu *iwu, const uint8_t *p, size_t n,
		       struct dect_iwu_out *out)
{
	uint8_t data[DECT_IWU_CONTENT_MAX], wanted[16];
	const uint8_t *known;
	struct buf b, content;
	unsigned session, entry;
	size_t k, nknown, nwanted = 0, i;
	int list;

	k = get_ext(p, n, &session);
	if (!k)
		goto malformed;
	p += k; n -= k;
	k = get_ext(p, n, &entry);
	if (!k)
		goto malformed;
	p += k; n -= k;
	list = session_list(iwu, session);
	if (list < 0) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_SESSION);
		return;
	}
	if (list == DECT_LIA_LIST_SUPPORTED) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	if (entry < 1u || entry > entry_count(iwu, (uint8_t)list)) {
		nack(iwu, out, session, DECT_LIA_NACK_NO_ENTRY);
		return;
	}
	/* the fields asked for that we hold, ascending; unknown ones ignored,
	 * and none known at all is 'procedure not allowed' */
	known = list_fields((uint8_t)list, &nknown);
	for (i = 0; i < nknown && nwanted < sizeof(wanted); i++)
		if (memchr(p, known[i], n))
			wanted[nwanted++] = known[i];
	if (!nwanted) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	/* 7.4.11.1: the PIN fields only on a ciphered link */
	if (list == DECT_LIA_LIST_SYSTEM_SETTINGS && !iwu->ciphered &&
	    (memchr(wanted, 0x01, nwanted) || memchr(wanted, 0x0D, nwanted))) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	if (assemble(iwu, (uint8_t)list, entry, entry, wanted, nwanted,
		     &content, data, sizeof(data))) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_RANGE);
		return;
	}
	iwu->sessions[session - 1u].locked = (uint8_t)entry;
	if (reply_begin(out, &b, DECT_LIA_EDIT_ENTRY_CFM))
		return;
	put_ext(&b, session);
	emit(out, &b);
	iwu->stats.replied++;
	send_data(out, session, &content);
	iwu->stats.entries_sent++;
	return;

malformed:
	iwu->stats.malformed++;
}

/*
 * The data of a 'save entry' has all arrived (7.4.10.4.5): apply it in best
 * effort mode and answer once. Only the Internal Names List's name can be
 * saved; any other field in the data is 'procedure not allowed', applied or
 * not, and so is a save of an entry not locked by 'edit entry' in this
 * session or of a new entry (none of our lists takes one). An empty save
 * ends an edit without change (7.4.10.4.5.1). The lock is released in
 * every case.
 */
static void finish_save(struct dect_iwu *iwu, struct dect_iwu_out *out)
{
	unsigned session = iwu->save_session, entry, len, fid, flen;
	const uint8_t *p = iwu->save_data;
	size_t n = iwu->save_len, k;
	uint8_t reason = 0, locked;
	struct buf b;
	int list = session_list(iwu, session);

	if (list < 0) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_SESSION);
		return;
	}
	locked = iwu->sessions[session - 1u].locked;
	iwu->sessions[session - 1u].locked = 0;
	if (iwu->save_entry && iwu->save_entry > entry_count(iwu, (uint8_t)list)) {
		nack(iwu, out, session, DECT_LIA_NACK_NO_ENTRY);
		return;
	}
	if (!iwu->save_entry || locked != iwu->save_entry) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	if (iwu->save_overflow) {
		nack(iwu, out, session, DECT_LIA_NACK_FORMAT);
		return;
	}
	if (n) {
		/* table 62: [entry id][entry length][fields ...] */
		k = get_ext(p, n, &entry);
		if (!k || entry != iwu->save_entry)
			goto format;
		p += k; n -= k;
		k = get_ext(p, n, &len);
		if (!k || len > n - k)
			goto format;
		p += k; n = len;
		while (n) {
			fid = *p++;
			n--;
			k = get_ext(p, n, &flen);
			if (!k || flen < 1u || flen > n - k)
				goto format;
			p += k; n -= k;
			if (list == DECT_LIA_LIST_SYSTEM_SETTINGS &&
			    fid == 0x01) {
				/*
				 * 'Current PIN code' (7.4.11.1): compare, never
				 * store; ciphered links only. A correct PIN
				 * opens PIN protected changes until the call
				 * ends.
				 */
				if (!iwu->ciphered)
					reason = DECT_LIA_NACK_NOT_ALLOWED;
				else if (iwu->pin_tries >= DECT_IWU_PIN_TRIES) {
					reason = DECT_LIA_NACK_INCORRECT_PIN;
					iwu->pin_ok = 0;
				} else if (flen != 5u || !iwu->pin_known ||
					 memcmp(p + 1, iwu->pin, 4)) {
					reason = DECT_LIA_NACK_INCORRECT_PIN;
					iwu->pin_ok = 0;
					iwu->pin_tries++;
					iwu->stats.pin_wrong++;
				} else {
					iwu->pin_ok = 1;
				}
			} else if (list == DECT_LIA_LIST_INTERNAL_NAMES &&
				   fid == 0x02) {
				/* [properties][UTF-8 name]; length 1 = reset */
				struct dect_iwu_handset *h =
					&iwu->handsets[iwu->save_entry - 1u];
				size_t nl = flen - 1u;

				if (nl > DECT_IWU_NAME_MAX - 1u) {
					/* truncated (7.4.10.1), never inside a
					 * UTF-8 sequence */
					nl = DECT_IWU_NAME_MAX - 1u;
					while (nl && (p[1 + nl] & 0xC0u) == 0x80u)
						nl--;
				}
				memcpy(h->name, p + 1, nl);
				h->name[nl] = '\0';
				iwu->names_changed |= 1u << (iwu->save_entry - 1u);
			} else {
				reason = DECT_LIA_NACK_NOT_ALLOWED;
			}
			p += flen; n -= flen;
		}
	}
	if (reason) {
		nack(iwu, out, session, reason);
		return;
	}
	/* table 53: the entry stays where it was (sorted by number) */
	if (reply_begin(out, &b, DECT_LIA_SAVE_ENTRY_CFM))
		return;
	put_ext(&b, session);
	put_ext(&b, iwu->save_entry);
	put_ext(&b, iwu->save_entry);
	put_ext(&b, entry_count(iwu, (uint8_t)list));
	emit(out, &b);
	iwu->stats.replied++;
	iwu->stats.saved++;
	return;

format:
	nack(iwu, out, session, DECT_LIA_NACK_FORMAT);
}

/*
 * "Search entries" (7.4.10.4.8) on the Internal Names List, sorted by its
 * 'Number' (the terminal identity number): the searched value is its IA5
 * decimal form. Matching option 00H wants the exact entry; 01H takes the
 * next one ("current index") and 02H the one before when there is none;
 * then entries are delivered as by 'read entries' from there. No match is
 * a confirm with counter 0 and no data packet.
 */
static void search_entries(struct dect_iwu *iwu, unsigned session, int list,
			   const uint8_t *p, size_t n, struct dect_iwu_out *out)
{
	uint8_t data[DECT_IWU_CONTENT_MAX], wanted[16];
	const uint8_t *known;
	struct buf b, content;
	unsigned opt, slen, v = 0, total, cur, start = 0, count, first, last;
	size_t nknown, nwanted = 0, j;
	int digits = 1, backward;

	if (n < 2u)
		goto malformed;
	opt = p[0];
	slen = p[1];
	p += 2; n -= 2;
	if (slen > n || n - slen < 2u)
		goto malformed;
	for (j = 0; j < slen; j++) {
		if (p[j] < '0' || p[j] > '9' || v > 1000u)
			digits = 0;
		else
			v = v * 10u + (unsigned)(p[j] - '0');
	}
	p += slen; n -= slen;
	backward = (p[0] & 0x80u) != 0;
	count = p[0] & 0x7Fu;
	p += 2; n -= 2;				/* counter, mark request */
	known = list_fields((uint8_t)list, &nknown);
	for (j = 0; j < nknown && nwanted < sizeof(wanted); j++)
		if (memchr(p, known[j], n))
			wanted[nwanted++] = known[j];

	/* the first entry whose number is >= v: the 'current entry' */
	total = entry_count(iwu, (uint8_t)list);
	for (cur = 1; cur <= total; cur++)
		if (iwu->handsets[cur - 1u].terminal_id >= v)
			break;
	if (digits && slen && cur <= total &&
	    iwu->handsets[cur - 1u].terminal_id == v)
		start = cur;				/* exact */
	else if (opt == 0x01u && cur <= total)
		start = cur;
	else if (opt == 0x02u && cur > 1u)
		start = cur - 1u;
	if (!count)
		count = 1;
	first = 1u, last = 0u;
	if (start && nwanted) {
		if (backward) {
			first = start + 1u > count ? start + 1u - count : 1u;
			last = start;
		} else {
			first = start;
			last = start + count - 1u > total ? total
							  : start + count - 1u;
		}
	}
	if (assemble(iwu, (uint8_t)list, first, last, wanted, nwanted, &content,
		     data, sizeof(data))) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_RANGE);
		return;
	}
	if (reply_begin(out, &b, DECT_LIA_SEARCH_ENTRIES_CFM))
		return;
	put_ext(&b, session);
	put_ext(&b, start ? first : 1u);
	put(&b, (uint8_t)(last >= first ? last - first + 1u : 0u));
	emit(out, &b);
	iwu->stats.replied++;
	send_data(out, session, &content);
	if (last >= first)
		iwu->stats.entries_sent += last - first + 1u;
	return;

malformed:
	iwu->stats.malformed++;
}

/*
 * "Delete entry" (7.4.10.4.6). Only the Internal Names List takes it, where
 * it means "deregister that handset": refused while that handset is in a
 * call ('temporarily not possible') and without the base PIN entered in
 * this call -- its 'Number' is PIN protected ('PIN code required'). Then
 * the confirm, with the count the list will have, and delete_request for
 * the owner, which runs the deregistration and removes the entry. On every
 * other list we serve it is 'procedure not allowed'.
 */
static void delete_entry(struct dect_iwu *iwu, const uint8_t *p, size_t n,
			 struct dect_iwu_out *out)
{
	unsigned session, entry;
	size_t k = get_ext(p, n, &session);
	struct buf b;
	int list;

	if (!k || !get_ext(p + k, n - k, &entry)) {
		iwu->stats.malformed++;
		return;
	}
	list = session_list(iwu, session);
	if (list < 0) {
		nack(iwu, out, session, DECT_LIA_NACK_INVALID_SESSION);
		return;
	}
	if (list != DECT_LIA_LIST_INTERNAL_NAMES) {
		nack(iwu, out, session, DECT_LIA_NACK_NOT_ALLOWED);
		return;
	}
	if (entry < 1u || entry > iwu->n_handsets) {
		nack(iwu, out, session, DECT_LIA_NACK_NO_ENTRY);
		return;
	}
	if (iwu->handsets[entry - 1u].in_call || iwu->delete_request) {
		nack(iwu, out, session, DECT_LIA_NACK_TEMP_NOT_POSSIBLE);
		return;
	}
	if (!iwu->pin_ok) {
		nack(iwu, out, session, DECT_LIA_NACK_PIN_REQUIRED);
		return;
	}
	iwu->delete_request = iwu->handsets[entry - 1u].terminal_id;
	if (reply_begin(out, &b, DECT_LIA_DELETE_ENTRY_CFM))
		return;
	put_ext(&b, session);
	put_ext(&b, iwu->n_handsets - 1u);
	emit(out, &b);
	iwu->stats.replied++;
}

/* One data packet of a 'save entry': [session][content ...] (table 61). */
static void save_data(struct dect_iwu *iwu, const uint8_t *p, size_t n)
{
	unsigned session;
	size_t k = get_ext(p, n, &session);

	if (!k || session != iwu->save_session) {
		iwu->stats.malformed++;
		return;
	}
	p += k; n -= k;
	if (n > sizeof(iwu->save_data) - iwu->save_len) {
		iwu->save_overflow = 1;
		return;
	}
	memcpy(iwu->save_data + iwu->save_len, p, n);
	iwu->save_len += n;
}

void dect_iwu_handle(struct dect_iwu *iwu, const uint8_t *in, size_t in_len,
		     struct dect_iwu_out *out)
{
	const uint8_t *p;
	size_t n;

	/* Discriminator, S/R and PD; then the command. */
	if (!in || in_len < 2u || (in[0] & 0x3Fu) != DECT_IWU_PD_LIA) {
		iwu->stats.malformed++;
		return;
	}
	if (!(in[0] & 0x40u)) {
		/* S/R = 0, a rejection: table 38 says the PP never sends it. */
		iwu->stats.ignored++;
		return;
	}
	p = in + 2;
	n = in_len - 2u;

	switch (in[1]) {
	case DECT_LIA_START_SESSION:
		start_session(iwu, p, n, out);
		break;
	case DECT_LIA_END_SESSION:
		end_session(iwu, p, n, out);
		break;
	case DECT_LIA_QUERY_FIELDS:
		query_fields(iwu, p, n, out);
		break;
	case DECT_LIA_READ_ENTRIES:
		read_entries(iwu, p, n, out);
		break;
	case DECT_LIA_SEARCH_ENTRIES: {
		/*
		 * Table 41: search is mandatory on the Internal Names List,
		 * and "I" on the List of Supported Lists, the DECT System
		 * Settings List and the Line Settings List, answered
		 * 'procedure not allowed' (note 1). We sent 'not supported'
		 * for all (spec sweep, H3, H12).
		 */
		unsigned session;
		size_t k = get_ext(p, n, &session);
		int list = k ? session_list(iwu, session) : -1;

		if (list == DECT_LIA_LIST_INTERNAL_NAMES)
			search_entries(iwu, session, list, p + k, n - k, out);
		else
			refuse(iwu, p, n, out, DECT_LIA_NACK_NOT_ALLOWED);
		break;
	}
	case DECT_LIA_EDIT_ENTRY:
		edit_entry(iwu, p, n, out);
		break;
	case DECT_LIA_DELETE_ENTRY:
		delete_entry(iwu, p, n, out);
		break;
	case DECT_LIA_DELETE_LIST:
		/* table 41 notes 1 and 4: 'procedure not allowed'. */
		refuse(iwu, p, n, out, DECT_LIA_NACK_NOT_ALLOWED);
		break;
	case DECT_LIA_SAVE_ENTRY: {
		/*
		 * 7.4.10.4.9: "For commands containing data (i.e. for the save
		 * command), this implies waiting until the 'data packet last'
		 * information is received" before the negative acknowledgement.
		 */
		unsigned session, entry;
		size_t k = get_ext(p, n, &session);

		if (!k || !get_ext(p + k, n - k, &entry)) {
			iwu->stats.malformed++;
			break;
		}
		iwu->save_pending = 1;
		iwu->save_session = (uint8_t)session;
		iwu->save_entry = (uint8_t)(entry > 127u ? 127u : entry);
		iwu->save_len = 0;
		iwu->save_overflow = 0;
		break;
	}
	case DECT_LIA_DATA:
		if (iwu->save_pending)
			save_data(iwu, p, n);	/* part of a save */
		else
			iwu->stats.ignored++;
		break;
	case DECT_LIA_DATA_LAST:
		if (iwu->save_pending) {
			save_data(iwu, p, n);
			iwu->save_pending = 0;
			finish_save(iwu, out);
		} else {
			iwu->stats.ignored++;
		}
		break;
	default:
		/* Confirms and acknowledgements are ours to send, not to
		 * answer; proprietary commands (bit 8 set) get nothing. */
		iwu->stats.ignored++;
		break;
	}
}

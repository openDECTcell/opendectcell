/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base's Contact List (TS 102 527-3 V1.7.1 clause 7.4.10.5.7): one store
 * shared by every link's list access responder (dect_iwu.c), kept sorted by
 * 'Name' then 'First name' in the case insensitive alphanumerical order
 * (7.4.10.3, 7.4.10.4.1.1).
 *
 * The contacts come from an address book on a server (a CardDAV sync, see
 * openwrt/net/opendectcell/files/carddav.uc) as a text file, and what handsets
 * change goes back as a journal of changes. Both formats are below. Each
 * entry carries the source's key for its contact (0 = created on a handset,
 * not on the server yet) and keeps its entry identifier across reloads, as
 * 7.4.10.4.5.1 asks ("should not re-assign a previously freed ... entry
 * identifier" while sessions are open: identifiers here only grow).
 *
 * The contacts file, written by the sync, read whole by dect_contacts_load():
 *
 *   #opendectcell-contacts 1 seq=N [clear=1]
 *   KEY<TAB>HINT<TAB>NAME<TAB>FIRST<TAB>MELODY<TAB>LINE[<TAB>NUMBER]...
 *
 * KEY is 8 hex digits (never 0); HINT is the entry identifier a handset gave
 * a contact it created (from the journal), or '-'; MELODY 0 (undefined) or
 * 1..7; LINE '*' (all lines) or a line id; NUMBER is PP:DIGITS with PP the
 * properties octet of 'Contact number' in hex (own 10, fixed 08, mobile 04,
 * work 02) and DIGITS from 0-9 * #. seq=N says the journal up to change N
 * has been applied: an entry changed on a handset later than that keeps the
 * handset's version. clear=1 allows 'delete list' (off by default: it would
 * empty the address book on the server).
 *
 * The journal, one line per change for the owner to append to a file:
 *
 *   SEQ<TAB>put<TAB>ID<TAB>KEY<TAB>NAME<TAB>FIRST<TAB>MELODY<TAB>LINE[<TAB>NUMBER]...
 *   SEQ<TAB>del<TAB>ID<TAB>KEY
 *   SEQ<TAB>clear
 *
 * No I/O and no allocation; the store is one static object of the owner.
 */
#ifndef OPENDECTCELL_DECT_CONTACTS_H
#define OPENDECTCELL_DECT_CONTACTS_H

#include <stddef.h>
#include <stdint.h>

#define DECT_CONTACTS_MAX	500u	/* entries */
#define DECT_CONTACT_NAME_MAX	32u	/* UTF-8 bytes with the NUL */
#define DECT_CONTACT_NUMS	3u	/* 'Contact number' instances (note 2, 7.4.10.5.7) */
#define DECT_CONTACT_DIGITS_MAX	32u	/* with the NUL */
#define DECT_CONTACT_ALL_LINES	0xFFu
#define DECT_CONTACT_ID_MAX	16383u	/* the extensible coding's limit */
#define DECT_CONTACTS_JOURNAL	64u	/* changes not yet taken by the owner */
#define DECT_CONTACTS_TOMBS	64u	/* deletions the sync has not seen yet */

/* 'Contact number' properties, 7.4.10.5.1.11 (the bits we keep). */
#define DECT_CONTACT_OWN	0x10u
#define DECT_CONTACT_FIXED	0x08u
#define DECT_CONTACT_MOBILE	0x04u
#define DECT_CONTACT_WORK	0x02u
#define DECT_CONTACT_TYPES	0x1Eu

struct dect_contact_number {
	uint8_t props;				/* DECT_CONTACT_* */
	char    digits[DECT_CONTACT_DIGITS_MAX];
};

struct dect_contact {
	uint16_t id;				/* entry identifier, 1.. */
	uint32_t key;				/* the source's, 0 = not there yet */
	char     name[DECT_CONTACT_NAME_MAX];	/* last name */
	char     first[DECT_CONTACT_NAME_MAX];
	uint8_t  n_numbers;
	struct dect_contact_number numbers[DECT_CONTACT_NUMS];
	uint8_t  melody;			/* 0 = undefined, 1..7 */
	uint8_t  line;				/* DECT_CONTACT_ALL_LINES or a line id */
	uint32_t dirty;				/* seq of its last handset change, 0 = none */
	const void *lock;			/* the responder editing it, or NULL */
	uint8_t  lock_session;
};

enum dect_contact_op { DECT_CONTACT_PUT = 1, DECT_CONTACT_DEL, DECT_CONTACT_CLEAR };

struct dect_contact_change {
	uint32_t seq;
	uint8_t  op;
	uint16_t id;
	uint32_t key;
};

struct dect_contacts {
	struct dect_contact e[DECT_CONTACTS_MAX];	/* unordered storage */
	uint16_t order[DECT_CONTACTS_MAX];	/* index i+1 -> e[order[i]] */
	unsigned n;
	uint16_t next_id;
	uint32_t seq;				/* the last journalled change */
	uint32_t synced;			/* the file's seq=N */
	uint32_t clear_seq;			/* a 'delete list' not yet synced */
	uint8_t  allow_clear;
	struct dect_contact_change journal[DECT_CONTACTS_JOURNAL];
	unsigned n_journal;
	struct { uint32_t key, seq; } tombs[DECT_CONTACTS_TOMBS];
	unsigned n_tombs;
	unsigned generation;			/* bumps on every change */
	unsigned dropped;			/* lines of a file that did not fit or parse */
};

void dect_contacts_init(struct dect_contacts *c);

/* The entry at list index idx (1-based, sorted), or NULL. */
const struct dect_contact *dect_contacts_at(const struct dect_contacts *c,
					    unsigned idx);
/* The list index of the entry with this identifier, 0 if there is none. */
unsigned dect_contacts_index(const struct dect_contacts *c, unsigned id);

/*
 * Compare two strings in the case insensitive alphanumerical order: ASCII
 * letters without case, the Latin-1 letters as their base letter (ä as a,
 * ß as ss: DIN 5007-1). <0, 0, >0.
 */
int dect_contacts_fold_cmp(const char *a, const char *b);

/*
 * Save an entry: id 0 creates one. `in` holds the new content (its id, key,
 * dirty and lock are ignored). Journals the change. The entry's identifier,
 * or 0 if the store or the journal is full.
 */
unsigned dect_contacts_put(struct dect_contacts *c, unsigned id,
			   const struct dect_contact *in);
/* Delete one entry (journalled); 0, or -1 if the journal is full. */
int dect_contacts_del(struct dect_contacts *c, unsigned id);
/* Delete them all (journalled); 0, or -1 if the journal is full. */
int dect_contacts_clear(struct dect_contacts *c);

/*
 * Replace the store's contents with a contacts file (see above), keeping
 * entry identifiers, locks and changes the file does not include yet.
 * 0, or -1 if the header is not ours (nothing changed then).
 */
int dect_contacts_load(struct dect_contacts *c, const char *text, size_t len);

/*
 * Take the oldest journalled change as one line (no newline), for the
 * owner to append to the journal file. Its length; 0 when there is none;
 * the change stays queued if `cap` is too small.
 */
size_t dect_contacts_take_change(struct dect_contacts *c, char *out, size_t cap);

/* Release every lock this responder holds (its sessions ended). */
void dect_contacts_unlock(struct dect_contacts *c, const void *owner);

/*
 * The contact with this number, for a caller's name: the digits compared
 * after dropping all but 0-9 * #, so "+49 30 123" and "004930123" match.
 */
const struct dect_contact *dect_contacts_find_number(const struct dect_contacts *c,
						     const char *digits);

#endif

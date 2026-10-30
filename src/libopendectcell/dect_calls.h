/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base's call lists (TS 102 527-3 V1.7.1 clauses 7.4.10.5.3-5): the
 * Missed Calls List (01H), the Outgoing Calls List (02H) and the Incoming
 * Accepted Calls List (03H), in RAM, shared by every link's list access
 * responder (dect_iwu.c).
 *
 * Each handset sees the calls of its own lines only, under its own line id
 * ("FP virtual list strategy 2", 7.4.10.2.1 note 8): an entry records, per
 * terminal id, the handset's line the call was on, 0 when it is not one of
 * that handset's. Lists are newest first ("ante chronological", 7.4.10.4.1);
 * a new call to or from the party of the newest entry with the same lines
 * merges into it (merging strategy 3, 7.4.10.5.3-4): a missed one counts up
 * 'Number of calls' and is unread again.
 *
 * No I/O and no allocation; the store is one static object of the owner.
 */
#ifndef OPENDECTCELL_DECT_CALLS_H
#define OPENDECTCELL_DECT_CALLS_H

#include <stddef.h>
#include <stdint.h>

#include "dect_time.h"

#define DECT_CALLS_MAX		40u	/* entries per list */
#define DECT_CALLS_TERMINALS	8u	/* terminal ids 1..7 */
#define DECT_CALL_NUMBER_MAX	33u	/* digits with the NUL */
#define DECT_CALL_NAME_MAX	33u	/* UTF-8 bytes with the NUL */

/* The lists, by their list identifiers (7.4.10.3). */
#define DECT_CALLS_MISSED	0x01u
#define DECT_CALLS_OUTGOING	0x02u
#define DECT_CALLS_ACCEPTED	0x03u

struct dect_call {
	uint16_t id;				/* entry identifier, 1.. */
	char     number[DECT_CALL_NUMBER_MAX];	/* digits * #, may be empty */
	char     name[DECT_CALL_NAME_MAX];	/* may be empty */
	struct dect_time when;			/* the last call's */
	uint8_t  line[DECT_CALLS_TERMINALS];	/* per terminal id; 0 = not its */
	uint8_t  unread;			/* missed calls only */
	uint8_t  count;				/* 'Number of calls', missed only */
};

struct dect_calls {
	struct dect_call e[3][DECT_CALLS_MAX];	/* [list-1], newest first */
	unsigned n[3];
	uint16_t next_id;
	/*
	 * The Missed Calls List changed on a handset (read, deleted): the
	 * owner sends the missed call notifications (7.4.1.3) and clears it.
	 */
	uint8_t  missed_changed;
};

void dect_calls_init(struct dect_calls *c);

/*
 * Add a call: list one of DECT_CALLS_*, number digits (others dropped, a
 * leading + as 00), name UTF-8 (cut on a character boundary), lines per
 * terminal id. The entry's identifier, or 0 for a bad list.
 */
unsigned dect_calls_add(struct dect_calls *c, unsigned list, const char *number,
			const char *name, const struct dect_time *when,
			const uint8_t line[DECT_CALLS_TERMINALS]);

/* Entries of a list the handset with this terminal id sees. */
unsigned dect_calls_count(const struct dect_calls *c, unsigned list,
			  unsigned tid);
/* Its idx-th (1-based, newest first), or NULL. */
struct dect_call *dect_calls_at(struct dect_calls *c, unsigned list,
				unsigned tid, unsigned idx);
/* The list index of entry id as that handset sees it, 0 if none. */
unsigned dect_calls_index(const struct dect_calls *c, unsigned list,
			  unsigned tid, unsigned id);
/* Delete an entry; 0, or -1 if there is none. */
int dect_calls_del(struct dect_calls *c, unsigned list, unsigned id);
/* Delete what the handset sees of a list. */
void dect_calls_clear(struct dect_calls *c, unsigned list, unsigned tid);

/*
 * For the missed call notification (7.4.1.3): the unread entries and all
 * entries of the Missed Calls List on the handset's line `line`.
 */
unsigned dect_calls_missed_unread(const struct dect_calls *c, unsigned tid,
				  unsigned line);
unsigned dect_calls_missed_total(const struct dect_calls *c, unsigned tid,
				 unsigned line);

/*
 * The lists as text, to keep them over a restart of the daemon (the owner
 * writes them to a file in RAM, /var/run): one line per entry, list order
 *
 *   #opendectcell-calls 1
 *   LIST<TAB>ID<TAB>UNREAD<TAB>COUNT<TAB>YYMMDDhhmmss<TAB>TZ<TAB>LINES<TAB>NUMBER<TAB>NAME
 *
 * TZ the quarter-hours from GMT, or '-' when unknown; LINES 16 hex digits,
 * the line per terminal id 0..7. dect_calls_save() returns the length, 0
 * if it does not fit; dect_calls_load() replaces the store, 0 or -1 if the
 * header is not ours (nothing changed then); lines that do not parse are
 * skipped.
 */
size_t dect_calls_save(const struct dect_calls *c, char *out, size_t cap);
int dect_calls_load(struct dect_calls *c, const char *text, size_t len);

#endif

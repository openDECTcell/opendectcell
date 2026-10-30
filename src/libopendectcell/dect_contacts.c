/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base's Contact List, shared by every link's list access responder.
 * See dect_contacts.h for the file formats.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dect_contacts.h"

void dect_contacts_init(struct dect_contacts *c)
{
	memset(c, 0, sizeof(*c));
	c->next_id = 1;
}

const struct dect_contact *dect_contacts_at(const struct dect_contacts *c,
					    unsigned idx)
{
	if (idx < 1u || idx > c->n)
		return NULL;
	return &c->e[c->order[idx - 1u]];
}

unsigned dect_contacts_index(const struct dect_contacts *c, unsigned id)
{
	unsigned i;

	for (i = 0; i < c->n; i++)
		if (c->e[c->order[i]].id == id)
			return i + 1u;
	return 0;
}

/* ---- the order ----------------------------------------------------------- */

/*
 * The next character of a UTF-8 string folded for comparison, as up to two
 * bytes in `out`; the number of input bytes it used (0 at the end).
 */
static size_t fold_next(const unsigned char *s, unsigned char out[2],
			size_t *nout)
{
	/* U+00C0..U+00FF as their base letters; 0 = keep the code point */
	static const char latin1[] =
		"aaaaaaaceeeeiiiidnooooo\0ouuuuyts"	/* C0..DF, D7 and DE kept */
		"aaaaaaaceeeeiiiidnooooo\0ouuuuyty";	/* E0..FF, F7 kept */
	unsigned cp;

	if (!s[0])
		return 0;
	if (s[0] < 0x80u) {
		out[0] = (unsigned char)(s[0] >= 'A' && s[0] <= 'Z' ? s[0] + 32 : s[0]);
		*nout = 1;
		return 1;
	}
	if ((s[0] & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
		cp = ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
		if (cp >= 0xC0u && latin1[cp - 0xC0u]) {
			char f = latin1[cp - 0xC0u];

			if (cp == 0xDFu) {			/* ß as ss */
				out[0] = 's'; out[1] = 's';
				*nout = 2;
			} else if (cp == 0xC6u || cp == 0xE6u) {	/* æ as ae */
				out[0] = 'a'; out[1] = 'e';
				*nout = 2;
			} else {
				out[0] = (unsigned char)f;
				*nout = 1;
			}
			return 2;
		}
	}
	/* anything else sorts by its first byte, after the letters */
	out[0] = s[0];
	*nout = 1;
	return 1;
}

int dect_contacts_fold_cmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a;
	const unsigned char *q = (const unsigned char *)b;
	unsigned char fa[2], fb[2];
	size_t na = 0, nb = 0, ia = 0, ib = 0;

	for (;;) {
		if (ia == na) {
			size_t k = fold_next(p, fa, &na);

			p += k;
			ia = 0;
			if (!k)
				na = 0;
		}
		if (ib == nb) {
			size_t k = fold_next(q, fb, &nb);

			q += k;
			ib = 0;
			if (!k)
				nb = 0;
		}
		if (!na || !nb)
			return (na ? 1 : 0) - (nb ? 1 : 0);
		if (fa[ia] != fb[ib])
			return fa[ia] < fb[ib] ? -1 : 1;
		ia++;
		ib++;
	}
}

/*
 * Name, then first name, without case (7.4.10.3); case sensitively among
 * those equal so (figure 54a, example 1), then by identifier: a full order.
 */
static int entry_cmp(const struct dect_contact *x, const struct dect_contact *y)
{
	int r = dect_contacts_fold_cmp(x->name, y->name);

	if (!r)
		r = dect_contacts_fold_cmp(x->first, y->first);
	if (!r)
		r = strcmp(x->name, y->name);
	if (!r)
		r = strcmp(x->first, y->first);
	if (!r)
		r = (x->id > y->id) - (x->id < y->id);
	return r;
}

static void sort(struct dect_contacts *c)
{
	unsigned i, j;

	/* insertion sort: the list is sorted but for one entry, or loaded once */
	for (i = 1; i < c->n; i++) {
		uint16_t v = c->order[i];

		for (j = i; j > 0 && entry_cmp(&c->e[c->order[j - 1u]], &c->e[v]) > 0; j--)
			c->order[j] = c->order[j - 1u];
		c->order[j] = v;
	}
}

/* ---- changes ------------------------------------------------------------- */

static struct dect_contact *by_id(struct dect_contacts *c, unsigned id,
				  unsigned *pos)
{
	unsigned i;

	for (i = 0; i < c->n; i++)
		if (c->e[c->order[i]].id == id) {
			if (pos)
				*pos = i;
			return &c->e[c->order[i]];
		}
	return NULL;
}

/* A free identifier: storage e[0..n-1] is scanned, not the order, which a
 * load has not rebuilt yet when it asks. */
static uint16_t new_id(struct dect_contacts *c)
{
	unsigned tries, i;

	for (tries = 0; tries <= DECT_CONTACT_ID_MAX; tries++) {
		uint16_t id = c->next_id;

		c->next_id = (uint16_t)(id >= DECT_CONTACT_ID_MAX ? 1u : id + 1u);
		for (i = 0; i < c->n && c->e[i].id != id; i++)
			;
		if (i == c->n)
			return id;
	}
	return 0;
}

static int journal(struct dect_contacts *c, uint8_t op, uint16_t id,
		   uint32_t key)
{
	struct dect_contact_change *ch;

	if (c->n_journal >= DECT_CONTACTS_JOURNAL)
		return -1;
	ch = &c->journal[c->n_journal++];
	ch->seq = ++c->seq;
	ch->op = op;
	ch->id = id;
	ch->key = key;
	c->generation++;
	return 0;
}

static void tomb(struct dect_contacts *c, uint32_t key)
{
	if (!key)
		return;
	if (c->n_tombs == DECT_CONTACTS_TOMBS) {
		/* the oldest goes; at worst a deleted contact shows again
		 * until the sync has caught up */
		memmove(&c->tombs[0], &c->tombs[1],
			(DECT_CONTACTS_TOMBS - 1u) * sizeof(c->tombs[0]));
		c->n_tombs--;
	}
	c->tombs[c->n_tombs].key = key;
	c->tombs[c->n_tombs].seq = c->seq;
	c->n_tombs++;
}

unsigned dect_contacts_put(struct dect_contacts *c, unsigned id,
			   const struct dect_contact *in)
{
	struct dect_contact *e;
	unsigned slot;

	if (c->n_journal >= DECT_CONTACTS_JOURNAL)
		return 0;
	if (id) {
		e = by_id(c, id, NULL);
		if (!e)
			return 0;
	} else {
		if (c->n >= DECT_CONTACTS_MAX)
			return 0;
		slot = c->n;			/* storage stays packed, see del */
		e = &c->e[slot];
		memset(e, 0, sizeof(*e));
		e->id = new_id(c);
		if (!e->id)
			return 0;
		c->order[c->n++] = (uint16_t)slot;
	}
	memcpy(e->name, in->name, sizeof(e->name));
	memcpy(e->first, in->first, sizeof(e->first));
	e->name[sizeof(e->name) - 1u] = '\0';
	e->first[sizeof(e->first) - 1u] = '\0';
	e->n_numbers = in->n_numbers > DECT_CONTACT_NUMS ? DECT_CONTACT_NUMS
							 : in->n_numbers;
	memcpy(e->numbers, in->numbers, sizeof(e->numbers));
	e->melody = in->melody;
	e->line = in->line;
	(void)journal(c, DECT_CONTACT_PUT, e->id, e->key);
	e->dirty = c->seq;
	sort(c);
	return e->id;
}

/* Remove the entry at sorted position pos, keeping the storage packed. */
static void remove_at(struct dect_contacts *c, unsigned pos)
{
	unsigned slot = c->order[pos], last = c->n - 1u, i;

	memmove(&c->order[pos], &c->order[pos + 1u],
		(c->n - pos - 1u) * sizeof(c->order[0]));
	c->n--;
	if (slot != last) {
		/* move the last stored entry into the hole */
		c->e[slot] = c->e[last];
		for (i = 0; i < c->n; i++)
			if (c->order[i] == last)
				c->order[i] = (uint16_t)slot;
	}
}

int dect_contacts_del(struct dect_contacts *c, unsigned id)
{
	struct dect_contact *e;
	unsigned pos;

	e = by_id(c, id, &pos);
	if (!e)
		return 0;
	if (journal(c, DECT_CONTACT_DEL, e->id, e->key))
		return -1;
	tomb(c, e->key);
	remove_at(c, pos);
	return 0;
}

int dect_contacts_clear(struct dect_contacts *c)
{
	if (journal(c, DECT_CONTACT_CLEAR, 0, 0))
		return -1;
	c->clear_seq = c->seq;
	c->n = 0;
	c->n_tombs = 0;
	return 0;
}

void dect_contacts_unlock(struct dect_contacts *c, const void *owner)
{
	unsigned i;

	for (i = 0; i < c->n; i++)
		if (c->e[i].lock == owner)
			c->e[i].lock = NULL;
}

/* ---- the files ----------------------------------------------------------- */

static int hexval(int ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/* One tab-separated field of [p, end) into out (truncated on a UTF-8
 * boundary); returns the start of the next field, or NULL at the end. */
static const char *field(const char *p, const char *end, char *out, size_t cap)
{
	const char *t = p;
	size_t n;

	while (t < end && *t != '\t')
		t++;
	n = (size_t)(t - p);
	if (n > cap - 1u) {
		n = cap - 1u;
		while (n && ((unsigned char)p[n] & 0xC0u) == 0x80u)
			n--;
	}
	memcpy(out, p, n);
	out[n] = '\0';
	return t < end ? t + 1 : NULL;
}

static int number_parse(const char *s, struct dect_contact_number *num)
{
	int h1 = hexval(s[0]), h2 = s[0] ? hexval(s[1]) : -1;
	size_t n = 0;

	if (h1 < 0 || h2 < 0 || s[2] != ':')
		return -1;
	num->props = (uint8_t)(((h1 << 4) | h2) & DECT_CONTACT_TYPES);
	for (s += 3; *s && n < sizeof(num->digits) - 1u; s++)
		if ((*s >= '0' && *s <= '9') || *s == '*' || *s == '#')
			num->digits[n++] = *s;
	num->digits[n] = '\0';
	return n ? 0 : -1;
}

/* One contact line; 0, or -1 if it does not parse. */
static int line_parse(const char *p, const char *end, struct dect_contact *e,
		      unsigned *hint)
{
	char f[DECT_CONTACT_DIGITS_MAX + 4u];
	unsigned long v;
	char *x;

	memset(e, 0, sizeof(*e));
	p = field(p, end, f, sizeof(f));
	v = strtoul(f, &x, 16);
	if (!p || x == f || *x || !v || v > 0xFFFFFFFFul)
		return -1;
	e->key = (uint32_t)v;
	p = field(p, end, f, sizeof(f));
	*hint = 0;
	if (!p)
		return -1;
	if (strcmp(f, "-")) {
		v = strtoul(f, &x, 10);
		if (x == f || *x)
			return -1;
		*hint = v <= DECT_CONTACT_ID_MAX ? (unsigned)v : 0u;
	}
	p = field(p, end, e->name, sizeof(e->name));
	if (!p)
		return -1;
	p = field(p, end, e->first, sizeof(e->first));
	if (!p)
		return -1;
	p = field(p, end, f, sizeof(f));
	if (!p || f[0] < '0' || f[0] > '7' || f[1])
		return -1;
	e->melody = (uint8_t)(f[0] - '0');
	p = field(p, end, f, sizeof(f));
	if (!strcmp(f, "*")) {
		e->line = DECT_CONTACT_ALL_LINES;
	} else {
		v = strtoul(f, &x, 10);
		if (x == f || *x || v > 126u)
			return -1;
		e->line = (uint8_t)v;
	}
	while (p && e->n_numbers < DECT_CONTACT_NUMS) {
		p = field(p, end, f, sizeof(f));
		if (!number_parse(f, &e->numbers[e->n_numbers]))
			e->n_numbers++;
	}
	return 0;
}

/* The first occurrence of `w` in [p, p+n), or NULL (no memmem in C11). */
static const char *find(const char *p, size_t n, const char *w)
{
	size_t k = strlen(w), i;

	for (i = 0; i + k <= n; i++)
		if (!memcmp(p + i, w, k))
			return p + i;
	return NULL;
}

static int tombed(const struct dect_contacts *c, uint32_t key)
{
	unsigned i;

	for (i = 0; i < c->n_tombs; i++)
		if (c->tombs[i].key == key)
			return 1;
	return 0;
}

int dect_contacts_load(struct dect_contacts *c, const char *text, size_t len)
{
	static struct dect_contacts old;	/* the store before; one owner */
	const char *p = text, *end = text + len, *nl;
	unsigned long synced = 0;
	unsigned i, hint;
	int clear = 0;
	const char *s;

	/* the header */
	nl = memchr(p, '\n', len);
	if (!nl || (size_t)(nl - p) < 19u || memcmp(p, "#opendectcell-contacts 1", 19))
		return -1;
	s = find(p, (size_t)(nl - p), "seq=");
	if (s)
		synced = strtoul(s + 4, NULL, 10);
	clear = find(p, (size_t)(nl - p), "clear=1") != NULL;

	old = *c;
	c->synced = (uint32_t)synced;
	c->allow_clear = (uint8_t)clear;
	if (c->seq < c->synced)
		c->seq = c->synced;	/* journal numbers never go back */
	/* the sync has seen these deletions now */
	for (i = 0; i < c->n_tombs; )
		if (c->tombs[i].seq <= c->synced) {
			memmove(&c->tombs[i], &c->tombs[i + 1u],
				(c->n_tombs - i - 1u) * sizeof(c->tombs[0]));
			c->n_tombs--;
		} else {
			i++;
		}
	c->n = 0;
	c->dropped = 0;

	/*
	 * A 'delete list' the sync has not applied yet: the file still holds
	 * what was deleted, so only the handsets' later entries stay.
	 */
	if (c->clear_seq > c->synced) {
		for (i = 0; i < old.n; i++)
			if (old.e[i].dirty > c->synced && c->n < DECT_CONTACTS_MAX)
				c->e[c->n++] = old.e[i];
		goto done;
	}
	c->clear_seq = 0;

	for (p = nl + 1; p < end; p = nl + 1) {
		struct dect_contact e, *prev = NULL;

		nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			nl = end;
		if (nl == p || *p == '#')
			continue;
		if (line_parse(p, nl, &e, &hint) || tombed(c, e.key) ||
		    c->n >= DECT_CONTACTS_MAX) {
			c->dropped++;
			continue;
		}
		/* the same contact as before: by key, or a new one by hint */
		for (i = 0; i < old.n && !prev; i++)
			if (old.e[i].key == e.key ||
			    (hint && !old.e[i].key && old.e[i].id == hint))
				prev = &old.e[i];
		if (prev && prev->dirty > c->synced) {
			/* changed on a handset since: the handset's version,
			 * with the key the server gave a new one */
			uint32_t key = e.key;

			e = *prev;
			if (!e.key)
				e.key = key;
		} else if (prev) {
			e.id = prev->id;
			e.lock = prev->lock;
			e.lock_session = prev->lock_session;
		} else {
			e.id = 0;
		}
		if (prev)
			prev->id = 0;		/* taken */
		c->e[c->n++] = e;
	}
	/* entries made on a handset that the file does not have yet */
	for (i = 0; i < old.n; i++)
		if (old.e[i].id && !old.e[i].key && old.e[i].dirty > c->synced &&
		    c->n < DECT_CONTACTS_MAX)
			c->e[c->n++] = old.e[i];
done:
	/* identifiers for the new ones, never one still in use */
	for (i = 0; i < c->n; i++)
		if (!c->e[i].id) {
			c->e[i].id = new_id(c);
			if (!c->e[i].id) {
				c->n = i;
				break;
			}
		}
	for (i = 0; i < c->n; i++)
		c->order[i] = (uint16_t)i;
	sort(c);
	c->generation++;
	return 0;
}

static size_t put_str(char *out, size_t cap, size_t at, const char *s)
{
	size_t n = strlen(s);

	if (at < cap) {
		size_t k = n < cap - at ? n : cap - at;

		memcpy(out + at, s, k);
	}
	return at + n;
}

size_t dect_contacts_take_change(struct dect_contacts *c, char *out, size_t cap)
{
	const struct dect_contact_change *ch;
	const struct dect_contact *e = NULL;
	char tmp[64];
	size_t at = 0;
	unsigned i;

	if (!c->n_journal || !cap)
		return 0;
	ch = &c->journal[0];
	if (ch->op == DECT_CONTACT_CLEAR) {
		snprintf(tmp, sizeof(tmp), "%u\tclear", (unsigned)ch->seq);
		at = put_str(out, cap, 0, tmp);
	} else if (ch->op == DECT_CONTACT_DEL) {
		snprintf(tmp, sizeof(tmp), "%u\tdel\t%u\t%08x", (unsigned)ch->seq,
			 (unsigned)ch->id, (unsigned)ch->key);
		at = put_str(out, cap, 0, tmp);
	} else {
		unsigned pos;

		e = by_id(c, ch->id, &pos);
		if (!e) {
			/* deleted again before we got to it: a no-op */
			memmove(&c->journal[0], &c->journal[1],
				(c->n_journal - 1u) * sizeof(c->journal[0]));
			c->n_journal--;
			return dect_contacts_take_change(c, out, cap);
		}
		snprintf(tmp, sizeof(tmp), "%u\tput\t%u\t%08x\t", (unsigned)ch->seq,
			 (unsigned)e->id, (unsigned)e->key);
		at = put_str(out, cap, 0, tmp);
		at = put_str(out, cap, at, e->name);
		at = put_str(out, cap, at, "\t");
		at = put_str(out, cap, at, e->first);
		if (e->line == DECT_CONTACT_ALL_LINES)
			snprintf(tmp, sizeof(tmp), "\t%u\t*", (unsigned)e->melody);
		else
			snprintf(tmp, sizeof(tmp), "\t%u\t%u", (unsigned)e->melody,
				 (unsigned)e->line);
		at = put_str(out, cap, at, tmp);
		for (i = 0; i < e->n_numbers; i++) {
			snprintf(tmp, sizeof(tmp), "\t%02x:", e->numbers[i].props);
			at = put_str(out, cap, at, tmp);
			at = put_str(out, cap, at, e->numbers[i].digits);
		}
	}
	if (at >= cap)
		return 0;
	out[at] = '\0';
	memmove(&c->journal[0], &c->journal[1],
		(c->n_journal - 1u) * sizeof(c->journal[0]));
	c->n_journal--;
	return at;
}

/* ---- caller names -------------------------------------------------------- */

/* Digits only, a leading + as 00. */
static size_t digits_of(const char *s, char *out, size_t cap)
{
	size_t n = 0;

	for (; *s && n + 2u < cap; s++) {
		if (*s == '+' && !n) {
			out[n++] = '0';
			out[n++] = '0';
		} else if ((*s >= '0' && *s <= '9') || *s == '*' || *s == '#') {
			out[n++] = *s;
		}
	}
	out[n] = '\0';
	return n;
}

/*
 * Equal, or one is the other without its trunk or international prefix:
 * "030123456" and "004930123456" share "30123456". At least 6 digits must
 * match, so short internal numbers never match a long one by chance.
 */
static int same_number(const char *a, size_t na, const char *b, size_t nb)
{
	const char *sa = a, *sb = b;

	if (na == nb)
		return !memcmp(a, b, na);
	while (*sa == '0')
		sa++;
	while (*sb == '0')
		sb++;
	na = strlen(sa);
	nb = strlen(sb);
	if (na < 6u || nb < 6u)
		return 0;
	if (na > nb)
		return !strcmp(sa + na - nb, sb);
	return !strcmp(sb + nb - na, sa);
}

const struct dect_contact *dect_contacts_find_number(const struct dect_contacts *c,
						     const char *digits)
{
	char want[DECT_CONTACT_DIGITS_MAX + 2u], have[DECT_CONTACT_DIGITS_MAX + 2u];
	size_t nw = digits_of(digits, want, sizeof(want)), nh;
	unsigned i, k;

	if (!nw)
		return NULL;
	for (i = 0; i < c->n; i++) {
		const struct dect_contact *e = &c->e[c->order[i]];

		for (k = 0; k < e->n_numbers; k++) {
			nh = digits_of(e->numbers[k].digits, have, sizeof(have));
			if (nh && same_number(want, nw, have, nh))
				return e;
		}
	}
	return NULL;
}

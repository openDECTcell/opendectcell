/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base's call lists. See dect_calls.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dect_calls.h"

void dect_calls_init(struct dect_calls *c)
{
	memset(c, 0, sizeof(*c));
	c->next_id = 1;
}

static int list_ok(unsigned list)
{
	return list >= DECT_CALLS_MISSED && list <= DECT_CALLS_ACCEPTED;
}

/* Digits only, a leading + as 00. */
static void digits_of(const char *s, char *out, size_t cap)
{
	size_t n = 0;

	for (; s && *s && n + 3u < cap; s++) {
		if (*s == '+' && !n) {
			out[n++] = '0';
			out[n++] = '0';
		} else if ((*s >= '0' && *s <= '9') || *s == '*' || *s == '#') {
			out[n++] = *s;
		}
	}
	out[n] = '\0';
}

/* UTF-8 text without control characters, cut on a character boundary. */
static void text_of(const char *s, char *out, size_t cap)
{
	size_t n = 0;

	for (; s && s[n] && n + 1u < cap; n++)
		out[n] = (unsigned char)s[n] < 0x20u || s[n] == 0x7F ? ' ' : s[n];
	/* cut: never inside a character (s[n] continues the last one) */
	if (s && s[n])
		while (n && ((unsigned char)s[n] & 0xC0u) == 0x80u)
			n--;
	out[n] = '\0';
}

static uint16_t new_id(struct dect_calls *c)
{
	unsigned tries, l, i;

	for (tries = 0; tries < 16384u; tries++) {
		uint16_t id = c->next_id;
		int used = 0;

		c->next_id = (uint16_t)(id >= 16383u ? 1u : id + 1u);
		for (l = 0; l < 3u && !used; l++)
			for (i = 0; i < c->n[l] && !used; i++)
				used = c->e[l][i].id == id;
		if (!used)
			return id;
	}
	return 0;
}

unsigned dect_calls_add(struct dect_calls *c, unsigned list, const char *number,
			const char *name, const struct dect_time *when,
			const uint8_t line[DECT_CALLS_TERMINALS])
{
	struct dect_call e, *top;
	unsigned l;

	if (!list_ok(list))
		return 0;
	l = list - 1u;
	memset(&e, 0, sizeof(e));
	digits_of(number, e.number, sizeof(e.number));
	text_of(name, e.name, sizeof(e.name));
	e.when = *when;
	memcpy(e.line, line, sizeof(e.line));
	e.unread = list == DECT_CALLS_MISSED;
	e.count = 1;

	/* merging strategy 3: with the newest entry only */
	top = c->n[l] ? &c->e[l][0] : NULL;
	if (top && e.number[0] && !strcmp(top->number, e.number) &&
	    !memcmp(top->line, e.line, sizeof(e.line))) {
		top->when = e.when;
		if (e.name[0])
			memcpy(top->name, e.name, sizeof(e.name));
		if (list == DECT_CALLS_MISSED) {
			top->unread = 1;
			if (top->count < 255u)
				top->count++;
		}
		return top->id;
	}
	e.id = new_id(c);
	if (!e.id)
		return 0;
	if (c->n[l] == DECT_CALLS_MAX)
		c->n[l]--;			/* the oldest goes */
	memmove(&c->e[l][1], &c->e[l][0], c->n[l] * sizeof(e));
	c->e[l][0] = e;
	c->n[l]++;
	return e.id;
}

static int seen_by(const struct dect_call *e, unsigned tid)
{
	return tid < DECT_CALLS_TERMINALS && e->line[tid];
}

/* The stored lists a list shows: one, or all three for the All Calls List. */
static unsigned view_of(unsigned list)
{
	if (list == DECT_CALLS_ALL)
		return 7u;
	return list_ok(list) ? 1u << (list - 1u) : 0u;
}

/* Newer first by the date and time of the last call; 0 if equal. */
static int time_cmp(const struct dect_time *a, const struct dect_time *b)
{
	const uint8_t x[6] = { a->year, a->month, a->day,
			       a->hour, a->minute, a->second };
	const uint8_t y[6] = { b->year, b->month, b->day,
			       b->hour, b->minute, b->second };
	unsigned k;

	for (k = 0; k < 6u; k++)
		if (x[k] != y[k])
			return x[k] > y[k] ? -1 : 1;
	return 0;
}

/*
 * Walk a list as the handset with terminal id tid sees it, newest first:
 * the stored lists of the view merged by time (each is newest first
 * already; on equal times the lower list identifier first). Stops at the
 * idx-th entry (1-based) or, with idx 0, at the entry `id`; *pos counts
 * the entries passed. NULL at the end.
 */
static const struct dect_call *walk(const struct dect_calls *c, unsigned list,
				    unsigned tid, unsigned idx, unsigned id,
				    unsigned *pos)
{
	unsigned at[3] = { 0, 0, 0 }, view = view_of(list), l, best;

	*pos = 0;
	for (;;) {
		const struct dect_call *e = NULL;

		best = 3u;
		for (l = 0; l < 3u; l++) {
			if (!(view & (1u << l)))
				continue;
			while (at[l] < c->n[l] && !seen_by(&c->e[l][at[l]], tid))
				at[l]++;
			if (at[l] < c->n[l] &&
			    (!e || time_cmp(&c->e[l][at[l]].when, &e->when) < 0)) {
				e = &c->e[l][at[l]];
				best = l;
			}
		}
		if (!e)
			return NULL;
		at[best]++;
		++*pos;
		if (idx ? *pos == idx : e->id == id)
			return e;
	}
}

unsigned dect_calls_count(const struct dect_calls *c, unsigned list,
			  unsigned tid)
{
	unsigned n;

	(void)walk(c, list, tid, ~0u, 0, &n);
	return n;
}

struct dect_call *dect_calls_at(struct dect_calls *c, unsigned list,
				unsigned tid, unsigned idx)
{
	unsigned n;

	return idx ? (struct dect_call *)walk(c, list, tid, idx, 0, &n) : NULL;
}

unsigned dect_calls_index(const struct dect_calls *c, unsigned list,
			  unsigned tid, unsigned id)
{
	unsigned n;

	return id && walk(c, list, tid, 0, id, &n) ? n : 0;
}

unsigned dect_calls_type(const struct dect_calls *c, const struct dect_call *e)
{
	unsigned l;

	for (l = 0; l < 3u; l++)
		if (e >= c->e[l] && e < c->e[l] + c->n[l])
			return l + 1u;
	return 0;
}

static void remove_at(struct dect_calls *c, unsigned l, unsigned i)
{
	memmove(&c->e[l][i], &c->e[l][i + 1u],
		(c->n[l] - i - 1u) * sizeof(c->e[l][0]));
	c->n[l]--;
}

int dect_calls_del(struct dect_calls *c, unsigned list, unsigned id)
{
	unsigned i, l, view = view_of(list);

	for (l = 0; l < 3u; l++)
		for (i = 0; (view & (1u << l)) && i < c->n[l]; i++)
			if (c->e[l][i].id == id) {
				remove_at(c, l, i);
				if (l == DECT_CALLS_MISSED - 1u)
					c->missed_changed = 1;
				return 0;
			}
	return -1;
}

void dect_calls_clear(struct dect_calls *c, unsigned list, unsigned tid)
{
	unsigned i, l, view = view_of(list);

	for (l = 0; l < 3u; l++) {
		if (!(view & (1u << l)))
			continue;
		for (i = 0; i < c->n[l]; )
			if (seen_by(&c->e[l][i], tid))
				remove_at(c, l, i);
			else
				i++;
		if (l == DECT_CALLS_MISSED - 1u)
			c->missed_changed = 1;
	}
}

unsigned dect_calls_missed_unread(const struct dect_calls *c, unsigned tid,
				  unsigned line)
{
	unsigned i, n = 0;

	for (i = 0; tid < DECT_CALLS_TERMINALS && i < c->n[0]; i++)
		n += c->e[0][i].line[tid] == line && c->e[0][i].unread;
	return n;
}

unsigned dect_calls_missed_total(const struct dect_calls *c, unsigned tid,
				 unsigned line)
{
	unsigned i, n = 0;

	for (i = 0; tid < DECT_CALLS_TERMINALS && i < c->n[0]; i++)
		n += c->e[0][i].line[tid] == line;
	return n;
}

size_t dect_calls_save(const struct dect_calls *c, char *out, size_t cap)
{
	size_t at;
	unsigned l, i, k;
	int n;

	n = snprintf(out, cap, "#opendectcell-calls 1\n");
	if (n < 0 || (size_t)n >= cap)
		return 0;
	at = (size_t)n;
	for (l = 0; l < 3u; l++)
		for (i = 0; i < c->n[l]; i++) {
			const struct dect_call *e = &c->e[l][i];
			char lines[2u * DECT_CALLS_TERMINALS + 1u], tz[8];

			for (k = 0; k < DECT_CALLS_TERMINALS; k++)
				snprintf(lines + 2u * k, 3, "%02x", e->line[k]);
			if (e->when.tz_valid)
				snprintf(tz, sizeof(tz), "%d", e->when.tz_quarters);
			else
				snprintf(tz, sizeof(tz), "-");
			n = snprintf(out + at, cap - at,
				     "%u\t%u\t%u\t%u\t%02u%02u%02u%02u%02u%02u\t%s\t%s\t%s\t%s\n",
				     l + 1u, e->id, e->unread, e->count,
				     e->when.year, e->when.month, e->when.day,
				     e->when.hour, e->when.minute, e->when.second,
				     tz, lines, e->number, e->name);
			if (n < 0 || (size_t)n >= cap - at)
				return 0;
			at += (size_t)n;
		}
	return at;
}

/* The next tab-separated field of [*p, end) into out; -1 if none. */
static int field_next(const char **p, const char *end, char *out, size_t cap)
{
	const char *t = *p;
	size_t n;

	if (!t || t > end)
		return -1;
	while (t < end && *t != '\t')
		t++;
	n = (size_t)(t - *p);
	if (n >= cap)
		return -1;
	memcpy(out, *p, n);
	out[n] = '\0';
	*p = t < end ? t + 1 : end + 1;
	return 0;
}

int dect_calls_load(struct dect_calls *c, const char *text, size_t len)
{
	const char *p = text, *end = text + len, *nl;
	unsigned max_id = 0;

	if (len < 17u || memcmp(text, "#opendectcell-calls 1\n", 17))
		return -1;
	dect_calls_init(c);
	for (p = text + 17; p < end; p = nl + 1) {
		char f[8][DECT_CALL_NAME_MAX + 4];
		struct dect_call e;
		unsigned v[4], k, dt[6];
		int ok = 1;

		nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			nl = end;
		for (k = 0; k < 8u && ok; k++)
			ok = !field_next(&p, nl, f[k], sizeof(f[k]));
		if (!ok || sscanf(f[0], "%u", &v[0]) != 1 ||
		    sscanf(f[1], "%u", &v[1]) != 1 ||
		    sscanf(f[2], "%u", &v[2]) != 1 ||
		    sscanf(f[3], "%u", &v[3]) != 1 ||
		    sscanf(f[4], "%2u%2u%2u%2u%2u%2u", &dt[0], &dt[1], &dt[2],
			   &dt[3], &dt[4], &dt[5]) != 6 ||
		    v[0] < DECT_CALLS_MISSED || v[0] > DECT_CALLS_ACCEPTED ||
		    !v[1] || v[1] > 16383u || strlen(f[6]) != 2u * DECT_CALLS_TERMINALS ||
		    c->n[v[0] - 1u] >= DECT_CALLS_MAX)
			continue;
		memset(&e, 0, sizeof(e));
		e.id = (uint16_t)v[1];
		e.unread = v[2] != 0u && v[0] == DECT_CALLS_MISSED;
		e.count = (uint8_t)(v[3] > 255u ? 255u : v[3] ? v[3] : 1u);
		e.when.year = (uint8_t)dt[0];
		e.when.month = (uint8_t)dt[1];
		e.when.day = (uint8_t)dt[2];
		e.when.hour = (uint8_t)dt[3];
		e.when.minute = (uint8_t)dt[4];
		e.when.second = (uint8_t)dt[5];
		if (strcmp(f[5], "-")) {
			e.when.tz_quarters = (int16_t)atoi(f[5]);
			e.when.tz_valid = 1;
		}
		for (k = 0; k < DECT_CALLS_TERMINALS; k++) {
			unsigned b;

			if (sscanf(f[6] + 2u * k, "%2x", &b) != 1)
				ok = 0;
			e.line[k] = (uint8_t)b;
		}
		if (!ok)
			continue;
		digits_of(f[7], e.number, sizeof(e.number));
		/* the name is the rest of the line (it holds no tab anyway),
		 * taken into a terminated copy so text_of() stops at its end */
		{
			char nm[2u * DECT_CALL_NAME_MAX];
			size_t nn = p <= nl ? (size_t)(nl - p) : 0u;

			if (nn > sizeof(nm) - 1u)
				nn = sizeof(nm) - 1u;
			memcpy(nm, p <= nl ? p : "", nn);
			nm[nn] = '\0';
			text_of(nm, e.name, sizeof(e.name));
		}
		c->e[v[0] - 1u][c->n[v[0] - 1u]++] = e;
		if (e.id > max_id)
			max_id = e.id;
	}
	c->next_id = (uint16_t)(max_id >= 16383u ? 1u : max_id + 1u);
	return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base's call lists. See dect_calls.h.
 */

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

unsigned dect_calls_count(const struct dect_calls *c, unsigned list,
			  unsigned tid)
{
	unsigned i, n = 0;

	if (!list_ok(list))
		return 0;
	for (i = 0; i < c->n[list - 1u]; i++)
		n += seen_by(&c->e[list - 1u][i], tid);
	return n;
}

struct dect_call *dect_calls_at(struct dect_calls *c, unsigned list,
				unsigned tid, unsigned idx)
{
	unsigned i;

	if (!list_ok(list) || !idx)
		return NULL;
	for (i = 0; i < c->n[list - 1u]; i++)
		if (seen_by(&c->e[list - 1u][i], tid) && !--idx)
			return &c->e[list - 1u][i];
	return NULL;
}

unsigned dect_calls_index(const struct dect_calls *c, unsigned list,
			  unsigned tid, unsigned id)
{
	unsigned i, idx = 0;

	if (!list_ok(list))
		return 0;
	for (i = 0; i < c->n[list - 1u]; i++) {
		const struct dect_call *e = &c->e[list - 1u][i];

		if (!seen_by(e, tid))
			continue;
		idx++;
		if (e->id == id)
			return idx;
	}
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
	unsigned i;

	if (!list_ok(list))
		return -1;
	for (i = 0; i < c->n[list - 1u]; i++)
		if (c->e[list - 1u][i].id == id) {
			remove_at(c, list - 1u, i);
			if (list == DECT_CALLS_MISSED)
				c->missed_changed = 1;
			return 0;
		}
	return -1;
}

void dect_calls_clear(struct dect_calls *c, unsigned list, unsigned tid)
{
	unsigned i;

	if (!list_ok(list))
		return;
	for (i = 0; i < c->n[list - 1u]; )
		if (seen_by(&c->e[list - 1u][i], tid))
			remove_at(c, list - 1u, i);
		else
			i++;
	if (list == DECT_CALLS_MISSED)
		c->missed_changed = 1;
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

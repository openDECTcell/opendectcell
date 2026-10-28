// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The control socket's line format: docs/bridge-interface.md §2, and
 * dect_ctl.h for the rules this file keeps.
 */
#include "dect_ctl.h"

#include <stdio.h>
#include <string.h>

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

/* Verbs and keys: lower-case letters, digits, '-' and '_'. */
static int is_word(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
	       c == '_';
}

/* Copy a word from p, at most cap-1 characters; returns its length or 0. */
static size_t take_word(const char *p, const char *end, char *dst,
			size_t cap)
{
	size_t n = 0;

	while (p + n < end && is_word(p[n])) {
		if (n + 1 >= cap)
			return 0;
		dst[n] = p[n];
		n++;
	}
	dst[n] = '\0';
	return n;
}

int dect_ctl_parse(const char *line, size_t len, struct dect_ctl_msg *m)
{
	const char *p = line, *end = line + len;
	size_t i;

	memset(m, 0, sizeof(*m));
	if (len && line[len - 1] == '\r')
		end--;
	for (i = 0; line + i < end; i++)
		if ((unsigned char)line[i] < 0x20u && line[i] != '\t')
			return -1;		/* no control characters */
	while (p < end && is_space(*p))
		p++;
	i = take_word(p, end, m->verb, sizeof(m->verb));
	if (!i)
		return -1;
	p += i;
	if (p < end && !is_space(*p))
		return -1;
	for (;;) {
		struct dect_ctl_arg *a;
		size_t n = 0;

		while (p < end && is_space(*p))
			p++;
		if (p == end)
			return 0;
		if (m->n == DECT_CTL_ARGS_MAX)
			return -1;
		a = &m->arg[m->n];
		i = take_word(p, end, a->key, sizeof(a->key));
		if (!i)
			return -1;
		p += i;
		if (p == end || *p != '=')
			return -1;
		p++;
		if (p < end && *p == '"') {
			p++;
			for (;;) {
				char c;

				if (p == end)
					return -1;	/* unterminated */
				c = *p++;
				if (c == '"')
					break;
				if (c == '\\') {
					if (p == end || (*p != '"' && *p != '\\'))
						return -1;
					c = *p++;
				}
				if (n + 1 >= sizeof(a->val))
					return -1;
				a->val[n++] = c;
			}
		} else {
			while (p < end && !is_space(*p)) {
				if (*p == '"' || *p == '\\' ||
				    n + 1 >= sizeof(a->val))
					return -1;
				a->val[n++] = *p++;
			}
		}
		a->val[n] = '\0';
		if (p < end && !is_space(*p))
			return -1;
		m->n++;
	}
}

const char *dect_ctl_get(const struct dect_ctl_msg *m, const char *key)
{
	unsigned i;

	for (i = 0; i < m->n; i++)
		if (!strcmp(m->arg[i].key, key))
			return m->arg[i].val;
	return NULL;
}

int dect_ctl_get_uint(const struct dect_ctl_msg *m, const char *key,
		      unsigned min, unsigned max, unsigned *out)
{
	const char *v = dect_ctl_get(m, key);
	unsigned long x = 0;

	if (!v || !*v || strlen(v) > 9)
		return -1;
	for (; *v; v++) {
		if (*v < '0' || *v > '9')
			return -1;
		x = x * 10u + (unsigned)(*v - '0');
	}
	if (x < min || x > max)
		return -1;
	*out = (unsigned)x;
	return 0;
}

static void put(struct dect_ctl_out *o, const char *s, size_t n)
{
	/* one octet stays free for the newline */
	if (o->overflow || o->len + n + 1 >= sizeof(o->buf)) {
		o->overflow = 1;
		return;
	}
	memcpy(o->buf + o->len, s, n);
	o->len += n;
	o->buf[o->len] = '\0';
}

void dect_ctl_begin(struct dect_ctl_out *o, const char *verb)
{
	o->len = 0;
	o->overflow = 0;
	o->buf[0] = '\0';
	put(o, verb, strlen(verb));
}

void dect_ctl_add(struct dect_ctl_out *o, const char *key, const char *val)
{
	const char *v;
	int quote = !*val;

	for (v = val; *v; v++)
		if (is_space(*v) || *v == '"' || *v == '\\' || *v == '=' ||
		    (unsigned char)*v < 0x20u)
			quote = 1;
	put(o, " ", 1);
	put(o, key, strlen(key));
	put(o, "=", 1);
	if (!quote) {
		put(o, val, strlen(val));
		return;
	}
	put(o, "\"", 1);
	for (v = val; *v; v++) {
		if (*v == '"' || *v == '\\')
			put(o, "\\", 1);
		/* a control character would end or split the line */
		put(o, (unsigned char)*v < 0x20u ? "?" : v, 1);
	}
	put(o, "\"", 1);
}

void dect_ctl_add_uint(struct dect_ctl_out *o, const char *key, unsigned v)
{
	char num[12];

	snprintf(num, sizeof(num), "%u", v);
	dect_ctl_add(o, key, num);
}

int dect_ctl_end(struct dect_ctl_out *o)
{
	if (o->overflow)
		return -1;
	o->buf[o->len++] = '\n';	/* put() kept this octet free */
	o->buf[o->len] = '\0';
	return (int)o->len;
}

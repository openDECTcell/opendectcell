/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Handset profiles: docs/handset-profiles.md, implemented. See dect_profile.h.
 */

#include <stdio.h>
#include <string.h>

#include "dect_profile.h"

/* ---- the key table ------------------------------------------------------ */

enum key_type { T_WORDS, T_INT, T_HEX };

struct key {
	const char *name;
	enum key_type type;
	size_t off;			/* into struct dect_hs_profile */
	const char *words;		/* T_WORDS: "w0|w1|...", value = index */
	unsigned lo, hi;		/* T_INT */
	uint8_t def;
};

#define OFF(f) offsetof(struct dect_hs_profile, f)
#define AOO "auto|on|off"
#define YN "no|yes"

/* §5 of the format page, in its order. The words' order is the value. */
static const struct key keys[] = {
	{ "cc.call_status", T_WORDS, OFF(cc_call_status), AOO, 0, 0, 0 },
	{ "cc.ring_callinfo", T_WORDS, OFF(cc_ring_callinfo),
	  "auto|full|line|none", 0, 0, 0 },
	{ "cc.connect_status_info", T_WORDS, OFF(cc_connect_status_info), AOO,
	  0, 0, 0 },
	{ "cc.basic_service", T_WORDS, OFF(cc_basic_service),
	  "auto|wideband|basic", 0, 0, 0 },
	{ "cc.signal.external", T_INT, OFF(cc_signal_external), NULL,
	  0x40, 0x48, 0x41 },
	{ "cc.signal.internal", T_INT, OFF(cc_signal_internal), NULL,
	  0x40, 0x48, 0x40 },
	{ "cc.bind_before_connect", T_WORDS, OFF(cc_bind_before_connect), YN,
	  0, 0, 0 },
	{ "cc.name.method", T_WORDS, OFF(cc_name_method),
	  "cnip|display|both|none", 0, 0, 0 },
	{ "cc.name.display", T_HEX, OFF(cc_name_display), NULL, 0, 0, 0 },
	{ "cc.name.alphabet", T_WORDS, OFF(cc_name_alphabet), "auto|utf8|dect",
	  0, 0, 0 },
	{ "cc.timer", T_WORDS, OFF(cc_timer), "handset|base", 0, 0, 0 },
	{ "cc.number.method", T_WORDS, OFF(cc_number_method),
	  "clip|display|both", 0, 0, 0 },
	{ "lce.release_after_call", T_WORDS, OFF(lce_release_after_call), YN,
	  0, 0, 1 },
	{ "reg.tries_0000_first", T_WORDS, OFF(reg_tries_0000_first), YN,
	  0, 0, 0 },
	{ "lists.serve", T_WORDS, OFF(lists_serve), YN, 0, 0, 1 },
	{ "line.selection", T_WORDS, OFF(line_selection), "auto|fp|hash",
	  0, 0, 0 },
	{ "notify", T_WORDS, OFF(notify), AOO, 0, 0, 0 },
	{ "trust.terminal_capability", T_WORDS, OFF(trust_terminal_capability),
	  YN, 0, 0, 1 },
	{ "cap.ngdect", T_WORDS, OFF(cap_ngdect), "-|gap|part1|part3",
	  0, 0, 0 },
	{ "feature.call_waiting", T_WORDS, OFF(feature_call_waiting),
	  "off|on", 0, 0, 1 },
	{ "feature.call_lists", T_WORDS, OFF(feature_call_lists), "off|on",
	  0, 0, 1 },
	{ "feature.contacts", T_WORDS, OFF(feature_contacts), "off|on",
	  0, 0, 1 },
};
#define N_KEYS (sizeof(keys) / sizeof(keys[0]))

/* Header keys: not layered, taken from the file itself. */
enum { H_FORMAT = 200, H_ID, H_KIND, H_EXTENDS, H_NAME, H_MATCH, H_SOURCE };

static const struct {
	const char *name;
	int id;
} headers[] = {
	{ "format", H_FORMAT }, { "id", H_ID }, { "kind", H_KIND },
	{ "extends", H_EXTENDS }, { "name", H_NAME },
	{ "match.model", H_MATCH }, { "source", H_SOURCE },
};

/* The index of word w in "a|b|c", or -1. A cap.ngdect "-" is not a word
 * the user may write. */
static int word_index(const char *list, const char *w)
{
	size_t wl = strlen(w);
	int i = 0;

	while (*list) {
		const char *bar = strchr(list, '|');
		size_t n = bar ? (size_t)(bar - list) : strlen(list);

		if (n == wl && !memcmp(list, w, n) && strcmp(w, "-"))
			return i;
		if (!bar)
			break;
		list = bar + 1;
		i++;
	}
	return -1;
}

static int parse_int(const char *v, unsigned *out)
{
	unsigned long x = 0;
	int base = 10, digits = 0;

	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
		base = 16;
		v += 2;
	}
	for (; *v; v++, digits++) {
		int d;

		if (*v >= '0' && *v <= '9')
			d = *v - '0';
		else if (base == 16 && *v >= 'a' && *v <= 'f')
			d = *v - 'a' + 10;
		else if (base == 16 && *v >= 'A' && *v <= 'F')
			d = *v - 'A' + 10;
		else
			return -1;
		x = x * (unsigned)base + (unsigned)d;
		if (x > 0xFFFFu)
			return -1;
	}
	if (!digits)
		return -1;
	*out = (unsigned)x;
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * "hex:..." into octets; with placeholders allowed, {text} and {len} become
 * FFH 01H / FFH 02H and a literal FFH is doubled. Returns the length, -1 on
 * a bad value.
 */
static int parse_hex(const char *v, uint8_t *out, size_t cap, int placeholders)
{
	size_t n = 0;

	if (strncmp(v, "hex:", 4))
		return -1;
	for (v += 4; *v; ) {
		int hi, lo;

		if (*v == ' ' || *v == '\t') {
			v++;
			continue;
		}
		if (placeholders && (!strncmp(v, "{text}", 6) ||
				     !strncmp(v, "{len}", 5))) {
			if (n + 2u > cap)
				return -1;
			out[n++] = 0xFF;
			out[n++] = v[1] == 't' ? 0x01 : 0x02;
			v += v[1] == 't' ? 6 : 5;
			continue;
		}
		hi = hexval(v[0]);
		lo = hi >= 0 ? hexval(v[1]) : -1;
		if (lo < 0)
			return -1;
		if (n + 1u + (placeholders && hi == 15 && lo == 15) > cap)
			return -1;
		out[n++] = (uint8_t)(hi << 4 | lo);
		if (placeholders && out[n - 1u] == 0xFF)
			out[n++] = 0xFF;
		v += 2;
	}
	return (int)n;
}

/* A dotted version into up to four fields; -1 if it is not one. */
static int parse_version(const char *v, uint32_t f[4], uint8_t *n)
{
	*n = 0;
	while (*v) {
		uint32_t x = 0;
		int digits = 0;

		if (*n >= 4u)
			return -1;
		for (; *v >= '0' && *v <= '9'; v++, digits++)
			x = x * 10u + (uint32_t)(*v - '0');
		if (!digits)
			return -1;
		f[(*n)++] = x;
		if (*v == '.')
			v++;
		else if (*v)
			return -1;
	}
	return *n ? 0 : -1;
}

static int fields_cmp(const uint32_t *a, uint8_t na, const uint32_t *b,
		      uint8_t nb)
{
	uint8_t i;

	for (i = 0; i < na || i < nb; i++) {
		uint32_t x = i < na ? a[i] : 0, y = i < nb ? b[i] : 0;

		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

int dect_profile_version_cmp(const char *a, const char *b)
{
	uint32_t fa[4], fb[4];
	uint8_t na, nb;

	if (parse_version(a, fa, &na) || parse_version(b, fb, &nb))
		return strcmp(a, b);
	return fields_cmp(fa, na, fb, nb);
}

/* ---- parsing ------------------------------------------------------------ */

static void say(dect_profile_report r, void *ctx, unsigned line,
		const char *what, const char *key)
{
	char msg[160];

	if (!r)
		return;
	snprintf(msg, sizeof(msg), "%s%s%s", what, key ? ": " : "",
		 key ? key : "");
	r(ctx, line, msg);
}

/* Check a value against its key; 0 if good. */
static int check_value(const struct key *k, const char *v)
{
	uint8_t tmp[DECT_PROFILE_HEX_MAX];
	unsigned x;

	switch (k->type) {
	case T_WORDS:
		return word_index(k->words, v) < 0 ? -1 : 0;
	case T_INT:
		return parse_int(v, &x) || x < k->lo || x > k->hi ? -1 : 0;
	case T_HEX:
		return parse_hex(v, tmp, sizeof(tmp), 1) < 0 ? -1 : 0;
	}
	return -1;
}

/* Unquote a string value in place; -1 if it is not a proper one. */
static int unquote(char *v)
{
	size_t i, o = 0, n = strlen(v);

	if (n < 2u || v[0] != '"' || v[n - 1u] != '"')
		return -1;
	for (i = 1; i + 1u < n; i++) {
		if (v[i] == '\\' && i + 2u < n &&
		    (v[i + 1u] == '"' || v[i + 1u] == '\\'))
			i++;
		else if (v[i] == '"')
			return -1;
		v[o++] = v[i];
	}
	v[o] = '\0';
	return 0;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

/* "[firmware A .. B]" */
static int parse_section(char *s, struct dect_profile_section *sec)
{
	char *a, *dots, *b;

	if (strncmp(s, "[firmware ", 10) || s[strlen(s) - 1] != ']')
		return -1;
	s[strlen(s) - 1] = '\0';
	a = s + 10;
	dots = strstr(a, "..");
	if (!dots)
		return -1;
	*dots = '\0';
	b = trim(dots + 2);
	a = trim(a);
	if (parse_version(a, sec->from, &sec->n_from) ||
	    parse_version(b, sec->to, &sec->n_to) ||
	    fields_cmp(sec->from, sec->n_from, sec->to, sec->n_to) > 0)
		return -1;
	return 0;
}

int dect_profile_parse(const char *text, size_t len, struct dect_profile *p,
		       dect_profile_report report, void *ctx)
{
	char line[256];
	unsigned lineno = 0, have_format = 0, i;
	size_t pos = 0;
	struct dect_profile_section *sec;

	memset(p, 0, sizeof(*p));
	p->n_sec = 1;
	sec = &p->sec[0];
	while (pos < len) {
		size_t e = pos, n;
		char *s, *eq, *key, *val, *hash;
		int quoted = 0;

		while (e < len && text[e] != '\n')
			e++;
		n = e - pos;
		lineno++;
		if (n >= sizeof(line)) {
			say(report, ctx, lineno, "line too long", NULL);
			return -1;
		}
		memcpy(line, text + pos, n);
		line[n] = '\0';
		pos = e < len ? e + 1u : e;

		/* a comment starts at a '#' outside quotes */
		for (hash = line; *hash; hash++) {
			if (*hash == '"')
				quoted = !quoted;
			else if (*hash == '#' && !quoted) {
				*hash = '\0';
				break;
			}
		}
		s = trim(line);
		if (!*s)
			continue;
		if (*s == '[') {
			if (p->n_sec >= DECT_PROFILE_SECTIONS) {
				say(report, ctx, lineno, "too many sections",
				    NULL);
				return -1;
			}
			sec = &p->sec[p->n_sec];
			memset(sec, 0, sizeof(*sec));
			if (parse_section(s, sec)) {
				say(report, ctx, lineno,
				    "not a [firmware A .. B] section", NULL);
				return -1;
			}
			p->n_sec++;
			continue;
		}
		eq = strchr(s, '=');
		if (!eq) {
			say(report, ctx, lineno, "not key = value", NULL);
			return -1;
		}
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);

		for (i = 0; i < sizeof(headers) / sizeof(headers[0]); i++)
			if (!strcmp(key, headers[i].name))
				break;
		if (i < sizeof(headers) / sizeof(headers[0])) {
			int h = headers[i].id;

			if (sec != &p->sec[0]) {
				say(report, ctx, lineno,
				    "header key inside a section", key);
				return -1;
			}
			switch (h) {
			case H_FORMAT:
				if (strcmp(val, "1")) {
					say(report, ctx, lineno,
					    "unknown format", val);
					return -1;
				}
				have_format = 1;
				break;
			case H_ID:
			case H_EXTENDS: {
				char *dst = h == H_ID ? p->id : p->extends;
				size_t k;

				if (*dst || !*val ||
				    strlen(val) >= DECT_PROFILE_ID_MAX)
					goto bad_header;
				for (k = 0; val[k]; k++)
					if (!((val[k] >= 'a' && val[k] <= 'z') ||
					      (val[k] >= '0' && val[k] <= '9') ||
					      val[k] == '-'))
						goto bad_header;
				strcpy(dst, val);
				break;
			}
			case H_KIND:
				if (p->kind)
					goto bad_header;
				if (!strcmp(val, "manufacturer"))
					p->kind = DECT_PROFILE_MANUFACTURER;
				else if (!strcmp(val, "device"))
					p->kind = DECT_PROFILE_DEVICE;
				else
					goto bad_header;
				break;
			case H_NAME:
			case H_SOURCE: {
				char *dst = h == H_NAME ? p->name : p->source;

				if (*dst || unquote(val) || !*val ||
				    strlen(val) >= DECT_PROFILE_STR_MAX)
					goto bad_header;
				strcpy(dst, val);
				break;
			}
			case H_MATCH: {
				int m = parse_hex(val, p->match_model,
						  sizeof(p->match_model), 0);

				if (m <= 0 || p->match_model_len)
					goto bad_header;
				p->match_model_len = (uint8_t)m;
				break;
			}
			}
			continue;
bad_header:
			say(report, ctx, lineno, "bad or repeated header key",
			    key);
			return -1;
		}

		for (i = 0; i < N_KEYS; i++)
			if (!strcmp(key, keys[i].name))
				break;
		if (i == N_KEYS) {
			say(report, ctx, lineno, "unknown key, ignored", key);
			continue;
		}
		{
			unsigned k;

			for (k = 0; k < sec->n_kv; k++)
				if (sec->kv[k].key == i) {
					say(report, ctx, lineno,
					    "repeated key", key);
					return -1;
				}
		}
		if (strlen(val) >= DECT_PROFILE_STR_MAX ||
		    check_value(&keys[i], val)) {
			say(report, ctx, lineno, "bad value, key ignored", key);
			continue;
		}
		if (sec->n_kv >= DECT_PROFILE_KEYS) {
			say(report, ctx, lineno, "too many keys", NULL);
			return -1;
		}
		sec->kv[sec->n_kv].key = (uint8_t)i;
		sec->kv[sec->n_kv].line = (uint16_t)lineno;
		strcpy(sec->kv[sec->n_kv].val, val);
		sec->n_kv++;
	}

	if (!have_format || !p->id[0] || !p->kind || !p->name[0] ||
	    !p->source[0]) {
		say(report, ctx, 0, "a required header key is missing "
		    "(format, id, kind, name, source)", NULL);
		return -1;
	}
	if (p->kind == DECT_PROFILE_DEVICE && !p->extends[0]) {
		say(report, ctx, 0, "a device profile needs extends", NULL);
		return -1;
	}
	if (p->kind == DECT_PROFILE_MANUFACTURER &&
	    (p->extends[0] || p->n_sec > 1u)) {
		say(report, ctx, 0, "a manufacturer profile has no extends "
		    "and no firmware sections", NULL);
		return -1;
	}
	return 0;
}

/* ---- resolution --------------------------------------------------------- */

void dect_profile_defaults(struct dect_hs_profile *hp)
{
	size_t i;

	memset(hp, 0, sizeof(*hp));
	for (i = 0; i < N_KEYS; i++)
		if (keys[i].type != T_HEX)
			((uint8_t *)hp)[keys[i].off] = keys[i].def;
	hp->cc_name_display[0] = 0xFF;		/* {text} */
	hp->cc_name_display[1] = 0x01;
	hp->cc_name_display_len = 2;
}

static void apply(const struct dect_profile_section *sec,
		  struct dect_hs_profile *out)
{
	unsigned i;

	for (i = 0; i < sec->n_kv; i++) {
		const struct key *k = &keys[sec->kv[i].key];
		const char *v = sec->kv[i].val;
		unsigned x;

		switch (k->type) {
		case T_WORDS:
			((uint8_t *)out)[k->off] =
				(uint8_t)word_index(k->words, v);
			break;
		case T_INT:
			if (!parse_int(v, &x))
				((uint8_t *)out)[k->off] = (uint8_t)x;
			break;
		case T_HEX: {
			int n = parse_hex(v, out->cc_name_display,
					  sizeof(out->cc_name_display), 1);

			out->cc_name_display_len = n > 0 ? (uint8_t)n : 0;
			break;
		}
		}
	}
}

void dect_profile_resolve(const struct dect_profile *mfr,
			  const struct dect_profile *dev, const char *fw,
			  struct dect_hs_profile *out)
{
	uint32_t f[4];
	uint8_t nf = 0;
	unsigned s;
	int have_fw = fw && *fw && !parse_version(fw, f, &nf);

	dect_profile_defaults(out);
	if (mfr)
		apply(&mfr->sec[0], out);
	if (!dev)
		return;
	apply(&dev->sec[0], out);
	for (s = 1; have_fw && s < dev->n_sec; s++) {
		const struct dect_profile_section *sec = &dev->sec[s];

		if (fields_cmp(f, nf, sec->from, sec->n_from) >= 0 &&
		    fields_cmp(f, nf, sec->to, sec->n_to) <= 0)
			apply(sec, out);
	}
}

size_t dect_profile_describe(const struct dect_hs_profile *hp, char *out,
			     size_t cap)
{
	struct dect_hs_profile def;
	size_t len = 0, i;

	if (!cap)
		return 0;
	out[0] = '\0';
	dect_profile_defaults(&def);
	for (i = 0; i < N_KEYS; i++) {
		const struct key *k = &keys[i];
		uint8_t v = ((const uint8_t *)hp)[k->off];
		char item[96];
		int n;

		if (k->type == T_HEX) {
			if (hp->cc_name_display_len == def.cc_name_display_len &&
			    !memcmp(hp->cc_name_display, def.cc_name_display,
				    def.cc_name_display_len))
				continue;
			n = snprintf(item, sizeof(item), "%s=(%u octets)",
				     k->name, hp->cc_name_display_len);
		} else if (v == ((const uint8_t *)&def)[k->off]) {
			continue;
		} else if (k->type == T_INT) {
			n = snprintf(item, sizeof(item), "%s=0x%02x", k->name, v);
		} else {
			const char *w = k->words;
			unsigned j;

			for (j = 0; j < v && strchr(w, '|'); j++)
				w = strchr(w, '|') + 1;
			n = snprintf(item, sizeof(item), "%s=%.*s", k->name,
				     (int)(strchr(w, '|') ? (size_t)(strchr(w, '|') - w)
							  : strlen(w)), w);
		}
		if (n < 0 || len + (size_t)n + 2u > cap)
			break;
		len += (size_t)snprintf(out + len, cap - len, "%s%s",
					len ? " " : "", item);
	}
	return len;
}

size_t dect_profile_display(const struct dect_hs_profile *hp, const char *text,
			    uint8_t *out, size_t cap)
{
	size_t tl = strlen(text), n = 2, i;

	if (tl > 255u || cap < 2u)
		return 0;
	for (i = 0; i < hp->cc_name_display_len; i++) {
		uint8_t c = hp->cc_name_display[i];

		if (c == 0xFF && i + 1u < hp->cc_name_display_len) {
			uint8_t m = hp->cc_name_display[++i];

			if (m == 0x01) {
				if (n + tl > cap)
					return 0;
				memcpy(out + n, text, tl);
				n += tl;
				continue;
			}
			if (m == 0x02)
				c = (uint8_t)tl;
		}
		if (n + 1u > cap)
			return 0;
		out[n++] = c;
	}
	if (n - 2u > 255u)
		return 0;
	out[0] = 0x28;				/* <<MULTI-DISPLAY>> */
	out[1] = (uint8_t)(n - 2u);
	return n;
}

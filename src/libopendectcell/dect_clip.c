// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CALLING-PARTY-NUMBER and CALLING-PARTY-NAME. See dect_clip.h for the
 * interface and for why this module omits rather than guesses.
 */
#include "dect_clip.h"
#include "dect_nwk.h"

#include <string.h>

/*
 * Walk one UTF-8 code point starting at s[0], with `avail` bytes left.
 * Returns its length in bytes, or 0 if the sequence is malformed, truncated,
 * overlong, a surrogate, or beyond U+10FFFF.
 */
static size_t utf8_step(const unsigned char *s, size_t avail)
{
	unsigned int cp;
	size_t need, i;

	if (avail == 0)
		return 0;
	if (s[0] < 0x80)
		return 1;
	if (s[0] < 0xC2)		/* continuation byte, or overlong C0/C1 */
		return 0;
	if (s[0] < 0xE0) {
		need = 2;
		cp = s[0] & 0x1Fu;
	} else if (s[0] < 0xF0) {
		need = 3;
		cp = s[0] & 0x0Fu;
	} else if (s[0] < 0xF5) {
		need = 4;
		cp = s[0] & 0x07u;
	} else {
		return 0;
	}
	if (avail < need)
		return 0;
	for (i = 1; i < need; i++) {
		if ((s[i] & 0xC0u) != 0x80u)
			return 0;
		cp = (cp << 6) | (s[i] & 0x3Fu);
	}
	if (need == 3 && cp < 0x800u)
		return 0;
	if (need == 4 && cp < 0x10000u)
		return 0;
	if (cp > 0x10FFFFu)
		return 0;
	if (cp >= 0xD800u && cp <= 0xDFFFu)	/* surrogate half */
		return 0;
	return need;
}

size_t dect_clip_utf8_trunc(const char *s, size_t max_bytes)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t len = strlen(s), off = 0;

	while (off < len) {
		size_t step = utf8_step(p + off, len - off);

		if (step == 0 || off + step > max_bytes)
			break;
		off += step;
	}
	return off;
}

/* Every code point is well formed and none is a C0 control or DEL. */
static int utf8_name_ok(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t off = 0;

	while (off < len) {
		size_t step = utf8_step(p + off, len - off);

		if (step == 0)
			return 0;
		if (step == 1 && (p[off] < 0x20u || p[off] == 0x7Fu))
			return 0;
		off += step;
	}
	return 1;
}

static int ascii_printable(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if ((unsigned char)s[i] < 0x20u || (unsigned char)s[i] > 0x7Eu)
			return 0;
	return 1;
}

void dect_clip_init(struct dect_clip *id)
{
	memset(id, 0, sizeof(*id));
	id->number_presentation = DECT_CLIP_PRES_UNSET;
	id->name_presentation = DECT_CLIP_PRES_UNSET;
	id->name_alphabet = DECT_CLIP_ALPHABET_UTF8;
}

int dect_clip_set_number(struct dect_clip *id, const char *number,
			 enum dect_clip_ton ton, enum dect_clip_npi npi,
			 enum dect_clip_presentation pres,
			 enum dect_clip_screening scr)
{
	size_t len = number ? strlen(number) : 0;

	if (len == 0 || len > DECT_CLIP_NUMBER_MAX)
		return -1;
	if (!ascii_printable(number, len))
		return -1;
	if ((unsigned)ton > 7u || (unsigned)npi > 15u)
		return -1;
	if ((unsigned)pres > 3u && pres != DECT_CLIP_PRES_UNSET)
		return -1;
	if ((unsigned)scr > 3u)
		return -1;

	memcpy(id->number, number, len);
	id->number[len] = '\0';
	id->number_ton = (uint8_t)ton;
	id->number_npi = (uint8_t)npi;
	id->number_presentation = (uint8_t)pres;
	id->number_screening = (uint8_t)scr;
	return 0;
}

int dect_clip_set_name(struct dect_clip *id, const char *name,
		       enum dect_clip_alphabet alphabet,
		       enum dect_clip_presentation pres,
		       enum dect_clip_screening scr)
{
	size_t len = name ? strlen(name) : 0;

	if (len == 0 || len > DECT_CLIP_NAME_MAX)
		return -1;
	if ((unsigned)pres > 3u && pres != DECT_CLIP_PRES_UNSET)
		return -1;
	if ((unsigned)scr > 3u)
		return -1;

	switch (alphabet) {
	case DECT_CLIP_ALPHABET_UTF8:
		if (!utf8_name_ok(name, len))
			return -1;
		break;
	case DECT_CLIP_ALPHABET_DECT:
	case DECT_CLIP_ALPHABET_NETWORK:
		/* We do not transliterate: a caller that declares a non-UTF-8
		 * alphabet must hand us text that is already in it, and the
		 * only subset we can check is printable ASCII. */
		if (!ascii_printable(name, len))
			return -1;
		break;
	default:
		return -1;
	}

	memcpy(id->name, name, len);
	id->name[len] = '\0';
	id->name_alphabet = (uint8_t)alphabet;
	id->name_presentation = (uint8_t)pres;
	id->name_screening = (uint8_t)scr;
	return 0;
}

size_t dect_clip_put_number(uint8_t *out, size_t cap, size_t off,
			    const struct dect_clip *id)
{
	uint8_t body[2 + DECT_CLIP_NUMBER_MAX];
	size_t n = strlen(id->number);

	if (n == 0)
		return off;
	if (id->number_presentation != DECT_CLIP_PRES_ALLOWED)
		return off;	/* withheld, unavailable or simply not stated */
	if (n > DECT_CLIP_NUMBER_MAX)
		return off;

	/* Number descriptor: extension bit clear, so a second one follows. */
	body[0] = (uint8_t)(((id->number_ton & 0x07u) << 4)
			    | (id->number_npi & 0x0Fu));
	/* Presentation/screening descriptor: extension bit set, bits 5-3 zero. */
	body[1] = (uint8_t)(0x80u
			    | ((id->number_presentation & 0x03u) << 5)
			    | (id->number_screening & 0x03u));
	memcpy(body + 2, id->number, n);

	return dect_nwk_put_ie(out, cap, off, DECT_IE_CALLING_PARTY_NUMBER,
			       body, n + 2);
}

size_t dect_clip_put_name(uint8_t *out, size_t cap, size_t off,
			  const struct dect_clip *id, size_t budget)
{
	uint8_t body[1 + DECT_CLIP_NAME_MAX];
	size_t n = strlen(id->name);

	if (n == 0)
		return off;
	if (id->name_presentation != DECT_CLIP_PRES_ALLOWED)
		return off;
	if (budget == 0 || budget > DECT_CLIP_NAME_MAX)
		budget = DECT_CLIP_NAME_MAX;

	if (id->name_alphabet == DECT_CLIP_ALPHABET_UTF8)
		n = dect_clip_utf8_trunc(id->name, budget);
	else if (n > budget)
		n = budget;
	if (n == 0)		/* the budget could not hold one character */
		return off;

	body[0] = (uint8_t)(((id->name_presentation & 0x03u) << 5)
			    | ((id->name_alphabet & 0x07u) << 2)
			    | (id->name_screening & 0x03u));
	memcpy(body + 1, id->name, n);

	return dect_nwk_put_ie(out, cap, off, DECT_IE_CALLING_PARTY_NAME,
			       body, n + 1);
}

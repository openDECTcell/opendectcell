/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Handset profiles: the parser, the key table and the resolution of the
 * layers. The format is docs/handset-profiles.md, which this file
 * implements and must not outgrow: a key added here is added there first.
 *
 * Quirks are data, features are code: a profile only sets the values of
 * struct dect_hs_profile below; what each value does is in the code that
 * reads it. No I/O here -- the caller reads the file and hands over the
 * text, and receives every complaint through a report callback.
 */
#ifndef OPENDECTCELL_DECT_PROFILE_H
#define OPENDECTCELL_DECT_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define DECT_PROFILE_ID_MAX	32u
#define DECT_PROFILE_STR_MAX	128u
#define DECT_PROFILE_KEYS	32u	/* per section */
#define DECT_PROFILE_SECTIONS	8u	/* the base plus firmware ranges */
#define DECT_PROFILE_HEX_MAX	48u

enum dect_profile_kind {
	DECT_PROFILE_NONE,
	DECT_PROFILE_MANUFACTURER,
	DECT_PROFILE_DEVICE,
};

/* Tri-state for the auto/on/off keys. */
enum { DECT_PROF_AUTO = 0, DECT_PROF_ON = 1, DECT_PROF_OFF = 2 };

/*
 * The resolved values for one handset: the defaults of the format page,
 * overlaid by the manufacturer file, the device file and its matching
 * firmware sections. Every field is one octet so the key table can address
 * them alike; the meaning of each is in docs/handset-profiles.md §5.
 */
struct dect_hs_profile {
	uint8_t cc_call_status;		/* AUTO / ON / OFF */
	uint8_t cc_ring_callinfo;	/* 0 auto, 1 full, 2 line, 3 none */
	uint8_t cc_connect_status_info;	/* AUTO / ON / OFF */
	uint8_t cc_basic_service;	/* 0 auto, 1 wideband, 2 basic */
	uint8_t cc_signal_external;	/* 0x40..0x48 */
	uint8_t cc_signal_internal;
	uint8_t cc_bind_before_connect;	/* 0 no, 1 yes */
	uint8_t cc_name_method;		/* 0 cnip, 1 display, 2 both, 3 none */
	uint8_t cc_name_alphabet;	/* 0 auto, 1 utf8, 2 dect */
	uint8_t cc_timer;		/* 0 handset, 1 base */
	uint8_t cc_number_method;	/* 0 clip, 1 display, 2 both */
	uint8_t lce_release_after_call;	/* 0 no, 1 yes */
	uint8_t reg_tries_0000_first;
	uint8_t lists_serve;
	uint8_t line_selection;		/* 0 auto, 1 fp, 2 hash */
	uint8_t notify;			/* AUTO / ON / OFF */
	uint8_t trust_terminal_capability;
	uint8_t cap_ngdect;		/* 0 from the handset, 1 gap, 2 part1,
					 * 3 part3 */
	uint8_t feature_call_waiting;
	uint8_t feature_call_lists;
	uint8_t feature_contacts;
	/*
	 * cc.name.display: the contents of <<MULTI-DISPLAY>>, with {text}
	 * stored as FFH 01H, {len} as FFH 02H and a literal FFH as FFH FFH.
	 * dect_profile_display() expands it.
	 */
	uint8_t cc_name_display[DECT_PROFILE_HEX_MAX];
	uint8_t cc_name_display_len;
};

/* One key = value as read, kept for the layering. */
struct dect_profile_kv {
	uint8_t  key;			/* index into the key table */
	uint16_t line;
	char     val[DECT_PROFILE_STR_MAX];
};

struct dect_profile_section {
	uint32_t from[4], to[4];	/* firmware range; unused in section 0 */
	uint8_t  n_from, n_to;
	struct dect_profile_kv kv[DECT_PROFILE_KEYS];
	unsigned n_kv;
};

/* One parsed file. */
struct dect_profile {
	enum dect_profile_kind kind;
	char id[DECT_PROFILE_ID_MAX];
	char extends[DECT_PROFILE_ID_MAX];
	char name[DECT_PROFILE_STR_MAX];
	char source[DECT_PROFILE_STR_MAX];
	uint8_t match_model[DECT_PROFILE_HEX_MAX];
	uint8_t match_model_len;
	struct dect_profile_section sec[DECT_PROFILE_SECTIONS];
	unsigned n_sec;			/* 1 + firmware sections */
};

/* line 0 = the file as a whole */
typedef void (*dect_profile_report)(void *ctx, unsigned line, const char *msg);

/* The standard defaults (docs/handset-profiles.md §5). */
void dect_profile_defaults(struct dect_hs_profile *hp);

/*
 * Parse one file's text. Unknown keys and bad values are reported and
 * skipped; a syntax error, a repeated key or a missing required header key
 * rejects the file (-1). 0 on success.
 */
int dect_profile_parse(const char *text, size_t len, struct dect_profile *p,
		       dect_profile_report report, void *ctx);

/*
 * Resolve: defaults, then mfr (may be NULL), then dev (may be NULL), then
 * dev's firmware sections whose range holds fw (NULL or "" = none). Values
 * were checked by the parser, so this cannot fail.
 */
void dect_profile_resolve(const struct dect_profile *mfr,
			  const struct dect_profile *dev, const char *fw,
			  struct dect_hs_profile *out);

/*
 * The keys whose resolved value differs from the default, as
 * "key=value" items separated by spaces, for the log line. Returns the
 * length written (NUL-terminated), 0 if none differ.
 */
size_t dect_profile_describe(const struct dect_hs_profile *hp, char *out,
			     size_t cap);

/*
 * The <<MULTI-DISPLAY>> element for text, from cc.name.display: 28H, the
 * length, the expanded contents. 0 if it does not fit or text is longer
 * than 255 octets.
 */
size_t dect_profile_display(const struct dect_hs_profile *hp, const char *text,
			    uint8_t *out, size_t cap);

/*
 * The key table for a settings page, as a JSON array of
 * {"key","group","label","type":"words"|"int"|"hex", "values" (words),
 * "min"/"max" (int), "default"}. Returns the length, 0 if cap is short.
 */
size_t dect_profile_keys_json(char *out, size_t cap);

/*
 * Apply a handset's own choices on top of its resolved profile: lines of
 * "key = value", the behaviour keys of §5 only (no header, no sections),
 * checked as a profile's are. 0, or -1 with the reason reported (line
 * numbers are the text's own); hp is then untouched.
 */
int dect_profile_override(const char *text, size_t len,
			  struct dect_hs_profile *hp,
			  dect_profile_report report, void *ctx);

/* Dotted version compare, field by field as integers: <0, 0, >0. */
int dect_profile_version_cmp(const char *a, const char *b);

#endif

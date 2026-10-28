/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The control socket's line format (docs/bridge-interface.md §2): one
 * message per line, a verb, then key=value pairs; a value with spaces or
 * quotes goes in double quotes with \" and \\ escaped. Keys may repeat
 * (`lines line=1 ... line=2 ...`).
 *
 * Parsing and building only, no I/O. Control characters are refused on
 * input and replaced on output, so a handset name can never end a line
 * early and smuggle in a second message.
 */
#ifndef OPENDECTCELL_DECT_CTL_H
#define OPENDECTCELL_DECT_CTL_H

#include <stddef.h>

#define DECT_CTL_LINE_MAX	512u	/* with the newline */
#define DECT_CTL_ARGS_MAX	16u
#define DECT_CTL_KEY_MAX	24u	/* with the terminator */
#define DECT_CTL_VAL_MAX	128u

struct dect_ctl_arg {
	char key[DECT_CTL_KEY_MAX];
	char val[DECT_CTL_VAL_MAX];
};

struct dect_ctl_msg {
	char verb[DECT_CTL_KEY_MAX];
	struct dect_ctl_arg arg[DECT_CTL_ARGS_MAX];
	unsigned n;
};

/*
 * Parse one line (without its newline; a trailing "\r" is allowed).
 * Returns 0, or -1 when the line is empty, malformed or over a limit;
 * *m is then unusable.
 */
int dect_ctl_parse(const char *line, size_t len, struct dect_ctl_msg *m);

/* The value of the first `key`, or NULL. */
const char *dect_ctl_get(const struct dect_ctl_msg *m, const char *key);

/*
 * A decimal value of `key` in [min, max]: 0 and *out, or -1 when it is
 * absent, not a plain decimal, or out of range.
 */
int dect_ctl_get_uint(const struct dect_ctl_msg *m, const char *key,
		      unsigned min, unsigned max, unsigned *out);

/* Building a line: begin, add pairs, end. */
struct dect_ctl_out {
	char   buf[DECT_CTL_LINE_MAX];
	size_t len;
	int    overflow;
};

void dect_ctl_begin(struct dect_ctl_out *o, const char *verb);
void dect_ctl_add(struct dect_ctl_out *o, const char *key, const char *val);
void dect_ctl_add_uint(struct dect_ctl_out *o, const char *key, unsigned v);

/* Appends the newline. Returns the length, or -1 if the line overflowed. */
int dect_ctl_end(struct dect_ctl_out *o);

#endif

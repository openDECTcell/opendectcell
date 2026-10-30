/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The base station above the links. See dect_base.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dect_base.h"

void dect_base_init(struct dect_base *b)
{
	memset(b, 0, sizeof(*b));
}

void dect_base_changed(struct dect_base *b)
{
	if (b->ops && b->ops->save)
		b->ops->save(b->ctx, b->cur, &b->rec);
}

void dect_base_stash(struct dect_base *b)
{
	if (b->cur < DECT_BASE_HS_MAX)
		b->hs[b->cur] = b->rec;
}

void dect_base_load(struct dect_base *b, unsigned i)
{
	if (i < DECT_BASE_HS_MAX)
		b->rec = b->hs[i];
}

int dect_base_switch(struct dect_base *b, unsigned i)
{
	if (i == b->cur || i >= b->n_hs)
		return 0;
	dect_base_stash(b);
	dect_base_load(b, i);
	b->cur = i;
	return 1;
}

int dect_base_send(struct dect_base *b, const struct dect_mail *m)
{
	return b->ops && b->ops->send ? b->ops->send(b->ctx, m) : -1;
}

void dect_base_random(struct dect_base *b, uint8_t *out, size_t n)
{
	if (b->ops && b->ops->random)
		b->ops->random(b->ctx, out, n);
	else
		memset(out, 0, n);
}

void dect_base_log(struct dect_base *b, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	if (!b->ops || !b->ops->log)
		return;
	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	b->ops->log(b->ctx, line);
}

// SPDX-License-Identifier: GPL-2.0-or-later
/* The SIP peers an account that does not register may be called from. See peers.h. */
#define _POSIX_C_SOURCE 200809L
#include "peers.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

int dect_peers_add(struct dect_peers *l, const char *text)
{
	char buf[64];
	char *slash, *end;
	struct dect_peer *p;
	unsigned long prefix;

	if (!text || !*text || strlen(text) >= sizeof(buf) || l->n >= DECT_PEERS_MAX)
		return -1;
	strcpy(buf, text);
	p = &l->p[l->n];
	memset(p, 0, sizeof(*p));
	slash = strchr(buf, '/');
	if (slash)
		*slash = '\0';
	if (inet_pton(AF_INET, buf, p->addr) == 1) {
		p->af = 4;
		p->prefix = 32;
	} else if (inet_pton(AF_INET6, buf, p->addr) == 1) {
		p->af = 6;
		p->prefix = 128;
	} else {
		return -1;
	}
	if (slash) {
		if (!slash[1])
			return -1;
		prefix = strtoul(slash + 1, &end, 10);
		if (*end || prefix > p->prefix)
			return -1;
		p->prefix = (unsigned)prefix;
	}
	l->n++;
	return 0;
}

int dect_peers_parse(struct dect_peers *l, const char *csv)
{
	char buf[512];
	char *tok, *save = NULL;

	if (!csv || strlen(csv) >= sizeof(buf))
		return -1;
	strcpy(buf, csv);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		if (dect_peers_add(l, tok))
			return -1;
	return 0;
}

static int in_prefix(const uint8_t *a, const uint8_t *b, unsigned bits)
{
	unsigned whole = bits / 8u, rest = bits % 8u;

	if (memcmp(a, b, whole))
		return 0;
	if (rest) {
		uint8_t mask = (uint8_t)(0xFFu << (8u - rest));

		return (a[whole] & mask) == (b[whole] & mask);
	}
	return 1;
}

int dect_peers_match(const struct dect_peers *l, int af, const uint8_t *addr)
{
	static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
	unsigned i;

	if (af == 6 && !memcmp(addr, mapped, sizeof(mapped))) {
		af = 4;
		addr += 12;
	}
	for (i = 0; i < l->n; i++)
		if (l->p[i].af == af && in_prefix(l->p[i].addr, addr, l->p[i].prefix))
			return 1;
	return 0;
}

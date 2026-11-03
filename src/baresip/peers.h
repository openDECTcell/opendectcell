/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SIP peers an account that does not register may be called from.
 *
 * An account that registers takes an INVITE only from where its REGISTER was answered (baresip's own
 * origin check). An account that does not register has no such address, so it takes an INVITE only
 * from an address the administrator named (`list sip_peer` in /etc/config/opendectcell). This file is the
 * list and its match; it needs no baresip header, so it is tested on the PC.
 */
#ifndef OPENDECTCELL_PEERS_H
#define OPENDECTCELL_PEERS_H

#include <stddef.h>
#include <stdint.h>

#define DECT_PEERS_MAX 16

struct dect_peer {
	int      af;		/* 4 or 6 */
	uint8_t  addr[16];	/* network order, the prefix's bits only matter */
	unsigned prefix;	/* bits, 0..32 (v4) or 0..128 (v6) */
};

struct dect_peers {
	struct dect_peer p[DECT_PEERS_MAX];
	unsigned         n;
};

/* Add "a.b.c.d", "a.b.c.d/nn", "x::y" or "x::y/nn". 0, or -1 for text that is not an address, a prefix
 * out of range, or a full list. */
int dect_peers_add(struct dect_peers *l, const char *text);

/* Add a comma-separated list of the above; -1 if any entry is bad (the good ones before it are kept). */
int dect_peers_parse(struct dect_peers *l, const char *csv);

/* Is the address (4 or 16 octets in network order, af 4 or 6) inside one of the entries? An IPv4-mapped
 * IPv6 address is taken as the IPv4 one. */
int dect_peers_match(const struct dect_peers *l, int af, const uint8_t *addr);

#endif

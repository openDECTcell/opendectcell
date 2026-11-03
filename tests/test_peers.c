// SPDX-License-Identifier: GPL-2.0-or-later
/* Tests for src/baresip/peers.c: the list of peers a non-registering SIP account may be called from. */
#include "check.h"
#include "../src/baresip/peers.h"

#include <arpa/inet.h>
#include <string.h>

static int match4(const struct dect_peers *l, const char *a)
{
	uint8_t b[4];

	CHECK_EQ(inet_pton(AF_INET, a, b), 1);
	return dect_peers_match(l, 4, b);
}

static int match6(const struct dect_peers *l, const char *a)
{
	uint8_t b[16];

	CHECK_EQ(inet_pton(AF_INET6, a, b), 1);
	return dect_peers_match(l, 6, b);
}

int main(void)
{
	struct dect_peers l;

	memset(&l, 0, sizeof(l));
	/* an empty list matches nothing: a non-registering account is closed by default */
	CHECK_EQ(match4(&l, "192.0.2.5"), 0);
	CHECK_EQ(match6(&l, "2001:db8::1"), 0);

	CHECK_EQ(dect_peers_parse(&l, "192.0.2.5,198.51.100.0/24,2001:db8::/32"), 0);
	CHECK_EQ(l.n, 3u);
	CHECK_EQ(match4(&l, "192.0.2.5"), 1);
	CHECK_EQ(match4(&l, "192.0.2.6"), 0);		/* a host entry is exact */
	CHECK_EQ(match4(&l, "198.51.100.77"), 1);	/* a /24 */
	CHECK_EQ(match4(&l, "198.51.101.1"), 0);
	CHECK_EQ(match6(&l, "2001:db8:abcd::1"), 1);
	CHECK_EQ(match6(&l, "2001:db9::1"), 0);
	CHECK_EQ(match6(&l, "::ffff:192.0.2.5"), 1);	/* an IPv4-mapped address is the IPv4 one */
	CHECK_EQ(match6(&l, "::ffff:192.0.2.6"), 0);

	/* prefixes that are not a whole octet */
	memset(&l, 0, sizeof(l));
	CHECK_EQ(dect_peers_add(&l, "10.8.0.0/13"), 0);
	CHECK_EQ(match4(&l, "10.15.255.255"), 1);
	CHECK_EQ(match4(&l, "10.16.0.0"), 0);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(dect_peers_add(&l, "0.0.0.0/0"), 0);	/* anything: the administrator's explicit choice */
	CHECK_EQ(match4(&l, "203.0.113.9"), 1);

	/* bad text is refused, and refused entries are not added */
	memset(&l, 0, sizeof(l));
	CHECK_EQ(dect_peers_add(&l, ""), -1);
	CHECK_EQ(dect_peers_add(&l, "example.org"), -1);
	CHECK_EQ(dect_peers_add(&l, "192.0.2.5/33"), -1);
	CHECK_EQ(dect_peers_add(&l, "192.0.2.5/"), -1);
	CHECK_EQ(dect_peers_add(&l, "192.0.2.5/x"), -1);
	CHECK_EQ(dect_peers_add(&l, "2001:db8::/129"), -1);
	CHECK_EQ(dect_peers_parse(&l, "192.0.2.5,nonsense"), -1);
	CHECK_EQ(l.n, 1u);				/* the good one before it stays */
	{
		char big[600];

		memset(big, '1', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		CHECK_EQ(dect_peers_parse(&l, big), -1);	/* too long for the buffer */
	}
	/* the list is bounded */
	memset(&l, 0, sizeof(l));
	{
		unsigned i;
		char a[32];

		for (i = 0; i < DECT_PEERS_MAX; i++) {
			snprintf(a, sizeof(a), "192.0.2.%u", i + 1u);
			CHECK_EQ(dect_peers_add(&l, a), 0);
		}
		CHECK_EQ(dect_peers_add(&l, "192.0.2.200"), -1);
	}
	return CHECK_DONE();
}

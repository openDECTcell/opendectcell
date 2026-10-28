// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_ctl.c: the control socket's line format
 * (docs/bridge-interface.md §2). No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_ctl.h"

#include <string.h>

static int parse(const char *s, struct dect_ctl_msg *m)
{
	return dect_ctl_parse(s, strlen(s), m);
}

static void test_parse_plain(void)
{
	struct dect_ctl_msg m;
	unsigned v = 0;

	CHECK_EQ(parse("deregister hs=3", &m), 0);
	CHECK(!strcmp(m.verb, "deregister"));
	CHECK_EQ(m.n, 1);
	CHECK(!strcmp(dect_ctl_get(&m, "hs"), "3"));
	CHECK_EQ(dect_ctl_get_uint(&m, "hs", 1, 6, &v), 0);
	CHECK_EQ(v, 3);
	CHECK(dect_ctl_get(&m, "line") == NULL);

	/* a bare verb, spaces around, a CR from a telnet-style client */
	CHECK_EQ(parse("  hello  \r", &m), 0);
	CHECK(!strcmp(m.verb, "hello"));
	CHECK_EQ(m.n, 0);
}

static void test_parse_quoted_and_repeated(void)
{
	struct dect_ctl_msg m;

	CHECK_EQ(parse("lines line=1 name=\"Line 1\" hs=1,2 line=2 "
		       "name=\"Say \\\"hi\\\" \\\\o/\" hs=2,3", &m), 0);
	CHECK(!strcmp(m.verb, "lines"));
	CHECK_EQ(m.n, 6);
	CHECK(!strcmp(m.arg[1].val, "Line 1"));
	CHECK(!strcmp(m.arg[4].val, "Say \"hi\" \\o/"));
	CHECK(!strcmp(m.arg[5].key, "hs"));
	CHECK(!strcmp(dect_ctl_get(&m, "line"), "1"));	/* the first */

	CHECK_EQ(parse("x name=\"\" v=", &m), 0);
	CHECK_EQ(m.n, 2);
	CHECK(!strcmp(m.arg[0].val, ""));
	CHECK(!strcmp(m.arg[1].val, ""));
}

static void test_parse_refused(void)
{
	struct dect_ctl_msg m;
	char big[DECT_CTL_VAL_MAX + 16];
	char many[400];
	unsigned i, v;

	CHECK_EQ(parse("", &m), -1);
	CHECK_EQ(parse("   ", &m), -1);
	CHECK_EQ(parse("Hello", &m), -1);		/* upper case */
	CHECK_EQ(parse("hs hs", &m), -1);		/* no '=' */
	CHECK_EQ(parse("hs =3", &m), -1);		/* no key */
	CHECK_EQ(parse("hs a=\"open", &m), -1);	/* unterminated */
	CHECK_EQ(parse("hs a=\"x\"y", &m), -1);	/* junk after a quote */
	CHECK_EQ(parse("hs a=b\"c", &m), -1);	/* quote in a bare value */
	CHECK_EQ(parse("hs a=\"\\n\"", &m), -1);	/* unknown escape */
	CHECK_EQ(parse("hs a=1\x01", &m), -1);	/* control character */
	CHECK_EQ(dect_ctl_parse("hs a=1\nderegister hs=1", 22, &m), -1);

	memset(big, 'a', sizeof(big));
	memcpy(big, "x v=", 4);
	big[sizeof(big) - 1] = '\0';
	CHECK_EQ(parse(big, &m), -1);			/* value too long */

	many[0] = '\0';
	strcat(many, "x");
	for (i = 0; i <= DECT_CTL_ARGS_MAX; i++)
		strcat(many, " k=1");
	CHECK_EQ(parse(many, &m), -1);			/* too many pairs */

	CHECK_EQ(parse("register-window seconds=12x", &m), 0);
	CHECK_EQ(dect_ctl_get_uint(&m, "seconds", 0, 3600, &v), -1);
	CHECK_EQ(parse("register-window seconds=4000", &m), 0);
	CHECK_EQ(dect_ctl_get_uint(&m, "seconds", 0, 3600, &v), -1);
	CHECK_EQ(parse("register-window seconds=", &m), 0);
	CHECK_EQ(dect_ctl_get_uint(&m, "seconds", 0, 3600, &v), -1);
}

static void test_build(void)
{
	struct dect_ctl_out o;
	struct dect_ctl_msg m;

	dect_ctl_begin(&o, "hs");
	dect_ctl_add_uint(&o, "hs", 2);
	dect_ctl_add(&o, "state", "registered");
	dect_ctl_add(&o, "name", "Kueche Nord");
	CHECK_EQ(dect_ctl_end(&o), (int)strlen(o.buf));
	CHECK(!strcmp(o.buf,
		      "hs hs=2 state=registered name=\"Kueche Nord\"\n"));

	/* a handset name cannot end the line or break the quoting */
	dect_ctl_begin(&o, "hs");
	dect_ctl_add(&o, "name", "a\"b\\c\nderegister hs=1");
	CHECK(dect_ctl_end(&o) > 0);
	CHECK(!strcmp(o.buf, "hs name=\"a\\\"b\\\\c?deregister hs=1\"\n"));
	CHECK(strchr(o.buf, '\n') == o.buf + o.len - 1);
	/* and it reads back as one message with the same value */
	CHECK_EQ(dect_ctl_parse(o.buf, o.len - 1, &m), 0);
	CHECK_EQ(m.n, 1);
	CHECK(!strcmp(m.arg[0].val, "a\"b\\c?deregister hs=1"));

	dect_ctl_begin(&o, "x");
	dect_ctl_add(&o, "empty", "");
	dect_ctl_end(&o);
	CHECK(!strcmp(o.buf, "x empty=\"\"\n"));
}

static void test_build_overflow(void)
{
	struct dect_ctl_out o;
	char val[100];
	unsigned i;

	memset(val, 'v', sizeof(val) - 1);
	val[sizeof(val) - 1] = '\0';
	dect_ctl_begin(&o, "x");
	for (i = 0; i < 6; i++)
		dect_ctl_add(&o, "k", val);
	CHECK_EQ(dect_ctl_end(&o), -1);
	CHECK(o.len < sizeof(o.buf));
}

int main(void)
{
	test_parse_plain();
	test_parse_quoted_and_repeated();
	test_parse_refused();
	test_build();
	test_build_overflow();
	return CHECK_DONE();
}

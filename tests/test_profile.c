// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_profile.c: docs/handset-profiles.md, the
 * parser, the layers and the display expansion. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_profile.h"

#include <string.h>

static unsigned n_reports;
static unsigned last_line;
static char last_msg[160];

static void report(void *ctx, unsigned line, const char *msg)
{
	(void)ctx;
	n_reports++;
	last_line = line;
	snprintf(last_msg, sizeof(last_msg), "%s", msg);
}

static int parse(const char *text, struct dect_profile *p)
{
	n_reports = 0;
	last_msg[0] = '\0';
	return dect_profile_parse(text, strlen(text), p, report, NULL);
}

static const char gigaset[] =
	"format = 1\n"
	"id = gigaset\n"
	"kind = manufacturer\n"
	"name = \"Gigaset\"\n"
	"source = \"bench 2026-10-28\"\n"
	"cc.name.method = both   # source: test 1\n";

static const char c430a[] =
	"# the example of docs/handset-profiles.md section 8\n"
	"format = 1\n"
	"id = gigaset-c430a\n"
	"kind = device\n"
	"extends = gigaset\n"
	"name = \"Gigaset C430A\"\n"
	"source = \"vRINGC430A-20261028-095744-1; [CR A-0125]\"\n"
	"match.model = hex:00 02 11\n"
	"cc.name.display = hex:02 0a 1a 91 02 02 {len} {text} 0d\n"
	"cc.signal.external = 0x44\n"
	"future.key = 7\n"
	"[firmware 42.010 .. 44.999]\n"
	"lce.release_after_call = no\n"
	"[firmware 45 .. 99]\n"
	"cc.timer = base\n";

static void test_parse_and_layers(void)
{
	struct dect_profile mfr, dev;
	struct dect_hs_profile hp;
	char desc[256];

	CHECK_EQ(parse(gigaset, &mfr), 0);
	CHECK_EQ(n_reports, 0u);
	CHECK_EQ(mfr.kind, DECT_PROFILE_MANUFACTURER);
	CHECK(strcmp(mfr.name, "Gigaset") == 0);

	CHECK_EQ(parse(c430a, &dev), 0);
	CHECK_EQ(n_reports, 1u);		/* the unknown key, ignored */
	CHECK_EQ(last_line, 11u);
	CHECK(strstr(last_msg, "future.key") != NULL);
	CHECK_EQ(dev.kind, DECT_PROFILE_DEVICE);
	CHECK(strcmp(dev.extends, "gigaset") == 0);
	CHECK_EQ(dev.match_model_len, 3u);
	CHECK_EQ(dev.n_sec, 3u);

	/* no firmware known: the base sections only */
	dect_profile_resolve(&mfr, &dev, NULL, &hp);
	CHECK_EQ(hp.cc_name_method, 2u);		/* both, from gigaset */
	CHECK_EQ(hp.cc_signal_external, 0x44u);
	CHECK_EQ(hp.lce_release_after_call, 1u);	/* default */
	CHECK_EQ(hp.cc_timer, 0u);

	/* 43.5 is in the first range; 42.9 is not (42.9 < 42.10) */
	dect_profile_resolve(&mfr, &dev, "43.5", &hp);
	CHECK_EQ(hp.lce_release_after_call, 0u);
	CHECK_EQ(hp.cc_timer, 0u);
	dect_profile_resolve(&mfr, &dev, "42.9", &hp);
	CHECK_EQ(hp.lce_release_after_call, 1u);
	dect_profile_resolve(&mfr, &dev, "45.000", &hp);
	CHECK_EQ(hp.cc_timer, 1u);

	/* the log line names what differs from the defaults */
	dect_profile_resolve(&mfr, &dev, "43.5", &hp);
	CHECK(dect_profile_describe(&hp, desc, sizeof(desc)) > 0);
	CHECK(strstr(desc, "cc.name.method=both") != NULL);
	CHECK(strstr(desc, "cc.signal.external=0x44") != NULL);
	CHECK(strstr(desc, "lce.release_after_call=no") != NULL);
	CHECK(strstr(desc, "cc.name.display=") != NULL);
	CHECK(strstr(desc, "cc.timer") == NULL);

	/* the device layer beats the manufacturer layer */
	{
		static const char over[] =
			"format = 1\nid = x\nkind = device\nextends = gigaset\n"
			"name = \"X\"\nsource = \"t\"\ncc.name.method = none\n";
		struct dect_profile x;

		CHECK_EQ(parse(over, &x), 0);
		dect_profile_resolve(&mfr, &x, NULL, &hp);
		CHECK_EQ(hp.cc_name_method, 3u);
	}
}

/* The defaults are the format page's, and describe to nothing. */
static void test_defaults(void)
{
	struct dect_hs_profile hp;
	char desc[64];
	uint8_t ie[32];
	size_t n;

	dect_profile_defaults(&hp);
	CHECK_EQ(hp.cc_signal_external, 0x41u);
	CHECK_EQ(hp.cc_signal_internal, 0x40u);
	CHECK_EQ(hp.lce_release_after_call, 1u);
	CHECK_EQ(hp.lists_serve, 1u);
	CHECK_EQ(hp.trust_terminal_capability, 1u);
	CHECK_EQ(hp.feature_call_waiting, 1u);
	CHECK_EQ(dect_profile_describe(&hp, desc, sizeof(desc)), 0u);
	/* default display: the text alone */
	n = dect_profile_display(&hp, "Anna", ie, sizeof(ie));
	CHECK_EQ(n, 6u);
	CHECK(memcmp(ie, "\x28\x04" "Anna", 6) == 0);
}

/*
 * The expansion: bench test S3b's element for "Wednesday" comes out octet
 * for octet from the template (docs/handset-profiles.md section 8), and a
 * literal FFH survives.
 */
static void test_display_expansion(void)
{
	static const uint8_t s3b[] = {
		0x28, 0x11, 0x02, 0x0a, 0x1a, 0x91, 0x02, 0x02, 0x09,
		'W', 'e', 'd', 'n', 'e', 's', 'd', 'a', 'y', 0x0d,
	};
	static const char ff[] =
		"format = 1\nid = f\nkind = device\nextends = m\nname = \"F\"\n"
		"source = \"t\"\ncc.name.display = hex:ff {text}\n";
	struct dect_profile mfr, dev;
	struct dect_hs_profile hp;
	uint8_t ie[64];
	size_t n;

	CHECK_EQ(parse(gigaset, &mfr), 0);
	CHECK_EQ(parse(c430a, &dev), 0);
	dect_profile_resolve(&mfr, &dev, NULL, &hp);
	n = dect_profile_display(&hp, "Wednesday", ie, sizeof(ie));
	CHECK_EQ(n, sizeof(s3b));
	CHECK(memcmp(ie, s3b, sizeof(s3b)) == 0);
	CHECK_EQ(dect_profile_display(&hp, "Wednesday", ie, 10), 0u);

	CHECK_EQ(parse(ff, &dev), 0);
	dect_profile_resolve(NULL, &dev, NULL, &hp);
	n = dect_profile_display(&hp, "A", ie, sizeof(ie));
	CHECK_EQ(n, 4u);
	CHECK(memcmp(ie, "\x28\x02\xff" "A", 4) == 0);
}

/* What rejects a file, and what only skips a key. */
static void test_errors(void)
{
	static const char *const rejected[] = {
		/* no source */
		"format = 1\nid = a\nkind = manufacturer\nname = \"A\"\n",
		/* wrong format */
		"format = 2\nid = a\nkind = manufacturer\nname = \"A\"\n"
		"source = \"s\"\n",
		/* a device without extends */
		"format = 1\nid = a\nkind = device\nname = \"A\"\nsource = \"s\"\n",
		/* a repeated key */
		"format = 1\nid = a\nkind = manufacturer\nname = \"A\"\n"
		"source = \"s\"\ncc.timer = base\ncc.timer = base\n",
		/* not key = value */
		"format = 1\nid = a\nkind = manufacturer\nname = \"A\"\n"
		"source = \"s\"\nwhatever\n",
		/* a bad id */
		"format = 1\nid = A_b\nkind = manufacturer\nname = \"A\"\n"
		"source = \"s\"\n",
		/* a firmware section in a manufacturer file */
		"format = 1\nid = a\nkind = manufacturer\nname = \"A\"\n"
		"source = \"s\"\n[firmware 1 .. 2]\n",
		/* a backwards range */
		"format = 1\nid = a\nkind = device\nextends = m\nname = \"A\"\n"
		"source = \"s\"\n[firmware 3 .. 2]\n",
		/* an unquoted name */
		"format = 1\nid = a\nkind = manufacturer\nname = A\n"
		"source = \"s\"\n",
	};
	static const char skipped[] =
		"format = 1\nid = a\nkind = manufacturer\nname = \"A # B\"\n"
		"source = \"s\"\n"
		"cc.signal.external = 0x50\n"	/* out of range */
		"cc.timer = sometimes\n"	/* not a word */
		"cc.name.display = hex:0g\n"	/* not hex */
		"cap.ngdect = -\n";		/* the internal word */
	struct dect_profile p;
	struct dect_hs_profile hp;
	size_t i;

	for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
		CHECK_EQ(parse(rejected[i], &p), -1);
		CHECK(n_reports >= 1u);
	}
	CHECK_EQ(parse(skipped, &p), 0);
	CHECK_EQ(n_reports, 4u);
	CHECK(strcmp(p.name, "A # B") == 0);	/* '#' inside quotes */
	dect_profile_resolve(&p, NULL, NULL, &hp);
	CHECK_EQ(hp.cc_signal_external, 0x41u);	/* the default kept */
	CHECK_EQ(hp.cc_timer, 0u);
}

static void test_version_compare(void)
{
	CHECK(dect_profile_version_cmp("42.10", "42.9") > 0);
	CHECK(dect_profile_version_cmp("42.010", "42.10") == 0);
	CHECK(dect_profile_version_cmp("42", "42.0") == 0);
	CHECK(dect_profile_version_cmp("1.2.3", "1.3") < 0);
}

int main(void)
{
	test_parse_and_layers();
	test_defaults();
	test_display_expansion();
	test_errors();
	test_version_compare();
	return CHECK_DONE();
}

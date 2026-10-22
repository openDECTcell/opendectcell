// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for src/libopendectcell/dect_iwu.c: the <<IWU-TO-IWU>> session responder.
 * Record framing from [CR A-StockTrace-5 §23]. No hardware.
 */
#include "check.h"
#include "../src/libopendectcell/dect_iwu.h"

#include <string.h>

/* The exact open a Gigaset A540 sends, from work/box-tmp/vT3nb.log. */
static const uint8_t a540_open_05[] = { 0xC3, 0x00, 0x05, 0x00 };

static void test_open_is_answered_and_echoes_the_session_id(void)
{
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];
	size_t n;

	dect_iwu_init(&iwu);
	n = dect_iwu_reply(&iwu, a540_open_05, sizeof(a540_open_05),
			   out, sizeof(out));
	CHECK_EQ(n, 11u);
	CHECK_EQ(out[0], DECT_IWU_DISC);
	CHECK_EQ(out[1], DECT_IWU_OPEN_RESP);
	/* The session id is the PEER's and must come back unchanged. */
	CHECK_EQ(out[2], 0x05u);
	CHECK_EQ(out[3], DECT_IWU_TAG);
	CHECK_EQ(out[8], 0xFFu);
	CHECK_EQ(out[9], 0x01u);
	CHECK_EQ(iwu.stats.opened, 1u);
}

/* The A540 opens four sessions per call; each must get its own id back. */
static void test_every_session_id_is_echoed(void)
{
	static const uint8_t ids[] = { 0x00, 0x05, 0x06, 0x08 };
	struct dect_iwu iwu;
	size_t i;

	dect_iwu_init(&iwu);
	for (i = 0; i < sizeof(ids); i++) {
		uint8_t in[] = { 0xC3, DECT_IWU_OPEN, ids[i], 0x00 };
		uint8_t out[DECT_IWU_REPLY_MAX];
		size_t n = dect_iwu_reply(&iwu, in, sizeof(in),
					  out, sizeof(out));

		CHECK_EQ(n, 11u);
		CHECK_EQ(out[2], ids[i]);
	}
	CHECK_EQ(iwu.stats.opened, 4u);
}

static void test_close_is_acknowledged(void)
{
	static const uint8_t close[] = { 0xC3, DECT_IWU_CLOSE, DECT_IWU_TAG };
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];
	size_t n;

	dect_iwu_init(&iwu);
	n = dect_iwu_reply(&iwu, close, sizeof(close), out, sizeof(out));
	CHECK_EQ(n, 3u);
	CHECK_EQ(out[1], DECT_IWU_CLOSE_ACK);
	CHECK_EQ(out[2], DECT_IWU_TAG);
	CHECK_EQ(iwu.stats.closed, 1u);
}

static void test_typed_requests_get_their_paired_response(void)
{
	static const uint8_t req06[] = { 0xC3, 0x06, 0x91, 0x81, 0x03,
					 0x00, 0x01, 0x02, 0x03 };
	static const uint8_t req10[] = { 0xC3, 0x10, 0x91, 0x00, 0x05, 0x31 };
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];
	size_t n;

	dect_iwu_init(&iwu);
	n = dect_iwu_reply(&iwu, req06, sizeof(req06), out, sizeof(out));
	CHECK_EQ(n, 5u);
	CHECK_EQ(out[1], DECT_IWU_RESP_07);
	CHECK_EQ(out[3], 0x81u);
	CHECK_EQ(out[4], 0x03u);

	n = dect_iwu_reply(&iwu, req10, sizeof(req10), out, sizeof(out));
	CHECK_EQ(n, 4u);
	CHECK_EQ(out[1], DECT_IWU_RESP_12);
	CHECK_EQ(out[3], 0x01u);
	CHECK_EQ(iwu.stats.replied, 2u);
}

/*
 * Anything we do not recognise gets silence, not a guess. The base's own
 * push (0x14) is the peer's to answer, and our own responses must not
 * provoke a reply if they are ever fed back.
 */
static void test_unknown_records_are_ignored(void)
{
	static const uint8_t push[] = { 0xC3, DECT_IWU_PUSH_14, 0x91, 0x80 };
	static const uint8_t own_resp[] = { 0xC3, DECT_IWU_OPEN_RESP, 0x05,
					    0x91, 0x82 };
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];

	dect_iwu_init(&iwu);
	CHECK_EQ(dect_iwu_reply(&iwu, push, sizeof(push), out, sizeof(out)), 0u);
	CHECK_EQ(dect_iwu_reply(&iwu, own_resp, sizeof(own_resp),
				out, sizeof(out)), 0u);
	CHECK_EQ(iwu.stats.ignored, 2u);
	CHECK_EQ(iwu.stats.opened, 0u);
}

static void test_malformed_records_are_rejected(void)
{
	static const uint8_t not_c3[] = { 0xC4, 0x00, 0x05 };
	static const uint8_t too_short[] = { 0xC3 };
	static const uint8_t open_without_id[] = { 0xC3, DECT_IWU_OPEN };
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];

	dect_iwu_init(&iwu);
	CHECK_EQ(dect_iwu_reply(&iwu, not_c3, sizeof(not_c3),
				out, sizeof(out)), 0u);
	CHECK_EQ(dect_iwu_reply(&iwu, too_short, sizeof(too_short),
				out, sizeof(out)), 0u);
	/* An open with no session id: there is nothing to echo, so refuse. */
	CHECK_EQ(dect_iwu_reply(&iwu, open_without_id, sizeof(open_without_id),
				out, sizeof(out)), 0u);
	CHECK_EQ(dect_iwu_reply(&iwu, NULL, 0, out, sizeof(out)), 0u);
	CHECK_EQ(iwu.stats.malformed, 4u);
}

/* A caller with no room gets nothing, and no stat is counted for a reply
 * that was never produced. */
static void test_short_output_buffer_is_safe(void)
{
	struct dect_iwu iwu;
	uint8_t out[4];

	dect_iwu_init(&iwu);
	CHECK_EQ(dect_iwu_reply(&iwu, a540_open_05, sizeof(a540_open_05),
				out, sizeof(out)), 0u);
	CHECK_EQ(iwu.stats.opened, 0u);
}

static void test_open_params_are_overridable(void)
{
	struct dect_iwu iwu;
	uint8_t out[DECT_IWU_REPLY_MAX];

	dect_iwu_init(&iwu);
	/* Defaults are the lowest observed values. */
	CHECK_EQ(iwu.open_x, 0x82u);
	CHECK_EQ(iwu.open_y, 0x01u);
	dect_iwu_set_open_params(&iwu, 0x84u, 0x03u);
	CHECK_EQ(dect_iwu_reply(&iwu, a540_open_05, sizeof(a540_open_05),
				out, sizeof(out)), 11u);
	CHECK_EQ(out[4], 0x84u);
	CHECK_EQ(out[10], 0x03u);
}

int main(void)
{
	test_open_is_answered_and_echoes_the_session_id();
	test_every_session_id_is_echoed();
	test_close_is_acknowledged();
	test_typed_requests_get_their_paired_response();
	test_unknown_records_are_ignored();
	test_malformed_records_are_rejected();
	test_short_output_buffer_is_safe();
	test_open_params_are_overridable();
	return CHECK_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dect_diag: the 0xF625 decode, the protocol half from a link, and the JSON
 * that leaves the process. Values are from our own captures where we have
 * them (docs/diagnostics.md, vF418 and vC4T4d).
 */
#include <string.h>

#include "check.h"
#include "dect_diag.h"

/* A 0x2D-byte 0xF625 parameter block, as the chip sends it. */
static void status_block(uint8_t *p, uint16_t frames, uint16_t interval)
{
	static const uint8_t rssi[10] = { 0x1e, 0x1e, 0x20, 0x20, 0x1f,
					  0x1d, 0x2c, 0x24, 0x1d, 0x1f };

	memset(p, 0, 0x2D);
	p[0] = 1;			/* link */
	p[1] = 0x0a;
	p[2] = 4;			/* time slot */
	p[3] = 6;			/* carrier: rssi[6] = 0x2c, the peak */
	p[5] = 2;
	memcpy(&p[6], rssi, sizeof(rssi));
	p[0x14] = 1;			/* s12b0 fired once */
	p[0x1C] = 2;			/* the unmapped one, twice */
	p[0x1A] = (uint8_t)interval;
	p[0x1B] = (uint8_t)(interval >> 8);
	p[0x24] = (uint8_t)frames;
	p[0x25] = (uint8_t)(frames >> 8);
}

static void test_decode_a_real_row(void)
{
	uint8_t p[0x2D];
	struct dect_diag_status st;

	status_block(p, 343, 0x0320);
	CHECK_EQ(dect_diag_decode_status(p, sizeof(p), &st), 0);
	CHECK_EQ(st.valid, 1u);
	CHECK_EQ(st.link, 1u);
	CHECK_EQ(st.time_slot, 4u);
	CHECK_EQ(st.carrier, 6u);
	CHECK_EQ(st.b5, 2u);
	CHECK_EQ(st.rssi[6], 0x2cu);
	CHECK_EQ(st.has_counters, 1u);
	CHECK_EQ(st.events[2], 1u);		/* 0x14 */
	CHECK_EQ(st.events[5], 2u);		/* 0x1C */
	CHECK_EQ(st.events[0], 0u);
	CHECK_EQ(st.interval_frames, 800u);
	CHECK_EQ(st.frames, 343u);
	CHECK(strcmp(dect_diag_event_name(5), "handover") == 0);	/* A-0123 */
	CHECK(strcmp(dect_diag_event_name(8), "?") == 0);
}

static void test_short_rows(void)
{
	uint8_t p[0x2D];
	struct dect_diag_status st;

	status_block(p, 0, 0);
	CHECK_EQ(dect_diag_decode_status(p, 0x0F, &st), -1);
	CHECK_EQ(st.valid, 0u);
	CHECK_EQ(dect_diag_decode_status(p, 0x10, &st), 0);	/* head only */
	CHECK_EQ(st.valid, 1u);
	CHECK_EQ(st.has_counters, 0u);
	CHECK_EQ(dect_diag_decode_status(p, 0x25, &st), 0);
	CHECK_EQ(st.has_counters, 0u);
	CHECK_EQ(dect_diag_decode_status(NULL, 0x2D, &st), -1);
}

/* vC4T4d: 343 -> 1142 frames over 8 s; and a 16-bit wrap carried over. */
static void test_frames_unwrap(void)
{
	uint8_t p[0x2D];
	struct dect_diag_status st;
	struct dect_diag_frames f;

	memset(&f, 0, sizeof(f));
	status_block(p, 343, 0);
	dect_diag_decode_status(p, sizeof(p), &st);
	CHECK_EQ(dect_diag_frames_update(&f, &st), 343u);
	status_block(p, 1142, 0);
	dect_diag_decode_status(p, sizeof(p), &st);
	CHECK_EQ(dect_diag_frames_update(&f, &st), 1142u);
	status_block(p, 65500, 0);
	dect_diag_decode_status(p, sizeof(p), &st);
	CHECK_EQ(dect_diag_frames_update(&f, &st), 65500u);
	status_block(p, 100, 0);			/* wrapped */
	dect_diag_decode_status(p, sizeof(p), &st);
	CHECK_EQ(dect_diag_frames_update(&f, &st), 65636u);
	/* a head-only row leaves the count alone */
	dect_diag_decode_status(p, 0x10, &st);
	CHECK_EQ(dect_diag_frames_update(&f, &st), 65636u);
}

/* What the handset says about itself in {LOCATE-REQUEST} is kept. */
static void test_locate_request_self_description(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	static const uint8_t model[] = { 0x0b, 0x74, 0x08 };	/* the C4's */
	static const uint8_t codecs[] = { 0x90, 0x03, 0x00, 0x01,
					  0x02, 0x00, 0x84 };
	struct dect_mm_cfg cfg;
	struct dect_mm mm;
	uint8_t buf[64], out[64];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	dect_mm_init(&mm, &cfg);
	memcpy(mm.port_id, ipui, sizeof(ipui));
	mm.port_id_len = sizeof(ipui);
	n = dect_nwk_build_hdr(buf, sizeof(buf), DECT_PD_MM, DECT_TI_ORIGINATOR,
			       0, DECT_MM_LOCATE_REQUEST);
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_PORTABLE_IDENTITY,
			    ipui, sizeof(ipui));
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_MODEL_IDENTIFIER,
			    model, sizeof(model));
	n = dect_nwk_put_ie(buf, sizeof(buf), n, DECT_IE_CODEC_LIST,
			    codecs, sizeof(codecs));
	dect_mm_rx(&mm, buf, n, NULL, NULL);
	dect_mm_poll(&mm, out, sizeof(out));
	CHECK_EQ(mm.peer_model_len, sizeof(model));
	CHECK(memcmp(mm.peer_model, model, sizeof(model)) == 0);
	CHECK_EQ(mm.peer_codecs_len, sizeof(codecs));
	CHECK(memcmp(mm.peer_codecs, codecs, sizeof(codecs)) == 0);
}

static void test_from_link_and_json(void)
{
	static const uint8_t ipui[] = { 0x80, 0xa8, 0x02, 0x11, 0x22, 0x33 };
	struct dect_mm_cfg cfg;
	struct dect_link l;
	struct dect_diag_link d;
	uint8_t p[0x2D];
	char js[1024], tiny[16];
	size_t n;

	memset(&cfg, 0, sizeof(cfg));
	cfg.sub_slot = 2;
	dect_link_init(&l, 3, &cfg);
	memcpy(l.mm.port_id, ipui, sizeof(ipui));
	l.mm.port_id_len = sizeof(ipui);
	l.mm.adopted = 1;
	l.mm.stats.located = 1;
	l.mm.peer_model[0] = 0x0b;
	l.mm.peer_model_len = 1;

	memset(&d, 0, sizeof(d));
	status_block(p, 343, 800);
	dect_diag_decode_status(p, sizeof(p), &d.radio);
	d.call_frames = 343;
	dect_diag_from_link(&l, &d);
	CHECK_EQ(d.handle, 3u);
	CHECK_EQ(d.identity_len, sizeof(ipui));
	CHECK_EQ(d.handset_tag, 1);
	CHECK_EQ(d.slot, 2u);
	CHECK_EQ(d.tpui[2], 2u);
	CHECK_EQ(d.located, 1u);
	CHECK_EQ(d.call_state, DECT_CC_ST_NULL);
	CHECK_EQ(d.radio.carrier, 6u);		/* kept across from_link */
	CHECK_EQ(d.call_frames, 343u);

	n = dect_diag_json(&d, 0, js, sizeof(js));
	CHECK(n > 0 && n == strlen(js));
	CHECK(strstr(js, "\"ipui\"") == NULL);	/* not without asking */
	CHECK(strstr(js, "\"slot\":2") != NULL);
	CHECK(strstr(js, "\"call\":\"idle\"") != NULL);
	CHECK(strstr(js, "\"carrier\":6") != NULL);
	CHECK(strstr(js, "\"handover\":2") != NULL);
	CHECK(strstr(js, "\"call_frames\":343") != NULL);
	CHECK(js[0] == '{' && js[n - 1] == '}');

	n = dect_diag_json(&d, 1, js, sizeof(js));
	CHECK(strstr(js, "\"ipui\":\"80a802112233\"") != NULL);

	/* too small: nothing half-written */
	CHECK_EQ(dect_diag_json(&d, 0, tiny, sizeof(tiny)), 0u);
	CHECK_EQ(tiny[0], '\0');
}

/*
 * FRITZ!OS's numbers (clean room A-0121): dBm from the raw RSSI, and the
 * Quality % from it and the error counters over CONNECTTIME [0x22]; the
 * 32-bit frame count at [0x24] (A-0123).
 */
static void test_dbm_and_quality(void)
{
	uint8_t row[0x2D];
	struct dect_diag_status st;

	CHECK_EQ(dect_diag_rssi_dbm10(30), -884);	/* A-0121: -88.4 */
	CHECK_EQ(dect_diag_rssi_dbm10(33), -819);	/* -81.9 */
	CHECK_EQ(dect_diag_rssi_dbm10(59), -257);	/* -25.7 */
	CHECK_EQ(dect_diag_rssi_dbm10(0), DECT_DIAG_NO_DBM);
	CHECK_EQ(dect_diag_rssi_dbm10(255), DECT_DIAG_NO_DBM);

	memset(row, 0, sizeof(row));
	row[3] = 4;				/* carrier */
	row[6 + 4] = 59;			/* -25.7 dBm: q_rssi 100 */
	row[0x22] = 0x20; row[0x23] = 0x03;	/* CONNECTTIME 800 */
	row[0x24] = 0x01; row[0x26] = 0x01;	/* uptime 0x00010001 */
	CHECK_EQ(dect_diag_decode_status(row, sizeof(row), &st), 0);
	CHECK_EQ(st.connect_frames, 800u);
	CHECK_EQ(st.uptime, 0x10001u);
	CHECK_EQ(dect_diag_quality(&st), 100);	/* no errors */

	row[0x10] = 8;				/* ACRC 8 of 800: 99 % good */
	dect_diag_decode_status(row, sizeof(row), &st);
	CHECK_EQ(dect_diag_quality(&st), 90);

	row[0x10] = 100;			/* 87 % good */
	dect_diag_decode_status(row, sizeof(row), &st);
	CHECK_EQ(dect_diag_quality(&st), 5);

	row[0x10] = 0;
	row[6 + 4] = 33;			/* -81.9 dBm: q_rssi 20 */
	dect_diag_decode_status(row, sizeof(row), &st);
	CHECK_EQ(dect_diag_quality(&st), 20);

	row[0x22] = 0; row[0x23] = 0;		/* no frames: nothing to judge */
	dect_diag_decode_status(row, sizeof(row), &st);
	CHECK_EQ(dect_diag_quality(&st), -1);
}

int main(void)
{
	test_dbm_and_quality();
	test_decode_a_real_row();
	test_short_rows();
	test_frames_unwrap();
	test_locate_request_self_description();
	test_from_link_and_json();
	return CHECK_DONE();
}

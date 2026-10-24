// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * STUB-facing C-plane transport. See dect_cs.h.
 */
#include "dect_cs.h"
#include "dect_proto.h"

#include <string.h>

/* 0x0203 param layout [CR COMMAND_SET §8.2]. */
#define SEG_OFF_HANDLE	0u
#define SEG_OFF_MARKER	1u	/* always 0x01 */
#define SEG_OFF_DATA	2u	/* 5 bytes */
#define SEG_OFF_TAG	9u	/* 3 bytes */
#define SEG_OFF_HANDLE2	12u	/* the handle again, in both 13- and 15-byte forms */

int dect_cs_decode_segment(const struct dect_mail *m, struct dect_cs_seg *seg)
{
	if (m->prim != DECT_PRIM_SIGNALLING)
		return -1;
	if (m->param_len < DECT_CS_MIN_PARAM)
		return -1;
	/*
	 * The marker and the handle copy are fixed in all 42 252 segments of
	 * the 2026-10-24 corpus; a segment that breaks either is malformed and
	 * must not reach DLC reassembly. Found by the second external review,
	 * the clean-room record/an external review-1-24102026.md, finding 7.
	 */
	if (m->param[SEG_OFF_MARKER] != 0x01u ||
	    m->param[SEG_OFF_HANDLE2] != m->param[SEG_OFF_HANDLE])
		return -1;

	seg->handle = m->param[SEG_OFF_HANDLE];
	memcpy(seg->data, &m->param[SEG_OFF_DATA], DECT_CS_SEG_LEN);
	memcpy(seg->tag, &m->param[SEG_OFF_TAG], DECT_CS_TAG_LEN);
	return 0;
}

int dect_cs_append_mail(struct dect_mail *m, uint8_t src,
			uint8_t handle, const uint8_t *data, size_t n)
{
	if (n == 0 || n > DECT_APPEND_BUF_MAX)
		return -1;

	memset(m, 0, sizeof(*m));
	m->src = src;
	m->dst = DECT_TASK_CONTACT_DST;
	m->prim = DECT_PRIM_APPEND_DATA;
	m->param[0] = handle;
	m->param[1] = (uint8_t)n;
	memcpy(&m->param[2], data, n);
	m->param_len = (uint16_t)(2u + n);
	return 0;
}

void dect_cs_trigger_mail(struct dect_mail *m, uint8_t src, uint8_t handle)
{
	memset(m, 0, sizeof(*m));
	m->src = src;
	m->dst = DECT_TASK_CONTACT_DST;
	m->prim = DECT_PRIM_TRIGGER_SIG;
	m->param[0] = handle;
	m->param_len = 1;
}

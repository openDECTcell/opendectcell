// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DECT DLC LAPC frame codec. See dect_lapc.h.
 * ETSI EN 300 175-4 V1.5.1 §7.2-7.7, §7.11.
 */
#include "dect_lapc.h"

#include <string.h>

/* Address octet bit fields (§7.2-7.3). */
#define ADDR_RES	0x01u		/* bit 1, always 1 */
#define ADDR_CR		0x02u		/* bit 2 */
#define ADDR_SAPI_SH	2u		/* bits 4-3 */
#define ADDR_LLN_SH	4u		/* bits 7-5 */
#define ADDR_NLF	0x80u		/* bit 8 */

/* Control octet (§7.4, §7.11). */
#define CTRL_PF		0x10u		/* bit 5: poll/final */
#define CTRL_NR_SH	5u		/* bits 8-6 */
#define CTRL_NS_SH	1u		/* bits 4-2 */
#define CTRL_SS_SH	2u		/* S-frame supervisory code, bits 4-3 */

/* Length-indicator octet (§7.6-7.7). */
#define LI_N		0x01u		/* bit 1: last LI octet */
#define LI_M		0x02u		/* bit 2: more data */
#define LI_SH		2u		/* bits 8-3: length */

static enum dect_lapc_type decode_control(uint8_t ctrl, uint8_t *ns,
					  uint8_t *nr, uint8_t *pf)
{
	*ns = 0;
	*nr = 0;
	*pf = (ctrl & CTRL_PF) ? 1u : 0u;

	if ((ctrl & 0x01u) == 0) {		/* I-frame: bit1 = 0 */
		*ns = (ctrl >> CTRL_NS_SH) & 0x07u;
		*nr = (ctrl >> CTRL_NR_SH) & 0x07u;
		return DECT_LAPC_I;
	}
	if ((ctrl & 0x03u) == 0x01u) {		/* S-frame: bits 2-1 = 01 */
		*nr = (ctrl >> CTRL_NR_SH) & 0x07u;
		switch ((ctrl >> CTRL_SS_SH) & 0x03u) {
		case 0: return DECT_LAPC_RR;
		case 1: return DECT_LAPC_RNR;
		case 2: return DECT_LAPC_REJ;
		default: return DECT_LAPC_UNKNOWN;
		}
	}
	/* U-frame: bits 2-1 = 11. Strip the P/F bit before matching. */
	switch (ctrl & ~CTRL_PF) {
	case 0x2Fu: return DECT_LAPC_SABM;
	case 0x0Fu: return DECT_LAPC_DM;
	case 0x03u: return DECT_LAPC_UI;
	case 0x43u: return DECT_LAPC_DISC;
	case 0x63u: return DECT_LAPC_UA;
	default: return DECT_LAPC_UNKNOWN;
	}
}

int dect_lapc_parse(const uint8_t *buf, size_t lapc_len, struct dect_lapc_frame *f)
{
	uint8_t addr, ctrl, li;
	size_t info_len;

	if (lapc_len < 3)
		return -1;

	addr = buf[0];
	ctrl = buf[1];
	li = buf[2];

	memset(f, 0, sizeof(*f));
	f->nlf  = (addr & ADDR_NLF) ? 1u : 0u;
	f->lln  = (addr >> ADDR_LLN_SH) & 0x07u;
	f->sapi = (addr >> ADDR_SAPI_SH) & 0x03u;
	f->cr   = (addr & ADDR_CR) ? 1u : 0u;
	f->type = decode_control(ctrl, &f->ns, &f->nr, &f->pf);
	f->more = (li & LI_M) ? 1u : 0u;

	info_len = li >> LI_SH;
	if (info_len > DECT_LC_INFO_MAX || 3 + info_len != lapc_len)
		return -1;
	f->info = (info_len > 0) ? &buf[3] : NULL;
	f->info_len = info_len;
	return 0;
}

static uint8_t build_addr(uint8_t cr, uint8_t nlf, uint8_t sapi, uint8_t lln)
{
	return (uint8_t)((nlf ? ADDR_NLF : 0u) |
			 ((lln & 0x07u) << ADDR_LLN_SH) |
			 ((sapi & 0x03u) << ADDR_SAPI_SH) |
			 (cr ? ADDR_CR : 0u) | ADDR_RES);
}

size_t dect_lapc_build_i(uint8_t *out, size_t out_cap, uint8_t cr, uint8_t nlf,
			 uint8_t ns, uint8_t nr, uint8_t pf, uint8_t more,
			 const uint8_t *info, size_t info_len)
{
	if (info_len > DECT_LC_INFO_MAX || out_cap < 3 + info_len)
		return 0;

	out[0] = build_addr(cr, nlf, DECT_LAPC_SAPI_CO, DECT_LAPC_LLN_A);
	out[1] = (uint8_t)(((nr & 0x07u) << CTRL_NR_SH) |
			   (pf ? CTRL_PF : 0u) |
			   ((ns & 0x07u) << CTRL_NS_SH));	/* bit1 = 0 => I */
	out[2] = (uint8_t)(((info_len & 0x3Fu) << LI_SH) |
			   (more ? LI_M : 0u) | LI_N);
	if (info_len > 0)
		memcpy(&out[3], info, info_len);
	return 3 + info_len;
}

size_t dect_lapc_build_rr(uint8_t *out, size_t out_cap, uint8_t cr,
			  uint8_t nr, uint8_t pf)
{
	if (out_cap < 3)
		return 0;

	out[0] = build_addr(cr, 0, DECT_LAPC_SAPI_CO, DECT_LAPC_LLN_A);
	out[1] = (uint8_t)(((nr & 0x07u) << CTRL_NR_SH) |
			   (pf ? CTRL_PF : 0u) | 0x01u);	/* bits2-1 = 01, SS = 00 */
	out[2] = (uint8_t)LI_N;				/* LI = 0, M = 0, N = 1 */
	return 3;
}

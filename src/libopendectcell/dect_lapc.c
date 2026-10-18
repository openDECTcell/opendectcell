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
			 uint8_t lln, uint8_t ns, uint8_t nr, uint8_t pf,
			 uint8_t more, const uint8_t *info, size_t info_len)
{
	if (info_len > DECT_LC_INFO_MAX || out_cap < 3 + info_len)
		return 0;

	out[0] = build_addr(cr, nlf, DECT_LAPC_SAPI_CO, lln);
	out[1] = (uint8_t)(((nr & 0x07u) << CTRL_NR_SH) |
			   (pf ? CTRL_PF : 0u) |
			   ((ns & 0x07u) << CTRL_NS_SH));	/* bit1 = 0 => I */
	out[2] = (uint8_t)(((info_len & 0x3Fu) << LI_SH) |
			   (more ? LI_M : 0u) | LI_N);
	if (info_len > 0)
		memcpy(&out[3], info, info_len);
	return 3 + info_len;
}

size_t dect_lapc_build_rr(uint8_t *out, size_t out_cap, uint8_t cr, uint8_t nlf,
			  uint8_t lln, uint8_t nr, uint8_t pf)
{
	if (out_cap < 3)
		return 0;

	out[0] = build_addr(cr, nlf, DECT_LAPC_SAPI_CO, lln);
	out[1] = (uint8_t)(((nr & 0x07u) << CTRL_NR_SH) |
			   (pf ? CTRL_PF : 0u) | 0x01u);	/* bits2-1 = 01, SS = 00 */
	out[2] = (uint8_t)LI_N;				/* LI = 0, M = 0, N = 1 */
	return 3;
}

/* ---- class-A acknowledged-link state machine ---- */

#define DEFAULT_RETRY_MS	1000u	/* retransmit an unacked I-frame */
#define CR_CMD			1u	/* FT commands (our I-frames) */
#define CR_RSP			0u	/* FT responses (our RR/UA) */
#define SEG_MAX			DECT_LC_INFO_MAX	/* info octets per I-frame */

void dect_lapc_init(struct dect_lapc *l, uint32_t retry_ms)
{
	memset(l, 0, sizeof(*l));
	l->retry_ms = retry_ms ? retry_ms : DEFAULT_RETRY_MS;
	l->lln = DECT_LAPC_LLN_A;	/* until the peer's first frame sets it */
	l->modulus = 2;			/* class A until a class-B request is accepted */
	l->nlf_pending = 1;	/* first I-frame of the link sets NLF */
}

/* Advance a sequence variable modulo the link's modulus (2 = A, 8 = B). */
static uint8_t seq_inc(const struct dect_lapc *l, uint8_t v)
{
	return (uint8_t)((v + 1u) % l->modulus);
}

/* Enqueue a built LAPC frame of len octets for transmission. */
static void txq_push(struct dect_lapc *l, const uint8_t *buf, size_t len)
{
	unsigned slot;

	if (l->txq_count >= DECT_LAPC_TXQ || len == 0 || len > DECT_LC_LAPC_MAX)
		return;		/* drop silently if the caller polls too rarely */
	slot = (l->txq_head + l->txq_count) % DECT_LAPC_TXQ;
	memcpy(l->txq[slot].buf, buf, len);
	l->txq[slot].len = len;
	l->txq_count++;
}

/*
 * Queue an RR. nlf must be set on the response to the peer's establishment
 * frame (EN 300 175-4 §9.2.3.1: the initiator only confirms establishment on an
 * RR with NLF set) and cleared afterwards. We always answer on the class-A LLN,
 * which refuses any class-B request (§9.2.2.2) and keeps the link class A.
 */
static void queue_rr(struct dect_lapc *l, uint8_t nlf, uint8_t pf)
{
	uint8_t rr[3];
	size_t n = dect_lapc_build_rr(rr, sizeof(rr), CR_RSP, nlf,
				      l->lln, l->vr, pf);

	txq_push(l, rr, n);
	l->stats.tx_rr++;
}

/* Queue a UA response (control 0x63, no information field). */
static void queue_ua(struct dect_lapc *l, uint8_t pf)
{
	uint8_t frame[3];

	frame[0] = build_addr(CR_RSP, 0, DECT_LAPC_SAPI_CO, l->lln);
	frame[1] = (uint8_t)(0x63u | (pf ? CTRL_PF : 0u));
	frame[2] = (uint8_t)LI_N;
	txq_push(l, frame, 3);
}

/* Queue the current outstanding I-frame (first send or retransmit). */
static void queue_cur_i(struct dect_lapc *l, uint64_t now_ms)
{
	txq_push(l, l->cur_i, l->cur_i_len);
	l->last_tx_ms = now_ms;
	l->stats.tx_i++;
}

/* If the window is open and the message has more to send, build the next I-frame. */
static void pump_tx(struct dect_lapc *l, uint64_t now_ms)
{
	size_t seg;

	if (l->outstanding || l->txmsg_off >= l->txmsg_len)
		return;

	seg = l->txmsg_len - l->txmsg_off;
	if (seg > SEG_MAX)
		seg = SEG_MAX;

	/* Class A: P/F must be 0 in all transmitted frames (EN 300 175-4 §9.2.1.1). */
	l->cur_i_len = dect_lapc_build_i(l->cur_i, sizeof(l->cur_i), CR_CMD,
					 l->nlf_pending, l->lln, l->vs, l->vr, 0,
					 l->txmsg_off + seg < l->txmsg_len,
					 &l->txmsg[l->txmsg_off], seg);
	l->nlf_pending = 0;
	l->txmsg_off += seg;
	l->vs = seq_inc(l, l->vs);
	l->outstanding = 1;
	queue_cur_i(l, now_ms);
}

int dect_lapc_send(struct dect_lapc *l, const uint8_t *msg, size_t len,
		   uint64_t now_ms)
{
	if (l->outstanding || l->txmsg_off < l->txmsg_len)
		return -1;		/* window 1: one message at a time */
	if (len == 0 || len > sizeof(l->txmsg))
		return -1;

	memcpy(l->txmsg, msg, len);
	l->txmsg_len = len;
	l->txmsg_off = 0;
	pump_tx(l, now_ms);
	return 0;
}

/* Process the N(R) carried by any frame: has it acknowledged our I-frame? */
static void handle_ack(struct dect_lapc *l, uint8_t nr, uint64_t now_ms)
{
	if (!l->outstanding)
		return;
	/* The outstanding frame had N(S) = va; it is acked once N(R) = vs (= va^1). */
	if (nr == l->vs) {
		l->va = nr;
		l->outstanding = 0;
		if (l->txmsg_off >= l->txmsg_len && l->txmsg_len) {
			l->txmsg_len = 0;	/* message fully sent and acked */
			l->txmsg_off = 0;
			l->stats.msgs_tx++;
		}
		pump_tx(l, now_ms);		/* next segment, if any */
	}
}

int dect_lapc_rx(struct dect_lapc *l, const struct dect_lapc_frame *f,
		 uint64_t now_ms, uint8_t *out, size_t out_cap)
{
	int msg_len = 0;

	l->established = 1;
	/*
	 * The PT opens the link on an assignable LLN (a class-B request,
	 * EN 300 175-4 §9.2.2.2). AVM's FP refuses class B for an unassigned
	 * subscriber and answers on class A (LLN 1), so do the same [CR AVM].
	 */
	l->lln = DECT_LAPC_LLN_A;
	l->modulus = 2;
	if (f->nlf) {
		l->vr = 0;		/* new link: discard the old receive state */
		l->nlf_pending = 0;	/* PT established the link; our frames are subsequent */
	}

	switch (f->type) {
	case DECT_LAPC_SABM:
		l->vs = l->va = l->vr = 0;
		l->outstanding = 0;
		l->rxmsg_len = 0;
		queue_ua(l, f->pf);
		return 0;

	case DECT_LAPC_DISC:
		queue_ua(l, f->pf);
		l->established = 0;
		return 0;

	case DECT_LAPC_RR:
	case DECT_LAPC_RNR:
	case DECT_LAPC_REJ:
		handle_ack(l, f->nr, now_ms);
		if (f->type == DECT_LAPC_REJ && l->outstanding)
			queue_cur_i(l, now_ms);	/* explicit retransmit request */
		return 0;

	case DECT_LAPC_I:
		handle_ack(l, f->nr, now_ms);	/* I-frames also carry N(R) */
		/*
		 * Every in-sequence I-frame (including the empty establishment frame)
		 * advances V(R) and is acknowledged with RR N(R)=V(R); the NLF bit is
		 * mirrored so the establishment response carries it (EN 300 175-4
		 * §9.2.3.1). The empty establishment frame carries no NWK data, so
		 * nothing is delivered for it.
		 */
		if (f->ns == l->vr) {		/* in sequence */
			if (l->rxmsg_len + f->info_len > sizeof(l->rxmsg))
				l->rxmsg_len = 0;	/* overflow: drop */
			else if (f->info_len) {
				memcpy(&l->rxmsg[l->rxmsg_len], f->info, f->info_len);
				l->rxmsg_len += f->info_len;
			}
			l->vr = seq_inc(l, l->vr);
			l->stats.rx_i++;
			queue_rr(l, f->nlf, f->pf);
			if (!f->more && l->rxmsg_len) {		/* complete NWK message */
				if (out_cap >= l->rxmsg_len) {
					memcpy(out, l->rxmsg, l->rxmsg_len);
					msg_len = (int)l->rxmsg_len;
					l->stats.msgs_rx++;
				}
				l->rxmsg_len = 0;
			}
		} else {			/* duplicate / out of sequence */
			l->stats.rx_dup++;
			queue_rr(l, f->nlf, f->pf);	/* re-acknowledge */
		}
		return msg_len;

	default:
		return 0;
	}
}

void dect_lapc_tick(struct dect_lapc *l, uint64_t now_ms)
{
	if (l->outstanding && now_ms - l->last_tx_ms >= l->retry_ms) {
		l->stats.retransmits++;
		queue_cur_i(l, now_ms);
	}
}

size_t dect_lapc_poll_tx(struct dect_lapc *l, uint8_t *out, size_t out_cap)
{
	size_t len;

	if (l->txq_count == 0)
		return 0;
	len = l->txq[l->txq_head].len;
	if (out_cap < len)
		return 0;
	memcpy(out, l->txq[l->txq_head].buf, len);
	l->txq_head = (l->txq_head + 1) % DECT_LAPC_TXQ;
	l->txq_count--;
	return len;
}

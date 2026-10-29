/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The cell <-> controller wire protocol, v1. See dect_cell.h; the section
 * numbers are those of opendectcell-controller docs/cell-protocol.md.
 */

#include <string.h>

#include "dect_cell.h"

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

/* §3.3, the "Max body" column */
size_t dect_cell_body_max(uint16_t type)
{
	switch (type) {
	case DECT_CELL_HELLO:		return 1024;
	case DECT_CELL_HELLO_ACK:	return 512;
	case DECT_CELL_HEARTBEAT:
	case DECT_CELL_HEARTBEAT_ECHO:	return 4;
	case DECT_CELL_GOODBYE:
	case DECT_CELL_ERROR:
	case DECT_CELL_CELL_STATE:	return 256;
	case DECT_CELL_FLOW:
	case DECT_CELL_MAIL_REJECT:	return 3;
	case DECT_CELL_ENROLL_REQUEST:
	case DECT_CELL_STATS:
	case DECT_CELL_RENEW_REQUEST:	return 2048;
	case DECT_CELL_ENROLL_RESPONSE:
	case DECT_CELL_RENEW_RESPONSE:
	case DECT_CELL_TRUST_UPDATE:	return 4096;
	case DECT_CELL_MAIL_UP:		return 9u + DECT_CELL_MAIL_PARAM_MAX;
	case DECT_CELL_MAIL_DOWN:	return 5u + DECT_CELL_MAIL_PARAM_MAX;
	case DECT_CELL_CONFIG_SET:
	case DECT_CELL_STATUS:
	case DECT_CELL_MEDIA_OPEN:	return 512;
	case DECT_CELL_CONFIG_ACK:
	case DECT_CELL_TRUST_ACK:
	case DECT_CELL_MEDIA_OPEN_ACK:
	case DECT_CELL_MEDIA_CLOSE:
	case DECT_CELL_MEDIA_CLOSED:	return 64;
	case DECT_CELL_STATUS_REQUEST:
	case DECT_CELL_STATS_REQUEST:	return 0;
	case DECT_CELL_STATE_SNAPSHOT:	return 1024;
	}
	return 0;
}

long dect_cell_parse(const uint8_t *buf, size_t n, struct dect_cell_frame *f)
{
	unsigned len;

	if (n < 2u)
		return 0;
	len = get16(buf);		/* the octets after the length field */
	if (len < 4u || len > 4u + DECT_CELL_BODY_MAX)
		return -1;
	if (n < 2u + len)
		return 0;
	f->type = get16(buf + 2);
	f->corr = get16(buf + 4);
	f->body = buf + DECT_CELL_HDR;
	f->len = len - 4u;
	return (long)(2u + len);
}

size_t dect_cell_put(uint8_t *out, size_t cap, uint16_t type, uint16_t corr,
		     const uint8_t *body, size_t len)
{
	if (len > DECT_CELL_BODY_MAX || cap < DECT_CELL_HDR + len)
		return 0;
	put16(out, (uint16_t)(4u + len));
	put16(out + 2, type);
	put16(out + 4, corr);
	if (len)
		memcpy(out + DECT_CELL_HDR, body, len);
	return DECT_CELL_HDR + len;
}

void dect_cell_tlv_init(struct dect_cell_tlv *t, uint8_t *buf, size_t cap)
{
	t->buf = buf;
	t->cap = cap;
	t->len = 0;
	t->err = 0;
}

void dect_cell_tlv_put(struct dect_cell_tlv *t, uint8_t tag, const void *v,
		       size_t n)
{
	if (t->err || n > 0xFFFFu || t->cap - t->len < 3u + n) {
		t->err = 1;
		return;
	}
	t->buf[t->len] = tag;
	put16(t->buf + t->len + 1u, (uint16_t)n);
	if (n)
		memcpy(t->buf + t->len + 3u, v, n);
	t->len += 3u + n;
}

void dect_cell_tlv_u8(struct dect_cell_tlv *t, uint8_t tag, uint8_t v)
{
	dect_cell_tlv_put(t, tag, &v, 1);
}

void dect_cell_tlv_u16(struct dect_cell_tlv *t, uint8_t tag, uint16_t v)
{
	uint8_t b[2];

	put16(b, v);
	dect_cell_tlv_put(t, tag, b, sizeof(b));
}

void dect_cell_tlv_u32(struct dect_cell_tlv *t, uint8_t tag, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
			 (uint8_t)(v >> 8), (uint8_t)v };

	dect_cell_tlv_put(t, tag, b, sizeof(b));
}

void dect_cell_tlv_u64(struct dect_cell_tlv *t, uint8_t tag, uint64_t v)
{
	uint8_t b[8];
	unsigned i;

	for (i = 0; i < 8u; i++)
		b[i] = (uint8_t)(v >> (56u - 8u * i));
	dect_cell_tlv_put(t, tag, b, sizeof(b));
}

void dect_cell_tlv_str(struct dect_cell_tlv *t, uint8_t tag, const char *s)
{
	dect_cell_tlv_put(t, tag, s, strlen(s));
}

int dect_cell_tlv_check(const uint8_t *body, size_t len)
{
	size_t pos = 0;

	while (pos < len) {
		size_t n;

		if (len - pos < 3u)
			return -1;
		n = get16(body + pos + 1u);
		if (len - pos - 3u < n)
			return -1;
		pos += 3u + n;
	}
	return 0;
}

const uint8_t *dect_cell_tlv_next(const uint8_t *body, size_t len,
				  uint8_t tag, size_t *pos, size_t *n)
{
	while (*pos + 3u <= len) {
		size_t at = *pos, vn = get16(body + at + 1u);

		if (len - at - 3u < vn)
			return NULL;		/* not checked: stop */
		*pos = at + 3u + vn;
		if (body[at] == tag) {
			*n = vn;
			return body + at + 3u;
		}
	}
	return NULL;
}

const uint8_t *dect_cell_tlv_get(const uint8_t *body, size_t len,
				 uint8_t tag, size_t *n)
{
	const uint8_t *v, *last = NULL;
	size_t pos = 0, vn;

	while ((v = dect_cell_tlv_next(body, len, tag, &pos, &vn))) {
		last = v;
		*n = vn;
	}
	return last;
}

int dect_cell_tlv_get_u8(const uint8_t *body, size_t len, uint8_t tag,
			 uint8_t *v)
{
	size_t n;
	const uint8_t *p = dect_cell_tlv_get(body, len, tag, &n);

	if (!p || n != 1u)
		return -1;
	*v = p[0];
	return 0;
}

int dect_cell_tlv_get_u16(const uint8_t *body, size_t len, uint8_t tag,
			  uint16_t *v)
{
	size_t n;
	const uint8_t *p = dect_cell_tlv_get(body, len, tag, &n);

	if (!p || n != 2u)
		return -1;
	*v = get16(p);
	return 0;
}

int dect_cell_tlv_get_u32(const uint8_t *body, size_t len, uint8_t tag,
			  uint32_t *v)
{
	size_t n;
	const uint8_t *p = dect_cell_tlv_get(body, len, tag, &n);

	if (!p || n != 4u)
		return -1;
	*v = get32(p);
	return 0;
}

/* src | dst | prim (2) | plen | param, after rx_time on MAIL_UP */
static size_t mail_put(uint8_t *out, size_t cap,
		       const struct dect_cell_mail *m)
{
	if (m->plen > DECT_CELL_MAIL_PARAM_MAX || cap < 5u + m->plen)
		return 0;
	out[0] = m->src;
	out[1] = m->dst;
	put16(out + 2, m->prim);
	out[4] = (uint8_t)m->plen;
	if (m->plen)
		memcpy(out + 5, m->param, m->plen);
	return 5u + m->plen;
}

size_t dect_cell_mail_up(uint8_t *out, size_t cap,
			 const struct dect_cell_mail *m)
{
	size_t n;

	if (cap < 4u)
		return 0;
	out[0] = (uint8_t)(m->rx_time >> 24);
	out[1] = (uint8_t)(m->rx_time >> 16);
	out[2] = (uint8_t)(m->rx_time >> 8);
	out[3] = (uint8_t)m->rx_time;
	n = mail_put(out + 4, cap - 4u, m);
	return n ? 4u + n : 0;
}

size_t dect_cell_mail_down(uint8_t *out, size_t cap,
			   const struct dect_cell_mail *m)
{
	return mail_put(out, cap, m);
}

int dect_cell_mail_parse(const uint8_t *body, size_t len, int up,
			 struct dect_cell_mail *m)
{
	size_t off = up ? 4u : 0u;

	if (len < off + 5u)
		return -1;
	m->rx_time = up ? get32(body) : 0;
	body += off;
	len -= off;
	m->src = body[0];
	m->dst = body[1];
	m->prim = get16(body + 2);
	m->plen = body[4];
	/* §7.1: the body is exactly the header and plen octets */
	if (m->plen > DECT_CELL_MAIL_PARAM_MAX || len != 5u + m->plen)
		return -1;
	m->param = body + 5;
	return 0;
}

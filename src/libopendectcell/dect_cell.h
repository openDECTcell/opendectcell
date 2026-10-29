/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The cell <-> controller wire protocol, v1 (opendectcell-controller
 * docs/cell-protocol.md): frames, TLV bodies and the mail tunnel's fixed
 * layouts. dect-cell speaks it to the controller over TLS, and dect-stubd
 * speaks the same frames to dect-cell on a local socket (§14.2).
 *
 * Parsing and building only, no I/O. All integers on the wire are
 * big-endian.
 */
#ifndef OPENDECTCELL_DECT_CELL_H
#define OPENDECTCELL_DECT_CELL_H

#include <stddef.h>
#include <stdint.h>

#define DECT_CELL_PROTO_MAJOR	1u
#define DECT_CELL_PROTO_MINOR	0u

/* §3.1: length (2) | type (2) | corr (2) | body */
#define DECT_CELL_HDR		6u
#define DECT_CELL_BODY_MAX	4096u
#define DECT_CELL_FRAME_MAX	(DECT_CELL_HDR + DECT_CELL_BODY_MAX)

/* §3.3, the message types */
#define DECT_CELL_HELLO			0x0001u
#define DECT_CELL_HELLO_ACK		0x0002u
#define DECT_CELL_HEARTBEAT		0x0003u
#define DECT_CELL_HEARTBEAT_ECHO	0x0004u
#define DECT_CELL_GOODBYE		0x0005u
#define DECT_CELL_ERROR			0x0006u
#define DECT_CELL_FLOW			0x0007u
#define DECT_CELL_ENROLL_REQUEST	0x0010u
#define DECT_CELL_ENROLL_RESPONSE	0x0011u
#define DECT_CELL_MAIL_UP		0x0100u
#define DECT_CELL_MAIL_DOWN		0x0101u
#define DECT_CELL_MAIL_REJECT		0x0102u
#define DECT_CELL_CELL_STATE		0x0200u
#define DECT_CELL_CONFIG_SET		0x0201u
#define DECT_CELL_CONFIG_ACK		0x0202u
#define DECT_CELL_STATUS		0x0203u
#define DECT_CELL_STATUS_REQUEST	0x0204u
#define DECT_CELL_STATS_REQUEST		0x0205u
#define DECT_CELL_STATS			0x0206u
#define DECT_CELL_STATE_SNAPSHOT	0x0207u
#define DECT_CELL_RENEW_REQUEST		0x0210u
#define DECT_CELL_RENEW_RESPONSE	0x0211u
#define DECT_CELL_TRUST_UPDATE		0x0212u
#define DECT_CELL_TRUST_ACK		0x0213u
#define DECT_CELL_MEDIA_OPEN		0x0300u
#define DECT_CELL_MEDIA_OPEN_ACK	0x0301u
#define DECT_CELL_MEDIA_CLOSE		0x0302u
#define DECT_CELL_MEDIA_CLOSED		0x0303u

/* §6.1, HELLO tag 7 and HELLO_ACK tag 10 */
#define DECT_CELL_CAP_MAIL	(1u << 0)
#define DECT_CELL_CAP_MEDIA	(1u << 1)
#define DECT_CELL_CAP_SRTP	(1u << 2)
#define DECT_CELL_CAP_PLAIN_RTP	(1u << 3)
#define DECT_CELL_CAP_STATS	(1u << 4)
#define DECT_CELL_CAP_FLOW	(1u << 5)
#define DECT_CELL_CAP_RESUME	(1u << 6)

/* §6.2, cell states */
#define DECT_CELL_ST_PENDING	0u
#define DECT_CELL_ST_ACTIVE	1u
#define DECT_CELL_ST_DISABLED	2u
#define DECT_CELL_ST_REVOKED	3u

/* §11, ERROR codes and GOODBYE reasons */
#define DECT_CELL_ERR_UNKNOWN_TYPE	1u
#define DECT_CELL_ERR_MALFORMED		2u
#define DECT_CELL_ERR_TOO_LARGE		3u
#define DECT_CELL_ERR_NOT_ALLOWED	4u
#define DECT_CELL_ERR_UNSUPPORTED	5u
#define DECT_CELL_ERR_BUSY		6u
#define DECT_CELL_ERR_MEDIA_FAILED	7u
#define DECT_CELL_ERR_INTERNAL		8u

#define DECT_CELL_BYE_SHUTDOWN		1u
#define DECT_CELL_BYE_PROTOCOL		2u
#define DECT_CELL_BYE_VERSION		3u
#define DECT_CELL_BYE_REVOKED		4u
#define DECT_CELL_BYE_SUPERSEDED	5u
#define DECT_CELL_BYE_OVERLOAD		6u
#define DECT_CELL_BYE_AUTH		7u

/* §7.2, MAIL_REJECT reasons */
#define DECT_CELL_REJ_CHIP_DOWN		1u
#define DECT_CELL_REJ_QUEUE_FULL	2u
#define DECT_CELL_REJ_CHIP_REFUSED	3u

/* §8.2, audio formats */
#define DECT_CELL_FMT_L16_8K	1u
#define DECT_CELL_FMT_G722	6u

/* §7.1: a mail's parameters, at most 249 octets */
#define DECT_CELL_MAIL_PARAM_MAX	249u

struct dect_cell_frame {
	uint16_t type;
	uint16_t corr;
	const uint8_t *body;
	size_t len;
};

/*
 * The largest body a type may carry (§3.3), or 0 for a type this side does
 * not know.
 */
size_t dect_cell_body_max(uint16_t type);

/*
 * One frame from the start of buf (n octets available). Returns the octets
 * it takes (header and body), 0 when more octets are needed, or -1 when the
 * length field is out of range and the framing is lost (§3.4). The body
 * points into buf. A body over its type's bound is still returned: the
 * caller answers it with ERROR TOO_LARGE and keeps the link.
 */
long dect_cell_parse(const uint8_t *buf, size_t n, struct dect_cell_frame *f);

/* Header and body into out. Returns the frame's length, or 0 if it does
 * not fit or the body is over DECT_CELL_BODY_MAX. */
size_t dect_cell_put(uint8_t *out, size_t cap, uint16_t type, uint16_t corr,
		     const uint8_t *body, size_t len);

/*
 * A TLV body (§3.2): tag u8 | len u16 | value. Writing past cap sets err
 * and writes nothing more, so a builder checks once at the end.
 */
struct dect_cell_tlv {
	uint8_t *buf;
	size_t cap, len;
	int err;
};

void dect_cell_tlv_init(struct dect_cell_tlv *t, uint8_t *buf, size_t cap);
void dect_cell_tlv_put(struct dect_cell_tlv *t, uint8_t tag, const void *v,
		       size_t n);
void dect_cell_tlv_u8(struct dect_cell_tlv *t, uint8_t tag, uint8_t v);
void dect_cell_tlv_u16(struct dect_cell_tlv *t, uint8_t tag, uint16_t v);
void dect_cell_tlv_u32(struct dect_cell_tlv *t, uint8_t tag, uint32_t v);
void dect_cell_tlv_u64(struct dect_cell_tlv *t, uint8_t tag, uint64_t v);
void dect_cell_tlv_str(struct dect_cell_tlv *t, uint8_t tag, const char *s);

/* 0 if body is a well-formed TLV sequence, else -1. */
int dect_cell_tlv_check(const uint8_t *body, size_t len);

/*
 * Tag tag's value in a well-formed TLV body (dect_cell_tlv_check()): the
 * last one when it repeats (§3.2), or NULL. *n is its length.
 */
const uint8_t *dect_cell_tlv_get(const uint8_t *body, size_t len,
				 uint8_t tag, size_t *n);

/*
 * Walk a repeatable tag: *pos starts at 0. Returns each value in order, or
 * NULL after the last.
 */
const uint8_t *dect_cell_tlv_next(const uint8_t *body, size_t len,
				  uint8_t tag, size_t *pos, size_t *n);

/* Fixed-width values of a tag; -1 if absent or of the wrong length. */
int dect_cell_tlv_get_u8(const uint8_t *body, size_t len, uint8_t tag,
			 uint8_t *v);
int dect_cell_tlv_get_u16(const uint8_t *body, size_t len, uint8_t tag,
			  uint16_t *v);
int dect_cell_tlv_get_u32(const uint8_t *body, size_t len, uint8_t tag,
			  uint32_t *v);

/* §7.1, the mail tunnel's fixed layouts */
struct dect_cell_mail {
	uint32_t rx_time;	/* MAIL_UP only: µs, the cell's monotonic clock */
	uint8_t  src, dst;
	uint16_t prim;
	const uint8_t *param;
	size_t   plen;
};

/* MAIL_UP's body into out; its length, or 0 if it does not fit. */
size_t dect_cell_mail_up(uint8_t *out, size_t cap,
			 const struct dect_cell_mail *m);

/* A MAIL_DOWN body (up = 0) or MAIL_UP body (up = 1); 0 or -1. */
int dect_cell_mail_parse(const uint8_t *body, size_t len, int up,
			 struct dect_cell_mail *m);

/* MAIL_DOWN's body into out; its length, or 0 if it does not fit. */
size_t dect_cell_mail_down(uint8_t *out, size_t cap,
			   const struct dect_cell_mail *m);

#endif /* OPENDECTCELL_DECT_CELL_H */

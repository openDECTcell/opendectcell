/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STUB-facing C-plane transport: the glue between STUB connection mails and
 * the host DLC layer. The STUB runs only the radio and MAC; the whole DECT
 * C-plane stack (DLC, NWK, MM, authentication) runs on the host
 * [CR COMMAND_SET line 68].
 *
 * Inbound, the STUB delivers the Cs logical channel one 5-byte A-field segment
 * at a time as 0x0203 SIGNALLING_SEGMENT mails [CR COMMAND_SET §8.2]; this
 * module extracts {handle, tag, segment} so the DLC layer can reassemble LAPC
 * frames. Outbound, the host appends a byte stream to the connection's buffer
 * with 0x0244 APPEND_CONNECTION_DATA [CR COMMAND_SET §6.4]; the STUB segments
 * it into Cs and sends it. **Do not follow it with 0x0202 TRIGGER_SIGNALLING.**
 * COMMAND_SET §6.4 describes 0x0202 as the flush, but on hardware it drops
 * the connection within ~40 ms, and AVM's own host sends 0x0244 alone.
 * 0x0244 is sufficient and self-flushing.
 *
 * Pure encode/decode on struct dect_mail. No I/O, no clock. The Lc and LAPC
 * frame formats (ETSI EN 300 175-4) live in dect_lc and dect_lapc, not here.
 */
#ifndef OPENDECTCELL_DECT_CS_H
#define OPENDECTCELL_DECT_CS_H

#include <stddef.h>
#include <stdint.h>

#include "dect_mail.h"

#define DECT_CS_SEG_LEN		5u	/* one Cs A-field tail segment */
#define DECT_CS_TAG_LEN		3u	/* handset tag (PMID-like) */
#define DECT_CS_MIN_PARAM	13u	/* {handle,01,seg[5],00,00,tag[3],handle} */
#define DECT_CS_SHORT_PARAM	7u	/* {handle,01,seg[5]}: the 06.88 STUB */

/* One inbound Cs segment, decoded from a 0x0203 mail. */
struct dect_cs_seg {
	uint8_t handle;			/* connection handle (terminal index 0..6) */
	uint8_t tag[DECT_CS_TAG_LEN];	/* handset tag; zero in the short form */
	uint8_t data[DECT_CS_SEG_LEN];	/* the 5-byte Cs channel segment */
};

/*
 * Decode a 0x0203 SIGNALLING_SEGMENT mail into seg. Returns 0 on success,
 * -1 if m is not a signalling segment or is too short. Accepts the documented
 * 13-byte layout and the longer (15-byte, with a 2-byte ref trailer) form seen
 * on hardware; only the fields above are read. Also the 7-byte short form
 * {handle,01,seg[5]} of the 7412's 06.88 STUB, which has no tag.
 */
int dect_cs_decode_segment(const struct dect_mail *m, struct dect_cs_seg *seg);

/*
 * Build an APPEND_CONNECTION_DATA (0x0244) mail carrying n bytes of outbound
 * C-plane data for the given connection handle. src is our task (DECT_TASK_PCM),
 * dst is DECT_TASK_CONTACT_DST. Returns 0 on success, -1 if n is 0 or exceeds
 * DECT_APPEND_BUF_MAX. Send it on its own: it needs no flush, and a
 * TRIGGER_SIGNALLING (0x0202) after it drops the connection.
 */
int dect_cs_append_mail(struct dect_mail *m, uint8_t src,
			uint8_t handle, const uint8_t *data, size_t n);

#endif /* OPENDECTCELL_DECT_CS_H */

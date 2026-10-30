/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The libopendectcell API version, and the STUB control-channel task and
 * primitive identifiers the library itself uses. The full primitive table,
 * with sources, is docs/stub-api.md; each value here carries its source tag.
 */
#ifndef OPENDECTCELL_DECT_PROTO_H
#define OPENDECTCELL_DECT_PROTO_H

/*
 * libopendectcell API version. A single integer, bumped on ANY change to a public
 * function signature or a public struct layout -- not on behaviour changes and
 * not on comments. Consumers compiling against a pinned checkout can refuse to
 * build against an API they were not written for:
 *
 *	#if OPENDECTCELL_API_VERSION != 88
 *	#error libopendectcell API version mismatch
 *	#endif
 *
 * It tells you the API SHAPE changed, not that behaviour changed underneath
 * a stable signature: treat it as a build-time guard. The record of each
 * version is docs/api-versions.md.
 */
#define OPENDECTCELL_API_VERSION 88

/* Task identifiers (the src/dst bytes of a mail). */
#define DECT_TASK_CONTACT_DST	0x02u	/* contact destination task [CR A-0003] */
#define DECT_TASK_CPLANE	0x03u	/* C-plane / DLC events [CR COMMAND_SET §8.2] */
#define DECT_TASK_PCM		0x22u	/* PCM task, host side (observed; 7520 analysis) */
#define DECT_TASK_PCM_REMOTE	0x23u	/* PCM task, remote side (observed) */

/* Primitive identifiers (mail offset 4, see dect_mail_set_swap()). */
#define DECT_PRIM_CONTACT	0xF600u	/* host -> STUB contact (observed) */
#define DECT_PRIM_CONTACT_ACK1	0xF601u	/* STUB answer 1, 5 params (observed) */
#define DECT_PRIM_CONTACT_ACK2	0xF602u	/* STUB answer 2, 23 params (observed) */
#define DECT_PRIM_STATUS	0xFFF1u	/* periodic status / liveness (observed) */
#define DECT_PRIM_BUILD_STAMP	0xFFF2u	/* ASCII STUB build stamp (observed) */

/* STUB -> host connection events, destination task 0x03 [CR COMMAND_SET §8.2]. */
#define DECT_PRIM_CONN_UP	0x0200u	/* CONNECTION_ESTABLISHED */
#define DECT_PRIM_CONN_DOWN	0x0201u	/* CONNECTION_RELEASED */
#define DECT_PRIM_SIGNALLING	0x0203u	/* 5-byte NWK/Cs signalling segment */

/*
 * Host -> STUB outbound C-plane data, destination task 0x02
 * [CR COMMAND_SET §6.4]: appended to the per-connection buffer, which the
 * STUB segments and sends by itself (see dect_cs.h).
 */
#define DECT_PRIM_APPEND_DATA	0x0244u	/* APPEND_CONNECTION_DATA: {handle,n,data[n]} */

/* APPEND_CONNECTION_DATA: per-connection buffer size, data dropped if it won't fit. */
#define DECT_APPEND_BUF_MAX	205u	/* [CR COMMAND_SET §6.4] */

#endif /* OPENDECTCELL_DECT_PROTO_H */

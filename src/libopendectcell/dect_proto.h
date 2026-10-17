/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * STUB control-channel task and primitive identifiers.
 * Sources: docs/stub-pcm-control-channel.md ("Verified on hardware"),
 * docs/models/7520-DECT-analysis.md, clean-room answer A-0003. Only values
 * that are documented there are listed; each carries its source tag.
 */
#ifndef OPENDECTCELL_DECT_PROTO_H
#define OPENDECTCELL_DECT_PROTO_H

/* Task identifiers (the src/dst bytes of a mail). */
#define DECT_TASK_UI		0x14u	/* UI task (observed; 7520 analysis) */
#define DECT_TASK_CONTACT_DST	0x02u	/* contact destination task [CR A-0003] */
#define DECT_TASK_PCM		0x22u	/* PCM task, host side (observed; 7520 analysis) */
#define DECT_TASK_PCM_REMOTE	0x23u	/* PCM task, remote side (observed) */
#define DECT_TASK_MAC_INT	0x26u	/* MAC_INTERRUPT_TASK (7520 analysis) */

/* Primitive identifiers (mail offset 4, little-endian on the wire). */
#define DECT_PRIM_CONTACT	0xF600u	/* host -> STUB contact (observed) */
#define DECT_PRIM_CONTACT_ACK1	0xF601u	/* STUB answer 1, 5 params (observed) */
#define DECT_PRIM_CONTACT_ACK2	0xF602u	/* STUB answer 2, 23 params (observed) */
#define DECT_PRIM_STATUS	0xFFF1u	/* periodic status / liveness (observed) */
#define DECT_PRIM_BUILD_STAMP	0xFFF2u	/* ASCII STUB build stamp (observed) */
#define DECT_PRIM_F618		0xF618u	/* follows a status mail (observed) */

/*
 * 0xFFF1 is a shared debug/status primitive selected by a sub-command byte
 * (the first parameter). STUB->host status uses sub-command 4; host->STUB
 * GenDSP audio uses the sub-commands below [CR A-0004].
 */
#define DECT_PRIM_GENDSP	0xFFF1u	/* host->STUB GenDSP debug (= 0xFFF1) */
#define DECT_SUB_CONNECT	0x0Au	/* params: chn_dst, chn_src */
#define DECT_SUB_LOOP		0x09u	/* params: 32-bit LE channel (1..3 on, 0 off) */
#define DECT_SUB_STATUS		0x04u	/* STUB->host periodic status */

/* GenDSP audio channels [CR A-0004] (also 7520 analysis). */
#define DECT_AUDIO_CHN_PCM0	4u	/* PCM pass-through channels 4..6 */
#define DECT_AUDIO_CHN_SILENCE	12u
#define DECT_AUDIO_CHN_TESTTONE	15u

#endif /* OPENDECTCELL_DECT_PROTO_H */

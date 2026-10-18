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
#define DECT_TASK_CPLANE	0x03u	/* C-plane / DLC events [CR COMMAND_SET §8.2] */
#define DECT_TASK_AUDIO		0x10u	/* audio manager events [CR COMMAND_SET §8.4] */
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

/*
 * MAC / radio control and registration, destination task 0x02.
 * Sources: clean-room firmware spec [CR COMMAND_SET] (reconstructed from the
 * SC14441 STUB images, §6.1/§6.2); each verified on hardware 2026-10-18 (G5).
 */
#define DECT_PRIM_RADIO_ON_OFF	0x015Bu	/* 1 param: 1 = radio/beacon on, 0 = off */
#define DECT_RADIO_ON		0x01u
#define DECT_RADIO_OFF		0x00u
#define DECT_PRIM_OPEN_REG	0x022Bu	/* data[0]='A' (0x41) sets a44, opens reg */
#define DECT_OPEN_REG_A		0x41u	/* 'A': "access rights requests supported" */
#define DECT_PRIM_CLOSE_REG	0x022Cu	/* 1 param: !=0 clears a44 (closes reg) */

/* STUB -> host MAC/connection events seen during a handset access (task varies). */
#define DECT_PRIM_RADIO_STATUS	0xF623u	/* periodic while radio on [CR COMMAND_SET §8.1] */
#define DECT_PRIM_CONN_UP	0x0200u	/* CONNECTION_ESTABLISHED (dst 0x03) */
#define DECT_PRIM_CONN_DOWN	0x0201u	/* CONNECTION_RELEASED (dst 0x03) */
#define DECT_PRIM_SIGNALLING	0x0203u	/* 5-byte NWK/Cs signalling segment (dst 0x03) */
#define DECT_PRIM_CONN_PARAM	0x0243u	/* CONNECTION_PARAMETER (dst 0x03) [CR §8.2] */
#define DECT_PRIM_AUDIO_STATE	0x0242u	/* audio channel state (dst 0x10) [CR §8.4] */
#define DECT_PRIM_ACCESS_EVENT	0xF604u	/* HANDSET_ACCESS_EVENT (short-form access) */
#define DECT_PRIM_ACCESS_CHAL	0x0E0Du	/* ACCESS_CHALLENGE_DATA (dst 0x05) */

/*
 * Host -> STUB C-plane / connection commands (destination task 0x02),
 * [CR COMMAND_SET §6.4]. The STUB runs only the radio/MAC; DLC, NWK, MM and
 * authentication all run here on the host [CR COMMAND_SET line 68]. Outbound
 * C-plane signalling is appended to the per-connection buffer with 0x0244 and
 * then flushed to air with 0x0202.
 */
#define DECT_PRIM_TRIGGER_SIG	0x0202u	/* TRIGGER_SIGNALLING: flush pending C-plane */
#define DECT_PRIM_APPEND_DATA	0x0244u	/* APPEND_CONNECTION_DATA: {handle,n,data[n]} */
#define DECT_PRIM_SEND_ACKED	0x015Cu	/* SEND_ACKED_DATA: 40-byte B-field, by tag */
#define DECT_PRIM_ACKED_CONFIRM	0x015Du	/* ACKED_DATA_CONFIRM reply (task 0x14) */
#define DECT_PRIM_RELEASE_CONN	0x017Au	/* RELEASE_CONNECTION: {handle, reason} */
#define DECT_PRIM_ASSIGN_AUDIO	0x0230u	/* ASSIGN_AUDIO_CHANNEL: {handle, chan_flags} */

/* APPEND_CONNECTION_DATA: per-connection buffer size, data dropped if it won't fit. */
#define DECT_APPEND_BUF_MAX	205u	/* [CR COMMAND_SET §6.4] */

#endif /* OPENDECTCELL_DECT_PROTO_H */

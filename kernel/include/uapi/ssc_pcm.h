/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copy of kmod-lantiq-ssc-pcm's src/include/uapi/ssc_pcm.h (uapi version 3),
 * the generic Lantiq SSC PCM transport dect-stubd uses on the 7412. Keep it
 * in step with that repository; do not change it here.
 */
/*
 * uapi for the Lantiq VR9 SSC "SPI-TDM" transport.
 *
 * The controller is used as a PCM/TDM slave: a peer device supplies the bit
 * clock and frame sync, and the SSC shifts fixed-size frames in and out. This
 * is a generic transport interface; it knows nothing about the peer device.
 */
#ifndef _UAPI_SSC_PCM_H
#define _UAPI_SSC_PCM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SSC_PCM_IOC_VERSION	3U

/*
 * Bring-up is an explicit step sequence so a test can stop before the step
 * that misbehaves. SSC_PCM_IOC_STEP(n) performs step n; n must be the current
 * step + 1. Step 0 undoes everything from any step.
 *
 *   1 CLOCK  module clock on, controller registers readable
 *   2 IFACE  pins muxed to the controller (done by pinctrl where described)
 *   3 TDM    slave framing and FIFOs configured
 *   4 DMA    engine running: blocks become readable, written frames are
 *            sent (the idle frame when none are queued)
 */
#define SSC_PCM_STEP_OFF		0U
#define SSC_PCM_STEP_CLOCK	1U
#define SSC_PCM_STEP_IFACE	2U
#define SSC_PCM_STEP_TDM		3U
#define SSC_PCM_STEP_DMA		4U

struct ssc_pcm_info {
	__u32 version;
	__u32 step;
	__u32 slots;		/* 16-bit slots per frame */
	__u32 frame_bytes;
	char name[32];
	__u32 reserved[4];
};

/* Fields that are 0 on input take the default; the applied values return. */
struct ssc_pcm_config {
	__u32 slots;		/* 2..64, even, default 8 */
	__u32 frames_per_block;	/* default 32 */
	__u32 n_desc;		/* ring slots, 2..16, default 4 */
	__u32 flags;
	__u32 reserved[3];
};

/* Register dump, read back from the controller. */
struct ssc_pcm_regs {
	__u32 step;
	__u32 valid;
	__u32 reg[8];
	__u32 reserved[4];
};

struct ssc_pcm_stats {
	__u32 step;
	__u32 block_bytes;
	__aligned_u64 frames;	/* complete frames received */
	__aligned_u64 blocks;	/* blocks completed */
	__aligned_u64 dropped;	/* blocks overwritten before read() */
	__u32 overruns;		/* RX FIFO overflows: received words lost */
	__u32 tx_queued;	/* frames waiting in the transmit queue */
	__aligned_u64 tx_frames;	/* written frames sent */
	__aligned_u64 tx_idle;	/* idle frames sent */
	__u32 tx_underruns;	/* TX FIFO underflows: the bus got no word */
	__u32 rx_irqs;		/* engine runs from the RX interrupt line */
	__u32 tx_irqs;		/* engine runs from the TX interrupt line */
	__u32 polls;		/* engine runs from the fallback timer */
};

/* Frame sent whenever no written frame is queued; zeros by default. */
#define SSC_PCM_MAX_FRAME_BYTES	128	/* 64 slots */
struct ssc_pcm_tx_idle {
	__u8 frame[SSC_PCM_MAX_FRAME_BYTES];	/* first frame_bytes used */
};

#define SSC_PCM_IOC_MAGIC	'S'
#define SSC_PCM_IOC_INFO		_IOR(SSC_PCM_IOC_MAGIC, 0x00, struct ssc_pcm_info)
#define SSC_PCM_IOC_CONFIG	_IOWR(SSC_PCM_IOC_MAGIC, 0x01, struct ssc_pcm_config)
#define SSC_PCM_IOC_STEP		_IOW(SSC_PCM_IOC_MAGIC, 0x02, __u32)
#define SSC_PCM_IOC_REGS		_IOR(SSC_PCM_IOC_MAGIC, 0x03, struct ssc_pcm_regs)
#define SSC_PCM_IOC_STATS	_IOR(SSC_PCM_IOC_MAGIC, 0x04, struct ssc_pcm_stats)
#define SSC_PCM_IOC_TX_IDLE	_IOW(SSC_PCM_IOC_MAGIC, 0x05, struct ssc_pcm_tx_idle)

/*
 * read() returns whole blocks: a ssc_pcm_block header followed by len payload
 * bytes, raw as the controller wrote them. The buffer must hold one block.
 * seq numbers each block as it completes, so a gap means lost blocks; the
 * first block after a gap also carries SSC_PCM_BF_DROPPED. A block is only
 * produced from words actually received. Blocking unless O_NONBLOCK;
 * -ENODATA when the engine is not running. poll() reports EPOLLIN.
 *
 * write() queues whole frames (count a multiple of frame_bytes, same byte
 * order as read()). They are sent in order, back to back; when the queue is
 * empty the idle frame is sent. Accepts as many whole frames as fit and
 * returns the bytes taken; blocks for space unless O_NONBLOCK. -ENODATA when
 * the engine is not running. poll() reports EPOLLOUT when a frame fits.
 */
#define SSC_PCM_BLOCK_MAGIC	0x53445054U	/* "TPDS" */
#define SSC_PCM_BF_DROPPED	(1U << 0)

struct ssc_pcm_block {
	__u32 magic;
	__u32 seq;
	__u32 flags;
	__u32 len;
	__aligned_u64 ts_ns;
	__u32 reserved[2];
};

#endif /* _UAPI_SSC_PCM_H */

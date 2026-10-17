/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UAPI_OPENDECTCELL_H
#define _UAPI_OPENDECTCELL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* v2: compatible extension of v1 with the PCM receive path (Phase B). */
#define DECT_IOC_VERSION	2U
#define DECT_STAGE_A		1U
#define DECT_STAGE_B		2U

#define DECT_FEATURE_RESET	(1U << 0)
#define DECT_FEATURE_STRAP	(1U << 1)
#define DECT_FEATURE_GPIO_CAPTURE (1U << 2)
#define DECT_FEATURE_PCM_RX	(1U << 3)

struct dect_ioc_info {
	__u32 version;
	__u32 stage;
	__u32 features;
	__u32 reserved0;
	char board[32];
	char transport[32];
	__u32 reserved[4];
};

/*
 * Diagnostic only: byte per observed DCL rising edge.
 * bit 0 DCL (always 1); bit 1 FSC; bit 2 DI; bit 3 DO.
 * Signals are sampled separately, not atomically.
 *
 * The ioctl uses a 64-bit userspace pointer rather than an embedded array:
 * a 16-KiB _IOWR structure would exceed the ioctl size encoding limit.
 *
 * Input: duration_us (1..8000), sample_capacity (>= 16384), samples_ptr.
 * Output: all other fields (reserved[] is input-zero / output-zero).
 * Output data consists of sample_count bytes at samples_ptr. The remaining
 * buffer is unchanged. A 1.024 MHz DCL cannot yield 16 Ki samples in 8 ms.
 *
 * Calibration measures MMIO access throughput and observes DCL edges.
 * If NO_CLOCK or TOO_SLOW is set, sample_count is zero. INCOMPLETE means
 * the observed edge count is appreciably below the calibrated DCL rate.
 * None of these metrics establishes lossless signal capture.
 */
#define DECT_CAPTURE_BUFFER_BYTES	16384U
#define DECT_CAPTURE_MAX_US		8000U

#define DECT_CAPTURE_F_NO_CLOCK		(1U << 0)
#define DECT_CAPTURE_F_TOO_SLOW		(1U << 1)
#define DECT_CAPTURE_F_INCOMPLETE	(1U << 2)

struct dect_ioc_capture {
	__u32 duration_us;
	__u32 sample_capacity;
	__aligned_u64 samples_ptr;
	__u32 flags;
	__u32 sample_count;
	__u32 ns_per_read;
	__u32 dcl_period_ns;
	__u32 reads_per_period;
	__u32 reads_per_half_period;
	__u32 observed_clock_hz;
	__u32 reserved0;
	__aligned_u64 start_ns;
	__aligned_u64 end_ns;
	__u32 reserved[4];
};

/*
 * DECT_IOC_INFO:    read dect_ioc_info (stage B: PCM receive available).
 * DECT_IOC_RESET:   write __u32: 1 = assert reset (GPIO low), 0 = release.
 * DECT_IOC_STRAP:   write __u32: 1 = drive strap low, 0 = restore UART.
 * DECT_IOC_CAPTURE: read/write dect_ioc_capture; input-only GPIO polling.
 * Other RESET/STRAP values return -EINVAL. Timing/baud stay in userspace.
 */
#define DECT_IOC_MAGIC	'D'
#define DECT_IOC_INFO	_IOR(DECT_IOC_MAGIC, 0x00, struct dect_ioc_info)
#define DECT_IOC_RESET	_IOW(DECT_IOC_MAGIC, 0x01, __u32)
#define DECT_IOC_STRAP	_IOW(DECT_IOC_MAGIC, 0x02, __u32)
#define DECT_IOC_CAPTURE _IOWR(DECT_IOC_MAGIC, 0x03, struct dect_ioc_capture)

/*
 * PCM receive path (Phase B). The SoC is a TDM slave of the DECT chip's
 * clocks, which only run while the STUB runs.
 *
 * Bring-up is an explicit sequence of steps, so that a test can stop
 * before the step that misbehaves. DECT_IOC_PCM_STEP(n) performs step n;
 * n must be the current step + 1 (step 5 may be skipped). Step 0 undoes
 * everything, from any step. close() and module unload do the same.
 *
 *   1 CLOCK   take the SoC's audio bus clock; ADSS registers readable
 *   2 IFACE   interface enable, audio bus branches on
 *   3 PADCLK  I2S block reset; receive/transmit clocks from the pads
 *   4 TDM     TDM slave configuration, stereo block, pins 20-23 to audio
 *   5 STEREO  receiver enabled without DMA (sample counter should run)
 *   6 DMA     DMA ring started; blocks become readable
 *
 * DECT_IOC_PCM_CONFIG (only at step 0): fields that are 0 on input take
 * the default; the effective configuration is returned. A block holds
 * frames_per_block frames of slots 16-bit words (at most 4095 bytes).
 */
#define DECT_PCM_STEP_OFF	0U
#define DECT_PCM_STEP_CLOCK	1U
#define DECT_PCM_STEP_IFACE	2U
#define DECT_PCM_STEP_PADCLK	3U
#define DECT_PCM_STEP_TDM	4U
#define DECT_PCM_STEP_STEREO	5U
#define DECT_PCM_STEP_DMA	6U

struct dect_pcm_config {
	__u32 slots;		/* 16-bit slots per frame, 1..16; default 8 */
	__u32 rx_delay;		/* data delay in bit clocks, 0 or 1; default 1 */
	__u32 tx_delay;		/* same for transmit; default 1 */
	__u32 frames_per_block;	/* default 32 (4 ms) */
	__u32 n_desc;		/* DMA descriptors/buffers, 2..16; default 4 */
	__u32 flags;		/* input: DECT_PCM_CF_*; output: as applied */
	__u32 reserved[2];
};

/* Input flag: rx_delay = 0 was meant literally (0 means default otherwise). */
#define DECT_PCM_CF_RX_DELAY_SET	(1U << 0)
#define DECT_PCM_CF_TX_DELAY_SET	(1U << 1)
/* Also run the transmit DMA (pin 27 driven); see DECT_IOC_PCM_TX_FRAME. */
#define DECT_PCM_CF_TX			(1U << 2)

/*
 * Register dump, read back from the hardware. valid tells which groups were
 * read; a group is skipped while its bus clock may be off (reading it could
 * hang the bus). tlmm[] is always valid.
 */
#define DECT_PCM_REGS_GLB	(1U << 0)	/* glb[] and adcc[] */
#define DECT_PCM_REGS_STEREO	(1U << 1)
#define DECT_PCM_REGS_MBOX	(1U << 2)

struct dect_pcm_regs {
	__u32 step;
	__u32 valid;
	/*
	 * ADSS: 0x10 CHIP_CTRL_I2S, 0x14 I2S_RST, 0x18 CLK_I2S_CTRL,
	 * 0x1C TDM_CTRL, 0x30 AUDIO_MODE
	 */
	__u32 glb[5];
	/*
	 * ADCC, ADSS offsets 0x104 0x108 0x10C 0x124 0x128 0x12C (RX BCLK,
	 * MCLK), 0x144 ... 0x16C (TX), 0x200 0x204 0x208 0x20C 0x210 (AHB)
	 */
	__u32 adcc[17];
	/* receive stereo block, offsets 0x00 .. 0x28 */
	__u32 stereo[11];
	/*
	 * receive MBOX, offsets 0x10 0x18 0x1C 0x20 0x24 0x38 0x40 0x44
	 * 0x4C 0x58
	 */
	__u32 mbox[10];
	/* TLMM control registers of GPIO 20..27 */
	__u32 tlmm[8];
	__u32 reserved[4];
};

struct dect_pcm_stats {
	__u32 step;
	__u32 block_bytes;
	__aligned_u64 irqs;		/* MBOX interrupts handled */
	__aligned_u64 blocks;		/* completed descriptors */
	__aligned_u64 dropped;		/* blocks lost: read() too slow */
	__aligned_u64 multi;		/* interrupts with > 1 block */
	__aligned_u64 empty;		/* interrupts with no block */
	__aligned_u64 errors;		/* interrupts with error bits */
	__u32 last_error_status;	/* MBOX INT_STATUS of the last one */
	__u32 int_status;		/* live MBOX INT_STATUS (if running) */
	__u32 rx_samples_lsb;		/* live stereo counters (step >= 4) */
	__u32 rx_samples_msb;
	__u32 overruns;
	__u32 tx_irqs;			/* transmit MBOX interrupts */
	__u32 tx_blocks;		/* transmit buffers refilled */
	__u32 reserved[3];
};

/*
 * Transmit content: every frame of every transmit buffer is a copy of
 * frame[0 .. slots * 2), in DMA-buffer order (the same order read() shows
 * for receive). Default all 0xFF (idle). Takes effect at the next refill.
 */
struct dect_pcm_tx_frame {
	__u32 len;			/* must be slots * 2 */
	__u32 reserved;
	__u8 frame[32];
};

/*
 * read() returns whole blocks: a dect_pcm_block header followed by len
 * payload bytes, raw as the DMA wrote them. The buffer must hold at least
 * one block. seq counts completed DMA blocks, so a gap means lost blocks
 * (also flagged). Blocking unless O_NONBLOCK; -ENODATA when not at step 6
 * and nothing is queued. poll() reports EPOLLIN.
 */
#define DECT_PCM_BLOCK_MAGIC	0x4d435044U	/* "DPCM" */
#define DECT_PCM_BF_DROPPED	(1U << 0)	/* blocks lost before this */

struct dect_pcm_block {
	__u32 magic;
	__u32 seq;
	__u32 flags;
	__u32 len;
	__aligned_u64 ts_ns;		/* ktime_get_ns() in the interrupt */
	__u32 desc;			/* descriptor index */
	__u32 desc_ctrl;		/* descriptor word 0 as completed */
};

#define DECT_IOC_PCM_CONFIG _IOWR(DECT_IOC_MAGIC, 0x10, struct dect_pcm_config)
#define DECT_IOC_PCM_STEP   _IOW(DECT_IOC_MAGIC, 0x11, __u32)
#define DECT_IOC_PCM_REGS   _IOR(DECT_IOC_MAGIC, 0x12, struct dect_pcm_regs)
#define DECT_IOC_PCM_STATS  _IOR(DECT_IOC_MAGIC, 0x13, struct dect_pcm_stats)
#define DECT_IOC_PCM_TX_FRAME _IOW(DECT_IOC_MAGIC, 0x14, struct dect_pcm_tx_frame)

#endif /* _UAPI_OPENDECTCELL_H */

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UAPI_OPENDECTCELL_H
#define _UAPI_OPENDECTCELL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Compatible extension of the Phase A v1 interface. */
#define DECT_IOC_VERSION	1U
#define DECT_STAGE_A		1U

#define DECT_FEATURE_RESET	(1U << 0)
#define DECT_FEATURE_STRAP	(1U << 1)
#define DECT_FEATURE_GPIO_CAPTURE (1U << 2)

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
 * DECT_IOC_INFO:    read dect_ioc_info (stage stays A: DMA is not done).
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

#endif /* _UAPI_OPENDECTCELL_H */

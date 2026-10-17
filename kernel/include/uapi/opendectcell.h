/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UAPI_OPENDECTCELL_H
#define _UAPI_OPENDECTCELL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DECT_IOC_VERSION	1U
#define DECT_STAGE_A		1U

/* INFO feature bits; no PCM/TDM functionality is present in Phase A. */
#define DECT_FEATURE_RESET	(1U << 0)
#define DECT_FEATURE_STRAP	(1U << 1)

/* Fixed-size, pointer-free layout; reserved words must be ignored by v1. */
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
 * DECT_IOC_INFO:  read dect_ioc_info.
 * DECT_IOC_RESET: write __u32: 1 = assert reset (GPIO low), 0 = release.
 * DECT_IOC_STRAP: write __u32: 1 = drive boot strap low, 0 = restore UART.
 * Passing a value other than 0 or 1 returns -EINVAL.
 * Sleeps and baud-rate handling remain in userspace.
 */
#define DECT_IOC_MAGIC	'D'
#define DECT_IOC_INFO	_IOR(DECT_IOC_MAGIC, 0x00, struct dect_ioc_info)
#define DECT_IOC_RESET	_IOW(DECT_IOC_MAGIC, 0x01, __u32)
#define DECT_IOC_STRAP	_IOW(DECT_IOC_MAGIC, 0x02, __u32)

#endif /* _UAPI_OPENDECTCELL_H */

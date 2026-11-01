/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace ABI for the gxfp5130 Linux transport driver.
 *
 * The transport mirrors the de-facto eSPI mailbox ABI used by the GXFP5130
 * userspace stack:
 *
 *   write(2)  submit one complete eSPI-wrapped frame (max GXFP_TX_MAX
 *             bytes).  The kernel copies it into the TX window and pulses
 *             write-done; framing is built by userspace.
 *
 *   read(2)   one raw RX window snapshot, exactly GXFP_RX_RECORD_SIZE
 *             bytes: struct gxfp_rx_record followed by GXFP_RX_MAX bytes.
 *             Each doorbell interrupt produces one record.
 *
 *   poll(2)   POLLIN while a record is queued.
 *
 *   ioctl     GXFP_IOCTL_FLUSH_RX drops queued records.  Debug ioctls
 *             (info/peek/pulse/setline) use magic 'G'.
 */
#ifndef _UAPI_LINUX_GXFP5130_H
#define _UAPI_LINUX_GXFP5130_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define GXFP_TX_MAX		0x200u
#define GXFP_RX_MAX		0xE00u

struct gxfp_rx_record {
	__u32 len;		/* valid bytes in data, currently GXFP_RX_MAX */
	__u32 reserved;		/* must be zero */
	__u64 ts_ns;		/* CLOCK_MONOTONIC timestamp of the snapshot */
	__u8  data[];
};

#define GXFP_RX_HDR_SIZE	16u
#define GXFP_RX_RECORD_SIZE	(GXFP_RX_HDR_SIZE + GXFP_RX_MAX)

/* Discard all queued RX records. */
#define GXFP_IOCTL_FLUSH_RX	_IO(0x3F, 0x00)

/* ---- debug / diagnostic interface (magic 'G') -------------------------- */

#define GXFP_ABI_VERSION	2
#define GXFP_FW_VERSION_MAX	64

struct gxfp_info {
	__u32 abi_version;
	__u32 mbox_phys;
	__u32 mbox_size;
	__s32 gpio_write_done;
	__s32 gpio_read_done;
	__s32 irq;
	__u8  fw_version[GXFP_FW_VERSION_MAX];
	__u8  fw_len;
	__u8  ready;
	__u8  tls_used;
	__u8  reserved[5];
};

struct gxfp_peek {
	__u32 off;
	__u32 len;
	__u8  data[128];
};

struct gxfp_line {
	__u32 which;		/* 0 = write-done, 1 = read-done */
	__u32 value;
};

#define GXFP_IOCTL_GET_INFO	_IOR('G', 0x01, struct gxfp_info)
#define GXFP_IOCTL_PEEK		_IOWR('G', 0x04, struct gxfp_peek)
#define GXFP_IOCTL_PULSE	_IOW('G', 0x05, __u32)
#define GXFP_IOCTL_SET_LINE	_IOW('G', 0x06, struct gxfp_line)

#endif /* _UAPI_LINUX_GXFP5130_H */

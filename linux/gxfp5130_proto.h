/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gxfp5130_proto.h - Goodix MP / GXFP5130 frame codec.
 *
 * Shared between the kernel transport driver and host-side unit tests.
 * All multi-byte fields are little endian.
 *
 *   MP wrapper:   flags(1) len(2) checksum(1)
 *   Goodix body:  cmd(1) decl_len(2) payload(decl_len-1) checksum(1)
 */
#ifndef GXFP5130_PROTO_H
#define GXFP5130_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define GXFP_MP_TYPE_A		0xA0
#define GXFP_MP_TYPE_B		0xB0
#define GXFP_MP_TYPE_C		0xC0
#define GXFP_MP_LEN_MAX		0x6c00
#ifndef GXFP_MAX_FRAME
#define GXFP_MAX_FRAME		0x1c00
#endif

#ifdef __KERNEL__
#define GXFP_EMSGSIZE		(-EMSGSIZE)
#else
#define GXFP_EMSGSIZE		(-90)	/* -EMSGSIZE */
#endif

/* Goodix command layer checksum: 0xAA - sum(cmd, decl_len, payload). */
static inline u8 gxfp_goodix_checksum(u8 cmd, u16 decl_len, const u8 *payload,
				      size_t payload_len)
{
	u32 sum = (u32)cmd + (u32)(decl_len & 0xff) +
		  (u32)((decl_len >> 8) & 0xff);
	size_t i;

	for (i = 0; i < payload_len; i++)
		sum += payload[i];

	return (u8)(0xaa - (u8)sum);
}

/*
 * Build a Goodix body: <cmd> <decl_len:2> <payload> <checksum>.
 * Returns the body length, or a negative errno.
 */
static inline int gxfp_build_body(u8 cmd, const u8 *payload,
				  size_t payload_len, u8 *out, size_t out_cap)
{
	u16 decl_len = (u16)(payload_len + 1);
	size_t total = 3 + payload_len + 1;

	if (total > out_cap || payload_len > GXFP_MAX_FRAME - 4)
		return GXFP_EMSGSIZE;

	out[0] = cmd;
	out[1] = (u8)(decl_len & 0xff);
	out[2] = (u8)(decl_len >> 8);
	if (payload_len)
		memcpy(out + 3, payload, payload_len);
	out[3 + payload_len] =
		gxfp_goodix_checksum(cmd, decl_len, payload, payload_len);

	return (int)total;
}

/* Validate the 4-byte MP header; returns the wrapped length in *mp_len. */
static inline bool gxfp_parse_mp_header(const u8 *hdr, size_t len, u16 *mp_len)
{
	u8 flags, type;
	u16 l;

	if (len < 4)
		return false;

	flags = hdr[0];
	type = flags >> 4;
	if (type != (GXFP_MP_TYPE_A >> 4) && type != (GXFP_MP_TYPE_B >> 4) &&
	    type != (GXFP_MP_TYPE_C >> 4))
		return false;

	l = (u16)hdr[1] | ((u16)hdr[2] << 8);
	if (l < 4 || l >= GXFP_MP_LEN_MAX)
		return false;

	if (hdr[3] != (u8)(flags + hdr[1] + hdr[2]))
		return false;

	if (mp_len)
		*mp_len = l;
	return true;
}

/* Build the 4-byte MP wrapper. Returns 4. */
static inline int gxfp_build_mp_header(u8 flags, size_t body_len, u8 *out)
{
	out[0] = flags;
	out[1] = (u8)(body_len & 0xff);
	out[2] = (u8)(body_len >> 8);
	out[3] = (u8)(out[0] + out[1] + out[2]);
	return 4;
}

/*
 * Parse a Goodix body.  Returns true when the framing is consistent;
 * *ck_ok reports whether the trailing checksum matched (0x88 placeholders
 * are tolerated by callers).
 */
static inline bool gxfp_parse_body(const u8 *buf, size_t len, u8 *cmd,
				   const u8 **payload, size_t *payload_len,
				   bool *ck_ok)
{
	u16 decl_len;
	size_t plen;

	if (len < 4)
		return false;

	decl_len = (u16)buf[1] | ((u16)buf[2] << 8);
	if (decl_len < 1)
		return false;
	plen = decl_len - 1;
	if (3 + plen + 1 > len)
		return false;

	*cmd = buf[0];
	*payload = buf + 3;
	*payload_len = plen;
	*ck_ok = buf[3 + plen] ==
		 gxfp_goodix_checksum(buf[0], decl_len, buf + 3, plen);
	return true;
}

#endif /* GXFP5130_PROTO_H */

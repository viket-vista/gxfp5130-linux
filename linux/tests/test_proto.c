// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side unit tests for the GXFP5130 frame codec.
 *
 * The expected byte strings below are derived from the protocol recovered
 * from the vendor driver (see docs/REVERSE-ENGINEERING.md).
 */
#include <stdio.h>
#include <string.h>

#include "../gxfp5130_proto.h"

static int failures;

#define EXPECT(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
			failures++;					\
		}							\
	} while (0)

static void dump(const char *tag, const u8 *buf, size_t len)
{
	size_t i;

	printf("%-12s", tag);
	for (i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	printf("\n");
}

static void test_body_no_payload(void)
{
	u8 body[16];
	static const u8 want[] = { 0xa8, 0x01, 0x00, 0x01 };
	int n = gxfp_build_body(0xa8, NULL, 0, body, sizeof(body));

	dump("body a8", body, sizeof(want));
	EXPECT(n == 4, "body length");
	EXPECT(!memcmp(body, want, sizeof(want)), "body bytes for 0xA8");
}

static void test_body_payload(void)
{
	u8 body[16];
	static const u8 want[] = { 0xa8, 0x03, 0x00, 0x47, 0x46, 0x72 };
	u8 payload[] = { 'G', 'F' };
	int n = gxfp_build_body(0xa8, payload, 2, body, sizeof(body));

	dump("body GF", body, 6);
	EXPECT(n == 6, "body length with payload");
	EXPECT(!memcmp(body, want, sizeof(want)), "body bytes for payload");
}

static void test_body_state_cmd(void)
{
	u8 body[16];
	static const u8 want[] = {
		0xae, 0x06, 0x00, 0x55, 0x00, 0x00, 0x00, 0x00, 0xa1
	};
	u8 payload[5] = { 0x55, 0x00, 0x00, 0x00, 0x00 };
	int n = gxfp_build_body(0xae, payload, 5, body, sizeof(body));

	dump("body ae", body, 9);
	EXPECT(n == 9, "0xAE body length");
	EXPECT(!memcmp(body, want, sizeof(want)), "0xAE bytes");
}

static void test_mp_header(void)
{
	u8 hdr[4];
	static const u8 want[] = { 0xa0, 0x04, 0x00, 0xa4 };

	gxfp_build_mp_header(GXFP_MP_TYPE_A, 4, hdr);
	dump("mp a0", hdr, 4);
	EXPECT(!memcmp(hdr, want, sizeof(want)), "MP header bytes");
}

static void test_roundtrip(void)
{
	u8 body[64], frame[80];
	u8 payload[] = { 'G', 'F' };
	const u8 *out_payload;
	u8 cmd;
	size_t out_len, total;
	u16 mp_len;
	int body_len, n;
	bool ck_ok;

	body_len = gxfp_build_body(0xa8, payload, 2, body, sizeof(body));
	EXPECT(body_len == 6, "roundtrip body len");

	n = gxfp_build_mp_header(GXFP_MP_TYPE_A, (size_t)body_len, frame);
	memcpy(frame + n, body, (size_t)body_len);
	total = (size_t)n + (size_t)body_len;

	dump("frame", frame, total);

	EXPECT(gxfp_parse_mp_header(frame, total, &mp_len), "parse mp");
	EXPECT(mp_len == (u16)body_len, "mp length");

	EXPECT(gxfp_parse_body(frame + 4, total - 4, &cmd, &out_payload,
			       &out_len, &ck_ok), "parse body");
	EXPECT(cmd == 0xa8, "cmd");
	EXPECT(out_len == 2, "payload len");
	EXPECT(out_payload[0] == 'G' && out_payload[1] == 'F', "payload");
	EXPECT(ck_ok, "checksum ok");
}

static void test_rejects(void)
{
	u8 hdr[4];
	u16 len;
	u8 body[8];
	const u8 *p;
	u8 cmd;
	size_t plen;
	bool ck;

	/* wrong MP type nibble */
	memcpy(hdr, "\x90\x04\x00\x94", 4);
	EXPECT(!gxfp_parse_mp_header(hdr, 4, &len), "reject type 0x9");

	/* bad MP checksum */
	memcpy(hdr, "\xa0\x04\x00\xa5", 4);
	EXPECT(!gxfp_parse_mp_header(hdr, 4, &len), "reject bad mp ck");

	/* too short */
	memcpy(hdr, "\xa0\x04\x00", 3);
	EXPECT(!gxfp_parse_mp_header(hdr, 3, &len), "reject short header");

	/* length below 4 */
	memcpy(hdr, "\xa0\x03\x00\xa3", 4);
	EXPECT(!gxfp_parse_mp_header(hdr, 4, &len), "reject len < 4");

	/* length above the protocol maximum */
	memcpy(hdr, "\xa0\x00\x6c\x0c", 4);
	EXPECT(!gxfp_parse_mp_header(hdr, 4, &len), "reject len >= 0x6c00");

	/* body checksum mismatch is reported, framing is still valid */
	memcpy(body, "\xa8\x01\x00\xff", 4);
	EXPECT(gxfp_parse_body(body, 4, &cmd, &p, &plen, &ck), "parse body");
	EXPECT(!ck, "detect bad body checksum");

	/* 0x88 placeholder checksum is reported as mismatch but tolerated */
	memcpy(body, "\xa8\x01\x00\x88", 4);
	EXPECT(gxfp_parse_body(body, 4, &cmd, &p, &plen, &ck), "parse 0x88");
	EXPECT(!ck, "0x88 is not a valid checksum");

	/* declared length longer than the buffer */
	memcpy(body, "\xa8\x05\x00\x00", 4);
	EXPECT(!gxfp_parse_body(body, 4, &cmd, &p, &plen, &ck), "reject short");

	/* build into a too small buffer */
	EXPECT(gxfp_build_body(0xa8, NULL, 0, body, 3) == GXFP_EMSGSIZE,
	       "reject small out buffer");
}

int main(void)
{
	test_body_no_payload();
	test_body_payload();
	test_body_state_cmd();
	test_mp_header();
	test_roundtrip();
	test_rejects();

	if (failures) {
		printf("\n%d test(s) FAILED\n", failures);
		return 1;
	}
	printf("\nall codec tests passed\n");
	return 0;
}

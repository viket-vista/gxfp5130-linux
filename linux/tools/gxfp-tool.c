// SPDX-License-Identifier: GPL-2.0
/*
 * gxfp-tool - small userspace client for /dev/gxfp (gxfp5130 driver).
 *
 * The device speaks the raw eSPI mailbox ABI: write() submits a complete
 * eSPI-wrapped frame, read() returns raw RX window snapshots.
 *
 * Usage:
 *   gxfp-tool info
 *   gxfp-tool cmd <cmd-hex> [payload-hex] [timeout_ms]
 *   gxfp-tool raw <frame-hex>
 *   gxfp-tool listen [count] [timeout_ms]
 *   gxfp-tool peek [off] [len]
 *   gxfp-tool pulse <wr|rd>
 *   gxfp-tool setline <wr|rd> <0|1>
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../gxfp5130_uapi.h"

#define ESPI_MAGIC	0xF0
#define MP_TYPE_CMD	0xA0
#define MP_TYPE_TLS	0xB0
#define MP_TYPE_NOTICE	0xC0

static uint16_t seq16 = 0x8881;

static void hexdump(const unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		printf("%02x ", buf[i]);
		if ((i + 1) % 16 == 0 && i + 1 < len)
			printf("\n           ");
	}
	printf("\n");
}

static int hex2bin(const char *str, unsigned char *out, size_t out_cap)
{
	size_t n = 0;
	const char *p = str;

	while (*p) {
		unsigned int v;
		int consumed;

		if (*p == ' ' || *p == ':' || *p == '-')
			p++;
		if (!*p)
			break;
		if (sscanf(p, "%2x%n", &v, &consumed) != 1)
			return -1;
		if (n >= out_cap)
			return -1;
		out[n++] = (unsigned char)v;
		p += consumed;
	}
	return (int)n;
}

static int open_dev(void)
{
	int fd = open("/dev/gxfp", O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open /dev/gxfp: %s\n", strerror(errno));
		fprintf(stderr, "Is the gxfp5130 module loaded?\n");
	}
	return fd;
}

static int cmd_info(void)
{
	struct gxfp_info info;
	int fd, ret;

	fd = open_dev();
	if (fd < 0)
		return 1;

	memset(&info, 0, sizeof(info));
	ret = ioctl(fd, GXFP_IOCTL_GET_INFO, &info);
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ioctl GET_INFO: %s\n", strerror(errno));
		return 1;
	}

	printf("abi_version   : %u\n", info.abi_version);
	printf("mailbox       : 0x%08x (%u bytes)\n", info.mbox_phys,
	       info.mbox_size);
	printf("gpio write-done: %d\n", info.gpio_write_done);
	printf("gpio read-done : %d\n", info.gpio_read_done);
	printf("irq           : %d\n", info.irq);
	printf("ready         : %u\n", info.ready);
	printf("tls_used      : %u\n", info.tls_used);
	printf("fw_version    : %.*s\n", info.fw_len, info.fw_version);
	return 0;
}

static int cmd_peek(int argc, char **argv)
{
	struct gxfp_peek pk;
	int fd, ret;

	memset(&pk, 0, sizeof(pk));
	pk.off = 0x200;
	pk.len = 32;
	if (argc > 2)
		pk.off = (__u32)strtoul(argv[2], NULL, 0);
	if (argc > 3)
		pk.len = (__u32)strtoul(argv[3], NULL, 0);
	if (pk.len > sizeof(pk.data)) {
		fprintf(stderr, "len must be <= %zu\n", sizeof(pk.data));
		return 1;
	}

	fd = open_dev();
	if (fd < 0)
		return 1;
	ret = ioctl(fd, GXFP_IOCTL_PEEK, &pk);
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ioctl PEEK: %s\n", strerror(errno));
		return 1;
	}

	printf("mailbox[0x%x..0x%x]:\n", pk.off, pk.off + pk.len);
	hexdump(pk.data, pk.len);
	return 0;
}

static int cmd_pulse(int argc, char **argv)
{
	__u32 which;
	int fd, ret;

	if (argc < 3) {
		fprintf(stderr, "usage: gxfp-tool pulse <wr|rd>\n");
		return 1;
	}
	if (!strcmp(argv[2], "wr"))
		which = 0;
	else if (!strcmp(argv[2], "rd"))
		which = 1;
	else {
		fprintf(stderr, "pulse: expected wr or rd\n");
		return 1;
	}

	fd = open_dev();
	if (fd < 0)
		return 1;
	ret = ioctl(fd, GXFP_IOCTL_PULSE, &which);
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ioctl PULSE: %s\n", strerror(errno));
		return 1;
	}
	printf("pulsed %s\n", argv[2]);
	return 0;
}

static int cmd_setline(int argc, char **argv)
{
	struct gxfp_line ln;
	int fd, ret;

	if (argc < 4) {
		fprintf(stderr, "usage: gxfp-tool setline <wr|rd> <0|1>\n");
		return 1;
	}
	if (!strcmp(argv[2], "wr"))
		ln.which = 0;
	else if (!strcmp(argv[2], "rd"))
		ln.which = 1;
	else {
		fprintf(stderr, "setline: expected wr or rd\n");
		return 1;
	}
	ln.value = (__u32)atoi(argv[3]);

	fd = open_dev();
	if (fd < 0)
		return 1;
	ret = ioctl(fd, GXFP_IOCTL_SET_LINE, &ln);
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ioctl SET_LINE: %s\n", strerror(errno));
		return 1;
	}
	printf("set %s=%u\n", argv[2], ln.value);
	return 0;
}

/* Build a complete eSPI-wrapped command frame: wrapper + MP + Goodix body. */
static int build_cmd_frame(unsigned char cmd, const unsigned char *payload,
			   size_t payload_len, unsigned char *out,
			   size_t out_cap)
{
	unsigned char mp[0x200];
	unsigned char body[0x200];
	size_t body_len = 3 + payload_len + 1;
	size_t mp_len = 4 + body_len;
	size_t raw = 8 + mp_len;
	size_t tx_len = (raw + 7) & ~7u;
	uint16_t decl = (uint16_t)(payload_len + 1);
	uint32_t sum;
	size_t i;

	if (body_len > sizeof(body) || mp_len > sizeof(mp) || tx_len > out_cap)
		return -1;

	body[0] = cmd;
	body[1] = (unsigned char)(decl & 0xff);
	body[2] = (unsigned char)(decl >> 8);
	memcpy(body + 3, payload, payload_len);
	sum = (uint32_t)cmd + (decl & 0xff) + ((decl >> 8) & 0xff);
	for (i = 0; i < payload_len; i++)
		sum += payload[i];
	body[3 + payload_len] = (unsigned char)(0xaa - (unsigned char)sum);

	mp[0] = MP_TYPE_CMD;
	mp[1] = (unsigned char)(body_len & 0xff);
	mp[2] = (unsigned char)(body_len >> 8);
	mp[3] = (unsigned char)(mp[0] + mp[1] + mp[2]);
	memcpy(mp + 4, body, body_len);

	memset(out, 0, tx_len);
	out[0] = ESPI_MAGIC;
	out[1] = (unsigned char)(mp_len & 0xff);
	out[2] = (unsigned char)(mp_len >> 8);
	out[3] = (unsigned char)(out[0] + out[1] + out[2]);
	seq16++;
	out[4] = (unsigned char)(seq16 & 0xff);
	out[5] = (unsigned char)(seq16 >> 8);
	memcpy(out + 8, mp, mp_len);

	return (int)tx_len;
}

/* Parse one RX record, print the MP frame if it is well formed. */
static int print_record(const unsigned char *rec, size_t n)
{
	static const unsigned char hdr[GXFP_RX_HDR_SIZE];
	const unsigned char *data;
	uint32_t len;
	uint16_t mp_len;
	uint8_t flags, type, ck;
	uint8_t cmd, decl_lo, decl_hi;
	size_t plen, total;

	if (n != GXFP_RX_RECORD_SIZE)
		return -1;

	memcpy((void *)&len, rec, sizeof(len));
	(void)hdr;
	data = rec + GXFP_RX_HDR_SIZE;

	if (len == 0 || len > GXFP_RX_MAX)
		return -1;

	flags = data[0];
	type = flags >> 4;
	if (type != 0xA && type != 0xB && type != 0xC)
		return -1;

	mp_len = (uint16_t)(data[1] | (data[2] << 8));
	ck = (uint8_t)(flags + data[1] + data[2]);
	if (data[3] != ck || mp_len < 4 || (size_t)mp_len + 4 > len)
		return -1;

	total = 4u + mp_len;
	cmd = data[4];
	decl_lo = data[5];
	decl_hi = data[6];
	plen = (size_t)decl_lo | ((size_t)decl_hi << 8);
	plen = plen ? plen - 1 : 0;
	if (7 + plen > total)
		return -1;

	printf("frame: type=0x%x mp_len=%u cmd=0x%02x payload=%zu\n",
	       type, mp_len, cmd, plen);
	if (plen)
		hexdump(data + 7, plen);
	return 0;
}

static int read_frames(int fd, int timeout_ms, int max_frames)
{
	unsigned char *rec = malloc(GXFP_RX_RECORD_SIZE);
	int got = 0;

	if (!rec)
		return -1;

	while (got < max_frames) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr, n;

		pr = poll(&pfd, 1, timeout_ms);
		if (pr <= 0)
			break;

		n = (int)read(fd, rec, GXFP_RX_RECORD_SIZE);
		if (n < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "read: %s\n", strerror(errno));
			break;
		}
		if (n == 0)
			break;

		if (!print_record(rec, (size_t)n))
			got++;
	}

	free(rec);
	return got;
}

static int cmd_raw(int argc, char **argv)
{
	unsigned char frame[GXFP_TX_MAX];
	int n, fd;
	ssize_t wr;

	if (argc < 3) {
		fprintf(stderr, "usage: gxfp-tool raw <frame-hex>\n");
		return 1;
	}
	n = hex2bin(argv[2], frame, sizeof(frame));
	if (n <= 0) {
		fprintf(stderr, "bad hex\n");
		return 1;
	}

	fd = open_dev();
	if (fd < 0)
		return 1;
	wr = write(fd, frame, (size_t)n);
	close(fd);
	if (wr < 0) {
		fprintf(stderr, "write: %s\n", strerror(errno));
		return 1;
	}
	printf("sent %d bytes\n", n);
	return 0;
}

static int cmd_cmd(int argc, char **argv)
{
	unsigned char payload[GXFP_TX_MAX];
	unsigned char frame[GXFP_TX_MAX];
	unsigned char cmd;
	int fd, plen = 0, timeout_ms = 1000;
	int tx_len, got;
	ssize_t wr;

	if (argc < 3) {
		fprintf(stderr, "usage: gxfp-tool cmd <cmd-hex> [payload-hex] [timeout_ms]\n");
		return 1;
	}
	if (hex2bin(argv[2], &cmd, 1) != 1) {
		fprintf(stderr, "bad command hex\n");
		return 1;
	}
	if (argc > 3 && strlen(argv[3])) {
		plen = hex2bin(argv[3], payload, sizeof(payload));
		if (plen < 0) {
			fprintf(stderr, "bad payload hex\n");
			return 1;
		}
	}
	if (argc > 4)
		timeout_ms = atoi(argv[4]);

	tx_len = build_cmd_frame(cmd, payload, (size_t)plen, frame,
				 sizeof(frame));
	if (tx_len < 0) {
		fprintf(stderr, "cannot build frame\n");
		return 1;
	}

	fd = open_dev();
	if (fd < 0)
		return 1;

	ioctl(fd, GXFP_IOCTL_FLUSH_RX);

	wr = write(fd, frame, (size_t)tx_len);
	if (wr < 0) {
		fprintf(stderr, "write: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("tx cmd 0x%02x payload %d (%d bytes)\n", cmd, plen, tx_len);

	got = read_frames(fd, timeout_ms, 3);
	close(fd);
	if (got <= 0) {
		fprintf(stderr, "no response within %d ms\n", timeout_ms);
		return 1;
	}
	return 0;
}

static int cmd_listen(int argc, char **argv)
{
	int count = -1, fd, timeout_ms = 1000;

	if (argc > 2)
		count = atoi(argv[2]);
	if (argc > 3)
		timeout_ms = atoi(argv[3]);

	fd = open_dev();
	if (fd < 0)
		return 1;

	read_frames(fd, timeout_ms, count < 0 ? 64 : count);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <info|cmd|raw|listen|peek|pulse|setline> ...\n",
			argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "info"))
		return cmd_info();
	if (!strcmp(argv[1], "cmd"))
		return cmd_cmd(argc, argv);
	if (!strcmp(argv[1], "raw"))
		return cmd_raw(argc, argv);
	if (!strcmp(argv[1], "listen"))
		return cmd_listen(argc, argv);
	if (!strcmp(argv[1], "peek"))
		return cmd_peek(argc, argv);
	if (!strcmp(argv[1], "pulse"))
		return cmd_pulse(argc, argv);
	if (!strcmp(argv[1], "setline"))
		return cmd_setline(argc, argv);

	fprintf(stderr, "unknown command '%s'\n", argv[1]);
	return 1;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * gxfp5130.c - Linux transport driver for the Goodix GXFP5130 fingerprint
 *              sensor as found in Huawei MateBook laptops.
 *
 * The sensor is not reachable on a standard SPI/I2C bus.  The Embedded
 * Controller exposes a 4 KiB eSPI mailbox window in host MMIO space plus
 * two "done" lines and an interrupt line.  The kernel is deliberately a
 * dumb transport:
 *
 *   write(2)  one complete eSPI-wrapped frame (max 0x200 bytes).  The
 *             kernel copies it into the TX window and pulses write-done.
 *             Framing (eSPI wrapper, MP frame, Goodix body) is built by
 *             userspace.
 *
 *   read(2)   one opaque RX window snapshot, GXFP_RX_RECORD_SIZE bytes:
 *             struct gxfp_rx_record { len, reserved, timestamp_ns, data[] }
 *             with GXFP_RX_MAX bytes of raw window data.  Each doorbell
 *             interrupt produces one record; multi-chunk frames are
 *             assembled by userspace.
 *
 *   poll(2)   POLLIN while a record is queued.
 *
 *   ioctl     GXFP_IOCTL_FLUSH_RX discards queued records; a few debug
 *             ioctls (info, peek, pulse, setline) live under magic 'G'.
 *
 * Wire format, for reference (all multi-byte fields little endian):
 *
 *   eSPI wrapper: magic(1) mp_len(2) checksum(1) seq(2) reserved(2)
 *                 magic    = 0xF0
 *                 checksum = magic + mp_len_lo + mp_len_hi
 *
 *   MP frame:     flags(1) len(2) checksum(1), flags = 0xA0/0xB0/0xC0
 *                 checksum = flags + len_lo + len_hi
 *
 *   Goodix body:  cmd(1) decl_len(2) payload(decl_len-1) checksum(1)
 *                 checksum = 0xAA - (cmd + decl_len + sum(payload))
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/poll.h>

#include "gxfp5130_uapi.h"

#define GXFP_DRV_NAME		"gxfp5130"
#define GXFP_DEV_NAME		"gxfp"

#define GXFP_MBOX_DEFAULT_BASE	0xFE800000UL
#define GXFP_MBOX_DEFAULT_SIZE	0x1000
#define GXFP_MBOX_TX_OFF	0x000
#define GXFP_MBOX_RX_OFF	0x200

#define GXFP_ESPI_WRAPPER_MAGIC	0xF0
#define GXFP_ESPI_SEQ16_INIT	0x8881
#define GXFP_ESPI_ALIGN		8

#define GXFP_CMD_FW_VERSION	0xA8
#define GXFP_CMD_ACK		0xB0

#define GXFP_RXQ_SIZE		SZ_256K

/* ---- module parameters ------------------------------------------------- */

static unsigned long mbox_phys;
module_param(mbox_phys, ulong, 0444);
MODULE_PARM_DESC(mbox_phys, "EC mailbox physical address (0 = auto/0xFE800000)");

static unsigned int mbox_size;
module_param(mbox_size, uint, 0444);
MODULE_PARM_DESC(mbox_size, "EC mailbox window size (0 = auto/0x1000)");

static unsigned int tx_off;
module_param(tx_off, uint, 0444);
MODULE_PARM_DESC(tx_off, "command (host->EC) buffer offset inside the window");

static unsigned int rx_off = GXFP_MBOX_RX_OFF;
module_param(rx_off, uint, 0644);
MODULE_PARM_DESC(rx_off, "response (EC->host) buffer offset inside the window");

static int gpio_wr_index = 1;
module_param(gpio_wr_index, int, 0444);
MODULE_PARM_DESC(gpio_wr_index, "ACPI GPIO index of the write-done line");

static int gpio_rd_index = 2;
module_param(gpio_rd_index, int, 0444);
MODULE_PARM_DESC(gpio_rd_index, "ACPI GPIO index of the read-done line");

static bool swap_done;
module_param(swap_done, bool, 0444);
MODULE_PARM_DESC(swap_done, "swap the two GpioIo 'done' lines");

static bool invert_done;
module_param(invert_done, bool, 0444);
MODULE_PARM_DESC(invert_done, "invert the logical level of the done lines");

static unsigned int probe_timeout_ms = 800;
module_param(probe_timeout_ms, uint, 0444);
MODULE_PARM_DESC(probe_timeout_ms, "timeout of the firmware version handshake");

static bool probe_handshake;
module_param(probe_handshake, bool, 0444);
MODULE_PARM_DESC(probe_handshake, "query the firmware version at probe time "
				 "(default off: userspace owns the session init)");

static unsigned int poll_ms;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "poll mailbox every N ms when no IRQ is available "
			  "(0 = auto: 20 ms if no IRQ)");

static bool frame_log;
module_param(frame_log, bool, 0644);
MODULE_PARM_DESC(frame_log, "log transferred frames (debug)");

/* ---- device state ------------------------------------------------------ */

struct gxfp_acpi_res {
	phys_addr_t	mem;
	resource_size_t	mem_len;
	int		n_gpio_io;
	int		n_gpio_int;
	u16		io_pin[4];
	u16		int_pin;
};

struct gxfp_dev {
	struct device		*dev;
	struct miscdevice	misc;

	void __iomem		*mbox;
	phys_addr_t		mbox_phys;
	resource_size_t		mbox_size;
	unsigned int		tx_off;
	unsigned int		rx_off;

	struct gpio_desc	*gpio_write_done;
	struct gpio_desc	*gpio_read_done;
	int			irq;

	struct mutex		io_lock;	/* serialises mailbox access */
	spinlock_t		rx_lock;
	struct kfifo		rxq;
	wait_queue_head_t	rx_wait;
	atomic_t		reader_open;
	bool			rx_overflow;
	atomic64_t		stat_records;
	atomic64_t		stat_overflows;

	bool			ready;
	bool			tls_used;
	u8			fw_version[GXFP_FW_VERSION_MAX];
	u8			fw_len;

	u16			seq16;
	struct delayed_work	poll_work;
	unsigned int		poll_ms;
	bool			polling;

	struct gxfp_acpi_res	acpi;
};

/* ---- mailbox access ---------------------------------------------------- */

static void gxfp_mbox_write(struct gxfp_dev *g, unsigned int off,
			    const u8 *buf, size_t len)
{
	size_t i = 0;

	if ((off & 7) == 0) {
		for (; i + 8 <= len; i += 8) {
			u64 v;

			memcpy(&v, buf + i, 8);
			writeq(v, g->mbox + off + i);
		}
	}
	for (; i + 4 <= len; i += 4) {
		u32 v;

		memcpy(&v, buf + i, 4);
		writel(v, g->mbox + off + i);
	}
	if (i < len) {
		u32 v = 0;

		memcpy(&v, buf + i, len - i);
		writel(v, g->mbox + off + i);
	}
	wmb();
}

static void gxfp_mbox_read(struct gxfp_dev *g, unsigned int off,
			   u8 *buf, size_t len)
{
	size_t i = 0;

	for (; i + 8 <= len; i += 8) {
		u64 v = readq(g->mbox + off + i);

		memcpy(buf + i, &v, 8);
	}
	for (; i + 4 <= len; i += 4) {
		u32 v = readl(g->mbox + off + i);

		memcpy(buf + i, &v, 4);
	}
	if (i < len) {
		u32 v = readl(g->mbox + off + i);

		memcpy(buf + i, &v, len - i);
	}
	rmb();
}

static void gxfp_dump_frame(struct gxfp_dev *g, const char *tag,
			    const u8 *buf, size_t len)
{
	if (!frame_log)
		return;
	dev_info(g->dev, "%s%zu bytes\n", tag, len);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 1, buf, len,
		       false);
}

/* ---- GPIO handshake ---------------------------------------------------- */

static void gxfp_pulse_write_done(struct gxfp_dev *g)
{
	if (!g->gpio_write_done)
		return;

	gpiod_set_value_cansleep(g->gpio_write_done, invert_done ? 1 : 0);
	udelay(50);
	gpiod_set_value_cansleep(g->gpio_write_done, invert_done ? 0 : 1);
	udelay(4000);
	gpiod_set_value_cansleep(g->gpio_write_done, invert_done ? 1 : 0);
	udelay(200);
}

static void gxfp_pulse_read_done(struct gxfp_dev *g)
{
	if (!g->gpio_read_done)
		return;

	gpiod_set_value_cansleep(g->gpio_read_done, invert_done ? 0 : 1);
	udelay(50);
	gpiod_set_value_cansleep(g->gpio_read_done, invert_done ? 1 : 0);
	udelay(200);
}

/* ---- raw frame transmit ------------------------------------------------ */

static int gxfp_send_raw(struct gxfp_dev *g, const u8 *frame, size_t len)
{
	size_t tx_len;
	u8 *buf;
	int ret = 0;

	if (!len || len > GXFP_TX_MAX)
		return -EMSGSIZE;

	tx_len = ALIGN(len, GXFP_ESPI_ALIGN);
	buf = kzalloc(tx_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, frame, len);

	gxfp_dump_frame(g, "gxfp tx: ", buf, tx_len);

	mutex_lock(&g->io_lock);
	gxfp_mbox_write(g, g->tx_off, buf, tx_len);
	gxfp_pulse_write_done(g);
	mutex_unlock(&g->io_lock);

	kfree(buf);
	return ret;
}

/* ---- receive ----------------------------------------------------------- */

/*
 * Snapshot the whole RX window into one record.  Each doorbell interrupt
 * yields one snapshot; userspace assembles frames from consecutive records.
 */
static void gxfp_capture_record(struct gxfp_dev *g, u8 *rec)
{
	mutex_lock(&g->io_lock);
	*(u32 *)(rec + 0) = GXFP_RX_MAX;
	*(u32 *)(rec + 4) = 0;
	*(u64 *)(rec + 8) = ktime_get_ns();
	gxfp_mbox_read(g, g->rx_off, rec + GXFP_RX_HDR_SIZE, GXFP_RX_MAX);
	mutex_unlock(&g->io_lock);

	gxfp_pulse_read_done(g);
}

static atomic_t gxfp_dbg_recs;

static void gxfp_log_record(struct gxfp_dev *g, const u8 *rec)
{
	const u8 *d = rec + GXFP_RX_HDR_SIZE;

	if (!frame_log)
		return;
	if (atomic_inc_return(&gxfp_dbg_recs) > 400)
		return;
	dev_info(g->dev, "rx[%d]: %02x %02x %02x %02x %02x %02x\n",
		 atomic_read(&gxfp_dbg_recs), d[0], d[1], d[2], d[3], d[4], d[5]);
}

static void gxfp_queue_record(struct gxfp_dev *g, const u8 *rec)
{
	unsigned long flags;

	gxfp_log_record(g, rec);

	spin_lock_irqsave(&g->rx_lock, flags);
	if (kfifo_avail(&g->rxq) >= GXFP_RX_RECORD_SIZE) {
		kfifo_in(&g->rxq, rec, GXFP_RX_RECORD_SIZE);
		atomic64_inc(&g->stat_records);
	} else {
		/* tell userspace explicitly, like the reference driver */
		g->rx_overflow = true;
		atomic64_inc(&g->stat_overflows);
		kfifo_reset(&g->rxq);
	}
	spin_unlock_irqrestore(&g->rx_lock, flags);

	if (atomic64_read(&g->stat_overflows))
		dev_warn_ratelimited(g->dev,
				     "RX queue full (records=%lld overflows=%lld)\n",
				     (long long)atomic64_read(&g->stat_records),
				     (long long)atomic64_read(&g->stat_overflows));

	wake_up_interruptible(&g->rx_wait);
}

static irqreturn_t gxfp_irq_thread(int irq, void *data)
{
	struct gxfp_dev *g = data;
	u8 *rec = kmalloc(GXFP_RX_RECORD_SIZE, GFP_KERNEL);

	if (!rec)
		return IRQ_HANDLED;

	gxfp_capture_record(g, rec);
	gxfp_queue_record(g, rec);
	kfree(rec);
	return IRQ_HANDLED;
}

static irqreturn_t gxfp_irq_handler(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static void gxfp_poll_work(struct work_struct *work)
{
	struct gxfp_dev *g = container_of(to_delayed_work(work),
					  struct gxfp_dev, poll_work);
	u8 *rec = kmalloc(GXFP_RX_RECORD_SIZE, GFP_KERNEL);
	u8 head[4];

	if (rec) {
		mutex_lock(&g->io_lock);
		gxfp_mbox_read(g, g->rx_off, head, sizeof(head));
		mutex_unlock(&g->io_lock);

		if ((head[0] >> 4) == 0xA || (head[0] >> 4) == 0xB ||
		    (head[0] >> 4) == 0xC) {
			gxfp_capture_record(g, rec);
			gxfp_queue_record(g, rec);
		}
		kfree(rec);
	}

	if (g->polling)
		schedule_delayed_work(&g->poll_work,
				      msecs_to_jiffies(g->poll_ms));
}

/* ---- probe-time firmware handshake ------------------------------------- */

static int gxfp_read_window_mp(struct gxfp_dev *g, u8 *buf, size_t cap,
			       u16 *mp_len)
{
	size_t n = min_t(size_t, cap, GXFP_RX_MAX);
	u8 flags, type;

	if (n < 16)
		return -EINVAL;

	mutex_lock(&g->io_lock);
	gxfp_mbox_read(g, g->rx_off, buf, n);
	mutex_unlock(&g->io_lock);

	flags = buf[0];
	type = flags >> 4;
	if (type != 0xA && type != 0xB && type != 0xC)
		return -EAGAIN;

	*mp_len = (u16)buf[1] | ((u16)buf[2] << 8);
	if (*mp_len < 4 || *mp_len >= 0x6c00)
		return -EBADMSG;
	if (buf[3] != (u8)(flags + buf[1] + buf[2]))
		return -EBADMSG;

	return 0;
}

static int gxfp_probe_version(struct gxfp_dev *g)
{
	u8 frame[16];
	unsigned long deadline = jiffies + msecs_to_jiffies(probe_timeout_ms);
	int tries;

	/* eSPI wrapper + MP(A0) + body "A8 01 00 01" (version request) */
	frame[0] = GXFP_ESPI_WRAPPER_MAGIC;
	frame[1] = 4;
	frame[2] = 0;
	frame[3] = (u8)(frame[0] + frame[1] + frame[2]);
	g->seq16 = GXFP_ESPI_SEQ16_INIT;
	frame[4] = (u8)(g->seq16 & 0xff);
	frame[5] = (u8)(g->seq16 >> 8);
	frame[6] = 0;
	frame[7] = 0;
	frame[8] = 0xA0;
	frame[9] = 4;
	frame[10] = 0;
	frame[11] = 0xA4;
	frame[12] = GXFP_CMD_FW_VERSION;
	frame[13] = 1;
	frame[14] = 0;
	frame[15] = 1;

	for (tries = 0; tries < 3; tries++) {
		u8 *win = kzalloc(GXFP_RX_MAX, GFP_KERNEL);
		u16 mp_len;

		if (!win)
			return -ENOMEM;

		gxfp_send_raw(g, frame, sizeof(frame));

		while (time_before(jiffies, deadline)) {
			u8 cmd;

			msleep(5);
			if (gxfp_read_window_mp(g, win, GXFP_RX_MAX,
						&mp_len))
				continue;

			cmd = win[4];
			if (cmd == GXFP_CMD_FW_VERSION) {
				size_t plen = (size_t)win[5] |
					      ((size_t)win[6] << 8);

				plen = plen ? plen - 1 : 0;
				if (plen > sizeof(g->fw_version))
					plen = sizeof(g->fw_version);
				memcpy(g->fw_version, win + 7, plen);
				g->fw_len = (u8)plen;
				g->ready = true;
				kfree(win);
				gxfp_pulse_read_done(g);
				return 0;
			}
			if (cmd == GXFP_CMD_ACK)
				gxfp_pulse_read_done(g);
		}

		kfree(win);
		msleep(20);
	}

	return -ETIMEDOUT;
}

/* ---- character device -------------------------------------------------- */

static int gxfp_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct gxfp_dev *g = container_of(mdev, struct gxfp_dev, misc);

	if (atomic_cmpxchg(&g->reader_open, 0, 1))
		return -EBUSY;

	filp->private_data = g;
	return 0;
}

static int gxfp_release(struct inode *inode, struct file *filp)
{
	struct gxfp_dev *g = filp->private_data;

	atomic_set(&g->reader_open, 0);
	return 0;
}

static ssize_t gxfp_write(struct file *filp, const char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct gxfp_dev *g = filp->private_data;
	u8 *kbuf;
	int ret;

	if (count < 1 || count > GXFP_TX_MAX)
		return -EMSGSIZE;

	kbuf = memdup_user(ubuf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	ret = gxfp_send_raw(g, kbuf, count);
	kfree(kbuf);
	return ret ? ret : (ssize_t)count;
}

static ssize_t gxfp_read(struct file *filp, char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct gxfp_dev *g = filp->private_data;
	u8 *rec;
	unsigned long flags;
	size_t got = 0;

	if (count < GXFP_RX_RECORD_SIZE)
		return -EMSGSIZE;

	rec = kmalloc(GXFP_RX_RECORD_SIZE, GFP_KERNEL);
	if (!rec)
		return -ENOMEM;

	for (;;) {
		spin_lock_irqsave(&g->rx_lock, flags);
		if (g->rx_overflow) {
			g->rx_overflow = false;
			spin_unlock_irqrestore(&g->rx_lock, flags);
			dev_warn_ratelimited(g->dev, "RX overflow reported to userspace\n");
			kfree(rec);
			return -ENOBUFS;
		}
		if (kfifo_len(&g->rxq) >= GXFP_RX_RECORD_SIZE) {
			kfifo_out(&g->rxq, rec, GXFP_RX_RECORD_SIZE);
			got = GXFP_RX_RECORD_SIZE;
		}
		spin_unlock_irqrestore(&g->rx_lock, flags);

		if (got)
			break;
		if (filp->f_flags & O_NONBLOCK)
			break;
		if (signal_pending(current))
			break;
		if (wait_event_interruptible(g->rx_wait,
					     kfifo_len(&g->rxq) >=
						     GXFP_RX_RECORD_SIZE))
			break;
	}

	if (got && copy_to_user(ubuf, rec, got))
		got = 0;

	kfree(rec);
	return got ? (ssize_t)got : -EAGAIN;
}

static __poll_t gxfp_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct gxfp_dev *g = filp->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	poll_wait(filp, &g->rx_wait, wait);

	spin_lock_irqsave(&g->rx_lock, flags);
	if (kfifo_len(&g->rxq) >= GXFP_RX_RECORD_SIZE)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&g->rx_lock, flags);

	return mask;
}

static long gxfp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gxfp_dev *g = filp->private_data;
	struct gxfp_info info;
	unsigned long flags;

	switch (cmd) {
	case GXFP_IOCTL_FLUSH_RX:
		spin_lock_irqsave(&g->rx_lock, flags);
		kfifo_reset(&g->rxq);
		spin_unlock_irqrestore(&g->rx_lock, flags);
		return 0;

	case GXFP_IOCTL_GET_INFO:
		memset(&info, 0, sizeof(info));
		info.abi_version = GXFP_ABI_VERSION;
		info.mbox_phys = (__u32)g->mbox_phys;
		info.mbox_size = (__u32)g->mbox_size;
		info.gpio_write_done = g->gpio_write_done ?
			desc_to_gpio(g->gpio_write_done) : -1;
		info.gpio_read_done = g->gpio_read_done ?
			desc_to_gpio(g->gpio_read_done) : -1;
		info.irq = g->irq;
		info.ready = g->ready;
		info.tls_used = g->tls_used;
		info.fw_len = g->fw_len;
		memcpy(info.fw_version, g->fw_version, g->fw_len);
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;

	case GXFP_IOCTL_PEEK: {
		struct gxfp_peek pk;

		if (copy_from_user(&pk, (void __user *)arg, sizeof(pk)))
			return -EFAULT;
		if (pk.len > sizeof(pk.data) ||
		    (resource_size_t)pk.off + pk.len > g->mbox_size)
			return -EINVAL;
		if (!pk.len)
			return 0;

		mutex_lock(&g->io_lock);
		gxfp_mbox_read(g, pk.off, pk.data, pk.len);
		mutex_unlock(&g->io_lock);

		return copy_to_user((void __user *)arg, &pk, sizeof(pk)) ?
			-EFAULT : 0;
	}

	case GXFP_IOCTL_PULSE: {
		__u32 which;

		if (copy_from_user(&which, (void __user *)arg, sizeof(which)))
			return -EFAULT;
		if (which == 0) {
			if (!g->gpio_write_done)
				return -ENODEV;
			gxfp_pulse_write_done(g);
		} else if (which == 1) {
			if (!g->gpio_read_done)
				return -ENODEV;
			gxfp_pulse_read_done(g);
		} else {
			return -EINVAL;
		}
		return 0;
	}

	case GXFP_IOCTL_SET_LINE: {
		struct gxfp_line ln;

		if (copy_from_user(&ln, (void __user *)arg, sizeof(ln)))
			return -EFAULT;
		if (ln.which == 0) {
			if (!g->gpio_write_done)
				return -ENODEV;
			gpiod_set_value_cansleep(g->gpio_write_done,
						 ln.value ? 1 : 0);
		} else if (ln.which == 1) {
			if (!g->gpio_read_done)
				return -ENODEV;
			gpiod_set_value_cansleep(g->gpio_read_done,
						 ln.value ? 1 : 0);
		} else {
			return -EINVAL;
		}
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations gxfp_fops = {
	.owner		= THIS_MODULE,
	.open		= gxfp_open,
	.release	= gxfp_release,
	.read		= gxfp_read,
	.write		= gxfp_write,
	.poll		= gxfp_poll,
	.unlocked_ioctl	= gxfp_ioctl,
	.compat_ioctl	= gxfp_ioctl,
};

/* ---- ACPI / hardware resources ---------------------------------------- */

static int gxfp_acpi_cb(struct acpi_resource *ares, void *data)
{
	struct gxfp_acpi_res *r = data;

	switch (ares->type) {
	case ACPI_RESOURCE_TYPE_MEMORY32:
		if (ares->data.memory32.address_length >= 0x400 &&
		    ares->data.memory32.address_length <= 0x100000) {
			r->mem = ares->data.memory32.minimum;
			r->mem_len = ares->data.memory32.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		if (ares->data.fixed_memory32.address_length >= 0x400 &&
		    ares->data.fixed_memory32.address_length <= 0x100000) {
			r->mem = ares->data.fixed_memory32.address;
			r->mem_len = ares->data.fixed_memory32.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
		if (ares->data.address32.resource_type == ACPI_MEMORY_RANGE &&
		    ares->data.address32.address.address_length >= 0x400) {
			r->mem = ares->data.address32.address.minimum;
			r->mem_len = ares->data.address32.address.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if (ares->data.address64.resource_type == ACPI_MEMORY_RANGE &&
		    ares->data.address64.address.address_length >= 0x400) {
			r->mem = ares->data.address64.address.minimum;
			r->mem_len = ares->data.address64.address.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_GPIO: {
		struct acpi_resource_gpio *gp = &ares->data.gpio;

		if (gp->pin_table_length < 1)
			break;
		if (gp->connection_type == ACPI_RESOURCE_GPIO_TYPE_INT) {
			if (r->n_gpio_int == 0)
				r->int_pin = gp->pin_table[0];
			r->n_gpio_int++;
		} else if (gp->connection_type ==
			   ACPI_RESOURCE_GPIO_TYPE_IO) {
			if (r->n_gpio_io < 4)
				r->io_pin[r->n_gpio_io] = gp->pin_table[0];
			r->n_gpio_io++;
		}
		break;
	}
	default:
		break;
	}

	return 0;
}

static struct gpio_desc *gxfp_get_gpio(struct gxfp_dev *g, const char *what,
					int index)
{
	struct gpio_desc *desc = devm_gpiod_get_index(g->dev, NULL, index,
						      GPIOD_OUT_LOW);
	long err;

	if (IS_ERR(desc)) {
		err = PTR_ERR(desc);
		dev_warn(g->dev, "%s GPIO (ACPI index %d): %ld%s\n",
			 what, index, err,
			 err == -EBUSY ?
			 " (pad busy: leaked by an earlier crashed unload, "
			 "or claimed by another driver - try a reboot)" : "");
		return NULL;
	}

	dev_info(g->dev, "%s GPIO: %d (ACPI index %d, active_low=%d)\n",
		 what, desc_to_gpio(desc), index, gpiod_is_active_low(desc));
	return desc;
}

static int gxfp_request_gpios(struct gxfp_dev *g)
{
	struct gpio_desc *wr, *rd;

	wr = gxfp_get_gpio(g, "write-done", gpio_wr_index);
	rd = gxfp_get_gpio(g, "read-done", gpio_rd_index);

	if (!wr && !rd) {
		wr = gxfp_get_gpio(g, "write-done", 0);
		rd = gxfp_get_gpio(g, "read-done", 1);
	}

	if (swap_done) {
		g->gpio_write_done = rd;
		g->gpio_read_done = wr;
	} else {
		g->gpio_write_done = wr;
		g->gpio_read_done = rd;
	}

	return (wr || rd) ? 0 : -ENOENT;
}

static int gxfp_hw_init(struct gxfp_dev *g)
{
	struct acpi_device *adev = ACPI_COMPANION(g->dev);
	LIST_HEAD(res_list);
	int ret;

	memset(&g->acpi, 0, sizeof(g->acpi));

	if (adev) {
		ret = acpi_dev_get_resources(adev, &res_list, gxfp_acpi_cb,
					     &g->acpi);
		acpi_dev_free_resource_list(&res_list);
		if (ret < 0)
			dev_dbg(g->dev, "ACPI resource parsing failed: %d\n",
				ret);
	}

	if (!g->acpi.mem || g->acpi.mem_len < 0x400) {
		g->mbox_phys = mbox_phys ? mbox_phys : GXFP_MBOX_DEFAULT_BASE;
		g->mbox_size = mbox_size ? mbox_size : GXFP_MBOX_DEFAULT_SIZE;
		dev_info(g->dev, "no usable ACPI mailbox, using %pa\n",
			 &g->mbox_phys);
	} else {
		g->mbox_phys = g->acpi.mem;
		g->mbox_size = g->acpi.mem_len;
	}

	if (mbox_phys)
		g->mbox_phys = mbox_phys;
	if (mbox_size)
		g->mbox_size = mbox_size;

	g->tx_off = tx_off;
	g->rx_off = rx_off;

	if (!g->mbox_size || g->tx_off + GXFP_TX_MAX > g->mbox_size ||
	    g->rx_off + GXFP_RX_MAX > g->mbox_size) {
		dev_err(g->dev, "invalid window (size=%pa tx=0x%x rx=0x%x)\n",
			&g->mbox_size, g->tx_off, g->rx_off);
		return -EINVAL;
	}

	if (!devm_request_mem_region(g->dev, g->mbox_phys, g->mbox_size,
				     GXFP_DRV_NAME))
		dev_warn(g->dev, "mailbox region busy, mapping anyway\n");

	g->mbox = devm_ioremap(g->dev, g->mbox_phys, g->mbox_size);
	if (!g->mbox) {
		dev_err(g->dev, "cannot map mailbox at %pa\n", &g->mbox_phys);
		return -ENOMEM;
	}

	g->irq = -1;
	if (adev) {
		int irq = acpi_dev_gpio_irq_get(adev, 0);

		if (irq > 0)
			g->irq = irq;
	}
	if (g->irq < 0) {
		int irq = platform_get_irq_optional(to_platform_device(g->dev),
						   0);

		if (irq > 0)
			g->irq = irq;
	}

	gxfp_request_gpios(g);

	dev_info(g->dev, "mailbox %pa size %pa tx=0x%x rx=0x%x irq=%d\n",
		 &g->mbox_phys, &g->mbox_size, g->tx_off, g->rx_off, g->irq);
	return 0;
}

/* ---- probe / remove ---------------------------------------------------- */

static int gxfp_probe(struct platform_device *pdev)
{
	struct gxfp_dev *g;
	int ret;

	g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->dev = &pdev->dev;
	g->irq = -1;
	mutex_init(&g->io_lock);
	spin_lock_init(&g->rx_lock);
	init_waitqueue_head(&g->rx_wait);
	atomic_set(&g->reader_open, 0);
	g->seq16 = GXFP_ESPI_SEQ16_INIT;

	ret = kfifo_alloc(&g->rxq, GXFP_RXQ_SIZE, GFP_KERNEL);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, g);

	ret = gxfp_hw_init(g);
	if (ret)
		goto err_fifo;

	INIT_DELAYED_WORK(&g->poll_work, gxfp_poll_work);

	if (probe_handshake) {
		msleep(150);
		ret = gxfp_probe_version(g);
		if (!ret)
			dev_info(g->dev, "firmware version: %*phN (%.*s)\n",
				 g->fw_len, g->fw_version, g->fw_len,
				 g->fw_version);
		else
			dev_warn(g->dev, "firmware handshake failed: %d\n",
				 ret);
	} else {
		msleep(150);
	}

	if (g->irq > 0) {
		ret = devm_request_threaded_irq(g->dev, g->irq,
						gxfp_irq_handler,
						gxfp_irq_thread,
						IRQF_ONESHOT,
						GXFP_DRV_NAME, g);
		if (ret) {
			dev_warn(g->dev, "cannot request IRQ %d: %d\n",
				 g->irq, ret);
			g->irq = -1;
		}
	}

	g->poll_ms = poll_ms;
	if (g->irq < 0 && g->poll_ms == 0)
		g->poll_ms = 20;
	if (g->poll_ms) {
		g->polling = true;
		schedule_delayed_work(&g->poll_work,
				      msecs_to_jiffies(g->poll_ms));
	}

	g->misc.minor = MISC_DYNAMIC_MINOR;
	g->misc.name = GXFP_DEV_NAME;
	g->misc.fops = &gxfp_fops;
	g->misc.parent = g->dev;
	g->misc.mode = 0600;

	ret = misc_register(&g->misc);
	if (ret) {
		dev_err(g->dev, "cannot register misc device: %d\n", ret);
		goto err_poll;
	}

	dev_info(g->dev, "registered /dev/%s (irq=%d, poll=%u ms)\n",
		 GXFP_DEV_NAME, g->irq, g->poll_ms);
	return 0;

err_poll:
	if (g->polling) {
		g->polling = false;
		cancel_delayed_work_sync(&g->poll_work);
	}
err_fifo:
	kfifo_free(&g->rxq);
	return ret;
}

static void gxfp_remove(struct platform_device *pdev)
{
	struct gxfp_dev *g = platform_get_drvdata(pdev);

	if (g->polling) {
		g->polling = false;
		cancel_delayed_work_sync(&g->poll_work);
	}

	misc_deregister(&g->misc);
	wake_up_interruptible(&g->rx_wait);
	kfifo_free(&g->rxq);
}

static const struct acpi_device_id gxfp_acpi_ids[] = {
	{ "GXFP5130", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, gxfp_acpi_ids);

static struct platform_driver gxfp_driver = {
	.driver = {
		.name = GXFP_DRV_NAME,
		.acpi_match_table = gxfp_acpi_ids,
	},
	.probe = gxfp_probe,
	.remove = gxfp_remove,
};
module_platform_driver(gxfp_driver);

MODULE_AUTHOR("Reverse-engineered Linux driver");
MODULE_DESCRIPTION("Goodix GXFP5130 eSPI mailbox fingerprint transport");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2");

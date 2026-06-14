// SPDX-License-Identifier: GPL-2.0
/*
 * scsilink.c - DaynaPORT SCSI/Link Ethernet driver for Linux 7.x
 *
 * Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>
 *
 * A SCSI upper-level device driver that binds to a DaynaPORT SCSI/Link Ethernet
 * adapter (as emulated by BlueSCSI V2 / ZuluSCSI / PiSCSI) and presents it to
 * Linux as a standard Ethernet interface.
 *
 * The device is a SCSI Processor device (type 0x03, vendor "Dayna", product
 * "SCSI/Link") that moves Ethernet frames with vendor SCSI opcodes:
 *   0x08 READ(6)   - receive packet(s)
 *   0x0A WRITE(6)  - transmit a packet
 *   0x09           - retrieve MAC address + stats (18 bytes)
 *   0x0E           - enable/disable the interface
 *
 * The protocol opcodes/framing and the RX record parser live in the shared
 * lib/daynaport.h; this file supplies only the 7.x-specific glue.  Protocol
 * reference: reference/daynaport.md.
 *
 * Architecture:
 *   - Binds as a struct scsi_driver; the SCSI bus probes every connected device
 *     against us and probe() claims the TYPE_PROCESSOR Dayna/SCSI/Link match.
 *   - All SCSI I/O is one synchronous scsi_execute_cmd() call.
 *   - There is no RX interrupt, so RX is polled.  A single per-interface kthread
 *     serializes the device's one-command-at-a-time nature: it sends up to
 *     tx_burst queued frames then issues one READ(6), with no locks beyond the
 *     sk_buff_head's own.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/delay.h>		/* msleep */
#include <linux/jiffies.h>		/* msecs_to_jiffies */
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>		/* struct scsi_sense_hdr */

#include "daynaport.h"			/* shared DaynaPORT protocol defs + RX parser */

/* ----------------------------------------------------------------------- *
 *  Tunables / constants
 *
 *  DaynaPORT SCSI opcodes, command flags, and RX framing constants live in the
 *  shared, version-independent "daynaport.h" (included above; see
 *  reference/daynaport.md).  The driver-policy tunables below stay here.
 * ----------------------------------------------------------------------- */

/* Default and floor for rx_req_len (below): the allocation length we put in the
 * READ CDB.  It is only a hint -- the device may return less, more, or ignore
 * it.  BlueSCSI (blind mode, cdb[5]=0xC0) packs frames into one response until
 * "total + DAYNAPORT_SCSI_PACKET_MAX + 6 > size" (network.c,
 * DAYNAPORT_SCSI_PACKET_MAX = 1524), capped at 2 frames by its bus-hold guard;
 * 4096 leaves room for that 2-frame batch.  ZuluSCSI ignores the field outright.
 * The floor matters: below ~1530 BlueSCSI cannot pack even one max-size frame,
 * so RX would silently stall -- hence SCSILINK_RX_REQ_MIN.  The ceiling is
 * SCSILINK_RBUF_LEN (requesting more than the buffer is incoherent). */
#define SCSILINK_RX_REQ		4096	/* default rx_req_len, bytes */
#define SCSILINK_RX_REQ_MIN	2048	/* floor: >= one full frame + pack overhead */

/* RX SCSI transfer buffer.  Sized generously to hold a large multi-frame
 * response.  We never assume how much the device returns: BlueSCSI bounds a
 * batch to the requested length, but ZuluSCSI ignores the request and caps only
 * on a byte budget, so it can return more than we ask for.  The poll zeroes this
 * WHOLE buffer before each READ so a zero-length terminator always lands after
 * the data (however much arrives), and daynaport_rx_parse() walks records
 * bounded by this length.  No GFP_DMA needed: the block layer bounces for any
 * addressing constraint of the underlying HBA. */
#define SCSILINK_RBUF_LEN	16384
#define SCSILINK_TBUF_LEN	1536	/* one frame + slack */

/* SCSI command timeout / retries.  Keep the timeout generous so a momentarily
 * busy bus does not trigger the mid-layer's reset escalation (which would
 * disturb other targets and clear our enable state). */
#define SCSILINK_TIMEOUT	(10 * HZ)
#define SCSILINK_RETRIES	1
#define SCSILINK_TX_WATCHDOG	(5 * HZ)

/* TX queue depth before we push back on the stack with netif_stop_queue(). */
#define SCSILINK_TXQ_MAX	16

/* Default TX burst: frames to WRITE before yielding the single command slot to
 * one RX poll (see the tx_burst param below for the full rationale). */
#define SCSILINK_TX_BURST	16

/* RX poll cadence.  Policy: while data is flowing, poll at the fast rate; once
 * the device goes empty, hold the fast rate for fast_hold more polls (a
 * download's queue goes briefly empty as TCP's window breathes -- without the
 * hold the next packet would wait up to a full idle interval), then relax to the
 * idle rate.  Tunable at load AND at runtime -- the params are writable and the
 * poll loop reads them live, so the cadence can be swept on a running interface:
 *
 *   modprobe scsilink poll_ms=80 poll0_ms=20 fast_hold=16   # at load
 *   echo 5 > /sys/module/scsilink/parameters/poll0_ms       # live, no reload
 *
 * poll_ms   idle rate: interval between READs when no data is waiting.
 * poll0_ms  fast rate: interval while data is flowing (and during the hold).
 * fast_hold number of empty polls to stay fast before relaxing to idle.
 */
static int poll_ms   = 80;
static int poll0_ms  = 20;
static int fast_hold = 16;
module_param(poll_ms,   int, 0644);
module_param(poll0_ms,  int, 0644);
module_param(fast_hold, int, 0644);
MODULE_PARM_DESC(poll_ms,   "RX idle poll interval, milliseconds (default 80)");
MODULE_PARM_DESC(poll0_ms,  "RX fast poll interval, milliseconds (default 20)");
MODULE_PARM_DESC(fast_hold, "empty polls to stay fast before relaxing (default 16)");

/* READ-request length (bytes) placed in the CDB; the device may cap or ignore
 * it.  Writable at load and at runtime; the set handler validates and clamps
 * every write to [SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN], so the poll path can
 * use it unguarded. */
static int rx_req_len = SCSILINK_RX_REQ;

static int scsilink_set_rx_req_len(const char *val, const struct kernel_param *kp)
{
	int n, c, ret;

	ret = kstrtoint(val, 0, &n);
	if (ret)
		return ret;

	c = clamp(n, SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN);
	if (c != n)
		pr_warn("scsilink: rx_req_len %d out of [%d, %d]; using %d\n",
			n, SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN, c);
	rx_req_len = c;
	return 0;
}

static const struct kernel_param_ops scsilink_rx_req_len_ops = {
	.set = scsilink_set_rx_req_len,
	.get = param_get_int,
};
module_param_cb(rx_req_len, &scsilink_rx_req_len_ops, &rx_req_len, 0644);
MODULE_PARM_DESC(rx_req_len,
	"READ request length in bytes; device may cap or ignore it "
	"(default 4096, clamped to [2048, 16384])");

/* TX fairness knob: the maximum number of frames to WRITE before the poll loop
 * yields the device's single command slot to one RX poll.  Larger favors upload
 * throughput (back-to-back WRITEs amortize per-command SCSI overhead); smaller
 * favors RX fairness (inbound ACKs/replies drain sooner, so the adapter's RX
 * FIFO overflows less under a sustained upload).  Writable at load and at
 * runtime; the set handler clamps every write to [1, SCSILINK_TXQ_MAX] so the
 * poll loop can read it unguarded -- 0 would stall TX outright, and a burst
 * larger than the queue can hold just drains it whole. */
static int tx_burst = SCSILINK_TX_BURST;

static int scsilink_set_tx_burst(const char *val, const struct kernel_param *kp)
{
	int n, c, ret;

	ret = kstrtoint(val, 0, &n);
	if (ret)
		return ret;

	c = clamp(n, 1, SCSILINK_TXQ_MAX);
	if (c != n)
		pr_warn("scsilink: tx_burst %d out of [1, %d]; using %d\n",
			n, SCSILINK_TXQ_MAX, c);
	tx_burst = c;
	return 0;
}

static const struct kernel_param_ops scsilink_tx_burst_ops = {
	.set = scsilink_set_tx_burst,
	.get = param_get_int,
};
module_param_cb(tx_burst, &scsilink_tx_burst_ops, &tx_burst, 0644);
MODULE_PARM_DESC(tx_burst,
	"max frames to send before yielding to one RX poll "
	"(default 16, clamped to [1, 16])");

/* ----------------------------------------------------------------------- *
 *  Per-interface state.  Lives in netdev_priv(dev) (allocated by alloc_etherdev).
 * ----------------------------------------------------------------------- */

struct scsilink {
	struct scsi_device	*sdev;		/* our SCSI target */
	struct net_device	*dev;		/* the net interface */
	struct task_struct	*poll_task;	/* RX/TX serializing kthread */
	wait_queue_head_t	wq;		/* wakes the kthread on TX/stop */
	struct sk_buff_head	txq;		/* frames queued by ndo_start_xmit */
	u8			*rbuf;		/* RX transfer buffer */
	u8			*tbuf;		/* TX transfer buffer */
	int			fast_left;	/* fast polls remaining after activity */
};

/* ----------------------------------------------------------------------- *
 *  SCSI helper
 * ----------------------------------------------------------------------- */

/*
 * Issue one 6-byte-CDB SCSI command synchronously and block until it completes.
 * Process (kthread / probe) context only.  Returns the scsi_execute_cmd result
 * (0 == good); callers that care check for nonzero, the RX path ignores it (see
 * scsilink_rx_poll).
 */
static int scsilink_scsi(struct scsilink *dp, const unsigned char *cdb,
			 blk_opf_t opf, void *buf, unsigned int len)
{
	struct scsi_sense_hdr sshdr;
	const struct scsi_exec_args args = {
		.sshdr = &sshdr,
	};

	return scsi_execute_cmd(dp->sdev, cdb, opf, buf, len,
				SCSILINK_TIMEOUT, SCSILINK_RETRIES, &args);
}

/* ----------------------------------------------------------------------- *
 *  Receive
 * ----------------------------------------------------------------------- */

/*
 * Deliver one frame (FCS already stripped) up to the stack.  This is the
 * version-specific half of the RX path: the record-walking lives in the shared
 * daynaport_rx_parse() (daynaport.h), which invokes this callback once per good
 * frame.  ctx is our per-interface state.
 */
static void scsilink_deliver(void *ctx, const unsigned char *frame, int len)
{
	struct scsilink *dp = ctx;
	struct net_device *dev = dp->dev;
	struct sk_buff *skb;

	skb = netdev_alloc_skb_ip_align(dev, len);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}
	skb_put_data(skb, frame, len);
	skb->protocol = eth_type_trans(skb, dev);
	dev->stats.rx_bytes += len;
	dev->stats.rx_packets++;
	netif_rx(skb);
}

/*
 * Issue one READ(6) and deliver whatever frames come back.  The DaynaPORT ends
 * the DATA-IN phase early (variable-length responses), which the HBA reports as
 * a short transfer -- so we trust the self-describing, length-checked buffer
 * rather than the SCSI result and parse regardless.  Returns the unicast
 * (to-us) frame count -- the fast/idle cadence keys off that, so broadcast and
 * multicast chatter is delivered but does not pin us in fast-poll.
 */
static int scsilink_rx_poll(struct scsilink *dp)
{
	unsigned char cdb[6];
	int errors = 0;
	int ucast = 0;

	/* Pre-zero the whole buffer so the bytes just past the device's data read
	 * back as a zero-length header -- which is how daynaport_rx_parse() knows
	 * to stop.  Zero the ENTIRE buffer, not just the requested length: a batch
	 * exceeding rx_req_len (ZuluSCSI ignores the request) would otherwise run
	 * past the cleared region into stale bytes that parse as a bogus record. */
	memset(dp->rbuf, 0, SCSILINK_RBUF_LEN);

	daynaport_cdb6(cdb, SCSILINK_CMD_RECV, rx_req_len, SCSILINK_RECV_FLAG);

	scsilink_scsi(dp, cdb, REQ_OP_DRV_IN, dp->rbuf, SCSILINK_RBUF_LEN);

	daynaport_rx_parse(dp->rbuf, SCSILINK_RBUF_LEN,
			   scsilink_deliver, dp, &errors, &ucast);
	dp->dev->stats.rx_errors += errors;
	return ucast;
}

/* ----------------------------------------------------------------------- *
 *  Transmit
 * ----------------------------------------------------------------------- */

/* Transmit one frame via WRITE(6).  kthread context. */
static void scsilink_tx_one(struct scsilink *dp, struct sk_buff *skb)
{
	struct net_device *dev = dp->dev;
	unsigned char cdb[6];
	int len = skb->len;

	if (len > SCSILINK_MAXFRAME)
		len = SCSILINK_MAXFRAME;
	if (skb_copy_bits(skb, 0, dp->tbuf, len)) {
		dev->stats.tx_errors++;
		return;
	}
	if (len < SCSILINK_MINFRAME) {		/* pad runt frames */
		memset(dp->tbuf + len, 0, SCSILINK_MINFRAME - len);
		len = SCSILINK_MINFRAME;
	}

	daynaport_cdb6(cdb, SCSILINK_CMD_SEND, len, SCSILINK_SEND_FLAG);

	if (scsilink_scsi(dp, cdb, REQ_OP_DRV_OUT, dp->tbuf, len)) {
		dev->stats.tx_errors++;
	} else {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += len;
	}
}

/* ----------------------------------------------------------------------- *
 *  Poll kthread - the single thread that serializes all device I/O
 * ----------------------------------------------------------------------- */

static int scsilink_poll_thread(void *arg)
{
	struct scsilink *dp = arg;
	struct sk_buff *skb;
	bool fast = false;
	int n;

	while (!kthread_should_stop()) {
		/* Read the cadence knobs live so runtime sysfs writes take effect
		 * on the next poll without a reload.  `fast` is the rate the shared
		 * cadence policy chose after the previous poll. */
		int ms = fast ? poll0_ms : poll_ms;
		unsigned long to = max_t(unsigned long, msecs_to_jiffies(ms), 1);
		int sent = 0;

		/* Sleep until a frame is queued, we're asked to stop, or the poll
		 * interval elapses -- whichever comes first. */
		wait_event_interruptible_timeout(dp->wq,
				!skb_queue_empty(&dp->txq) || kthread_should_stop(),
				to);
		if (kthread_should_stop())
			break;

		/* Send at most tx_burst frames before yielding the device's single
		 * command slot to the RX poll below.  The device does one command at
		 * a time: while we hold the bus writing, inbound frames (ACKs, ping
		 * replies) pile up in the adapter's small RX FIFO and can overflow.
		 * The wait above returns immediately while the queue is non-empty, so
		 * capping the burst makes the loop a weighted tx_burst:1 round-robin
		 * that guarantees RX a turn every tx_burst frames. */
		while (sent < tx_burst && (skb = skb_dequeue(&dp->txq)) != NULL) {
			scsilink_tx_one(dp, skb);
			dev_consume_skb_any(skb);
			sent++;
		}
		if (netif_queue_stopped(dp->dev))
			netif_wake_queue(dp->dev);

		/* One RX poll, then let the shared policy pick the next rate: fast
		 * while to-us frames are arriving and through a brief hold after
		 * (a download's queue goes briefly empty as TCP's window breathes),
		 * idle once that hold is spent.  Broadcast/multicast is delivered but
		 * does not sustain fast-poll. */
		n = scsilink_rx_poll(dp);
		fast = daynaport_poll_fast(n, &dp->fast_left, fast_hold);
	}
	return 0;
}

/* ----------------------------------------------------------------------- *
 *  net_device_ops
 * ----------------------------------------------------------------------- */

static int scsilink_open(struct net_device *dev)
{
	struct scsilink *dp = netdev_priv(dev);

	dp->fast_left = 0;
	dp->poll_task = kthread_run(scsilink_poll_thread, dp, "scsilink/%s",
				    dev->name);
	if (IS_ERR(dp->poll_task)) {
		int err = PTR_ERR(dp->poll_task);

		dp->poll_task = NULL;
		return err;
	}
	netif_start_queue(dev);
	return 0;
}

static int scsilink_stop(struct net_device *dev)
{
	struct scsilink *dp = netdev_priv(dev);

	netif_stop_queue(dev);
	if (dp->poll_task) {
		/* Blocks until any in-flight scsi_execute_cmd() returns (<= the
		 * 10s command timeout); the kthread won't start another. */
		kthread_stop(dp->poll_task);
		dp->poll_task = NULL;
	}
	skb_queue_purge(&dp->txq);
	return 0;
}

static netdev_tx_t scsilink_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct scsilink *dp = netdev_priv(dev);

	/* Cannot sleep here; hand the frame to the kthread.  sk_buff_head has its
	 * own lock, so no extra locking and no racy tx_pending flag. */
	skb_queue_tail(&dp->txq, skb);
	if (skb_queue_len(&dp->txq) >= SCSILINK_TXQ_MAX)
		netif_stop_queue(dev);
	wake_up_interruptible(&dp->wq);
	return NETDEV_TX_OK;
}

static void scsilink_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	netdev_warn(dev, "TX timeout\n");
	dev->stats.tx_errors++;
	netif_wake_queue(dev);
}

static const struct net_device_ops scsilink_netdev_ops = {
	.ndo_open		= scsilink_open,
	.ndo_stop		= scsilink_stop,
	.ndo_start_xmit		= scsilink_xmit,
	.ndo_tx_timeout		= scsilink_tx_timeout,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ----------------------------------------------------------------------- *
 *  SCSI driver: probe / remove
 * ----------------------------------------------------------------------- */

/* Does this SCSI device look like a DaynaPORT SCSI/Link? */
static bool is_scsilink(struct scsi_device *sdev)
{
	return sdev->type == TYPE_PROCESSOR &&
	       !memcmp(sdev->vendor, SCSILINK_VENDOR, sizeof(SCSILINK_VENDOR) - 1) &&
	       !memcmp(sdev->model,  SCSILINK_MODEL,  sizeof(SCSILINK_MODEL)  - 1);
}

static int scsilink_probe(struct scsi_device *sdev)
{
	struct device *gendev = &sdev->sdev_gendev;
	struct net_device *dev;
	struct scsilink *dp;
	unsigned char cdb[6];
	int err;

	if (!is_scsilink(sdev))
		return -ENODEV;

	if (scsi_device_get(sdev))
		return -ENODEV;

	dev = alloc_etherdev(sizeof(struct scsilink));
	if (!dev) {
		err = -ENOMEM;
		goto put_sdev;
	}
	SET_NETDEV_DEV(dev, gendev);

	dp = netdev_priv(dev);
	dp->sdev = sdev;
	dp->dev  = dev;
	skb_queue_head_init(&dp->txq);
	init_waitqueue_head(&dp->wq);

	dp->rbuf = kmalloc(SCSILINK_RBUF_LEN, GFP_KERNEL);
	dp->tbuf = kmalloc(SCSILINK_TBUF_LEN, GFP_KERNEL);
	if (!dp->rbuf || !dp->tbuf) {
		err = -ENOMEM;
		goto free_bufs;
	}

	dev->netdev_ops     = &scsilink_netdev_ops;
	dev->watchdog_timeo = SCSILINK_TX_WATCHDOG;

	/* Bring the adapter up: ENABLE (0x0E), wait the required ~0.5s settle,
	 * then read the MAC + stats (0x09).  Enable once here, not per open. */
	daynaport_cdb6(cdb, SCSILINK_CMD_ENABLE, 0, SCSILINK_ENABLE_FLAG);
	err = scsilink_scsi(dp, cdb, REQ_OP_DRV_IN, NULL, 0);
	if (err)
		dev_warn(gendev, "scsilink: enable failed (0x%x)\n", err);
	msleep(500);

	daynaport_cdb6(cdb, SCSILINK_CMD_STATS, SCSILINK_STATS_LEN, 0);
	err = scsilink_scsi(dp, cdb, REQ_OP_DRV_IN, dp->rbuf, SCSILINK_STATS_LEN);
	if (err) {
		dev_err(gendev, "scsilink: MAC read failed (0x%x)\n", err);
		err = -EIO;
		goto free_bufs;
	}
	eth_hw_addr_set(dev, dp->rbuf);		/* first 6 bytes = MAC */

	err = register_netdev(dev);
	if (err)
		goto free_bufs;

	dev_set_drvdata(gendev, dp);
	netdev_info(dev, "DaynaPORT SCSI/Link, MAC %pM\n", dev->dev_addr);
	return 0;

free_bufs:
	kfree(dp->rbuf);
	kfree(dp->tbuf);
	free_netdev(dev);
put_sdev:
	scsi_device_put(sdev);
	return err;
}

static void scsilink_remove(struct scsi_device *sdev)
{
	struct scsilink *dp = dev_get_drvdata(&sdev->sdev_gendev);
	struct net_device *dev = dp->dev;

	unregister_netdev(dev);		/* runs ndo_stop: kthread_stop + txq purge */
	kfree(dp->rbuf);
	kfree(dp->tbuf);
	free_netdev(dev);		/* frees dp (it is netdev_priv) */
	scsi_device_put(sdev);
}

/* ----------------------------------------------------------------------- *
 *  Module glue.  Registers as an upper-level SCSI device driver: the SCSI bus
 *  probes every connected device against us, and because no in-tree ULD claims
 *  TYPE_PROCESSOR, the Dayna/SCSI/Link device is left for us to bind.
 * ----------------------------------------------------------------------- */

static struct scsi_driver scsilink_template = {
	.gendrv = {
		.name	= "scsilink",
		.owner	= THIS_MODULE,
	},
	.probe	= scsilink_probe,
	.remove	= scsilink_remove,
};

static int __init scsilink_init(void)
{
	return scsi_register_driver(&scsilink_template);
}

static void __exit scsilink_exit(void)
{
	scsi_unregister_driver(&scsilink_template);
}

module_init(scsilink_init);
module_exit(scsilink_exit);

MODULE_DESCRIPTION("DaynaPORT SCSI/Link Ethernet driver");
MODULE_AUTHOR("Jeff Flitton <jeff@flitton.dev>");
MODULE_LICENSE("GPL");
MODULE_ALIAS_SCSI_DEVICE(TYPE_PROCESSOR);

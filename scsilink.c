/*
 * scsilink.c - DaynaPORT SCSI/Link Ethernet driver for Linux 2.0.x
 *
 * Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>
 *
 * A SCSI upper-level device driver that binds to a DaynaPORT
 * SCSI/Link Ethernet adapter (as emulated by BlueSCSI V2 / PiSCSI) and
 * presents it to Linux as a standard Ethernet interface
 *
 * The device is a SCSI Processor device (type 0x03, vendor "Dayna",
 * product "SCSI/Link") that moves Ethernet frames with vendor SCSI opcodes:
 *   0x08 READ(6)   - receive packet(s)
 *   0x0A WRITE(6)  - transmit a packet
 *   0x09           - retrieve MAC address + stats (18 bytes)
 *   0x0E           - enable/disable the interface
 *
 * There is no RX interrupt, so receive is polled: a timer issues READ(6)
 * via scsi_do_cmd() and its completion callback delivers packets and
 * re-arms the timer at an adaptive rate.
 *
 * Target: Linux 2.0.x, i386, Adaptec AHA-1542 (cmd_per_lun = 1, so a single
 * outstanding command per device; unchecked_isa_dma = 1, so transfer buffers
 * must be GFP_DMA / in the low 16MB).
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License, version 2, as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, see the file COPYING, or
 *	<https://www.gnu.org/licenses/>.
 *
 * Tested on i386 / AHA-1542 / BlueSCSI V2, against Linux 2.0.35 and 2.0.40.
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/interrupt.h>		/* mark_bh, NET_BH */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>

#include <asm/system.h>			/* cli/sti, save_flags */

#include <linux/fs.h>
#include <linux/blk.h>			/* struct request + RQ_* (Scsi_Cmnd) */

#include "scsi.h"			/* pulls in <scsi/scsi.h> and hosts.h */
#include "hosts.h"

/* ----------------------------------------------------------------------- *
 *  Tunables / constants
 * ----------------------------------------------------------------------- */

#define SCSILINK_MAX		4	/* max DaynaPORT interfaces we manage */

/* DaynaPORT SCSI opcodes (see reference/daynaport.md) */
#define SCSILINK_CMD_RECV		0x08	/* READ(6)  - receive */
#define SCSILINK_CMD_STATS		0x09	/* retrieve MAC + stats (18 bytes) */
#define SCSILINK_CMD_SEND		0x0A	/* WRITE(6) - transmit */
#define SCSILINK_CMD_ENABLE	0x0E	/* enable/disable interface */

#define SCSILINK_STATS_LEN		0x12	/* 18 bytes: 6 MAC + 3x4 counters */
#define SCSILINK_ENABLE_FLAG	0x80	/* cdb[5] for enable */
#define SCSILINK_RECV_FLAG		0xC0	/* cdb[5] for READ: blind mode (per NetBSD dse) */
#define SCSILINK_SEND_FLAG		0x00	/* cdb[5] for WRITE: raw frame format */

/* RX framing: 2-byte big-endian length + 4-byte flag field per packet */
#define SCSILINK_RX_HDR		6
#define SCSILINK_FCS_LEN		4	/* trailing Ethernet FCS, stripped */
#define SCSILINK_MINFRAME		60	/* pad short TX frames to this */
#define SCSILINK_MAXFRAME		ETH_FRAME_LEN	/* 1514, no FCS */
#define SCSILINK_RX_MAXREC		(SCSILINK_MAXFRAME + SCSILINK_FCS_LEN)

/* Length we request in the READ CDB = one max record (header + frame + FCS).
 * BlueSCSI uses this only to bound the batch: its guard "total + 1524 + 6 > size
 * -> stop" (network.c) forces exactly one frame per READ whenever size < ~3060,
 * which 1524 does.  NetBSD dse requests the same 1524.
 *
 * Tried 2-frame batching on real HW (i486/AHA-1542/BlueSCSI, 2026-06-08): raised
 * this to 4096 so BlueSCSI's guard left room for its own 2-frame cap.  Result:
 * throughput unchanged within noise (37 vs 39 kB/s), zero RX errors.  Reverted --
 * frames-per-READ is not the bottleneck here:
 *   - at download rates the RX ring rarely holds 2 frames at poll time, so batch
 *     mode mostly still sent 1; when it sent 2, BlueSCSI's 300us inter-packet
 *     delay (network.c) offset the saved per-transaction overhead.
 *   - the real limiter is structural: cmd_per_lun=1 serialises RX vs TX-ACKs,
 *     fixed per-READ SCSI + firmware-delay overhead, and a ~20ms inter-READ gap
 *     (SCSILINK_POLL0) that the more_pending fast-drain can't shorten, because
 *     BlueSCSI's final record always carries flag 0x00 -- it never signals
 *     "ring still has data" (network.c:305), so more_pending is always 0 here. */
#define SCSILINK_RX_REQ		(SCSILINK_RX_HDR + SCSILINK_RX_MAXREC)	/* 1524 */

/* RX SCSI transfer (DMA) buffer.  Generous like NetBSD's; the device sends only
 * one frame (capped by SCSILINK_RX_REQ) so the transfer underruns this harmlessly.
 * MUST be GFP_DMA for the AHA-1542 (24-bit ISA DMA). */
#define SCSILINK_RBUF_LEN		16384
#define SCSILINK_TBUF_LEN		1536	/* one frame + slack */

/* SCSI command timeout / retries.  Keep the timeout generous so a momentarily
 * busy bus does not trigger the mid-layer's reset escalation (which would
 * disturb other targets and clear our enable state). */
#define SCSILINK_TIMEOUT		(10 * HZ)
#define SCSILINK_RETRIES		1
#define SCSILINK_TX_WATCHDOG	(5 * HZ)

/* RX poll cadence (ticks).  BlueSCSI caps each READ batch at ~2 frames, so the
 * old "ramp to fast only at >=10 frames/read" heuristic never engaged and the
 * link stayed stuck at the slow idle rate.  New policy: drain fast whenever the
 * last READ returned anything, relax to the idle rate only when truly empty. */
#define SCSILINK_POLL		((HZ / 12) ? (HZ / 12) : 1)	/* ~80ms idle */
#define SCSILINK_POLL0		((HZ / 50) ? (HZ / 50) : 1)	/* ~20ms while flowing */

/* Hysteresis: after a read returns data, stay at the fast rate for this many
 * subsequent empty polls before relaxing to idle.  A download's queue goes
 * briefly empty as TCP's window breathes; without this we'd drop to the 80ms
 * idle rate on the first gap and the next packet would wait up to 80ms,
 * capping the window.  ~16 fast polls ~= 0.3s of "keep up" after activity. */
#define SCSILINK_FAST_HOLD		16

/* ----------------------------------------------------------------------- *
 *  Per-interface state.  Lives in dev->priv (allocated by init_etherdev).
 * ----------------------------------------------------------------------- */

struct scsilink {
	struct device		*dev;		/* the net interface */
	Scsi_Device		*sdev;		/* our SCSI target */
	struct enet_statistics	stats;
	struct timer_list	rx_timer;
	unsigned char		*rbuf;		/* GFP_DMA RX buffer */
	unsigned char		*tbuf;		/* GFP_DMA TX buffer */
	int			running;	/* interface up + polling */
	int			inited;		/* finish() hardware init done */
	int			last_to;	/* last poll interval (ticks) */
	int			more_pending;	/* device flagged more RX queued */
	int			fast_left;	/* fast polls remaining after activity */
};

static struct scsilink *scsilink_devs[SCSILINK_MAX];

/* ----------------------------------------------------------------------- *
 *  Forward decls
 * ----------------------------------------------------------------------- */

static int  scsilink_detect(Scsi_Device *);
static int  scsilink_attach(Scsi_Device *);
static void scsilink_finish(void);
static void scsilink_detach(Scsi_Device *);

static int  scsilink_open(struct device *);
static int  scsilink_stop(struct device *);
static int  scsilink_xmit(struct sk_buff *, struct device *);
static struct enet_statistics *scsilink_get_stats(struct device *);
static void scsilink_set_multicast(struct device *);

static void scsilink_rx_kick(unsigned long);
static void scsilink_recv_done(Scsi_Cmnd *);
static void scsilink_send_done(Scsi_Cmnd *);

struct Scsi_Device_Template scsilink_template = {
	NULL,			/* next */
	"scsilink",		/* name */
	"sl",			/* tag */
	NULL,			/* usage_count (set in init_module) */
	TYPE_PROCESSOR,		/* scsi_type (0x03) */
	0,			/* major (none) */
	0, 0, 0,		/* nr_dev, dev_noticed, dev_max */
	0,			/* blk: character/none, not a block device */
	scsilink_detect,
	NULL,			/* init: nothing to pre-size (static array) */
	scsilink_finish,
	scsilink_attach,
	scsilink_detach
};

/* ----------------------------------------------------------------------- *
 *  Helpers
 * ----------------------------------------------------------------------- */

/* Does this SCSI device look like a DaynaPORT SCSI/Link? */
static int scsilink_matches(Scsi_Device *sd)
{
	return sd->type == TYPE_PROCESSOR
	    && memcmp(sd->vendor, "Dayna", 5) == 0
	    && memcmp(sd->model,  "SCSI/Link", 9) == 0;
}

static struct scsilink *scsilink_find(Scsi_Device *sd)
{
	int i;
	for (i = 0; i < SCSILINK_MAX; i++)
		if (scsilink_devs[i] && scsilink_devs[i]->sdev == sd)
			return scsilink_devs[i];
	return NULL;
}

/* Sleep ~ticks jiffies in process context (used at attach/finish only). */
static void scsilink_delay(int ticks)
{
	current->state = TASK_INTERRUPTIBLE;
	current->timeout = jiffies + ticks;
	schedule();
	current->timeout = 0;
	current->state = TASK_RUNNING;
}

static void scsilink_sync_done(Scsi_Cmnd *SCpnt)
{
	SCpnt->request.rq_status = RQ_SCSI_DONE;
	if (SCpnt->request.sem != NULL)
		up(SCpnt->request.sem);
}

/*
 * Issue one SCSI command synchronously and block until it completes.
 * Process context only.  Returns SCpnt->result (0 == success).
 */
static int scsilink_scsi_sync(struct scsilink *dp, unsigned char *cdb,
			   void *buf, int buflen)
{
	Scsi_Cmnd *SCpnt;
	struct semaphore sem = MUTEX_LOCKED;
	int result;

	SCpnt = allocate_device(NULL, dp->sdev, 1);	/* may sleep */
	SCpnt->request.rq_status = RQ_SCSI_BUSY;
	SCpnt->cmd_len = 0;
	SCpnt->request.sem = &sem;

	scsi_do_cmd(SCpnt, cdb, buf, buflen, scsilink_sync_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);
	down(&sem);

	result = SCpnt->result;

	SCpnt->request.rq_status = RQ_INACTIVE;		/* release block */
	wake_up(&dp->sdev->device_wait);
	return result;
}

/* ----------------------------------------------------------------------- *
 *  Receive
 * ----------------------------------------------------------------------- */

/* Hand one frame (FCS already stripped) up to the stack. */
static void scsilink_rx_one(struct scsilink *dp, unsigned char *frame, int len)
{
	struct sk_buff *skb;

	skb = dev_alloc_skb(len + 2);
	if (skb == NULL) {
		dp->stats.rx_dropped++;
		return;
	}
	skb->dev = dp->dev;
	skb_reserve(skb, 2);			/* align the IP header */
	memcpy(skb_put(skb, len), frame, len);
	skb->protocol = eth_type_trans(skb, dp->dev);
	netif_rx(skb);
	dp->stats.rx_packets++;
}

/*
 * Parse a READ response.  Each record is:
 *   [2-byte BE length][4-byte flags][length bytes of frame incl 4-byte FCS]
 *
 * We do NOT use the flag field to decide whether another record follows: on
 * this hardware flag 0x10 means "more packets are available in device memory --
 * issue another READ", NOT "another record is concatenated in this buffer".
 * Trusting it made us read a phantom second record out of stale buffer (ASCII
 * payload bytes showing up as a bogus header).  Instead we rely on a
 * zero-length header to terminate: the caller pre-zeroes the buffer, so the
 * bytes right after the real data read back as length 0.  The "more available"
 * signal is handled naturally by the next poll's READ.  Returns packet count.
 */
static int scsilink_rx(struct scsilink *dp)
{
	unsigned char *p = dp->rbuf;
	int avail = SCSILINK_RBUF_LEN;
	int n = 0, len, flag;

	dp->more_pending = 0;
	while (avail >= SCSILINK_RX_HDR) {
		len  = (p[0] << 8) | p[1];	/* big-endian, incl FCS */
		flag = p[5];			/* flag field low byte */
		p     += SCSILINK_RX_HDR;
		avail -= SCSILINK_RX_HDR;

		if (len == 0)			/* zeroed tail / no (more) data */
			break;

		if (len < SCSILINK_MINFRAME + SCSILINK_FCS_LEN || len > SCSILINK_RX_MAXREC) {
			dp->stats.rx_errors++;	/* garbled record; bail out */
			break;
		}
		if (len > avail)		/* would run off the buffer */
			break;

		scsilink_rx_one(dp, p, len - SCSILINK_FCS_LEN);
		n++;

		/* flag 0x10 = the device still has packets queued; not used to
		 * find the next record (see above) but to decide how soon to
		 * poll again -- we re-poll on the next tick to drain the queue. */
		dp->more_pending = (flag & 0x10) != 0;

		p     += len;
		avail -= len;
	}
	return n;
}

/* RX poll timer: issue a READ(6).  Bottom-half context - must not sleep. */
static void scsilink_rx_kick(unsigned long arg)
{
	struct scsilink *dp = (struct scsilink *) arg;
	Scsi_Cmnd *SCpnt;
	unsigned long flags;
	unsigned char cmd[6];

	if (!dp->running)
		return;

	/* request_queueable() requires interrupts off and returns NULL if the
	 * single command block is busy (e.g. a TX is in flight). */
	save_flags(flags);
	cli();
	SCpnt = request_queueable(NULL, dp->sdev);
	restore_flags(flags);

	if (SCpnt == NULL) {
		/* The single command block is busy -- almost always a TX (a TCP
		 * ACK) in flight, i.e. traffic IS flowing.  Retry at the fast
		 * rate, not the idle rate: backing off to SCSILINK_POLL here was a
		 * major download throttle (RX kept stalling behind ACKs). */
		dp->rx_timer.expires = jiffies + SCSILINK_POLL0;
		add_timer(&dp->rx_timer);
		return;
	}

	/* Pre-zero the buffer so the bytes just past the device's data read back
	 * as a zero-length header, which is how scsilink_rx() knows to stop (we do
	 * not trust the device's "more" flag for that -- see scsilink_rx). */
	memset(dp->rbuf, 0, SCSILINK_RBUF_LEN);

	SCpnt->cmd_len = 0;
	cmd[0] = SCSILINK_CMD_RECV;
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = (SCSILINK_RX_REQ >> 8) & 0xff;	/* one frame/READ (BlueSCSI cap) */
	cmd[4] = SCSILINK_RX_REQ & 0xff;
	cmd[5] = SCSILINK_RECV_FLAG;

	scsi_do_cmd(SCpnt, cmd, dp->rbuf, SCSILINK_RBUF_LEN, scsilink_recv_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);
}

/* READ(6) completion - interrupt context.  Deliver packets, re-arm poll. */
static void scsilink_recv_done(Scsi_Cmnd *SCpnt)
{
	struct scsilink *dp = scsilink_find(SCpnt->device);
	int n = 0, next;

	if (dp != NULL) {
		/* The DaynaPORT ends the DATA-IN phase early (variable-length
		 * responses), which the AHA-1542 reports as a short-transfer
		 * (non-DID_OK) result even though the data is fine.  So we trust
		 * the self-describing, length-checked buffer rather than the SCSI
		 * host status and parse regardless. */
		n = scsilink_rx(dp);
	}

	/* release the command block */
	SCpnt->request.rq_status = RQ_INACTIVE;
	wake_up(&SCpnt->device->device_wait);

	if (dp == NULL || !dp->running)
		return;

	/*
	 * Re-arm the poll:
	 *  - device says more is queued -> next tick (drain the backlog ASAP)
	 *  - got data this time          -> fast rate, and hold fast for a while
	 *  - empty but recently active   -> stay fast (window-breathing gap)
	 *  - idle                        -> idle rate
	 */
	if (dp->more_pending) {
		dp->fast_left = SCSILINK_FAST_HOLD;
		next = 1;
	} else if (n > 0) {
		dp->fast_left = SCSILINK_FAST_HOLD;
		next = SCSILINK_POLL0;
	} else if (dp->fast_left > 0) {
		dp->fast_left--;
		next = SCSILINK_POLL0;
	} else {
		next = SCSILINK_POLL;
	}
	dp->last_to = next;
	dp->rx_timer.expires = jiffies + next;
	add_timer(&dp->rx_timer);
}

/* ----------------------------------------------------------------------- *
 *  Transmit
 * ----------------------------------------------------------------------- */

static int scsilink_xmit(struct sk_buff *skb, struct device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;
	Scsi_Cmnd *SCpnt;
	unsigned long flags;
	unsigned char cmd[6];
	int len;

	/* A TX already outstanding?  Honour a watchdog so we can't wedge. */
	if (dev->tbusy) {
		if (jiffies - dev->trans_start < SCSILINK_TX_WATCHDOG)
			return 1;
		dev->tbusy = 0;			/* assume it got lost */
	}

	if (skb == NULL) {			/* legacy "poke" */
		dev_tint(dev);
		return 0;
	}

	save_flags(flags);
	cli();
	SCpnt = request_queueable(NULL, dp->sdev);
	if (SCpnt == NULL) {			/* block busy (RX in flight) */
		restore_flags(flags);
		return 1;			/* tell the stack to retry */
	}
	dev->tbusy = 1;
	restore_flags(flags);

	len = skb->len;
	if (len > SCSILINK_MAXFRAME)
		len = SCSILINK_MAXFRAME;
	memcpy(dp->tbuf, skb->data, len);
	if (len < SCSILINK_MINFRAME) {		/* pad runt frames */
		memset(dp->tbuf + len, 0, SCSILINK_MINFRAME - len);
		len = SCSILINK_MINFRAME;
	}

	SCpnt->cmd_len = 0;
	cmd[0] = SCSILINK_CMD_SEND;
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = (len >> 8) & 0xff;
	cmd[4] = len & 0xff;
	cmd[5] = SCSILINK_SEND_FLAG;

	dev->trans_start = jiffies;
	scsi_do_cmd(SCpnt, cmd, dp->tbuf, len, scsilink_send_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);

	dev_kfree_skb(skb, FREE_WRITE);
	return 0;
}

/* WRITE(6) completion - interrupt context. */
static void scsilink_send_done(Scsi_Cmnd *SCpnt)
{
	struct scsilink *dp = scsilink_find(SCpnt->device);

	if (dp != NULL) {
		if (host_byte(SCpnt->result) == DID_OK)
			dp->stats.tx_packets++;
		else
			dp->stats.tx_errors++;
		dp->dev->tbusy = 0;
		mark_bh(NET_BH);
	}

	SCpnt->request.rq_status = RQ_INACTIVE;
	wake_up(&SCpnt->device->device_wait);
}

/* ----------------------------------------------------------------------- *
 *  Net device hooks
 * ----------------------------------------------------------------------- */

static int scsilink_open(struct device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;

	dp->running     = 1;
	dev->tbusy      = 0;
	dev->interrupt  = 0;
	dev->start      = 1;

	dp->last_to = SCSILINK_POLL;
	dp->rx_timer.expires = jiffies + SCSILINK_POLL;
	add_timer(&dp->rx_timer);

	MOD_INC_USE_COUNT;
	return 0;
}

static int scsilink_stop(struct device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;

	dp->running = 0;
	dev->start  = 0;
	dev->tbusy  = 1;

	del_timer(&dp->rx_timer);
	/* A recv command may still be in flight; its done callback sees
	 * running == 0 and will not re-arm the timer. */

	MOD_DEC_USE_COUNT;
	return 0;
}

static struct enet_statistics *scsilink_get_stats(struct device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;
	return &dp->stats;
}

/* v1: no multicast filtering.  BlueSCSI ignores 0x0D and 2.0f receives all
 * broadcast anyway; and this hook may run where we cannot sleep to issue a
 * SCSI command.  See reference/linux-2.0.md s7.5. */
static void scsilink_set_multicast(struct device *dev)
{
}

/* ----------------------------------------------------------------------- *
 *  SCSI device template callbacks
 * ----------------------------------------------------------------------- */

static int scsilink_detect(Scsi_Device *sd)
{
	if (!scsilink_matches(sd))
		return 0;
	scsilink_template.dev_noticed++;
	printk("scsilink: detected DaynaPORT SCSI/Link at scsi%d, channel %d,"
	       " id %d, lun %d\n", sd->host->host_no, sd->channel, sd->id, sd->lun);
	return 1;
}

/*
 * attach() runs before the mid-layer has built this device's command blocks,
 * so we must NOT issue any SCSI command here.  We only allocate state and the
 * net device (no I/O, no register_netdev).  Device init happens in finish().
 */
static int scsilink_attach(Scsi_Device *sd)
{
	struct device *dev;
	struct scsilink *dp;
	int i;

	if (!scsilink_matches(sd))
		return 0;

	for (i = 0; i < SCSILINK_MAX; i++)
		if (scsilink_devs[i] == NULL)
			break;
	if (i >= SCSILINK_MAX) {			/* no room */
		sd->attached--;
		return 1;
	}

	dev = init_etherdev(NULL, sizeof(struct scsilink));
	if (dev == NULL) {
		sd->attached--;
		return 1;
	}
	dp = (struct scsilink *) dev->priv;
	memset(dp, 0, sizeof(*dp));
	dp->dev  = dev;
	dp->sdev = sd;

	dp->rbuf = kmalloc(SCSILINK_RBUF_LEN, GFP_ATOMIC | GFP_DMA);
	dp->tbuf = kmalloc(SCSILINK_TBUF_LEN, GFP_ATOMIC | GFP_DMA);
	if (dp->rbuf == NULL || dp->tbuf == NULL) {
		if (dp->rbuf) kfree(dp->rbuf);
		if (dp->tbuf) kfree(dp->tbuf);
		/* dev leaks back to the net layer; acceptable on OOM at attach */
		sd->attached--;
		return 1;
	}

	dev->open		= scsilink_open;
	dev->stop		= scsilink_stop;
	dev->hard_start_xmit	= scsilink_xmit;
	dev->get_stats		= scsilink_get_stats;
	dev->set_multicast_list	= scsilink_set_multicast;

	dp->rx_timer.data     = (unsigned long) dp;
	dp->rx_timer.function = scsilink_rx_kick;
	init_timer(&dp->rx_timer);

	scsilink_devs[i] = dp;
	scsilink_template.nr_dev++;
	return 0;
}

/*
 * finish() runs after the mid-layer has built command blocks for every
 * attached device, so this is where we talk to the hardware: enable the
 * interface, wait the required settle, and read the MAC.  The net device was
 * already created and published to dev_base by init_etherdev() in attach().
 */
static void scsilink_finish(void)
{
	unsigned char cmd[6];
	int i, rc;

	for (i = 0; i < SCSILINK_MAX; i++) {
		struct scsilink *dp = scsilink_devs[i];
		if (dp == NULL || dp->inited)
			continue;
		dp->inited = 1;

		/* ENABLE the interface (0x0E, flag 0x80), then wait ~0.5s.
		 * No data phase.  XXX NetBSD issues this as a 6-byte DATA-IN;
		 * BlueSCSI takes it as no-data, which is what we do here. */
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = SCSILINK_CMD_ENABLE;
		cmd[5] = SCSILINK_ENABLE_FLAG;
		rc = scsilink_scsi_sync(dp, cmd, NULL, 0);
		if (rc)
			printk("scsilink: %s: enable failed (0x%x)\n",
			       dp->dev->name, rc);
		scsilink_delay(HZ / 2);

		/* Read MAC + stats (0x09, 18 bytes) into the RX buffer. */
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = SCSILINK_CMD_STATS;
		cmd[4] = SCSILINK_STATS_LEN;
		rc = scsilink_scsi_sync(dp, cmd, dp->rbuf, SCSILINK_STATS_LEN);
		if (rc) {
			printk("scsilink: %s: MAC read failed (0x%x)\n",
			       dp->dev->name, rc);
			continue;	/* leave dev_addr zeroed */
		}
		memcpy(dp->dev->dev_addr, dp->rbuf, ETH_ALEN);

		printk("scsilink: %s: DaynaPORT SCSI/Link, MAC "
		       "%02x:%02x:%02x:%02x:%02x:%02x\n", dp->dev->name,
		       dp->dev->dev_addr[0], dp->dev->dev_addr[1],
		       dp->dev->dev_addr[2], dp->dev->dev_addr[3],
		       dp->dev->dev_addr[4], dp->dev->dev_addr[5]);
	}
}

static void scsilink_detach(Scsi_Device *sd)
{
	int i;

	for (i = 0; i < SCSILINK_MAX; i++) {
		struct scsilink *dp = scsilink_devs[i];
		if (dp == NULL || dp->sdev != sd)
			continue;

		dp->running = 0;
		del_timer(&dp->rx_timer);
		unregister_netdev(dp->dev);	/* remove from dev_base */
		if (dp->rbuf) kfree(dp->rbuf);
		if (dp->tbuf) kfree(dp->tbuf);
		/*
		 * XXX We intentionally do NOT kfree(dp->dev): init_etherdev()
		 * may hand back a static device from Space.c (and embeds priv,
		 * i.e. our dp, inside its allocation in the new-device case).
		 * The exact free model is ambiguous in 2.0.x; resolve at first
		 * build/test.  The small leak on rmmod is acceptable for now.
		 */
		scsilink_devs[i] = NULL;
		scsilink_template.nr_dev--;
		sd->attached--;
	}
}

/* ----------------------------------------------------------------------- *
 *  Module glue.  Registers as an upper-level SCSI *device* driver
 *  (MODULE_SCSI_DEV) - NOT a host adapter, so we do not use scsi_module.c.
 * ----------------------------------------------------------------------- */

#ifdef MODULE

int init_module(void)
{
	scsilink_template.usage_count = &mod_use_count_;
	return scsi_register_module(MODULE_SCSI_DEV, &scsilink_template);
}

void cleanup_module(void)
{
	scsi_unregister_module(MODULE_SCSI_DEV, &scsilink_template);
}

#endif /* MODULE */

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
 * Target: Linux 2.0.x, i386, ISA SCSI card (Adaptec AHA-1542, cmd_per_lun = 1,
 * so a single outstanding command per device; unchecked_isa_dma = 1,
 * so transfer buffers must be GFP_DMA / in the low 16MB).
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

/* Length we request in each READ CDB.  BlueSCSI (blind mode, cdb[5]=0xC0) packs
 * frames into one response until "total + DAYNAPORT_SCSI_PACKET_MAX + 6 > size"
 * (network.c, DAYNAPORT_SCSI_PACKET_MAX = 1524), capped at 2 frames by its
 * bus-hold guard.  Requesting one record (1524) forces a single frame; 4096
 * leaves room for two.
 */
#define SCSILINK_RX_REQ		4096	/* room for BlueSCSI's 2-frame batch */

/* RX SCSI transfer (DMA) buffer, and the length handed to scsi_do_cmd.  Sized
 * generously (NetBSD uses the same 16K) to hold a large multi-frame response.
 * We never assume how much the device returns: BlueSCSI bounds a batch to the
 * requested length, but ZuluSCSI ignores the request and caps only on a byte
 * budget, so it can return more than we ask for.
 * scsilink_rx_kick() zeroes this WHOLE buffer
 * before each READ so a zero-length terminator always lands after the data
 * (however much arrives), and scsilink_rx() walks records bounded by this
 * length; a response larger than the buffer is simply received across
 * successive READs.  Do NOT shrink the pre-zero to the requested length -- a
 * batch exceeding it would run past the cleared region into stale bytes that
 * parse as a bogus record.  MUST be GFP_DMA for 24-bit ISA DMA). */
#define SCSILINK_RBUF_LEN		16384
#define SCSILINK_TBUF_LEN		1536	/* one frame + slack */

/* SCSI command timeout / retries.  Keep the timeout generous so a momentarily
 * busy bus does not trigger the mid-layer's reset escalation (which would
 * disturb other targets and clear our enable state). */
#define SCSILINK_TIMEOUT		(10 * HZ)
#define SCSILINK_RETRIES		1
#define SCSILINK_TX_WATCHDOG	(5 * HZ)

/* RX poll cadence.  Policy: while data is flowing, poll at the fast rate; once
 * the device goes empty, hold the fast rate for fast_hold more polls (a
 * download's queue goes briefly empty as TCP's window breathes -- without the
 * hold the next packet would wait up to a full idle interval), then relax to
 * the idle rate.
 *
 * The three knobs are bare module parameters (Linux 2.0 has no MODULE_PARM), so
 * the cadence can be swept at load time without rebuilding:
 *
 *   insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16
 *
 * poll_ms   idle rate: interval between READs when no data is waiting.
 * poll0_ms  fast rate: interval while data is flowing (and during the hold).
 * fast_hold number of empty polls to stay fast before relaxing to idle.
 *
 * poll_ms / poll0_ms are milliseconds, converted to ticks (scsilink_poll /
 * scsilink_poll0) in init_module, each floored at 1 tick.  The tick variables
 * carry HZ-derived defaults so a non-module (built-in) build is still sane. */
static int poll_ms   = 80;
static int poll0_ms  = 20;
static int fast_hold = 16;

static int scsilink_poll  = ((HZ / 12) ? (HZ / 12) : 1);	/* idle, ticks */
static int scsilink_poll0 = ((HZ / 50) ? (HZ / 50) : 1);	/* fast, ticks */

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
static int is_scsilink(Scsi_Device *sd)
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
 *   [2-byte BE length][4-byte flag field][length bytes of frame incl 4-byte FCS]
 *
 * Per the Dayna command set, the flag field's low byte is 0x10 ("more packets
 * available in device memory") or 0x00 ("last packet"), and a zero-length
 * header terminates the response.
 *
 * In practice the flag scopes to the response, not the device's whole queue.
 * BlueSCSI and ZuluSCSI share the same SCSI2SD-derived network.c and both emit
 * data[5] = (done ? 0 : 0x10): 0x10 = "another record follows in THIS response,"
 * 0x00 = "last record of it."  Since "done" is also set by a per-READ byte cap
 * (~2 max frames), a response can end -- last record 0x00 -- while the device's
 * ring still holds frames (self-consistent if you read it as a device draining a
 * small per-transaction buffer).  So neither value is a cross-READ signal: 0x10
 * means only "another record follows in this same response," and 0x00 only
 * "this response is over" -- NOT "the device's ring is empty."  The flag
 * reliably delimits records within one response, but says nothing about whether
 * another READ is worthwhile.
 *
 * We therefore ignore the flag and terminate on the zero-length header: the
 * caller pre-zeroes the buffer, so the bytes just past the device's data read
 * back as a length-0 header and stop us cleanly.  (We cannot instead bound the
 * buffer by the AHA-1542's residual; it reports the device's early end of the
 * DATA-IN phase as a short transfer.)  Returns packet count.
 */
static int scsilink_rx(struct scsilink *dp)
{
	unsigned char *p = dp->rbuf;
	int avail = SCSILINK_RBUF_LEN;
	int n = 0, len;

	while (avail >= SCSILINK_RX_HDR) {
		len  = (p[0] << 8) | p[1];	/* big-endian, incl FCS */
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
		 * rate so RX does not stall behind ACKs. */
		dp->rx_timer.expires = jiffies + scsilink_poll0;
		add_timer(&dp->rx_timer);
		return;
	}

	/* Pre-zero the whole buffer so the bytes just past the device's data read
	 * back as a zero-length header -- which is how scsilink_rx() knows to stop
	 * (we do not trust the device's flag field for that; see scsilink_rx).  The
	 * device can return more than SCSILINK_RX_REQ: ZuluSCSI (verified from its
	 * source) ignores the requested length, so we don't assume the response fits
	 * in it.  Zero the entire buffer to guarantee a terminator lands after
	 * whatever it sends -- zeroing only the requested length lets a larger batch
	 * run past the cleared region into stale bytes that parse as a bogus record. */
	memset(dp->rbuf, 0, SCSILINK_RBUF_LEN);

	SCpnt->cmd_len = 0;
	cmd[0] = SCSILINK_CMD_RECV;
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = (SCSILINK_RX_REQ >> 8) & 0xff;	/* request up to a 2-frame batch */
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
		 * responses), which the SCSI controller reports as a short-transfer
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
	 *  - got data this time        -> fast rate, and hold fast for a while
	 *  - empty but recently active -> stay fast (window-breathing gap)
	 *  - idle                      -> idle rate
	 */
	if (n > 0) {
		dp->fast_left = fast_hold;
		next = scsilink_poll0;
	} else if (dp->fast_left > 0) {
		dp->fast_left--;
		next = scsilink_poll0;
	} else {
		next = scsilink_poll;
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

	dp->last_to = scsilink_poll;
	dp->rx_timer.expires = jiffies + scsilink_poll;
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
	if (!is_scsilink(sd))
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

	if (!is_scsilink(sd))
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
	/* Convert the millisecond poll knobs to ticks, floored at 1 (a 0-tick
	 * timer would fire immediately and spin). */
	scsilink_poll  = (poll_ms  * HZ) / 1000;
	if (scsilink_poll  < 1) scsilink_poll  = 1;
	scsilink_poll0 = (poll0_ms * HZ) / 1000;
	if (scsilink_poll0 < 1) scsilink_poll0 = 1;

	scsilink_template.usage_count = &mod_use_count_;
	return scsi_register_module(MODULE_SCSI_DEV, &scsilink_template);
}

void cleanup_module(void)
{
	scsi_unregister_module(MODULE_SCSI_DEV, &scsilink_template);
}

#endif /* MODULE */

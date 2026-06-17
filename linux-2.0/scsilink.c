/*
 * scsilink.c - DaynaPORT SCSI/Link Ethernet driver for Linux 2.0.x
 *
 * Copyright (C) 2026 Jeff Flitton <jeff@flitton.dev>
 *
 * A SCSI upper-level device driver that binds to a DaynaPORT
 * SCSI/Link Ethernet adapter (as emulated by ZuluSCSI / BlueSCSI V2 / PiSCSI)
 * and presents it to Linux as a standard Ethernet interface
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
#include <linux/interrupt.h>		/* mark_bh, NET_BH, IMMEDIATE_BH */
#include <linux/tqueue.h>		/* tq_immediate, queue_task (deferred re-issue) */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>

#include <asm/system.h>			/* cli/sti, save_flags */

#include <linux/fs.h>
#include <linux/blk.h>			/* struct request + RQ_* (Scsi_Cmnd) */

#include "scsi.h"			/* pulls in <scsi/scsi.h> and hosts.h */
#include "hosts.h"

#include "daynaport.h"			/* shared DaynaPORT protocol defs + RX parser */

/* ----------------------------------------------------------------------- *
 *  Tunables / constants
 * ----------------------------------------------------------------------- */

#define SCSILINK_MAX		4	/* max DaynaPORT interfaces we manage */

/* DaynaPORT SCSI opcodes, command flags, and RX framing constants live in the
 * shared, version-independent "daynaport.h" (included above; see
 * reference/daynaport.md).  The driver-policy tunables below stay here. */

/* Default and floor for rx_req_len (below): the allocation length we put in the
 * READ CDB.  It is only a hint -- the device may return less or cap it.  BlueSCSI
 * and ZuluSCSI are both based on SCSI2SD: in blind mode (cdb[5]=0xC0) the
 * firmware packs frames into one response until
 * "total + DAYNAPORT_SCSI_PACKET_MAX + 6 > size" (network.c,
 * DAYNAPORT_SCSI_PACKET_MAX = 1524), then hard-stops at 2 frames on a bus-hold
 * guard; 4096 leaves room for
 * that 2-frame batch.  The floor matters: below ~1530 the firmware cannot pack
 * even one max-size frame, so RX would silently stall -- hence
 * SCSILINK_RX_REQ_MIN.  The ceiling is SCSILINK_RBUF_LEN (requesting more than
 * the buffer is incoherent).
 */
#define SCSILINK_RX_REQ		4096	/* default rx_req_len, bytes */
#define SCSILINK_RX_REQ_MIN	2048	/* floor: >= one full frame + pack overhead */

/* RX SCSI transfer (DMA) buffer, and the length handed to scsi_do_cmd.  Sized
 * generously (NetBSD uses the same 16K) to hold a large multi-frame response.
 * We never assume how much the device returns: a batch is a variable-length run
 * of frames (up to the firmware's 2-frame cap) and we do not trust the device's
 * length/flag fields to bound it.  scsilink_rx_kick() zeroes this WHOLE buffer
 * before each READ so a zero-length terminator always lands after the data
 * (however much arrives), and scsilink_rx() walks records bounded by this
 * length; a response larger than the buffer is simply received across
 * successive READs.  Do NOT shrink the pre-zero to the requested length -- a
 * larger batch would run past the cleared region into stale bytes that
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
 *   insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096
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

/* READ-request length (bytes) placed in the CDB; the device may cap or ignore
 * it.  A bare module parameter like the cadence knobs above; init_module
 * validates and clamps it to [SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN] so the
 * poll path can use it unguarded. */
static int rx_req_len = SCSILINK_RX_REQ;

/* RX diagnostics, off by default.  A bare module parameter (insmod scsilink.o
 * debug=1): when set, scsilink_rx() logs per-READ yield and rate every 256
 * READs.  Kept because the RX bottleneck varies by HBA/CPU and these targets
 * have no profiler -- that one line is the diagnostic. */
static int debug = 0;

/* ----------------------------------------------------------------------- *
 *  Per-interface state.  Lives in dev->priv (allocated by init_etherdev).
 * ----------------------------------------------------------------------- */

struct scsilink {
	struct device		*dev;		/* the net interface */
	Scsi_Device		*sdev;		/* our SCSI target */
	struct enet_statistics	stats;
	struct timer_list	rx_timer;	/* paces RX polling when idle/empty */
	struct tq_struct	rx_task;	/* immediate-BH re-issue while RX is hot */
	unsigned char		*rbuf;		/* GFP_DMA RX buffer */
	unsigned char		*tbuf;		/* GFP_DMA TX buffer */
	int			running;	/* interface up + polling */
	int			inited;		/* finish() hardware init done */
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

static void scsilink_rx_kick(unsigned long);
static void scsilink_rx_task(void *);
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
	    && memcmp(sd->vendor, SCSILINK_VENDOR, sizeof(SCSILINK_VENDOR) - 1) == 0
	    && memcmp(sd->model,  SCSILINK_MODEL,  sizeof(SCSILINK_MODEL)  - 1) == 0;
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

/*
 * Deliver one frame (FCS already stripped) up to the stack.  This is the
 * version-specific half of the RX path: the record-walking lives in the shared
 * daynaport_rx_parse() (daynaport.h), which invokes this callback once per good
 * frame.  ctx is our per-interface state.
 */
static void scsilink_deliver(void *ctx, const unsigned char *frame, int len)
{
	struct scsilink *dp = (struct scsilink *) ctx;
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
 * Parse a READ response out of the (pre-zeroed) RX buffer, delivering each frame
 * via scsilink_deliver().  The walk itself -- record framing, the zero-length
 * terminator, why the device's "more" flag is ignored -- lives in the shared
 * daynaport_rx_parse() (daynaport.h).  Returns the TOTAL frame count delivered
 * (nonzero keeps RX polling the next READ back-to-back); the unicast (to-us)
 * count comes back via *ucast, which the fast/idle backoff keys off so
 * broadcast/multicast chatter is delivered but does not pin us in fast-poll.
 */
static int scsilink_rx(struct scsilink *dp, int *ucast)
{
	int errors = 0;
	int got;

	*ucast = 0;
	got = daynaport_rx_parse(dp->rbuf, SCSILINK_RBUF_LEN,
				 scsilink_deliver, dp, &errors, ucast);
	dp->stats.rx_errors += errors;

	/* Optional RX diagnostics (module param `debug`, off by default): per-READ
	 * yield + rate every 256 READs -- tells arrival-limited (mostly empty reads)
	 * from host-pull-limited (full batches at a low read rate) on a given
	 * HBA/CPU.  Counters aggregate across all interfaces; fine for one DaynaPORT. */
	if (debug) {
		static unsigned long dbg_reads, dbg_empty, dbg_frames, dbg_t0;

		if (dbg_reads == 0)
			dbg_t0 = jiffies;
		dbg_reads++;
		dbg_frames += got;
		if (got == 0)
			dbg_empty++;
		if (dbg_reads >= 256) {
			unsigned long dt = jiffies - dbg_t0;
			if (dt == 0)
				dt = 1;
			printk("scsilink: dbg 256 reads/%lu ticks = %lu reads/s, "
			       "%lu empty, %lu frames (%lu.%02lu per read)\n",
			       dt, (256UL * HZ) / dt, dbg_empty, dbg_frames,
			       dbg_frames / 256, ((dbg_frames % 256) * 100) / 256);
			dbg_reads = dbg_empty = dbg_frames = 0;
		}
	}

	return got;
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
	 * (we do not trust the device's flag field for that; see scsilink_rx).  We do
	 * not assume the response fits in the requested length: the buffer is sized
	 * for a worst-case batch and we let the cleared terminator mark the end.
	 * Zero the entire buffer to guarantee a terminator lands after whatever it
	 * sends -- zeroing only the requested length lets a larger batch run past the
	 * cleared region into stale bytes that parse as a bogus record. */
	memset(dp->rbuf, 0, SCSILINK_RBUF_LEN);

	SCpnt->cmd_len = 0;
	/* request the configured batch length (rx_req_len); the device may cap it */
	daynaport_cdb6(cmd, SCSILINK_CMD_RECV, rx_req_len, SCSILINK_RECV_FLAG);

	scsi_do_cmd(SCpnt, cmd, dp->rbuf, SCSILINK_RBUF_LEN, scsilink_recv_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);
}

/* Immediate-queue task: re-issue a READ off the completion path so a productive
 * download polls back-to-back instead of waiting out a poll interval.  Runs in
 * IMMEDIATE_BH context -- a safe place to call scsi_do_cmd(), exactly like the
 * timer -- so a synchronous completion cannot recurse through scsi_do_cmd().
 * Shares scsilink_rx_kick(): if a TX grabbed the command block first, that path
 * falls back to the timer. */
static void scsilink_rx_task(void *data)
{
	scsilink_rx_kick((unsigned long) data);
}

/* READ(6) completion - interrupt context.  Deliver packets, then re-issue. */
static void scsilink_recv_done(Scsi_Cmnd *SCpnt)
{
	struct scsilink *dp = scsilink_find(SCpnt->device);
	int got = 0, ucast = 0, fast;

	if (dp != NULL) {
		/* The DaynaPORT ends the DATA-IN phase early (variable-length
		 * responses), which the SCSI controller reports as a short-transfer
		 * (non-DID_OK) result even though the data is fine.  So we trust
		 * the self-describing, length-checked buffer rather than the SCSI
		 * host status and parse regardless. */
		got = scsilink_rx(dp, &ucast);
	}

	/* release the command block */
	SCpnt->request.rq_status = RQ_INACTIVE;
	wake_up(&SCpnt->device->device_wait);

	if (dp == NULL || !dp->running)
		return;

	/* Maintain the fast/idle hold counter from the to-us count (broadcast does
	 * not pin fast-poll), then pick the next poll:
	 *
	 *   got > 0  the adapter likely has more frames queued -- re-issue the next
	 *            READ immediately via the immediate task queue, so a saturated
	 *            download polls back-to-back at the speed of the SCSI round-trip
	 *            instead of waiting out a poll interval.  We defer through a
	 *            bottom half rather than calling scsi_do_cmd() straight from this
	 *            completion (which could recurse into the mid-layer).  TX stays
	 *            fair: a queued frame's NET_BH runs before our IMMEDIATE_BH, so
	 *            an ACK takes the command block first and RX falls to the timer
	 *            for that one cycle.
	 *   got == 0 nothing waiting -- relax to the timer at the fast/idle cadence.
	 */
	fast = daynaport_poll_fast(ucast, &dp->fast_left, fast_hold);
	if (got > 0) {
		queue_task(&dp->rx_task, &tq_immediate);
		mark_bh(IMMEDIATE_BH);
	} else {
		dp->rx_timer.expires = jiffies + (fast ? scsilink_poll0 : scsilink_poll);
		add_timer(&dp->rx_timer);
	}
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
	daynaport_cdb6(cmd, SCSILINK_CMD_SEND, len, SCSILINK_SEND_FLAG);

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

	dp->rx_timer.data     = (unsigned long) dp;
	dp->rx_timer.function = scsilink_rx_kick;
	init_timer(&dp->rx_timer);
	dp->rx_task.routine   = scsilink_rx_task;	/* sync/next zeroed by memset above */
	dp->rx_task.data      = dp;

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
		daynaport_cdb6(cmd, SCSILINK_CMD_ENABLE, 0, SCSILINK_ENABLE_FLAG);
		rc = scsilink_scsi_sync(dp, cmd, NULL, 0);
		if (rc)
			printk("scsilink: %s: enable failed (0x%x)\n",
			       dp->dev->name, rc);
		scsilink_delay(HZ / 2);

		/* Read MAC + stats (0x09, 18 bytes) into the RX buffer. */
		daynaport_cdb6(cmd, SCSILINK_CMD_STATS, SCSILINK_STATS_LEN, 0);
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

	/* Clamp the requested READ length into [MIN, buffer]: too small and BlueSCSI
	 * packs zero frames (RX stalls); larger than the buffer is incoherent. */
	if (rx_req_len < SCSILINK_RX_REQ_MIN || rx_req_len > SCSILINK_RBUF_LEN) {
		int c = rx_req_len < SCSILINK_RX_REQ_MIN
		      ? SCSILINK_RX_REQ_MIN : SCSILINK_RBUF_LEN;
		printk("scsilink: rx_req_len %d out of [%d, %d]; using %d\n",
		       rx_req_len, SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN, c);
		rx_req_len = c;
	}

	printk("scsilink: poll_ms=%d (%d ticks), poll0_ms=%d (%d ticks), "
	       "fast_hold=%d, rx_req_len=%d (HZ=%d)\n",
	       poll_ms, scsilink_poll, poll0_ms, scsilink_poll0,
	       fast_hold, rx_req_len, HZ);

	scsilink_template.usage_count = &mod_use_count_;
	return scsi_register_module(MODULE_SCSI_DEV, &scsilink_template);
}

void cleanup_module(void)
{
	scsi_unregister_module(MODULE_SCSI_DEV, &scsilink_template);
}

#endif /* MODULE */

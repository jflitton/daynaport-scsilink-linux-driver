/*
 * scsilink.c - DaynaPORT SCSI/Link Ethernet driver for Linux 2.4.x
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
 * The protocol opcodes/framing and the RX record parser are shared verbatim with
 * every other kernel-version driver in this repo via lib/daynaport.h; this file
 * supplies only the 2.4-specific glue.  Protocol reference: reference/daynaport.md.
 *
 * Design:
 *   - Registers as an upper-level SCSI device driver (Scsi_Device_Template,
 *     MODULE_SCSI_DEV): the mid-layer offers every scanned device to detect()/
 *     attach(), and we claim the TYPE_PROCESSOR Dayna/SCSI/Link match.
 *   - All SCSI I/O goes through one persistent Scsi_Request per interface; the
 *     adapter handles one command at a time, so a `busy` flag (under a spinlock)
 *     serializes everything onto that request.
 *   - There is no RX interrupt, so receive is polled.  A fair I/O engine shares
 *     the single command block between RX polls and a TX queue: while frames are
 *     queued it cycles commands immediately -- up to tx_burst sends, then a
 *     forced RX poll -- so neither direction starves (a download's TX ACKs and
 *     an upload's RX ACKs both get serviced).  When the TX queue drains it hands
 *     RX back to a timer at an adaptive fast/idle cadence.  The next command is
 *     started from a tasklet so a synchronous SCSI completion cannot recurse.
 *     enable + MAC read at attach use the synchronous scsi_wait_req().
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>			/* kmalloc / kfree (linux/malloc.h in 2.0) */
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>		/* tasklet */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>

#include <linux/blkdev.h>		/* request_queue_t, used by scsi.h */

#include "scsi.h"			/* Scsi_Device, Scsi_Cmnd, Scsi_Request, ... */
#include "hosts.h"			/* Scsi_Device_Template, scsi_register_module */

#include "daynaport.h"			/* shared DaynaPORT protocol defs + RX parser */

/* ----------------------------------------------------------------------- *
 *  Tunables / constants
 * ----------------------------------------------------------------------- */

#define SCSILINK_MAX		4	/* max DaynaPORT interfaces we manage */

/* DaynaPORT SCSI opcodes, command flags, and RX framing constants live in the
 * shared, version-independent "daynaport.h" (included above; see
 * reference/daynaport.md).  The driver-policy tunables below stay here. */

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

/* RX SCSI transfer buffer, and the length handed to the READ.  Sized generously
 * (NetBSD uses the same 16K) to hold a large multi-frame response.  We never
 * assume how much the device returns: BlueSCSI bounds a batch to the requested
 * length, but ZuluSCSI ignores the request and caps only on a byte budget, so it
 * can return more than we ask for.  scsilink_rx_issue() zeroes this WHOLE buffer
 * before each READ so a zero-length terminator always lands after the data
 * (however much arrives), and daynaport_rx_parse() walks records bounded by this
 * length.  Do NOT shrink the pre-zero to the requested length -- a batch
 * exceeding it would run past the cleared region into stale bytes that parse as a
 * bogus record.  No GFP_DMA: the SCSI mid-layer bounces for any addressing
 * constraint of the underlying HBA. */
#define SCSILINK_RBUF_LEN	16384
#define SCSILINK_TBUF_LEN	1536	/* one frame + slack */

/* TX queue depth before we push back on the stack with netif_stop_queue. */
#define SCSILINK_TXQ_MAX	16

/* Default tx_burst: frames to WRITE before yielding the single command slot to
 * one RX poll (see the tx_burst param below for the rationale). */
#define SCSILINK_TX_BURST	16

/* SCSI command timeout / retries.  Keep the timeout generous so a momentarily
 * busy bus does not trigger the mid-layer's reset escalation (which would
 * disturb other targets and clear our enable state). */
#define SCSILINK_TIMEOUT	(10 * HZ)
#define SCSILINK_RETRIES	1
#define SCSILINK_TX_WATCHDOG	(5 * HZ)

/* RX poll cadence.  This governs only EMPTY polling: a READ that returns frames
 * is followed immediately by the next (see scsilink_kick), so a live download
 * polls back-to-back at the speed of the SCSI round-trip -- NOT on this timer.
 * A fixed inter-READ delay here would cap RX at one batch per poll, which on a
 * device that packs only a couple of frames per READ throttles a download to a
 * small fraction of line rate.  Once a READ comes back empty we hold the fast
 * rate for fast_hold more polls (a download's queue goes briefly empty as TCP's
 * window breathes), then relax to the idle rate.
 *
 * The knobs are module parameters, so the cadence can be swept at load time:
 *
 *   insmod scsilink.o poll_ms=80 poll0_ms=20 fast_hold=16 rx_req_len=4096 tx_burst=16
 *
 * poll_ms   idle rate: interval between empty READs when no data is waiting.
 * poll0_ms  fast rate: interval between empty READs during the post-activity hold.
 * fast_hold number of empty polls to stay fast before relaxing to idle.
 *
 * poll_ms / poll0_ms are converted to ticks (scsilink_poll / scsilink_poll0) in
 * scsilink_init, each floored at 1 tick. */
static int poll_ms   = 80;
static int poll0_ms  = 20;
static int fast_hold = 16;
MODULE_PARM(poll_ms,   "i");
MODULE_PARM(poll0_ms,  "i");
MODULE_PARM(fast_hold, "i");
MODULE_PARM_DESC(poll_ms,   "RX idle poll interval, milliseconds (default 80)");
MODULE_PARM_DESC(poll0_ms,  "RX fast poll interval, milliseconds (default 20)");
MODULE_PARM_DESC(fast_hold, "empty polls to stay fast before relaxing (default 16)");

static int scsilink_poll  = ((HZ / 12) ? (HZ / 12) : 1);	/* idle, ticks */
static int scsilink_poll0 = ((HZ / 50) ? (HZ / 50) : 1);	/* fast, ticks */

/* Fairness knob.  The adapter does one command at a time, so RX polling and TX
 * share a single SCSI request.  tx_burst caps consecutive TX frames before a
 * forced RX poll, so neither direction starves: a download's TX ACKs and an
 * upload's RX ACKs both get serviced.  Lower favours RX, higher favours upload
 * throughput. */
static int tx_burst = SCSILINK_TX_BURST;
MODULE_PARM(tx_burst, "i");
MODULE_PARM_DESC(tx_burst,
	"max TX frames between forced RX polls (default 16, clamped to [1, 16])");

/* READ-request length (bytes) placed in the CDB; the device may cap or ignore
 * it.  A module parameter like the cadence knobs above; scsilink_init validates
 * and clamps it to [SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN] so the poll path can
 * use it unguarded. */
static int rx_req_len = SCSILINK_RX_REQ;
MODULE_PARM(rx_req_len, "i");
MODULE_PARM_DESC(rx_req_len,
	"READ request length in bytes; device may cap or ignore it "
	"(default 4096, clamped to [2048, 16384])");

/* RX diagnostics, off by default.  When set, scsilink_rx() logs per-READ yield
 * and rate every 256 READs -- the RX bottleneck varies by HBA/CPU and these
 * targets have no profiler, so that line is the field diagnostic. */
static int debug = 0;
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "log per-READ RX stats every 256 reads (default 0=off)");

/* ----------------------------------------------------------------------- *
 *  Per-interface state.  Lives in dev->priv (allocated by init_etherdev).
 * ----------------------------------------------------------------------- */

struct scsilink {
	struct net_device	*dev;		/* the net interface */
	Scsi_Device		*sdev;		/* our SCSI target */
	Scsi_Request		*sreq;		/* reused for every command to sdev */
	struct net_device_stats	stats;
	struct timer_list	rx_timer;	/* paces RX polling when txq is empty */
	struct tasklet_struct	io_task;	/* defers "start next command" off completions */
	spinlock_t		lock;		/* guards busy / txq / tx_run / fast_left */
	int			busy;		/* a SCSI command is in flight on sreq */
	struct sk_buff_head	txq;		/* frames queued by xmit, awaiting WRITE */
	int			tx_run;		/* consecutive TX sends since the last RX */
	unsigned char		*rbuf;		/* RX transfer buffer */
	unsigned char		*tbuf;		/* TX transfer buffer */
	int			running;	/* interface up + polling */
	int			inited;		/* finish() hardware init done */
	int			fast_left;	/* fast polls remaining after activity */
	int			rx_hot;		/* last READ returned frames -> poll next now */
};

static struct scsilink *scsilink_devs[SCSILINK_MAX];

/* ----------------------------------------------------------------------- *
 *  Forward decls
 * ----------------------------------------------------------------------- */

static int  scsilink_detect(Scsi_Device *);
static int  scsilink_attach(Scsi_Device *);
static void scsilink_finish(void);
static void scsilink_detach(Scsi_Device *);

static int  scsilink_open(struct net_device *);
static int  scsilink_stop(struct net_device *);
static int  scsilink_xmit(struct sk_buff *, struct net_device *);
static struct net_device_stats *scsilink_get_stats(struct net_device *);
static void scsilink_set_multicast(struct net_device *);
static void scsilink_tx_timeout(struct net_device *);

static void scsilink_kick(struct scsilink *);
static void scsilink_rx_issue(struct scsilink *);
static void scsilink_tx_issue(struct scsilink *, struct sk_buff *);
static void scsilink_timer(unsigned long);
static void scsilink_io_task(unsigned long);
static void scsilink_recv_done(Scsi_Cmnd *);
static void scsilink_send_done(Scsi_Cmnd *);

static struct Scsi_Device_Template scsilink_template = {
	.name		= "scsilink",
	.tag		= "sl",
	.scsi_type	= TYPE_PROCESSOR,	/* 0x03 */
	.detect		= scsilink_detect,
	.attach		= scsilink_attach,
	.finish		= scsilink_finish,
	.detach		= scsilink_detach,
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

/* Sleep ~ticks jiffies in process context (used at attach/finish/teardown). */
static void scsilink_delay(int ticks)
{
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(ticks);
}

/* Wait (process context) for any in-flight command on dp->sreq to complete, so
 * the request and buffers can be reused or released without racing a done
 * callback.  Bounded a little past the SCSI command timeout. */
static void scsilink_drain(struct scsilink *dp)
{
	int tick  = (HZ / 100) ? (HZ / 100) : 1;
	int tries = SCSILINK_TIMEOUT / tick + 10;

	while (dp->busy && tries-- > 0) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(tick);
	}
}

/*
 * Issue one 6-byte-CDB SCSI command synchronously and block until it completes.
 * Process (finish) context only.  Returns sr_result (0 == good).
 */
static int scsilink_scsi_sync(struct scsilink *dp, unsigned char *cdb,
			      void *buf, int buflen, int dir)
{
	dp->sreq->sr_data_direction = dir;
	scsi_wait_req(dp->sreq, cdb, buf, buflen, SCSILINK_TIMEOUT, SCSILINK_RETRIES);
	return dp->sreq->sr_result;
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
	dp->stats.rx_bytes += len;
	dp->stats.rx_packets++;
	netif_rx(skb);
}

/*
 * Parse a READ response out of the (pre-zeroed) RX buffer, delivering each frame
 * via scsilink_deliver().  The walk itself -- record framing, the zero-length
 * terminator, why the device's "more" flag is ignored -- lives in the shared
 * daynaport_rx_parse() (daynaport.h).  Returns the TOTAL frame count delivered
 * (the engine polls back-to-back while that is nonzero); the unicast (to-us)
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
	 * from host-pull-limited (full batches at a low read rate) on a given HBA/CPU.
	 * Counters aggregate across all interfaces; fine for one DaynaPORT. */
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
			printk(KERN_INFO "scsilink: dbg 256 reads/%lu ticks = %lu reads/s, "
			       "%lu empty, %lu frames (%lu.%02lu per read)\n",
			       dt, (256UL * HZ) / dt, dbg_empty, dbg_frames,
			       dbg_frames / 256, ((dbg_frames % 256) * 100) / 256);
			dbg_reads = dbg_empty = dbg_frames = 0;
		}
	}

	return got;
}

/* Issue one READ(6).  The caller has already claimed the command block
 * (dp->busy).  Not a sleeping context. */
static void scsilink_rx_issue(struct scsilink *dp)
{
	unsigned char cmd[6];

	/* Pre-zero the whole buffer so the bytes just past the device's data read
	 * back as a zero-length header -- how daynaport_rx_parse() knows to stop.
	 * Zero the ENTIRE buffer, not just rx_req_len: ZuluSCSI ignores the
	 * requested length, so a larger batch would otherwise run past the cleared
	 * region into stale bytes that parse as a bogus record. */
	memset(dp->rbuf, 0, SCSILINK_RBUF_LEN);

	daynaport_cdb6(cmd, SCSILINK_CMD_RECV, rx_req_len, SCSILINK_RECV_FLAG);
	dp->sreq->sr_data_direction = SCSI_DATA_READ;
	scsi_do_req(dp->sreq, cmd, dp->rbuf, SCSILINK_RBUF_LEN, scsilink_recv_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);
}

/* READ(6) completion - SCSI done context.  Deliver packets, then defer picking
 * the next command (the tasklet avoids recursion on a synchronous completion). */
static void scsilink_recv_done(Scsi_Cmnd *SCpnt)
{
	struct scsilink *dp = scsilink_find(SCpnt->device);
	unsigned long flags;
	int got, ucast;

	if (dp == NULL)
		return;

	/* The DaynaPORT ends the DATA-IN phase early (variable-length responses),
	 * which the HBA reports as a short transfer even though the data is fine --
	 * so we trust the self-describing, length-checked buffer and parse
	 * regardless of the SCSI result. */
	got = scsilink_rx(dp, &ucast);

	spin_lock_irqsave(&dp->lock, flags);
	dp->busy = 0;
	/* To-us frames re-arm the fast/idle backoff; any frames at all mark RX
	 * "hot" so the engine issues the next READ back-to-back (scsilink_kick),
	 * rather than waiting out a poll interval while a download is streaming. */
	daynaport_poll_fast(ucast, &dp->fast_left, fast_hold);
	dp->rx_hot = (got > 0);
	spin_unlock_irqrestore(&dp->lock, flags);

	if (dp->running)
		tasklet_schedule(&dp->io_task);
}

/* ----------------------------------------------------------------------- *
 *  Transmit
 * ----------------------------------------------------------------------- */

/* Issue one WRITE(6) for skb.  The caller has already claimed the command block
 * (dp->busy) and dequeued skb. */
static void scsilink_tx_issue(struct scsilink *dp, struct sk_buff *skb)
{
	struct net_device *dev = dp->dev;
	unsigned char cmd[6];
	int len;

	/* We do not advertise scatter/gather, so the stack hands us a linear skb. */
	len = skb->len;
	if (len > SCSILINK_MAXFRAME)
		len = SCSILINK_MAXFRAME;
	memcpy(dp->tbuf, skb->data, len);
	if (len < SCSILINK_MINFRAME) {		/* pad runt frames */
		memset(dp->tbuf + len, 0, SCSILINK_MINFRAME - len);
		len = SCSILINK_MINFRAME;
	}

	daynaport_cdb6(cmd, SCSILINK_CMD_SEND, len, SCSILINK_SEND_FLAG);
	dev->trans_start = jiffies;
	dp->sreq->sr_data_direction = SCSI_DATA_WRITE;
	scsi_do_req(dp->sreq, cmd, dp->tbuf, len, scsilink_send_done,
		    SCSILINK_TIMEOUT, SCSILINK_RETRIES);

	dev_kfree_skb(skb);			/* copied into tbuf; safe to free */
}

/* WRITE(6) completion - SCSI done context. */
static void scsilink_send_done(Scsi_Cmnd *SCpnt)
{
	struct scsilink *dp = scsilink_find(SCpnt->device);
	unsigned long flags;

	if (dp == NULL)
		return;

	if (host_byte(SCpnt->result) == DID_OK)
		dp->stats.tx_packets++;
	else
		dp->stats.tx_errors++;

	spin_lock_irqsave(&dp->lock, flags);
	dp->busy = 0;
	spin_unlock_irqrestore(&dp->lock, flags);

	if (dp->running)
		tasklet_schedule(&dp->io_task);
}

/* ----------------------------------------------------------------------- *
 *  I/O engine -- fair arbiter over the single SCSI command block
 *
 *  The adapter does one command at a time, so RX polling and TX share a single
 *  Scsi_Request.  While TX is queued we cycle commands immediately -- up to
 *  tx_burst sends, then a forced RX poll -- so neither direction starves (a
 *  download's TX ACKs and an upload's RX ACKs both get through).  When the TX
 *  queue is empty we hand RX back to the timer at the fast/idle cadence.
 * ----------------------------------------------------------------------- */

/* Start the next command if the block is free.  Runs from the tasklet (after a
 * completion) and from xmit; never sleeps. */
static void scsilink_kick(struct scsilink *dp)
{
	unsigned long flags;
	struct sk_buff *skb = NULL;
	int op_tx = 0;

	spin_lock_irqsave(&dp->lock, flags);
	if (dp->busy || !dp->running) {
		spin_unlock_irqrestore(&dp->lock, flags);
		return;
	}

	if (!skb_queue_empty(&dp->txq)) {
		dp->busy = 1;
		if (dp->tx_run < tx_burst) {		/* send a queued frame */
			op_tx = 1;
			dp->tx_run++;
			skb = __skb_dequeue(&dp->txq);
			if (netif_queue_stopped(dp->dev) &&
			    skb_queue_len(&dp->txq) < SCSILINK_TXQ_MAX)
				netif_wake_queue(dp->dev);
		} else {				/* burst spent: force an RX */
			dp->tx_run = 0;
		}
		spin_unlock_irqrestore(&dp->lock, flags);
		if (op_tx)
			scsilink_tx_issue(dp, skb);
		else
			scsilink_rx_issue(dp);
		return;
	}

	/* TX queue empty.  If the last READ pulled in frames, issue the next one
	 * back-to-back: a saturated download always has more queued, and the SCSI
	 * round-trip itself paces us, so we drain at bus speed instead of one batch
	 * per poll interval.  Only once a READ comes back empty do we fall to the
	 * timer -- poll0 through the brief post-activity hold, then the idle rate. */
	dp->tx_run = 0;
	if (dp->rx_hot) {
		dp->busy = 1;
		spin_unlock_irqrestore(&dp->lock, flags);
		scsilink_rx_issue(dp);
		return;
	}
	mod_timer(&dp->rx_timer,
		  jiffies + (dp->fast_left > 0 ? scsilink_poll0 : scsilink_poll));
	spin_unlock_irqrestore(&dp->lock, flags);
}

/* Tasklet: defers scsilink_kick off the SCSI completion path so a synchronous
 * completion cannot recurse through scsi_do_req. */
static void scsilink_io_task(unsigned long arg)
{
	scsilink_kick((struct scsilink *) arg);
}

/* Poll timer: time for a cadence RX poll (armed only when the TX queue is
 * empty).  Timer (softirq) context - must not sleep. */
static void scsilink_timer(unsigned long arg)
{
	struct scsilink *dp = (struct scsilink *) arg;
	unsigned long flags;

	spin_lock_irqsave(&dp->lock, flags);
	if (dp->busy || !dp->running || dp->sreq == NULL) {
		spin_unlock_irqrestore(&dp->lock, flags);
		return;
	}
	dp->busy   = 1;
	dp->tx_run = 0;
	spin_unlock_irqrestore(&dp->lock, flags);

	scsilink_rx_issue(dp);
}

/* ----------------------------------------------------------------------- *
 *  Net device hooks
 * ----------------------------------------------------------------------- */

/* Queue a frame for the I/O engine.  Cannot sleep, so we never issue SCSI here;
 * we enqueue and kick the engine, which serializes TX fairly against RX. */
static int scsilink_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;
	unsigned long flags;

	if (dp->sreq == NULL) {			/* device never finished init */
		dev_kfree_skb(skb);
		dp->stats.tx_errors++;
		return 0;
	}

	spin_lock_irqsave(&dp->lock, flags);
	__skb_queue_tail(&dp->txq, skb);
	if (skb_queue_len(&dp->txq) >= SCSILINK_TXQ_MAX)
		netif_stop_queue(dev);
	spin_unlock_irqrestore(&dp->lock, flags);

	scsilink_kick(dp);			/* send now if the block is idle */
	return 0;
}

static int scsilink_open(struct net_device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;

	if (dp->sreq == NULL)			/* finish() failed to init us */
		return -ENODEV;

	dp->running   = 1;
	dp->fast_left = 0;
	dp->tx_run    = 0;
	dp->rx_hot    = 0;
	mod_timer(&dp->rx_timer, jiffies + scsilink_poll);	/* start polling */

	netif_start_queue(dev);
	MOD_INC_USE_COUNT;
	return 0;
}

static int scsilink_stop(struct net_device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;

	netif_stop_queue(dev);
	dp->running = 0;
	del_timer(&dp->rx_timer);
	/* A command may still be in flight; its done callback sees running == 0 and
	 * will not re-schedule the engine.  Wait it out, stop the tasklet, and drop
	 * anything still queued, so nothing touches dp after we return. */
	scsilink_drain(dp);
	tasklet_kill(&dp->io_task);
	skb_queue_purge(&dp->txq);

	MOD_DEC_USE_COUNT;
	return 0;
}

static struct net_device_stats *scsilink_get_stats(struct net_device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;
	return &dp->stats;
}

/* v1: no multicast filtering.  BlueSCSI ignores 0x0D and 2.0f receives all
 * broadcast anyway; and this hook may run where we cannot sleep to issue a SCSI
 * command.  See reference/daynaport.md s4.6. */
static void scsilink_set_multicast(struct net_device *dev)
{
}

/* TX watchdog (dev->watchdog_timeo).  The mid-layer has its own command timeout,
 * so this is a backstop: log, count, and re-kick the engine. */
static void scsilink_tx_timeout(struct net_device *dev)
{
	struct scsilink *dp = (struct scsilink *) dev->priv;

	printk(KERN_WARNING "scsilink: %s: TX timeout\n", dev->name);
	dp->stats.tx_errors++;
	dev->trans_start = jiffies;
	netif_wake_queue(dev);
	if (dp->running)
		tasklet_schedule(&dp->io_task);
}

/* ----------------------------------------------------------------------- *
 *  SCSI device template callbacks
 * ----------------------------------------------------------------------- */

static int scsilink_detect(Scsi_Device *sd)
{
	if (!is_scsilink(sd))
		return 0;
	scsilink_template.dev_noticed++;
	printk(KERN_INFO "scsilink: detected DaynaPORT SCSI/Link at scsi%d,"
	       " channel %d, id %d, lun %d\n",
	       sd->host->host_no, sd->channel, sd->id, sd->lun);
	return 1;
}

/*
 * attach() runs before the mid-layer has built this device's command blocks, so
 * we must NOT issue any SCSI command here.  We only allocate state and the net
 * device.  Hardware init (enable, MAC read) happens in finish().
 */
static int scsilink_attach(Scsi_Device *sd)
{
	struct net_device *dev;
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
	spin_lock_init(&dp->lock);
	skb_queue_head_init(&dp->txq);

	dp->rbuf = kmalloc(SCSILINK_RBUF_LEN, GFP_KERNEL);
	dp->tbuf = kmalloc(SCSILINK_TBUF_LEN, GFP_KERNEL);
	if (dp->rbuf == NULL || dp->tbuf == NULL) {
		if (dp->rbuf) kfree(dp->rbuf);
		if (dp->tbuf) kfree(dp->tbuf);
		unregister_netdev(dev);
		kfree(dev);
		sd->attached--;
		return 1;
	}

	dev->open		= scsilink_open;
	dev->stop		= scsilink_stop;
	dev->hard_start_xmit	= scsilink_xmit;
	dev->get_stats		= scsilink_get_stats;
	dev->set_multicast_list	= scsilink_set_multicast;
	dev->tx_timeout		= scsilink_tx_timeout;
	dev->watchdog_timeo	= SCSILINK_TX_WATCHDOG;

	init_timer(&dp->rx_timer);
	dp->rx_timer.data     = (unsigned long) dp;
	dp->rx_timer.function = scsilink_timer;
	tasklet_init(&dp->io_task, scsilink_io_task, (unsigned long) dp);

	scsilink_devs[i] = dp;
	scsilink_template.nr_dev++;
	return 0;
}

/*
 * finish() runs after the mid-layer has built command blocks for every attached
 * device, so this is where we talk to the hardware: allocate the per-device SCSI
 * request, enable the interface, wait the required settle, and read the MAC.  The
 * net device was already created and registered by init_etherdev() in attach().
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

		dp->sreq = scsi_allocate_request(dp->sdev);
		if (dp->sreq == NULL) {
			printk(KERN_ERR "scsilink: %s: cannot allocate SCSI request\n",
			       dp->dev->name);
			continue;
		}

		/* ENABLE the interface (0x0E, flag 0x80), no data phase, then wait
		 * ~0.5s for it to settle. */
		daynaport_cdb6(cmd, SCSILINK_CMD_ENABLE, 0, SCSILINK_ENABLE_FLAG);
		rc = scsilink_scsi_sync(dp, cmd, NULL, 0, SCSI_DATA_NONE);
		if (rc)
			printk(KERN_WARNING "scsilink: %s: enable failed (0x%x)\n",
			       dp->dev->name, rc);
		scsilink_delay(HZ / 2);

		/* Read MAC + stats (0x09, 18 bytes) into the RX buffer. */
		daynaport_cdb6(cmd, SCSILINK_CMD_STATS, SCSILINK_STATS_LEN, 0);
		rc = scsilink_scsi_sync(dp, cmd, dp->rbuf, SCSILINK_STATS_LEN,
					SCSI_DATA_READ);
		if (rc) {
			printk(KERN_ERR "scsilink: %s: MAC read failed (0x%x)\n",
			       dp->dev->name, rc);
			continue;	/* leave dev_addr zeroed */
		}
		memcpy(dp->dev->dev_addr, dp->rbuf, ETH_ALEN);

		printk(KERN_INFO "scsilink: %s: DaynaPORT SCSI/Link, MAC "
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
		struct net_device *dev;
		if (dp == NULL || dp->sdev != sd)
			continue;

		dev = dp->dev;			/* dp is embedded in dev's allocation */

		dp->running = 0;
		del_timer(&dp->rx_timer);
		scsilink_drain(dp);		/* no command in flight after this */
		tasklet_kill(&dp->io_task);
		skb_queue_purge(&dp->txq);

		unregister_netdev(dev);
		if (dp->sreq)
			scsi_release_request(dp->sreq);
		if (dp->rbuf) kfree(dp->rbuf);
		if (dp->tbuf) kfree(dp->tbuf);

		scsilink_devs[i] = NULL;
		scsilink_template.nr_dev--;
		sd->attached--;

		kfree(dev);			/* frees dev + priv (dp); must be last */
	}
}

/* ----------------------------------------------------------------------- *
 *  Module glue.  Registers as an upper-level SCSI *device* driver
 *  (MODULE_SCSI_DEV) - NOT a host adapter.
 * ----------------------------------------------------------------------- */

static int __init scsilink_init(void)
{
	/* Convert the millisecond poll knobs to ticks, floored at 1 (a 0-tick timer
	 * would fire immediately and spin). */
	scsilink_poll  = (poll_ms  * HZ) / 1000;
	if (scsilink_poll  < 1) scsilink_poll  = 1;
	scsilink_poll0 = (poll0_ms * HZ) / 1000;
	if (scsilink_poll0 < 1) scsilink_poll0 = 1;

	/* Clamp tx_burst to [1, SCSILINK_TXQ_MAX]: 0 would never send TX, and a
	 * burst larger than the queue can hold just drains it whole. */
	if (tx_burst < 1)
		tx_burst = 1;
	else if (tx_burst > SCSILINK_TXQ_MAX)
		tx_burst = SCSILINK_TXQ_MAX;

	/* Clamp the requested READ length into [MIN, buffer]: too small and BlueSCSI
	 * packs zero frames (RX stalls); larger than the buffer is incoherent. */
	if (rx_req_len < SCSILINK_RX_REQ_MIN || rx_req_len > SCSILINK_RBUF_LEN) {
		int c = rx_req_len < SCSILINK_RX_REQ_MIN
		      ? SCSILINK_RX_REQ_MIN : SCSILINK_RBUF_LEN;
		printk(KERN_WARNING "scsilink: rx_req_len %d out of [%d, %d]; using %d\n",
		       rx_req_len, SCSILINK_RX_REQ_MIN, SCSILINK_RBUF_LEN, c);
		rx_req_len = c;
	}

	scsilink_template.module = THIS_MODULE;
	return scsi_register_module(MODULE_SCSI_DEV, &scsilink_template);
}

static void __exit scsilink_exit(void)
{
	scsi_unregister_module(MODULE_SCSI_DEV, &scsilink_template);
}

module_init(scsilink_init);
module_exit(scsilink_exit);

MODULE_DESCRIPTION("DaynaPORT SCSI/Link Ethernet driver");
MODULE_AUTHOR("Jeff Flitton <jeff@flitton.dev>");
MODULE_LICENSE("GPL");

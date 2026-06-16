/* SPDX-License-Identifier: GPL-2.0 */
/*
 * daynaport.h - DaynaPORT SCSI/Link protocol definitions, CDB assembly, RX frame
 *               parser, and adaptive-poll cadence: the version-independent core
 *               shared across every kernel-version driver in this repo.
 *
 * This header is deliberately free of any kernel API (no sk_buff, net_device,
 * or stats types) and is C89-clean, so the same code compiles under gcc 2.7.x
 * for the Linux 2.0 driver and under a modern toolchain for later ports.  The
 * per-frame delivery -- which necessarily touches each era's network stack --
 * is supplied by the caller as a callback.
 *
 * Protocol reference: reference/daynaport.md.
 */

#ifndef DAYNAPORT_H
#define DAYNAPORT_H

/* SCSI opcodes the adapter uses to move Ethernet frames. */
#define SCSILINK_CMD_RECV	0x08	/* READ(6)  - receive packet(s) */
#define SCSILINK_CMD_STATS	0x09	/* retrieve MAC + stats (18 bytes) */
#define SCSILINK_CMD_SEND	0x0A	/* WRITE(6) - transmit a packet */
#define SCSILINK_CMD_ENABLE	0x0E	/* enable/disable the interface */

#define SCSILINK_STATS_LEN	0x12	/* 18 bytes: 6 MAC + 3x4 counters */
#define SCSILINK_ENABLE_FLAG	0x80	/* cdb[5] for ENABLE */
#define SCSILINK_RECV_FLAG	0xC0	/* cdb[5] for READ: blind mode (per NetBSD dse) */
#define SCSILINK_SEND_FLAG	0x00	/* cdb[5] for WRITE: raw frame format */

/* SCSI INQUIRY identity of a DaynaPORT SCSI/Link: a TYPE_PROCESSOR device whose
 * vendor/product strings match these.  Drivers do the type check and the memcmp
 * themselves (the scsi_device struct differs across kernels); centralizing the
 * strings lets the match lengths come from the literals instead of magic numbers
 * (memcmp <string> with sizeof(SCSILINK_VENDOR) - 1, etc.). */
#define SCSILINK_VENDOR		"Dayna"
#define SCSILINK_MODEL		"SCSI/Link"

/* RX framing: each record is a 2-byte big-endian length + 4-byte flag field,
 * followed by <length> bytes of frame including a trailing 4-byte Ethernet FCS. */
#define SCSILINK_RX_HDR		6	/* 2-byte length + 4-byte flag field */
#define SCSILINK_FCS_LEN	4	/* trailing Ethernet FCS, stripped */
#define SCSILINK_MINFRAME	60	/* minimum Ethernet payload (also TX pad) */
#define SCSILINK_MAXFRAME	1514	/* == ETH_FRAME_LEN, no FCS */
#define SCSILINK_RX_MAXREC	(SCSILINK_MAXFRAME + SCSILINK_FCS_LEN)

/*
 * Build a DaynaPORT 6-byte CDB in cdb[0..5]: opcode in cdb[0], a big-endian
 * transfer length in cdb[3..4], the per-command flag in cdb[5]; cdb[1..2] are
 * always zero.  All four commands this driver issues share this one shape, so
 * ENABLE passes len 0 (no data phase) and STATS passes SCSILINK_STATS_LEN.  Pure
 * byte assembly, no kernel API.
 */
static void daynaport_cdb6(unsigned char *cdb, unsigned char op,
			   int len, unsigned char flag)
{
	cdb[0] = op;
	cdb[1] = 0;
	cdb[2] = 0;
	cdb[3] = (len >> 8) & 0xff;
	cdb[4] = len & 0xff;
	cdb[5] = flag;
}

/*
 * Per-frame delivery callback.  daynaport_rx_parse() calls this once for each
 * good frame, with the FCS already stripped; ctx is passed through untouched so
 * the caller can recover its per-interface state.
 */
typedef void (*daynaport_deliver_fn)(void *ctx, const unsigned char *frame, int len);

/*
 * Walk a DaynaPORT READ(6) response and deliver each frame via deliver().
 *
 *   buf/buflen  the RX transfer buffer and its length.  The caller MUST pre-zero
 *               the buffer before the READ: the device returns a variable number
 *               of records and we terminate on a zero-length header, so the
 *               cleared tail past the device's data reads back as that
 *               terminator.  (The device's per-record "more" flag scopes only to
 *               one response and is NOT a reliable cross-READ signal, so we
 *               ignore it and rely on the length headers and the zero
 *               terminator.  We also cannot bound the walk by an ISA HBA's
 *               residual: it reports the device's early end of the DATA-IN phase
 *               as a short transfer.)
 *   deliver/ctx per-frame callback and its opaque context.
 *   errors      if non-NULL, incremented by 1 when a garbled record is seen (the
 *               walk then stops); lets the caller fold it into its own stats
 *               without this code knowing the stats representation.
 *   unicast     if non-NULL, incremented for each frame addressed to a single
 *               station (Ethernet group bit clear).  The DaynaPORT only ever
 *               delivers an initiator its own unicast plus broadcast/multicast,
 *               so this is effectively "frames that were really for us" -- the
 *               signal an adaptive RX poll should accelerate on, so ambient
 *               broadcast/multicast chatter does not pin it in fast-poll.
 *
 * Returns the number of frames delivered.
 */
static int daynaport_rx_parse(const unsigned char *buf, int buflen,
			      daynaport_deliver_fn deliver, void *ctx,
			      int *errors, int *unicast)
{
	const unsigned char *p = buf;
	int avail = buflen;
	int n = 0;
	int len;

	while (avail >= SCSILINK_RX_HDR) {
		len    = (p[0] << 8) | p[1];	/* big-endian, includes FCS */
		p     += SCSILINK_RX_HDR;
		avail -= SCSILINK_RX_HDR;

		if (len == 0)			/* zeroed tail / no (more) data */
			break;

		if (len < SCSILINK_MINFRAME + SCSILINK_FCS_LEN ||
		    len > SCSILINK_RX_MAXREC) {
			if (errors)		/* garbled record; bail out */
				*errors += 1;
			break;
		}

		if (len > avail)		/* would run off the buffer */
			break;

		deliver(ctx, p, len - SCSILINK_FCS_LEN);
		n += 1;
		if (unicast && !(p[0] & 0x01))	/* group bit clear -> unicast (for us) */
			*unicast += 1;

		p     += len;
		avail -= len;
	}

	return n;
}

/*
 * Adaptive RX-poll cadence (shared policy; the mechanism -- a 2.0 timer or a 7.x
 * kthread sleep -- is the caller's).  Call once after each READ with the number
 * of `unicast` (to-us) frames it delivered; this updates the held-fast counter
 * and returns whether the NEXT poll should use the fast rate.  Any to-us frame
 * re-arms a full run of fast_hold fast polls; otherwise each empty poll spends
 * one held fast poll, and once they are gone we relax to the idle rate.  The
 * caller passes only the unicast count, so ambient broadcast/multicast chatter
 * is delivered but does not pin us in fast-poll.  Returns nonzero for fast.
 */
static int daynaport_poll_fast(int unicast, int *fast_left, int fast_hold)
{
	if (unicast > 0)
		*fast_left = fast_hold;
	else if (*fast_left > 0)
		*fast_left -= 1;

	return *fast_left > 0;
}

#endif /* DAYNAPORT_H */

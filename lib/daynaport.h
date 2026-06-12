/* SPDX-License-Identifier: GPL-2.0 */
/*
 * daynaport.h - DaynaPORT SCSI/Link protocol definitions and RX frame parser,
 *               shared verbatim across every kernel-version driver in this repo.
 *
 * This header is deliberately free of any kernel API (no sk_buff, net_device,
 * or stats types) and is C89-clean, so the same code compiles under gcc 2.7.x
 * for the Linux 2.0 driver and under a modern toolchain for later ports.  The
 * per-frame delivery -- which necessarily touches each era's network stack --
 * is supplied by the caller as a callback.
 *
 * Protocol reference: docs/daynaport.md.
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

/* RX framing: each record is a 2-byte big-endian length + 4-byte flag field,
 * followed by <length> bytes of frame including a trailing 4-byte Ethernet FCS. */
#define SCSILINK_RX_HDR		6	/* 2-byte length + 4-byte flag field */
#define SCSILINK_FCS_LEN	4	/* trailing Ethernet FCS, stripped */
#define SCSILINK_MINFRAME	60	/* minimum Ethernet payload (also TX pad) */
#define SCSILINK_MAXFRAME	1514	/* == ETH_FRAME_LEN, no FCS */
#define SCSILINK_RX_MAXREC	(SCSILINK_MAXFRAME + SCSILINK_FCS_LEN)

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
 *
 * Returns the number of frames delivered.
 */
static int daynaport_rx_parse(const unsigned char *buf, int buflen,
			      daynaport_deliver_fn deliver, void *ctx,
			      int *errors)
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

		p     += len;
		avail -= len;
	}

	return n;
}

#endif /* DAYNAPORT_H */

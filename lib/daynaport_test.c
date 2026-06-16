/* SPDX-License-Identifier: GPL-2.0 */
/*
 * daynaport_test.c - host-side unit tests for the shared DaynaPORT protocol
 *                    core in daynaport.h.
 *
 * Plain userspace C89: no kernel, no hardware.  These exercise the three pure,
 * kernel-API-free functions the header exposes -- CDB assembly, the RX record
 * parser, and the adaptive-poll cadence -- so a regression in that shared logic
 * (e.g. the fast/idle off-by-one the 2.0 and 7.0 drivers once disagreed on) is
 * caught by `make test` on the host, with no rig or VM in the loop.
 *
 * Build/run:  make test   (in this directory)
 */

#include <stdio.h>
#include <string.h>

#include "daynaport.h"

/* ----------------------------------------------------------------------- *
 *  Tiny assertion harness
 * ----------------------------------------------------------------------- */

static int checks;
static int failures;

#define CHECK(cond)							\
	do {								\
		checks++;						\
		if (!(cond)) {						\
			failures++;					\
			printf("FAIL %s:%d: %s\n",			\
			       __FILE__, __LINE__, #cond);		\
		}							\
	} while (0)

/* ----------------------------------------------------------------------- *
 *  RX-parse helpers: capture delivered frames, and build RX records
 * ----------------------------------------------------------------------- */

#define MAXCAP 16

struct cap {
	int		n;		/* deliver() call count */
	int		len[MAXCAP];	/* delivered length per call */
	unsigned char	first[MAXCAP];	/* first delivered byte per call */
};

static void cap_deliver(void *ctx, const unsigned char *frame, int len)
{
	struct cap *c = (struct cap *) ctx;

	if (c->n < MAXCAP) {
		c->len[c->n]   = len;
		c->first[c->n] = frame[0];
	}
	c->n++;
}

/*
 * Append one RX record at *off: 2-byte big-endian length, 4-byte (ignored) flag
 * field, then `len` payload bytes including the trailing FCS.  payload[0] = first
 * (sets the destination group bit for the unicast test); the rest is a ramp, so a
 * delivery pointer off by the header size would show up as wrong content.
 */
static void put_record(unsigned char *buf, int *off, int len, unsigned char first)
{
	int i;

	buf[(*off)++] = (unsigned char) ((len >> 8) & 0xff);
	buf[(*off)++] = (unsigned char) (len & 0xff);
	buf[(*off)++] = 0;		/* flag field - parser skips it */
	buf[(*off)++] = 0;
	buf[(*off)++] = 0;
	buf[(*off)++] = 0;
	for (i = 0; i < len; i++)
		buf[*off + i] = (unsigned char) (i == 0 ? first : (i & 0xff));
	*off += len;
}

/* ----------------------------------------------------------------------- *
 *  daynaport_cdb6
 * ----------------------------------------------------------------------- */

static void test_cdb6(void)
{
	unsigned char cdb[6];

	/* RECV: opcode in [0], big-endian length in [3..4], flag in [5];
	 * [1..2] always zero.  4096 -> 0x10,0x00. */
	daynaport_cdb6(cdb, SCSILINK_CMD_RECV, 4096, SCSILINK_RECV_FLAG);
	CHECK(cdb[0] == SCSILINK_CMD_RECV);
	CHECK(cdb[1] == 0);
	CHECK(cdb[2] == 0);
	CHECK(cdb[3] == 0x10);
	CHECK(cdb[4] == 0x00);
	CHECK(cdb[5] == SCSILINK_RECV_FLAG);

	/* STATS: 0x12 fits in [4] alone, [3] stays zero, flag 0. */
	daynaport_cdb6(cdb, SCSILINK_CMD_STATS, SCSILINK_STATS_LEN, 0);
	CHECK(cdb[0] == SCSILINK_CMD_STATS);
	CHECK(cdb[3] == 0);
	CHECK(cdb[4] == SCSILINK_STATS_LEN);
	CHECK(cdb[5] == 0);

	/* ENABLE: no data phase (len 0), flag 0x80. */
	daynaport_cdb6(cdb, SCSILINK_CMD_ENABLE, 0, SCSILINK_ENABLE_FLAG);
	CHECK(cdb[0] == SCSILINK_CMD_ENABLE);
	CHECK(cdb[3] == 0);
	CHECK(cdb[4] == 0);
	CHECK(cdb[5] == SCSILINK_ENABLE_FLAG);
}

/* ----------------------------------------------------------------------- *
 *  daynaport_rx_parse
 * ----------------------------------------------------------------------- */

static void test_parse_empty(void)
{
	unsigned char buf[128];
	struct cap c;
	int err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 0);
	CHECK(c.n == 0);
	CHECK(err == 0);
	CHECK(uc == 0);
}

static void test_parse_single_unicast(void)
{
	unsigned char buf[512];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	/* len 100 incl FCS -> 96 delivered; first byte 0x02 (even) = unicast. */
	put_record(buf, &off, 100, 0x02);

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 1);
	CHECK(c.n == 1);
	CHECK(c.len[0] == 100 - SCSILINK_FCS_LEN);
	CHECK(c.first[0] == 0x02);		/* delivery skipped the 6-byte header */
	CHECK(uc == 1);
	CHECK(err == 0);
}

static void test_parse_broadcast_not_unicast(void)
{
	unsigned char buf[512];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	/* first byte 0x01: group bit set -> delivered but NOT counted unicast. */
	put_record(buf, &off, 100, 0x01);

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 1);
	CHECK(c.n == 1);
	CHECK(uc == 0);
	CHECK(err == 0);
}

static void test_parse_multi_mixed(void)
{
	unsigned char buf[2048];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	put_record(buf, &off, 64,   0x00);	/* min size, unicast */
	put_record(buf, &off, 1518, 0xff);	/* max size, broadcast */
	put_record(buf, &off, 200,  0x10);	/* unicast */

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 3);
	CHECK(c.n == 3);
	CHECK(c.len[0] == 64   - SCSILINK_FCS_LEN);
	CHECK(c.len[1] == 1518 - SCSILINK_FCS_LEN);
	CHECK(c.len[2] == 200  - SCSILINK_FCS_LEN);
	CHECK(uc == 2);				/* 0x00 and 0x10 even; 0xff odd */
	CHECK(err == 0);
}

static void test_parse_zero_terminator_halts(void)
{
	unsigned char buf[512];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	put_record(buf, &off, 100, 0x02);
	off += SCSILINK_RX_HDR;			/* a zero-length header: stop here */
	put_record(buf, &off, 100, 0x04);	/* valid, but must NOT be parsed */

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 1);				/* only the record before the zero */
	CHECK(c.n == 1);
	CHECK(uc == 1);
	CHECK(err == 0);
}

static void test_parse_runt_after_valid(void)
{
	unsigned char buf[512];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	put_record(buf, &off, 100, 0x02);				/* valid */
	put_record(buf, &off, SCSILINK_MINFRAME + SCSILINK_FCS_LEN - 1, 0x06);
								/* 63 < 64 -> garbled */

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 1);			/* the valid record was delivered first */
	CHECK(err == 1);		/* then the runt bumped errors and stopped */
}

static void test_parse_oversize(void)
{
	unsigned char buf[2048];
	struct cap c;
	int off = 0, err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	put_record(buf, &off, SCSILINK_RX_MAXREC + 1, 0x02);	/* 1519 > 1518 */

	n = daynaport_rx_parse(buf, (int) sizeof(buf), cap_deliver, &c, &err, &uc);
	CHECK(n == 0);
	CHECK(err == 1);
	CHECK(c.n == 0);
}

static void test_parse_truncated_tail(void)
{
	unsigned char buf[256];
	struct cap c;
	int err = 0, uc = 0, n;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));

	/* Header claims a 200-byte record, but only 100 bytes follow it.  That is
	 * a clean short tail (a batch split across READs), NOT a garbled record:
	 * the walk stops with no error and no delivery. */
	buf[0] = 0;
	buf[1] = 200;
	n = daynaport_rx_parse(buf, SCSILINK_RX_HDR + 100, cap_deliver, &c, &err, &uc);
	CHECK(n == 0);
	CHECK(err == 0);
	CHECK(c.n == 0);
}

/* ----------------------------------------------------------------------- *
 *  daynaport_poll_fast
 * ----------------------------------------------------------------------- */

static void test_poll_fast_hold_sequence(void)
{
	int fast_left = 0;
	int hold = 3;
	int fast_count = 0;
	int r, i;

	/* A to-us frame arms a full hold and asks for a fast next poll. */
	r = daynaport_poll_fast(1, &fast_left, hold);
	CHECK(fast_left == hold);
	CHECK(r == 1);
	if (r)
		fast_count++;

	/* Empty polls stay fast until the hold drains, then relax to idle. */
	for (i = 0; i < hold + 5; i++) {
		r = daynaport_poll_fast(0, &fast_left, hold);
		if (!r)
			break;
		fast_count++;
	}

	/* Exactly `hold` fast polls follow the activity -- the documented
	 * semantics (the 2.0 driver once held one extra; see daynaport.h). */
	CHECK(fast_count == hold);
	CHECK(fast_left == 0);
}

static void test_poll_fast_rearm(void)
{
	int fast_left = 0;
	int hold = 4;

	daynaport_poll_fast(1, &fast_left, hold);		/* fast_left = 4 */
	daynaport_poll_fast(0, &fast_left, hold);		/* fast_left = 3 */
	CHECK(daynaport_poll_fast(5, &fast_left, hold) == 1);	/* re-arms fully */
	CHECK(fast_left == hold);
}

static void test_poll_fast_zero_hold(void)
{
	int fast_left = 0;

	/* hold 0: activity arms to 0, so the next poll is already idle. */
	CHECK(daynaport_poll_fast(1, &fast_left, 0) == 0);
	CHECK(fast_left == 0);
}

static void test_poll_fast_idle_no_underflow(void)
{
	int fast_left = 0;

	/* Already idle: an empty poll must not drive the counter negative. */
	CHECK(daynaport_poll_fast(0, &fast_left, 8) == 0);
	CHECK(fast_left == 0);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
	test_cdb6();

	test_parse_empty();
	test_parse_single_unicast();
	test_parse_broadcast_not_unicast();
	test_parse_multi_mixed();
	test_parse_zero_terminator_halts();
	test_parse_runt_after_valid();
	test_parse_oversize();
	test_parse_truncated_tail();

	test_poll_fast_hold_sequence();
	test_poll_fast_rearm();
	test_poll_fast_zero_hold();
	test_poll_fast_idle_no_underflow();

	printf("%d checks, %d failure%s\n",
	       checks, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

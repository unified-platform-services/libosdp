/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <osdp.h>
#include "test.h"
#include "osdp_multipart.h"

static int test_mp_hdr_roundtrip_w16(void)
{
	uint8_t buf[OSDP_MP_HDR_SIZE(OSDP_MP_W16)];
	uint32_t total = 0, offset = 0;
	uint16_t data_len = 0;
	int n;

	n = osdp_mp_hdr_write(OSDP_MP_W16, 0x1234, 0x0056, 0x0040, buf);
	if (n != 6) {
		printf(SUB_1 "w16 hdr write len %d != 6\n", n);
		return -1;
	}
	n = osdp_mp_hdr_read(OSDP_MP_W16, buf, sizeof(buf), &total, &offset,
			     &data_len);
	if (n != 6 || total != 0x1234 || offset != 0x0056 ||
	    data_len != 0x0040) {
		printf(SUB_1 "w16 rt total:%x off:%x dlen:%x\n", total, offset,
		       data_len);
		return -1;
	}
	return 0;
}

static int test_mp_hdr_roundtrip_w32(void)
{
	uint8_t buf[OSDP_MP_HDR_SIZE(OSDP_MP_W32)];
	uint32_t total = 0, offset = 0;
	uint16_t data_len = 0;
	int n;

	n = osdp_mp_hdr_write(OSDP_MP_W32, 0x00ABCDEF, 0x00012345, 0x0100, buf);
	if (n != 10) {
		printf(SUB_1 "w32 hdr write len %d != 10\n", n);
		return -1;
	}
	n = osdp_mp_hdr_read(OSDP_MP_W32, buf, sizeof(buf), &total, &offset,
			     &data_len);
	if (n != 10 || total != 0x00ABCDEF || offset != 0x00012345 ||
	    data_len != 0x0100) {
		printf(SUB_1 "w32 rt total:%x off:%x dlen:%x\n", total, offset,
		       data_len);
		return -1;
	}
	return 0;
}

static int test_mp_hdr_read_short(void)
{
	uint8_t buf[4] = { 0 };

	if (osdp_mp_hdr_read(OSDP_MP_W32, buf, 4, NULL, NULL, NULL) != -1) {
		return -1; /* must reject: 4 < 10 */
	}
	return 0;
}

/* Build all frames of a buffer-backed transfer into `out`, committing each,
 * until DONE. Returns total bytes emitted, or -1. */
static int drive_tx_to_buffer(struct osdp_multipart *mp, uint8_t *out,
			      int per_frame_max)
{
	int total = 0, n;

	while (osdp_mp_is_active(mp)) {
		n = osdp_mp_tx_build(mp, out + total, per_frame_max);
		if (n < 0) {
			return -1;
		}
		total += n;
		osdp_mp_tx_commit(mp);
	}
	return total;
}

static int test_mp_tx_single_frame(void)
{
	struct osdp_multipart mp;
	uint8_t src[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t out[64];
	int n;

	osdp_mp_reset(&mp);
	osdp_mp_bind_buffer(&mp, src, sizeof(src));
	if (osdp_mp_tx_init(&mp, sizeof(src), OSDP_MP_W16) != 0) {
		return -1;
	}
	/* Big packet: whole payload fits one frame. */
	n = drive_tx_to_buffer(&mp, out, 64);
	/* [total=4][off=0][dlen=4][DE AD BE EF] = 6 + 4 = 10 */
	if (n != 10 || out[0] != 4 || out[2] != 0 || out[4] != 4 ||
	    memcmp(out + 6, src, 4) != 0) {
		printf(SUB_1 "tx single frame n=%d\n", n);
		return -1;
	}
	if (mp.state != OSDP_MP_DONE) {
		return -1;
	}
	return 0;
}

static int test_mp_tx_multi_frame(void)
{
	struct osdp_multipart mp;
	uint8_t src[10];
	uint8_t out[128];
	int n, i;

	for (i = 0; i < 10; i++) {
		src[i] = (uint8_t)i;
	}
	osdp_mp_reset(&mp);
	osdp_mp_bind_buffer(&mp, src, sizeof(src));
	osdp_mp_tx_init(&mp, sizeof(src), OSDP_MP_W16);
	/* per_frame_max = hdr(6) + 4 chunk bytes -> 3 frames: 4 + 4 + 2 */
	n = drive_tx_to_buffer(&mp, out, 6 + 4);
	/* frame0: total10 off0 dlen4 | frame1: off4 dlen4 | frame2: off8 dlen2 */
	if (n != (6 + 4) + (6 + 4) + (6 + 2)) {
		printf(SUB_1 "tx multi frame n=%d\n", n);
		return -1;
	}
	/* second frame's OFFSET field (LSB at out[10+2]) must be 4 */
	if (out[10 + 2] != 4) {
		printf(SUB_1 "tx frame1 offset=%d\n", out[10 + 2]);
		return -1;
	}
	if (mp.state != OSDP_MP_DONE) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_roundtrip(void)
{
	struct osdp_multipart tx, rx;
	uint8_t src[10], dst[10], wire[128];
	int i, wlen;
	enum osdp_mp_rc rc = OSDP_MP_RC_MORE;
	int pos = 0;

	for (i = 0; i < 10; i++) {
		src[i] = (uint8_t)(0xA0 + i);
	}
	/* Produce a 3-frame wire stream (4 + 4 + 2). */
	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	wlen = drive_tx_to_buffer(&tx, wire, 6 + 4);

	/* Reassemble it. */
	memset(dst, 0, sizeof(dst));
	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	while (pos < wlen) {
		uint32_t total, off;
		uint16_t dlen;
		int flen;

		osdp_mp_hdr_read(OSDP_MP_W16, wire + pos, wlen - pos, &total,
				 &off, &dlen);
		flen = OSDP_MP_HDR_SIZE(OSDP_MP_W16) + dlen;
		rc = osdp_mp_rx_consume(&rx, wire + pos, flen);
		if (rc == OSDP_MP_RC_ERR) {
			return -1;
		}
		osdp_mp_rx_commit(&rx);
		pos += flen;
	}
	if (rc != OSDP_MP_RC_DONE || memcmp(src, dst, sizeof(src)) != 0) {
		printf(SUB_1 "rx roundtrip rc=%d\n", rc);
		return -1;
	}
	return 0;
}

static int test_mp_rx_reject_gap(void)
{
	struct osdp_multipart rx;
	uint8_t dst[10], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	/* First frame declares offset 4 (should be 0) -> forward gap -> ERR. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 4, 2, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_reject_oversize(void)
{
	struct osdp_multipart rx;
	uint8_t dst[4], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst)); /* cap 4 */
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	/* total 10 > cap 4 -> ERR. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 0, 2, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_early_terminate(void)
{
	struct osdp_multipart rx;
	uint8_t dst[10], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	/* offset(10) >= total(10), dlen 0 -> EARLY_TERM. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 10, 0, frame);
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_EARLY_TERM) {
		return -1;
	}
	return 0;
}

struct mp_stream_src {
	const uint8_t *data;
	uint32_t len;
	int busy_first_n; /* return 0 for the first N read attempts */
	int reads;
};

static int stream_read(void *arg, void *buf, uint32_t size, uint32_t offset)
{
	struct mp_stream_src *s = arg;

	s->reads++;
	if (s->busy_first_n > 0) {
		s->busy_first_n--;
		return 0; /* source temporarily empty */
	}
	if (offset >= s->len) {
		return 0;
	}
	if (offset + size > s->len) {
		size = s->len - offset;
	}
	memcpy(buf, s->data + offset, size);
	return (int)size;
}

struct mp_stream_dst {
	uint8_t data[64];
	uint32_t len;
};

static int stream_write(void *arg, const void *buf, uint32_t size,
			uint32_t offset)
{
	struct mp_stream_dst *d = arg;

	if (offset + size > sizeof(d->data)) {
		return -1;
	}
	memcpy(d->data + offset, buf, size);
	if (offset + size > d->len) {
		d->len = offset + size;
	}
	return (int)size;
}

static int test_mp_stream_roundtrip(void)
{
	static const uint8_t payload[20] = { 1,	 2,  3,	 4,  5,	 6,  7,
					     8,	 9,  10, 11, 12, 13, 14,
					     15, 16, 17, 18, 19, 20 };
	struct mp_stream_src src = { .data = payload, .len = sizeof(payload) };
	struct mp_stream_dst dst = { 0 };
	struct osdp_mp_ops tx_ops = { .read = stream_read, .arg = &src };
	struct osdp_mp_ops rx_ops = { .write = stream_write, .arg = &dst };
	struct osdp_multipart tx, rx;
	uint8_t frame[32];
	enum osdp_mp_rc rc = OSDP_MP_RC_MORE;
	int n;

	osdp_mp_reset(&tx);
	osdp_mp_bind_ops(&tx, &tx_ops);
	osdp_mp_tx_init(&tx, sizeof(payload), OSDP_MP_W32);

	osdp_mp_reset(&rx);
	osdp_mp_bind_ops(&rx, &rx_ops);
	osdp_mp_rx_init(&rx, OSDP_MP_W32);

	/* Pipe each TX frame straight into RX; 8 chunk bytes per frame. */
	while (osdp_mp_is_active(&tx)) {
		n = osdp_mp_tx_build(&tx, frame,
				     OSDP_MP_HDR_SIZE(OSDP_MP_W32) + 8);
		if (n < 0) {
			return -1;
		}
		osdp_mp_tx_commit(&tx);
		rc = osdp_mp_rx_consume(&rx, frame, n);
		if (rc == OSDP_MP_RC_ERR) {
			return -1;
		}
		osdp_mp_rx_commit(&rx);
	}
	if (rc != OSDP_MP_RC_DONE || dst.len != sizeof(payload) ||
	    memcmp(dst.data, payload, sizeof(payload)) != 0) {
		printf(SUB_1 "stream rt rc=%d len=%u\n", rc, dst.len);
		return -1;
	}
	return 0;
}

static int test_mp_tx_keepalive_on_busy(void)
{
	static const uint8_t payload[4] = { 9, 8, 7, 6 };
	struct mp_stream_src src = { .data = payload,
				     .len = sizeof(payload),
				     .busy_first_n = 1 };
	struct osdp_mp_ops tx_ops = { .read = stream_read, .arg = &src };
	struct osdp_multipart tx;
	uint8_t frame[32];
	int n;

	osdp_mp_reset(&tx);
	osdp_mp_bind_ops(&tx, &tx_ops);
	osdp_mp_tx_init(&tx, sizeof(payload), OSDP_MP_W32);

	/* First build: source busy -> header-only keep-alive, state WAIT. */
	n = osdp_mp_tx_build(&tx, frame, sizeof(frame));
	if (n != OSDP_MP_HDR_SIZE(OSDP_MP_W32) || tx.state != OSDP_MP_WAIT ||
	    tx.last_len != 0) {
		printf(SUB_1 "keepalive frame n=%d state=%d\n", n, tx.state);
		return -1;
	}
	osdp_mp_tx_commit(&tx); /* no-op: offset unchanged */
	if (tx.offset != 0) {
		return -1;
	}
	/* Second build: source recovered -> real data, back to INPROG. */
	n = osdp_mp_tx_build(&tx, frame, sizeof(frame));
	if (n != OSDP_MP_HDR_SIZE(OSDP_MP_W32) + 4 ||
	    tx.state != OSDP_MP_INPROG) {
		printf(SUB_1 "recovered frame n=%d state=%d\n", n, tx.state);
		return -1;
	}
	return 0;
}

struct mp_done_probe {
	int calls;
	enum osdp_mp_rc last;
};

static void on_done_probe(void *arg, enum osdp_mp_rc outcome)
{
	struct mp_done_probe *p = arg;

	p->calls++;
	p->last = outcome;
}

static int test_mp_done_cb_on_complete(void)
{
	struct osdp_multipart tx;
	struct mp_done_probe probe = { 0 };
	uint8_t src[4] = { 1, 2, 3, 4 };
	uint8_t out[64];

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_done_cb(&tx, on_done_probe, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	(void)drive_tx_to_buffer(&tx, out, 64);
	if (probe.calls != 1 || probe.last != OSDP_MP_RC_DONE) {
		printf(SUB_1 "done cb calls=%d last=%d\n", probe.calls,
		       probe.last);
		return -1;
	}
	return 0;
}

static int test_mp_abort_fires_cb(void)
{
	struct osdp_multipart tx;
	struct mp_done_probe probe = { 0 };
	uint8_t src[10] = { 0 };

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_done_cb(&tx, on_done_probe, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	osdp_mp_abort(&tx);
	if (osdp_mp_is_active(&tx) || probe.calls != 1 ||
	    probe.last != OSDP_MP_RC_ERR) {
		return -1;
	}
	/* Second abort is a no-op (not active). */
	osdp_mp_abort(&tx);
	if (probe.calls != 1) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_reject_differing_total(void)
{
	struct osdp_multipart rx;
	uint8_t dst[20], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst));
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	/* frame 0: total=8, off=0, dlen=4 -> MORE */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 8, 0, 4, frame);
	frame[flen++] = 1;
	frame[flen++] = 2;
	frame[flen++] = 3;
	frame[flen++] = 4;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_MORE) {
		return -1;
	}
	osdp_mp_rx_commit(&rx);
	/* frame 1: total=9 (changed!) -> ERR */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 9, 4, 4, frame);
	frame[flen++] = 5;
	frame[flen++] = 6;
	frame[flen++] = 7;
	frame[flen++] = 8;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	return 0;
}

static int test_mp_rx_first_frame_error_keeps_total_clean(void)
{
	struct osdp_multipart rx;
	uint8_t dst[4], frame[16];
	int flen;

	osdp_mp_reset(&rx);
	osdp_mp_bind_buffer(&rx, dst, sizeof(dst)); /* cap 4 */
	osdp_mp_rx_init(&rx, OSDP_MP_W16);
	/* First frame declares total 10 > cap 4 -> ERR; mp->total must stay 0
	 * so it is not poisoned for a subsequent corrected first frame. */
	flen = osdp_mp_hdr_write(OSDP_MP_W16, 10, 0, 2, frame);
	frame[flen++] = 0x11;
	frame[flen++] = 0x22;
	if (osdp_mp_rx_consume(&rx, frame, flen) != OSDP_MP_RC_ERR) {
		return -1;
	}
	if (rx.total != 0) {
		printf(SUB_1 "rx total poisoned: %u\n", rx.total);
		return -1;
	}
	return 0;
}

static int test_mp_tx_next_action(void)
{
	struct osdp_multipart tx;
	uint8_t src[10] = { 0 };

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_DATA) {
		return -1;
	}
	osdp_mp_set_wait(&tx, 100000); /* 100s window; will not elapse here */
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_WAIT) {
		return -1;
	}
	osdp_mp_abort(&tx);
	if (osdp_mp_tx_next(&tx) != OSDP_MP_ACT_DONE) {
		return -1;
	}
	return 0;
}

static int test_mp_finish_idempotent(void)
{
	struct osdp_multipart tx;
	struct mp_done_probe probe = { 0 };
	uint8_t src[4] = { 1, 2, 3, 4 };
	uint8_t out[64];

	osdp_mp_reset(&tx);
	osdp_mp_bind_buffer(&tx, src, sizeof(src));
	osdp_mp_set_done_cb(&tx, on_done_probe, &probe);
	osdp_mp_tx_init(&tx, sizeof(src), OSDP_MP_W16);
	(void)drive_tx_to_buffer(&tx, out, 64);
	if (probe.calls != 1 || probe.last != OSDP_MP_RC_DONE) {
		return -1;
	}
	/* A second commit re-enters mp_finish (offset already >= total);
	 * only the internal guard prevents a second callback fire. */
	osdp_mp_tx_commit(&tx);
	if (probe.calls != 1 || tx.state != OSDP_MP_DONE) {
		printf(SUB_1 "finish not idempotent calls=%d state=%d\n",
		       probe.calls, tx.state);
		return -1;
	}
	return 0;
}

void run_multipart_tests(struct test *t)
{
	bool result = true;

	printf("\nBegin Multipart Engine Tests\n");

	result &= (test_mp_hdr_roundtrip_w16() == 0);
	result &= (test_mp_hdr_roundtrip_w32() == 0);
	result &= (test_mp_hdr_read_short() == 0);
	result &= (test_mp_tx_single_frame() == 0);
	result &= (test_mp_tx_multi_frame() == 0);
	result &= (test_mp_rx_roundtrip() == 0);
	result &= (test_mp_rx_reject_gap() == 0);
	result &= (test_mp_rx_reject_oversize() == 0);
	result &= (test_mp_rx_early_terminate() == 0);
	result &= (test_mp_rx_reject_differing_total() == 0);
	result &= (test_mp_rx_first_frame_error_keeps_total_clean() == 0);
	result &= (test_mp_stream_roundtrip() == 0);
	result &= (test_mp_tx_keepalive_on_busy() == 0);
	result &= (test_mp_done_cb_on_complete() == 0);
	result &= (test_mp_abort_fires_cb() == 0);
	result &= (test_mp_tx_next_action() == 0);
	result &= (test_mp_finish_idempotent() == 0);

	printf(SUB_1 "Multipart engine tests %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

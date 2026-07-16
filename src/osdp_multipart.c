/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "osdp_multipart.h"

#include <string.h>

/* Fill a progress record from engine state + relayed identity and fire the
 * consumer callback for `phase`. No-op when no callback is registered. */
static void mp_emit(struct osdp_multipart *mp, enum osdp_mp_phase phase)
{
	struct osdp_mp_progress p;

	if (!mp->event_cb) {
		return;
	}
	p.msg_type = mp->msg_type;
	p.object_id = mp->object_id;
	p.total = mp->total;
	p.offset = mp->offset;
	p.outcome = mp->done_outcome;
	mp->event_cb(mp->event_arg, phase, &p);
}

static void mp_write_wide(enum osdp_mp_width w, uint32_t val, uint8_t *buf,
			  int *len)
{
	if (w == OSDP_MP_W32) {
		bwrite_u32_le(val, buf, len);
	} else {
		bwrite_u16_le((uint16_t)val, buf, len);
	}
}

static uint32_t mp_read_wide(enum osdp_mp_width w, const uint8_t *buf, int *pos)
{
	if (w == OSDP_MP_W32) {
		return bread_u32_le(buf, pos);
	}
	return bread_u16_le(buf, pos);
}

int osdp_mp_hdr_write(enum osdp_mp_width w, uint32_t total, uint32_t offset,
		      uint16_t data_len, uint8_t *buf)
{
	int len = 0;

	mp_write_wide(w, total, buf, &len);
	mp_write_wide(w, offset, buf, &len);
	bwrite_u16_le(data_len, buf, &len);
	return len;
}

int osdp_mp_hdr_read(enum osdp_mp_width w, const uint8_t *buf, int len,
		     uint32_t *total, uint32_t *offset, uint16_t *data_len)
{
	int pos = 0;

	if (len < OSDP_MP_HDR_SIZE(w)) {
		return -1;
	}
	*total = mp_read_wide(w, buf, &pos);
	*offset = mp_read_wide(w, buf, &pos);
	*data_len = bread_u16_le(buf, &pos);
	return pos;
}

/* Internal memcpy data plane used when the engine is buffer-bound. */
static int mp_buf_read(void *arg, void *dst, uint32_t size, uint32_t offset)
{
	struct osdp_multipart *mp = arg;

	if (offset >= mp->buf_len) {
		return 0;
	}
	/* offset < buf_len here, so buf_len - offset never underflows; this
	 * form also avoids an offset+size overflow for wire-controlled sizes. */
	if (size > mp->buf_len - offset) {
		size = mp->buf_len - offset;
	}
	memcpy(dst, mp->buf + offset, size);
	return (int)size;
}

static int mp_buf_write(void *arg, const void *src, uint32_t size,
			uint32_t offset)
{
	struct osdp_multipart *mp = arg;

	if (offset + size > mp->buf_len) {
		return OSDP_MP_RX_ABORT;
	}
	memcpy(mp->buf + offset, src, size);
	return OSDP_MP_RX_OK;
}

void osdp_mp_reset(struct osdp_multipart *mp)
{
	memset(mp, 0, sizeof(*mp));
	mp->state = OSDP_MP_IDLE;
}

void osdp_mp_bind_ops(struct osdp_multipart *mp, const struct osdp_mp_ops *ops)
{
	mp->ops = *ops;
	mp->buf = NULL;
	mp->buf_len = 0;
}

void osdp_mp_bind_buffer(struct osdp_multipart *mp, uint8_t *buf, uint32_t len)
{
	mp->buf = buf;
	mp->buf_len = len;
	mp->ops.read = mp_buf_read;
	mp->ops.write = mp_buf_write;
	mp->ops.arg = mp;
}

bool osdp_mp_is_active(const struct osdp_multipart *mp)
{
	return mp->state == OSDP_MP_INPROG || mp->state == OSDP_MP_WAIT;
}

int osdp_mp_tx_init(struct osdp_multipart *mp, uint32_t total,
		    enum osdp_mp_width w)
{
	if (total == 0) {
		return -1;
	}
	mp->width = w;
	mp->total = total;
	mp->offset = 0;
	mp->last_len = 0;
	mp->errors = 0;
	mp->start_emitted = false;
	mp->state = OSDP_MP_INPROG;
	return 0;
}

int osdp_mp_tx_build(struct osdp_multipart *mp, uint8_t *buf, int max_len)
{
	int hdr = OSDP_MP_HDR_SIZE(mp->width);
	int avail = max_len - hdr;
	uint32_t remaining = mp->total - mp->offset;
	int n;

	if (avail < 0) {
		return -1;
	}
	if ((uint32_t)avail > remaining) {
		avail = (int)remaining;
	}

	n = mp->ops.read(mp->ops.arg, buf + hdr, (uint32_t)avail, mp->offset);
	if (n < 0) {
		return -1;
	}
	if (n == 0) {
		/* Source temporarily empty: header-only keep-alive, park in
		 * WAIT. Offset does not advance; commit is a no-op. */
		mp->last_len = 0;
		mp->state = OSDP_MP_WAIT;
		osdp_mp_hdr_write(mp->width, mp->total, mp->offset, 0, buf);
		return hdr;
	}
	mp->last_len = n;
	mp->state = OSDP_MP_INPROG;
	osdp_mp_hdr_write(mp->width, mp->total, mp->offset, (uint16_t)n, buf);
	return hdr + n;
}

int osdp_mp_tx_build_idle(struct osdp_multipart *mp, uint8_t *buf)
{
	mp->last_len = 0; /* commit stays a no-op */
	return osdp_mp_hdr_write(mp->width, mp->total, mp->offset, 0, buf);
}

void osdp_mp_tx_commit(struct osdp_multipart *mp)
{
	bool advanced = mp->last_len > 0;

	mp->offset += (uint32_t)mp->last_len;
	mp->last_len = 0;
	if (advanced && mp->offset < mp->total) {
		mp_emit(mp, OSDP_MP_PHASE_PROGRESS);
	}
}

int osdp_mp_rx_init(struct osdp_multipart *mp, enum osdp_mp_width w,
		    uint32_t total)
{
	mp->width = w;
	mp->total = total;
	mp->offset = 0;
	mp->last_len = 0;
	mp->last_off = 0;
	mp->errors = 0;
	mp->start_emitted = false;
	mp->state = OSDP_MP_INPROG;
	return 0;
}

enum osdp_mp_rc osdp_mp_rx_consume(struct osdp_multipart *mp,
				   const uint8_t *buf, int len)
{
	int hdr = OSDP_MP_HDR_SIZE(mp->width);
	uint32_t total, off, cur_total;
	uint16_t dlen;

	if (osdp_mp_hdr_read(mp->width, buf, len, &total, &off, &dlen) < 0) {
		return OSDP_MP_RC_ERR;
	}
	if (len - hdr < (int)dlen) {
		return OSDP_MP_RC_ERR;
	}

	/* mp->total == 0 means the length is not yet fixed (rx_init got no
	 * hint and no frame has committed): the first frame fixes it. Once
	 * fixed — by rx_init or by a prior frame — every frame must agree.
	 * Validate against a LOCAL total and only commit mp->total once every
	 * ERR check has passed, so a rejected first frame does not poison
	 * mp->total for the next attempt. */
	if (mp->offset == 0 && mp->last_len == 0 && mp->total == 0) {
		cur_total = total;
	} else {
		if (total != mp->total) {
			return OSDP_MP_RC_ERR;
		}
		cur_total = mp->total;
	}

	if (mp->buf != NULL && cur_total > mp->buf_len) {
		return OSDP_MP_RC_ERR;
	}

	/* Early termination signalled by the sender. */
	if (off >= cur_total && dlen == 0) {
		mp->total = cur_total;
		return OSDP_MP_RC_EARLY_TERM;
	}

	/* No forward gaps; retransmit (off <= committed) is tolerated. */
	if (off > mp->offset) {
		return OSDP_MP_RC_ERR;
	}
	if (dlen > cur_total - off) {
		return OSDP_MP_RC_ERR;
	}

	if (dlen > 0) {
		int st = mp->ops.write(mp->ops.arg, buf + hdr, dlen, off);
		if (st == OSDP_MP_RX_RETRY) {
			return OSDP_MP_RC_RETRY; /* offset/last_len unchanged */
		}
		if (st != OSDP_MP_RX_OK) {
			return OSDP_MP_RC_ERR;   /* ABORT or unexpected */
		}
	}
	mp->total = cur_total; /* all ERR checks passed: commit total */
	mp->last_off = off;
	mp->last_len = (int)dlen;

	if (dlen >= mp->total - off) {
		return OSDP_MP_RC_DONE;
	}
	return OSDP_MP_RC_MORE;
}

void osdp_mp_rx_commit(struct osdp_multipart *mp)
{
	uint32_t end = mp->last_off + (uint32_t)mp->last_len;
	bool advanced = mp->last_len > 0 && end > mp->offset;

	if (end > mp->offset) {
		mp->offset = end;
	}
	mp->last_len = 0;
	if (advanced && mp->offset < mp->total) {
		mp_emit(mp, OSDP_MP_PHASE_PROGRESS);
	}
}

void osdp_mp_set_event_cb(struct osdp_multipart *mp, osdp_mp_event_fn fn,
			  void *arg)
{
	mp->event_cb = fn;
	mp->event_arg = arg;
}

void osdp_mp_set_identity(struct osdp_multipart *mp, int msg_type,
			  int object_id)
{
	mp->msg_type = msg_type;
	mp->object_id = object_id;
}

void osdp_mp_set_wait(struct osdp_multipart *mp, uint32_t ms)
{
	mp->wait_time_ms = ms;
	mp->tstamp = osdp_millis_now();
}

void osdp_mp_park(struct osdp_multipart *mp)
{
	mp->state = OSDP_MP_WAIT;
}

void osdp_mp_emit_start(struct osdp_multipart *mp)
{
	if (!mp->start_emitted) {
		mp->start_emitted = true;
		mp_emit(mp, OSDP_MP_PHASE_START);
	}
}

/* Consumer-driven terminal. Idempotent: fires DONE(outcome) exactly once for a
 * transfer that actually started. Reaching offset==total is NOT terminal on its
 * own (sender keep-alive parks at EOF), so only this call ends the transfer. */
void osdp_mp_finish(struct osdp_multipart *mp, int outcome)
{
	if (mp->state == OSDP_MP_DONE || mp->state == OSDP_MP_IDLE) {
		return;
	}
	mp->done_outcome = outcome;
	mp->state = OSDP_MP_DONE;
	mp_emit(mp, OSDP_MP_PHASE_DONE);
}

enum osdp_mp_action osdp_mp_tx_next(struct osdp_multipart *mp)
{
	if (!osdp_mp_is_active(mp)) {
		return OSDP_MP_ACT_DONE;
	}
	if (mp->wait_time_ms &&
	    osdp_millis_since(mp->tstamp) < mp->wait_time_ms) {
		return OSDP_MP_ACT_WAIT;
	}
	return OSDP_MP_ACT_DATA;
}

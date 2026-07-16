/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_MULTIPART_H_
#define _OSDP_MULTIPART_H_

#include "osdp_common.h"

/* Width of the TOTAL/OFFSET fields. DATA_LEN is always 2 bytes.
 * W16 == OSDP 5.10 / OSDP_MULTI_HDR_IEC; W32 == file transfer. */
enum osdp_mp_width {
	OSDP_MP_W16 = 2,
	OSDP_MP_W32 = 4,
};

enum osdp_mp_state {
	OSDP_MP_IDLE,
	OSDP_MP_INPROG,
	OSDP_MP_WAIT,
	OSDP_MP_DONE,
};

enum osdp_mp_rc {
	OSDP_MP_RC_MORE = 0,
	OSDP_MP_RC_DONE,
	OSDP_MP_RC_EARLY_TERM,
	OSDP_MP_RC_ERR,
};

enum osdp_mp_action {
	OSDP_MP_ACT_DATA, /* build/send the next fragment now */
	OSDP_MP_ACT_WAIT, /* throttled; do not send yet       */
	OSDP_MP_ACT_DONE, /* transfer finished                */
	OSDP_MP_ACT_ABORT, /* unrecoverable; tear down         */
};

/* Offset-addressed data plane. Signatures mirror struct osdp_file_ops so a
 * file transfer binds its public ops directly (no wrapper). */
struct osdp_mp_ops {
	int (*read)(void *arg, void *buf, uint32_t size, uint32_t offset);
	int (*write)(void *arg, const void *buf, uint32_t size,
		     uint32_t offset);
	void *arg;
};

typedef void (*osdp_mp_done_fn)(void *arg, enum osdp_mp_rc outcome);

struct osdp_multipart {
	enum osdp_mp_state state;
	enum osdp_mp_width width;
	uint32_t total; /* full payload length                 */
	uint32_t offset; /* committed offset (next to send/expect) */
	int last_len; /* bytes moved by the last un-committed frame */
	uint32_t last_off; /* declared offset of the last un-committed frame */
	struct osdp_mp_ops ops;
	uint8_t *buf; /* non-NULL when buffer-bound          */
	uint32_t buf_len;
	osdp_mp_done_fn on_done;
	void *on_done_arg;
	uint32_t wait_time_ms; /* sender throttle (set by consumer)   */
	tick_t tstamp;
	int errors;
};

/* Header size for a given width: TOTAL(w) + OFFSET(w) + DATA_LEN(2). */
#define OSDP_MP_HDR_SIZE(w) (2 * (int)(w) + 2)

/* --- Header codec --- */
int osdp_mp_hdr_write(enum osdp_mp_width w, uint32_t total, uint32_t offset,
		      uint16_t data_len, uint8_t *buf);
int osdp_mp_hdr_read(enum osdp_mp_width w, const uint8_t *buf, int len,
		     uint32_t *total, uint32_t *offset, uint16_t *data_len);

/* --- Lifecycle / binding --- */
void osdp_mp_reset(struct osdp_multipart *mp);
void osdp_mp_bind_ops(struct osdp_multipart *mp, const struct osdp_mp_ops *ops);
void osdp_mp_bind_buffer(struct osdp_multipart *mp, uint8_t *buf, uint32_t len);
bool osdp_mp_is_active(const struct osdp_multipart *mp);

/* --- TX --- */
int osdp_mp_tx_init(struct osdp_multipart *mp, uint32_t total,
		    enum osdp_mp_width w);
int osdp_mp_tx_build(struct osdp_multipart *mp, uint8_t *buf, int max_len);
void osdp_mp_tx_commit(struct osdp_multipart *mp);

/* --- RX --- */
int osdp_mp_rx_init(struct osdp_multipart *mp, enum osdp_mp_width w);
enum osdp_mp_rc osdp_mp_rx_consume(struct osdp_multipart *mp,
				   const uint8_t *buf, int len);
void osdp_mp_rx_commit(struct osdp_multipart *mp);

/* --- Control / scheduling --- */
void osdp_mp_set_done_cb(struct osdp_multipart *mp, osdp_mp_done_fn fn,
			 void *arg);
void osdp_mp_set_wait(struct osdp_multipart *mp, uint32_t ms);
void osdp_mp_abort(struct osdp_multipart *mp);
enum osdp_mp_action osdp_mp_tx_next(struct osdp_multipart *mp);

#endif /* _OSDP_MULTIPART_H_ */

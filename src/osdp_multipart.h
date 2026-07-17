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
	/* Header-only frame with offset >= total. The engine only classifies;
	 * meaning is family-specific and the consumer must decide: W16
	 * multi-part receivers shall treat it as early termination of the
	 * transfer (OSDP 2.2 §5.10.2), while FILETRANSFER receivers treat it
	 * as the CP's idle keep-alive sent after FTSTAT status 3 (§7.25). */
	OSDP_MP_RC_EARLY_TERM,
	OSDP_MP_RC_ERR,
	OSDP_MP_RC_RETRY,   /* receiver transient failure; do not advance */
};

enum osdp_mp_action {
	OSDP_MP_ACT_DATA, /* build/send the next fragment now */
	OSDP_MP_ACT_WAIT, /* throttled; do not send yet       */
	OSDP_MP_ACT_DONE, /* transfer finished                */
	OSDP_MP_ACT_ABORT, /* unrecoverable; tear down         */
};

/* Receiver data-plane return convention (engine-internal). A bound write hook
 * returns one of these; it does NOT return a byte count. */
enum osdp_mp_rx_status {
	OSDP_MP_RX_OK    =  0,  /* chunk consumed; advance */
	OSDP_MP_RX_RETRY = -1,  /* transient; keep offset, receiver re-obtains chunk */
	OSDP_MP_RX_ABORT = -2,  /* fatal; tear the transfer down */
};

/* Offset-addressed data plane. Signatures mirror struct osdp_file_ops; file
 * transfer binds through a thin adapter that only bridges write's return
 * convention. read returns a byte count (0 = temporarily empty); write returns
 * an enum osdp_mp_rx_status. */
struct osdp_mp_ops {
	int (*read)(void *arg, void *buf, uint32_t size, uint32_t offset);
	int (*write)(void *arg, const void *buf, uint32_t size,
		     uint32_t offset);
	void *arg;
};

/* Lifecycle phases reported to the consumer's event callback. */
enum osdp_mp_phase {
	OSDP_MP_PHASE_START,     /* transfer opened */
	OSDP_MP_PHASE_PROGRESS,  /* a data fragment just committed */
	OSDP_MP_PHASE_DONE,      /* terminal; outcome is meaningful */
};

/* Progress record handed to the callback. msg_type/object_id are opaque relay
 * values set via osdp_mp_set_identity(); the engine never interprets them. */
struct osdp_mp_progress {
	int msg_type;
	int object_id;
	uint32_t total;
	uint32_t offset;
	int outcome;   /* meaningful at DONE */
};

typedef void (*osdp_mp_event_fn)(void *arg, enum osdp_mp_phase phase,
				 const struct osdp_mp_progress *p);

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
	osdp_mp_event_fn event_cb;
	void *event_arg;
	int msg_type;      /* opaque relay: multipart family */
	int object_id;     /* opaque relay: consumer id */
	int done_outcome;  /* relayed at DONE */
	bool start_emitted; /* START fires at most once per transfer */
	uint32_t wait_time_ms; /* sender throttle (set by consumer)   */
	tick_t tstamp;
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
/* Like tx_build, but a consumer-owned prefix of pfx_len bytes rides between
 * the header and the data on the offset-0 fragment only — the GENAUTH/CRAUTH
 * Algorithm/Key fields (OSDP 2.2 Tables 32/33, "present in first fragment
 * only"). The prefix is not counted in TOTAL or DATA_LEN. */
int osdp_mp_tx_build_ex(struct osdp_multipart *mp, uint8_t *buf, int max_len,
			const uint8_t *pfx, int pfx_len);
/* Header-only frame at the committed offset, bypassing the data plane: the
 * FILETRANSFER idle keep-alive (OSDP 2.2 §7.25) or, once offset reaches
 * total, a W16 early-termination frame (§5.10.2). Returns the header size. */
int osdp_mp_tx_build_idle(struct osdp_multipart *mp, uint8_t *buf);
void osdp_mp_tx_commit(struct osdp_multipart *mp);

/* --- RX --- */
/* total: declared payload length when the consumer already knows it (e.g.
 * peeked from the first frame's header), so START reports a real size; 0 to
 * learn it from the first consumed frame. */
int osdp_mp_rx_init(struct osdp_multipart *mp, enum osdp_mp_width w,
		    uint32_t total);
enum osdp_mp_rc osdp_mp_rx_consume(struct osdp_multipart *mp,
				   const uint8_t *buf, int len);
/* Receiver counterpart of osdp_mp_tx_build_ex: on the offset-0 fragment,
 * pfx_len prefix bytes between header and data are copied out to pfx. */
enum osdp_mp_rc osdp_mp_rx_consume_ex(struct osdp_multipart *mp,
				      const uint8_t *buf, int len,
				      uint8_t *pfx, int pfx_len);
void osdp_mp_rx_commit(struct osdp_multipart *mp);

/* --- Control / scheduling --- */
void osdp_mp_set_event_cb(struct osdp_multipart *mp, osdp_mp_event_fn fn,
			  void *arg);
void osdp_mp_set_identity(struct osdp_multipart *mp, int msg_type,
			  int object_id);
void osdp_mp_set_wait(struct osdp_multipart *mp, uint32_t ms);
/* Park the transfer in WAIT: nothing to move right now (e.g. FILETRANSFER
 * completion held open by FTSTAT status 3). Only osdp_mp_finish() ends a
 * transfer. */
void osdp_mp_park(struct osdp_multipart *mp);
void osdp_mp_finish(struct osdp_multipart *mp, int outcome);

/* Emit the START notification exactly once per transfer. Inits never emit it
 * themselves; the consumer calls this from whichever context should deliver
 * it (e.g. the CP refresh loop rather than the submit call). Safe to call on
 * every processing tick. */
void osdp_mp_emit_start(struct osdp_multipart *mp);
enum osdp_mp_action osdp_mp_tx_next(struct osdp_multipart *mp);

/* Ready-made event_cb that turns an engine phase into an OSDP notification;
 * bind with arg = the owning struct osdp_pd. Lives in osdp_common.c because
 * it needs pd internals; declared here because its contract is engine types
 * only. */
void osdp_mp_pd_notify(void *arg, enum osdp_mp_phase phase,
		       const struct osdp_mp_progress *p);

#endif /* _OSDP_MULTIPART_H_ */

/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_BIO_H_
#define _OSDP_BIO_H_

#include "osdp_common.h"
#include "osdp_multipart.h"

#define TO_BIO(pd) ((pd)->bio)

/* Opt-in multi-part biometric read reply (PD -> CP), enabled by
 * OSDP_FLAG_BIOREADR_MULTIPART on both roles. The first reply to CMD_BIOREAD is
 * a standard REPLY_BIOREADR whose `length` field carries the TOTAL template
 * size; when it falls short of that, its payload is fragment 0 of a W16
 * multi-part transfer (OSDP 2.2 §5.10) whose remaining fragments the CP pulls
 * with osdp_POLL. A reply that fits in one packet stays single-part. */
enum osdp_bio_phase {
	OSDP_BIO_IDLE = 0,
	OSDP_BIO_REPLY, /* template transfer/reassembly in progress */
};

struct osdp_bio {
	enum osdp_bio_phase phase;
	bool start_emitted;  /* one MP_START per transfer */
	uint8_t reader;      /* fragment-0 metadata, cached for the final event */
	uint8_t status;
	uint8_t type;
	uint8_t quality;
	tick_t tstamp;       /* last forward progress; bounds a stalled op */
	struct osdp_multipart mp;
	uint8_t data[OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN]; /* template staging */
};

bool osdp_bio_is_active(struct osdp_pd *pd);
void osdp_bio_abort(struct osdp_pd *pd);

/* --- PD role --- */
bool osdp_bio_pd_reply_pending(struct osdp_pd *pd);
/* Build one reply fragment into `buf`. `event` seeds the transfer on the first
 * fragment; NULL continues an in-flight one. A template that fits in one packet
 * is written as a plain single-part reply. Returns bytes written, or -1. */
int osdp_bio_pd_reply_build(struct osdp_pd *pd, const struct osdp_event *event,
			    uint8_t *buf, int max_len);

/* --- CP role --- */
/* CMD_POLL while a reply is being reassembled, CMD_ABORT on a stalled op, else
 * 0 (nothing to drive). */
int osdp_bio_cp_get_command(struct osdp_pd *pd);
/* Feed one reply fragment. Returns 1 with `event` filled when the template is
 * complete, 0 when more fragments are expected (or the op was torn down), -1
 * on a malformed first fragment. */
int osdp_bio_cp_reply_consume(struct osdp_pd *pd, const uint8_t *buf, int len,
			      struct osdp_event *event);

#endif /* _OSDP_BIO_H_ */

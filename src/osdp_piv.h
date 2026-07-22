/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_PIV_H_
#define _OSDP_PIV_H_

#include "osdp_common.h"
#include "osdp_multipart.h"

#define TO_PIV(pd) ((pd)->piv)

/* A smartcard/PIV operation is a command leg (CP -> PD) followed by a reply
 * leg (PD -> CP); both ride the W16 multi-part codec (OSDP 2.2 §5.10). The
 * reply leg's fragments are pulled by the CP with osdp_POLL. */
enum osdp_piv_phase {
	OSDP_PIV_IDLE = 0,
	OSDP_PIV_CMD,   /* command leg in flight */
	OSDP_PIV_REPLY, /* reply leg in flight (or awaiting the app's data) */
};

struct osdp_piv {
	enum osdp_piv_phase phase;
	int mp_msg;         /* OSDP_MP_MSG_* of the active op */
	uint8_t app_cmd;    /* OSDP_CMD_PIVDATA/... */
	uint8_t wire_cmd;   /* CMD_PIVDATA/... */
	uint8_t wire_reply; /* REPLY_PIVDATAR/... */
	uint8_t event_type; /* OSDP_EVENT_* the reply surfaces as */
	bool start_emitted; /* one MP_START per op, not per leg */
	uint8_t algorithm;  /* GENAUTH/CRAUTH first-fragment prefix */
	uint8_t key;
	struct osdp_cmd_pivdata req; /* PIVDATA request arguments */
	tick_t tstamp;      /* last forward progress; bounds a stalled op */
	struct osdp_multipart mp; /* one leg at a time */
	uint8_t data[OSDP_PIV_DATA_MAX_LEN]; /* staging for the active leg */
};

bool osdp_piv_is_active(struct osdp_pd *pd);
/* True when the active PIV op's wire command is `cmd_id`. */
bool osdp_piv_owns_cmd(struct osdp_pd *pd, int cmd_id);
void osdp_piv_abort(struct osdp_pd *pd);

/* --- CP role --- */
int osdp_piv_cp_submit(struct osdp_pd *pd, const struct osdp_cmd *cmd);
int osdp_piv_cp_get_command(struct osdp_pd *pd);
int osdp_piv_cp_cmd_build(struct osdp_pd *pd, uint8_t *buf, int max_len);
/* The PD ACKed the command leg (app deferred its reply); start polling. */
void osdp_piv_cp_cmd_acked(struct osdp_pd *pd);
/* Feed one reply fragment. Returns 1 with `event` filled when the reply is
 * complete, 0 when more fragments are expected (or the op was torn down),
 * -1 when no op expects this reply. */
int osdp_piv_cp_reply_consume(struct osdp_pd *pd, const uint8_t *buf, int len,
			      struct osdp_event *event);

/* --- PD role --- */
/* Prepare the per-PD context for an incoming command of `wire_cmd`'s family
 * (allocates on first use; supersedes any stale op). */
int osdp_piv_pd_open(struct osdp_pd *pd, uint8_t wire_cmd, int object_id);
/* Consume one fragment of a multi-part command (GENAUTH/CRAUTH). Returns 1
 * with `cmd` filled when the challenge is complete, 0 when more fragments
 * are expected (reply with ACK), -1 on a malformed fragment. */
int osdp_piv_pd_cmd_frag(struct osdp_pd *pd, uint8_t wire_cmd,
			 const uint8_t *buf, int len, struct osdp_cmd *cmd);
bool osdp_piv_pd_reply_pending(struct osdp_pd *pd);
int osdp_piv_pd_reply_id(struct osdp_pd *pd);
/* Build one reply fragment. `event` seeds the reply leg on the first
 * fragment; NULL continues an in-flight one. */
int osdp_piv_pd_reply_build(struct osdp_pd *pd, const struct osdp_event *event,
			    uint8_t *buf, int max_len);

#endif /* _OSDP_PIV_H_ */

/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "osdp_piv.h"
#include "osdp_file.h"

/* Per-family wiring of the §5.10 smartcard/PIV multi-part consumers. */
struct piv_family {
	int mp_msg;
	uint8_t app_cmd;    /* OSDP_CMD_* */
	uint8_t wire_cmd;   /* CMD_* */
	uint8_t wire_reply; /* REPLY_* */
	uint8_t event_type; /* OSDP_EVENT_* */
};

static const struct piv_family piv_families[] = {
	{
		.mp_msg = OSDP_MP_MSG_PIV,
		.app_cmd = OSDP_CMD_PIVDATA,
		.wire_cmd = CMD_PIVDATA,
		.wire_reply = REPLY_PIVDATAR,
		.event_type = OSDP_EVENT_PIVDATAR,
	},
};

static const struct piv_family *family_by_app_cmd(int app_cmd)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZEOF(piv_families); i++) {
		if (piv_families[i].app_cmd == app_cmd) {
			return &piv_families[i];
		}
	}
	return NULL;
}

static const struct piv_family *family_by_wire_cmd(uint8_t wire_cmd)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZEOF(piv_families); i++) {
		if (piv_families[i].wire_cmd == wire_cmd) {
			return &piv_families[i];
		}
	}
	return NULL;
}

#ifdef OPT_OSDP_STATIC
#ifndef OSDP_PIV_STATIC_SLOTS
#define OSDP_PIV_STATIC_SLOTS OSDP_CP_MAX_PDS
#endif
static inline struct osdp_piv *piv_static_slot_get(int pd_idx)
{
	static struct osdp_piv g_osdp_piv_slots[OSDP_PIV_STATIC_SLOTS];
	if (pd_idx < 0 || pd_idx >= OSDP_PIV_STATIC_SLOTS) {
		return NULL;
	}
	return &g_osdp_piv_slots[pd_idx];
}
#endif /* OPT_OSDP_STATIC */

static struct osdp_piv *piv_get(struct osdp_pd *pd)
{
	if (!pd->piv) {
#ifdef OPT_OSDP_STATIC
		pd->piv = piv_static_slot_get(pd->idx);
		if (pd->piv) {
			memset(pd->piv, 0, sizeof(struct osdp_piv));
		}
#else
		pd->piv = calloc(1, sizeof(struct osdp_piv));
#endif
		if (pd->piv == NULL) {
			LOG_ERR("PIV: failed to allocate context");
			return NULL;
		}
	}
	return pd->piv;
}

/* End the active op; the context (and its allocation) survives for reuse. */
static void piv_op_reset(struct osdp_piv *p)
{
	p->phase = OSDP_PIV_IDLE;
	p->mp_msg = 0;
	osdp_mp_reset(&p->mp);
}

static void piv_op_setup(struct osdp_pd *pd, struct osdp_piv *p,
			 const struct piv_family *f, int object_id)
{
	p->mp_msg = f->mp_msg;
	p->wire_cmd = f->wire_cmd;
	p->wire_reply = f->wire_reply;
	p->event_type = f->event_type;
	p->tstamp = osdp_millis_now();
	osdp_mp_reset(&p->mp);
	osdp_mp_set_event_cb(&p->mp, osdp_mp_pd_notify, pd);
	osdp_mp_set_identity(&p->mp, f->mp_msg, object_id);
	p->phase = OSDP_PIV_CMD;
}

bool osdp_piv_is_active(struct osdp_pd *pd)
{
	return TO_PIV(pd) && TO_PIV(pd)->phase != OSDP_PIV_IDLE;
}

void osdp_piv_abort(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase == OSDP_PIV_IDLE) {
		return;
	}
	/* DONE never precedes START, even for an op that dies before its
	 * first fragment. */
	osdp_mp_emit_start(&p->mp);
	osdp_mp_finish(&p->mp, OSDP_FILE_TX_OUTCOME_ABORTED);
	piv_op_reset(p);
}

/* --- CP role --- */

int osdp_piv_cp_submit(struct osdp_pd *pd, const struct osdp_cmd *cmd)
{
	const struct piv_family *f = family_by_app_cmd(cmd->id);
	struct osdp_piv *p = piv_get(pd);

	if (!p || !f) {
		return -1;
	}
	if (p->phase != OSDP_PIV_IDLE) {
		LOG_ERR("PIV: an operation is already in progress");
		return -1;
	}
	/* §5.10.2: a multi-part transfer may not interleave with another. */
	if (osdp_file_tx_is_active(pd)) {
		LOG_ERR("PIV: file transfer in progress");
		return -1;
	}

	switch (cmd->id) {
	case OSDP_CMD_PIVDATA:
		p->req = cmd->pivdata;
		piv_op_setup(pd, p, f, cmd->pivdata.element);
		break;
	default:
		return -1;
	}
	return 0;
}

int osdp_piv_cp_get_command(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (!p || p->phase == OSDP_PIV_IDLE) {
		return 0;
	}
	if (osdp_millis_since(p->tstamp) > OSDP_PIV_OP_TIMEOUT_MS) {
		LOG_ERR("PIV: operation timed out; aborting");
		osdp_piv_abort(pd);
		return CMD_ABORT;
	}
	switch (p->phase) {
	case OSDP_PIV_CMD:
		return p->wire_cmd;
	case OSDP_PIV_REPLY:
		/* Reply fragments are pulled with polls (§5.10.2: the ACU is
		 * in control of the retrieval of replies). */
		return CMD_POLL;
	default:
		return 0;
	}
}

int osdp_piv_cp_cmd_build(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	struct osdp_piv *p = TO_PIV(pd);
	int len = 0;

	if (!p || p->phase != OSDP_PIV_CMD || pd->cmd_id != p->wire_cmd) {
		LOG_ERR("PIV: no command staged for build");
		return -1;
	}

	switch (pd->cmd_id) {
	case CMD_PIVDATA:
		if (max_len < CMD_PIVDATA_DATA_LEN) {
			return -1;
		}
		buf[len++] = p->req.oid[0];
		buf[len++] = p->req.oid[1];
		buf[len++] = p->req.oid[2];
		buf[len++] = p->req.element;
		buf[len++] = p->req.offset;
		break;
	default:
		return -1;
	}

	/* Pure serialization: the phy layer re-invokes this on retries. The
	 * CMD -> REPLY transition happens when a valid reply arrives. */
	return len;
}

/* The PD answered the command leg; the reply leg begins. */
static void piv_cp_reply_begin(struct osdp_pd *pd, struct osdp_piv *p)
{
	ARG_UNUSED(pd);

	osdp_mp_bind_buffer(&p->mp, p->data, sizeof(p->data));
	osdp_mp_rx_init(&p->mp, OSDP_MP_W16, 0);
	osdp_mp_emit_start(&p->mp);
	p->tstamp = osdp_millis_now();
	p->phase = OSDP_PIV_REPLY;
}

void osdp_piv_cp_cmd_acked(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	if (p && p->phase == OSDP_PIV_CMD && pd->cmd_id == p->wire_cmd) {
		/* App on the PD deferred its reply; poll for it. */
		piv_cp_reply_begin(pd, p);
	}
}

int osdp_piv_cp_reply_consume(struct osdp_pd *pd, const uint8_t *buf, int len,
			      struct osdp_event *event)
{
	struct osdp_piv *p = TO_PIV(pd);
	enum osdp_mp_rc rc;

	if (!p || pd->reply_id != p->wire_reply) {
		LOG_ERR("PIV: unexpected reply %02x", pd->reply_id);
		return -1;
	}
	if (p->phase == OSDP_PIV_CMD && pd->cmd_id == p->wire_cmd) {
		/* First fragment arrived inline as the command's reply. */
		piv_cp_reply_begin(pd, p);
	} else if (p->phase != OSDP_PIV_REPLY) {
		LOG_ERR("PIV: reply %02x with no op in progress", pd->reply_id);
		return -1;
	}

	rc = osdp_mp_rx_consume(&p->mp, buf, len);
	switch (rc) {
	case OSDP_MP_RC_MORE:
		osdp_mp_rx_commit(&p->mp);
		p->tstamp = osdp_millis_now();
		return 0;
	case OSDP_MP_RC_DONE:
		osdp_mp_rx_commit(&p->mp);
		memset(event, 0, sizeof(*event));
		event->type = p->event_type;
		event->piv_reply.length = (uint16_t)p->mp.total;
		memcpy(event->piv_reply.data, p->data, p->mp.total);
		osdp_mp_finish(&p->mp, OSDP_FILE_TX_OUTCOME_OK);
		piv_op_reset(p);
		return 1;
	case OSDP_MP_RC_EARLY_TERM:
		/* §5.10.2: the sender terminated the transfer early. */
		LOG_ERR("PIV: PD terminated the reply early; aborting");
		osdp_piv_abort(pd);
		return 0;
	default:
		LOG_ERR("PIV: bad reply fragment at off:%" PRIu32 "; aborting",
			p->mp.offset);
		osdp_piv_abort(pd);
		return 0;
	}
}

/* --- PD role --- */

int osdp_piv_pd_open(struct osdp_pd *pd, uint8_t wire_cmd, int object_id)
{
	const struct piv_family *f = family_by_wire_cmd(wire_cmd);
	struct osdp_piv *p = piv_get(pd);

	if (!p || !f) {
		return -1;
	}
	/* A new command supersedes whatever the previous op left behind. */
	osdp_piv_abort(pd);
	piv_op_setup(pd, p, f, object_id);
	return 0;
}

bool osdp_piv_pd_reply_pending(struct osdp_pd *pd)
{
	struct osdp_piv *p = TO_PIV(pd);

	return p && p->phase == OSDP_PIV_REPLY && osdp_mp_is_active(&p->mp) &&
	       p->mp.offset < p->mp.total;
}

int osdp_piv_pd_reply_id(struct osdp_pd *pd)
{
	return TO_PIV(pd)->wire_reply;
}

int osdp_piv_pd_reply_build(struct osdp_pd *pd, const struct osdp_event *event,
			    uint8_t *buf, int max_len)
{
	struct osdp_piv *p = TO_PIV(pd);
	int n;

	if (!p) {
		LOG_ERR("PIV: no context to build reply from");
		return -1;
	}
	if (event) { /* first fragment: seed the reply leg from the app */
		if (p->phase != OSDP_PIV_CMD) {
			/* No command is awaiting this reply (e.g. the op was
			 * aborted while the app's event sat in the queue). */
			LOG_ERR("PIV: reply event with no command pending");
			return -1;
		}
		if (event->piv_reply.length == 0 ||
		    event->piv_reply.length > sizeof(p->data)) {
			LOG_ERR("PIV: invalid reply length %u",
				event->piv_reply.length);
			return -1;
		}
		memcpy(p->data, event->piv_reply.data, event->piv_reply.length);
		osdp_mp_bind_buffer(&p->mp, p->data, event->piv_reply.length);
		osdp_mp_tx_init(&p->mp, event->piv_reply.length, OSDP_MP_W16);
		osdp_mp_emit_start(&p->mp);
		p->phase = OSDP_PIV_REPLY;
	} else if (!osdp_piv_pd_reply_pending(pd)) {
		LOG_ERR("PIV: no reply in progress");
		return -1;
	}

	/* The multipart build greedily consumes all offered space, but the
	 * phy layer still adds SC padding + MAC when finalizing the packet.
	 * Reserve for it, as the file transfer build does. */
	max_len -= 16;
	n = osdp_mp_tx_build(&p->mp, buf, max_len);
	if (n < 0) {
		LOG_ERR("PIV: reply fragment build failed; aborting");
		osdp_piv_abort(pd);
		return -1;
	}
	/* Commit immediately: a lost reply is replayed verbatim by the phy
	 * layer's sequence-repeat handling, never re-built. */
	osdp_mp_tx_commit(&p->mp);
	p->tstamp = osdp_millis_now();
	if (p->mp.offset >= p->mp.total) {
		osdp_mp_finish(&p->mp, OSDP_FILE_TX_OUTCOME_OK);
		piv_op_reset(p);
	}
	return n;
}

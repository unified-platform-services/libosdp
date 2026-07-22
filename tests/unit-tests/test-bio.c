/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Biometric command handling that is awkward to observe end-to-end.
 *
 * The NAK code a PD sends never reaches the CP application -- the CP only logs
 * it -- so an over-the-wire test cannot tell OSDP_PD_NAK_BIO_TYPE apart from
 * OSDP_PD_NAK_RECORD. These tests drive pd_decode_command() directly instead
 * and read pd->nak_code, which is what actually goes on the wire.
 *
 * It also white-box tests the opt-in multi-part BIOREADR reply by driving the
 * osdp_bio build/consume path directly (the over-the-wire round-trip lives in
 * the pytest suite). The single-part biometric round-trips live in
 * test-commands.c.
 */

#include <osdp.h>
#include "test.h"
#include "osdp_bio.h"

/* Exported as a test_<fn> alias under UNIT_TESTING via OSDP_TEST_ALIAS. */
extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

struct test_bio_ctx {
	int nak_ret;   /* what the command callback returns */
	bool cmd_seen;
	struct osdp_cmd last_cmd;
	bool mp_done_seen;
	int mp_done_outcome;
};

static struct test_bio_ctx g_bio_ctx;

static int test_bio_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_bio_ctx *ctx = arg;

	if (cmd->id == OSDP_CMD_NOTIFICATION &&
	    cmd->notif.type == OSDP_NOTIFICATION_MP_DONE) {
		ctx->mp_done_seen = true;
		ctx->mp_done_outcome = cmd->notif.mp.outcome;
		return 0;
	}
	ctx->cmd_seen = true;
	ctx->last_cmd = *cmd;
	return ctx->nak_ret;
}

/* osdp_BIOREAD: reader, type, format, quality */
static int bio_make_bioread(uint8_t *buf, uint8_t type)
{
	int len = 0;

	buf[len++] = 0x73; /* CMD_BIOREAD */
	buf[len++] = 0;    /* reader */
	buf[len++] = type;
	buf[len++] = OSDP_BIO_FMT_ANSI_INCITS_378;
	buf[len++] = 0x80; /* quality */
	return len;
}

/*
 * The app selects the NAK code by returning its negated value; only the codes
 * an app can know are honoured. Everything else -- including the idiomatic -1
 * -- must fall back to OSDP_PD_NAK_RECORD.
 */
static bool test_bio_nak_code_mapping(struct osdp_pd *pd)
{
	struct {
		int ret;
		int expected_nak;
		const char *desc;
	} cases[] = {
		{ -OSDP_PD_NAK_BIO_TYPE, OSDP_PD_NAK_BIO_TYPE,
		  "-NAK_BIO_TYPE selects BIO_TYPE" },
		{ -OSDP_PD_NAK_BIO_FMT, OSDP_PD_NAK_BIO_FMT,
		  "-NAK_BIO_FMT selects BIO_FMT" },
		{ -OSDP_PD_NAK_RECORD, OSDP_PD_NAK_RECORD,
		  "-NAK_RECORD selects RECORD" },
		{ -1, OSDP_PD_NAK_RECORD,
		  "legacy -1 still means RECORD" },
		{ -OSDP_PD_NAK_SC_COND, OSDP_PD_NAK_RECORD,
		  "a LibOSDP-only code falls back to RECORD" },
		{ -100, OSDP_PD_NAK_RECORD,
		  "a garbage value falls back to RECORD" },
	};
	uint8_t buf[16];
	int i, len;
	bool result = true;

	for (i = 0; i < (int)ARRAY_SIZEOF(cases); i++) {
		g_bio_ctx.nak_ret = cases[i].ret;
		g_bio_ctx.cmd_seen = false;

		len = bio_make_bioread(buf, OSDP_BIO_TYPE_RIGHT_INDEX_FINGER_PRINT);
		test_pd_decode_command(pd, buf, len);

		if (!g_bio_ctx.cmd_seen) {
			printf(SUB_2 "%s: callback not invoked\n", cases[i].desc);
			result = false;
			continue;
		}
		if (pd->reply_id != REPLY_NAK) {
			printf(SUB_2 "%s: expected NAK, got reply %02x\n",
			       cases[i].desc, pd->reply_id);
			result = false;
			continue;
		}
		if (pd->nak_code != cases[i].expected_nak) {
			printf(SUB_2 "%s: expected NAK code %d, got %d\n",
			       cases[i].desc, cases[i].expected_nak,
			       pd->nak_code);
			result = false;
			continue;
		}
		printf(SUB_2 "%s\n", cases[i].desc);
	}

	return result;
}

/* A BIOREAD that the app accepts is ACK'd when no reply event is submitted */
static bool test_bio_decode_ok(struct osdp_pd *pd)
{
	uint8_t buf[16];
	int len;

	printf(SUB_2 "testing BIOREAD decode\n");

	g_bio_ctx.nak_ret = 0;
	g_bio_ctx.cmd_seen = false;

	len = bio_make_bioread(buf, OSDP_BIO_TYPE_LEFT_IRIS_SCAN);
	test_pd_decode_command(pd, buf, len);

	if (!g_bio_ctx.cmd_seen ||
	    g_bio_ctx.last_cmd.id != OSDP_CMD_BIOREAD) {
		printf(SUB_2 "BIOREAD did not reach the app\n");
		return false;
	}
	if (g_bio_ctx.last_cmd.bioread.type != OSDP_BIO_TYPE_LEFT_IRIS_SCAN ||
	    g_bio_ctx.last_cmd.bioread.format != OSDP_BIO_FMT_ANSI_INCITS_378 ||
	    g_bio_ctx.last_cmd.bioread.quality != 0x80) {
		printf(SUB_2 "BIOREAD fields decoded incorrectly\n");
		return false;
	}
	if (pd->reply_id != REPLY_ACK) {
		printf(SUB_2 "expected ACK when app defers, got %02x\n",
		       pd->reply_id);
		return false;
	}
	return true;
}

/* A PD that reports no biometric capability must NAK without asking the app */
static bool test_bio_capability_gate(struct osdp_pd *pd)
{
	struct osdp_pd_cap saved;
	uint8_t buf[16];
	int len;
	bool result = true;

	printf(SUB_2 "testing BIOREAD is gated on OSDP_PD_CAP_BIOMETRICS\n");

	saved = pd->cap[OSDP_PD_CAP_BIOMETRICS];
	memset(&pd->cap[OSDP_PD_CAP_BIOMETRICS], 0, sizeof(struct osdp_pd_cap));

	g_bio_ctx.nak_ret = 0;
	g_bio_ctx.cmd_seen = false;

	len = bio_make_bioread(buf, OSDP_BIO_TYPE_RIGHT_THUMB_PRINT);
	test_pd_decode_command(pd, buf, len);

	if (g_bio_ctx.cmd_seen) {
		printf(SUB_2 "app callback must not run without the capability\n");
		result = false;
	}
	if (pd->reply_id != REPLY_NAK ||
	    pd->nak_code != OSDP_PD_NAK_CMD_UNKNOWN) {
		printf(SUB_2 "expected NAK(CMD_UNKNOWN), got reply %02x nak %d\n",
		       pd->reply_id, pd->nak_code);
		result = false;
	}

	pd->cap[OSDP_PD_CAP_BIOMETRICS] = saved;
	return result;
}

/*
 * Drive the opt-in multi-part BIOREADR reply path directly: build fragments on
 * a PD-role context and reassemble them on a CP-role context. `total_len` is
 * the template size and `max_len` bounds each fragment; a small max_len forces
 * several fragments, a large one keeps the reply single-part.
 */
static bool test_bio_multipart_transfer(struct osdp_pd *pd_tx,
					struct osdp_pd *pd_rx,
					int total_len, int max_len,
					const char *label)
{
	struct osdp_event ev, out;
	uint8_t frag[OSDP_EVENT_BIOREADR_MAX_TEMPLATE_LEN + 32];
	const struct osdp_event *seed = &ev;
	int i, n, rc = 0, guard;

	printf(SUB_2 "%s\n", label);

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = 3;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT;
	ev.bioreadr.quality = 200;
	ev.bioreadr.length = total_len;
	for (i = 0; i < total_len; i++) {
		ev.bioreadr.data[i] = (uint8_t)(0xC0 + i);
	}
	memset(&out, 0, sizeof(out));

	for (guard = 0; guard < 512; guard++) {
		bool first = (seed != NULL);

		if (!first && !osdp_bio_pd_reply_pending(pd_tx)) {
			printf(SUB_2 "%s: transfer stalled before completion\n",
			       label);
			return false;
		}
		n = osdp_bio_pd_reply_build(pd_tx, seed, frag, max_len);
		if (n < 0) {
			printf(SUB_2 "%s: fragment build failed\n", label);
			return false;
		}
		if (first) {
			/* Fragment 0 carries the standard BIOREADR header, with
			 * the `length` field holding the total template size. */
			if (frag[0] != 3 ||
			    frag[1] != OSDP_BIO_STATUS_SUCCESS ||
			    frag[2] != OSDP_BIO_TYPE_RIGHT_THUMB_PRINT ||
			    frag[3] != 200) {
				printf(SUB_2 "%s: fragment 0 header swap wrong\n",
				       label);
				return false;
			}
			if ((frag[4] | (frag[5] << 8)) != total_len) {
				printf(SUB_2 "%s: fragment 0 length %d != total\n",
				       label, frag[4] | (frag[5] << 8));
				return false;
			}
		}
		rc = osdp_bio_cp_reply_consume(pd_rx, frag, n, &out);
		seed = NULL;
		if (rc != 0) {
			break;
		}
	}
	if (rc != 1) {
		printf(SUB_2 "%s: reassembly did not complete (rc=%d)\n",
		       label, rc);
		return false;
	}
	if (out.type != OSDP_EVENT_BIOREADR || out.bioreadr.reader != 3 ||
	    out.bioreadr.status != OSDP_BIO_STATUS_SUCCESS ||
	    out.bioreadr.type != OSDP_BIO_TYPE_RIGHT_THUMB_PRINT ||
	    out.bioreadr.quality != 200 || out.bioreadr.length != total_len) {
		printf(SUB_2 "%s: reassembled header fields mismatch\n", label);
		return false;
	}
	for (i = 0; i < total_len; i++) {
		if (out.bioreadr.data[i] != (uint8_t)(0xC0 + i)) {
			printf(SUB_2 "%s: reassembled data mismatch at %d\n",
			       label, i);
			return false;
		}
	}
	return true;
}

/*
 * Losing CP activity mid multi-part BIOREADR reply must tear the bio op down
 * exactly like CMD_ABORT does: the op goes inactive and the app receives
 * MP_DONE(ABORTED). Drives the real osdp_pd_refresh() offline path by
 * backdating pd->tstamp past OSDP_PD_ONLINE_TOUT_MS.
 */
static bool test_bio_abort_on_pd_offline(struct osdp_pd *pd, osdp_t *pd_ctx)
{
	struct osdp_event ev;
	uint8_t frag[96];
	bool result = false;

	printf(SUB_2 "testing bio abort on PD link loss\n");

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_BIOREADR;
	ev.bioreadr.reader = 1;
	ev.bioreadr.status = OSDP_BIO_STATUS_SUCCESS;
	ev.bioreadr.type = OSDP_BIO_TYPE_RIGHT_THUMB_PRINT;
	ev.bioreadr.quality = 100;
	ev.bioreadr.length = 200;

	if (osdp_bio_pd_reply_build(pd, &ev, frag, 64) < 0 ||
	    !osdp_bio_is_active(pd)) {
		printf(SUB_2 "failed to start multi-part reply\n");
		return false;
	}

	g_bio_ctx.mp_done_seen = false;
	SET_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	pd_set_online(pd);
	pd->tstamp = osdp_millis_now() - (OSDP_PD_ONLINE_TOUT_MS + 100);
	osdp_pd_refresh(pd_ctx);

	if (osdp_bio_is_active(pd)) {
		printf(SUB_2 "bio op still active after link loss\n");
		goto out;
	}
	if (!g_bio_ctx.mp_done_seen) {
		printf(SUB_2 "no MP_DONE notification on link loss\n");
		goto out;
	}
	if (g_bio_ctx.mp_done_outcome != OSDP_MP_OUTCOME_ABORTED) {
		printf(SUB_2 "MP_DONE outcome %d, want ABORTED\n",
		       g_bio_ctx.mp_done_outcome);
		goto out;
	}
	result = true;
out:
	CLEAR_FLAG(pd, PD_FLAG_ENABLE_NOTIF);
	return result;
}

void run_bio_tests(struct test *t)
{
	osdp_t *cp = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd, *cp_pd;

	printf("\nBegin Biometric Tests\n");

	if (test_setup_devices(t, &cp, &pd_ctx)) {
		printf(SUB_1 "device setup failed!\n");
		TEST_REPORT(t, false);
		return;
	}

	pd = osdp_to_pd(pd_ctx, 0);
	cp_pd = osdp_to_pd(cp, 0);
	osdp_pd_set_command_callback(pd_ctx, test_bio_command_callback,
				     &g_bio_ctx);

	TEST_CASE(t, "bio_decode_ok", test_bio_decode_ok(pd));
	TEST_CASE(t, "bio_nak_code_mapping", test_bio_nak_code_mapping(pd));
	TEST_CASE(t, "bio_capability_gate", test_bio_capability_gate(pd));
	/* A template that spans several fragments round-trips, and one that
	 * fits a single packet stays single-part. */
	TEST_CASE(t, "bio_multipart_multi_fragment",
		  test_bio_multipart_transfer(
			  pd, cp_pd, 200, 64,
			  "multi-fragment BIOREADR reassembles"));
	TEST_CASE(t, "bio_multipart_single_packet",
		  test_bio_multipart_transfer(
			  pd, cp_pd, 30, 200,
			  "single-packet BIOREADR stays single-part"));
	TEST_CASE(t, "bio_abort_on_pd_offline",
		  test_bio_abort_on_pd_offline(pd, pd_ctx));

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd_ctx);
}

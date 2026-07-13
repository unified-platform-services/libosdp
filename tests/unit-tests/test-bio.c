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
 * The biometric round-trips themselves live in test-commands.c.
 */

#include <osdp.h>
#include "test.h"

/* Exported as a test_<fn> alias under UNIT_TESTING via OSDP_TEST_ALIAS. */
extern int test_pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len);

struct test_bio_ctx {
	int nak_ret;   /* what the command callback returns */
	bool cmd_seen;
	struct osdp_cmd last_cmd;
};

static struct test_bio_ctx g_bio_ctx;

static int test_bio_command_callback(void *arg, struct osdp_cmd *cmd)
{
	struct test_bio_ctx *ctx = arg;

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

void run_bio_tests(struct test *t)
{
	osdp_t *cp = NULL, *pd_ctx = NULL;
	struct osdp_pd *pd;
	bool result = true;

	printf("\nBegin Biometric Tests\n");

	if (test_setup_devices(t, &cp, &pd_ctx)) {
		printf(SUB_1 "device setup failed!\n");
		TEST_REPORT(t, false);
		return;
	}

	pd = osdp_to_pd(pd_ctx, 0);
	osdp_pd_set_command_callback(pd_ctx, test_bio_command_callback,
				     &g_bio_ctx);

	result &= test_bio_decode_ok(pd);
	result &= test_bio_nak_code_mapping(pd);
	result &= test_bio_capability_gate(pd);

	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd_ctx);

	printf(SUB_1 "Biometric tests %s\n", result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

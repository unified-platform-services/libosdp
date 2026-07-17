/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>

#include <osdp.h>
#include "test.h"

extern int test_state_update(struct osdp_pd *);
extern bool test_cp_cmd_failure_is_soft(struct osdp_pd *);
extern int test_cp_decode_response(struct osdp_pd *, uint8_t *, int);

int test_fsm_resp = 0;
#ifdef OPT_OSDP_RX_ZERO_COPY
/* One canned reply is served per command; release_pkt marks it consumed so a
 * poll before the next command does not re-deliver a stale frame. */
static bool s_fsm_served;
#endif

int test_cp_fsm_send(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);

#ifndef OPT_OSDP_SKIP_MARK_BYTE
	int cmd_id_offset = OSDP_CMD_ID_OFFSET + 1;
#else
	int cmd_id_offset = OSDP_CMD_ID_OFFSET;
#endif

	switch (buf[cmd_id_offset]) {
	case 0x60:
		test_fsm_resp = 1;
		break;
	case 0x61:
		test_fsm_resp = 2;
		break;
	case 0x62:
		test_fsm_resp = 3;
		break;
	default:
		printf(SUB_1 "invalid ID:0x%02x\n", buf[cmd_id_offset

		]);
	}
#ifdef OPT_OSDP_RX_ZERO_COPY
	s_fsm_served = false;
#endif
	return len;
}

/* Copy the canned reply selected by the last command into `buf`; returns its
 * length or -1 when no reply is pending. Shared by the byte-stream recv and the
 * zero-copy recv_pkt paths. */
static int fsm_fill_response(uint8_t *buf)
{
	uint8_t resp_id[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x14, 0x00, 0x04, 0x45, 0xa1, 0xa2, 0xa3, 0xb1,
		0xc1, 0xd1, 0xd2, 0xd3, 0xd4, 0xe1, 0xe2, 0xe3, 0xf8, 0xd9
	};
	uint8_t resp_cap[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x0b, 0x00, 0x05, 0x46, 0x04, 0x04, 0x01, 0xb3, 0xec
	};
	uint8_t resp_ack[] = {
#ifndef OPT_OSDP_SKIP_MARK_BYTE
		0xff,
#endif
		0x53, 0xe5, 0x08, 0x00, 0x06, 0x40, 0xb0, 0xf0
	};

	switch (test_fsm_resp) {
	case 1:
		memcpy(buf, resp_ack, sizeof(resp_ack));
		return sizeof(resp_ack);
	case 2:
		memcpy(buf, resp_id, sizeof(resp_id));
		return sizeof(resp_id);
	case 3:
		memcpy(buf, resp_cap, sizeof(resp_cap));
		return sizeof(resp_cap);
	}
	return -1;
}

int test_cp_fsm_receive(void *data, uint8_t *buf, int len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return fsm_fill_response(buf);
}

#ifdef OPT_OSDP_RX_ZERO_COPY
int test_cp_fsm_recv_pkt(void *data, const uint8_t **buf, int *max_len)
{
	static uint8_t frame[32];
	int n;

	ARG_UNUSED(data);
	if (s_fsm_served) {
		return -1;
	}
	n = fsm_fill_response(frame);
	if (n <= 0) {
		return -1;
	}
	*buf = frame;
	*max_len = n;
	return 0;
}

void test_cp_fsm_release_pkt(void *data, const uint8_t *buf)
{
	ARG_UNUSED(data);
	ARG_UNUSED(buf);
	s_fsm_served = true;
}
#endif /* OPT_OSDP_RX_ZERO_COPY */

int test_cp_fsm_setup(struct test *t)
{
	/* mock application data */
	struct osdp_channel channel = {
		.data = NULL,
		.send = test_cp_fsm_send,
#ifndef OPT_OSDP_RX_ZERO_COPY
		.recv = test_cp_fsm_receive,
#else
		.recv_pkt = test_cp_fsm_recv_pkt,
		.release_pkt = test_cp_fsm_release_pkt,
#endif
		.flush = NULL,
	};
	osdp_pd_info_t info = {
		.address = 101,
		.baud_rate = 9600,
		.flags = 0,
		.scbk = NULL,
	};
#ifndef OPT_OSDP_LOG_MINIMAL
	osdp_logger_init("osdp::cp", t->loglevel, NULL);
#endif /* OPT_OSDP_LOG_MINIMAL */
	struct osdp *ctx = (struct osdp *)osdp_cp_setup(&channel, 1, &info);
	if (ctx == NULL) {
		printf("   init failed!\n");
		return -1;
	}
	SET_CURRENT_PD(ctx, 0);
	SET_FLAG(GET_CURRENT_PD(ctx), PD_FLAG_SKIP_SEQ_CHECK);
	t->mock_data = (void *)ctx;
	return 0;
}

void test_cp_fsm_teardown(struct test *t)
{
	osdp_cp_teardown(t->mock_data);
}

/**
 * A command failure is "soft" when the PD answered us and merely declined the
 * command: the app is told, but the link stays up. Everything else -- a silent
 * or garbled PD, a NAK that describes the link rather than the command, or a
 * command LibOSDP itself depends on -- must still take the PD offline.
 */
struct soft_fail_case {
	const char *name;
	enum osdp_cp_state_e state;
	enum osdp_cp_phy_state_e phy_state;
	int cmd_id;
	int reply_id;
	uint8_t nak_code;
	bool is_soft;
};

static const struct soft_fail_case soft_fail_cases[] = {
	/* The PD declined an app command; it is still very much alive. */
	{ "LED NAK'd with RECORD", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },
	{ "LED the PD does not implement", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_CMD_UNKNOWN, true },
	{ "file transfer refused", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_FILETRANSFER, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },
	{ "vendor command reported an error", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_MFG, REPLY_MFGERRR,
	  OSDP_PD_NAK_NONE, true },
	{ "app keyset NAK'd while online", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_KEYSET, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, true },

	/* The NAK code describes the link, not the command. */
	{ "NAK'd for want of secure channel", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_SC_COND, false },
	{ "NAK'd with a bad check character", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_MSG_CHK, false },
	{ "NAK'd with a sequence error", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_SEQ_NUM, false },

	/* The PD never answered, or answered something else entirely. */
	{ "no reply to an LED command", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_ERR, CMD_LED, REPLY_INVALID,
	  OSDP_PD_NAK_NONE, false },
	/* The phy rejected the frame; nak_code is whatever the last exchange
	 * left behind and must not be read as the PD's answer to this one. */
	{ "phy error carrying a stale NAK code", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_ERR, CMD_LED, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, false },
	{ "LED answered with a status report", OSDP_CP_STATE_ONLINE,
	  OSDP_CP_PHY_STATE_DONE, CMD_LED, REPLY_LSTATR,
	  OSDP_PD_NAK_NONE, false },

	/* Commands LibOSDP issues for itself; if these fail, the link is not
	 * usable no matter what the PD says. */
	{ "poll NAK'd", OSDP_CP_STATE_ONLINE, OSDP_CP_PHY_STATE_DONE,
	  CMD_POLL, REPLY_NAK, OSDP_PD_NAK_RECORD, false },
	{ "ID request NAK'd", OSDP_CP_STATE_INIT, OSDP_CP_PHY_STATE_DONE,
	  CMD_ID, REPLY_NAK, OSDP_PD_NAK_RECORD, false },
	{ "SCBK install NAK'd", OSDP_CP_STATE_SET_SCBK,
	  OSDP_CP_PHY_STATE_DONE, CMD_KEYSET, REPLY_NAK,
	  OSDP_PD_NAK_RECORD, false },
};

static void test_cp_soft_failure_matrix(struct test *t)
{
	bool result = true;
	struct osdp_pd pd;

	printf(SUB_1 "classifying command failures\n");

	for (size_t i = 0; i < ARRAY_SIZEOF(soft_fail_cases); i++) {
		const struct soft_fail_case *c = &soft_fail_cases[i];

		memset(&pd, 0, sizeof(pd));
		pd.state = c->state;
		pd.phy_state = c->phy_state;
		pd.cmd_id = c->cmd_id;
		pd.reply_id = c->reply_id;
		pd.nak_code = c->nak_code;

		if (test_cp_cmd_failure_is_soft(&pd) != c->is_soft) {
			printf(SUB_2 "%s: expected a %s failure\n", c->name,
			       c->is_soft ? "soft" : "hard");
			result = false;
		}
	}

	printf(SUB_1 "command failure classification %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

/**
 * NAK(MSG_CHK) means "your check character was wrong". From a PD that never
 * claimed CRC-16 that is how a checksum-only PD tells us so, and we downgrade
 * the link. From a PD that did claim CRC-16 it can only mean the frame was
 * corrupted in flight: downgrading there would silently reframe every packet
 * that follows, and the PD -- still speaking CRC -- would demand a sequence
 * reset and take the whole link offline. Resend instead.
 */
static int feed_msg_chk_nak(struct osdp_pd *pd, bool pd_declares_crc)
{
	struct osdp_pd_cap *cap =
		&pd->cap[OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT];
	uint8_t nak[] = { REPLY_NAK, OSDP_PD_NAK_MSG_CHK };

	pd->cmd_id = CMD_LED;
	pd->phy_retry_count = 0;
	SET_FLAG(pd, PD_FLAG_CP_USE_CRC);
	if (pd_declares_crc) {
		cap->function_code = OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT;
		cap->compliance_level = 1;
	} else {
		memset(cap, 0, sizeof(*cap));
	}
	return test_cp_decode_response(pd, nak, sizeof(nak));
}

static void test_cp_crc_nak_fallback(struct test *t)
{
	bool result = true;
	struct osdp_pd pd;
	struct osdp ctx;

	printf(SUB_1 "classifying a NAK(MSG_CHK)\n");

	memset(&ctx, 0, sizeof(ctx));
	memset(&pd, 0, sizeof(pd));
	pd.osdp_ctx = &ctx;

	feed_msg_chk_nak(&pd, false);
	if (ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "a PD that never claimed CRC-16 must be downgraded\n");
		result = false;
	}

	/* A resend request is a non-zero rc; the retry funnel owns the
	 * budget accounting, so the decoder must leave the counter alone. */
	if (feed_msg_chk_nak(&pd, true) == 0 || pd.phy_retry_count != 0) {
		printf(SUB_2 "a corrupted frame must be resent; retries: %d\n",
		       pd.phy_retry_count);
		result = false;
	}
	if (!ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "a PD that claimed CRC-16 must not be downgraded\n");
		result = false;
	}

	/* Once the resends are spent, it is an ordinary NAK again */
	pd.phy_retry_count = OSDP_CMD_MAX_RETRIES;
	if (test_cp_decode_response(&pd, (uint8_t[]){ REPLY_NAK,
						      OSDP_PD_NAK_MSG_CHK },
				    2) != 0 ||
	    !ISSET_FLAG(&pd, PD_FLAG_CP_USE_CRC)) {
		printf(SUB_2 "resends must be bounded, not endless\n");
		result = false;
	}

	printf(SUB_1 "NAK(MSG_CHK) classification %s\n",
	       result ? "succeeded" : "failed");
	TEST_REPORT(t, result);
}

void run_cp_fsm_tests(struct test *t)
{
	int result = true;
	uint32_t count = 0;
	struct osdp *ctx;

	printf("\nStarting CP Phy state tests\n");

	test_cp_soft_failure_matrix(t);
	test_cp_crc_nak_fallback(t);

	if (test_cp_fsm_setup(t))
		return;

	ctx = t->mock_data;

	printf(SUB_1 "executing state_update()\n");
	while (1) {
		test_state_update(GET_CURRENT_PD(ctx));

		if (GET_CURRENT_PD(ctx)->state == OSDP_CP_STATE_OFFLINE) {
			printf(SUB_2 "state_update() CP went offline\n");
			result = false;
			break;
		}
		if (count++ > 300)
			break;
		usleep(1000);
	}
	printf(SUB_1 "state_update test %s\n", result ? "succeeded" : "failed");

	TEST_REPORT(t, result);

	test_cp_fsm_teardown(t);
}

// unnecessary

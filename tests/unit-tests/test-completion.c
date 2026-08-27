/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-cutting completion behaviour that belongs to no single engine:
 * re-entrancy from inside a completion, and the teardown guard.
 */

#include <osdp.h>
#include "test.h"

struct completion_ctx {
	osdp_t *cp;
	osdp_t *pd;
	int cp_runner;
	int pd_runner;
	struct test_completion comp;
	/* Set by a callback that re-enters libosdp; read by the test. */
	atomic_int resubmit_rc;
	atomic_bool resubmit_armed;
};

static struct completion_ctx g_comp;

static struct osdp_cmd make_led_cmd(void)
{
	struct osdp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.id = OSDP_CMD_LED;
	cmd.led.reader = 0;
	cmd.led.led_number = 0;
	cmd.led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmd.led.temporary.on_count = 10;
	cmd.led.temporary.off_count = 10;
	cmd.led.temporary.on_color = OSDP_LED_COLOR_RED;
	cmd.led.temporary.off_color = OSDP_LED_COLOR_NONE;
	cmd.led.temporary.timer_count = 10;
	cmd.led.permanent.control_code = OSDP_CMD_LED_PERMANENT_CC_NOP;
	return cmd;
}

/* Completion that submits a fresh command from inside itself. */
static void resubmit_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				   enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_cmd next = make_led_cmd();

		atomic_store(&c->resubmit_rc,
			     osdp_cp_submit_command(c->cp, 0, &next) == 0 ?
				     1 : 0);
	}
	test_cmd_free(cmd);
}

/*
 * A command submitted from inside a FLUSHED completion belongs to the queue
 * that exists *after* the flush: flush removes what was queued when flush was
 * called, so the new command must survive.
 */
static bool test_submit_from_flush_completion(void)
{
	struct osdp_cmd cmd = make_led_cmd();

	test_completion_reset(&g_comp.comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);

	if (!test_submit_command(g_comp.cp, 0, &cmd)) {
		printf(SUB_2 "flush: submit rejected\n");
		return false;
	}
	osdp_cp_flush_commands(g_comp.cp, 0);

	if (test_completion_count(&g_comp.comp) < 1) {
		printf(SUB_2 "flush: no completion fired\n");
		return false;
	}
	if (test_completion_status(&g_comp.comp) != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "flush: status %d, want FLUSHED\n",
		       test_completion_status(&g_comp.comp));
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 1) {
		printf(SUB_2 "flush: re-entrant submit failed\n");
		return false;
	}
	/* The re-entrant command must still be queued: flushing again finds
	 * exactly it. */
	if (osdp_cp_flush_commands(g_comp.cp, 0) != 1) {
		printf(SUB_2 "flush: re-entrant command did not survive\n");
		return false;
	}
	return true;
}

void run_completion_tests(struct test *t)
{
	printf("\nBegin completion tests\n");

	if (test_setup_devices(t, &g_comp.cp, &g_comp.pd)) {
		printf(SUB_1 "completion: device setup failed\n");
		return;
	}
	osdp_cp_set_command_completion_callback(g_comp.cp,
						resubmit_completion_cb,
						&g_comp);

	g_comp.cp_runner = async_runner_start(g_comp.cp, osdp_cp_refresh);
	g_comp.pd_runner = async_runner_start(g_comp.pd, osdp_pd_refresh);
	if (g_comp.cp_runner < 0 || g_comp.pd_runner < 0) {
		printf(SUB_1 "completion: failed to create CP/PD runners\n");
		return;
	}
	if (!test_wait_for_online(g_comp.cp, 0, 10)) {
		printf(SUB_1 "completion: PD failed to come online\n");
		return;
	}

	TEST_CASE(t, "submit_from_flush_completion",
		  test_submit_from_flush_completion());

	async_runner_stop(g_comp.cp_runner);
	async_runner_stop(g_comp.pd_runner);

	osdp_cp_teardown(g_comp.cp);
	osdp_pd_teardown(g_comp.pd);
	test_alloc_reset();

	printf("End completion tests\n\n");
}

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

/* Flushing from inside a completion must not re-enter the drain loop. */
static void flush_from_completion_cb(void *arg, int pd, struct osdp_cmd *cmd,
				     enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		atomic_store(&c->resubmit_rc,
			     osdp_cp_flush_commands(c->cp, 0));
	}
	test_cmd_free(cmd);
}

static bool test_flush_from_completion(void)
{
	struct osdp_cmd a = make_led_cmd();
	struct osdp_cmd b = make_led_cmd();

	test_completion_reset(&g_comp.comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);
	osdp_cp_set_command_completion_callback(g_comp.cp,
						flush_from_completion_cb,
						&g_comp);

	if (!test_submit_command(g_comp.cp, 0, &a) ||
	    !test_submit_command(g_comp.cp, 0, &b)) {
		printf(SUB_2 "nested flush: submit rejected\n");
		return false;
	}
	/* The outer flush detaches both; the inner one, fired from the first
	 * completion, sees an already-empty live queue and returns 0. */
	if (osdp_cp_flush_commands(g_comp.cp, 0) != 2) {
		printf(SUB_2 "nested flush: outer flush miscounted\n");
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 0) {
		printf(SUB_2 "nested flush: inner returned %d, want 0\n",
		       atomic_load(&g_comp.resubmit_rc));
		return false;
	}
	if (test_completion_count(&g_comp.comp) != 2) {
		printf(SUB_2 "nested flush: %d completions, want 2\n",
		       test_completion_count(&g_comp.comp));
		return false;
	}
	/* Restore the suite's default callback for the cases that follow. */
	osdp_cp_set_command_completion_callback(g_comp.cp,
						resubmit_completion_cb,
						&g_comp);
	return true;
}

static struct test_completion g_pd_comp;

static void pd_resubmit_completion_cb(void *arg, struct osdp_event *ev,
				      enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	test_completion_record(&g_pd_comp, ev->type, (int)status);
	if (atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_event next;

		memset(&next, 0, sizeof(next));
		next.type = OSDP_EVENT_CARDREAD;
		next.cardread.reader_no = 0;
		next.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
		next.cardread.length = 16;
		atomic_store(&c->resubmit_rc,
			     osdp_pd_submit_event(c->pd, &next) == 0 ? 1 : 0);
	}
	test_event_free(ev);
}

/* An event submitted from inside a FLUSHED completion survives the flush. */
static bool test_pd_submit_from_flush_completion(void)
{
	struct osdp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = OSDP_EVENT_CARDREAD;
	ev.cardread.reader_no = 0;
	ev.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	ev.cardread.length = 16;

	test_completion_reset(&g_pd_comp);
	atomic_store(&g_comp.resubmit_rc, -1);
	atomic_store(&g_comp.resubmit_armed, true);
	osdp_pd_set_event_completion_callback(g_comp.pd,
					      pd_resubmit_completion_cb,
					      &g_comp);

	if (!test_submit_event(g_comp.pd, &ev)) {
		printf(SUB_2 "pd flush: submit rejected\n");
		return false;
	}
	osdp_pd_flush_events(g_comp.pd);

	if (test_completion_status(&g_pd_comp) != OSDP_COMPLETION_FLUSHED) {
		printf(SUB_2 "pd flush: status %d, want FLUSHED\n",
		       test_completion_status(&g_pd_comp));
		return false;
	}
	if (atomic_load(&g_comp.resubmit_rc) != 1) {
		printf(SUB_2 "pd flush: re-entrant submit failed\n");
		return false;
	}
	if (osdp_pd_flush_events(g_comp.pd) != 1) {
		printf(SUB_2 "pd flush: re-entrant event did not survive\n");
		return false;
	}
	return true;
}

/* Completion that calls back into libosdp; records what it got back. */
static void reenter_on_abort_cb(void *arg, int pd, struct osdp_cmd *cmd,
				enum osdp_completion_status status)
{
	struct completion_ctx *c = arg;

	ARG_UNUSED(pd);
	test_completion_record(&c->comp, cmd->id, (int)status);
	if (status == OSDP_COMPLETION_ABORTED &&
	    atomic_exchange(&c->resubmit_armed, false)) {
		struct osdp_cmd next = make_led_cmd();

		atomic_store(&c->resubmit_rc,
			     osdp_cp_submit_command(c->cp, 0, &next));
	}
	test_cmd_free(cmd);
}

/* Isolated from g_comp: this case tears its own devices down, and must not
 * disturb the suite-owned pair that later cases still rely on. */
static struct completion_ctx g_teardown_comp;

/*
 * An osdp_* call from inside an ABORTED completion must fail cleanly rather
 * than enqueue into a context that is being torn down.
 */
static bool test_submit_during_teardown_is_refused(struct test *t)
{
	osdp_t *cp = NULL, *pd = NULL;
	struct osdp_cmd cmd = make_led_cmd();
	int cp_runner, pd_runner;

	if (test_setup_devices(t, &cp, &pd)) {
		printf(SUB_2 "teardown: device setup failed\n");
		return false;
	}
	g_teardown_comp.cp = cp;
	test_completion_reset(&g_teardown_comp.comp);
	atomic_store(&g_teardown_comp.resubmit_rc, 0);
	atomic_store(&g_teardown_comp.resubmit_armed, true);
	osdp_cp_set_command_completion_callback(cp, reenter_on_abort_cb,
						&g_teardown_comp);

	/* cp_submit_command() refuses while the PD is offline, so this pair
	 * needs its own brief run before it can be torn down. */
	cp_runner = async_runner_start(cp, osdp_cp_refresh);
	pd_runner = async_runner_start(pd, osdp_pd_refresh);
	if (cp_runner < 0 || pd_runner < 0 ||
	    !test_wait_for_online(cp, 0, 10)) {
		printf(SUB_2 "teardown: PD failed to come online\n");
		async_runner_stop(cp_runner);
		async_runner_stop(pd_runner);
		osdp_cp_teardown(cp);
		osdp_pd_teardown(pd);
		return false;
	}

	if (!test_submit_command(cp, 0, &cmd)) {
		printf(SUB_2 "teardown: submit rejected\n");
		async_runner_stop(cp_runner);
		async_runner_stop(pd_runner);
		osdp_cp_teardown(cp);
		osdp_pd_teardown(pd);
		return false;
	}
	async_runner_stop(cp_runner);
	async_runner_stop(pd_runner);
	osdp_cp_teardown(cp);
	osdp_pd_teardown(pd);

	if (test_completion_status(&g_teardown_comp.comp) !=
	    OSDP_COMPLETION_ABORTED) {
		printf(SUB_2 "teardown: status %d, want ABORTED\n",
		       test_completion_status(&g_teardown_comp.comp));
		return false;
	}
	if (atomic_load(&g_teardown_comp.resubmit_rc) != -1) {
		printf(SUB_2 "teardown: submit returned %d, want -1\n",
		       atomic_load(&g_teardown_comp.resubmit_rc));
		return false;
	}
	return true;
}

/* Every submitted object must have come back by the time teardown returns. */
static bool test_no_objects_outstanding_after_teardown(void)
{
	if (test_alloc_outstanding() != 0) {
		printf(SUB_2 "outstanding: %d object(s) never completed\n",
		       test_alloc_outstanding());
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
		goto stop_runners;
	}
	if (!test_wait_for_online(g_comp.cp, 0, 10)) {
		printf(SUB_1 "completion: PD failed to come online\n");
		goto stop_runners;
	}

	TEST_CASE(t, "submit_from_flush_completion",
		  test_submit_from_flush_completion());
	TEST_CASE(t, "flush_from_completion", test_flush_from_completion());
	TEST_CASE(t, "pd_submit_from_flush_completion",
		  test_pd_submit_from_flush_completion());

	async_runner_stop(g_comp.cp_runner);
	async_runner_stop(g_comp.pd_runner);

	/*
	 * The mock channel is a single shared pair of buffers, not one per
	 * CP/PD pair, so the teardown case's own devices cannot come online
	 * while g_comp's runners are still driving it. Its pair is set up and
	 * torn down here, between g_comp's runners stopping and g_comp itself
	 * being torn down below.
	 */
	TEST_CASE(t, "submit_during_teardown_is_refused",
		  test_submit_during_teardown_is_refused(t));
	goto teardown;

stop_runners:
	async_runner_stop(g_comp.cp_runner);
	async_runner_stop(g_comp.pd_runner);
teardown:
	osdp_cp_teardown(g_comp.cp);
	osdp_pd_teardown(g_comp.pd);

	/*
	 * Teardown is itself a source of leaks, so this has to be checked once
	 * both contexts are gone -- and before test_alloc_reset() drops the
	 * accounting that would show them.
	 */
	TEST_CASE(t, "no_objects_outstanding_after_teardown",
		  test_no_objects_outstanding_after_teardown());
	test_alloc_reset();

	printf("End completion tests\n\n");
}

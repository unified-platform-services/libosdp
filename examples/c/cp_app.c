/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <osdp.h>
#include <stdint.h>

/**
 * This method overrides the one provided by libosdp. It should return
 * a millisecond reference point from some tick source.
 */
int64_t osdp_millis_now()
{
	return 0;
}

int sample_cp_send_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);

	// TODO (user): send buf of len bytes, over the UART channel.

	return len;
}

int sample_cp_recv_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);
	(void)(len);

	// TODO (user): read from UART channel into buf, for upto len bytes.

	return 0;
}

osdp_pd_info_t pd_info[] = {
	{
		.address = 101,
		.baud_rate = 115200,
		.flags = 0,
		.scbk = NULL,
	},
};

static struct osdp_channel cp_channel = {
	.data = NULL,
	.send = sample_cp_send_func,
	.recv = sample_cp_recv_func,
};

static const char *completion_status_name(enum osdp_completion_status status)
{
	switch (status) {
	case OSDP_COMPLETION_OK:       return "OK";
	case OSDP_COMPLETION_FAILED:   return "FAILED";
	case OSDP_COMPLETION_FLUSHED:  return "FLUSHED";
	case OSDP_COMPLETION_ABORTED:  return "ABORTED";
	case OSDP_COMPLETION_ACCEPTED: return "ACCEPTED";
	default:                       return "?";
	}
}

/**
 * Ownership of a submitted command returns here, exactly once, whatever its
 * fate: it was answered by the PD (OK), it failed (FAILED), it was dropped by
 * osdp_cp_flush_commands() (FLUSHED), or the context was torn down with it
 * still queued (ABORTED). This is where a heap-allocated command is freed --
 * do it here and nowhere else, and the application never has to track which
 * commands are still in flight.
 *
 * Runs on the thread that called osdp_cp_refresh() (or flush/teardown), so
 * keep it short: no blocking, no long work.
 */
static void cp_command_completion(void *arg, int pd, struct osdp_cmd *cmd,
				  enum osdp_completion_status status)
{
	(void)(arg);

	printf("CP: PD%d command %d completed: %s\n", pd, cmd->id,
	       completion_status_name(status));

	/* Last use of cmd -- ownership ends here. */
	free(cmd);
}

/**
 * Commands are queued *by reference*: libosdp does not copy them. A submitted
 * command must stay alive and unmodified until its completion callback fires,
 * so it cannot live on the stack of a function that returns, and it cannot be
 * reused for a second submission while the first is still queued.
 *
 * Allocating per command and freeing in the completion callback is the
 * simplest correct pattern -- submit and forget.
 */
static int cp_submit_led_command(osdp_t *ctx, int pd)
{
	struct osdp_cmd *cmd = calloc(1, sizeof(struct osdp_cmd));

	if (cmd == NULL) {
		return -1;
	}

	cmd->id = OSDP_CMD_LED;
	cmd->led.reader = 0;
	cmd->led.led_number = 0;
	cmd->led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmd->led.temporary.on_count = 10;
	cmd->led.temporary.off_count = 10;
	cmd->led.temporary.on_color = OSDP_LED_COLOR_GREEN;
	cmd->led.temporary.off_color = OSDP_LED_COLOR_NONE;
	cmd->led.temporary.timer_count = 20;

	if (osdp_cp_submit_command(ctx, pd, cmd)) {
		/* Rejected: the command was never queued, so no completion
		 * callback will fire for it. It is ours to free right now. */
		printf("CP: failed to submit LED command\n");
		free(cmd);
		return -1;
	}

	/* Accepted. The completion callback owns it from here -- do not touch
	 * cmd again, it may already have been freed. */
	return 0;
}

int main()
{
	osdp_t *ctx;

	osdp_logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

	ctx = osdp_cp_setup(&cp_channel, 1, pd_info);
	if (ctx == NULL) {
		printf("cp init failed!\n");
		return -1;
	}

	/* Register this before the first submission: libosdp refuses to accept
	 * a command with no completion callback, because the callback is the
	 * only way ownership of the command gets back to the application. */
	osdp_cp_set_command_completion_callback(ctx, cp_command_completion,
						NULL);

	while (1) {
		uint8_t online_mask = 0;

		/* Commands can only be submitted to a PD that is online; the
		 * submission is rejected otherwise. */
		osdp_get_status_mask(ctx, &online_mask);
		if (online_mask & (1 << 0)) {
			// your application code decides when to send.
			cp_submit_led_command(ctx, 0);
		}

		/* libosdp is not internally synchronized. If commands are
		 * submitted from another thread, serialize that thread against
		 * this refresh with a lock of your own. */
		osdp_cp_refresh(ctx);
		// delay();
	}

	/* Never reached here, but on a real shutdown path teardown completes
	 * every still-queued command as ABORTED, so the callback above frees
	 * them and nothing leaks. */
	osdp_cp_teardown(ctx);
	return 0;
}

/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <osdp.hpp>

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
		.name = "pd[101]",
		.baud_rate = 115200,
		.address = 101,
		.flags = 0,
		.id = {},
		.cap = nullptr,
		.scbk = nullptr,
	}
};

static struct osdp_channel cp_channel = {
	.data = nullptr,
	.recv = sample_cp_recv_func,
	.send = sample_cp_send_func,
	.flush = nullptr,
	.close = nullptr,
};

int event_handler(void *data, int pd, struct osdp_event *event) {
	(void)(data);

	std::cout << "PD" << pd << " EVENT: " << event->type << std::endl;
	return 0;
}

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
 * fate: answered by the PD (OK), failed (FAILED), dropped by flush_commands()
 * (FLUSHED), or still queued when the context was torn down (ABORTED).
 *
 * This is where a heap-allocated command is deleted -- here and nowhere else,
 * so the application never has to track which commands are still in flight.
 *
 * Runs on the thread that called refresh() (or flush/teardown), so keep it
 * short: no blocking, no long work.
 */
void command_completion_handler(void *data, int pd, struct osdp_cmd *cmd,
				enum osdp_completion_status status)
{
	(void)(data);

	std::cout << "PD" << pd << " command " << cmd->id << " completed: "
		  << completion_status_name(status) << std::endl;

	/* Last use of cmd -- ownership ends here. */
	delete cmd;
}

/**
 * Commands are queued *by reference*: libosdp does not copy them. A submitted
 * command must stay alive and unmodified until its completion callback fires,
 * so it cannot be a local that goes out of scope, and it cannot be reused for
 * a second submission while the first is still queued.
 *
 * Allocating per submission and deleting in the completion handler is the
 * simplest correct pattern -- submit and forget. Note that nothing touches cmd
 * after a successful submit: a command consumed by an internal engine (file
 * transfer, smartcard) completes synchronously with ACCEPTED from inside
 * submit_command(), so by the time it returns the handler may already have
 * deleted it.
 */
static bool submit_led_command(OSDP::ControlPanel &cp, int pd)
{
	struct osdp_cmd *cmd = new struct osdp_cmd();

	cmd->id = OSDP_CMD_LED;
	cmd->led.reader = 0;
	cmd->led.led_number = 0;
	cmd->led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
	cmd->led.temporary.on_count = 10;
	cmd->led.temporary.off_count = 10;
	cmd->led.temporary.on_color = OSDP_LED_COLOR_GREEN;
	cmd->led.temporary.off_color = OSDP_LED_COLOR_NONE;
	cmd->led.temporary.timer_count = 20;

	if (cp.submit_command(pd, cmd)) {
		/* Rejected: the command was never queued, so no completion
		 * callback will fire for it. It is ours to delete right now. */
		std::cout << "failed to submit LED command" << std::endl;
		delete cmd;
		return false;
	}

	/* Accepted. The completion handler owns it from here -- do not touch
	 * cmd again, it may already have been deleted. */
	return true;
}

int main()
{
	OSDP::ControlPanel cp;

	cp.logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

	cp.setup(&cp_channel, 1, pd_info);

	cp.set_event_callback(event_handler, nullptr);

	/* Register this before the first submission: libosdp refuses to accept
	 * a command with no completion callback, because the callback is the
	 * only way ownership of the command gets back to the application. */
	cp.set_command_completion_callback(command_completion_handler, nullptr);

	while (1) {
		uint8_t online_mask = 0;

		/* Commands can only be submitted to a PD that is online; the
		 * submission is rejected otherwise. */
		cp.get_status_mask(&online_mask);
		if (online_mask & (1 << 0)) {
			// your application code decides when to send.
			submit_led_command(cp, 0);
		}

		/* libosdp is not internally synchronized. If commands are
		 * submitted from another thread, serialize that thread against
		 * this refresh with a lock of your own. */
		cp.refresh();
		std::this_thread::sleep_for(std::chrono::microseconds(10 * 1000));
	}

	/* Never reached here, but on a real shutdown path teardown completes
	 * every still-queued command as ABORTED, so the handler above reclaims
	 * them and nothing leaks. */
	return 0;
}

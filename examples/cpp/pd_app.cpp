/*
 * Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <chrono>
#include <cstring>
#include <thread>
#include <osdp.hpp>

int sample_pd_send_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);

	// TODO (user): send buf of len bytes, over the UART channel.

	return len;
}

int sample_pd_recv_func(void *data, uint8_t *buf, int len)
{
	(void)(data);
	(void)(buf);
	(void)(len);

	// TODO (user): read from UART channel into buf, for upto len bytes.

	return 0;
}

osdp_pd_info_t info_pd = {
	.name = "pd[101]",
	.baud_rate = 9600,
	.address = 101,
	.flags = 0,
	.id = {
		.version = 1,
		.model = 153,
		.vendor_code = 31337,
		.serial_number = 0x01020304,
		.firmware_version = 0x0A0B0C0D,
	},
		.cap = (struct osdp_pd_cap []) {
		{
			.function_code = OSDP_PD_CAP_READER_LED_CONTROL,
			.compliance_level = 1,
			.num_items = 1
		},
		{
			.function_code = OSDP_PD_CAP_READER_AUDIBLE_OUTPUT,
			.compliance_level = 1,
			.num_items = 1
		},
			{ OSDP_PD_CAP_SENTINEL, 0, 0 }
		},
		/* No SCBK: the secure channel is disabled and this link runs in
		 * plain text. Pass a key here, or OSDP_FLAG_INSTALL_MODE in
		 * .flags, to bring up OSDP-SC. */
		.scbk = nullptr,
};

static struct osdp_channel pd_channel = {
	.data = nullptr,
	.recv = sample_pd_recv_func,
	.send = sample_pd_send_func,
	.flush = nullptr,
	.close = nullptr,
};

/**
 * The CP has sent us a command; act on it and return 0 to accept it (non-zero
 * makes libosdp answer the CP with a NAK).
 *
 * cmd is owned by libosdp and is valid only for the duration of this call. To
 * use it later, copy what you need out of it -- do not store the pointer.
 */
int pd_command_handler(void *data, struct osdp_cmd *cmd)
{
	(void)(data);

	std::cout << "PD: CMD: " << cmd->id << std::endl;
	return 0;
}

static const char *completion_status_name(enum osdp_completion_status status)
{
	switch (status) {
	case OSDP_COMPLETION_OK:      return "OK";
	case OSDP_COMPLETION_FAILED:  return "FAILED";
	case OSDP_COMPLETION_FLUSHED: return "FLUSHED";
	case OSDP_COMPLETION_ABORTED: return "ABORTED";
	default:                      return "?";
	}
}

/**
 * Ownership of a submitted event returns here, exactly once, whatever its
 * fate: it went out to the CP (OK), it could not be sent (FAILED), it was
 * dropped by flush_events() (FLUSHED), or it was still queued when the context
 * was torn down (ABORTED).
 *
 * This is where a heap-allocated event is deleted -- here and nowhere else.
 *
 * Note that OK means the reply carrying this event was handed to the
 * transport, not that the CP acknowledged it; OSDP has no PD-side delivery
 * receipt.
 *
 * Runs on the thread that called refresh() (or flush/teardown), so keep it
 * short.
 */
void event_completion_handler(void *data, struct osdp_event *event,
			      enum osdp_completion_status status)
{
	(void)(data);

	std::cout << "PD: event " << event->type << " completed: "
		  << completion_status_name(status) << std::endl;

	/* Last use of event -- ownership ends here. */
	delete event;
}

/**
 * Events are queued *by reference*: libosdp does not copy them. A submitted
 * event must stay alive and unmodified until its completion callback fires,
 * so it cannot be a local that goes out of scope, and it cannot be reused for
 * a second submission while the first is still queued.
 *
 * Allocating per submission and deleting in the completion handler is the
 * simplest correct pattern -- submit and forget. The event is delivered to the
 * CP as the reply to some future POLL.
 */
static bool submit_card_read(OSDP::PeripheralDevice &pd,
			     const uint8_t *card_data, int nr_bits)
{
	struct osdp_event *event = new struct osdp_event();

	event->type = OSDP_EVENT_CARDREAD;
	event->cardread.reader_no = 0;
	event->cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	event->cardread.direction = 0;
	event->cardread.length = nr_bits;
	std::memcpy(event->cardread.data, card_data, (nr_bits + 7) / 8);

	if (pd.submit_event(event)) {
		/* Rejected: the event was never queued, so no completion
		 * callback will fire for it. It is ours to delete right now. */
		std::cout << "failed to submit card read event" << std::endl;
		delete event;
		return false;
	}

	/* Accepted. The completion handler owns it from here -- do not touch
	 * event again. */
	return true;
}

int main()
{
	OSDP::PeripheralDevice pd;

	pd.logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

	pd.setup(&pd_channel, &info_pd);

	pd.set_command_callback(pd_command_handler, nullptr);

	/* Register this before the first submission: libosdp refuses to accept
	 * an event with no completion callback, because the callback is the
	 * only way ownership of the event gets back to the application. */
	pd.set_event_completion_callback(event_completion_handler, nullptr);

	while (1) {
		/* libosdp is not internally synchronized. If events are
		 * submitted from another thread (a card reader ISR handing off
		 * to a task, say), serialize that thread against this refresh
		 * with a lock of your own. */
		pd.refresh();

		// your application code; when a card is presented:
		if (false) {
			const uint8_t card_data[] = { 0x01, 0x23, 0x45, 0x67 };

			submit_card_read(pd, card_data, 32);
		}
		std::this_thread::sleep_for(std::chrono::microseconds(10 * 1000));
	}

	/* Never reached here, but on a real shutdown path teardown completes
	 * every still-queued event as ABORTED, so the handler above reclaims
	 * them and nothing leaks. */
	return 0;
}

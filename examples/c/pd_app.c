/*
 * Copyright (c) 2019-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/**
 * The CP has sent us a command; act on it and return 0 to accept it (non-zero
 * makes libosdp answer the CP with a NAK).
 *
 * cmd is owned by libosdp and is valid only for the duration of this call. To
 * use it later, copy what you need out of it -- do not store the pointer.
 */
int pd_command_handler(void *arg, struct osdp_cmd *cmd)
{
	(void)(arg);

	printf("PD: CMD: %d\n", cmd->id);
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
 * dropped by osdp_pd_flush_events() (FLUSHED), or the context was torn down
 * with it still queued (ABORTED). This is where a heap-allocated event is
 * freed -- do it here and nowhere else.
 *
 * Note that OK means the reply carrying this event was handed to the
 * transport, not that the CP acknowledged it; OSDP has no PD-side delivery
 * receipt.
 *
 * Runs on the thread that called osdp_pd_refresh() (or flush/teardown), so
 * keep it short.
 */
static void pd_event_completion(void *arg, struct osdp_event *event,
				enum osdp_completion_status status)
{
	(void)(arg);

	printf("PD: event %d completed: %s\n", event->type,
	       completion_status_name(status));

	/* Last use of event -- ownership ends here. */
	free(event);
}

/**
 * Events are queued *by reference*: libosdp does not copy them. A submitted
 * event must stay alive and unmodified until its completion callback fires,
 * so it cannot live on the stack of a function that returns, and it cannot be
 * reused for a second submission while the first is still queued.
 *
 * Allocating per event and freeing in the completion callback is the simplest
 * correct pattern -- submit and forget. The event is delivered to the CP as
 * the reply to some future POLL.
 */
static int pd_submit_card_read(osdp_t *ctx, const uint8_t *card_data,
			       int nr_bits)
{
	struct osdp_event *event = calloc(1, sizeof(struct osdp_event));

	if (event == NULL) {
		return -1;
	}

	event->type = OSDP_EVENT_CARDREAD;
	event->cardread.reader_no = 0;
	event->cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
	event->cardread.direction = 0;
	event->cardread.length = nr_bits;
	memcpy(event->cardread.data, card_data, (nr_bits + 7) / 8);

	if (osdp_pd_submit_event(ctx, event)) {
		/* Rejected: the event was never queued, so no completion
		 * callback will fire for it. It is ours to free right now. */
		printf("PD: failed to submit card read event\n");
		free(event);
		return -1;
	}

	/* Accepted. The completion callback owns it from here -- do not touch
	 * event again, it may already have been freed. */
	return 0;
}

osdp_pd_info_t info_pd = {
	.address = 101,
	.baud_rate = 9600,
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
	 * plain text. Pass a key here, or OSDP_FLAG_INSTALL_MODE in .flags,
	 * to bring up OSDP-SC. */
	.scbk = NULL,
};

static struct osdp_channel pd_channel = {
	.data = NULL,
	.send = sample_pd_send_func,
	.recv = sample_pd_recv_func,
};

int main()
{
	osdp_t *ctx;

	osdp_logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

	ctx = osdp_pd_setup(&pd_channel, &info_pd);
	if (ctx == NULL) {
		printf("pd init failed!\n");
		return -1;
	}

	osdp_pd_set_command_callback(ctx, pd_command_handler, NULL);

	/* Register this before the first submission: libosdp refuses to accept
	 * an event with no completion callback, because the callback is the
	 * only way ownership of the event gets back to the application. */
	osdp_pd_set_event_completion_callback(ctx, pd_event_completion, NULL);

	while (1) {
		/* libosdp is not internally synchronized. If events are
		 * submitted from another thread (a card reader ISR handing off
		 * to a task, say), serialize that thread against this refresh
		 * with a lock of your own. */
		osdp_pd_refresh(ctx);

		// your application code; when a card is presented:
		if (0) {
			uint8_t card_data[] = { 0x01, 0x23, 0x45, 0x67 };

			pd_submit_card_read(ctx, card_data, 32);
		}
		// delay();
	}

	/* Never reached here, but on a real shutdown path teardown completes
	 * every still-queued event as ABORTED, so the callback above frees
	 * them and nothing leaks. */
	osdp_pd_teardown(ctx);
	return 0;
}

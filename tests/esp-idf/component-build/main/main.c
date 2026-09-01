/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Link check for the ESP-IDF component. Nothing here is executed; the point is
 * to reference enough of the public API that the linker must pull in the PD
 * role, the channel plumbing and the platform glue in esp-idf/src/osdp_esp.c.
 * A component that compiles but fails to link -- the failure mode the "-u
 * osdp_esp_platform_glue" hack in component.cmake exists to prevent -- would
 * otherwise reach the registry unnoticed.
 */

#include <stddef.h>
#include <stdint.h>

#include <osdp.h>

static int channel_send(void *data, uint8_t *buf, int len)
{
	(void)data;
	(void)buf;
	return len;
}

#ifndef OPT_OSDP_RX_ZERO_COPY
static int channel_recv(void *data, uint8_t *buf, int len)
{
	(void)data;
	(void)buf;
	(void)len;
	return 0;
}
#endif

static struct osdp_pd_cap pd_cap[] = {
	{
		.function_code = OSDP_PD_CAP_CONTACT_STATUS_MONITORING,
		.compliance_level = 1,
		.num_items = 1,
	},
	{ .function_code = OSDP_PD_CAP_SENTINEL },
};

void app_main(void)
{
	osdp_pd_info_t info = {
		.name = "pd-build-test",
		.baud_rate = 115200,
		.address = 101,
		.flags = 0,
		.id = {
			.version = 1,
			.model = 1,
			.vendor_code = 0x00030201,
			.serial_number = 0x01020304,
			.firmware_version = 0x0a0b0c0d,
		},
		.cap = pd_cap,
		.scbk = NULL,
	};
	struct osdp_channel channel = {
		.data = NULL,
		.send = channel_send,
#ifndef OPT_OSDP_RX_ZERO_COPY
		.recv = channel_recv,
#endif
		.flush = NULL,
		.close = NULL,
	};
	osdp_t *ctx;

	osdp_logger_init("osdp::pd", OSDP_LOG_INFO, NULL);

	ctx = osdp_pd_setup(&channel, &info);
	if (ctx == NULL) {
		return;
	}
	osdp_pd_refresh(ctx);
	osdp_pd_teardown(ctx);
}

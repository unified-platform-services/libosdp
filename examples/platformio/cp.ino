/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <osdp.hpp>

OSDP::ControlPanel cp;
osdp_pd_info_t pd_info[] = {
    {},
};
static struct osdp_channel cp_channel = {};

int serial1_send_func(void *data, uint8_t *buf, int len)
{
    (void)(data);

    int sent = 0;
    for (int i = 0; i < len; i++) {
        if (Serial1.write(buf[i])) {
            sent++;
        } else {
            break;
        }
    }
    return sent;
}

int serial1_recv_func(void *data, uint8_t *buf, int len)
{
    (void)(data);

    int read = 0;
    while (Serial1.available() && read < len) {
        buf[read] = Serial1.read();
        read++;
    }
    return read;
}

void init_cp_info()
{
    pd_info[0].name = "pd[101]";
    pd_info[0].baud_rate = 115200;
    pd_info[0].address = 101;
    pd_info[0].flags = 0;
    pd_info[0].id.version = 0;
    pd_info[0].id.model = 0;
    pd_info[0].id.vendor_code = 0;
    pd_info[0].id.serial_number = 0;
    pd_info[0].id.firmware_version = 0;
    pd_info[0].cap = nullptr;
    pd_info[0].scbk = nullptr;

    cp_channel.recv = serial1_recv_func;
    cp_channel.send = serial1_send_func;
}

int event_handler(void *data, int pd, struct osdp_event *event)
{
    (void)(data);
    (void)(pd);
    (void)(event);

    Serial.println("Received an event!");
    return 0;
}

/*
 * Commands are queued by reference: libosdp does not copy them, so a submitted
 * command must stay alive and unmodified until its completion callback fires.
 *
 * With no heap to allocate from, one static command plus an in-flight flag
 * honours that: the flag is set before submitting and cleared by the completion
 * callback, so the command is never refilled while libosdp still holds it.
 */
static struct osdp_cmd led_cmd;
static volatile bool led_cmd_in_flight;

/*
 * Ownership of led_cmd returns here, exactly once, whatever its fate: answered
 * by the PD (OK), failed (FAILED), dropped by flush_commands() (FLUSHED), or
 * still queued at teardown (ABORTED).
 *
 * Runs from inside cp.refresh(), so keep it short.
 */
void command_completion_handler(void *data, int pd, struct osdp_cmd *cmd,
                                enum osdp_completion_status status)
{
    (void)(data);
    (void)(pd);
    (void)(cmd);

    led_cmd_in_flight = false;

    if (status != OSDP_COMPLETION_OK) {
        Serial.println("LED command did not succeed");
    }
}

void setup()
{
    Serial.begin(115200);
    Serial1.begin(115200);

    cp.logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

    init_cp_info();
    cp.setup(&cp_channel, 1, pd_info);
    cp.set_event_callback(event_handler, nullptr);

    /* Register before the first submission: libosdp refuses to accept a
     * command with no completion callback, because the callback is the only
     * way ownership of the command gets back to the application. */
    cp.set_command_completion_callback(command_completion_handler, nullptr);
}

void loop()
{
    uint8_t online_mask = 0;

    /* Commands can only be submitted to a PD that is online, and led_cmd can
     * only be refilled once the previous submission has completed. */
    cp.get_status_mask(&online_mask);
    if ((online_mask & (1 << 0)) && !led_cmd_in_flight) {
        led_cmd.id = OSDP_CMD_LED;
        led_cmd.led.reader = 0;
        led_cmd.led.led_number = 0;
        led_cmd.led.temporary.control_code = OSDP_CMD_LED_TEMPORARY_CC_SET;
        led_cmd.led.temporary.on_count = 10;
        led_cmd.led.temporary.off_count = 10;
        led_cmd.led.temporary.on_color = OSDP_LED_COLOR_GREEN;
        led_cmd.led.temporary.off_color = OSDP_LED_COLOR_NONE;
        led_cmd.led.temporary.timer_count = 20;

        /* Set the flag before submitting: the completion callback can run
         * before submit_command() returns. */
        led_cmd_in_flight = true;
        if (cp.submit_command(0, &led_cmd)) {
            /* Never queued, so no completion callback will fire for it. */
            led_cmd_in_flight = false;
        }
    }

    /* OSDP timing requires a refresh at least once every 50ms. */
    cp.refresh();
    delay(50);
}

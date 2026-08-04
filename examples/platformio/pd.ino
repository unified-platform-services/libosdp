/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <osdp.hpp>

OSDP::PeripheralDevice pd;
osdp_pd_info_t info_pd = {};
static const struct osdp_pd_cap pd_cap[] = {
    { OSDP_PD_CAP_READER_LED_CONTROL, 1, 1 },
    { OSDP_PD_CAP_READER_AUDIBLE_OUTPUT, 1, 1 },
    { OSDP_PD_CAP_SENTINEL, 0, 0 }
};
static struct osdp_channel pd_channel = {};

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

void init_pd_info()
{
    info_pd.name = "pd[101]";
    info_pd.baud_rate = 9600;
    info_pd.address = 101;
    info_pd.flags = 0;
    info_pd.id.version = 1;
    info_pd.id.model = 153;
    info_pd.id.vendor_code = 31337;
    info_pd.id.serial_number = 0x01020304;
    info_pd.id.firmware_version = 0x0A0B0C0D;
    info_pd.cap = pd_cap;
    info_pd.scbk = nullptr;

    pd_channel.recv = serial1_recv_func;
    pd_channel.send = serial1_send_func;
}

int pd_command_handler(void *data, struct osdp_cmd *cmd)
{
    (void)(data);
    (void)(cmd);

    Serial.println("Received a command!");
    return 0;
}

/*
 * Events are queued by reference: libosdp does not copy them, so a submitted
 * event must stay alive and unmodified until its completion callback fires.
 *
 * With no heap to allocate from, one static event plus an in-flight flag
 * honours that: the flag is set before submitting and cleared by the completion
 * callback, so the event is never refilled while libosdp still holds it.
 */
static struct osdp_event card_event;
static volatile bool card_event_in_flight;

/*
 * Ownership of card_event returns here, exactly once, whatever its fate. OK
 * means the reply carrying it was handed to the transport, not that the CP
 * acknowledged it; OSDP gives the PD no delivery receipt.
 *
 * Runs from inside pd.refresh(), so keep it short.
 */
void event_completion_handler(void *data, struct osdp_event *event,
                              enum osdp_completion_status status)
{
    (void)(data);
    (void)(event);

    card_event_in_flight = false;

    if (status != OSDP_COMPLETION_OK) {
        Serial.println("card read did not go out");
    }
}

/* TODO (user): return true when the reader hardware has a card to report. */
bool card_available(uint8_t *card_data, int *nr_bits)
{
    (void)(card_data);
    (void)(nr_bits);

    return false;
}

void setup()
{
    Serial.begin(115200);
    Serial1.begin(115200);

    pd.logger_init("osdp::pd", OSDP_LOG_DEBUG, NULL);

    init_pd_info();
    pd.setup(&pd_channel, &info_pd);

    pd.set_command_callback(pd_command_handler, nullptr);

    /* Register before the first submission: libosdp refuses to accept an
     * event with no completion callback, because the callback is the only way
     * ownership of the event gets back to the application. */
    pd.set_event_completion_callback(event_completion_handler, nullptr);
}

void loop()
{
    uint8_t card_data[8];
    int nr_bits = 0;

    /* card_event can only be refilled once the previous submission has
     * completed. */
    if (!card_event_in_flight && card_available(card_data, &nr_bits)) {
        card_event.type = OSDP_EVENT_CARDREAD;
        card_event.cardread.reader_no = 0;
        card_event.cardread.format = OSDP_CARD_FMT_RAW_WIEGAND;
        card_event.cardread.direction = 0;
        card_event.cardread.length = nr_bits;
        memcpy(card_event.cardread.data, card_data, (nr_bits + 7) / 8);

        /* Set the flag before submitting: the completion callback can run
         * before submit_event() returns. */
        card_event_in_flight = true;
        if (pd.submit_event(&card_event)) {
            /* Never queued, so no completion callback will fire for it. */
            card_event_in_flight = false;
        }
    }

    /* OSDP timing requires a refresh at least once every 50ms. */
    pd.refresh();
    delay(50);
}

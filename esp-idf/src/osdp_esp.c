/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP-IDF platform glue for LibOSDP -- the counterpart of zephyr/src/osdp.c.
 *
 * ESP-IDF is a hosted newlib environment, so LibOSDP builds and runs against the
 * generic POSIX paths in utils/src/utils.c without any porting. What it does not
 * get for free are the two __weak hooks below: utils.c reaches for esp_random()
 * only under ARDUINO_ARCH_ESP32 (the Arduino core), which plain ESP-IDF never
 * defines, and its millis_now() goes through gettimeofday().
 *
 * Both strong definitions here only take effect because component.cmake forces
 * this translation unit into the link with `-u osdp_esp_platform_glue`; see the
 * comment there.
 */

#include <esp_random.h>
#include <esp_timer.h>

#include <utils/utils.h>

/*
 * esp_timer_get_time() counts microseconds since boot from a 64-bit timer that
 * survives light sleep, which is what OSDP timeouts need. The alternative,
 * xTaskGetTickCount(), is quantised to the FreeRTOS tick (10 ms by default) --
 * too coarse for OSDP_RESP_TOUT_MS and the inter-packet timing in osdp_phy.c.
 */
tick_t osdp_millis_now(void)
{
	return (tick_t)(esp_timer_get_time() / 1000);
}

/*
 * Only the TinyAES backend draws on this: crypto/tinyaes.c builds the secure
 * channel's random bytes out of rand_u32(). The MbedTLS backend uses MbedTLS's
 * own DRBG instead, so in that configuration nothing references this and the
 * linker drops it.
 *
 * It is still worth defining, because the difference between the two backends is
 * one Kconfig flip and the fallback in utils.c on ESP-IDF is libc rand() -- a
 * fixed sequence from a fixed seed, which is not a defensible source for
 * cryptographic material.
 *
 * esp_random() is a true hardware RNG only while an RF subsystem (Wi-Fi or
 * Bluetooth) is active, or while the SAR ADC entropy source has been enabled;
 * with both off it degrades to a pseudo-random sequence. An application running
 * secure channel over TinyAES without RF should call bootloader_random_enable()
 * before osdp_pd_setup() (and bootloader_random_disable() before it later starts
 * Wi-Fi/BT, which cannot share the entropy source). That is left to the
 * application because the right answer depends on whether it uses RF at all.
 */
uint32_t rand_u32(void)
{
	return esp_random();
}

/*
 * Link anchor. Referenced by nothing; exists so component.cmake's `-u` can name
 * a symbol unique to this object file and drag it into the link ahead of the
 * __weak definitions in osdp_common.c and utils.c.
 */
void osdp_esp_platform_glue(void)
{
}

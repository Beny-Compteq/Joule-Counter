/*
 * Status LED patterns.
 *
 *   blue, breathing   no host connected
 *   blue, steady      host connected, idle
 *   green, breathing  streaming samples
 *   red added         DUT output is powered
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include "leds.h"
#include "power.h"
#include "protocol.h"
#include "usb_ppk.h"

LOG_MODULE_REGISTER(leds, CONFIG_LOG_DEFAULT_LEVEL);

#define LED_RED		0
#define LED_GREEN	1
#define LED_BLUE	2

#define TICK_MS		40
#define BREATH_STEPS	50

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(pwmleds));
static bool ready;

static uint8_t breathe(uint32_t tick, uint8_t low, uint8_t high)
{
	uint32_t phase = tick % (2 * BREATH_STEPS);
	uint32_t up = phase < BREATH_STEPS ? phase : (2 * BREATH_STEPS - phase);

	return low + (uint8_t)((high - low) * up / BREATH_STEPS);
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	led_set_brightness(leds, LED_RED, r);
	led_set_brightness(leds, LED_GREEN, g);
	led_set_brightness(leds, LED_BLUE, b);
}

static void led_thread(void *p1, void *p2, void *p3)
{
	uint32_t tick = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!ready) {
		k_msleep(TICK_MS);
	}

	for (;;) {
		uint8_t r = power_output() ? 25 : 0;
		uint8_t g = 0;
		uint8_t b = 0;

		if (protocol_streaming()) {
			g = breathe(tick, 5, 60);
		} else if (usb_ppk_data_connected()) {
			b = 30;
		} else {
			b = breathe(tick, 2, 25);
		}

		set_rgb(r, g, b);
		tick++;
		k_msleep(TICK_MS);
	}
}

K_THREAD_DEFINE(leds_tid, 1024, led_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(14), 0, 0);

int leds_init(void)
{
	if (!device_is_ready(leds)) {
		LOG_ERR("PWM LEDs not ready");
		return -ENODEV;
	}

	set_rgb(20, 20, 20);
	ready = true;

	return 0;
}

/*
 * Firmware for the Nordic Power Profiler Kit II (PCA63100).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board_io.h"
#include "leds.h"
#include "metadata.h"
#include "power.h"
#include "protocol.h"
#include "sampling.h"
#include "usb_ppk.h"

LOG_MODULE_REGISTER(ppk2, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Power Profiler Kit II on %s", CONFIG_BOARD_TARGET);

	/* Order matters: the supply path must be safe before anything that
	 * could power the terminals, and the SAADC must be ready before the
	 * regulators are tuned.
	 */
	ret = board_io_init();
	if (ret < 0) {
		LOG_ERR("board_io_init: %d", ret);
		return 0;
	}

	metadata_init();
	sampling_init();

	ret = power_init();
	if (ret < 0) {
		LOG_WRN("power_init: %d", ret);
	}

	protocol_init();

	ret = usb_ppk_init();
	if (ret < 0) {
		LOG_ERR("usb_ppk_init: %d", ret);
	}

	leds_init();

	LOG_INF("ready: %s mode, %u mV, %s",
		power_mode() == 2 ? "source" : "ampere", power_vdd(),
		metadata_is_calibrated() ? "calibrated" : "uncalibrated");

	return 0;
}

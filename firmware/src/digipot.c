/*
 * MCP4451 quad digital potentiometer (U10).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "digipot.h"

LOG_MODULE_REGISTER(digipot, CONFIG_LOG_DEFAULT_LEVEL);

static const struct i2c_dt_spec pot = I2C_DT_SPEC_GET(DT_NODELABEL(digipot));

/* Volatile wiper register addresses, MCP4451 datasheet table 4-1. */
static const uint8_t wiper_reg[4] = { 0x00, 0x01, 0x06, 0x07 };

static uint16_t last_code[4];

int digipot_init(void)
{
	if (!i2c_is_ready_dt(&pot)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	return 0;
}

int digipot_set(enum digipot_wiper wiper, uint16_t code)
{
	if (wiper > DIGIPOT_IA_OFFSET) {
		return -EINVAL;
	}

	if (code > DIGIPOT_CODE_MAX) {
		code = DIGIPOT_CODE_MAX;
	}

	/* Command byte: AD3..AD0, C1 C0 = 00 (write), D9 D8. Then D7..D0. */
	const uint8_t buf[2] = {
		(uint8_t)((wiper_reg[wiper] << 4) | ((code >> 8) & 0x03)),
		(uint8_t)(code & 0xFF),
	};

	int ret = i2c_write_dt(&pot, buf, sizeof(buf));

	if (ret == 0) {
		last_code[wiper] = code;
	} else {
		LOG_WRN("wiper %d write failed: %d", wiper, ret);
	}

	return ret;
}

uint16_t digipot_get(enum digipot_wiper wiper)
{
	return wiper <= DIGIPOT_IA_OFFSET ? last_code[wiper] : 0;
}

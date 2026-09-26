/*
 * Control and status lines of the PPK2 front end.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "board_io.h"
#include "ppk2.h"

LOG_MODULE_REGISTER(board_io, CONFIG_LOG_DEFAULT_LEVEL);

#define CTRL_NODE DT_NODELABEL(ppk2_control)

static const struct gpio_dt_spec ctrl[BOARD_CTRL_COUNT] = {
	[BOARD_CTRL_VOUT_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, vout_en_gpios),
	[BOARD_CTRL_VLDO_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, vldo_en_gpios),
	[BOARD_CTRL_VEXT_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, vext_en_gpios),
	[BOARD_CTRL_REG_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, reg_en_gpios),
	[BOARD_CTRL_ANA_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, ana_en_gpios),
	[BOARD_CTRL_LP_EN] = GPIO_DT_SPEC_GET(CTRL_NODE, lp_en_gpios),
};

static const struct gpio_dt_spec cal[BOARD_CAL_COUNT] = {
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, cal_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, cal_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, cal_gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, cal_gpios, 3),
};

static const struct gpio_dt_spec sw[4] = {
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, sw_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, sw_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, sw_gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, sw_gpios, 3),
};

static const struct gpio_dt_spec lp_data[8] = {
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 3),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 4),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 5),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 6),
	GPIO_DT_SPEC_GET_BY_IDX(CTRL_NODE, lp_data_gpios, 7),
};

static const struct gpio_dt_spec ext_usb = GPIO_DT_SPEC_GET(CTRL_NODE, ext_usb_gpios);

/*
 * The sampling interrupt reads GPIO port registers directly rather than
 * going through the driver, so the pin numbers it assumes are pinned to the
 * devicetree here.
 */
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, sw_gpios, 0) == 0);
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, sw_gpios, 1) == 1);
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, sw_gpios, 2) == 26);
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, sw_gpios, 3) == 27);
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, lp_data_gpios, 0) == BOARD_P0_LOGIC_SHIFT);
BUILD_ASSERT(DT_GPIO_PIN_BY_IDX(CTRL_NODE, lp_data_gpios, 7) == BOARD_P0_LOGIC_SHIFT + 7);
BUILD_ASSERT(DT_GPIO_PIN(CTRL_NODE, ext_usb_gpios) == BOARD_P1_EXT_USB_PIN);

/*
 * The comparators close the bypass switches in order as the current rises:
 * SW1 first, then SW2 on top of it, and so on. Any other pattern is a
 * snapshot taken mid-transition.
 */
static const uint8_t range_lut[16] = {
	[0x0] = 0, [0x1] = 1, [0x3] = 2, [0x7] = 3, [0xF] = 4,
	[0x2] = PPK2_RANGE_SWITCHING, [0x4] = PPK2_RANGE_SWITCHING,
	[0x5] = PPK2_RANGE_SWITCHING, [0x6] = PPK2_RANGE_SWITCHING,
	[0x8] = PPK2_RANGE_SWITCHING, [0x9] = PPK2_RANGE_SWITCHING,
	[0xA] = PPK2_RANGE_SWITCHING, [0xB] = PPK2_RANGE_SWITCHING,
	[0xC] = PPK2_RANGE_SWITCHING, [0xD] = PPK2_RANGE_SWITCHING,
	[0xE] = PPK2_RANGE_SWITCHING,
};

static int configure_all(const struct gpio_dt_spec *specs, size_t n, gpio_flags_t flags)
{
	for (size_t i = 0; i < n; i++) {
		if (!gpio_is_ready_dt(&specs[i])) {
			return -ENODEV;
		}

		int ret = gpio_pin_configure_dt(&specs[i], flags);

		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

int board_io_init(void)
{
	int ret;

	/* Everything that can put voltage on the terminals starts off. */
	ret = configure_all(ctrl, ARRAY_SIZE(ctrl), GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("control lines: %d", ret);
		return ret;
	}

	ret = configure_all(cal, ARRAY_SIZE(cal), GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("calibration loads: %d", ret);
		return ret;
	}

	ret = configure_all(sw, ARRAY_SIZE(sw), GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("range inputs: %d", ret);
		return ret;
	}

	ret = configure_all(lp_data, ARRAY_SIZE(lp_data), GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("logic port: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&ext_usb, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	/* The analog rails and the logic-port translator are always wanted. */
	board_io_set(BOARD_CTRL_ANA_EN, true);
	board_io_set(BOARD_CTRL_LP_EN, true);

	return 0;
}

int board_io_set(enum board_ctrl line, bool on)
{
	if (line >= BOARD_CTRL_COUNT) {
		return -EINVAL;
	}

	return gpio_pin_set_dt(&ctrl[line], on ? 1 : 0);
}

bool board_io_get(enum board_ctrl line)
{
	if (line >= BOARD_CTRL_COUNT) {
		return false;
	}

	return gpio_pin_get_dt(&ctrl[line]) > 0;
}

int board_io_set_cal_load(enum board_cal_load load)
{
	/* Never two loads at once: open the active one before closing another. */
	for (int i = 0; i < BOARD_CAL_COUNT; i++) {
		if (i != load) {
			gpio_pin_set_dt(&cal[i], 0);
		}
	}

	if (load >= 0 && load < BOARD_CAL_COUNT) {
		return gpio_pin_set_dt(&cal[load], 1);
	}

	return 0;
}

uint8_t board_io_decode_range(uint32_t sw_bits)
{
	return range_lut[sw_bits & 0xF];
}

uint8_t board_io_read_range(void)
{
	uint32_t bits = 0;

	for (int i = 0; i < 4; i++) {
		if (gpio_pin_get_dt(&sw[i]) > 0) {
			bits |= BIT(i);
		}
	}

	return board_io_decode_range(bits);
}

bool board_io_ext_usb_present(void)
{
	return gpio_pin_get_dt(&ext_usb) > 0;
}

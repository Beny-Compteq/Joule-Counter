/*
 * Control and status lines of the PPK2 front end.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BOARD_IO_H_
#define BOARD_IO_H_

#include <stdbool.h>
#include <stdint.h>

enum board_ctrl {
	BOARD_CTRL_VOUT_EN,
	BOARD_CTRL_VLDO_EN,
	BOARD_CTRL_VEXT_EN,
	BOARD_CTRL_REG_EN,
	BOARD_CTRL_ANA_EN,
	BOARD_CTRL_LP_EN,
	BOARD_CTRL_COUNT,
};

enum board_cal_load {
	BOARD_CAL_OFF = -1,
	BOARD_CAL_100K = 0,
	BOARD_CAL_10K,
	BOARD_CAL_1K,
	BOARD_CAL_100,
	BOARD_CAL_COUNT,
};

/* Bit positions in GPIO port 0 used by the sampling interrupt. */
#define BOARD_P0_SW_MASK	(BIT(0) | BIT(1) | BIT(26) | BIT(27))
#define BOARD_P0_LOGIC_SHIFT	10U
#define BOARD_P1_EXT_USB_PIN	9U

int board_io_init(void);

int board_io_set(enum board_ctrl line, bool on);
bool board_io_get(enum board_ctrl line);

/* Switches one calibration load across VDUT+/VDUT-, or none. */
int board_io_set_cal_load(enum board_cal_load load);

/* Current range as the front end reports it: 0..4, or PPK2_RANGE_SWITCHING. */
uint8_t board_io_read_range(void);

/* Decodes the four SW status bits (SW1 in bit 0) into a range. */
uint8_t board_io_decode_range(uint32_t sw_bits);

bool board_io_ext_usb_present(void);

#endif /* BOARD_IO_H_ */

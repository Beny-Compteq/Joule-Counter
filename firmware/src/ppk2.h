/*
 * Definitions shared across the PPK2 firmware.
 *
 * The wire format here is shared with the Joule-Counter desktop app
 * (Joule-Counter/src/device/serialDevice.ts); change both together.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PPK2_H_
#define PPK2_H_

#include <stdint.h>

/*
 * The two-channel scan itself still converts at 100 kHz, but two words per
 * sample is 800 kB/s and a full-speed CDC link on a real host carried about
 * 530 kB/s sustained. 50 kHz is the 400 kB/s the one-word stream already
 * proved, with margin, and doubles the per-scan acquisition budget.
 */
#define PPK2_SAMPLE_RATE_HZ	50000U
#define PPK2_BLOCK_SAMPLES	512U
#define PPK2_RANGE_COUNT	5U

/* Reported in the range field while the switches are between two ranges. */
#define PPK2_RANGE_SWITCHING	7U

/*
 * Each sample is two 32-bit little-endian words: the current word first,
 * then the voltage word, from the same SAADC scan (one END covers both, so
 * the pair is always from the same 10 us-ish instant). This is Joule
 * Counter's own wire format, no longer PPK2 wire-compatible.
 *
 * Current word:
 *   bits  0..13  ADC value. The 14-bit differential SAADC result shifted
 *                right by two, clamped at zero. The app multiplies it back
 *                by four, so the calibration constants in the EEPROM are in
 *                raw 14-bit counts.
 *   bits 14..16  measurement range 0..4, or 7 while switching.
 *   bit  17      auxiliary USB connector (J1) VBUS present.
 *   bits 18..23  six-bit sample counter, contiguous over emitted samples.
 *   bits 24..31  logic port D0..D7.
 *
 * Voltage word:
 *   bits  0..13  ADC value, same convention as the current word (VDUT vs
 *                AGND, uncalibrated linear divider/gain math on the host).
 *   bits 14..31  reserved, always zero.
 */
#define PPK2_WORDS_PER_SAMPLE	2U

#define PPK2_SAMPLE_ADC_MASK	0x3FFFU
#define PPK2_SAMPLE_RANGE_POS	14U
#define PPK2_SAMPLE_EXT_USB_POS	17U
#define PPK2_SAMPLE_COUNTER_POS	18U
#define PPK2_SAMPLE_COUNTER_MASK 0x3FU
#define PPK2_SAMPLE_LOGIC_POS	24U

#define PPK2_VOLTAGE_ADC_MASK	0x3FFFU

enum ppk2_mode {
	PPK2_MODE_AMPERE = 1,
	PPK2_MODE_SOURCE = 2,
};

/* Commands the host may send on the data port, one opcode byte each. */
enum ppk2_cmd {
	PPK2_CMD_TRIGGER_SET		= 0x01,
	PPK2_CMD_AVG_NUM_SET		= 0x02,
	PPK2_CMD_TRIGGER_WINDOW_SET	= 0x03,
	PPK2_CMD_TRIGGER_INTERVAL_SET	= 0x04,
	PPK2_CMD_TRIGGER_SINGLE_SET	= 0x05,
	PPK2_CMD_AVERAGE_START		= 0x06,
	PPK2_CMD_AVERAGE_STOP		= 0x07,
	PPK2_CMD_RANGE_SET		= 0x08,
	PPK2_CMD_LCD_SET		= 0x09,
	PPK2_CMD_TRIGGER_STOP		= 0x0A,
	PPK2_CMD_DEVICE_RUNNING_SET	= 0x0C,
	PPK2_CMD_REGULATOR_SET		= 0x0D,
	PPK2_CMD_SWITCH_POINT_DOWN	= 0x0E,
	PPK2_CMD_SWITCH_POINT_UP	= 0x0F,
	PPK2_CMD_SET_POWER_MODE		= 0x11,
	PPK2_CMD_RES_USER_SET		= 0x12,
	PPK2_CMD_SPIKE_FILTERING_ON	= 0x15,
	PPK2_CMD_SPIKE_FILTERING_OFF	= 0x16,
	PPK2_CMD_GET_METADATA		= 0x19,
	PPK2_CMD_RESET			= 0x20,
	PPK2_CMD_SET_USER_GAINS		= 0x25,
};

#endif /* PPK2_H_ */

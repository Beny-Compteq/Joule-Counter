/*
 * Supply path: ampere-meter vs source-meter mode, output voltage, DUT power.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POWER_H_
#define POWER_H_

#include <stdbool.h>
#include <stdint.h>

#define POWER_VDD_MIN_MV	800
#define POWER_VDD_MAX_MV	5000

struct power_status {
	uint8_t mode;
	uint16_t vdd_mv;
	bool output_on;
	bool retune_pending;
	uint16_t ia_code;
	uint16_t ldo_code;
	uint16_t bb_code;
	float vldo_mv;
	float vbb_mv;
	float vin_mv;
	float vdut_mv;
	bool ext_usb;
};

int power_init(void);

int power_set_mode(uint8_t mode);
int power_set_vdd(uint16_t mv);
int power_set_output(bool on);

uint8_t power_mode(void);
uint16_t power_vdd(void);
bool power_output(void);

/* To be called when the sample stream stops: finishes deferred regulator work. */
void power_on_stream_stopped(void);

int power_set_ia_code(uint16_t code);
/* Adjusts the IA offset wiper until the zero-current reading hits target. */
int power_trim_ia(int32_t target_raw, uint16_t *code_out);

/* With measure set, also reads the voltage monitors (stream must be stopped). */
int power_get_status(struct power_status *st, bool measure);

#endif /* POWER_H_ */

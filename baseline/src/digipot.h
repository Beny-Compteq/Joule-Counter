/*
 * MCP4451 quad digital potentiometer (U10).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DIGIPOT_H_
#define DIGIPOT_H_

#include <stdint.h>

/* Wiper assignments follow the POT0..POT3 nets on the schematic. */
enum digipot_wiper {
	DIGIPOT_VBB = 0,	/* W0: ADP2504 buck-boost feedback, via R112 */
	DIGIPOT_VLDO = 1,	/* W1: ADP1708 LDO feedback, via R14 */
	DIGIPOT_UNUSED = 2,	/* W2: test point only */
	DIGIPOT_IA_OFFSET = 3,	/* W3: instrumentation amplifier offset */
};

#define DIGIPOT_CODE_MAX 256U

int digipot_init(void);
int digipot_set(enum digipot_wiper wiper, uint16_t code);
uint16_t digipot_get(enum digipot_wiper wiper);

#endif /* DIGIPOT_H_ */

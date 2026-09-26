/*
 * PPK2 host protocol on the USB data port.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>

struct protocol_stats {
	uint32_t commands;
	uint32_t unknown_bytes;
	uint32_t tx_failures;
};

int protocol_init(void);

bool protocol_streaming(void);
void protocol_get_stats(struct protocol_stats *st);

#endif /* PROTOCOL_H_ */

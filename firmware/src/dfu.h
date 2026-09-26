/*
 * Hand-over to the factory nRF5 bootloader.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DFU_H_
#define DFU_H_

#include <stdint.h>

/* Layout of the reply to the DFU trigger's DFU_INFO request. */
struct dfu_info {
	uint32_t address;
	uint32_t firmware_size;
	uint16_t version_major;
	uint16_t version_minor;
	uint32_t firmware_id;
	uint32_t flash_size;
	uint32_t flash_page_size;
} __packed;

void dfu_get_info(struct dfu_info *info);

/* Reboots into the bootloader after a short delay so USB can finish up. */
void dfu_enter_bootloader(void);

#endif /* DFU_H_ */

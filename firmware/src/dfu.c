/*
 * Hand-over to the factory nRF5 bootloader.
 *
 * The bootloader starts DFU mode when it finds BOOTLOADER_DFU_START in
 * GPREGRET after a reset. That is also what the desktop app's firmware update
 * relies on, through the DFU trigger USB interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <hal/nrf_power.h>

#include "dfu.h"

LOG_MODULE_REGISTER(dfu, CONFIG_LOG_DEFAULT_LEVEL);

#define BOOTLOADER_DFU_START	0xB1
#define NRF52840_FLASH_SIZE	(1024U * 1024U)
#define NRF52840_FLASH_PAGE	4096U

extern char __rom_region_start[];
/* Exact image size, as the linker computes it; the region end is page-rounded. */
extern char _flash_used[];

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	nrf_power_gpregret_set(NRF_POWER, 0, BOOTLOADER_DFU_START);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

void dfu_get_info(struct dfu_info *info)
{
	info->address = (uint32_t)__rom_region_start;
	info->firmware_size = (uint32_t)_flash_used;
	info->version_major = CONFIG_PPK2_FW_VERSION_MAJOR;
	info->version_minor = CONFIG_PPK2_FW_VERSION_MINOR;
	info->firmware_id = 0;
	info->flash_size = NRF52840_FLASH_SIZE;
	info->flash_page_size = NRF52840_FLASH_PAGE;
}

void dfu_enter_bootloader(void)
{
	LOG_INF("rebooting into bootloader");
	k_work_schedule(&reboot_work, K_MSEC(100));
}

/*
 * PPK2 host protocol on the USB data port.
 *
 * Commands arrive as an opcode byte followed by a fixed number of argument
 * bytes. Replies are either the metadata text (terminated by "END") or the
 * binary sample stream, never both at once.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include "metadata.h"
#include "power.h"
#include "ppk2.h"
#include "protocol.h"
#include "sampling.h"
#include "usb_ppk.h"

LOG_MODULE_REGISTER(protocol, CONFIG_LOG_DEFAULT_LEVEL);

#define CMD_MAX_ARGS		5
#define CMD_QUEUE_DEPTH		16
#define CMD_INTERNAL_LINK	0xFE	/* data port connected / disconnected */

#define TX_BLOCK_TIMEOUT	K_MSEC(250)
#define TX_TEXT_TIMEOUT		K_MSEC(500)

struct cmd {
	uint8_t op;
	uint8_t len;
	uint8_t arg[CMD_MAX_ARGS];
};

/* Argument byte count per opcode; -1 marks opcodes this firmware does not know. */
static const int8_t cmd_arg_len[0x26] = {
	[0 ... 0x25] = -1,
	[PPK2_CMD_TRIGGER_SET] = 2,
	[PPK2_CMD_AVG_NUM_SET] = 1,
	[PPK2_CMD_TRIGGER_WINDOW_SET] = 2,
	[PPK2_CMD_TRIGGER_INTERVAL_SET] = 1,
	[PPK2_CMD_TRIGGER_SINGLE_SET] = 0,
	[PPK2_CMD_AVERAGE_START] = 0,
	[PPK2_CMD_AVERAGE_STOP] = 0,
	[PPK2_CMD_RANGE_SET] = 1,
	[PPK2_CMD_LCD_SET] = 1,
	[PPK2_CMD_TRIGGER_STOP] = 0,
	[PPK2_CMD_DEVICE_RUNNING_SET] = 1,
	[PPK2_CMD_REGULATOR_SET] = 2,
	[PPK2_CMD_SWITCH_POINT_DOWN] = 1,
	[PPK2_CMD_SWITCH_POINT_UP] = 1,
	[PPK2_CMD_SET_POWER_MODE] = 1,
	[PPK2_CMD_RES_USER_SET] = 3,
	[PPK2_CMD_SPIKE_FILTERING_ON] = 0,
	[PPK2_CMD_SPIKE_FILTERING_OFF] = 0,
	[PPK2_CMD_GET_METADATA] = 0,
	[PPK2_CMD_RESET] = 0,
	[PPK2_CMD_SET_USER_GAINS] = 5,
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd), CMD_QUEUE_DEPTH, 4);

/* Serialises everything that goes out on the data port. */
static K_MUTEX_DEFINE(tx_lock);

static volatile bool streaming;
static struct protocol_stats stats;

/* Parser state, touched only from the USB receive context. */
static struct cmd partial;
static bool partial_active;

static void rx_handler(const uint8_t *data, size_t len)
{
	for (size_t k = 0; k < len; k++) {
		uint8_t b = data[k];

		if (!partial_active) {
			int8_t n = b < ARRAY_SIZE(cmd_arg_len) ? cmd_arg_len[b] : -1;

			if (n < 0) {
				stats.unknown_bytes++;
				continue;
			}

			partial.op = b;
			partial.len = 0;
			partial_active = true;
			if (n > 0) {
				continue;
			}
		} else {
			partial.arg[partial.len++] = b;
			if (partial.len < cmd_arg_len[partial.op]) {
				continue;
			}
		}

		partial_active = false;
		if (k_msgq_put(&cmd_q, &partial, K_NO_WAIT) != 0) {
			LOG_WRN("command queue full, dropped 0x%02x", partial.op);
		}
	}
}

static void link_handler(bool connected)
{
	struct cmd c = { .op = CMD_INTERNAL_LINK, .len = 1, .arg = { connected } };

	k_msgq_put(&cmd_q, &c, K_NO_WAIT);
}

static void stream_start(void)
{
	if (streaming) {
		return;
	}

	int ret = sampling_start();

	if (ret < 0) {
		LOG_ERR("cannot start sampling: %d", ret);
		return;
	}

	streaming = true;
	LOG_INF("streaming");
}

static void stream_stop(void)
{
	if (!streaming) {
		return;
	}

	streaming = false;
	sampling_stop();
	power_on_stream_stopped();
	LOG_INF("stopped");
}

static void send_text(const char *text, size_t len)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	while (len > 0) {
		size_t chunk = MIN(len, 1024U);
		int ret = usb_ppk_data_send((const uint8_t *)text, chunk, TX_TEXT_TIMEOUT);

		if (ret != 0) {
			stats.tx_failures++;
			LOG_WRN("metadata send failed: %d", ret);
			break;
		}
		text += chunk;
		len -= chunk;
	}
	k_mutex_unlock(&tx_lock);
}

static void handle_get_metadata(void)
{
	static char text[METADATA_TEXT_SIZE];

	/* The host expects only text until it has seen END. */
	stream_stop();

	size_t len = metadata_format(text, sizeof(text));

	if (len == 0) {
		LOG_ERR("metadata does not fit its buffer");
		return;
	}

	send_text(text, len);
}

static void handle_set_user_gains(const uint8_t *arg)
{
	uint8_t range = arg[0];
	float gain;

	/* The app serialises the gain as a native little-endian float32. */
	memcpy(&gain, &arg[1], sizeof(gain));

	if (range >= PPK2_RANGE_COUNT || !isfinite(gain) || gain <= 0.0f) {
		LOG_WRN("rejected user gain %u", range);
		return;
	}

	metadata_get()->ug[range] = gain;
	metadata_save_deferred();
}

static void dispatch(const struct cmd *c)
{
	stats.commands++;

	switch (c->op) {
	case CMD_INTERNAL_LINK:
		if (!c->arg[0]) {
			/* Host went away: stop streaming. The DUT output is left
			 * as it was; the app turns it off itself on a clean
			 * close, and a USB hiccup should not cut the DUT's power.
			 */
			stream_stop();
		}
		break;

	case PPK2_CMD_AVERAGE_START:
		stream_start();
		break;

	case PPK2_CMD_AVERAGE_STOP:
		stream_stop();
		break;

	case PPK2_CMD_DEVICE_RUNNING_SET:
		power_set_output(c->arg[0] != 0);
		break;

	case PPK2_CMD_REGULATOR_SET:
		power_set_vdd((uint16_t)((c->arg[0] << 8) | c->arg[1]));
		break;

	case PPK2_CMD_SET_POWER_MODE:
		power_set_mode(c->arg[0]);
		break;

	case PPK2_CMD_GET_METADATA:
		handle_get_metadata();
		break;

	case PPK2_CMD_SET_USER_GAINS:
		handle_set_user_gains(c->arg);
		break;

	case PPK2_CMD_RESET:
		stream_stop();
		power_set_output(false);
		k_sleep(K_MSEC(100));
		sys_reboot(SYS_REBOOT_COLD);
		break;

	default:
		/* PPK1-era commands: accepted for compatibility, no effect. */
		break;
	}
}

static void command_thread(void *p1, void *p2, void *p3)
{
	struct cmd c;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		if (k_msgq_get(&cmd_q, &c, K_FOREVER) == 0) {
			dispatch(&c);
		}
	}
}

/*
 * Moves complete blocks from the sample ring to USB. After a stop it also
 * flushes whatever partial block is left so the host sees every sample taken.
 */
static void pump_thread(void *p1, void *p2, void *p3)
{
	static uint32_t block[PPK2_BLOCK_SAMPLES];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		sampling_wait_block(K_MSEC(200));

		for (;;) {
			size_t avail = sampling_available();
			bool running = sampling_running();

			if (avail == 0 || (avail < PPK2_BLOCK_SAMPLES && running)) {
				break;
			}

			size_t n = sampling_take(block, MIN(avail, PPK2_BLOCK_SAMPLES));

			if (n == 0) {
				break;
			}

			if (!streaming) {
				continue;
			}

			k_mutex_lock(&tx_lock, K_FOREVER);
			int ret = usb_ppk_data_send((const uint8_t *)block, n * sizeof(uint32_t),
						    TX_BLOCK_TIMEOUT);

			k_mutex_unlock(&tx_lock);

			if (ret == 0) {
				sampling_count_block_sent();
			} else {
				stats.tx_failures++;
			}
		}
	}
}

K_THREAD_DEFINE(protocol_cmd_tid, 2048, command_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(6), 0, 0);
K_THREAD_DEFINE(protocol_pump_tid, 2048, pump_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(2), 0, 0);

int protocol_init(void)
{
	usb_ppk_set_callbacks(rx_handler, link_handler);
	return 0;
}

bool protocol_streaming(void)
{
	return streaming;
}

void protocol_get_stats(struct protocol_stats *st)
{
	*st = stats;
}

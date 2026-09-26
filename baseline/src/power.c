/*
 * Supply path: ampere-meter vs source-meter mode, output voltage, DUT power.
 *
 * Source mode feeds the DUT from the ADP1708 LDO, itself fed by the ADP2504
 * buck-boost. Both regulators take their feedback through wipers of the
 * MCP4451, so the output voltage is set by walking the wiper while reading
 * the regulator output back through the SAADC. That readback needs the SAADC,
 * which the sample stream owns while it runs; a voltage change requested
 * mid-stream is applied from the last learned slope and refined once the
 * stream stops.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board_io.h"
#include "digipot.h"
#include "metadata.h"
#include "power.h"
#include "sampling.h"

LOG_MODULE_REGISTER(power, CONFIG_LOG_DEFAULT_LEVEL);

/* Headroom the LDO needs above its output, and the buck-boost's own range. */
#define BB_HEADROOM_MV		500
#define BB_MIN_MV		2500
#define BB_MAX_MV		5500

#define TUNE_TOLERANCE_MV	8.0f
#define TUNE_MAX_STEPS		24
#define TUNE_MAX_STRIDE		32

/* Wiper codes the stock firmware programs at power-up. */
#define LDO_DEFAULT_CODE	143
#define BB_DEFAULT_CODE		102

struct regulator {
	enum digipot_wiper wiper;
	enum sampling_channel monitor;
	uint16_t code;
	float measured_mv;
	/* mV per wiper step. Positive: a higher code raises the output. Seen on
	 * hardware: the buck-boost bottoms out near 2.8 V at code 3, and the
	 * stock defaults of 102/143 sit around 3.5 V.
	 */
	float slope;
	bool measured;
	uint32_t settle_ms;
};

static struct regulator ldo = {
	.wiper = DIGIPOT_VLDO,
	.monitor = SAMPLING_CH_VLDO,
	.code = LDO_DEFAULT_CODE,
	.slope = 16.0f,
	.settle_ms = 4,
};

static struct regulator bb = {
	.wiper = DIGIPOT_VBB,
	.monitor = SAMPLING_CH_VBB,
	.code = BB_DEFAULT_CODE,
	.slope = 12.0f,
	.settle_ms = 8,
};

/* An LDO asked for more than its input can give just follows the input, and
 * then no wiper step changes the reading. Treat "within this of the supply"
 * as dropout and walk down without waiting for a slope estimate.
 */
#define DROPOUT_MARGIN_MV	80.0f

static uint8_t mode = PPK2_MODE_AMPERE;
static uint16_t vdd_mv = CONFIG_PPK2_DEFAULT_VDD_MV;
static bool output_on;
static bool retune_pending;
static uint16_t ia_code;
static K_MUTEX_DEFINE(lock);

static uint16_t clamp_code(int32_t code)
{
	return (uint16_t)CLAMP(code, 0, (int32_t)DIGIPOT_CODE_MAX);
}

static int regulator_write(struct regulator *reg, uint16_t code)
{
	int ret = digipot_set(reg->wiper, code);

	if (ret == 0) {
		reg->code = code;
	}

	return ret;
}

#define SETTLE_STABLE_MV	6.0f
#define SETTLE_MAX_READS	12

/*
 * Reads the monitor until two consecutive readings agree. With no load the
 * regulator output caps only drain through the feedback dividers, hundreds
 * of kOhm, so a downward step can take far longer than any fixed delay; a
 * stale reading here is what previously walked the tuner off an end stop.
 */
static int regulator_measure(struct regulator *reg)
{
	float prev = 0.0f;
	float mv = 0.0f;
	int ret;

	for (int n = 0; n < SETTLE_MAX_READS; n++) {
		ret = sampling_measure_mv(reg->monitor, &mv);
		if (ret < 0) {
			return ret;
		}
		if (n > 0 && fabsf(mv - prev) <= SETTLE_STABLE_MV) {
			break;
		}
		prev = mv;
		k_msleep(reg->settle_ms);
	}

	reg->measured_mv = mv;
	reg->measured = true;

	return 0;
}

/*
 * A defined load while tuning: the 1 kOhm calibration resistor runs from
 * VDUT- to ground, so with the LDO switched onto VDUT+ it draws a couple of
 * milliamps through the shunts and lets the output caps follow the wiper
 * within milliseconds. Only while the DUT output switch is open, so nothing
 * connected to the terminals sees it.
 */
static void tune_load(bool on)
{
	if (output_on) {
		return;
	}

	board_io_set_cal_load(on ? BOARD_CAL_1K : BOARD_CAL_OFF);
	if (on) {
		k_msleep(2);
	}
}

/*
 * Walks the wiper towards target, re-estimating the local slope from each
 * pair of readings. Steps are bounded so a DUT already connected sees a ramp
 * rather than a jump.
 */
static int regulator_tune(struct regulator *reg, float target_mv, float supply_mv)
{
	int ret;

	ret = regulator_measure(reg);
	if (ret < 0) {
		return ret;
	}

	for (int step = 0; step < TUNE_MAX_STEPS; step++) {
		float err = target_mv - reg->measured_mv;

		if (fabsf(err) <= TUNE_TOLERANCE_MV) {
			return 0;
		}

		int32_t delta;
		bool in_dropout = supply_mv > 0.0f &&
				  reg->measured_mv > supply_mv - DROPOUT_MARGIN_MV &&
				  err < 0.0f;

		if (in_dropout) {
			delta = -TUNE_MAX_STRIDE;
		} else {
			delta = (int32_t)lroundf(err / reg->slope);
			delta = CLAMP(delta, -TUNE_MAX_STRIDE, TUNE_MAX_STRIDE);
			if (delta == 0) {
				delta = (err / reg->slope) > 0 ? 1 : -1;
			}
		}

		uint16_t next = clamp_code((int32_t)reg->code + delta);

		if (next == reg->code) {
			/* Pinned at an end stop: the target is out of reach. */
			return -ERANGE;
		}

		uint16_t prev_code = reg->code;
		float prev_mv = reg->measured_mv;

		ret = regulator_write(reg, next);
		if (ret < 0) {
			return ret;
		}
		k_msleep(reg->settle_ms);

		ret = regulator_measure(reg);
		if (ret < 0) {
			return ret;
		}

		float dmv = reg->measured_mv - prev_mv;
		int32_t dcode = (int32_t)reg->code - (int32_t)prev_code;

		/* Only believe a slope estimate the noise cannot have made up. */
		if (dcode != 0 && fabsf(dmv) > 2.0f) {
			float slope = dmv / (float)dcode;

			if (fabsf(slope) >= 0.5f && fabsf(slope) <= 200.0f) {
				reg->slope = 0.5f * reg->slope + 0.5f * slope;
			}
		}
	}

	return -ETIMEDOUT;
}

/* Open-loop step from the last known point, for use while the SAADC is busy. */
static int regulator_predict(struct regulator *reg, float target_mv)
{
	if (!reg->measured) {
		return -ENODATA;
	}

	int32_t delta = (int32_t)lroundf((target_mv - reg->measured_mv) / reg->slope);
	uint16_t next = clamp_code((int32_t)reg->code + delta);
	int ret = regulator_write(reg, next);

	if (ret == 0) {
		/* Assume we landed; the retune after the stream corrects it. */
		reg->measured_mv = target_mv;
	}

	return ret;
}

static float bb_target_for(uint16_t vdd)
{
	return (float)CLAMP((int)vdd + BB_HEADROOM_MV, BB_MIN_MV, BB_MAX_MV);
}

static int apply_vdd(bool can_measure)
{
	float ldo_target = (float)vdd_mv;
	float bb_target = bb_target_for(vdd_mv);
	int ret;

	if (!can_measure) {
		retune_pending = true;
		regulator_predict(&bb, bb_target);
		return regulator_predict(&ldo, ldo_target);
	}

	/* Raise the pre-regulator first, lower it last, so the LDO never runs
	 * out of headroom on the way. The LDO has to be switched onto VDUT+ for
	 * the tuning load to reach it.
	 */
	bool raising = !ldo.measured || ldo_target > ldo.measured_mv;
	bool ldo_was_connected = board_io_get(BOARD_CTRL_VLDO_EN);

	board_io_set(BOARD_CTRL_VLDO_EN, true);
	tune_load(true);

	if (raising) {
		ret = regulator_tune(&bb, bb_target, 0.0f);
		if (ret == 0 || ret == -ERANGE) {
			ret = regulator_tune(&ldo, ldo_target, bb.measured_mv);
		}
	} else {
		ret = regulator_tune(&ldo, ldo_target, bb.measured ? bb.measured_mv : 0.0f);
		if (ret == 0 || ret == -ERANGE) {
			ret = regulator_tune(&bb, bb_target, 0.0f);
		}
	}

	tune_load(false);

	/* The LDO's load regulation lifts the output once the tuning load is
	 * gone (about 20 mV at 2.3 V on a real kit, more than a wiper step), so
	 * the final check is made at the load the DUT will actually see.
	 */
	if (ret == 0) {
		ret = regulator_tune(&ldo, ldo_target, bb.measured_mv);
	}
	board_io_set(BOARD_CTRL_VLDO_EN, ldo_was_connected);

	retune_pending = false;

	if (ret == -ERANGE) {
		LOG_WRN("setpoint %u mV out of reach (LDO %d mV at code %u)",
			vdd_mv, (int)ldo.measured_mv, ldo.code);
	}

	return ret;
}

static void enter_ampere(void)
{
	board_io_set(BOARD_CTRL_VLDO_EN, false);
	board_io_set(BOARD_CTRL_REG_EN, false);
	k_msleep(5);
	board_io_set(BOARD_CTRL_VEXT_EN, true);
}

static int enter_source(void)
{
	board_io_set(BOARD_CTRL_VEXT_EN, false);
	board_io_set(BOARD_CTRL_REG_EN, true);
	regulator_write(&bb, bb.code);
	regulator_write(&ldo, ldo.code);
	k_msleep(20);

	/* apply_vdd() connects the LDO itself for the duration of the tune and
	 * restores this state afterwards, so set it before, not after.
	 */
	board_io_set(BOARD_CTRL_VLDO_EN, true);

	return apply_vdd(!sampling_running());
}

/*
 * The instrumentation amplifier's zero-current pedestal is what the factory
 * `O0` constant encodes: the raw code range 0 reads with nothing flowing. It
 * moves with temperature and with what the terminals have been doing (54 to
 * 68 counts across an afternoon at one wiper code, 3.6 counts per step), and
 * every range sees the shift at its own gain, so a stale trim is 0.1 uA at
 * range 0 and 11 uA at range 3. It is therefore re-trimmed whenever current
 * is known to be zero and the analog section is warm: just before the DUT
 * output is switched on in source mode. The boot trim is only a first guess.
 * Automatic trims stay in RAM; the EEPROM `IA` field is only written by an
 * explicit shell request.
 */
#define BOOT_TRIM_DELAY		K_MSEC(1500)
#define IA_TRIM_START		24

static int32_t pedestal_target(void)
{
	return metadata_is_calibrated() ? (int32_t)lroundf(metadata_get()->o[0]) : 100;
}

static void trim_pedestal(const char *when)
{
	uint16_t code;

	if (sampling_running() || output_on) {
		return;
	}
	if (power_trim_ia(pedestal_target(), &code) == 0) {
		LOG_INF("IA pedestal trimmed at %s: wiper %u", when, code);
	}
}

static void boot_trim_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	trim_pedestal("boot");
}

static K_WORK_DELAYABLE_DEFINE(boot_trim_work, boot_trim_handler);

int power_init(void)
{
	struct ppk2_metadata *m = metadata_get();
	int ret;

	ret = digipot_init();
	if (ret < 0) {
		return ret;
	}

	vdd_mv = CLAMP(m->vdd, POWER_VDD_MIN_MV, POWER_VDD_MAX_MV);

	k_mutex_lock(&lock, K_FOREVER);
	ia_code = metadata_ia_unknown() ? IA_TRIM_START : m->ia;
	digipot_set(DIGIPOT_IA_OFFSET, ia_code);

	/* The kit remembers its mode, but never powers the DUT on its own. */
	output_on = false;
	board_io_set(BOARD_CTRL_VOUT_EN, false);
	k_work_schedule(&boot_trim_work, BOOT_TRIM_DELAY);

	mode = m->mode == PPK2_MODE_SOURCE ? PPK2_MODE_SOURCE : PPK2_MODE_AMPERE;
	if (mode == PPK2_MODE_SOURCE) {
		ret = enter_source();
	} else {
		enter_ampere();
		ret = 0;
	}
	k_mutex_unlock(&lock);

	return ret;
}

int power_set_mode(uint8_t new_mode)
{
	int ret = 0;

	if (new_mode != PPK2_MODE_AMPERE && new_mode != PPK2_MODE_SOURCE) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (new_mode != mode) {
		mode = new_mode;
		if (mode == PPK2_MODE_SOURCE) {
			ret = enter_source();
		} else {
			enter_ampere();
		}

		metadata_get()->mode = mode;
		metadata_save_deferred();
	}
	k_mutex_unlock(&lock);

	return ret;
}

int power_set_vdd(uint16_t mv)
{
	int ret = 0;

	mv = CLAMP(mv, POWER_VDD_MIN_MV, POWER_VDD_MAX_MV);

	k_mutex_lock(&lock, K_FOREVER);
	vdd_mv = mv;
	if (mode == PPK2_MODE_SOURCE) {
		ret = apply_vdd(!sampling_running());
	}

	metadata_get()->vdd = vdd_mv;
	metadata_save_deferred();
	k_mutex_unlock(&lock);

	return ret;
}

int power_set_output(bool on)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (on && !output_on && mode == PPK2_MODE_SOURCE) {
		trim_pedestal("output enable");
	}
	output_on = on;
	int ret = board_io_set(BOARD_CTRL_VOUT_EN, on);

	k_mutex_unlock(&lock);

	return ret;
}

uint8_t power_mode(void)
{
	return mode;
}

uint16_t power_vdd(void)
{
	return vdd_mv;
}

bool power_output(void)
{
	return output_on;
}

void power_on_stream_stopped(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (retune_pending && mode == PPK2_MODE_SOURCE) {
		apply_vdd(true);
	}
	k_mutex_unlock(&lock);
}

int power_set_ia_code(uint16_t code)
{
	int ret = digipot_set(DIGIPOT_IA_OFFSET, code);

	if (ret == 0) {
		ia_code = code;
	}

	return ret;
}

/* 48 oversampled conversions average the 7-count sample noise to about one
 * count, and keep a trim to a few hundred milliseconds so it can run in
 * front of an output-enable request.
 */
#define IA_TRIM_SAMPLES		48

int power_trim_ia(int32_t target_raw, uint16_t *code_out)
{
	int32_t raw;
	int ret;
	float slope = 4.0f;	/* counts per wiper step, sign learned below */
	uint16_t code = ia_code;

	for (int step = 0; step < 40; step++) {
		ret = sampling_measure_raw(SAMPLING_CH_VSE_IA, IA_TRIM_SAMPLES, &raw);
		if (ret < 0) {
			return ret;
		}

		int32_t err = target_raw - raw;

		if (abs(err) <= 2) {
			break;
		}

		int32_t delta = (int32_t)lroundf((float)err / slope);

		delta = CLAMP(delta, -16, 16);
		if (delta == 0) {
			delta = err > 0 ? 1 : -1;
		}

		uint16_t next = clamp_code((int32_t)code + delta);

		if (next == code) {
			return -ERANGE;
		}

		ret = digipot_set(DIGIPOT_IA_OFFSET, next);
		if (ret < 0) {
			return ret;
		}
		k_msleep(3);

		int32_t raw_after;

		ret = sampling_measure_raw(SAMPLING_CH_VSE_IA, IA_TRIM_SAMPLES, &raw_after);
		if (ret < 0) {
			return ret;
		}

		int32_t dcode = (int32_t)next - (int32_t)code;

		if (abs(raw_after - raw) >= 2) {
			float s = (float)(raw_after - raw) / (float)dcode;

			if (fabsf(s) >= 0.25f && fabsf(s) <= 400.0f) {
				slope = s;
			}
		}
		code = next;
	}

	ia_code = code;
	*code_out = code;

	return 0;
}

int power_get_status(struct power_status *st, bool measure)
{
	int ret = 0;

	k_mutex_lock(&lock, K_FOREVER);
	st->mode = mode;
	st->vdd_mv = vdd_mv;
	st->output_on = output_on;
	st->retune_pending = retune_pending;
	st->ia_code = ia_code;
	st->ldo_code = ldo.code;
	st->bb_code = bb.code;
	st->ext_usb = board_io_ext_usb_present();
	st->vldo_mv = st->vbb_mv = st->vin_mv = st->vdut_mv = NAN;

	if (measure) {
		ret = sampling_measure_mv(SAMPLING_CH_VLDO, &st->vldo_mv);
		if (ret == 0) {
			ret = sampling_measure_mv(SAMPLING_CH_VBB, &st->vbb_mv);
		}
		if (ret == 0) {
			ret = sampling_measure_mv(SAMPLING_CH_VIN, &st->vin_mv);
		}
		if (ret == 0) {
			ret = sampling_measure_mv(SAMPLING_CH_VDUT, &st->vdut_mv);
		}
	}
	k_mutex_unlock(&lock);

	return ret;
}

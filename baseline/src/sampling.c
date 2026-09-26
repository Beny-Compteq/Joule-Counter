/*
 * SAADC front end: the 100 kHz current stream and the slow voltage monitors.
 *
 * The stream is driven entirely in hardware: TIMER2 fires TASKS_SAMPLE every
 * 10 us over PPI, and EVENTS_END re-arms the one-sample EasyDMA buffer via a
 * second PPI channel, so the CPU only has to pick each result up. The END
 * interrupt reads the result together with the range-switch and logic-port
 * pins and packs one 32-bit sample into a ring buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <hal/nrf_saadc.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_ppi.h>
#include <hal/nrf_gpio.h>

#include "board_io.h"
#include "ppk2.h"
#include "sampling.h"

LOG_MODULE_REGISTER(sampling, CONFIG_LOG_DEFAULT_LEVEL);

#define RING_SIZE	CONFIG_PPK2_SAMPLE_RING_SAMPLES
#define RING_MASK	(RING_SIZE - 1)
BUILD_ASSERT((RING_SIZE & RING_MASK) == 0, "ring size must be a power of two");
BUILD_ASSERT(RING_SIZE >= 2 * PPK2_BLOCK_SAMPLES);

/* 16 MHz timer ticks per sample period, and where in the period to sample. */
#define TIMER_PERIOD_TICKS	(16000000U / PPK2_SAMPLE_RATE_HZ)
#define TIMER_SAMPLE_TICK	112U
BUILD_ASSERT(TIMER_PERIOD_TICKS == 160);

/* Fixed PPI channels; nothing else in this build uses the PPI. */
#define PPI_CH_SAMPLE		NRF_PPI_CHANNEL0
#define PPI_CH_REARM		NRF_PPI_CHANNEL1
#define PPI_CH_COUNT		NRF_PPI_CHANNEL2
#define PPI_CH_END_STAMP	NRF_PPI_CHANNEL3

/*
 * The END handler has 10 us to collect each result before the next one
 * overwrites it. At any kernel-visible priority it still queues behind
 * irq_lock() sections, and under streaming USB load those cost 41% of the
 * conversions on a real kit. So it runs zero-latency, above irq_lock(), and
 * therefore makes no kernel calls: block-ready signalling comes from TIMER3,
 * which counts END events in hardware and interrupts every block.
 */
#define IRQ_PRIO_SAADC		0
#define IRQ_PRIO_TIMER3		1

/*
 * Acquisition time code 7 is not in the product specification (documented
 * codes stop at 5 = 40 us) but it is what the PPK2's own firmware programs
 * for the current channel, and the calibration constants in every kit's
 * EEPROM were derived with it. The instrumentation amplifier output reaches
 * AIN7 through a 10 kOhm/1 nF filter, so a longer acquisition than the
 * 3 us minimum is also what the source impedance calls for.
 */
#define STREAM_TACQ_CODE	7U

#define CH_CONFIG(gain, tacq)							\
	((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |		\
	 (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |		\
	 ((gain) << SAADC_CH_CONFIG_GAIN_Pos) |					\
	 (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |	\
	 ((tacq) << SAADC_CH_CONFIG_TACQ_Pos) |					\
	 (SAADC_CH_CONFIG_MODE_Diff << SAADC_CH_CONFIG_MODE_Pos) |		\
	 (SAADC_CH_CONFIG_BURST_Enabled << SAADC_CH_CONFIG_BURST_Pos))

/* Every channel is measured differentially against the analog ground on AIN5. */
struct channel_desc {
	nrf_saadc_input_t input;
	uint32_t gain_code;
	float full_scale_mv;	/* 600 mV internal reference divided by the gain */
	float divider;		/* external attenuation to undo */
};

static const struct channel_desc channels[SAMPLING_CH_COUNT] = {
	[SAMPLING_CH_VSE_IA] = { NRF_SAADC_INPUT_AIN7, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 1.0f },
	[SAMPLING_CH_VREF_IA] = { NRF_SAADC_INPUT_AIN0, SAADC_CH_CONFIG_GAIN_Gain4, 150.0f, 1.0f },
	[SAMPLING_CH_VLDO] = { NRF_SAADC_INPUT_AIN1, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 5.0f },
	[SAMPLING_CH_VBB] = { NRF_SAADC_INPUT_AIN2, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 5.0f },
	[SAMPLING_CH_VIN] = { NRF_SAADC_INPUT_AIN3, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 5.0f },
	[SAMPLING_CH_VDUT] = { NRF_SAADC_INPUT_AIN4, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 5.0f },
	[SAMPLING_CH_NTC] = { NRF_SAADC_INPUT_AIN6, SAADC_CH_CONFIG_GAIN_Gain1_3, 1800.0f, 1.0f },
};

static uint32_t ring[RING_SIZE];
static volatile uint32_t ring_wr;
static uint32_t ring_rd;
static volatile uint32_t sample_counter;
static volatile uint32_t isr_entries;
static uint8_t stream_tacq = STREAM_TACQ_CODE;
/* Sample period actually programmed; only the self-test changes it. */
static uint32_t period_ticks = TIMER_PERIOD_TICKS;

static volatile nrf_saadc_value_t adc_result;

static volatile bool running;
static volatile bool discard_switch = IS_ENABLED(CONFIG_PPK2_DISCARD_SWITCH_SAMPLES);
static struct sampling_metrics metrics;

static K_SEM_DEFINE(block_sem, 0, K_SEM_MAX_LIMIT);
static K_MUTEX_DEFINE(adc_lock);

/*
 * ISR diagnostics. TIMER3 counts END events in hardware; reading it on every
 * entry gives the exact number of ENDs since the previous entry, so ENDs the
 * handler never saw are counted rather than inferred. Run time is in 16 MHz
 * TIMER2 ticks (160 per sample).
 */
static volatile uint32_t isr_missed_ends;
static volatile uint32_t isr_max_end_gap;
static volatile uint32_t isr_max_run_ticks;
static uint32_t isr_last_end_count;

/* END-to-handler latency: TIMER4 free-runs at 16 MHz, PPI captures it into
 * CC0 on END and the handler captures CC1 on entry. Unwrapped 32-bit, so a
 * delay longer than one sample period shows as what it is.
 */
static volatile uint32_t isr_max_latency_ticks;
static volatile uint32_t isr_latency_hist[SAMPLING_LATENCY_BUCKETS];

/* Zero-latency: registers and plain memory only, nothing from the kernel. */
ISR_DIRECT_DECLARE(saadc_isr)
{
	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_CAPTURE2);
	uint32_t t_in = nrf_timer_cc_get(NRF_TIMER2, NRF_TIMER_CC_CHANNEL2);

	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_CAPTURE1);
	uint32_t latency = nrf_timer_cc_get(NRF_TIMER4, NRF_TIMER_CC_CHANNEL1) -
			   nrf_timer_cc_get(NRF_TIMER4, NRF_TIMER_CC_CHANNEL0);
	uint32_t bucket = latency / 16U;	/* 1 us buckets */

	if (bucket >= SAMPLING_LATENCY_BUCKETS) {
		bucket = SAMPLING_LATENCY_BUCKETS - 1;
	}
	isr_latency_hist[bucket]++;
	if (latency > isr_max_latency_ticks) {
		isr_max_latency_ticks = latency;
	}

	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_CAPTURE1);
	uint32_t ends = nrf_timer_cc_get(NRF_TIMER3, NRF_TIMER_CC_CHANNEL1);
	uint32_t gap = ends - isr_last_end_count;

	isr_last_end_count = ends;
	if (gap > 1) {
		isr_missed_ends += gap - 1;
		if (gap > isr_max_end_gap) {
			isr_max_end_gap = gap;
		}
	}

	isr_entries++;
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_END);

	/* The PPI re-arm has already restarted the DMA; the result is ours to
	 * read until the next conversion lands, 10 us from now.
	 */
	int32_t raw = adc_result;
	uint32_t p0 = nrf_gpio_port_in_read(NRF_P0);
	uint32_t p1 = nrf_gpio_port_in_read(NRF_P1);

	uint32_t sw = (p0 & 0x3U) | ((p0 >> 24) & 0xCU);
	uint8_t range = board_io_decode_range(sw);

	if (range == PPK2_RANGE_SWITCHING && discard_switch) {
		metrics.switch_dropped++;
	} else {
		/* Positive half only, two LSBs dropped: the host multiplies by
		 * four, which lands the value back in raw 14-bit counts.
		 */
		uint32_t adc = raw > 0 ? (((uint32_t)raw) >> 2) & 0xFFFU : 0U;
		uint32_t counter = sample_counter++;

		uint32_t word = adc |
				((uint32_t)range << PPK2_SAMPLE_RANGE_POS) |
				(((p1 >> BOARD_P1_EXT_USB_PIN) & 1U) << PPK2_SAMPLE_EXT_USB_POS) |
				((counter & PPK2_SAMPLE_COUNTER_MASK) << PPK2_SAMPLE_COUNTER_POS) |
				(((p0 >> BOARD_P0_LOGIC_SHIFT) & 0xFFU) << PPK2_SAMPLE_LOGIC_POS);

		uint32_t wr = ring_wr;

		ring[wr & RING_MASK] = word;
		ring_wr = wr + 1;
	}

	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_CAPTURE3);
	uint32_t t_out = nrf_timer_cc_get(NRF_TIMER2, NRF_TIMER_CC_CHANNEL3);
	uint32_t run = (t_out + period_ticks - t_in) % period_ticks;

	if (run > isr_max_run_ticks) {
		isr_max_run_ticks = run;
	}

	return 0;
}

/*
 * TIMER3 counts END events over PPI and reaches CC[0] every block; this is
 * the kernel-visible half of the sampling interrupt work.
 */
static void timer3_isr(const void *arg)
{
	ARG_UNUSED(arg);

	nrf_timer_event_clear(NRF_TIMER3, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_cc_set(NRF_TIMER3, NRF_TIMER_CC_CHANNEL0,
			 nrf_timer_cc_get(NRF_TIMER3, NRF_TIMER_CC_CHANNEL0) + PPK2_BLOCK_SAMPLES);
	k_sem_give(&block_sem);
}

static int wait_event(nrf_saadc_event_t event, uint32_t timeout_us)
{
	while (!nrf_saadc_event_check(NRF_SAADC, event)) {
		if (timeout_us == 0) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
		timeout_us--;
	}

	nrf_saadc_event_clear(NRF_SAADC, event);
	return 0;
}

static void saadc_common_setup(void)
{
	nrf_saadc_disable(NRF_SAADC);
	nrf_saadc_int_disable(NRF_SAADC, 0xFFFFFFFFU);
	nrf_saadc_resolution_set(NRF_SAADC, NRF_SAADC_RESOLUTION_14BIT);
	nrf_saadc_continuous_mode_disable(NRF_SAADC);

	for (uint8_t ch = 0; ch < 8; ch++) {
		nrf_saadc_channel_input_set(NRF_SAADC, ch, NRF_SAADC_INPUT_DISABLED,
					    NRF_SAADC_INPUT_DISABLED);
	}

	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STARTED);
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_END);
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_DONE);
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_RESULTDONE);
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);
}

static int saadc_calibrate_offset(void)
{
	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_CALIBRATEDONE);
	nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_CALIBRATEOFFSET);

	return wait_event(NRF_SAADC_EVENT_CALIBRATEDONE, 10000);
}

static void timer_setup(void)
{
	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(NRF_TIMER2, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(NRF_TIMER2, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(NRF_TIMER2, 0);
	nrf_timer_cc_set(NRF_TIMER2, NRF_TIMER_CC_CHANNEL0, TIMER_PERIOD_TICKS);
	nrf_timer_cc_set(NRF_TIMER2, NRF_TIMER_CC_CHANNEL1, TIMER_SAMPLE_TICK);
	nrf_timer_shorts_enable(NRF_TIMER2, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);

	nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CH_SAMPLE,
		nrf_timer_event_address_get(NRF_TIMER2, NRF_TIMER_EVENT_COMPARE1),
		nrf_saadc_task_address_get(NRF_SAADC, NRF_SAADC_TASK_SAMPLE));
	nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CH_REARM,
		nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END),
		nrf_saadc_task_address_get(NRF_SAADC, NRF_SAADC_TASK_START));

	/* TIMER3 counts END events in hardware, independent of the CPU, so a
	 * shortfall of interrupts against conversions is measurable.
	 */
	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(NRF_TIMER3, NRF_TIMER_MODE_COUNTER);
	nrf_timer_bit_width_set(NRF_TIMER3, NRF_TIMER_BIT_WIDTH_32);
	nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CH_COUNT,
		nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END),
		nrf_timer_task_address_get(NRF_TIMER3, NRF_TIMER_TASK_COUNT));

	/* TIMER4 timestamps each END for the latency measurement. */
	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_CLEAR);
	nrf_timer_mode_set(NRF_TIMER4, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(NRF_TIMER4, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(NRF_TIMER4, NRF_TIMER_FREQ_16MHz);
	nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CH_END_STAMP,
		nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END),
		nrf_timer_task_address_get(NRF_TIMER4, NRF_TIMER_TASK_CAPTURE0));
}

static uint32_t end_event_count(void)
{
	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_CAPTURE0);
	return nrf_timer_cc_get(NRF_TIMER3, NRF_TIMER_CC_CHANNEL0);
}


int sampling_init(void)
{
	int ret;

	IRQ_DIRECT_CONNECT(SAADC_IRQn, IRQ_PRIO_SAADC, saadc_isr, IRQ_ZERO_LATENCY);
	IRQ_CONNECT(TIMER3_IRQn, IRQ_PRIO_TIMER3, timer3_isr, NULL, 0);

	saadc_common_setup();
	NRF_SAADC->CH[0].CONFIG = CH_CONFIG(SAADC_CH_CONFIG_GAIN_Gain1_3, STREAM_TACQ_CODE);
	nrf_saadc_channel_input_set(NRF_SAADC, 0, NRF_SAADC_INPUT_AIN7, NRF_SAADC_INPUT_AIN5);
	nrf_saadc_oversample_set(NRF_SAADC, NRF_SAADC_OVERSAMPLE_DISABLED);
	nrf_saadc_enable(NRF_SAADC);

	ret = saadc_calibrate_offset();
	if (ret < 0) {
		LOG_WRN("offset calibration timed out");
	}

	nrf_saadc_disable(NRF_SAADC);
	timer_setup();

	return 0;
}

int sampling_start(void)
{
	int ret;

	k_mutex_lock(&adc_lock, K_FOREVER);

	if (running) {
		k_mutex_unlock(&adc_lock);
		return 0;
	}

	saadc_common_setup();
	NRF_SAADC->CH[0].CONFIG = CH_CONFIG(SAADC_CH_CONFIG_GAIN_Gain1_3, stream_tacq);
	nrf_saadc_channel_input_set(NRF_SAADC, 0, NRF_SAADC_INPUT_AIN7, NRF_SAADC_INPUT_AIN5);
	nrf_saadc_oversample_set(NRF_SAADC, NRF_SAADC_OVERSAMPLE_DISABLED);
	nrf_saadc_buffer_init(NRF_SAADC, (nrf_saadc_value_t *)&adc_result, 1);
	nrf_saadc_enable(NRF_SAADC);

	ring_rd = ring_wr;
	sample_counter = 0;
	isr_entries = 0;
	isr_missed_ends = 0;
	isr_max_end_gap = 0;
	isr_max_run_ticks = 0;
	isr_last_end_count = 0;
	isr_max_latency_ticks = 0;
	memset((void *)isr_latency_hist, 0, sizeof(isr_latency_hist));
	k_sem_reset(&block_sem);

	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_START);
	nrf_ppi_channel_enable(NRF_PPI, PPI_CH_END_STAMP);

	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_CLEAR);
	nrf_timer_cc_set(NRF_TIMER3, NRF_TIMER_CC_CHANNEL0, PPK2_BLOCK_SAMPLES);
	nrf_timer_event_clear(NRF_TIMER3, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_int_enable(NRF_TIMER3, NRF_TIMER_INT_COMPARE0_MASK);
	irq_enable(TIMER3_IRQn);
	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_START);

	nrf_saadc_int_enable(NRF_SAADC, NRF_SAADC_INT_END);
	irq_enable(SAADC_IRQn);

	nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_START);
	ret = wait_event(NRF_SAADC_EVENT_STARTED, 1000);
	if (ret < 0) {
		LOG_ERR("SAADC did not start");
		irq_disable(SAADC_IRQn);
		nrf_saadc_disable(NRF_SAADC);
		k_mutex_unlock(&adc_lock);
		return ret;
	}

	nrf_ppi_channel_enable(NRF_PPI, PPI_CH_COUNT);
	nrf_ppi_channel_enable(NRF_PPI, PPI_CH_REARM);
	nrf_ppi_channel_enable(NRF_PPI, PPI_CH_SAMPLE);
	nrf_timer_cc_set(NRF_TIMER2, NRF_TIMER_CC_CHANNEL0, period_ticks);
	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_START);

	running = true;
	k_mutex_unlock(&adc_lock);

	return 0;
}

void sampling_stop(void)
{
	k_mutex_lock(&adc_lock, K_FOREVER);

	if (!running) {
		k_mutex_unlock(&adc_lock);
		return;
	}

	nrf_timer_task_trigger(NRF_TIMER2, NRF_TIMER_TASK_STOP);
	nrf_ppi_channel_disable(NRF_PPI, PPI_CH_SAMPLE);
	nrf_ppi_channel_disable(NRF_PPI, PPI_CH_REARM);

	nrf_saadc_int_disable(NRF_SAADC, NRF_SAADC_INT_END);
	irq_disable(SAADC_IRQn);

	nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);
	nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_STOP);
	wait_event(NRF_SAADC_EVENT_STOPPED, 1000);
	nrf_saadc_disable(NRF_SAADC);

	/* Leave the counters readable until the next start. */
	nrf_ppi_channel_disable(NRF_PPI, PPI_CH_COUNT);
	nrf_ppi_channel_disable(NRF_PPI, PPI_CH_END_STAMP);
	nrf_timer_int_disable(NRF_TIMER3, NRF_TIMER_INT_COMPARE0_MASK);
	irq_disable(TIMER3_IRQn);
	nrf_timer_task_trigger(NRF_TIMER3, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(NRF_TIMER4, NRF_TIMER_TASK_STOP);

	running = false;
	/* Wake the consumer so it can flush what is left in the ring. */
	k_sem_give(&block_sem);

	k_mutex_unlock(&adc_lock);
}

bool sampling_running(void)
{
	return running;
}

int sampling_wait_block(k_timeout_t timeout)
{
	return k_sem_take(&block_sem, timeout);
}

size_t sampling_available(void)
{
	uint32_t wr = ring_wr;
	uint32_t used = wr - ring_rd;

	if (used > RING_SIZE) {
		/* The writer has lapped us. Skip whole blocks until the tail is
		 * safely ahead of where the writer will land next.
		 */
		uint32_t drop = ROUND_UP(used - (RING_SIZE - 2 * PPK2_BLOCK_SAMPLES),
					 PPK2_BLOCK_SAMPLES);

		ring_rd += drop;
		metrics.blocks_dropped += drop / PPK2_BLOCK_SAMPLES;
		used = wr - ring_rd;
	}

	return used;
}

size_t sampling_take(uint32_t *dst, size_t n)
{
	size_t avail = sampling_available();

	if (n > avail) {
		n = avail;
	}

	for (size_t k = 0; k < n; k++) {
		dst[k] = ring[(ring_rd + k) & RING_MASK];
	}

	/* If the writer overtook this region while we copied, the copy is
	 * garbage; drop it rather than ship it.
	 */
	if (ring_wr - ring_rd > RING_SIZE) {
		metrics.blocks_dropped++;
		ring_rd = ring_wr;
		return 0;
	}

	ring_rd += n;
	metrics.samples_emitted += n;

	return n;
}

void sampling_get_metrics(struct sampling_metrics *m)
{
	*m = metrics;
	m->end_events = end_event_count();
	m->isr_entries = isr_entries;
	m->isr_missed_ends = isr_missed_ends;
	m->isr_max_end_gap = isr_max_end_gap;
	m->isr_max_run_ticks = isr_max_run_ticks;
	m->isr_max_latency_ticks = isr_max_latency_ticks;
	memcpy(m->isr_latency_hist, (const void *)isr_latency_hist, sizeof(m->isr_latency_hist));
}

int sampling_selftest(uint32_t seconds, bool spin, uint32_t period, struct sampling_metrics *m)
{
	/* No faster than the production rate, and slow enough to be a test. */
	if (period < TIMER_PERIOD_TICKS || period > 16000U) {
		return -EINVAL;
	}
	period_ticks = period;

	int ret = sampling_start();

	if (ret < 0) {
		period_ticks = TIMER_PERIOD_TICKS;
		return ret;
	}
	if (spin) {
		/* Keep the CPU out of WFI for the whole run. */
		uint32_t start = k_uptime_get_32();

		while (k_uptime_get_32() - start < seconds * 1000U) {
			arch_nop();
		}
	} else {
		k_sleep(K_SECONDS(seconds));
	}
	sampling_stop();
	period_ticks = TIMER_PERIOD_TICKS;
	sampling_get_metrics(m);

	return 0;
}

int sampling_set_tacq(uint8_t code)
{
	if (code > 7) {
		return -EINVAL;
	}

	stream_tacq = code;
	return 0;
}

uint8_t sampling_get_tacq(void)
{
	return stream_tacq;
}

#ifdef CONFIG_PPK2_STREAM_KEEPS_CPU_AWAKE
/*
 * Waking the Cortex-M4 from WFI takes 12.5 us on this kit (END-to-handler
 * latency histogram: about 1.2 us with the CPU awake, 12.5 us from idle, and
 * constant-latency mode only brings that to 11.5 us). That is longer than the
 * sample period, so an idle CPU straddles two conversions on every wake-up
 * and services only half of them; at 66 kHz and below nothing is lost. The
 * idle thread therefore spins instead of sleeping while the stream runs.
 */
bool z_arm_on_enter_cpu_idle(void)
{
	return !running;
}
#endif

void sampling_reset_metrics(void)
{
	memset(&metrics, 0, sizeof(metrics));
}

void sampling_count_block_sent(void)
{
	metrics.blocks_sent++;
}

void sampling_set_discard_switch(bool enable)
{
	discard_switch = enable;
}

bool sampling_get_discard_switch(void)
{
	return discard_switch;
}

static int single_conversion(const struct channel_desc *desc, nrf_saadc_oversample_t oversample,
			     int32_t *raw)
{
	volatile nrf_saadc_value_t value = 0;
	int ret;

	saadc_common_setup();
	NRF_SAADC->CH[0].CONFIG = CH_CONFIG(desc->gain_code, SAADC_CH_CONFIG_TACQ_40us);
	nrf_saadc_channel_input_set(NRF_SAADC, 0, desc->input, NRF_SAADC_INPUT_AIN5);
	nrf_saadc_oversample_set(NRF_SAADC, oversample);
	nrf_saadc_buffer_init(NRF_SAADC, (nrf_saadc_value_t *)&value, 1);
	nrf_saadc_enable(NRF_SAADC);

	nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_START);
	ret = wait_event(NRF_SAADC_EVENT_STARTED, 1000);
	if (ret == 0) {
		/* With burst mode on, one SAMPLE task runs the whole
		 * oversampling sequence.
		 */
		nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_SAMPLE);
		ret = wait_event(NRF_SAADC_EVENT_END, 30000);
	}

	nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_STOP);
	wait_event(NRF_SAADC_EVENT_STOPPED, 1000);
	nrf_saadc_disable(NRF_SAADC);

	if (ret == 0) {
		*raw = value;
	}

	return ret;
}

int sampling_measure_raw(enum sampling_channel ch, unsigned int count, int32_t *raw_avg)
{
	int64_t sum = 0;
	int ret = 0;

	if (ch >= SAMPLING_CH_COUNT || count == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&adc_lock, K_FOREVER);
	if (running) {
		k_mutex_unlock(&adc_lock);
		return -EBUSY;
	}

	for (unsigned int k = 0; k < count; k++) {
		int32_t raw;

		ret = single_conversion(&channels[ch], NRF_SAADC_OVERSAMPLE_DISABLED, &raw);
		if (ret < 0) {
			break;
		}
		sum += raw;
	}
	k_mutex_unlock(&adc_lock);

	if (ret == 0) {
		*raw_avg = (int32_t)(sum / (int64_t)count);
	}

	return ret;
}

int sampling_measure_mv(enum sampling_channel ch, float *mv)
{
	int32_t raw;
	int ret;

	if (ch >= SAMPLING_CH_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&adc_lock, K_FOREVER);
	if (running) {
		k_mutex_unlock(&adc_lock);
		return -EBUSY;
	}
	ret = single_conversion(&channels[ch], NRF_SAADC_OVERSAMPLE_256X, &raw);
	k_mutex_unlock(&adc_lock);

	if (ret < 0) {
		return ret;
	}

	/* Differential 14-bit: 13 bits of magnitude per polarity. */
	*mv = (float)raw * channels[ch].full_scale_mv / 8192.0f * channels[ch].divider;

	return 0;
}

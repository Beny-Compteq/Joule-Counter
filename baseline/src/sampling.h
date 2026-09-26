/*
 * SAADC front end: the 100 kHz current stream and the slow voltage monitors.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAMPLING_H_
#define SAMPLING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/* END-to-handler latency histogram, 1 us per bucket, last bucket open-ended. */
#define SAMPLING_LATENCY_BUCKETS	24

struct sampling_metrics {
	uint32_t samples_emitted;
	uint32_t switch_dropped;
	uint32_t blocks_dropped;
	uint32_t blocks_sent;
	/* SAADC END events counted in hardware (TIMER3) since the last start. */
	uint32_t end_events;
	/* Times the END interrupt actually ran. Fewer than end_events means
	 * conversions were lost to interrupt latency.
	 */
	uint32_t isr_entries;
	/* END events that passed between consecutive ISR entries without one
	 * of their own, and the largest such run.
	 */
	uint32_t isr_missed_ends;
	uint32_t isr_max_end_gap;
	/* Longest handler run seen, in 16 MHz ticks (160 per sample). */
	uint32_t isr_max_run_ticks;
	/* END event to handler entry, 16 MHz ticks, and its distribution. */
	uint32_t isr_max_latency_ticks;
	uint32_t isr_latency_hist[SAMPLING_LATENCY_BUCKETS];
};

/* Runs the sampler with nothing consuming the stream, then reports metrics.
 * With spin set the calling thread busy-waits instead of sleeping, which
 * keeps the CPU out of WFI for the duration. period is the sample period in
 * 16 MHz ticks (160 for the production rate).
 */
int sampling_selftest(uint32_t seconds, bool spin, uint32_t period, struct sampling_metrics *m);

enum sampling_channel {
	SAMPLING_CH_VSE_IA,	/* AIN7, instrumentation amplifier output */
	SAMPLING_CH_VREF_IA,	/* AIN0, instrumentation amplifier reference */
	SAMPLING_CH_VLDO,	/* AIN1, LDO output, /5 */
	SAMPLING_CH_VBB,	/* AIN2, buck-boost output, /5 */
	SAMPLING_CH_VIN,	/* AIN3, VIN terminal, /5 */
	SAMPLING_CH_VDUT,	/* AIN4, VDUT+, /5 */
	SAMPLING_CH_NTC,	/* AIN6, thermistor divider */
	SAMPLING_CH_COUNT,
};

int sampling_init(void);

int sampling_start(void);
void sampling_stop(void);
bool sampling_running(void);

/* Blocks until a block-sized run of samples is likely available. */
int sampling_wait_block(k_timeout_t timeout);

/* Samples ready to be taken; also recovers from ring overflow. */
size_t sampling_available(void);

/* Copies out up to n samples. Returns the number copied. */
size_t sampling_take(uint32_t *dst, size_t n);

void sampling_get_metrics(struct sampling_metrics *m);
void sampling_reset_metrics(void);
void sampling_count_block_sent(void);

void sampling_set_discard_switch(bool enable);
bool sampling_get_discard_switch(void);

/* SAADC acquisition-time code for the stream (CONFIG.TACQ, 0..7). Takes
 * effect at the next start. Exposed for experiments; see STREAM_TACQ_CODE.
 */
int sampling_set_tacq(uint8_t code);
uint8_t sampling_get_tacq(void);

/*
 * One 256x oversampled conversion. Only while the stream is stopped.
 * The result is the voltage at the measured node, dividers included.
 */
int sampling_measure_mv(enum sampling_channel ch, float *mv);

/* Averaged raw 14-bit differential reading, for calibration work. */
int sampling_measure_raw(enum sampling_channel ch, unsigned int count, int32_t *raw_avg);

#endif /* SAMPLING_H_ */

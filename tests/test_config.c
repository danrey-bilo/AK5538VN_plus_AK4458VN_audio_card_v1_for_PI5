/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../akm_v5_config.h"

int main(void)
{
	const unsigned int registers[] = { 0x37, 0x37, 0x1f, 0x1f, 7, 7, 0x47 };
	const unsigned int ratios[] = { 512, 512, 256, 256, 128, 128, 64 };
	unsigned int i, j;

	for (i = 0; i < 7; i++) {
		const struct akm_v5_rate *rate = akm_v5_find_rate(akm_v5_rates[i].hz);
		assert(rate == &akm_v5_rates[i]);
		assert(rate->mclk / rate->hz == ratios[i]);
		assert(rate->mclk % rate->hz == 0);
		assert(akm_v5_adc_format(rate, 1) == registers[i]);
		assert(akm_v5_adc_format(rate, 0) == (registers[i] & ~1u));
		assert(rate->xo == (rate->mclk == 24576000));
		for (j = 0; j < 7; j++) {
			assert(!akm_v5_rate_conflicts(0, 1, rate->hz, akm_v5_rates[j].hz));
			assert(!akm_v5_rate_conflicts(1, 1, rate->hz, akm_v5_rates[j].hz));
			assert(!!akm_v5_rate_conflicts(1, 2, rate->hz, akm_v5_rates[j].hz) == (i != j));
			assert(!!akm_v5_rate_conflicts(3, 1, rate->hz, akm_v5_rates[j].hz) == (i != j));
		}
	}
	assert(!akm_v5_find_rate(0));
	assert(!akm_v5_find_rate(32000));
	assert(!akm_v5_find_rate(364000));
	assert(!akm_v5_find_rate(768000));
	for (i = 0; i < 8; i++) {
		assert(akm_v5_volume_regs[i] <= 0x14);
		for (j = 0; j < i; j++)
			assert(akm_v5_volume_regs[j] != akm_v5_volume_regs[i]);
	}
	puts("PASS: clock ratios, GPIO family, ADC format, duplex rate conflicts, volume register addresses");
	return 0;
}

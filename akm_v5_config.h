/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PIAOIP_AKM_V5_CONFIG_H
#define PIAOIP_AKM_V5_CONFIG_H

/* Portable policy, shared by the kernel driver and the host-side unit test. */
struct akm_v5_rate {
	unsigned int hz;
	unsigned int mclk;
	unsigned int xo; /* 0 = U21/GPIO5, 1 = U22/GPIO6 */
	unsigned int adc_cks;
};

static const struct akm_v5_rate akm_v5_rates[] = {
	{ 44100, 22579200, 0, 6 },
	{ 48000, 24576000, 1, 6 },
	{ 88200, 22579200, 0, 3 },
	{ 96000, 24576000, 1, 3 },
	{ 176400, 22579200, 0, 0 },
	{ 192000, 24576000, 1, 0 },
	{ 384000, 24576000, 1, 8 },
};

static inline const struct akm_v5_rate *akm_v5_find_rate(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < sizeof(akm_v5_rates) / sizeof(akm_v5_rates[0]); i++)
		if (akm_v5_rates[i].hz == rate)
			return &akm_v5_rates[i];
	return 0;
}

static inline unsigned int akm_v5_adc_format(const struct akm_v5_rate *rate,
					  int hpf)
{
	return (rate->adc_cks << 3) | 0x06 | !!hpf;
}

/* A second direction must keep the clock family AND rate of the first one. */
static inline int akm_v5_rate_conflicts(unsigned int configured_mask,
				       unsigned int stream_bit,
				       unsigned int current_rate,
				       unsigned int requested)
{
	return (configured_mask & ~stream_bit) && current_rate != requested;
}

static const unsigned char akm_v5_volume_regs[8] = {
	0x03, 0x04, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
};

/* ATS=00: allow the full 4080/fs ramp, filter tail and 1 ms margin.
 * A second stream closing must wait only for the remaining ramp, not restart it.
 */
static inline unsigned int akm_v5_mute_guard_us(unsigned int rate)
{
	return rate ? (unsigned int)((4112ULL * 1000000 + rate - 1) / rate) + 1000 : 100000;
}

static inline unsigned int akm_v5_mute_remaining_us(unsigned int rate,
						 unsigned long long elapsed_us)
{
	unsigned int guard = akm_v5_mute_guard_us(rate);

	return elapsed_us >= guard ? 0 : guard - (unsigned int)elapsed_us;
}

/* Stable ALSA enum order; AK4458 datasheet, Table 16. */
enum akm_v5_dac_filter {
	AKM_V5_DAC_SHARP,
	AKM_V5_DAC_SLOW,
	AKM_V5_DAC_SHORT_SHARP,
	AKM_V5_DAC_SHORT_SLOW,
	AKM_V5_DAC_SUPER_SLOW,
	AKM_V5_DAC_FILTER_COUNT,
};

static inline unsigned int akm_v5_dac_control2(unsigned int filter, int mute)
{
	/* DEM11:10 = 01 (off); preserve the selected SD bit when muting. */
	return 0x02 | (filter >= AKM_V5_DAC_SHORT_SHARP ? 0x20 : 0) | !!mute;
}

static inline unsigned int akm_v5_dac_control3(unsigned int filter)
{
	return filter == AKM_V5_DAC_SLOW || filter == AKM_V5_DAC_SHORT_SLOW;
}

static inline unsigned int akm_v5_dac_control4(unsigned int filter)
{
	return filter == AKM_V5_DAC_SUPER_SLOW;
}

#endif

// SPDX-License-Identifier: GPL-2.0-only
/* PiAoIP v5: AK5538 clock provider + AK4458, RP1 I2S1 four stereo lanes. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "akm_v5_config.h"

#define AKM_VERSION "0.1.0"
#define AKM_RATES (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
	SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
	SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_384000)

struct akm_board {
	struct device *dev;
	struct i2c_client *adc, *dac;
	struct regmap *adc_map, *dac_map;
	struct gpio_desc *adc_reset, *dac_reset, *xo44, *xo48;
	struct mutex lock; /* I2C, clock changes, controls and configured_mask */
	struct delayed_work mute_work;
	unsigned long running_mask; /* Atomic bitops: trigger callbacks never sleep. */
	unsigned int configured_mask, rate, suspended_rate, i2c_errors;
	int last_error;
	bool powered, output_enabled, hpf, adc_short_delay;
	bool removing;
	u8 volume[8];
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, codec, platform;
};

static int akm_result(struct akm_board *b, int ret)
{
	if (ret < 0) {
		b->last_error = ret;
		b->i2c_errors++;
	}
	return ret;
}

static int akm_write(struct akm_board *b, struct regmap *map,
		     unsigned int reg, unsigned int value)
{
	return akm_result(b, regmap_write(map, reg, value));
}

static int akm_read(struct akm_board *b, struct regmap *map,
		    unsigned int reg, unsigned int *value)
{
	return akm_result(b, regmap_read(map, reg, value));
}

static int akm_write_verify(struct akm_board *b, struct regmap *map,
			    unsigned int reg, unsigned int value)
{
	unsigned int actual;
	int ret = akm_write(b, map, reg, value);

	if (ret)
		return ret;
	ret = akm_read(b, map, reg, &actual);
	if (ret)
		return ret;
	if (actual != value) {
		dev_err(b->dev, "register %02x readback %02x != %02x\n",
			reg, actual, value);
		return akm_result(b, -EIO);
	}
	return 0;
}

/* Call with b->lock held, in process context. GPIO reset flags are active-low. */
static void akm_power_off(struct akm_board *b)
{
	gpiod_set_value_cansleep(b->dac_reset, 1);
	gpiod_set_value_cansleep(b->adc_reset, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(b->xo44, 0);
	gpiod_set_value_cansleep(b->xo48, 0);
	b->powered = false;
	b->rate = 0;
}

static int akm_set_mute(struct akm_board *b, bool mute)
{
	if (!b->powered)
		return 0;
	return akm_write(b, b->dac_map, 0x01, mute ? 0x23 : 0x22);
}

static int akm_configure(struct akm_board *b, const struct akm_v5_rate *rate)
{
	/* Only writable PCM configuration registers; reserved/status stay untouched. */
	static const struct { u8 reg, val; } dac_profile[] = {
		{ 0x01, 0x23 }, { 0x00, 0x8e }, { 0x02, 0x00 },
		{ 0x05, 0x00 }, { 0x0a, 0x0d }, { 0x0b, 0x0c },
		{ 0x0c, 0x00 }, { 0x0d, 0x00 }, { 0x0e, 0x50 },
	};
	unsigned int i;
	int ret;

	akm_power_off(b);
	/* Lock the entire I2C bus during LDO startup: no other client may transfer. */
	i2c_lock_bus(b->adc->adapter, I2C_LOCK_ROOT_ADAPTER);
	gpiod_set_value_cansleep(rate->xo ? b->xo48 : b->xo44, 1);
	msleep(5);
	gpiod_set_value_cansleep(b->adc_reset, 0);
	gpiod_set_value_cansleep(b->dac_reset, 0);
	msleep(20);
	i2c_unlock_bus(b->adc->adapter, I2C_LOCK_ROOT_ADAPTER);
	b->powered = true;

#define ADC(reg, val) do { \
	ret = akm_write_verify(b, b->adc_map, (reg), (val)); \
	if (ret) goto fail; \
} while (0)
	ADC(0x01, 0x00);
	ADC(0x00, 0xff);
	ADC(0x02, akm_v5_adc_format(rate, b->hpf));
	ADC(0x03, 0x00);
	ADC(0x04, b->adc_short_delay ? 0x02 : 0x00);
	ADC(0x05, 0x00);
	for (i = 0; i < ARRAY_SIZE(dac_profile); i++) {
		ret = akm_write_verify(b, b->dac_map,
				       dac_profile[i].reg, dac_profile[i].val);
		if (ret)
			goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(b->volume); i++) {
		ret = akm_write_verify(b, b->dac_map,
				       akm_v5_volume_regs[i], b->volume[i]);
		if (ret)
			goto fail;
	}
	ADC(0x01, 0x01);
#undef ADC
	/* 578/fs ADC initialization, plus margin for clock/filter stabilization. */
	msleep(DIV_ROUND_UP(578000U, rate->hz) + 5);
	ret = akm_write_verify(b, b->dac_map, 0x00, 0x8f);
	if (ret)
		goto fail;
	b->rate = rate->hz;
	b->last_error = 0;
	return 0;
fail:
	akm_power_off(b);
	return ret;
}

/* No control transfer is performed from a DMA/PCM atomic trigger callback. */
static void akm_mute_work(struct work_struct *work)
{
	struct akm_board *b = container_of(to_delayed_work(work),
					 struct akm_board, mute_work);
	bool mute;
	int ret;

	mutex_lock(&b->lock);
	mute = !b->output_enabled ||
		!test_bit(SNDRV_PCM_STREAM_PLAYBACK, &b->running_mask);
	ret = akm_set_mute(b, mute);
	if (ret)
		dev_err_ratelimited(b->dev, "DAC mute update failed: %d\n", ret);
	mutex_unlock(&b->lock);
}

static const unsigned int akm_rates_hz[] = {
	44100, 48000, 88200, 96000, 176400, 192000, 384000,
};

static const struct snd_pcm_hw_constraint_list akm_rate_list = {
	.count = ARRAY_SIZE(akm_rates_hz),
	.list = akm_rates_hz,
};

static int akm_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_RATE, &akm_rate_list);
}

static int akm_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct akm_board *b = snd_soc_card_get_drvdata(dai->component->card);
	const struct akm_v5_rate *rate = akm_v5_find_rate(params_rate(params));
	unsigned int bit = BIT(substream->stream);
	int ret = 0;

	if (!rate || params_channels(params) != 8 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S32_LE)
		return -EINVAL;
	mutex_lock(&b->lock);
	if (akm_v5_rate_conflicts(b->configured_mask, bit, b->rate, rate->hz)) {
		ret = -EBUSY;
		goto out;
	}
	if (!b->powered || b->rate != rate->hz) {
		ret = akm_configure(b, rate);
		if (ret) {
			b->configured_mask &= ~bit;
			goto out;
		}
	}
	b->configured_mask |= bit;
out:
	mutex_unlock(&b->lock);
	return ret;
}

static int akm_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct akm_board *b = snd_soc_card_get_drvdata(dai->component->card);
	unsigned int bit = BIT(substream->stream);
	int ret = 0;

	clear_bit(substream->stream, &b->running_mask);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		cancel_delayed_work_sync(&b->mute_work);
	mutex_lock(&b->lock);
	if (!(b->configured_mask & bit))
		goto out;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK ||
	    !(b->configured_mask & ~bit)) {
		ret = akm_set_mute(b, true);
		/* Preserve clocks throughout the longest default soft-mute ramp. */
		if (b->powered)
			msleep(100);
	}
	b->configured_mask &= ~bit;
	if (!b->configured_mask)
		akm_power_off(b);
out:
	mutex_unlock(&b->lock);
	return ret;
}

static void akm_shutdown_stream(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	akm_hw_free(substream, dai);
}

static int akm_trigger(struct snd_pcm_substream *substream, int cmd,
		       struct snd_soc_dai *dai)
{
	struct akm_board *b = snd_soc_card_get_drvdata(dai->component->card);
	bool start;

	if (READ_ONCE(b->removing))
		return -ENODEV;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		start = true;
		set_bit(substream->stream, &b->running_mask);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		start = false;
		clear_bit(substream->stream, &b->running_mask);
		break;
	default:
		return -EINVAL;
	}
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		mod_delayed_work(system_wq, &b->mute_work,
				 start ? msecs_to_jiffies(50) : 0);
	return 0;
}

static int akm_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBP_CFP)
		return -EINVAL;
	return 0;
}

static const struct snd_soc_dai_ops akm_dai_ops = {
	.startup = akm_startup,
	.shutdown = akm_shutdown_stream,
	.hw_params = akm_hw_params,
	.hw_free = akm_hw_free,
	.trigger = akm_trigger,
	.set_fmt = akm_set_fmt,
};

static struct snd_soc_dai_driver akm_dai = {
	.name = "piaoip-akm-hifi",
	.playback = {
		.stream_name = "Playback", .channels_min = 8, .channels_max = 8,
		.rates = AKM_RATES, .rate_min = 44100, .rate_max = 384000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "Capture", .channels_min = 8, .channels_max = 8,
		.rates = AKM_RATES, .rate_min = 44100, .rate_max = 384000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.symmetric_rate = 1,
	.symmetric_channels = 1,
	.symmetric_sample_bits = 1,
	.ops = &akm_dai_ops,
};

static struct akm_board *akm_control_board(struct snd_kcontrol *control)
{
	return snd_soc_card_get_drvdata(snd_soc_kcontrol_component(control)->card);
}

static int akm_volume_info(struct snd_kcontrol *control,
			   struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 8;
	info->value.integer.min = 0;
	info->value.integer.max = 255;
	return 0;
}

static int akm_volume_get(struct snd_kcontrol *control,
			  struct snd_ctl_elem_value *value)
{
	struct akm_board *b = akm_control_board(control);
	unsigned int i;

	mutex_lock(&b->lock);
	for (i = 0; i < 8; i++)
		value->value.integer.value[i] = b->volume[i];
	mutex_unlock(&b->lock);
	return 0;
}

static int akm_volume_put(struct snd_kcontrol *control,
			  struct snd_ctl_elem_value *value)
{
	struct akm_board *b = akm_control_board(control);
	unsigned int i;
	int ret = 0, changed = 0;

	for (i = 0; i < 8; i++)
		if (value->value.integer.value[i] < 0 ||
		    value->value.integer.value[i] > 255)
			return -EINVAL;
	mutex_lock(&b->lock);
	for (i = 0; i < 8; i++) {
		u8 volume = value->value.integer.value[i];

		if (volume == b->volume[i])
			continue;
		if (b->powered) {
			ret = akm_write_verify(b, b->dac_map, akm_v5_volume_regs[i], volume);
			if (ret)
				break;
		}
		b->volume[i] = volume;
		changed = 1;
	}
	mutex_unlock(&b->lock);
	return ret ? ret : changed;
}

/* private_value: 0 = user output switch, 1 = ADC HPF, 2 = ADC short delay. */
static int akm_bool_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct akm_board *b = akm_control_board(control);

	mutex_lock(&b->lock);
	switch (control->private_value) {
	case 0: value->value.integer.value[0] = b->output_enabled; break;
	case 1: value->value.integer.value[0] = b->hpf; break;
	case 2: value->value.integer.value[0] = b->adc_short_delay; break;
	}
	mutex_unlock(&b->lock);
	return 0;
}

static int akm_bool_put(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct akm_board *b = akm_control_board(control);
	bool requested = !!value->value.integer.value[0];
	bool *setting;
	int changed;

	mutex_lock(&b->lock);
	if (control->private_value && b->configured_mask) {
		mutex_unlock(&b->lock);
		return -EBUSY; /* Filter changes require a new, coordinated ADC reset. */
	}
	switch (control->private_value) {
	case 0: setting = &b->output_enabled; break;
	case 1: setting = &b->hpf; break;
	case 2: setting = &b->adc_short_delay; break;
	default:
		mutex_unlock(&b->lock);
		return -EINVAL;
	}
	changed = *setting != requested;
	*setting = requested;
	if (!control->private_value && b->powered)
		mod_delayed_work(system_wq, &b->mute_work, 0);
	mutex_unlock(&b->lock);
	return changed;
}

static int akm_status_info(struct snd_kcontrol *control, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = control->private_value ? 0x7fffffff : 384000;
	return 0;
}

static int akm_status_get(struct snd_kcontrol *control, struct snd_ctl_elem_value *value)
{
	struct akm_board *b = akm_control_board(control);

	mutex_lock(&b->lock);
	value->value.integer.value[0] = control->private_value ?
		min(b->i2c_errors, 0x7fffffffU) : b->rate;
	mutex_unlock(&b->lock);
	return 0;
}

static const DECLARE_TLV_DB_SCALE(akm_volume_tlv, -12750, 50, 1);

#define AKM_BOOL(name_, id_) { .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = (name_), .info = snd_ctl_boolean_mono_info, \
	.get = akm_bool_get, .put = akm_bool_put, .private_value = (id_) }
#define AKM_STATUS(name_, id_) { .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = (name_), .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = akm_status_info, .get = akm_status_get, .private_value = (id_) }

static const struct snd_kcontrol_new akm_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "DAC Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = akm_volume_info, .get = akm_volume_get, .put = akm_volume_put,
		.tlv.p = akm_volume_tlv,
	},
	AKM_BOOL("DAC Playback Switch", 0),
	AKM_BOOL("ADC High Pass Filter Switch", 1),
	AKM_BOOL("ADC Short Delay Filter Switch", 2),
	AKM_STATUS("Active Sample Rate", 0),
	AKM_STATUS("I2C Error Count", 1),
};

static const struct snd_soc_component_driver akm_component = {
	.name = "piaoip-akm-codec",
	.controls = akm_controls,
	.num_controls = ARRAY_SIZE(akm_controls),
	/* Card and component share a module; avoid a permanent self-reference. */
	.module_get_upon_open = 1,
};

static int akm_link_init(struct snd_soc_pcm_runtime *rtd)
{
	/* Four parallel stereo lanes: 64fs, never eight slots / 256fs. */
	return snd_soc_dai_set_bclk_ratio(snd_soc_rtd_to_cpu(rtd, 0), 64);
}

static int akm_suspend_post(struct snd_soc_card *card)
{
	struct akm_board *b = snd_soc_card_get_drvdata(card);

	cancel_delayed_work_sync(&b->mute_work);
	mutex_lock(&b->lock);
	b->suspended_rate = b->rate;
	WRITE_ONCE(b->running_mask, 0);
	if (b->powered) {
		akm_set_mute(b, true);
		msleep(100);
		akm_power_off(b);
	}
	mutex_unlock(&b->lock);
	return 0;
}

static int akm_resume_pre(struct snd_soc_card *card)
{
	struct akm_board *b = snd_soc_card_get_drvdata(card);
	const struct akm_v5_rate *rate;
	int ret = 0;

	mutex_lock(&b->lock);
	rate = akm_v5_find_rate(b->suspended_rate);
	if (b->configured_mask && rate)
		ret = akm_configure(b, rate);
	mutex_unlock(&b->lock);
	return ret;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct akm_board *b = snd_soc_card_get_drvdata(dev_get_drvdata(dev));
	ssize_t count;

	mutex_lock(&b->lock);
	count = sysfs_emit(buf,
		"version=%s\nchannels=8x8\nformat=S32_LE\nbclk_fs=64\n"
		"powered=%u\nrate=%u\nconfigured_mask=%u\nrunning_mask=%lu\n"
		"output_enabled=%u\nhpf=%u\nadc_short_delay=%u\n"
		"hpf_effective=%u\nadc_filter_selectable=%u\n"
		"i2c_errors=%u\nlast_error=%d\n",
		AKM_VERSION, b->powered, b->rate, b->configured_mask,
		READ_ONCE(b->running_mask), b->output_enabled, b->hpf,
		b->adc_short_delay,
		b->powered && b->rate <= 192000 && b->hpf,
		b->powered && b->rate <= 192000,
		b->i2c_errors, b->last_error);
	mutex_unlock(&b->lock);
	return count;
}
static DEVICE_ATTR_RO(status);

static ssize_t registers_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct akm_board *b = snd_soc_card_get_drvdata(dev_get_drvdata(dev));
	unsigned int reg, value;
	ssize_t count = 0;
	int ret;

	mutex_lock(&b->lock);
	if (!b->powered) {
		count = -EHOSTDOWN;
		goto out;
	}
	for (reg = 0; reg <= 5; reg++) {
		ret = akm_read(b, b->adc_map, reg, &value);
		if (ret) { count = ret; goto out; }
		count += sysfs_emit_at(buf, count, "ADC %02x %02x\n", reg, value);
	}
	for (reg = 0; reg <= 0x14; reg++) {
		ret = akm_read(b, b->dac_map, reg, &value);
		if (ret) { count = ret; goto out; }
		count += sysfs_emit_at(buf, count, "DAC %02x %02x\n", reg, value);
	}
out:
	mutex_unlock(&b->lock);
	return count;
}
static DEVICE_ATTR_RO(registers);

static struct attribute *akm_attrs[] = {
	&dev_attr_status.attr, &dev_attr_registers.attr, NULL,
};
ATTRIBUTE_GROUPS(akm);

static void akm_cleanup(void *data)
{
	struct akm_board *b = data;

	WRITE_ONCE(b->removing, true);
	cancel_delayed_work_sync(&b->mute_work);
	mutex_lock(&b->lock);
	if (b->powered) {
		akm_set_mute(b, true);
		msleep(100);
	}
	akm_power_off(b);
	mutex_unlock(&b->lock);
}

static void akm_put_node(void *data)
{
	of_node_put(data);
}

static const struct regmap_config akm_adc_regmap = {
	.reg_bits = 8, .val_bits = 8, .max_register = 5,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config akm_dac_regmap = {
	.reg_bits = 8, .val_bits = 8, .max_register = 0x14,
	.cache_type = REGCACHE_NONE,
};

static int akm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct i2c_adapter *adapter;
	struct akm_board *b;
	int ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->hpf = true;
	memset(b->volume, 0xff, sizeof(b->volume));
	mutex_init(&b->lock);
	INIT_DELAYED_WORK(&b->mute_work, akm_mute_work);
	/* ASoC owns device drvdata and stores struct snd_soc_card there. */
	snd_soc_card_set_drvdata(&b->card, b);
	platform_set_drvdata(pdev, &b->card);

	/* Acquire reset first. OUT_HIGH means asserted because the DT flags are LOW. */
	b->adc_reset = devm_gpiod_get(dev, "adc-reset", GPIOD_OUT_HIGH);
	if (IS_ERR(b->adc_reset))
		return dev_err_probe(dev, PTR_ERR(b->adc_reset), "ADC reset GPIO\n");
	b->dac_reset = devm_gpiod_get(dev, "dac-reset", GPIOD_OUT_HIGH);
	if (IS_ERR(b->dac_reset))
		return dev_err_probe(dev, PTR_ERR(b->dac_reset), "DAC reset GPIO\n");
	b->xo44 = devm_gpiod_get(dev, "xo44", GPIOD_OUT_LOW);
	if (IS_ERR(b->xo44))
		return dev_err_probe(dev, PTR_ERR(b->xo44), "22.5792 MHz OE GPIO\n");
	b->xo48 = devm_gpiod_get(dev, "xo48", GPIOD_OUT_LOW);
	if (IS_ERR(b->xo48))
		return dev_err_probe(dev, PTR_ERR(b->xo48), "24.576 MHz OE GPIO\n");

	node = of_parse_phandle(dev->of_node, "piaoip,i2c-bus", 0);
	if (!node)
		return -EINVAL;
	/* of_get takes both device and module refs, balanced by i2c_put_adapter. */
	adapter = of_get_i2c_adapter_by_node(node);
	of_node_put(node);
	if (!adapter)
		return -EPROBE_DEFER;
	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		i2c_put_adapter(adapter);
		return -EOPNOTSUPP;
	}
	b->adc = devm_i2c_new_dummy_device(dev, adapter, 0x10);
	b->dac = devm_i2c_new_dummy_device(dev, adapter, 0x11);
	i2c_put_adapter(adapter);
	if (IS_ERR(b->adc))
		return dev_err_probe(dev, PTR_ERR(b->adc), "claim AK5538 address 0x10\n");
	if (IS_ERR(b->dac))
		return dev_err_probe(dev, PTR_ERR(b->dac), "claim AK4458 address 0x11\n");
	b->adc_map = devm_regmap_init_i2c(b->adc, &akm_adc_regmap);
	if (IS_ERR(b->adc_map))
		return PTR_ERR(b->adc_map);
	b->dac_map = devm_regmap_init_i2c(b->dac, &akm_dac_regmap);
	if (IS_ERR(b->dac_map))
		return PTR_ERR(b->dac_map);
	ret = devm_add_action_or_reset(dev, akm_cleanup, b);
	if (ret)
		return ret;

	/* Probe real devices and verify all configuration writes, with output muted. */
	mutex_lock(&b->lock);
	ret = akm_configure(b, akm_v5_find_rate(48000));
	akm_power_off(b);
	mutex_unlock(&b->lock);
	if (ret)
		return dev_err_probe(dev, ret, "ADC/DAC I2C initialization/readback failed\n");

	node = of_parse_phandle(dev->of_node, "piaoip,cpu-dai", 0);
	if (!node)
		return -EINVAL;
	ret = devm_add_action_or_reset(dev, akm_put_node, node);
	if (ret)
		return ret;
	b->cpu.of_node = node;
	b->platform.of_node = node;
	b->codec.of_node = dev->of_node;
	b->codec.dai_name = "piaoip-akm-hifi";
	b->link.name = "AKM 8x8 I2S";
	b->link.stream_name = "AKM PCM";
	b->link.cpus = &b->cpu;
	b->link.num_cpus = 1;
	b->link.platforms = &b->platform;
	b->link.num_platforms = 1;
	b->link.codecs = &b->codec;
	b->link.num_codecs = 1;
	b->link.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			  SND_SOC_DAIFMT_CBP_CFP;
	b->link.symmetric_rate = 1;
	b->link.symmetric_channels = 1;
	b->link.symmetric_sample_bits = 1;
	b->link.init = akm_link_init;
	b->card.owner = THIS_MODULE;
	b->card.dev = dev;
	b->card.name = "PiAoIPAKM";
	b->card.long_name = "PiAoIP v5 AK5538 + AK4458 8x8";
	b->card.dai_link = &b->link;
	b->card.num_links = 1;
	b->card.suspend_post = akm_suspend_post;
	b->card.resume_pre = akm_resume_pre;
	snd_soc_card_set_drvdata(&b->card, b);
	ret = devm_snd_soc_register_component(dev, &akm_component, &akm_dai, 1);
	if (ret)
		return ret;
	ret = devm_snd_soc_register_card(dev, &b->card);
	if (ret)
		return dev_err_probe(dev, ret, "register ALSA card\n");
	dev_info(dev, "v%s: AK5538@10 + AK4458@11 verified; 8x8 I2S32; output muted\n",
		 AKM_VERSION);
	return 0;
}

static void akm_shutdown(struct platform_device *pdev)
{
	akm_cleanup(snd_soc_card_get_drvdata(platform_get_drvdata(pdev)));
}

static const struct of_device_id akm_of_match[] = {
	{ .compatible = "piaoip,akm-v5" },
	{ }
};
MODULE_DEVICE_TABLE(of, akm_of_match);

static struct platform_driver akm_driver = {
	.probe = akm_probe,
	.shutdown = akm_shutdown,
	.driver = {
		.name = "piaoip-akm",
		.of_match_table = akm_of_match,
		.dev_groups = akm_groups,
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(akm_driver);

MODULE_DESCRIPTION("PiAoIP v5 AK5538/AK4458 RP1 multichannel audio and board control");
MODULE_AUTHOR("PiAoIP project");
MODULE_LICENSE("GPL");
MODULE_VERSION(AKM_VERSION);

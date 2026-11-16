/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip SARADC, the RK3399 generation (v1 registers), as on the RK3568.
 *
 * One channel converts at a time: CTRL selects it & powers the ADC up, the
 * conversion starts DLY_PU_SOC clocks later, & the end raises the interrupt.
 * Writing 0 to CTRL powers the ADC down & clears the interrupt.
 */

#define DT_DRV_COMPAT rockchip_rk3568_saradc

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(adc_rockchip_saradc, CONFIG_ADC_LOG_LEVEL);

#define ADC_CONTEXT_USES_KERNEL_TIMER
#include "adc_context.h"

#define RK_SARADC_DATA       0x00
#define RK_SARADC_STAS       0x04
#define RK_SARADC_CTRL       0x08
#define RK_SARADC_DLY_PU_SOC 0x0c

#define RK_SARADC_CTRL_CHN_MASK   GENMASK(2, 0)
#define RK_SARADC_CTRL_POWER_CTRL BIT(3)
#define RK_SARADC_CTRL_IRQ_ENABLE BIT(5)
#define RK_SARADC_CTRL_IRQ_STATUS BIT(6)

#define RK_SARADC_RESOLUTION   10
#define RK_SARADC_NUM_CHANNELS 8
/* Converter clocks between power-up & the start of a conversion */
#define RK_SARADC_DLY_PU       8

struct adc_rockchip_saradc_config {
	DEVICE_MMIO_ROM;
	void (*irq_config)(void);
};

struct adc_rockchip_saradc_data {
	DEVICE_MMIO_RAM;
	struct adc_context ctx;
	const struct device *dev;
	uint16_t *buf;
	uint16_t *repeat_buf;
	/* Channels of the current sampling not yet converted */
	uint32_t channels;
};

static inline uint32_t saradc_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void saradc_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + reg);
}

static void saradc_start_channel(const struct device *dev, uint8_t channel)
{
	saradc_write(dev, RK_SARADC_CTRL, 0);
	saradc_write(dev, RK_SARADC_DLY_PU_SOC, RK_SARADC_DLY_PU);
	saradc_write(dev, RK_SARADC_CTRL,
		     RK_SARADC_CTRL_POWER_CTRL | RK_SARADC_CTRL_IRQ_ENABLE |
			     FIELD_PREP(RK_SARADC_CTRL_CHN_MASK, channel));
}

static int adc_rockchip_saradc_channel_setup(const struct device *dev,
					     const struct adc_channel_cfg *channel_cfg)
{
	ARG_UNUSED(dev);

	if (channel_cfg->channel_id >= RK_SARADC_NUM_CHANNELS) {
		LOG_ERR("unsupported channel %u", channel_cfg->channel_id);
		return -EINVAL;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		LOG_ERR("unsupported gain %d", channel_cfg->gain);
		return -ENOTSUP;
	}

	if (channel_cfg->reference != ADC_REF_INTERNAL) {
		LOG_ERR("unsupported reference %d", channel_cfg->reference);
		return -ENOTSUP;
	}

	if (channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		LOG_ERR("unsupported acquisition time");
		return -ENOTSUP;
	}

	if (channel_cfg->differential) {
		LOG_ERR("differential channels are not supported");
		return -ENOTSUP;
	}

	return 0;
}

static int adc_rockchip_saradc_start_read(const struct device *dev,
					  const struct adc_sequence *sequence)
{
	struct adc_rockchip_saradc_data *data = dev->data;
	size_t needed;

	if (sequence->resolution != RK_SARADC_RESOLUTION) {
		LOG_ERR("unsupported resolution %u", sequence->resolution);
		return -ENOTSUP;
	}

	if (sequence->oversampling != 0) {
		LOG_ERR("oversampling is not supported");
		return -ENOTSUP;
	}

	if (sequence->channels == 0 ||
	    (sequence->channels & ~BIT_MASK(RK_SARADC_NUM_CHANNELS)) != 0) {
		LOG_ERR("unsupported channels in mask 0x%08x", sequence->channels);
		return -EINVAL;
	}

	needed = POPCOUNT(sequence->channels) * sizeof(uint16_t);
	if (sequence->options != NULL) {
		needed *= 1 + sequence->options->extra_samplings;
	}

	if (sequence->buffer_size < needed) {
		return -ENOMEM;
	}

	data->buf = sequence->buffer;
	adc_context_start_read(&data->ctx, sequence);

	return adc_context_wait_for_completion(&data->ctx);
}

static int adc_rockchip_saradc_read_async(const struct device *dev,
					  const struct adc_sequence *sequence,
					  struct k_poll_signal *async)
{
	struct adc_rockchip_saradc_data *data = dev->data;
	int ret;

	adc_context_lock(&data->ctx, async != NULL, async);
	ret = adc_rockchip_saradc_start_read(dev, sequence);
	adc_context_release(&data->ctx, ret);

	return ret;
}

static int adc_rockchip_saradc_read(const struct device *dev, const struct adc_sequence *sequence)
{
	return adc_rockchip_saradc_read_async(dev, sequence, NULL);
}

static void adc_context_start_sampling(struct adc_context *ctx)
{
	struct adc_rockchip_saradc_data *data =
		CONTAINER_OF(ctx, struct adc_rockchip_saradc_data, ctx);

	data->channels = ctx->sequence.channels;
	data->repeat_buf = data->buf;

	saradc_start_channel(data->dev, find_lsb_set(data->channels) - 1);
}

static void adc_context_update_buffer_pointer(struct adc_context *ctx, bool repeat_sampling)
{
	struct adc_rockchip_saradc_data *data =
		CONTAINER_OF(ctx, struct adc_rockchip_saradc_data, ctx);

	if (repeat_sampling) {
		data->buf = data->repeat_buf;
	}
}

static void adc_rockchip_saradc_isr(const struct device *dev)
{
	struct adc_rockchip_saradc_data *data = dev->data;
	uint32_t ctrl = saradc_read(dev, RK_SARADC_CTRL);

	if ((ctrl & RK_SARADC_CTRL_IRQ_STATUS) == 0) {
		return;
	}

	*data->buf++ = saradc_read(dev, RK_SARADC_DATA) & BIT_MASK(RK_SARADC_RESOLUTION);

	/* Power down, which also clears the interrupt */
	saradc_write(dev, RK_SARADC_CTRL, 0);

	data->channels &= ~BIT(FIELD_GET(RK_SARADC_CTRL_CHN_MASK, ctrl));
	if (data->channels == 0) {
		adc_context_on_sampling_done(&data->ctx, dev);
		return;
	}

	saradc_start_channel(dev, find_lsb_set(data->channels) - 1);
}

static int adc_rockchip_saradc_init(const struct device *dev)
{
	const struct adc_rockchip_saradc_config *config = dev->config;
	struct adc_rockchip_saradc_data *data = dev->data;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	data->dev = dev;
	saradc_write(dev, RK_SARADC_CTRL, 0);

	config->irq_config();
	adc_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define ADC_ROCKCHIP_SARADC_INIT(n)                                                                \
	static void adc_rockchip_saradc_irq_config_##n(void)                                       \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), adc_rockchip_saradc_isr,    \
			    DEVICE_DT_INST_GET(n), DT_INST_IRQ(n, flags));                         \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static DEVICE_API(adc, adc_rockchip_saradc_api_##n) = {                                    \
		.channel_setup = adc_rockchip_saradc_channel_setup,                                \
		.read = adc_rockchip_saradc_read,                                                  \
		.ref_internal = DT_INST_PROP(n, vref_mv),                                          \
		IF_ENABLED(CONFIG_ADC_ASYNC, (.read_async = adc_rockchip_saradc_read_async,)) };      \
                                                                                                   \
	static const struct adc_rockchip_saradc_config adc_rockchip_saradc_config_##n = {          \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.irq_config = adc_rockchip_saradc_irq_config_##n,                                  \
	};                                                                                         \
                                                                                                   \
	static struct adc_rockchip_saradc_data adc_rockchip_saradc_data_##n = {                    \
		ADC_CONTEXT_INIT_TIMER(adc_rockchip_saradc_data_##n, ctx),                         \
		ADC_CONTEXT_INIT_LOCK(adc_rockchip_saradc_data_##n, ctx),                          \
		ADC_CONTEXT_INIT_SYNC(adc_rockchip_saradc_data_##n, ctx),                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, adc_rockchip_saradc_init, NULL, &adc_rockchip_saradc_data_##n,    \
			      &adc_rockchip_saradc_config_##n, POST_KERNEL,                        \
			      CONFIG_ADC_INIT_PRIORITY, &adc_rockchip_saradc_api_##n);

DT_INST_FOREACH_STATUS_OKAY(ADC_ROCKCHIP_SARADC_INIT)

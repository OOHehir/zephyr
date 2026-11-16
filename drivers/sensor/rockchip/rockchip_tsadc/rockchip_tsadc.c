/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip RK3568 TSADC: two on-die temperature sensors, at the CPU & the GPU.
 *
 * A fetch converts each channel once in user-defined mode: powering the ADC up
 * starts a conversion inter_pd_soc clocks later, & the end sets EOC in
 * TSADC_INT_PD. The controller's auto mode (& with it the hardware shutdown
 * output, TSHUT) is not used: on an RK3568B2 it never completed a conversion.
 */

#define DT_DRV_COMPAT rockchip_rk3568_tsadc

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/rockchip_tsadc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rockchip_tsadc, CONFIG_SENSOR_LOG_LEVEL);

#define RK_TSADC_USER_CON 0x00
#define RK_TSADC_INT_PD   0x0c
#define RK_TSADC_DATA(ch) (0x20 + (ch) * 4)

#define RK_TSADC_USER_CON_SRC_SEL  GENMASK(2, 0)
#define RK_TSADC_USER_CON_POWER_UP BIT(3)
#define RK_TSADC_USER_CON_PD_SOC   GENMASK(14, 6)
#define RK_TSADC_INT_PD_EOC        BIT(16)
#define RK_TSADC_DATA_MASK         GENMASK(11, 0)

/* Power-up to start of conversion: at least 90 us, 63 clocks at 706 kHz */
#define RK_TSADC_PD_SOC        63
/* A conversion takes about 110 us */
#define RK_TSADC_CONVERSION_US 1000

/* GRF_TSADC_CON: sensor enable & the bandgap trims, each with its write-enable bit */
#define RK_GRF_TSADC_CON      0x0600
#define RK_GRF_TSADC_ANA_REG0 0x00010001
#define RK_GRF_TSADC_ANA_REG1 0x00020002
#define RK_GRF_TSADC_ANA_REG2 0x00040004
#define RK_GRF_TSADC_TSEN     0x01000100

/*
 * CRU_CLKSEL_CON51: clk_tsadc_tsen = xin24m / 2 = 12 MHz (at most 17 MHz),
 * clk_tsadc = clk_tsadc_tsen / 17 = 706 kHz. Out of reset they run at 24 MHz
 * & 1.2 MHz.
 */
#define RK_CRU_CLKSEL_CON51 0x01cc
#define RK_CRU_TSADC_CLKSEL                                                                        \
	((GENMASK(14, 8) | GENMASK(5, 4) | GENMASK(2, 0)) << 16 | (16 << 8) | (0 << 4) | (1 << 0))

#define RK_TSADC_NUM_CHANNELS 2

struct rockchip_tsadc_config {
	DEVICE_MMIO_ROM;
	uintptr_t grf_phys;
	uintptr_t cru_phys;
	struct reset_dt_spec resets[3];
};

struct rockchip_tsadc_data {
	DEVICE_MMIO_RAM;
	struct k_mutex lock;
	uint16_t code[RK_TSADC_NUM_CHANNELS];
};

/*
 * Code to temperature, from the RK3568 TRM; linear between entries. The ADC
 * output falls as the temperature rises; the codes here are 4096 minus it.
 */
static const struct {
	uint16_t code;
	int16_t temp_c;
} rk3568_tsadc_table[] = {
	{1584, -40}, {1620, -35}, {1652, -30}, {1688, -25}, {1720, -20}, {1756, -15}, {1788, -10},
	{1824, -5},  {1856, 0},   {1892, 5},   {1924, 10},  {1956, 15},  {1992, 20},  {2024, 25},
	{2060, 30},  {2092, 35},  {2128, 40},  {2160, 45},  {2196, 50},  {2228, 55},  {2264, 60},
	{2300, 65},  {2332, 70},  {2368, 75},  {2400, 80},  {2436, 85},  {2468, 90},  {2500, 95},
	{2536, 100}, {2572, 105}, {2604, 110}, {2636, 115}, {2672, 120}, {2704, 125},
};

static int rockchip_tsadc_code_to_mdeg(uint16_t code, int32_t *mdeg)
{
	for (size_t i = 1; i < ARRAY_SIZE(rk3568_tsadc_table); i++) {
		int32_t c0 = rk3568_tsadc_table[i - 1].code;
		int32_t c1 = rk3568_tsadc_table[i].code;
		int32_t t0 = rk3568_tsadc_table[i - 1].temp_c * 1000;
		int32_t t1 = rk3568_tsadc_table[i].temp_c * 1000;

		if (code >= c0 && code <= c1) {
			*mdeg = t0 + (t1 - t0) * ((int32_t)code - c0) / (c1 - c0);
			return 0;
		}
	}

	/* Outside -40..125 C: a bad reading */
	return -EIO;
}

static int rockchip_tsadc_convert(const struct device *dev, int ch, uint16_t *code)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	bool done;

	sys_write32(RK_TSADC_INT_PD_EOC, base + RK_TSADC_INT_PD);
	sys_write32(FIELD_PREP(RK_TSADC_USER_CON_PD_SOC, RK_TSADC_PD_SOC) |
			    RK_TSADC_USER_CON_POWER_UP | FIELD_PREP(RK_TSADC_USER_CON_SRC_SEL, ch),
		    base + RK_TSADC_USER_CON);

	done = WAIT_FOR(sys_read32(base + RK_TSADC_INT_PD) & RK_TSADC_INT_PD_EOC,
			RK_TSADC_CONVERSION_US, k_busy_wait(10));

	/* Power down; the next power-up starts the next conversion */
	sys_write32(0, base + RK_TSADC_USER_CON);
	sys_write32(RK_TSADC_INT_PD_EOC, base + RK_TSADC_INT_PD);

	if (!done) {
		LOG_ERR("channel %d: conversion timed out", ch);
		return -ETIMEDOUT;
	}

	*code = BIT(12) - (sys_read32(base + RK_TSADC_DATA(ch)) & RK_TSADC_DATA_MASK);

	return 0;
}

static int rockchip_tsadc_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct rockchip_tsadc_data *data = dev->data;
	int first, last;
	int ret = 0;

	if (chan == SENSOR_CHAN_ALL) {
		first = 0;
		last = RK_TSADC_NUM_CHANNELS - 1;
	} else if (chan == SENSOR_CHAN_DIE_TEMP) {
		first = last = 0;
	} else if (chan == (enum sensor_channel)SENSOR_CHAN_ROCKCHIP_TSADC_GPU) {
		first = last = 1;
	} else {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (int i = first; i <= last && ret == 0; i++) {
		ret = rockchip_tsadc_convert(dev, i, &data->code[i]);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int rockchip_tsadc_channel_get(const struct device *dev, enum sensor_channel chan,
				      struct sensor_value *val)
{
	struct rockchip_tsadc_data *data = dev->data;
	int32_t mdeg;
	int ch;
	int ret;

	if (chan == SENSOR_CHAN_DIE_TEMP) {
		ch = 0;
	} else if (chan == (enum sensor_channel)SENSOR_CHAN_ROCKCHIP_TSADC_GPU) {
		ch = 1;
	} else {
		return -ENOTSUP;
	}

	ret = rockchip_tsadc_code_to_mdeg(data->code[ch], &mdeg);
	if (ret < 0) {
		LOG_DBG("channel %d code %u out of range", ch, data->code[ch]);
		return ret;
	}

	return sensor_value_from_milli(val, mdeg);
}

static DEVICE_API(sensor, rockchip_tsadc_api) = {
	.sample_fetch = rockchip_tsadc_sample_fetch,
	.channel_get = rockchip_tsadc_channel_get,
};

static int rockchip_tsadc_init(const struct device *dev)
{
	const struct rockchip_tsadc_config *cfg = dev->config;
	struct rockchip_tsadc_data *data = dev->data;
	mm_reg_t grf, cru;
	int ret;

	k_mutex_init(&data->lock);

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	device_map(&grf, cfg->grf_phys, 0x1000, K_MEM_CACHE_NONE);
	device_map(&cru, cfg->cru_phys, 0x1000, K_MEM_CACHE_NONE);

	sys_write32(RK_CRU_TSADC_CLKSEL, cru + RK_CRU_CLKSEL_CON51);

	for (int i = 0; i < ARRAY_SIZE(cfg->resets); i++) {
		ret = reset_line_assert_dt(&cfg->resets[i]);
		if (ret < 0) {
			return ret;
		}
	}
	k_busy_wait(10);
	for (int i = 0; i < ARRAY_SIZE(cfg->resets); i++) {
		ret = reset_line_deassert_dt(&cfg->resets[i]);
		if (ret < 0) {
			return ret;
		}
	}

	/* TRM: at least 10 us from TSEN to the trims, 90 us from the trims to use */
	sys_write32(RK_GRF_TSADC_TSEN, grf + RK_GRF_TSADC_CON);
	k_busy_wait(15);
	sys_write32(RK_GRF_TSADC_ANA_REG0, grf + RK_GRF_TSADC_CON);
	sys_write32(RK_GRF_TSADC_ANA_REG1, grf + RK_GRF_TSADC_CON);
	sys_write32(RK_GRF_TSADC_ANA_REG2, grf + RK_GRF_TSADC_CON);
	k_busy_wait(100);

	return 0;
}

#define ROCKCHIP_TSADC_INIT(n)                                                                     \
	static const struct rockchip_tsadc_config rockchip_tsadc_config_##n = {                    \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.grf_phys = DT_REG_ADDR(DT_INST_PHANDLE(n, rockchip_grf)),                         \
		.cru_phys = DT_REG_ADDR(DT_INST_RESET_CTLR(n)),                                    \
		.resets = {RESET_DT_SPEC_INST_GET_BY_IDX(n, 0),                                    \
			   RESET_DT_SPEC_INST_GET_BY_IDX(n, 1),                                    \
			   RESET_DT_SPEC_INST_GET_BY_IDX(n, 2)},                                   \
	};                                                                                         \
	static struct rockchip_tsadc_data rockchip_tsadc_data_##n;                                 \
	SENSOR_DEVICE_DT_INST_DEFINE(n, rockchip_tsadc_init, NULL, &rockchip_tsadc_data_##n,       \
				     &rockchip_tsadc_config_##n, POST_KERNEL,                      \
				     CONFIG_SENSOR_INIT_PRIORITY, &rockchip_tsadc_api);

DT_INST_FOREACH_STATUS_OKAY(ROCKCHIP_TSADC_INIT)

/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip PWM (RK3328 generation, as on the RK3568). Each node is one channel
 * of a four-channel block, with its own counter, period & duty registers.
 *
 * Setting LOCK in CTRL holds the period & duty registers back; clearing it
 * applies the new values (& polarity) together at the end of the period.
 */

#define DT_DRV_COMPAT rockchip_rk3568_pwm

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pwm_rockchip, CONFIG_PWM_LOG_LEVEL);

#define RK_PWM_CNT    0x00
#define RK_PWM_PERIOD 0x04
#define RK_PWM_DUTY   0x08
#define RK_PWM_CTRL   0x0c

#define RK_PWM_CTRL_EN            BIT(0)
#define RK_PWM_CTRL_MODE_MASK     GENMASK(2, 1)
#define RK_PWM_CTRL_CONTINUOUS    FIELD_PREP(RK_PWM_CTRL_MODE_MASK, 1)
#define RK_PWM_CTRL_DUTY_POS      BIT(3) /* output high during the duty cycle */
#define RK_PWM_CTRL_INACTIVE_POS  BIT(4) /* output high while disabled */
#define RK_PWM_CTRL_OUTPUT_CENTER BIT(5)
#define RK_PWM_CTRL_LOCK          BIT(6)
#define RK_PWM_CTRL_LP_EN         BIT(8)

struct pwm_rockchip_config {
	DEVICE_MMIO_ROM;
	const struct pinctrl_dev_config *pcfg;
	uint32_t clock_frequency;
};

struct pwm_rockchip_data {
	DEVICE_MMIO_RAM;
};

static inline uint32_t pwm_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void pwm_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + reg);
}

static int pwm_rockchip_set_cycles(const struct device *dev, uint32_t channel,
				   uint32_t period_cycles, uint32_t pulse_cycles, pwm_flags_t flags)
{
	uint32_t ctrl;

	if (channel != 0) {
		LOG_ERR("invalid channel %u", channel);
		return -EINVAL;
	}

	ctrl = pwm_read(dev, RK_PWM_CTRL);

	if (period_cycles == 0) {
		pwm_write(dev, RK_PWM_CTRL, ctrl & ~RK_PWM_CTRL_EN);
		return 0;
	}

	if ((ctrl & RK_PWM_CTRL_EN) != 0) {
		pwm_write(dev, RK_PWM_CTRL, ctrl | RK_PWM_CTRL_LOCK);
	}

	pwm_write(dev, RK_PWM_PERIOD, period_cycles);
	pwm_write(dev, RK_PWM_DUTY, pulse_cycles);

	ctrl &= ~(RK_PWM_CTRL_MODE_MASK | RK_PWM_CTRL_DUTY_POS | RK_PWM_CTRL_INACTIVE_POS |
		  RK_PWM_CTRL_OUTPUT_CENTER | RK_PWM_CTRL_LOCK | RK_PWM_CTRL_LP_EN);
	ctrl |= RK_PWM_CTRL_EN | RK_PWM_CTRL_CONTINUOUS;
	if ((flags & PWM_POLARITY_INVERTED) != 0) {
		ctrl |= RK_PWM_CTRL_INACTIVE_POS;
	} else {
		ctrl |= RK_PWM_CTRL_DUTY_POS;
	}

	pwm_write(dev, RK_PWM_CTRL, ctrl);

	return 0;
}

static int pwm_rockchip_get_cycles_per_sec(const struct device *dev, uint32_t channel,
					   uint64_t *cycles)
{
	const struct pwm_rockchip_config *config = dev->config;

	if (channel != 0) {
		return -EINVAL;
	}

	*cycles = config->clock_frequency;

	return 0;
}

static DEVICE_API(pwm, pwm_rockchip_api) = {
	.set_cycles = pwm_rockchip_set_cycles,
	.get_cycles_per_sec = pwm_rockchip_get_cycles_per_sec,
};

static int pwm_rockchip_init(const struct device *dev)
{
	const struct pwm_rockchip_config *config = dev->config;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

#define PWM_ROCKCHIP_INIT(n)                                                                       \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static const struct pwm_rockchip_config pwm_rockchip_config_##n = {                        \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.clock_frequency = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),            \
	};                                                                                         \
                                                                                                   \
	static struct pwm_rockchip_data pwm_rockchip_data_##n;                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, pwm_rockchip_init, NULL, &pwm_rockchip_data_##n,                  \
			      &pwm_rockchip_config_##n, POST_KERNEL, CONFIG_PWM_INIT_PRIORITY,     \
			      &pwm_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_ROCKCHIP_INIT)

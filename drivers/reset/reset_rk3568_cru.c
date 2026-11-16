/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip CRU soft resets. Reset id N is bit N % 16 of SOFTRST_CON(N / 16);
 * the upper half of each register is the write enable of the lower half.
 */

#define DT_DRV_COMPAT rockchip_rk3568_cru

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define RK_SOFTRST_PER_REG 16

struct reset_rockchip_config {
	DEVICE_MMIO_ROM;
	uint32_t softrst_offset;
	uint32_t softrst_count;
};

struct reset_rockchip_data {
	DEVICE_MMIO_RAM;
};

static int reset_rockchip_reg(const struct device *dev, uint32_t id, mem_addr_t *reg)
{
	const struct reset_rockchip_config *config = dev->config;

	if (id / RK_SOFTRST_PER_REG >= config->softrst_count) {
		return -EINVAL;
	}

	*reg = DEVICE_MMIO_GET(dev) + config->softrst_offset + (id / RK_SOFTRST_PER_REG) * 4;

	return 0;
}

static int reset_rockchip_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	mem_addr_t reg;
	int ret;

	ret = reset_rockchip_reg(dev, id, &reg);
	if (ret < 0) {
		return ret;
	}

	*status = (sys_read32(reg) & BIT(id % RK_SOFTRST_PER_REG)) != 0;

	return 0;
}

static int reset_rockchip_set(const struct device *dev, uint32_t id, bool assert)
{
	uint32_t bit = BIT(id % RK_SOFTRST_PER_REG);
	mem_addr_t reg;
	int ret;

	ret = reset_rockchip_reg(dev, id, &reg);
	if (ret < 0) {
		return ret;
	}

	sys_write32((bit << 16) | (assert ? bit : 0), reg);

	return 0;
}

static int reset_rockchip_line_assert(const struct device *dev, uint32_t id)
{
	return reset_rockchip_set(dev, id, true);
}

static int reset_rockchip_line_deassert(const struct device *dev, uint32_t id)
{
	return reset_rockchip_set(dev, id, false);
}

static int reset_rockchip_line_toggle(const struct device *dev, uint32_t id)
{
	int ret;

	ret = reset_rockchip_set(dev, id, true);
	if (ret < 0) {
		return ret;
	}

	k_busy_wait(10);

	return reset_rockchip_set(dev, id, false);
}

static DEVICE_API(reset, reset_rockchip_api) = {
	.status = reset_rockchip_status,
	.line_assert = reset_rockchip_line_assert,
	.line_deassert = reset_rockchip_line_deassert,
	.line_toggle = reset_rockchip_line_toggle,
};

static int reset_rockchip_init(const struct device *dev)
{
	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	return 0;
}

#define RESET_ROCKCHIP_INIT(n)                                                                     \
	static const struct reset_rockchip_config reset_rockchip_config_##n = {                    \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.softrst_offset = 0x400,                                                           \
		.softrst_count = 30,                                                               \
	};                                                                                         \
                                                                                                   \
	static struct reset_rockchip_data reset_rockchip_data_##n;                                 \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, reset_rockchip_init, NULL, &reset_rockchip_data_##n,              \
			      &reset_rockchip_config_##n, PRE_KERNEL_1,                            \
			      CONFIG_RESET_INIT_PRIORITY, &reset_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(RESET_ROCKCHIP_INIT)

/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip RK3568 pin controller: IOMUX, pin set routes, pull & drive strength.
 *
 * GPIO0's registers live in the PMU GRF, GPIO1-GPIO4's in the system GRF.
 * Every register is write-masked (the upper 16 bits enable the lower 16), so
 * pins are configured without a read-modify-write.
 */

#define DT_DRV_COMPAT rockchip_rk3568_pinctrl

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/util.h>

#define RK3568_NUM_BANKS 5

/* Register blocks per bank: GPIO0 in the PMU GRF, GPIO1-4 in the system GRF */
#define RK3568_IOMUX_PMU  0x0000
#define RK3568_IOMUX_GRF  0x0000
#define RK3568_IOMUX_STEP 0x20
#define RK3568_PULL_PMU   0x0020
#define RK3568_PULL_GRF   0x0080
#define RK3568_PULL_STEP  0x10
#define RK3568_DRV_PMU    0x0070
#define RK3568_DRV_GRF    0x0200
#define RK3568_DRV_STEP   0x40
#define RK3568_IOFUNC_SEL 0x0300
#define RK3568_ROUTE_PMU  0x0110

struct pinctrl_rockchip_config {
	DEVICE_MMIO_NAMED_ROM(pmu);
	DEVICE_MMIO_NAMED_ROM(grf);
};

struct pinctrl_rockchip_data {
	DEVICE_MMIO_NAMED_RAM(pmu);
	DEVICE_MMIO_NAMED_RAM(grf);
};

#define DEV_CFG(dev)  ((const struct pinctrl_rockchip_config *)(dev)->config)
#define DEV_DATA(dev) ((struct pinctrl_rockchip_data *)(dev)->data)

static const struct device *const pinctrl_dev = DEVICE_DT_INST_GET(0);

/* Write @val into the @width-bit field at @shift of a write-masked register */
static void rk_write_field(mem_addr_t reg, uint32_t shift, uint32_t width, uint32_t val)
{
	uint32_t mask = BIT_MASK(width) << shift;

	sys_write32((mask << 16) | ((val << shift) & mask), reg);
}

/* Base of the @bank's block whose GPIO0 copy is at @pmu_off & GPIO1's at @grf_off */
static mem_addr_t rk_bank_base(uint8_t bank, uint32_t pmu_off, uint32_t grf_off, uint32_t step)
{
	if (bank == 0) {
		return DEVICE_MMIO_NAMED_GET(pinctrl_dev, pmu) + pmu_off;
	}

	return DEVICE_MMIO_NAMED_GET(pinctrl_dev, grf) + grf_off + (bank - 1) * step;
}

static void rk3568_set_mux(uint8_t bank, uint8_t pin, uint8_t func)
{
	/* 4 pins per register, 4 bits per pin */
	mem_addr_t reg = rk_bank_base(bank, RK3568_IOMUX_PMU, RK3568_IOMUX_GRF, RK3568_IOMUX_STEP);

	rk_write_field(reg + (pin / 4) * 4, (pin % 4) * 4, 4, func);
}

static void rk3568_set_route(uint32_t pinmux)
{
	uint8_t route = RK_PINMUX_ROUTE_REG(pinmux);
	mem_addr_t reg;

	if (route == RK_ROUTE_NONE) {
		return;
	}

	if (route == RK_ROUTE_PMU_0110) {
		reg = DEVICE_MMIO_NAMED_GET(pinctrl_dev, pmu) + RK3568_ROUTE_PMU;
	} else {
		reg = DEVICE_MMIO_NAMED_GET(pinctrl_dev, grf) + RK3568_IOFUNC_SEL +
		      (route - RK_IOFUNC_SEL0) * 4;
	}

	rk_write_field(reg, RK_PINMUX_ROUTE_SHIFT(pinmux), RK_PINMUX_ROUTE_WIDTH(pinmux),
		       RK_PINMUX_ROUTE_VAL(pinmux));
}

static void rk3568_set_pull(uint8_t bank, uint8_t pin, uint8_t pull)
{
	/* 8 pins per register, 2 bits per pin */
	mem_addr_t reg = rk_bank_base(bank, RK3568_PULL_PMU, RK3568_PULL_GRF, RK3568_PULL_STEP);

	/* The TRM encodes pull-up as 3, not 1, on GPIO0_D3-D6 */
	if (bank == 0 && pin >= 27 && pin <= 30 && pull == ROCKCHIP_PULL_UP) {
		pull = 3;
	}

	rk_write_field(reg + (pin / 8) * 4, (pin % 8) * 2, 2, pull);
}

static void rk3568_set_drive(uint8_t bank, uint8_t pin, uint8_t level)
{
	/*
	 * 2 pins per register, 8 bits per pin; level n enables n + 1 drivers.
	 * GPIO1_C5, GPIO2_A2/B0, GPIO3_A0/A6 & GPIO4_A0 have a second register
	 * for drivers 7 & 8, which levels 0-5 never use.
	 */
	mem_addr_t reg = rk_bank_base(bank, RK3568_DRV_PMU, RK3568_DRV_GRF, RK3568_DRV_STEP);

	rk_write_field(reg + (pin / 2) * 4, (pin % 2) * 8, 8, BIT_MASK(level + 1));
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0; i < pin_cnt; i++) {
		uint8_t bank = RK_PINMUX_BANK(pins[i].pinmux);
		uint8_t pin = RK_PINMUX_PIN(pins[i].pinmux);

		if (bank >= RK3568_NUM_BANKS ||
		    (pins[i].drive > 5 && pins[i].drive != ROCKCHIP_PINCTRL_KEEP)) {
			return -EINVAL;
		}

		/* Select the pin set before the IOMUX, as Linux does */
		rk3568_set_route(pins[i].pinmux);
		rk3568_set_mux(bank, pin, RK_PINMUX_FUNC(pins[i].pinmux));

		if (pins[i].pull != ROCKCHIP_PINCTRL_KEEP) {
			rk3568_set_pull(bank, pin, pins[i].pull);
		}
		if (pins[i].drive != ROCKCHIP_PINCTRL_KEEP) {
			rk3568_set_drive(bank, pin, pins[i].drive);
		}
	}

	return 0;
}

static int pinctrl_rockchip_init(const struct device *dev)
{
	DEVICE_MMIO_NAMED_MAP(dev, pmu, K_MEM_CACHE_NONE);
	DEVICE_MMIO_NAMED_MAP(dev, grf, K_MEM_CACHE_NONE);

	return 0;
}

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one pin controller per SoC");

static const struct pinctrl_rockchip_config pinctrl_rockchip_config_0 = {
	DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(pmu, DT_DRV_INST(0)),
	DEVICE_MMIO_NAMED_ROM_INIT_BY_NAME(grf, DT_DRV_INST(0)),
};

static struct pinctrl_rockchip_data pinctrl_rockchip_data_0;

DEVICE_DT_INST_DEFINE(0, pinctrl_rockchip_init, NULL, &pinctrl_rockchip_data_0,
		      &pinctrl_rockchip_config_0, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		      NULL);

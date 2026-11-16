/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip GPIO controller, version 2 (RK356x, RK3588, ...).
 *
 * Every 32-bit control register is split into a _L half (pins 0-15) and an
 * _H half (pins 16-31). The upper 16 bits of a write are a write-enable
 * mask for the lower 16 bits, so single bits are changed without a
 * read-modify-write and without a lock.
 */

#define DT_DRV_COMPAT rockchip_gpio_bank

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#define RK_GPIO_SWPORT_DR    0x00
#define RK_GPIO_SWPORT_DDR   0x08
#define RK_GPIO_INT_EN       0x10
#define RK_GPIO_INT_MASK     0x18
#define RK_GPIO_INT_TYPE     0x20
#define RK_GPIO_INT_POLARITY 0x28
#define RK_GPIO_INT_BOTHEDGE 0x30
#define RK_GPIO_INT_STATUS   0x50
#define RK_GPIO_PORT_EOI     0x60
#define RK_GPIO_EXT_PORT     0x70

struct gpio_rockchip_config {
	struct gpio_driver_config common;
	DEVICE_MMIO_NAMED_ROM(reg_base);
	void (*irq_config_func)(void);
#ifdef CONFIG_PINCTRL
	/* Optional: pull & mux of the bank's pins, which live in the GRF */
	const struct pinctrl_dev_config *pcfg;
#endif
};

struct gpio_rockchip_data {
	struct gpio_driver_data common;
	DEVICE_MMIO_NAMED_RAM(reg_base);
	sys_slist_t cb;
};

#define DEV_CFG(dev)  ((const struct gpio_rockchip_config *)(dev)->config)
#define DEV_DATA(dev) ((struct gpio_rockchip_data *)(dev)->data)

/* Write @value to the bits of @mask in a write-masked _L/_H register pair */
static void gpio_rockchip_write_masked(const struct device *port, uint32_t reg, uint32_t mask,
				       uint32_t value)
{
	mem_addr_t base = DEVICE_MMIO_NAMED_GET(port, reg_base);
	uint32_t lo = mask & 0xffff;
	uint32_t hi = mask >> 16;

	if (lo != 0) {
		sys_write32((lo << 16) | (value & lo), base + reg);
	}
	if (hi != 0) {
		sys_write32((hi << 16) | ((value >> 16) & hi), base + reg + 4);
	}
}

static uint32_t gpio_rockchip_read_pair(const struct device *port, uint32_t reg)
{
	mem_addr_t base = DEVICE_MMIO_NAMED_GET(port, reg_base);

	return (sys_read32(base + reg) & 0xffff) | (sys_read32(base + reg + 4) << 16);
}

static int gpio_rockchip_pin_configure(const struct device *port, gpio_pin_t pin,
				       gpio_flags_t flags)
{
	uint32_t bit = BIT(pin);

	/* Pull, drive strength and open drain live in the GRF (pinctrl) */
	if ((flags & (GPIO_SINGLE_ENDED | GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_OUTPUT) != 0) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
			gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, bit, bit);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
			gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, bit, 0);
		}
		gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DDR, bit, bit);
	} else if ((flags & GPIO_INPUT) != 0) {
		gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DDR, bit, 0);
	} else {
		/* Disconnected: leave the pin as an input */
		gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DDR, bit, 0);
	}

	return 0;
}

static int gpio_rockchip_port_get_raw(const struct device *port, gpio_port_value_t *value)
{
	*value = sys_read32(DEVICE_MMIO_NAMED_GET(port, reg_base) + RK_GPIO_EXT_PORT);

	return 0;
}

static int gpio_rockchip_port_set_masked_raw(const struct device *port, gpio_port_pins_t mask,
					     gpio_port_value_t value)
{
	gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, mask, value);

	return 0;
}

static int gpio_rockchip_port_set_bits_raw(const struct device *port, gpio_port_pins_t pins)
{
	gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, pins, pins);

	return 0;
}

static int gpio_rockchip_port_clear_bits_raw(const struct device *port, gpio_port_pins_t pins)
{
	gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, pins, 0);

	return 0;
}

static int gpio_rockchip_port_toggle_bits(const struct device *port, gpio_port_pins_t pins)
{
	uint32_t dr = gpio_rockchip_read_pair(port, RK_GPIO_SWPORT_DR);

	gpio_rockchip_write_masked(port, RK_GPIO_SWPORT_DR, pins, ~dr);

	return 0;
}

static int gpio_rockchip_pin_interrupt_configure(const struct device *port, gpio_pin_t pin,
						 enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	uint32_t bit = BIT(pin);
	bool edge = (mode == GPIO_INT_MODE_EDGE);
	bool both = edge && (trig == GPIO_INT_TRIG_BOTH);
	bool high = (trig & GPIO_INT_HIGH_1) != 0;

	if (mode != GPIO_INT_MODE_DISABLED && mode != GPIO_INT_MODE_LEVEL &&
	    mode != GPIO_INT_MODE_EDGE) {
		return -ENOTSUP;
	}

	/* Disable and mask while reconfiguring */
	gpio_rockchip_write_masked(port, RK_GPIO_INT_EN, bit, 0);
	gpio_rockchip_write_masked(port, RK_GPIO_INT_MASK, bit, bit);

	if (mode == GPIO_INT_MODE_DISABLED) {
		return 0;
	}

	if (mode == GPIO_INT_MODE_LEVEL && trig == GPIO_INT_TRIG_BOTH) {
		return -ENOTSUP;
	}

	gpio_rockchip_write_masked(port, RK_GPIO_INT_TYPE, bit, edge ? bit : 0);
	gpio_rockchip_write_masked(port, RK_GPIO_INT_POLARITY, bit, high ? bit : 0);
	gpio_rockchip_write_masked(port, RK_GPIO_INT_BOTHEDGE, bit, both ? bit : 0);

	/* Drop anything latched while the pin was being set up */
	gpio_rockchip_write_masked(port, RK_GPIO_PORT_EOI, bit, bit);

	gpio_rockchip_write_masked(port, RK_GPIO_INT_MASK, bit, 0);
	gpio_rockchip_write_masked(port, RK_GPIO_INT_EN, bit, bit);

	return 0;
}

static int gpio_rockchip_manage_callback(const struct device *port, struct gpio_callback *cb,
					 bool set)
{
	struct gpio_rockchip_data *data = port->data;

	return gpio_manage_callback(&data->cb, cb, set);
}

static uint32_t gpio_rockchip_get_pending_int(const struct device *port)
{
	return sys_read32(DEVICE_MMIO_NAMED_GET(port, reg_base) + RK_GPIO_INT_STATUS);
}

static void gpio_rockchip_isr(const struct device *port)
{
	struct gpio_rockchip_data *data = port->data;
	uint32_t pending = gpio_rockchip_get_pending_int(port);

	/* EOI clears edge interrupts; level interrupts reassert while active */
	gpio_rockchip_write_masked(port, RK_GPIO_PORT_EOI, pending, pending);

	gpio_fire_callbacks(&data->cb, port, pending);
}

static int gpio_rockchip_init(const struct device *port)
{
	const struct gpio_rockchip_config *cfg = port->config;

	DEVICE_MMIO_NAMED_MAP(port, reg_base, K_MEM_CACHE_NONE);

#ifdef CONFIG_PINCTRL
	if (cfg->pcfg != NULL) {
		int ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);

		if (ret < 0) {
			return ret;
		}
	}
#endif

	cfg->irq_config_func();

	return 0;
}

static DEVICE_API(gpio, gpio_rockchip_api) = {
	.pin_configure = gpio_rockchip_pin_configure,
	.port_get_raw = gpio_rockchip_port_get_raw,
	.port_set_masked_raw = gpio_rockchip_port_set_masked_raw,
	.port_set_bits_raw = gpio_rockchip_port_set_bits_raw,
	.port_clear_bits_raw = gpio_rockchip_port_clear_bits_raw,
	.port_toggle_bits = gpio_rockchip_port_toggle_bits,
	.pin_interrupt_configure = gpio_rockchip_pin_interrupt_configure,
	.manage_callback = gpio_rockchip_manage_callback,
	.get_pending_int = gpio_rockchip_get_pending_int,
};

#define GPIO_ROCKCHIP_HAS_PINCTRL(n)                                                               \
	UTIL_AND(IS_ENABLED(CONFIG_PINCTRL), DT_INST_PINCTRL_HAS_NAME(n, default))

#define GPIO_ROCKCHIP_INIT(n)                                                                      \
	IF_ENABLED(GPIO_ROCKCHIP_HAS_PINCTRL(n), (PINCTRL_DT_INST_DEFINE(n);))                                                                                 \
                                                                                                   \
	static void gpio_rockchip_irq_config_##n(void)                                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), gpio_rockchip_isr,          \
			    DEVICE_DT_INST_GET(n), DT_INST_IRQ(n, flags));                         \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct gpio_rockchip_config gpio_rockchip_config_##n = {                      \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(n),                                      \
		DEVICE_MMIO_NAMED_ROM_INIT(reg_base, DT_DRV_INST(n)),                              \
		.irq_config_func = gpio_rockchip_irq_config_##n,                                   \
		IF_ENABLED(CONFIG_PINCTRL,                                                         \
			   (.pcfg = COND_CODE_1(GPIO_ROCKCHIP_HAS_PINCTRL(n),                      \
						(PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL)),)) };                               \
                                                                                                   \
	static struct gpio_rockchip_data gpio_rockchip_data_##n;                                   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, gpio_rockchip_init, NULL, &gpio_rockchip_data_##n,                \
			      &gpio_rockchip_config_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,  \
			      &gpio_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_ROCKCHIP_INIT)

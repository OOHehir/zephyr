/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip I2C controller (RK3399 & later), controller mode, interrupt driven.
 *
 * The controller sends up to 32 bytes per command from TXDATA, or receives up
 * to 32 into RXDATA. A write sends the address byte itself as the first data
 * byte; a read lets the controller send the address from MRXADDR.
 *
 * It cannot issue a START once a write has left the bus held, so a repeated
 * start is done in hardware only for a write of up to 3 bytes followed by a
 * read (register mode: MRXRADDR holds the bytes). Any other repeated start is
 * sent as a STOP & a START, which is the same on a single-controller bus.
 */

#define DT_DRV_COMPAT rockchip_rk3568_i2c

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_rockchip, CONFIG_I2C_LOG_LEVEL);

#include "i2c-priv.h"

#define RK_I2C_CON      0x000
#define RK_I2C_CLKDIV   0x004
#define RK_I2C_MRXADDR  0x008
#define RK_I2C_MRXRADDR 0x00c
#define RK_I2C_MTXCNT   0x010
#define RK_I2C_MRXCNT   0x014
#define RK_I2C_IEN      0x018
#define RK_I2C_IPD      0x01c
#define RK_I2C_TXDATA   0x100
#define RK_I2C_RXDATA   0x200

#define RK_I2C_CON_EN      BIT(0)
#define RK_I2C_CON_MOD_TX  (0 << 1)
#define RK_I2C_CON_MOD_TRX (1 << 1) /* send MRXADDR, then receive */
#define RK_I2C_CON_MOD_RX  (2 << 1)
#define RK_I2C_CON_START   BIT(3)
#define RK_I2C_CON_STOP    BIT(4)
#define RK_I2C_CON_LASTACK BIT(5) /* NAK the last byte received */
#define RK_I2C_CON_ACTACK  BIT(6) /* end the command on a NAK */
#define RK_I2C_CON_TUNING  GENMASK(15, 8)

/* Interrupt enable & pending bits; pending ones latch only while enabled */
#define RK_I2C_INT_MBTF   BIT(2) /* command sent */
#define RK_I2C_INT_MBRF   BIT(3) /* command received */
#define RK_I2C_INT_START  BIT(4)
#define RK_I2C_INT_STOP   BIT(5)
#define RK_I2C_INT_NAKRCV BIT(6)
#define RK_I2C_INT_ALL    0x7f

#define RK_I2C_ADDR_VALID BIT(24)
#define RK_I2C_FIFO_SIZE  32
#define RK_I2C_TIMEOUT    K_MSEC(100)

struct i2c_rockchip_config {
	DEVICE_MMIO_ROM;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config)(void);
	uint32_t clock_frequency;
	uint32_t bitrate;
};

struct i2c_rockchip_data {
	DEVICE_MMIO_RAM;
	struct k_mutex lock;
	/* CON timing tuning bits, kept from their reset value */
	uint32_t con_tuning;
	struct k_sem done;
	/* pending events the ISR saw for the current command */
	uint32_t ipd;
};

static inline uint32_t rk_i2c_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void rk_i2c_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + reg);
}

/* Write CON with @bits, keeping the tuning bits & acting on NAKs */
static void rk_i2c_con(const struct device *dev, uint32_t bits)
{
	struct i2c_rockchip_data *data = dev->data;

	rk_i2c_write(dev, RK_I2C_CON, bits | RK_I2C_CON_ACTACK | data->con_tuning);
}

/* Clear & enable the @events interrupts, ahead of the write that starts a command */
static void rk_i2c_arm(const struct device *dev, uint32_t events)
{
	struct i2c_rockchip_data *data = dev->data;

	data->ipd = 0;
	k_sem_reset(&data->done);
	rk_i2c_write(dev, RK_I2C_IPD, RK_I2C_INT_ALL);
	rk_i2c_write(dev, RK_I2C_IEN, events);
}

/* Wait for one of the armed @events; a NAK fails the command */
static int rk_i2c_wait(const struct device *dev, uint32_t events)
{
	struct i2c_rockchip_data *data = dev->data;

	if (k_sem_take(&data->done, RK_I2C_TIMEOUT) != 0) {
		rk_i2c_write(dev, RK_I2C_IEN, 0);
		return -ETIMEDOUT;
	}

	return (data->ipd & RK_I2C_INT_NAKRCV) ? -EIO : 0;
}

static void i2c_rockchip_isr(const struct device *dev)
{
	struct i2c_rockchip_data *data = dev->data;
	uint32_t ipd = rk_i2c_read(dev, RK_I2C_IPD) & rk_i2c_read(dev, RK_I2C_IEN);

	if (ipd == 0) {
		return;
	}

	/* One event ends the command: mask the rest until the next one is armed */
	rk_i2c_write(dev, RK_I2C_IEN, 0);
	rk_i2c_write(dev, RK_I2C_IPD, ipd);
	data->ipd = ipd;
	k_sem_give(&data->done);
}

static int rk_i2c_start(const struct device *dev)
{
	rk_i2c_arm(dev, RK_I2C_INT_START);
	rk_i2c_con(dev, RK_I2C_CON_EN | RK_I2C_CON_START);

	return rk_i2c_wait(dev, RK_I2C_INT_START);
}

static void rk_i2c_stop(const struct device *dev)
{
	rk_i2c_arm(dev, RK_I2C_INT_STOP);
	rk_i2c_con(dev, RK_I2C_CON_EN | RK_I2C_CON_STOP);

	if (rk_i2c_wait(dev, RK_I2C_INT_STOP) != 0) {
		LOG_WRN("no stop condition");
	}

	rk_i2c_write(dev, RK_I2C_IEN, 0);
	rk_i2c_con(dev, 0);
}

/* Send @len bytes of @buf, preceded by the address byte if @addr_byte >= 0 */
static int rk_i2c_send(const struct device *dev, int addr_byte, const uint8_t *buf, uint32_t len)
{
	do {
		uint32_t words[RK_I2C_FIFO_SIZE / 4] = {0};
		uint32_t n = 0;
		int ret;

		if (addr_byte >= 0) {
			words[0] = addr_byte;
			n = 1;
			addr_byte = -1;
		}

		for (; n < RK_I2C_FIFO_SIZE && len > 0; n++, len--) {
			words[n / 4] |= (uint32_t)*buf++ << ((n % 4) * 8);
		}

		for (uint32_t i = 0; i < DIV_ROUND_UP(n, 4); i++) {
			rk_i2c_write(dev, RK_I2C_TXDATA + i * 4, words[i]);
		}

		/* Writing MTXCNT starts the command */
		rk_i2c_con(dev, RK_I2C_CON_EN | RK_I2C_CON_MOD_TX);
		rk_i2c_arm(dev, RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV);
		rk_i2c_write(dev, RK_I2C_MTXCNT, n);
		ret = rk_i2c_wait(dev, RK_I2C_INT_MBTF | RK_I2C_INT_NAKRCV);
		if (ret != 0) {
			return ret;
		}
	} while (len > 0);

	return 0;
}

/*
 * Receive @len bytes into @buf. If @addr_byte >= 0, first send it, then the
 * @raddr_len bytes of @raddr & a repeated start. NAK the last byte if
 * @nak_last, as before a stop.
 */
static int rk_i2c_receive(const struct device *dev, int addr_byte, const uint8_t *raddr,
			  uint8_t raddr_len, uint8_t *buf, uint32_t len, bool nak_last)
{
	uint32_t mode = RK_I2C_CON_MOD_RX;

	if (len == 0) {
		return -ENOTSUP;
	}

	if (addr_byte >= 0) {
		uint32_t mrxraddr = 0;

		for (uint8_t i = 0; i < raddr_len; i++) {
			mrxraddr |= ((uint32_t)raddr[i] << (i * 8)) | (RK_I2C_ADDR_VALID << i);
		}

		rk_i2c_write(dev, RK_I2C_MRXADDR, RK_I2C_ADDR_VALID | addr_byte);
		rk_i2c_write(dev, RK_I2C_MRXRADDR, mrxraddr);
		mode = RK_I2C_CON_MOD_TRX;
	}

	while (len > 0) {
		uint32_t n = MIN(len, RK_I2C_FIFO_SIZE);
		uint32_t con = RK_I2C_CON_EN | mode;
		uint32_t word = 0;
		int ret;

		if (n == len && nak_last) {
			con |= RK_I2C_CON_LASTACK;
		}

		/* Writing MRXCNT starts the command, so set the mode first */
		rk_i2c_con(dev, con);
		rk_i2c_arm(dev, RK_I2C_INT_MBRF | RK_I2C_INT_NAKRCV);
		rk_i2c_write(dev, RK_I2C_MRXCNT, n);
		ret = rk_i2c_wait(dev, RK_I2C_INT_MBRF | RK_I2C_INT_NAKRCV);
		if (ret != 0) {
			return ret;
		}

		for (uint32_t i = 0; i < n; i++) {
			if (i % 4 == 0) {
				word = rk_i2c_read(dev, RK_I2C_RXDATA + i);
			}
			*buf++ = word >> ((i % 4) * 8);
		}

		len -= n;
		mode = RK_I2C_CON_MOD_RX;
	}

	return 0;
}

static int i2c_rockchip_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
				 uint16_t addr)
{
	struct i2c_rockchip_data *data = dev->data;
	bool started = false;
	int ret = 0;

	if (num_msgs == 0) {
		return 0;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	for (uint8_t i = 0; i < num_msgs; i++) {
		struct i2c_msg *msg = &msgs[i];
		bool read = (msg->flags & I2C_MSG_RW_MASK) == I2C_MSG_READ;
		struct i2c_msg *next = (i < num_msgs - 1) ? &msgs[i + 1] : NULL;
		int addr_byte = -1;

		if (msg->flags & I2C_MSG_ADDR_10_BITS) {
			ret = -ENOTSUP;
			break;
		}

		if (started && (msg->flags & I2C_MSG_RESTART)) {
			rk_i2c_stop(dev);
			started = false;
		}

		if (!started) {
			ret = rk_i2c_start(dev);
			if (ret != 0) {
				break;
			}
			started = true;
			addr_byte = (addr << 1) | (read ? 1 : 0);
		}

		if (!read && addr_byte >= 0 && msg->len <= 3 && !(msg->flags & I2C_MSG_STOP) &&
		    next != NULL && (next->flags & I2C_MSG_RW_MASK) == I2C_MSG_READ &&
		    (next->flags & I2C_MSG_RESTART)) {
			/* Register mode: this write, a repeated start & the read */
			msg = next;
			next = (++i < num_msgs - 1) ? &msgs[i + 1] : NULL;
			ret = rk_i2c_receive(dev, addr << 1, msgs[i - 1].buf, msgs[i - 1].len,
					     msg->buf, msg->len, true);
		} else if (read) {
			/* A repeated start after this read is a stop & a start: NAK the end */
			ret = rk_i2c_receive(dev, addr_byte, NULL, 0, msg->buf, msg->len,
					     next == NULL || (msg->flags & I2C_MSG_STOP) ||
						     (next->flags & I2C_MSG_RESTART));
		} else {
			ret = rk_i2c_send(dev, addr_byte, msg->buf, msg->len);
		}
		if (ret != 0) {
			break;
		}

		if (next == NULL || (msg->flags & I2C_MSG_STOP)) {
			rk_i2c_stop(dev);
			started = false;
		}
	}

	if (ret != 0 && started) {
		rk_i2c_stop(dev);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2c_rockchip_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_rockchip_config *config = dev->config;
	struct i2c_rockchip_data *data = dev->data;
	uint32_t scl;
	uint32_t div;
	uint32_t divh;
	uint32_t divl;

	if (!(dev_config & I2C_MODE_CONTROLLER) || (dev_config & I2C_ADDR_10_BITS)) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		scl = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		scl = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		scl = MHZ(1);
		break;
	default:
		return -ENOTSUP;
	}

	/* SCL = clock / (8 * (divl + 1 + divh + 1)), rounded down to at most @scl */
	div = DIV_ROUND_UP(config->clock_frequency, scl * 8);
	if (div < 2 || div > 2 * 0x10000) {
		return -EINVAL;
	}
	divh = div / 2;
	divl = div - divh;

	k_mutex_lock(&data->lock, K_FOREVER);
	rk_i2c_write(dev, RK_I2C_CLKDIV, ((divh - 1) << 16) | (divl - 1));
	k_mutex_unlock(&data->lock);

	return 0;
}

static int i2c_rockchip_init(const struct device *dev)
{
	const struct i2c_rockchip_config *config = dev->config;
	struct i2c_rockchip_data *data = dev->data;
	int ret;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	k_mutex_init(&data->lock);
	k_sem_init(&data->done, 0, 1);

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	data->con_tuning = rk_i2c_read(dev, RK_I2C_CON) & RK_I2C_CON_TUNING;
	rk_i2c_write(dev, RK_I2C_IEN, 0);
	rk_i2c_con(dev, 0);
	config->irq_config();

	return i2c_rockchip_configure(dev,
				      I2C_MODE_CONTROLLER | i2c_map_dt_bitrate(config->bitrate));
}

static DEVICE_API(i2c, i2c_rockchip_api) = {
	.configure = i2c_rockchip_configure,
	.transfer = i2c_rockchip_transfer,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

#define I2C_ROCKCHIP_INIT(n)                                                                       \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static void i2c_rockchip_irq_config_##n(void)                                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), i2c_rockchip_isr,           \
			    DEVICE_DT_INST_GET(n), DT_INST_IRQ(n, flags));                         \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct i2c_rockchip_config i2c_rockchip_config_##n = {                        \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.irq_config = i2c_rockchip_irq_config_##n,                                         \
		.clock_frequency = DT_INST_PROP_BY_PHANDLE(n, clocks, clock_frequency),            \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                       \
	};                                                                                         \
                                                                                                   \
	static struct i2c_rockchip_data i2c_rockchip_data_##n;                                     \
                                                                                                   \
	I2C_DEVICE_DT_INST_DEFINE(n, i2c_rockchip_init, NULL, &i2c_rockchip_data_##n,              \
				  &i2c_rockchip_config_##n, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, \
				  &i2c_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_ROCKCHIP_INIT)

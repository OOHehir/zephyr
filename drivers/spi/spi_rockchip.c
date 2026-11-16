/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip SPI controller (the RK3066 generation, as on the RK3568), as a
 * controller only. Transfers are polled through the 64-entry FIFOs, 8 or 16
 * bits a frame, always transmitting & receiving: NOP buffers send zeros &
 * received frames without a buffer are dropped.
 *
 * The driver switches the controller's clock to xin24m, so SCLK runs at
 * 24 MHz divided by an even number: at most 12 MHz.
 */

#define DT_DRV_COMPAT rockchip_rk3568_spi

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_rockchip, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"
#include "spi_rtio.h"

#define RK_SPI_CTRLR0  0x0000
#define RK_SPI_SSIENR  0x0008
#define RK_SPI_SER     0x000c
#define RK_SPI_BAUDR   0x0010
#define RK_SPI_TXFLR   0x001c
#define RK_SPI_RXFLR   0x0020
#define RK_SPI_SR      0x0024
#define RK_SPI_IMR     0x002c
#define RK_SPI_ICR     0x0038
#define RK_SPI_DMACR   0x003c
#define RK_SPI_VERSION 0x0048
#define RK_SPI_TXDR    0x0400
#define RK_SPI_RXDR    0x0800

#define RK_SPI_CTRLR0_DFS_8   (1 << 0)
#define RK_SPI_CTRLR0_DFS_16  (2 << 0)
#define RK_SPI_CTRLR0_SCPH    BIT(6)
#define RK_SPI_CTRLR0_SCPOL   BIT(7)
/* ss_n to sclk_out: one sclk_out cycle */
#define RK_SPI_CTRLR0_SSD_ONE BIT(10)
#define RK_SPI_CTRLR0_EM_BIG  BIT(11)
#define RK_SPI_CTRLR0_FBM_LSB BIT(12)
/* APB 8-bit transfers, for frames of 8 bits or less */
#define RK_SPI_CTRLR0_BHT_8   BIT(13)
#define RK_SPI_CTRLR0_SOI(cs) BIT(23 + (cs))
/* Internal loopback: rxd is connected to txd */
#define RK_SPI_CTRLR0_LBK     BIT(25)

#define RK_SPI_SR_BUSY BIT(0)

#define RK_SPI_ICR_ALL BIT(0)

#define RK_SPI_FIFO_LEN      64
#define RK_SPI_NATIVE_CS_NUM 2
#define RK_SPI_BAUDR_MIN     2
#define RK_SPI_BAUDR_MAX     65534
/* Allowance on top of the bits on the wire before a transfer counts as stuck */
#define RK_SPI_TIMEOUT_MS    100

/* CRU_CLKSEL_CON72: clk_spiN mux at bits 2N+1:2N, 1 = xin24m */
#define RK_CRU_CLKSEL_CON72 0x0220
#define RK_SPI_CLK_SEL_24M  1
#define RK_SPI_CLK_HZ       MHZ(24)

static const uintptr_t rk3568_spi_bases[] = {0xfe610000, 0xfe620000, 0xfe630000, 0xfe640000};

struct spi_rockchip_config {
	DEVICE_MMIO_ROM;
	const struct pinctrl_dev_config *pcfg;
	struct reset_dt_spec reset_apb;
	struct reset_dt_spec reset;
	uintptr_t phys;
	uintptr_t cru_phys;
};

struct spi_rockchip_data {
	DEVICE_MMIO_RAM;
	struct spi_context ctx;
	uint8_t dfs;
	uint32_t ctrlr0;
	uint32_t sclk_hz;
};

static inline uint32_t spi_rockchip_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(DEVICE_MMIO_GET(dev) + reg);
}

static inline void spi_rockchip_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	sys_write32(val, DEVICE_MMIO_GET(dev) + reg);
}

static int spi_rockchip_configure(const struct device *dev, const struct spi_config *config)
{
	struct spi_rockchip_data *data = dev->data;
	uint32_t ctrlr0 = RK_SPI_CTRLR0_SSD_ONE | RK_SPI_CTRLR0_EM_BIG | RK_SPI_CTRLR0_BHT_8;
	uint32_t baudr;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if (SPI_OP_MODE_GET(config->operation) != SPI_OP_MODE_CONTROLLER) {
		LOG_ERR("only controller mode is supported");
		return -ENOTSUP;
	}

	if (config->operation & SPI_HALF_DUPLEX) {
		LOG_ERR("half duplex is not supported");
		return -ENOTSUP;
	}

	if (IS_ENABLED(CONFIG_SPI_EXTENDED_MODES) &&
	    (config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		LOG_ERR("only single line mode is supported");
		return -ENOTSUP;
	}

	switch (SPI_WORD_SIZE_GET(config->operation)) {
	case 8:
		ctrlr0 |= RK_SPI_CTRLR0_DFS_8;
		data->dfs = 1;
		break;
	case 16:
		ctrlr0 |= RK_SPI_CTRLR0_DFS_16;
		data->dfs = 2;
		break;
	default:
		LOG_ERR("word size %u is not supported", SPI_WORD_SIZE_GET(config->operation));
		return -ENOTSUP;
	}

	if (!spi_cs_is_gpio(config) && config->peripheral >= RK_SPI_NATIVE_CS_NUM) {
		LOG_ERR("chip select %u is not supported", config->peripheral);
		return -EINVAL;
	}

	if (config->frequency == 0) {
		return -EINVAL;
	}

	/* The divider is even; round it up so SCLK does not exceed the request */
	baudr = 2 * DIV_ROUND_UP(RK_SPI_CLK_HZ, 2 * config->frequency);
	baudr = CLAMP(baudr, RK_SPI_BAUDR_MIN, RK_SPI_BAUDR_MAX);
	data->sclk_hz = RK_SPI_CLK_HZ / baudr;

	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPHA) {
		ctrlr0 |= RK_SPI_CTRLR0_SCPH;
	}
	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPOL) {
		ctrlr0 |= RK_SPI_CTRLR0_SCPOL;
	}
	if (SPI_MODE_GET(config->operation) & SPI_MODE_LOOP) {
		ctrlr0 |= RK_SPI_CTRLR0_LBK;
	}
	if (config->operation & SPI_TRANSFER_LSB) {
		ctrlr0 |= RK_SPI_CTRLR0_FBM_LSB;
	}
	if (!spi_cs_is_gpio(config) && (config->operation & SPI_CS_ACTIVE_HIGH)) {
		ctrlr0 |= RK_SPI_CTRLR0_SOI(config->peripheral);
	}

	/* CTRLR0 & BAUDR only take writes while the controller is disabled */
	spi_rockchip_write(dev, RK_SPI_SSIENR, 0);
	spi_rockchip_write(dev, RK_SPI_SER, 0);
	spi_rockchip_write(dev, RK_SPI_CTRLR0, ctrlr0);
	spi_rockchip_write(dev, RK_SPI_BAUDR, baudr);

	data->ctrlr0 = ctrlr0;
	data->ctx.config = config;

	LOG_DBG("ctrlr0 0x%08x, %u Hz", ctrlr0, RK_SPI_CLK_HZ / baudr);

	return 0;
}

static void spi_rockchip_cs(const struct device *dev, bool on)
{
	struct spi_rockchip_data *data = dev->data;
	const struct spi_config *config = data->ctx.config;

	if (spi_cs_is_gpio(config)) {
		spi_context_cs_control(&data->ctx, on);
	} else {
		spi_rockchip_write(dev, RK_SPI_SER, on ? BIT(config->peripheral) : 0);
	}
}

static int spi_rockchip_xfer(const struct device *dev)
{
	struct spi_rockchip_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	uint8_t dfs = data->dfs;
	size_t frames = MAX(spi_context_total_tx_len(ctx), spi_context_total_rx_len(ctx)) / dfs;
	size_t sent = 0;
	size_t received = 0;
	uint64_t wire_ms = (uint64_t)frames * dfs * 8 * MSEC_PER_SEC / data->sclk_hz;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(wire_ms + RK_SPI_TIMEOUT_MS));

	while (received < frames) {
		/* Keep at most a FIFO's worth in flight, so the RX FIFO cannot overflow */
		while (sent < frames && sent - received < RK_SPI_FIFO_LEN) {
			uint32_t word = 0;

			if (spi_context_tx_buf_on(ctx)) {
				word = dfs == 1 ? *ctx->tx_buf : sys_get_le16(ctx->tx_buf);
			}
			spi_rockchip_write(dev, RK_SPI_TXDR, word);
			if (spi_context_tx_on(ctx)) {
				spi_context_update_tx(ctx, dfs, 1);
			}
			sent++;
		}

		for (uint32_t n = spi_rockchip_read(dev, RK_SPI_RXFLR); n > 0; n--) {
			uint32_t word = spi_rockchip_read(dev, RK_SPI_RXDR);

			if (spi_context_rx_buf_on(ctx)) {
				if (dfs == 1) {
					*ctx->rx_buf = word;
				} else {
					sys_put_le16(word, ctx->rx_buf);
				}
			}
			if (spi_context_rx_on(ctx)) {
				spi_context_update_rx(ctx, dfs, 1);
			}
			received++;
		}

		if (sys_timepoint_expired(end)) {
			LOG_ERR("transfer timed out, %zu of %zu frames", received, frames);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int spi_rockchip_transceive(const struct device *dev, const struct spi_config *config,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs, bool asynchronous,
				   spi_callback_t cb, void *userdata)
{
	struct spi_rockchip_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	int ret;

	spi_context_lock(ctx, asynchronous, cb, userdata, config);

	ret = spi_rockchip_configure(dev, config);
	if (ret < 0) {
		goto out;
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, data->dfs);

	spi_rockchip_write(dev, RK_SPI_SSIENR, 1);
	spi_rockchip_cs(dev, true);

	ret = spi_rockchip_xfer(dev);

	if (ret == 0 && !WAIT_FOR(!(spi_rockchip_read(dev, RK_SPI_SR) & RK_SPI_SR_BUSY),
				  RK_SPI_TIMEOUT_MS * USEC_PER_MSEC, k_busy_wait(1))) {
		ret = -ETIMEDOUT;
	}

	if (ret < 0 || !(config->operation & SPI_HOLD_ON_CS)) {
		spi_rockchip_cs(dev, false);
		/* Disabling the controller also empties both FIFOs */
		spi_rockchip_write(dev, RK_SPI_SSIENR, 0);
	}

	spi_context_complete(ctx, dev, ret);
	ret = spi_context_wait_for_completion(ctx);
out:
	spi_context_release(ctx, ret);

	return ret;
}

static int spi_rockchip_transceive_sync(const struct device *dev, const struct spi_config *config,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs)
{
	return spi_rockchip_transceive(dev, config, tx_bufs, rx_bufs, false, NULL, NULL);
}

#ifdef CONFIG_SPI_ASYNC
/* Completes before returning: the callback runs in the caller's context */
static int spi_rockchip_transceive_async(const struct device *dev, const struct spi_config *config,
					 const struct spi_buf_set *tx_bufs,
					 const struct spi_buf_set *rx_bufs, spi_callback_t cb,
					 void *userdata)
{
	return spi_rockchip_transceive(dev, config, tx_bufs, rx_bufs, true, cb, userdata);
}
#endif

static int spi_rockchip_release(const struct device *dev, const struct spi_config *config)
{
	struct spi_rockchip_data *data = dev->data;

	if (data->ctx.config != NULL) {
		spi_rockchip_cs(dev, false);
	}
	spi_rockchip_write(dev, RK_SPI_SSIENR, 0);
	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_rockchip_api) = {
	.transceive = spi_rockchip_transceive_sync,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = spi_rockchip_transceive_async,
#endif
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = spi_rockchip_release,
};

static int spi_rockchip_init(const struct device *dev)
{
	const struct spi_rockchip_config *cfg = dev->config;
	struct spi_rockchip_data *data = dev->data;
	mm_reg_t cru;
	int idx = -1;
	int ret;

	for (int i = 0; i < ARRAY_SIZE(rk3568_spi_bases); i++) {
		if (cfg->phys == rk3568_spi_bases[i]) {
			idx = i;
		}
	}
	if (idx < 0) {
		LOG_ERR("unknown SPI controller at 0x%lx", (unsigned long)cfg->phys);
		return -EINVAL;
	}

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	device_map(&cru, cfg->cru_phys, 0x1000, K_MEM_CACHE_NONE);

	sys_write32((GENMASK(1, 0) << (16 + 2 * idx)) | (RK_SPI_CLK_SEL_24M << (2 * idx)),
		    cru + RK_CRU_CLKSEL_CON72);

	ret = reset_line_toggle_dt(&cfg->reset_apb);
	if (ret == 0) {
		ret = reset_line_toggle_dt(&cfg->reset);
	}
	if (ret < 0) {
		return ret;
	}

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	spi_rockchip_write(dev, RK_SPI_SSIENR, 0);
	spi_rockchip_write(dev, RK_SPI_IMR, 0);
	spi_rockchip_write(dev, RK_SPI_ICR, RK_SPI_ICR_ALL);
	spi_rockchip_write(dev, RK_SPI_DMACR, 0);

	LOG_DBG("version 0x%08x", spi_rockchip_read(dev, RK_SPI_VERSION));

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_ROCKCHIP_INIT(n)                                                                       \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static const struct spi_rockchip_config spi_rockchip_config_##n = {                        \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                                              \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.reset_apb = RESET_DT_SPEC_INST_GET_BY_IDX(n, 0),                                  \
		.reset = RESET_DT_SPEC_INST_GET_BY_IDX(n, 1),                                      \
		.phys = DT_INST_REG_ADDR(n),                                                       \
		.cru_phys = DT_REG_ADDR(DT_INST_RESET_CTLR(n)),                                    \
	};                                                                                         \
	static struct spi_rockchip_data spi_rockchip_data_##n = {                                  \
		SPI_CONTEXT_INIT_LOCK(spi_rockchip_data_##n, ctx),                                 \
		SPI_CONTEXT_INIT_SYNC(spi_rockchip_data_##n, ctx),                                 \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
	SPI_DEVICE_DT_INST_DEFINE(n, spi_rockchip_init, NULL, &spi_rockchip_data_##n,              \
				  &spi_rockchip_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY, \
				  &spi_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_ROCKCHIP_INIT)

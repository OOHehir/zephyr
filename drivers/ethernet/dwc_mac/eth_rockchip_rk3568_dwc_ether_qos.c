/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Rockchip RK3568 glue for the Synopsys DesignWare Ethernet QoS MAC (GMAC0 &
 * GMAC1), RGMII only.
 *
 * Per instance: apply the pins, release the AXI reset, select RGMII & set the clock delay
 * lines in the GRF, & on each link up pick the TX clock in the CRU: 125 MHz
 * for 1000M (the reset default), 25 MHz for 100M, 2.5 MHz for 10M.
 */

#define DT_DRV_COMPAT rockchip_rk3568_gmac

#define LOG_MODULE_NAME dwmac_rockchip
#define LOG_LEVEL       CONFIG_ETHERNET_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <sys/types.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>

#include "eth_dwmac_priv.h"

DWMAC_ASSERT_BUFFER_ALIGNMENT(0);

/* Rockchip "hiword" registers: bits 31:16 enable writes to bits 15:0 */
#define RK_HIWORD(mask, val) (((mask) << 16) | ((val) & (mask)))

/* GRF GMACn_CON0: clock delay lines */
#define RK_GMAC_CON0_TX_DL GENMASK(6, 0)
#define RK_GMAC_CON0_RX_DL GENMASK(14, 8)

/* GRF GMACn_CON1 */
#define RK_GMAC_CON1_TXCLK_DLY_EN BIT(0)
#define RK_GMAC_CON1_RXCLK_DLY_EN BIT(1)
#define RK_GMAC_CON1_INTF_SEL     GENMASK(6, 4)
#define RK_GMAC_CON1_INTF_RGMII   BIT(4)

/* CRU CLKSEL_CON31 (GMAC0) & CLKSEL_CON33 (GMAC1): RGMII TX clock select */
#define RK_CLKSEL_RGMII_SPEED      GENMASK(5, 4)
#define RK_CLKSEL_RGMII_SPEED_1000 0
#define RK_CLKSEL_RGMII_SPEED_10   2
#define RK_CLKSEL_RGMII_SPEED_100  3

/* After releasing the PHY's reset, before its RX clock is relied on */
#define RK_PHY_RESET_SETTLE_MS 100

struct dwmac_rockchip_config {
	/* Must be first: the core reads dev->config as a struct dwmac_config */
	struct dwmac_config common;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	/* The PHY's reset line, if its node has reset-gpios */
	struct gpio_dt_spec phy_reset;
	struct net_eth_mac_config mac_cfg;
	uintptr_t grf_phys;
	uintptr_t cru_phys;
	uint16_t grf_con0;
	uint16_t grf_con1;
	uint16_t cru_clksel;
	uint8_t tx_delay;
	uint8_t rx_delay;
	struct dwmac_dma_desc *descs;
	void (*irq_config)(void);
};

/* The GRF & CRU, shared by both GMACs (& other drivers), mapped once */
static mm_reg_t rk_grf;
static mm_reg_t rk_cru;

static void rk_map(const struct dwmac_rockchip_config *cfg)
{
	if (rk_grf == 0) {
		device_map(&rk_grf, cfg->grf_phys, 0x1000, K_MEM_CACHE_NONE);
	}
	if (rk_cru == 0) {
		device_map(&rk_cru, cfg->cru_phys, 0x1000, K_MEM_CACHE_NONE);
	}
}

int dwmac_bus_init(const struct device *dev)
{
	const struct dwmac_rockchip_config *cfg = dev->config;
	uint32_t con1 = RK_GMAC_CON1_INTF_RGMII;
	int ret;

	rk_map(cfg);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("cannot apply the pin state (%d)", ret);
		return ret;
	}

	/*
	 * The MAC's software reset, which the core issues next, completes only
	 * with the PHY's RX clock running: take the PHY out of reset now. Its
	 * driver pulses the line again later, which the running MAC survives.
	 */
	if (cfg->phy_reset.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->phy_reset)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->phy_reset, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
		k_msleep(RK_PHY_RESET_SETTLE_MS);
	}

	ret = reset_line_deassert_dt(&cfg->reset);
	if (ret < 0) {
		LOG_ERR("cannot release the MAC reset (%d)", ret);
		return ret;
	}
	k_busy_wait(10);

	sys_write32(RK_HIWORD(RK_GMAC_CON0_TX_DL | RK_GMAC_CON0_RX_DL,
			      FIELD_PREP(RK_GMAC_CON0_TX_DL, cfg->tx_delay) |
				      FIELD_PREP(RK_GMAC_CON0_RX_DL, cfg->rx_delay)),
		    rk_grf + cfg->grf_con0);

	if (cfg->tx_delay != 0) {
		con1 |= RK_GMAC_CON1_TXCLK_DLY_EN;
	}
	if (cfg->rx_delay != 0) {
		con1 |= RK_GMAC_CON1_RXCLK_DLY_EN;
	}
	sys_write32(RK_HIWORD(RK_GMAC_CON1_INTF_SEL | RK_GMAC_CON1_TXCLK_DLY_EN |
				      RK_GMAC_CON1_RXCLK_DLY_EN,
			      con1),
		    rk_grf + cfg->grf_con1);

	/* Start at 1000M, which is also the reset default */
	dwmac_platform_link_speed_changed(dev, LINK_FULL_1000BASE);

	return 0;
}

void dwmac_platform_link_speed_changed(const struct device *dev, enum phy_link_speed speed)
{
	const struct dwmac_rockchip_config *cfg = dev->config;
	uint32_t sel;

	if (PHY_LINK_IS_SPEED_1000M(speed)) {
		sel = RK_CLKSEL_RGMII_SPEED_1000;
	} else if (PHY_LINK_IS_SPEED_100M(speed)) {
		sel = RK_CLKSEL_RGMII_SPEED_100;
	} else {
		sel = RK_CLKSEL_RGMII_SPEED_10;
	}

	sys_write32(RK_HIWORD(RK_CLKSEL_RGMII_SPEED, FIELD_PREP(RK_CLKSEL_RGMII_SPEED, sel)),
		    rk_cru + cfg->cru_clksel);
}

int dwmac_platform_init(const struct device *dev)
{
	const struct dwmac_rockchip_config *cfg = dev->config;
	struct dwmac_priv *p = dev->data;
	size_t size = (NB_TX_DESCS + NB_RX_DESCS) * sizeof(struct dwmac_dma_desc);
	uint8_t *desc_uncached_addr;
	uintptr_t desc_phys_addr;
	int ret;

	/* make sure no valid cache lines map to the descriptor area */
	sys_cache_data_invd_range(cfg->descs, size);

	desc_phys_addr = k_mem_phys_addr(cfg->descs);

	/* remap descriptor rings uncached */
	k_mem_map_phys_bare(&desc_uncached_addr, desc_phys_addr, size,
			    K_MEM_PERM_RW | K_MEM_CACHE_NONE);

	p->tx_descs = (void *)desc_uncached_addr;
	desc_uncached_addr += NB_TX_DESCS * sizeof(struct dwmac_dma_desc);
	p->rx_descs = (void *)desc_uncached_addr;

	p->tx_descs_phys = UINT_TO_POINTER(desc_phys_addr);
	desc_phys_addr += NB_TX_DESCS * sizeof(struct dwmac_dma_desc);
	p->rx_descs_phys = UINT_TO_POINTER(desc_phys_addr);

	/* 1000M full duplex until the PHY reports the link */
	DWMAC_REG_WRITE(MAC_CONF, MAC_CONF_DM);
	DWMAC_REG_WRITE(DMA_SYSBUS_MODE,
			DMA_SYSBUS_MODE_AAL | DMA_SYSBUS_MODE_EAME | DMA_SYSBUS_MODE_FB);

	/* set up IRQs (still masked for now) */
	cfg->irq_config();

	ret = net_eth_mac_load(&cfg->mac_cfg, p->mac_addr);
	if (ret == -ENODATA) {
		LOG_WRN("no MAC address configured");
		return 0;
	}
	if (ret < 0) {
		LOG_ERR("cannot load the MAC address (%d)", ret);
		return ret;
	}

	return 0;
}

/* GRF & CRU registers per GMAC, keyed by the MAC's base address */
#define RK_GMAC0_BASE 0xfe2a0000
#define RK_GMAC1_BASE 0xfe010000

#define RK_GMAC_IS_0(n) (DT_INST_REG_ADDR(n) == RK_GMAC0_BASE)

#define DWMAC_ROCKCHIP_INIT(n)                                                                     \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) == RK_GMAC0_BASE || DT_INST_REG_ADDR(n) == RK_GMAC1_BASE, \
		     "unknown RK3568 GMAC");                                                       \
	BUILD_ASSERT(DT_INST_ENUM_HAS_VALUE(n, phy_connection_type, rgmii),                        \
		     "only RGMII with the delays in the MAC is supported");                        \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct dwmac_dma_desc __aligned(CONFIG_DCACHE_LINE_SIZE)                            \
	dwmac_rockchip_descs_##n[NB_TX_DESCS + NB_RX_DESCS];                                       \
                                                                                                   \
	static void dwmac_rockchip_irq_config_##n(void)                                            \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), dwmac_isr,                  \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static const struct dwmac_rockchip_config dwmac_rockchip_config_##n = {                    \
		.common =                                                                          \
			{                                                                          \
				DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)),                              \
				.phy_dev = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(n, phy_handle)),  \
			},                                                                         \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.phy_reset =                                                                       \
			GPIO_DT_SPEC_GET_OR(DT_INST_PHANDLE(n, phy_handle), reset_gpios, {0}),     \
		.mac_cfg = NET_ETH_MAC_DT_INST_CONFIG_INIT(n),                                     \
		.grf_phys = DT_REG_ADDR(DT_INST_PHANDLE(n, rockchip_grf)),                         \
		.cru_phys = DT_REG_ADDR(DT_INST_RESET_CTLR(n)),                                    \
		.grf_con0 = RK_GMAC_IS_0(n) ? 0x0380 : 0x0388,                                     \
		.grf_con1 = RK_GMAC_IS_0(n) ? 0x0384 : 0x038c,                                     \
		.cru_clksel = RK_GMAC_IS_0(n) ? 0x017c : 0x0184,                                   \
		.tx_delay = DT_INST_PROP(n, tx_delay),                                             \
		.rx_delay = DT_INST_PROP(n, rx_delay),                                             \
		.descs = dwmac_rockchip_descs_##n,                                                 \
		.irq_config = dwmac_rockchip_irq_config_##n,                                       \
	};                                                                                         \
                                                                                                   \
	static struct dwmac_priv dwmac_rockchip_data_##n;                                          \
                                                                                                   \
	ETH_NET_DEVICE_DT_INST_DEFINE(n, dwmac_probe, NULL, &dwmac_rockchip_data_##n,              \
				      &dwmac_rockchip_config_##n, CONFIG_ETH_INIT_PRIORITY,        \
				      &dwmac_api, NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(DWMAC_ROCKCHIP_INIT)

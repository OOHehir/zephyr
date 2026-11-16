/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ROCKCHIP_COMMON_PINCTRL_SOC_H_
#define ZEPHYR_SOC_ROCKCHIP_COMMON_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>
#include <zephyr/dt-bindings/pinctrl/rockchip-pinctrl.h>

/* pull & drive value meaning "leave as the boot loader set it" */
#define ROCKCHIP_PINCTRL_KEEP 0xff

#define ROCKCHIP_PULL_NONE 0
#define ROCKCHIP_PULL_UP   1
#define ROCKCHIP_PULL_DOWN 2

typedef struct {
	/** RK_PINMUX() or RK_PINMUX_ROUTE() cell */
	uint32_t pinmux;
	/** ROCKCHIP_PULL_* or ROCKCHIP_PINCTRL_KEEP */
	uint8_t pull;
	/** drive level 0-5 or ROCKCHIP_PINCTRL_KEEP */
	uint8_t drive;
} pinctrl_soc_pin_t;

#define Z_PINCTRL_ROCKCHIP_PULL(node_id)                                                           \
	COND_CODE_1(DT_PROP(node_id, bias_pull_up), (ROCKCHIP_PULL_UP),                            \
		(COND_CODE_1(DT_PROP(node_id, bias_pull_down), (ROCKCHIP_PULL_DOWN),               \
			(COND_CODE_1(DT_PROP(node_id, bias_disable), (ROCKCHIP_PULL_NONE),         \
				(ROCKCHIP_PINCTRL_KEEP))))))

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	{                                                                                          \
		.pinmux = DT_PROP_BY_IDX(node_id, prop, idx),                                      \
		.pull = Z_PINCTRL_ROCKCHIP_PULL(node_id),                                          \
		.drive = DT_PROP_OR(node_id, drive_strength, ROCKCHIP_PINCTRL_KEEP),               \
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_CHILD_VARGS(DT_PHANDLE(node_id, prop), DT_FOREACH_PROP_ELEM, pinmux,           \
				Z_PINCTRL_STATE_PIN_INIT)}

#endif /* ZEPHYR_SOC_ROCKCHIP_COMMON_PINCTRL_SOC_H_ */

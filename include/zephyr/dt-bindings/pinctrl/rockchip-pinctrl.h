/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ROCKCHIP_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ROCKCHIP_PINCTRL_H_

/* Pin within a bank: port A-D, line 0-7 (same names as Linux) */
#define RK_PA0 0
#define RK_PA1 1
#define RK_PA2 2
#define RK_PA3 3
#define RK_PA4 4
#define RK_PA5 5
#define RK_PA6 6
#define RK_PA7 7
#define RK_PB0 8
#define RK_PB1 9
#define RK_PB2 10
#define RK_PB3 11
#define RK_PB4 12
#define RK_PB5 13
#define RK_PB6 14
#define RK_PB7 15
#define RK_PC0 16
#define RK_PC1 17
#define RK_PC2 18
#define RK_PC3 19
#define RK_PC4 20
#define RK_PC5 21
#define RK_PC6 22
#define RK_PC7 23
#define RK_PD0 24
#define RK_PD1 25
#define RK_PD2 26
#define RK_PD3 27
#define RK_PD4 28
#define RK_PD5 29
#define RK_PD6 30
#define RK_PD7 31

#define RK_FUNC_GPIO 0

/*
 * Route registers, which pick the pin set (M0, M1 or M2) a peripheral uses:
 * GRF IOFUNC_SEL0-5 at 0x0300-0x0314, & PMU GRF 0x0110 for PWM0-PWM2.
 */
#define RK_ROUTE_NONE     0
#define RK_IOFUNC_SEL0    1
#define RK_IOFUNC_SEL1    2
#define RK_IOFUNC_SEL2    3
#define RK_IOFUNC_SEL3    4
#define RK_IOFUNC_SEL4    5
#define RK_IOFUNC_SEL5    6
#define RK_ROUTE_PMU_0110 7

/*
 * pinmux cell: bits 0-4 pin, 5-7 bank, 8-11 IOMUX function, 12-14 route
 * register, 16-19 route field shift, 20 route field width - 1, 21-22 route
 * field value
 */
#define RK_PINMUX(bank, pin, func) (((pin) & 0x1f) | (((bank) & 0x7) << 5) | (((func) & 0xf) << 8))

/* As RK_PINMUX(), also writing @val into the @width-bit field at @shift of @reg */
#define RK_PINMUX_ROUTE(bank, pin, func, reg, shift, width, val)                                   \
	(RK_PINMUX(bank, pin, func) | (((reg) & 0x7) << 12) | (((shift) & 0xf) << 16) |            \
	 ((((width) - 1) & 0x1) << 20) | (((val) & 0x3) << 21))

#define RK_PINMUX_PIN(pinmux)         ((pinmux) & 0x1f)
#define RK_PINMUX_BANK(pinmux)        (((pinmux) >> 5) & 0x7)
#define RK_PINMUX_FUNC(pinmux)        (((pinmux) >> 8) & 0xf)
#define RK_PINMUX_ROUTE_REG(pinmux)   (((pinmux) >> 12) & 0x7)
#define RK_PINMUX_ROUTE_SHIFT(pinmux) (((pinmux) >> 16) & 0xf)
#define RK_PINMUX_ROUTE_WIDTH(pinmux) ((((pinmux) >> 20) & 0x1) + 1)
#define RK_PINMUX_ROUTE_VAL(pinmux)   (((pinmux) >> 21) & 0x3)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ROCKCHIP_PINCTRL_H_ */

/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_RESET_ROCKCHIP_RK3568_RESET_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_RESET_ROCKCHIP_RK3568_RESET_H_

/* CRU soft reset ids, as Linux numbers them: bit (id % 16) of SOFTRST_CON(id / 16) */
#define SRST_A_GMAC0  215
#define SRST_A_GMAC1  236
#define SRST_P_SPI0   374
#define SRST_SPI0     375
#define SRST_P_SPI1   376
#define SRST_SPI1     377
#define SRST_P_SPI2   378
#define SRST_SPI2     379
#define SRST_P_SPI3   380
#define SRST_SPI3     381
#define SRST_P_TSADC  385
#define SRST_TSADC    386
#define SRST_TSADCPHY 471

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_RESET_ROCKCHIP_RK3568_RESET_H_ */

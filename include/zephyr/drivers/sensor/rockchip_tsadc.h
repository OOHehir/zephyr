/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Rockchip TSADC sensor channels
 *
 * SENSOR_CHAN_DIE_TEMP reads the sensor at the CPU; this channel reads the one
 * at the GPU.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_ROCKCHIP_TSADC_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_ROCKCHIP_TSADC_H_

#include <zephyr/drivers/sensor.h>

/** GPU temperature in degrees Celsius */
#define SENSOR_CHAN_ROCKCHIP_TSADC_GPU (SENSOR_CHAN_PRIV_START + 0)

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_ROCKCHIP_TSADC_H_ */

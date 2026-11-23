/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FIMC_IS_DRIVERS_H
#define FIMC_IS_DRIVERS_H

#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

extern struct platform_driver fimc_is_driver;
extern struct platform_driver fimc_is_sensor_driver;
extern struct platform_driver fimc_is_preproc_driver;
extern struct platform_driver fimc_is_device_preproc_driver;
extern struct platform_driver sensor_module_2l1_driver;
extern struct platform_driver sensor_module_2l2_driver;
extern struct platform_driver sensor_module_3h1_driver;
extern struct platform_driver sensor_module_3h1_c3_driver;
extern struct platform_driver sensor_module_3m3_driver;
extern struct platform_driver sensor_module_imx320_driver;
extern struct platform_driver sensor_module_imx320_c3_driver;
extern struct platform_driver sensor_module_imx333_driver;
extern struct platform_driver sensor_flash_gpio_platform_driver;
extern struct platform_driver sensor_flash_s2mpb02_platform_driver;

extern struct spi_driver fimc_is_spi0_driver;
extern struct spi_driver fimc_is_spi1_driver;

extern struct i2c_driver fimc_is_i2c0_driver;
extern struct i2c_driver sensor_5e6_driver;
extern struct i2c_driver cis_2l1_driver;
extern struct i2c_driver cis_2l2_driver;
extern struct i2c_driver cis_3h1_driver;
extern struct i2c_driver cis_3m3_driver;
extern struct i2c_driver cis_imx320_driver;
extern struct i2c_driver cis_imx333_driver;
extern struct i2c_driver actuator_ak7371_driver;
extern struct i2c_driver actuator_ak7372_driver;
extern struct i2c_driver sensor_eeprom_driver;
extern struct i2c_driver ois_rumbaS6_driver;

/* exported by the SMFC JPEG driver */
int exynos_smfc_wait_done(bool enable_hwfc);

#endif

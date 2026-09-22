// SPDX-License-Identifier: GPL-2.0-only
/*
 * MPU-6050 family — I2C bus front-end.
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "mpu6050.h"

static int mpu6050_i2c_probe(struct i2c_client *client)
{
	enum mpu6050_variant variant;
	struct regmap *regmap;

	variant = (enum mpu6050_variant)(uintptr_t)
			device_get_match_data(&client->dev);

	regmap = devm_regmap_init_i2c(client, &mpu6050_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "regmap init failed\n");

	return mpu6050_core_probe(&client->dev, regmap, client->irq, variant);
}

static const struct of_device_id mpu6050_of_match[] = {
	{ .compatible = "invensense,mpu6050", .data = (void *)MPU6050 },
	{ .compatible = "invensense,mpu6500", .data = (void *)MPU6500 },
	{ .compatible = "invensense,mpu9250", .data = (void *)MPU9250 },
	{ }
};
MODULE_DEVICE_TABLE(of, mpu6050_of_match);

static const struct i2c_device_id mpu6050_i2c_id[] = {
	{ "mpu6050", MPU6050 },
	{ "mpu6500", MPU6500 },
	{ "mpu9250", MPU9250 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mpu6050_i2c_id);

static struct i2c_driver mpu6050_i2c_driver = {
	.driver = {
		.name = "mpu6050_i2c",
		.of_match_table = mpu6050_of_match,
		.pm = &mpu6050_pm_ops,
	},
	.probe = mpu6050_i2c_probe,
	.id_table = mpu6050_i2c_id,
};
module_i2c_driver(mpu6050_i2c_driver);

MODULE_AUTHOR("fyy0619-cell");
MODULE_DESCRIPTION("MPU-6050 family I2C front-end");
MODULE_LICENSE("GPL");

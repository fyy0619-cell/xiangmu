// SPDX-License-Identifier: GPL-2.0-only
/*
 * MPU-6050 family — factory self-test, surfaced as a sysfs attribute.
 *
 * Reading in_accel_self_test / in_anglvel_self_test drives the on-chip
 * self-test actuators, measures the response (self-test enabled minus
 * disabled), and reports pass/fail per axis. Kernel code is integer-only:
 * the response is validated against absolute count windows derived from the
 * datasheet self-test-response specification rather than the floating-point
 * factory-trim polynomial (documented as an extension).
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#include <linux/delay.h>

#include <linux/iio/sysfs.h>

#include "mpu6050.h"

#define MPU6050_ST_SAMPLES		16
/* Acceptable self-test response windows in raw LSB (@ default FSR) */
#define MPU6050_ST_ACCEL_MIN		300
#define MPU6050_ST_ACCEL_MAX		15000
#define MPU6050_ST_GYRO_MIN		150
#define MPU6050_ST_GYRO_MAX		12000

/* Average MPU6050_ST_SAMPLES readings of a single data register (raw LSB). */
static int mpu6050_st_average(struct mpu6050_state *st, u8 reg, int *out)
{
	__be16 raw;
	int i, ret;
	long acc = 0;

	for (i = 0; i < MPU6050_ST_SAMPLES; i++) {
		ret = regmap_bulk_read(st->regmap, reg, &raw, sizeof(raw));
		if (ret)
			return ret;
		acc += (s16)be16_to_cpu(raw);
		usleep_range(1000, 1500);
	}
	*out = acc / MPU6050_ST_SAMPLES;
	return 0;
}

static bool mpu6050_st_in_window(int response, int min, int max)
{
	response = abs(response);
	return response >= min && response <= max;
}

/* Run self-test for accel (is_accel=true) or gyro; returns 3-bit pass mask. */
static int mpu6050_run_selftest(struct mpu6050_state *st, bool is_accel,
				int *pass_mask)
{
	u8 cfg_reg = is_accel ? MPU6050_REG_ACCEL_CONFIG
			      : MPU6050_REG_GYRO_CONFIG;
	u8 st_mask = is_accel ? MPU6050_ACCEL_CONFIG_ST_MASK
			      : MPU6050_GYRO_CONFIG_ST_MASK;
	u8 base_reg = is_accel ? MPU6050_REG_ACCEL_XOUT_H
			       : MPU6050_REG_GYRO_XOUT_H;
	int lo = is_accel ? MPU6050_ST_ACCEL_MIN : MPU6050_ST_GYRO_MIN;
	int hi = is_accel ? MPU6050_ST_ACCEL_MAX : MPU6050_ST_GYRO_MAX;
	int axis, off_val, on_val, ret;

	*pass_mask = 0;
	for (axis = 0; axis < 3; axis++) {
		u8 reg = base_reg + axis * 2;

		ret = regmap_update_bits(st->regmap, cfg_reg, st_mask, 0);
		if (ret)
			return ret;
		msleep(25);
		ret = mpu6050_st_average(st, reg, &off_val);
		if (ret)
			return ret;

		ret = regmap_update_bits(st->regmap, cfg_reg, st_mask, st_mask);
		if (ret)
			return ret;
		msleep(25);
		ret = mpu6050_st_average(st, reg, &on_val);
		if (ret)
			return ret;

		if (mpu6050_st_in_window(on_val - off_val, lo, hi))
			*pass_mask |= BIT(axis);
	}

	/* Always restore normal operation */
	return regmap_update_bits(st->regmap, cfg_reg, st_mask, 0);
}

/*
 * Single self_test read callback, shared by accel and gyro channels.
 * It dispatches on chan->type so one ext_info table serves both sensors.
 */
static ssize_t mpu6050_selftest_read(struct iio_dev *indio_dev,
				     uintptr_t priv,
				     const struct iio_chan_spec *chan,
				     char *buf)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	bool is_accel = chan->type == IIO_ACCEL;
	int mask, ret;

	ret = iio_device_claim_direct_mode(indio_dev);
	if (ret)
		return ret;
	mutex_lock(&st->lock);
	ret = mpu6050_run_selftest(st, is_accel, &mask);
	mutex_unlock(&st->lock);
	iio_device_release_direct_mode(indio_dev);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s x=%d y=%d z=%d\n",
			  mask == 0x7 ? "pass" : "fail",
			  !!(mask & BIT(0)), !!(mask & BIT(1)),
			  !!(mask & BIT(2)));
}

const struct iio_chan_spec_ext_info mpu6050_ext_info[] = {
	{
		.name = "self_test",
		.shared = IIO_SHARED_BY_TYPE,
		.read = mpu6050_selftest_read,
	},
	{ }
};

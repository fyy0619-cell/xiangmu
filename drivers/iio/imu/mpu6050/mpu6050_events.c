// SPDX-License-Identifier: GPL-2.0-only
/*
 * MPU-6050 family — motion-detection threshold events.
 *
 * Exposes the accelerometer motion interrupt as a standard IIO rising
 * magnitude event so userspace can arm "wake on movement" without polling.
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#include <linux/iio/events.h>

#include "mpu6050.h"

#define MPU6050_REG_MOT_DETECT_CTRL	0x69
#define MPU6050_MOT_DETECT_ACCEL_DELAY	GENMASK(5, 4)	/* 4 samples delay */
#define MPU6050_MOT_DUR_DEFAULT		1		/* 1 ms */

int mpu6050_read_event_config(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, MPU6050_REG_INT_ENABLE, &val);
	if (ret)
		return ret;
	return !!(val & MPU6050_INT_MOTION);
}

int mpu6050_write_event_config(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir, int state)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	if (state) {
		/* Recommended MPU-6050 motion-detect bring-up sequence */
		regmap_write(st->regmap, MPU6050_REG_MOT_DUR,
			     MPU6050_MOT_DUR_DEFAULT);
		regmap_update_bits(st->regmap, MPU6050_REG_MOT_DETECT_CTRL,
				   MPU6050_MOT_DETECT_ACCEL_DELAY,
				   MPU6050_MOT_DETECT_ACCEL_DELAY);
	}
	ret = regmap_update_bits(st->regmap, MPU6050_REG_INT_ENABLE,
				 MPU6050_INT_MOTION,
				 state ? MPU6050_INT_MOTION : 0);
	mutex_unlock(&st->lock);
	return ret;
}

int mpu6050_read_event_value(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     enum iio_event_type type,
			     enum iio_event_direction dir,
			     enum iio_event_info info, int *val, int *val2)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	unsigned int thr;
	int ret;

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	ret = regmap_read(st->regmap, MPU6050_REG_MOT_THR, &thr);
	if (ret)
		return ret;

	/* Threshold LSB is 32 mg; report in raw register counts */
	*val = thr;
	return IIO_VAL_INT;
}

int mpu6050_write_event_value(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir,
			      enum iio_event_info info, int val, int val2)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;
	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&st->lock);
	ret = regmap_write(st->regmap, MPU6050_REG_MOT_THR, val);
	mutex_unlock(&st->lock);
	return ret;
}

int mpu6050_events_setup(struct iio_dev *indio_dev)
{
	struct mpu6050_state *st = iio_priv(indio_dev);

	/* Start disarmed with a sane default threshold (~1.3 g) */
	return regmap_write(st->regmap, MPU6050_REG_MOT_THR, 40);
}

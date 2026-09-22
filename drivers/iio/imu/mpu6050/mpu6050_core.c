// SPDX-License-Identifier: GPL-2.0-only
/*
 * MPU-6050 family 6-axis IMU — core: device model, channels, PM.
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/sysfs.h>

#include "mpu6050.h"

#define MPU6050_RUNTIME_AUTOSUSPEND_MS	2000

const struct mpu6050_chip_info mpu6050_chip_info[] = {
	[MPU6050] = { .name = "mpu6050", .whoami = 0x68, .has_fifo = true },
	[MPU6500] = { .name = "mpu6500", .whoami = 0x70, .has_fifo = true },
	[MPU9250] = { .name = "mpu9250", .whoami = 0x71, .has_fifo = true },
};
EXPORT_SYMBOL_GPL(mpu6050_chip_info);

/*
 * Accelerometer scale in m/s^2 per LSB for the four full-scale ranges
 * (+/-2/4/8/16 g). value = 9.80665 * fsr_g / 32768.
 */
const struct mpu6050_fsr mpu6050_accel_fsr[4] = {
	{ 0, 598550 },	/* +/- 2g  */
	{ 0, 1197101 },	/* +/- 4g  */
	{ 0, 2394202 },	/* +/- 8g  */
	{ 0, 4788403 },	/* +/- 16g */
};
EXPORT_SYMBOL_GPL(mpu6050_accel_fsr);

/*
 * Gyroscope scale in rad/s per LSB for +/-250/500/1000/2000 dps.
 * value = (pi/180) * fsr_dps / 32768.
 */
const struct mpu6050_fsr mpu6050_gyro_fsr[4] = {
	{ 0, 133231 },	/* +/- 250 dps  */
	{ 0, 266462 },	/* +/- 500 dps  */
	{ 0, 532113 },	/* +/- 1000 dps */
	{ 0, 1064225 },	/* +/- 2000 dps */
};
EXPORT_SYMBOL_GPL(mpu6050_gyro_fsr);

const struct regmap_config mpu6050_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MPU6050_REG_WHO_AM_I,
};
EXPORT_SYMBOL_GPL(mpu6050_regmap_config);

/* Supported output data rates (Hz), used for *_available and validation */
static const int mpu6050_samp_freq_avail[] = { 10, 25, 50, 100, 200, 500, 1000 };

/* Motion-detection event shared by the three accel axes */
static const struct iio_event_spec mpu6050_motion_event = {
	.type = IIO_EV_TYPE_MAG,
	.dir = IIO_EV_DIR_RISING,
	.mask_separate = BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_ENABLE),
};

#define MPU6050_ACCEL_CHANNEL(_axis, _si) {				\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SCALE),	\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.info_mask_shared_by_all_available =				\
					BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.ext_info = mpu6050_ext_info,					\
	.event_spec = &mpu6050_motion_event,				\
	.num_event_specs = 1,						\
	.scan_index = _si,						\
	.scan_type = { .sign = 's', .realbits = 16,			\
		       .storagebits = 16, .endianness = IIO_BE },	\
}

#define MPU6050_GYRO_CHANNEL(_axis, _si) {				\
	.type = IIO_ANGL_VEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SCALE),	\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.info_mask_shared_by_all_available =				\
					BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.ext_info = mpu6050_ext_info,					\
	.scan_index = _si,						\
	.scan_type = { .sign = 's', .realbits = 16,			\
		       .storagebits = 16, .endianness = IIO_BE },	\
}

static const struct iio_chan_spec mpu6050_channels[] = {
	MPU6050_ACCEL_CHANNEL(X, MPU6050_SCAN_ACCEL_X),
	MPU6050_ACCEL_CHANNEL(Y, MPU6050_SCAN_ACCEL_Y),
	MPU6050_ACCEL_CHANNEL(Z, MPU6050_SCAN_ACCEL_Z),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = MPU6050_SCAN_TEMP,
		.scan_type = { .sign = 's', .realbits = 16,
			       .storagebits = 16, .endianness = IIO_BE },
	},
	MPU6050_GYRO_CHANNEL(X, MPU6050_SCAN_GYRO_X),
	MPU6050_GYRO_CHANNEL(Y, MPU6050_SCAN_GYRO_Y),
	MPU6050_GYRO_CHANNEL(Z, MPU6050_SCAN_GYRO_Z),
	IIO_CHAN_SOFT_TIMESTAMP(MPU6050_SCAN_TIMESTAMP),
};

/* Data register for a given accel/gyro axis or temperature */
static int mpu6050_data_reg(const struct iio_chan_spec *chan)
{
	switch (chan->type) {
	case IIO_ACCEL:
		return MPU6050_REG_ACCEL_XOUT_H +
		       (chan->channel2 - IIO_MOD_X) * 2;
	case IIO_ANGL_VEL:
		return MPU6050_REG_GYRO_XOUT_H +
		       (chan->channel2 - IIO_MOD_X) * 2;
	case IIO_TEMP:
		return MPU6050_REG_TEMP_OUT_H;
	default:
		return -EINVAL;
	}
}

int mpu6050_read_channel_raw(struct mpu6050_state *st,
			     const struct iio_chan_spec *chan, int *val)
{
	__be16 raw;
	int reg, ret;

	reg = mpu6050_data_reg(chan);
	if (reg < 0)
		return reg;

	ret = regmap_bulk_read(st->regmap, reg, &raw, sizeof(raw));
	if (ret)
		return ret;

	*val = (s16)be16_to_cpu(raw);
	return 0;
}

int mpu6050_set_samp_freq(struct mpu6050_state *st, int freq)
{
	unsigned int div;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(mpu6050_samp_freq_avail); i++)
		if (mpu6050_samp_freq_avail[i] == freq)
			break;
	if (i == ARRAY_SIZE(mpu6050_samp_freq_avail))
		return -EINVAL;

	div = MPU6050_INTERNAL_SAMPLE_RATE / freq - 1;
	ret = regmap_write(st->regmap, MPU6050_REG_SMPLRT_DIV, div);
	if (ret)
		return ret;

	st->samp_freq = freq;
	return 0;
}

static int mpu6050_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;
		ret = pm_runtime_resume_and_get(st->dev);
		if (ret < 0) {
			iio_device_release_direct_mode(indio_dev);
			return ret;
		}
		mutex_lock(&st->lock);
		ret = mpu6050_read_channel_raw(st, chan, val);
		mutex_unlock(&st->lock);
		pm_runtime_mark_last_busy(st->dev);
		pm_runtime_put_autosuspend(st->dev);
		iio_device_release_direct_mode(indio_dev);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			*val = mpu6050_accel_fsr[st->accel_fsr].scale_int;
			*val2 = mpu6050_accel_fsr[st->accel_fsr].scale_nano;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			*val = mpu6050_gyro_fsr[st->gyro_fsr].scale_int;
			*val2 = mpu6050_gyro_fsr[st->gyro_fsr].scale_nano;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:
			/* degrees C: 1000/340 milli-C per LSB */
			*val = 1000;
			*val2 = 340;
			return IIO_VAL_FRACTIONAL;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		/* temperature only: 36.53 degC == 36.53*340 raw LSB */
		*val = 12420;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = st->samp_freq;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int mpu6050_write_fsr(struct mpu6050_state *st,
			     const struct iio_chan_spec *chan, int val2)
{
	const struct mpu6050_fsr *tbl;
	unsigned int reg, mask, shift;
	int i;

	if (chan->type == IIO_ACCEL) {
		tbl = mpu6050_accel_fsr;
		reg = MPU6050_REG_ACCEL_CONFIG;
		mask = MPU6050_ACCEL_CONFIG_FS_MASK;
		shift = MPU6050_ACCEL_CONFIG_FS_SHIFT;
	} else {
		tbl = mpu6050_gyro_fsr;
		reg = MPU6050_REG_GYRO_CONFIG;
		mask = MPU6050_GYRO_CONFIG_FS_MASK;
		shift = MPU6050_GYRO_CONFIG_FS_SHIFT;
	}

	for (i = 0; i < 4; i++)
		if (tbl[i].scale_int == 0 && tbl[i].scale_nano == val2)
			break;
	if (i == 4)
		return -EINVAL;

	if (chan->type == IIO_ACCEL)
		st->accel_fsr = i;
	else
		st->gyro_fsr = i;

	return regmap_update_bits(st->regmap, reg, mask, i << shift);
}

static int mpu6050_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	ret = iio_device_claim_direct_mode(indio_dev);
	if (ret)
		return ret;

	mutex_lock(&st->lock);
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (val != 0) {
			ret = -EINVAL;
			break;
		}
		ret = mpu6050_write_fsr(st, chan, val2);
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = mpu6050_set_samp_freq(st, val);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&st->lock);

	iio_device_release_direct_mode(indio_dev);
	return ret;
}

static int mpu6050_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_INT_PLUS_NANO;
		if (chan->type == IIO_ACCEL) {
			*vals = (const int *)mpu6050_accel_fsr;
			*length = ARRAY_SIZE(mpu6050_accel_fsr) * 2;
		} else {
			*vals = (const int *)mpu6050_gyro_fsr;
			*length = ARRAY_SIZE(mpu6050_gyro_fsr) * 2;
		}
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = mpu6050_samp_freq_avail;
		*length = ARRAY_SIZE(mpu6050_samp_freq_avail);
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mpu6050_iio_info = {
	.read_raw = mpu6050_read_raw,
	.write_raw = mpu6050_write_raw,
	.read_avail = mpu6050_read_avail,
	.read_event_config = mpu6050_read_event_config,
	.write_event_config = mpu6050_write_event_config,
	.read_event_value = mpu6050_read_event_value,
	.write_event_value = mpu6050_write_event_value,
	.hwfifo_set_watermark = mpu6050_hwfifo_set_watermark,
	.hwfifo_flush_to_buffer = mpu6050_hwfifo_flush_to_buffer,
};

/* ------------------------------------------------------------------ */
/* Power management                                                  */
/* ------------------------------------------------------------------ */
static int mpu6050_set_power(struct mpu6050_state *st, bool on)
{
	return regmap_update_bits(st->regmap, MPU6050_REG_PWR_MGMT_1,
				  MPU6050_PWR1_SLEEP,
				  on ? 0 : MPU6050_PWR1_SLEEP);
}

static int mpu6050_chip_init(struct mpu6050_state *st)
{
	int ret;

	/* Reset then wait for the device to settle */
	ret = regmap_write(st->regmap, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	/* Wake, select gyro-X PLL as clock source (better stability) */
	ret = regmap_write(st->regmap, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_CLKSEL_PLL_X);
	if (ret)
		return ret;
	msleep(30);

	/* DLPF ~44 Hz bandwidth, gyro base rate becomes 1 kHz */
	ret = regmap_update_bits(st->regmap, MPU6050_REG_CONFIG,
				 MPU6050_CONFIG_DLPF_MASK, 0x03);
	if (ret)
		return ret;

	ret = mpu6050_set_samp_freq(st, 100);
	if (ret)
		return ret;

	/* Default full-scale: +/-2g, +/-250 dps */
	st->accel_fsr = 0;
	st->gyro_fsr = 0;
	regmap_update_bits(st->regmap, MPU6050_REG_ACCEL_CONFIG,
			   MPU6050_ACCEL_CONFIG_FS_MASK, 0);
	regmap_update_bits(st->regmap, MPU6050_REG_GYRO_CONFIG,
			   MPU6050_GYRO_CONFIG_FS_MASK, 0);
	return 0;
}

static void mpu6050_regulator_disable(void *data)
{
	struct mpu6050_state *st = data;

	if (!IS_ERR_OR_NULL(st->vddio))
		regulator_disable(st->vddio);
	if (!IS_ERR_OR_NULL(st->vdd))
		regulator_disable(st->vdd);
}

static int mpu6050_regulators_enable(struct mpu6050_state *st)
{
	int ret;

	st->vdd = devm_regulator_get_optional(st->dev, "vdd");
	st->vddio = devm_regulator_get_optional(st->dev, "vddio");

	if (!IS_ERR(st->vdd)) {
		ret = regulator_enable(st->vdd);
		if (ret)
			return ret;
	}
	if (!IS_ERR(st->vddio)) {
		ret = regulator_enable(st->vddio);
		if (ret) {
			if (!IS_ERR(st->vdd))
				regulator_disable(st->vdd);
			return ret;
		}
	}
	/* Power-on time before the chip answers on the bus */
	msleep(35);
	return devm_add_action_or_reset(st->dev,
					mpu6050_regulator_disable, st);
}

int mpu6050_core_probe(struct device *dev, struct regmap *regmap, int irq,
		       enum mpu6050_variant variant)
{
	struct iio_dev *indio_dev;
	struct mpu6050_state *st;
	unsigned int whoami;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->regmap = regmap;
	st->dev = dev;
	st->irq = irq;
	st->chip = &mpu6050_chip_info[variant];
	mutex_init(&st->lock);
	dev_set_drvdata(dev, indio_dev);

	ret = mpu6050_regulators_enable(st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable regulators\n");

	ret = regmap_read(regmap, MPU6050_REG_WHO_AM_I, &whoami);
	if (ret)
		return dev_err_probe(dev, ret, "WHO_AM_I read failed\n");
	if (whoami != st->chip->whoami)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected WHO_AM_I 0x%02x (want 0x%02x)\n",
				     whoami, st->chip->whoami);

	ret = mpu6050_chip_init(st);
	if (ret)
		return dev_err_probe(dev, ret, "chip init failed\n");

	indio_dev->name = st->chip->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &mpu6050_iio_info;
	indio_dev->channels = mpu6050_channels;
	indio_dev->num_channels = ARRAY_SIZE(mpu6050_channels);

	ret = mpu6050_buffer_setup(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "buffer setup failed\n");

	if (irq > 0) {
		ret = mpu6050_events_setup(indio_dev);
		if (ret)
			return dev_err_probe(dev, ret, "event setup failed\n");
	}

	/* Runtime PM: autosuspend to sleep mode when idle */
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(dev, MPU6050_RUNTIME_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "iio register failed\n");

	dev_info(dev, "%s registered (irq=%d, fifo=%s)\n", st->chip->name,
		 irq, st->chip->has_fifo ? "yes" : "no");
	return 0;
}
EXPORT_SYMBOL_GPL(mpu6050_core_probe);

static int mpu6050_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	ret = mpu6050_set_power(st, false);
	mutex_unlock(&st->lock);
	return ret;
}

static int mpu6050_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	ret = mpu6050_set_power(st, true);
	mutex_unlock(&st->lock);
	usleep_range(2000, 3000);
	return ret;
}

EXPORT_SYMBOL_GPL(mpu6050_pm_ops);
const struct dev_pm_ops mpu6050_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(mpu6050_runtime_suspend,
			   mpu6050_runtime_resume, NULL)
};

MODULE_AUTHOR("fyy0619-cell");
MODULE_DESCRIPTION("Invensense MPU-6050 family IIO core driver");
MODULE_LICENSE("GPL");

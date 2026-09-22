/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MPU-6050 family 6-axis IMU driver — shared definitions.
 *
 * Supports MPU-6050 / MPU-6500 / MPU-9250 (accel + gyro path) behind a
 * common chip_info abstraction, in the style of an upstream vendor driver.
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#ifndef _MPU6050_H_
#define _MPU6050_H_

#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>

/* ------------------------------------------------------------------ */
/* Register map (bank 0)                                              */
/* ------------------------------------------------------------------ */
#define MPU6050_REG_SELF_TEST_X		0x0D
#define MPU6050_REG_SELF_TEST_Y		0x0E
#define MPU6050_REG_SELF_TEST_Z		0x0F
#define MPU6050_REG_SELF_TEST_A		0x10
#define MPU6050_REG_SMPLRT_DIV		0x19
#define MPU6050_REG_CONFIG		0x1A
#define MPU6050_REG_GYRO_CONFIG		0x1B
#define MPU6050_REG_ACCEL_CONFIG	0x1C
#define MPU6050_REG_MOT_THR		0x1F
#define MPU6050_REG_MOT_DUR		0x20
#define MPU6050_REG_FIFO_EN		0x23
#define MPU6050_REG_INT_PIN_CFG		0x37
#define MPU6050_REG_INT_ENABLE		0x38
#define MPU6050_REG_INT_STATUS		0x3A
#define MPU6050_REG_ACCEL_XOUT_H	0x3B	/* 14 contiguous data bytes */
#define MPU6050_REG_TEMP_OUT_H		0x41
#define MPU6050_REG_GYRO_XOUT_H		0x43
#define MPU6050_REG_SIGNAL_PATH_RESET	0x68
#define MPU6050_REG_USER_CTRL		0x6A
#define MPU6050_REG_PWR_MGMT_1		0x6B
#define MPU6050_REG_PWR_MGMT_2		0x6C
#define MPU6050_REG_FIFO_COUNT_H	0x72
#define MPU6050_REG_FIFO_R_W		0x74
#define MPU6050_REG_WHO_AM_I		0x75

/* CONFIG (0x1A) */
#define MPU6050_CONFIG_DLPF_MASK	GENMASK(2, 0)

/* GYRO_CONFIG (0x1B) */
#define MPU6050_GYRO_CONFIG_FS_SHIFT	3
#define MPU6050_GYRO_CONFIG_FS_MASK	GENMASK(4, 3)
#define MPU6050_GYRO_CONFIG_ST_MASK	GENMASK(7, 5)	/* XG/YG/ZG self-test */

/* ACCEL_CONFIG (0x1C) */
#define MPU6050_ACCEL_CONFIG_FS_SHIFT	3
#define MPU6050_ACCEL_CONFIG_FS_MASK	GENMASK(4, 3)
#define MPU6050_ACCEL_CONFIG_ST_MASK	GENMASK(7, 5)	/* XA/YA/ZA self-test */

/* FIFO_EN (0x23) */
#define MPU6050_FIFO_EN_TEMP		BIT(7)
#define MPU6050_FIFO_EN_GYRO_X		BIT(6)
#define MPU6050_FIFO_EN_GYRO_Y		BIT(5)
#define MPU6050_FIFO_EN_GYRO_Z		BIT(4)
#define MPU6050_FIFO_EN_ACCEL		BIT(3)
#define MPU6050_FIFO_EN_ALL		(MPU6050_FIFO_EN_TEMP | \
					 MPU6050_FIFO_EN_GYRO_X | \
					 MPU6050_FIFO_EN_GYRO_Y | \
					 MPU6050_FIFO_EN_GYRO_Z | \
					 MPU6050_FIFO_EN_ACCEL)

/* INT_PIN_CFG (0x37) */
#define MPU6050_INT_PIN_CFG_LATCH_EN	BIT(5)
#define MPU6050_INT_PIN_CFG_RD_CLEAR	BIT(4)	/* any-read clears INT status */
#define MPU6050_INT_PIN_CFG_OPEN_DRAIN	BIT(6)
#define MPU6050_INT_PIN_CFG_ACTIVE_LOW	BIT(7)

/* INT_ENABLE / INT_STATUS (0x38 / 0x3A) */
#define MPU6050_INT_DATA_RDY		BIT(0)
#define MPU6050_INT_FIFO_OFLOW		BIT(4)
#define MPU6050_INT_MOTION		BIT(6)

/* USER_CTRL (0x6A) */
#define MPU6050_USER_CTRL_FIFO_EN	BIT(6)
#define MPU6050_USER_CTRL_FIFO_RESET	BIT(2)
#define MPU6050_USER_CTRL_SIG_COND_RST	BIT(0)

/* PWR_MGMT_1 (0x6B) */
#define MPU6050_PWR1_DEVICE_RESET	BIT(7)
#define MPU6050_PWR1_SLEEP		BIT(6)
#define MPU6050_PWR1_CYCLE		BIT(5)
#define MPU6050_PWR1_TEMP_DIS		BIT(3)
#define MPU6050_PWR1_CLKSEL_MASK	GENMASK(2, 0)
#define MPU6050_PWR1_CLKSEL_PLL_X	0x01

/* Base output rate with DLPF enabled (Hz) */
#define MPU6050_INTERNAL_SAMPLE_RATE	1000
/* One packed sample in the hardware FIFO when all sensors are queued */
#define MPU6050_FIFO_SAMPLE_BYTES	14
#define MPU6050_FIFO_HW_SIZE		1024

/* Scan buffer layout — order matches the ACCEL_XOUT_H..GYRO_ZOUT_L block */
enum mpu6050_scan_index {
	MPU6050_SCAN_ACCEL_X,
	MPU6050_SCAN_ACCEL_Y,
	MPU6050_SCAN_ACCEL_Z,
	MPU6050_SCAN_TEMP,
	MPU6050_SCAN_GYRO_X,
	MPU6050_SCAN_GYRO_Y,
	MPU6050_SCAN_GYRO_Z,
	MPU6050_SCAN_TIMESTAMP,
};

/* ------------------------------------------------------------------ */
/* Chip variant abstraction                                          */
/* ------------------------------------------------------------------ */
enum mpu6050_variant {
	MPU6050,
	MPU6500,
	MPU9250,
};

/**
 * struct mpu6050_chip_info - per-variant capability table
 * @name:	human readable device name (iio_dev->name)
 * @whoami:	expected WHO_AM_I value
 * @has_fifo:	hardware FIFO usable for watermark batching
 */
struct mpu6050_chip_info {
	const char *name;
	u8 whoami;
	bool has_fifo;
};

extern const struct mpu6050_chip_info mpu6050_chip_info[];

/* Full-scale range descriptor: raw LSB -> SI, exported as *_scale_available */
struct mpu6050_fsr {
	int scale_int;		/* integer part for IIO_VAL_INT_PLUS_NANO */
	int scale_nano;		/* fractional part in nano */
};

/* ------------------------------------------------------------------ */
/* Driver state                                                      */
/* ------------------------------------------------------------------ */
/**
 * struct mpu6050_state - per-device runtime state
 * @regmap:	register access (i2c today, spi-ready)
 * @dev:	backing device (for PM / logging)
 * @lock:	serialises config writes vs. data reads
 * @chip:	variant capability table
 * @trig:	data-ready / FIFO-watermark IIO trigger
 * @irq:	INT line (0 if none wired)
 * @accel_fsr:	index into mpu6050_accel_fsr[]
 * @gyro_fsr:	index into mpu6050_gyro_fsr[]
 * @samp_freq:	configured ODR in Hz
 * @watermark:	FIFO watermark in samples (0 => data-ready mode)
 * @vdd, @vddio: optional regulators
 */
struct mpu6050_state {
	struct regmap *regmap;
	struct device *dev;
	struct mutex lock;
	const struct mpu6050_chip_info *chip;
	struct iio_trigger *trig;
	int irq;
	unsigned int accel_fsr;
	unsigned int gyro_fsr;
	unsigned int samp_freq;
	unsigned int watermark;
	struct regulator *vdd;
	struct regulator *vddio;
};

extern const struct mpu6050_fsr mpu6050_accel_fsr[4];
extern const struct mpu6050_fsr mpu6050_gyro_fsr[4];
extern const struct regmap_config mpu6050_regmap_config;

/* core.c — shared probe entry used by every bus front-end */
int mpu6050_core_probe(struct device *dev, struct regmap *regmap, int irq,
		       enum mpu6050_variant variant);
extern const struct dev_pm_ops mpu6050_pm_ops;

/* helpers reused across compilation units */
int mpu6050_set_samp_freq(struct mpu6050_state *st, int freq);
int mpu6050_read_channel_raw(struct mpu6050_state *st,
			     const struct iio_chan_spec *chan, int *val);

/* buffer.c — triggered buffer + hardware FIFO watermark path */
int mpu6050_buffer_setup(struct iio_dev *indio_dev);
int mpu6050_hwfifo_set_watermark(struct iio_dev *indio_dev, unsigned int val);
int mpu6050_hwfifo_flush_to_buffer(struct iio_dev *indio_dev, unsigned int count);

/* events.c — motion-detection threshold events */
int mpu6050_events_setup(struct iio_dev *indio_dev);
int mpu6050_read_event_config(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type, enum iio_event_direction dir);
int mpu6050_write_event_config(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type, enum iio_event_direction dir,
			       int state);
int mpu6050_read_event_value(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     enum iio_event_type type, enum iio_event_direction dir,
			     enum iio_event_info info, int *val, int *val2);
int mpu6050_write_event_value(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type, enum iio_event_direction dir,
			      enum iio_event_info info, int val, int val2);

/* selftest.c — factory self-test, exposed via sysfs */
extern const struct iio_chan_spec_ext_info mpu6050_ext_info[];

#endif /* _MPU6050_H_ */

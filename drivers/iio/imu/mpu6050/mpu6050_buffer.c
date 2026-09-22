// SPDX-License-Identifier: GPL-2.0-only
/*
 * MPU-6050 family — triggered buffer, data-ready trigger and hardware
 * FIFO watermark batching.
 *
 * Two capture modes share one code path:
 *   - data-ready  (watermark <= 1): every sample raises INT, the trigger
 *     handler reads the 14-byte register block once and pushes it.
 *   - FIFO batch  (watermark  > 1): the chip queues samples in its 1 KiB
 *     hardware FIFO and raises INT on overflow / flush; the handler drains
 *     whole 14-byte frames, cutting IRQ load and CPU wakeups.
 *
 * Copyright (C) 2026 fyy0619-cell
 */
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#include "mpu6050.h"

/* Push one 14-byte hardware frame (accel/temp/gyro) into the IIO buffer. */
static void mpu6050_push_frame(struct iio_dev *indio_dev, u8 *frame, s64 ts)
{
	/*
	 * The register/FIFO byte order (accel, temp, gyro) already matches
	 * enum mpu6050_scan_index, so the raw frame is a valid scan payload.
	 * iio_push_to_buffers_with_timestamp() demultiplexes by scan_mask.
	 */
	struct {
		__be16 chans[7];
		s64 ts __aligned(8);
	} scan;

	memcpy(scan.chans, frame, MPU6050_FIFO_SAMPLE_BYTES);
	iio_push_to_buffers_with_timestamp(indio_dev, &scan, ts);
}

static int mpu6050_read_fifo_count(struct mpu6050_state *st)
{
	__be16 cnt;
	int ret;

	ret = regmap_bulk_read(st->regmap, MPU6050_REG_FIFO_COUNT_H,
			       &cnt, sizeof(cnt));
	if (ret)
		return ret;
	return be16_to_cpu(cnt);
}

/* Drain complete frames from the hardware FIFO; returns frames pushed. */
static int mpu6050_drain_fifo(struct iio_dev *indio_dev, s64 ts)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	u8 frame[MPU6050_FIFO_SAMPLE_BYTES];
	int count, frames, i, ret;

	count = mpu6050_read_fifo_count(st);
	if (count < 0)
		return count;

	frames = count / MPU6050_FIFO_SAMPLE_BYTES;
	for (i = 0; i < frames; i++) {
		ret = regmap_noinc_read(st->regmap, MPU6050_REG_FIFO_R_W,
					frame, sizeof(frame));
		if (ret)
			return ret;
		mpu6050_push_frame(indio_dev, frame, ts);
	}
	return frames;
}

static irqreturn_t mpu6050_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct mpu6050_state *st = iio_priv(indio_dev);
	u8 frame[MPU6050_FIFO_SAMPLE_BYTES];
	int ret;

	mutex_lock(&st->lock);
	if (st->watermark > 1) {
		ret = mpu6050_drain_fifo(indio_dev, pf->timestamp);
	} else {
		ret = regmap_bulk_read(st->regmap, MPU6050_REG_ACCEL_XOUT_H,
				       frame, sizeof(frame));
		if (!ret)
			mpu6050_push_frame(indio_dev, frame, pf->timestamp);
	}
	mutex_unlock(&st->lock);

	if (ret < 0)
		dev_err_ratelimited(st->dev, "capture failed: %d\n", ret);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* Enable/disable the on-chip data-ready interrupt behind the trigger. */
static int mpu6050_trig_set_state(struct iio_trigger *trig, bool enable)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct mpu6050_state *st = iio_priv(indio_dev);
	unsigned int mask = MPU6050_INT_DATA_RDY | MPU6050_INT_FIFO_OFLOW;

	return regmap_update_bits(st->regmap, MPU6050_REG_INT_ENABLE,
				  mask, enable ? mask : 0);
}

static const struct iio_trigger_ops mpu6050_trigger_ops = {
	.set_trigger_state = mpu6050_trig_set_state,
};

/* Configure the hardware FIFO before streaming starts. */
static int mpu6050_buffer_preenable(struct iio_dev *indio_dev)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	ret = pm_runtime_resume_and_get(st->dev);
	if (ret < 0)
		return ret;

	if (st->watermark > 1 && st->chip->has_fifo) {
		/* Reset and enable the FIFO, queue accel+temp+gyro frames */
		regmap_update_bits(st->regmap, MPU6050_REG_USER_CTRL,
				   MPU6050_USER_CTRL_FIFO_RESET,
				   MPU6050_USER_CTRL_FIFO_RESET);
		regmap_write(st->regmap, MPU6050_REG_FIFO_EN,
			     MPU6050_FIFO_EN_ALL);
		regmap_update_bits(st->regmap, MPU6050_REG_USER_CTRL,
				   MPU6050_USER_CTRL_FIFO_EN,
				   MPU6050_USER_CTRL_FIFO_EN);
	}
	return 0;
}

static int mpu6050_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct mpu6050_state *st = iio_priv(indio_dev);

	regmap_write(st->regmap, MPU6050_REG_FIFO_EN, 0);
	regmap_update_bits(st->regmap, MPU6050_REG_USER_CTRL,
			   MPU6050_USER_CTRL_FIFO_EN, 0);

	pm_runtime_mark_last_busy(st->dev);
	pm_runtime_put_autosuspend(st->dev);
	return 0;
}

static const struct iio_buffer_setup_ops mpu6050_buffer_setup_ops = {
	.preenable = mpu6050_buffer_preenable,
	.postdisable = mpu6050_buffer_postdisable,
};

int mpu6050_hwfifo_set_watermark(struct iio_dev *indio_dev, unsigned int val)
{
	struct mpu6050_state *st = iio_priv(indio_dev);

	mutex_lock(&st->lock);
	st->watermark = val;
	mutex_unlock(&st->lock);
	return 0;
}

/* Flush queued frames on demand; returns the number of samples pushed. */
int mpu6050_hwfifo_flush_to_buffer(struct iio_dev *indio_dev, unsigned int count)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	ret = mpu6050_drain_fifo(indio_dev, iio_get_time_ns(indio_dev));
	mutex_unlock(&st->lock);
	return ret;
}

static irqreturn_t mpu6050_irq_thread(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct mpu6050_state *st = iio_priv(indio_dev);
	unsigned int status;

	if (regmap_read(st->regmap, MPU6050_REG_INT_STATUS, &status))
		return IRQ_NONE;

	if (status & (MPU6050_INT_DATA_RDY | MPU6050_INT_FIFO_OFLOW))
		iio_trigger_poll_nested(st->trig);

	if (status & MPU6050_INT_MOTION)
		iio_push_event(indio_dev,
			IIO_MOD_EVENT_CODE(IIO_ACCEL, 0, IIO_MOD_X_OR_Y_OR_Z,
					   IIO_EV_TYPE_MAG, IIO_EV_DIR_RISING),
			iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

int mpu6050_buffer_setup(struct iio_dev *indio_dev)
{
	struct mpu6050_state *st = iio_priv(indio_dev);
	struct device *dev = st->dev;
	int ret;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      mpu6050_trigger_handler,
					      &mpu6050_buffer_setup_ops);
	if (ret)
		return ret;

	if (st->irq <= 0)
		return 0;	/* sysfs-trigger only; still fully usable */

	st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d", indio_dev->name,
					  iio_device_id(indio_dev));
	if (!st->trig)
		return -ENOMEM;

	st->trig->ops = &mpu6050_trigger_ops;
	iio_trigger_set_drvdata(st->trig, indio_dev);

	ret = devm_request_threaded_irq(dev, st->irq, NULL,
					mpu6050_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"mpu6050", indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "request irq failed\n");

	ret = devm_iio_trigger_register(dev, st->trig);
	if (ret)
		return ret;

	/* Default to this device's data-ready trigger */
	indio_dev->trig = iio_trigger_get(st->trig);

	/* Push-pull, active high, clear INT status on any read */
	regmap_write(st->regmap, MPU6050_REG_INT_PIN_CFG,
		     MPU6050_INT_PIN_CFG_RD_CLEAR);
	return 0;
}

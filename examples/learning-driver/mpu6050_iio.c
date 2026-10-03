// SPDX-License-Identifier: GPL-2.0
/*
 * MPU-6050 IIO 驱动(学习版)
 * 作用:把挂在 I2C 总线上的 MPU-6050 加速度计接入 Linux IIO 子系统,
 *       让用户空间能通过 cat /sys/bus/iio/devices/iio:deviceN/in_accel_x_raw 读数据。
 */
#include <linux/module.h>          // 模块基础:module_* 宏
#include <linux/i2c.h>             // i2c_driver / i2c_client / i2c 读写
#include <linux/mod_devicetable.h> // i2c_device_id / of_device_id
#include <linux/regmap.h>          // regmap:统一的寄存器访问抽象
#include <linux/iio/iio.h>         // IIO 子系统:iio_dev / 通道 / read_raw

/* ---------- 芯片寄存器地址 ---------- */
#define MPU6050_REG_WHO_AM_I     0x75     // 身份寄存器,固定读出 0x68
#define MPU6050_WHO_AM_I_VAL     0x68     // MPU-6050 的身份码
#define MPU6050_REG_PWR_MGMT_1   0x6B     // 电源管理寄存器
#define MPU6050_PWR1_SLEEP       (1 << 6) // 该寄存器 bit6 = 睡眠位
#define MPU6050_REG_ACCEL_XOUT_H 0x3B     // 加速度 X 高字节;X/Y/Z 共 6 个寄存器连续排列
#define MPU6050_REG_GYRO_XOUT_H  0x43   // 陀螺 X 高字节;X/Y/Z 连续
#define MPU6050_REG_TEMP_OUT_H   0x41   // 温度 高字节
/*
 * 每个设备的私有数据,和 iio_dev 一起分配(见 probe 的 devm_iio_device_alloc)。
 * 这里只存了 regmap:probe 里【存】进去,read_raw 里用 iio_priv【取】回来用。
 */
struct mpu6050_data {
	struct regmap *regmap;   // 读写这颗芯片寄存器的通道
};

/* 告诉 regmap:该芯片寄存器地址 8 位、数据 8 位,最大寄存器 0x75 */
static const struct regmap_config mpu6050_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x75,
};

/*
 * 定义一个加速度通道的宏。
 * axis = X/Y/Z;idx = 第几个轴(0/1/2),存进 .address,read_raw 里用它算寄存器地址。
 * info_mask_separate 含 RAW => IIO 会自动生成 in_accel_x_raw 这类"原始值"属性文件。
 */
  //下面这段(加了陀螺宏 + 温度通道):
  /* 加速度通道宏(X/Y/Z) */
  /* 加速度通道宏:RAW(每轴各一)+ SCALE(同类共享一个) */
  #define MPU6050_ACCEL_CHANNEL(axis, idx) {                     \
        .type = IIO_ACCEL, .modified = 1,                      \
        .channel2 = IIO_MOD_##axis,                            \
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),          \
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),  \
        .address = idx,                                        \
  }

  /* 陀螺仪通道宏:同上 */
  #define MPU6050_GYRO_CHANNEL(axis, idx) {                      \
        .type = IIO_ANGL_VEL, .modified = 1,                   \
        .channel2 = IIO_MOD_##axis,                            \
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),          \
        .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),  \
        .address = idx,                                        \
  }

  /* 完整通道表:加速度 X/Y/Z + 陀螺 X/Y/Z + 温度 */
  static const struct iio_chan_spec mpu6050_channels[] = {
        MPU6050_ACCEL_CHANNEL(X, 0),
        MPU6050_ACCEL_CHANNEL(Y, 1),
        MPU6050_ACCEL_CHANNEL(Z, 2),
        MPU6050_GYRO_CHANNEL(X, 0),
        MPU6050_GYRO_CHANNEL(Y, 1),
        MPU6050_GYRO_CHANNEL(Z, 2),
        {   /* 温度:RAW + SCALE + OFFSET 都是它自己独有 */
                .type = IIO_TEMP,
                .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |BIT(IIO_CHAN_INFO_SCALE) |BIT(IIO_CHAN_INFO_OFFSET),
        },
  };

/*
 * 数据回调:用户 cat in_accel_x_raw 时,IIO 核心会回调这里。
 * 参数都由内核填好递进来:
 *   indio_dev - 哪个 IIO 设备
 *   chan      - 哪个通道(X/Y/Z)
 *   val,val2  - 把结果写回这两个(输出参数)
 *   mask      - 内核问你要哪类值(RAW/SCALE...)
 */
//read_raw 整个替换(新增 SCALE 和 OFFSET 分支):
  static int mpu6050_read_raw(struct iio_dev *indio_dev,struct iio_chan_spec const *chan,int *val, int *val2, long mask)
  {
        struct mpu6050_data *data = iio_priv(indio_dev);
        unsigned int h, l;
        int reg, ret;

        switch (mask) {
        case IIO_CHAN_INFO_RAW:
                switch (chan->type) {               /* 按类型选寄存器基址 */
                case IIO_ACCEL:
                        reg = MPU6050_REG_ACCEL_XOUT_H + chan->address * 2;
                        break;
                case IIO_ANGL_VEL:
                        reg = MPU6050_REG_GYRO_XOUT_H + chan->address * 2;
                        break;
                case IIO_TEMP:
                        reg = MPU6050_REG_TEMP_OUT_H;
                        break;
                default:
                        return -EINVAL;
                }
                ret = regmap_read(data->regmap, reg, &h);
                if (ret)
                        return ret;
                ret = regmap_read(data->regmap, reg + 1, &l);
                if (ret)
                        return ret;
                *val = (s16)((h << 8) | l);
                return IIO_VAL_INT;

        case IIO_CHAN_INFO_SCALE:
                switch (chan->type) {
                case IIO_ACCEL:        /* ±2g:9.80665/16384 ≈ 0.000598550 m/s² */
                        *val = 0; *val2 = 598550;
                        return IIO_VAL_INT_PLUS_NANO;
                case IIO_ANGL_VEL:     /* ±250dps:(π/180)/131 ≈ 0.000133231 rad/s */
                        *val = 0; *val2 = 133231;
                        return IIO_VAL_INT_PLUS_NANO;
                case IIO_TEMP:         /* 温度:1000/340(单位 milli-℃ 每 LSB) */
                        *val = 1000; *val2 = 340;
                        return IIO_VAL_FRACTIONAL;
                default:
                        return -EINVAL;
                }

        case IIO_CHAN_INFO_OFFSET:
                /* 只有温度用到:偏置 12420(以 raw 计) */
                *val = 12420;
                return IIO_VAL_INT;

        default:
                return -EINVAL;
        }
}

/* 回调表:把 read_raw 交给 IIO 核心,它需要数据时来调 */
static const struct iio_info mpu6050_info = {
	.read_raw = mpu6050_read_raw,
};

static int mpu6050_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct mpu6050_data *data;
	unsigned int whoami;
	int ret;

	/*
	 * 一次性分配 [iio_dev + 私有数据] 一整块内存;
	 * iio_priv() 取出"私有数据"那半块的指针。devm_ => 设备移除时自动释放。
	  devm_iio_device_alloc 分配的是一整块内存,里面装两样东西,前后挨着:
           ┌──────────────────────────┬──────────────────────────┐
  │   iio_dev(内核用的部分)   │  你的私有数据 mpu6050_data │
  └──────────────────────────┴──────────────────────────┘
    ↑ indio_dev 指向这里          ↑ iio_priv(indio_dev) 指向这里
      - 内核需要 iio_dev(它来管理你的设备);
  - 你需要自己的数据(regmap、锁、配置…放 struct mpu6050_data)。
         */
	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;             /* 内存不足 */
	data = iio_priv(indio_dev);

	/* 建立读写这颗芯片的"通道"(regmap),并把它存进私有数据,供 read_raw 使用 */
	data->regmap = devm_regmap_init_i2c(client, &mpu6050_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(&client->dev, "regmap init failed\n");
		return PTR_ERR(data->regmap);
	}

	/* 读 WHO_AM_I 确认接的确实是 MPU-6050 */
	ret = regmap_read(data->regmap, MPU6050_REG_WHO_AM_I, &whoami);
	if (ret) {
		dev_err(&client->dev, "read WHO_AM_I failed: %d\n", ret);
		return ret;
	}
	if (whoami == MPU6050_WHO_AM_I_VAL)
		dev_info(&client->dev, "MPU-6050 detected!\n");
	else
		dev_warn(&client->dev,
			 "unexpected WHO_AM_I 0x%02x (real chip = 0x68)\n", whoami);

	/* 唤醒芯片:清 PWR_MGMT_1 的 sleep 位(上电默认睡眠,不唤醒读不到数据) */
	regmap_update_bits(data->regmap, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_SLEEP, 0);

	/* 填好 iio_dev 各字段 */
	indio_dev->name = "mpu6050";           /* 设备名 => /sys/.../name */
	indio_dev->modes = INDIO_DIRECT_MODE;  /* 直接读取模式 */
	indio_dev->info = &mpu6050_info;        /* 回调表(含 read_raw) */
	indio_dev->channels = mpu6050_channels;                  /* 通道数组 */
	indio_dev->num_channels = ARRAY_SIZE(mpu6050_channels);  /* 通道数量 */

	/*
	 * 注册到 IIO 核心 —— 这一步"开柜台":之后 /sys/bus/iio/devices/iio:deviceN
	 * 及其下的 in_accel_*_raw 属性文件才出现,用户才能 cat。devm_ 自动反注册。
	 */
	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret) {
		dev_err(&client->dev, "iio register failed: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "registered as an IIO device\n");
	return 0;
}

/* i2c 设备匹配表:按名字 "mpu6050" 匹配(VM 上 new_device 用它) */
static const struct i2c_device_id mpu6050_id[] = {
	{ "mpu6050", 0 },
	{ }                     /* 空项 = 表结束哨兵 */
};
MODULE_DEVICE_TABLE(i2c, mpu6050_id);

/* 设备树匹配表:按 compatible 匹配(真板子用) */
static const struct of_device_id mpu6050_of_match[] = {
	{ .compatible = "invensense,mpu6050" },
	{ }
};
MODULE_DEVICE_TABLE(of, mpu6050_of_match);

/* 驱动"名片":名字、匹配表、probe、能带哪些设备 */
static struct i2c_driver mpu6050_driver = {
	.driver = {
		.name = "mpu6050_iio",
		.of_match_table = mpu6050_of_match,
	},
	.probe = mpu6050_probe,
	.id_table = mpu6050_id,
};

/* 自动生成 module_init/exit:加载时注册 i2c 驱动,卸载时注销 */
module_i2c_driver(mpu6050_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("yyf");
MODULE_DESCRIPTION("MPU-6050 IIO driver (learning)");

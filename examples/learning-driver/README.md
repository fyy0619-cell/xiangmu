# learning-driver —— 从零手写的 MPU-6050 IIO 驱动(学习版)

这是**从零一步步手写**出来的最小可用 MPU-6050 IIO 驱动,用于学习理解。它是本仓库那份[生产级驱动](../../drivers/iio/imu/mpu6050/)的"入门前身":功能更少、代码更短、注释更多,先把 IIO 驱动的核心机制吃透。

完整的从零教程与概念讲解见 [`docs/learning/`](../../docs/learning/)(01–08)。

## 功能
- 挂在 I2C 总线,按 `compatible = "invensense,mpu6050"` / 名字 `mpu6050` 匹配。
- probe:regmap 建通道 → 读 WHO_AM_I 认芯片 → 唤醒(清 sleep 位)→ 注册 IIO。
- 通道:加速度 X/Y/Z + 陀螺 X/Y/Z + 温度。
- 每个通道提供 `raw`(原始值)与 `scale`(物理量换算);温度另有 `offset`。
- `read_raw` 按通道类型选寄存器、按 `mask` 返回 raw / scale / offset。

## 在 VM 上用 i2c-stub 测试(无需真硬件)
```sh
# 1) 编译加载
make
sudo make install && sudo depmod -a && sudo modprobe mpu6050_iio

# 2) 造一颗假 MPU6050(stub),自动找适配器号 N
sudo modprobe i2c-stub chip_addr=0x68
N=$(for d in /sys/class/i2c-adapter/i2c-*; do grep -qi stub "$d/name" && echo "${d##*i2c-}"; done)

# 3) 预置身份码 + 实例化设备,触发 probe
sudo i2cset -y $N 0x68 0x75 0x68
sudo sh -c "echo mpu6050 0x68 > /sys/bus/i2c/devices/i2c-$N/new_device"

# 4) 读
D=/sys/bus/iio/devices/iio:device0
ls $D/
cat $D/in_accel_x_raw $D/in_accel_scale
cat $D/in_anglvel_scale $D/in_temp_scale $D/in_temp_offset
```
> 物理量换算:加速度 = `in_accel_x_raw × in_accel_scale`(m/s²);角速度同理(rad/s);温度 = `(in_temp_raw + in_temp_offset) / 340`(℃)。

## 上真板子(STM32MP157)
把 `KERNELDIR` 指到板子的内核源码,交叉编译后拷到板子加载;真板子靠**设备树**里的 `mpu6050@68 { compatible="invensense,mpu6050"; ... }` 节点自动匹配(无需 new_device)。

## 和生产级驱动的区别
| | 本学习版 | 生产级(drivers/iio/imu/mpu6050) |
|---|---|---|
| 目的 | 学懂机制 | 可量产 |
| 组织 | 单文件 | core/i2c 多文件 |
| 功能 | raw + scale(+temp offset) | + 多变体/FIFO/触发缓冲/事件/runtime PM/自检 |

# mpu6050-iio — 生产级 MPU-6050 系列 IIO 驱动

![build](https://github.com/fyy0619-cell/xiangmu/actions/workflows/build.yml/badge.svg)
![license](https://img.shields.io/badge/license-GPL--2.0-blue)
![subsystem](https://img.shields.io/badge/subsystem-IIO-informational)

一个按 **mainline vendor 驱动标准**编写的 Invensense MPU-6050 / MPU-6500 /
MPU-9250 6 轴 IMU Linux 驱动,挂在 **IIO(Industrial I/O)** 子系统下,目标平台
**STM32MP157(100ASK)**。刻意做到"教程止步之后"的工程完成度,用作嵌入式
Linux 驱动方向的作品集与面试深聊材料。

> 设计动机、与 V4L2 的概念对照、功耗工程,见
> [`Documentation/driver-architecture.md`](Documentation/driver-architecture.md)。

## 特性

| 能力 | 说明 | 对应文件 |
|------|------|----------|
| 多芯片变体 | 6050/6500/9250 经 `chip_info` 表抽象,core/i2c 拆分 | `mpu6050_core.c`, `mpu6050_i2c.c` |
| regmap 寄存器抽象 | 缓存 + `debugfs/regmap` + 总线无关 | 全局 |
| 完整通道 | accel xyz / gyro xyz / temp + 时间戳 | `mpu6050_core.c` |
| 可配置量程 / ODR | `*_scale_available`、`sampling_frequency_available` | `mpu6050_core.c` |
| 触发缓冲 | DRDY 数据就绪触发 + 软件触发 | `mpu6050_buffer.c` |
| 硬件 FIFO 水位 | 攒够 watermark 批量搬运,省中断/唤醒 | `mpu6050_buffer.c` |
| 运动检测事件 | wake-on-motion 阈值中断 | `mpu6050_events.c` |
| runtime PM | autosuspend 到 sleep + system suspend/resume | `mpu6050_core.c` |
| factory self-test | sysfs 读触发,整数响应窗口判定 | `mpu6050_selftest.c` |
| 工程配套 | DT binding(yaml)、ABI 文档、CI、测试程序 | `Documentation/`, `.github/`, `test/` |

## 目录结构

```
drivers/iio/imu/mpu6050/        驱动源码(core / i2c / buffer / events / selftest)
dt/                             STM32MP157 设备树 overlay
Documentation/
  devicetree/bindings/...yaml   DT binding(可过 dt_binding_check)
  ABI/testing/...               sysfs ABI 文档
  driver-architecture.md        架构与设计说明
test/                           userspace 验证程序 + 冒烟脚本
.github/workflows/build.yml     交叉编译 CI
docs/superpowers/               设计 spec 与实现计划(项目演进记录)
```

## 构建

在 Ubuntu 交叉编译环境(项目里为 VMware Ubuntu,`ssh vm`):

```bash
cd drivers/iio/imu/mpu6050
make KDIR=/path/to/stm32mp157-kernel \
     ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
# 产出:mpu6050-core.ko  mpu6050-i2c.ko
```

> **内核 API 基线**:按 IIO/PM 的 **>= 5.15 / 6.1** API 编写。若你的板子是 ST
> OpenSTLinux 5.4 BSP,以下符号需 backport 或加兼容 shim:
> `devm_pm_runtime_enable`、`pm_runtime_resume_and_get`、`regmap_noinc_read`。
> README 末尾附最小兼容方案。

## 上板加载与验证

```bash
# 1) 更新含 mpu6050@68 节点的 dtb(见 dt/ overlay),重启
# 2) 传 .ko 到板子(NFS/TFTP),加载:
insmod mpu6050-core.ko
insmod mpu6050-i2c.ko
dmesg | tail                     # 期望:mpu6050 registered (irq=..., fifo=yes)

D=/sys/bus/iio/devices/iio:device0
iio_info                         # 枚举通道/属性

# 单次读取,静止时合加速度应 ≈ 9.8 m/s^2
cat $D/in_accel_scale $D/in_accel_z_raw
cat $D/in_accel_scale_available $D/sampling_frequency_available

# DRDY 硬件触发的缓冲流(100 Hz)
echo 1 > $D/scan_elements/in_accel_x_en
echo 1 > $D/scan_elements/in_accel_y_en
echo 1 > $D/scan_elements/in_accel_z_en
echo 1 > $D/scan_elements/in_timestamp_en
echo 100 > $D/sampling_frequency
echo $(cat $D/name)-dev0 > $D/trigger/current_trigger
echo 1 > $D/buffer/enable
timeout 2 iio_readdev -b 64 iio:device0 | wc -c   # ≈200 样本/2s
echo 0 > $D/buffer/enable

# FIFO 水位批量模式(降中断)
echo 16 > $D/buffer/hwfifo_watermark

# 运动检测事件
echo 40 > $D/events/in_accel_mag_rising_value
echo 1  > $D/events/in_accel_mag_rising_en
iio_event_monitor mpu6050        # 晃动板子看事件

# 自检
cat $D/in_accel_self_test        # pass x=1 y=1 z=1
cat $D/in_anglvel_self_test
```

## 测试程序

见 [`test/`](test/):`read_raw.c`(sysfs 换算校验重力)、`stream_buffer.c`
(直接 `read()` `/dev/iio:deviceN`)、`smoke_test.sh`(上板冒烟)。交叉编译:

```bash
arm-linux-gnueabihf-gcc -O2 -o test/read_raw test/read_raw.c
arm-linux-gnueabihf-gcc -O2 -o test/stream_buffer test/stream_buffer.c
```

## 兼容 5.4 BSP 的最小 shim(可选)

```c
/* 放在私有头,内核 < 5.10/5.15 时启用 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0)
static inline int pm_runtime_resume_and_get(struct device *dev)
{ int r = pm_runtime_get_sync(dev); if (r < 0) pm_runtime_put_noidle(dev); return r; }
#endif
/* devm_pm_runtime_enable: 用 pm_runtime_enable + devm_add_action_or_reset 包装 */
```

## 许可

GPL-2.0-only。见 [LICENSE](LICENSE)。本项目为学习/作品集用途,非 Invensense 官方驱动。

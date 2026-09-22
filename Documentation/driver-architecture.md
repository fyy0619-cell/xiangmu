# MPU-6050 IIO 驱动架构说明

> 面向读者:面试官 / code reviewer / 未来的自己。本文解释**为什么这样设计**,
> 而不仅是代码做了什么。

## 1. 定位

这是一个按 **mainline vendor 驱动标准**组织的 Invensense MPU-6050 系列
(6050 / 6500 / 9250)6 轴 IMU 驱动,挂在 Linux **IIO(Industrial I/O)**
子系统下。它刻意做到"网上教程止步之后"的工程完成度:多变体抽象、硬件 FIFO
水位批量搬运、runtime PM、运动检测事件、factory self-test、完整 ABI/binding
文档与 CI。

## 2. 分层与文件

```
              用户空间: iio_info / libiio / sysfs / /dev/iio:deviceN
                                   │  统一 IIO ABI
        ┌──────────────────────────┴───────────────────────────┐
        │                    IIO core (内核)                     │
        └──────────────────────────┬───────────────────────────┘
                                   │  iio_info 回调
   mpu6050-core.ko ────────────────┼───────────────────────────
     mpu6050_core.c    设备模型 / 通道 / read|write_raw / PM / probe
     mpu6050_buffer.c  triggered buffer + DRDY 触发 + FIFO 水位
     mpu6050_events.c  运动检测阈值事件
     mpu6050_selftest.c factory self-test (sysfs)
        │  regmap (寄存器抽象, 带缓存 / debugfs)
   mpu6050-i2c.ko ─────────────────┼───────────────────────────
     mpu6050_i2c.c     I2C 前端 (module 入口, 变体匹配)
        │  I2C 总线
                              MPU-6050 硬件
```

**为什么 core / i2c 拆两个模块?** 与 mainline `inv_mpu6050`
(`inv-mpu6050.ko` + `inv-mpu6050-i2c.ko`)一致:总线无关的设备逻辑在 core,
将来加 SPI 前端只需再写一个 `mpu6050_spi.c` 调用同一个 `mpu6050_core_probe()`,
不动 core 一行。这就是"面向接口、可独立演进"的工程边界。

**为什么用 regmap?** 量产写法:寄存器读写、缓存、`debugfs/regmap/*/registers`
一次性获得,替代裸 `i2c_smbus_*`;换总线时 core 代码零改动。

## 3. 两条数据通路

### 3.1 sysfs 单次(direct mode)
```
cat in_accel_x_raw
  -> read_raw(RAW)
  -> iio_device_claim_direct_mode()   # 与缓冲互斥
  -> pm_runtime_resume_and_get()      # 唤醒芯片
  -> regmap_bulk_read(ACCEL_XOUT_H)   # 16-bit 大端
  -> IIO_VAL_INT
```

### 3.2 缓冲流式(triggered buffer)
```
选通道 scan_elements/*_en  →  buffer/enable=1
        │
   DRDY / FIFO 中断 (GPIO IRQ, threaded)
        │  iio_trigger_poll_nested()
   pollfunc: mpu6050_trigger_handler()
        ├─ watermark<=1: 单帧 14B 读 → push
        └─ watermark >1: 读 FIFO_COUNT, 逐 14B 帧 drain → push
        │  iio_push_to_buffers_with_timestamp()
   /dev/iio:deviceN  ←  用户 read()
```

**scan_mask 的意义**:用户只使能需要的通道,`iio_push_to_buffers_*` 按
scan_mask 自动裁剪 payload —— 和 V4L2 里协商 format/分辨率后只搬有效数据是
同一种"能力协商 + 按需搬运"思想。

## 4. 与 V4L2 的概念对照

| 概念 | V4L2 | 本 IIO 驱动 |
|------|------|-------------|
| 设备能力描述 | `VIDIOC_ENUM_FMT` / `v4l2_format` | `iio_chan_spec[]` + `*_available` |
| 能力协商 | 协商 format/分辨率 | 选 scan_elements + scale/ODR |
| 缓冲队列 | `vb2_queue` (videobuf2) | `iio_buffer` (kfifo) |
| 硬件触发/中断驱动搬运 | frame-done IRQ → dqbuf | DRDY/FIFO IRQ → pollfunc push |
| 用户取数据 | `VIDIOC_DQBUF` / `read()` | `read(/dev/iio:deviceN)` |
| 子设备/多前端 | subdev + media controller | core + i2c/spi 前端拆分 |

## 5. 功耗工程(高通面试重点)

- **runtime PM + autosuspend**:空闲 2s 后自动进 sleep(清 `PWR_MGMT_1.SLEEP`
  的反向),被 `read_raw` / buffer preenable 唤醒;system suspend 走
  `pm_runtime_force_suspend`。
- **FIFO 水位**:不是每个样本都中断。攒够 watermark 帧再一次性搬运,显著降低
  IRQ 次数与 CPU 唤醒 —— 手机 always-on sensor 的核心省电手段。
- **wake-on-motion 事件**:主机可休眠,靠加速度阈值中断唤醒,而非轮询。

## 6. 并发与错误处理

- `st->lock` 串行化"配置写"与"数据读";缓冲期间 `claim_direct_mode` 拒绝
  sysfs 单次读,避免寄存器竞争。
- 所有 probe 失败路径走 `dev_err_probe()`,资源全部 `devm_` 托管,无手写
  remove。
- 中断为 threaded + ONESHOT,handler 内可安全做 I2C 传输。

## 7. 已知的板级待调点(诚实说明)

- 内核 API 基线:本驱动按 **>= 5.15 / 6.1** 的 IIO/PM API 编写
  (`devm_pm_runtime_enable`、`pm_runtime_resume_and_get`、`regmap_noinc_read`)。
  100ASK STM32MP157 的 ST 5.4 BSP 需少量 backport 兼容(见 README)。
- self-test 用**整数**自检响应窗口判定(内核禁用浮点);datasheet 的浮点
  factory-trim 多项式作为后续扩展。
- FIFO 水位路径的时间戳目前对整批帧用同一 `pf->timestamp`;精确到每帧的
  时间插值是可选增强。

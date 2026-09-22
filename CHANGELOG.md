# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- Core MPU-6050 family IIO driver split into `mpu6050-core` and
  `mpu6050-i2c` modules (multi-variant: 6050 / 6500 / 9250).
- regmap-based register access with a `chip_info` capability table.
- Accelerometer, gyroscope and temperature channels with configurable
  full-scale range and output data rate, plus `*_available` lists.
- Triggered buffer with a data-ready IIO trigger, software-trigger support,
  and hardware FIFO watermark batching (`hwfifo_watermark`).
- Motion-detection (wake-on-motion) threshold events.
- Runtime PM with autosuspend to sleep mode and system suspend/resume.
- Factory self-test surfaced via `in_accel_self_test` /
  `in_anglvel_self_test`.
- Device-tree binding (`invensense,mpu6050.yaml`), sysfs ABI documentation,
  STM32MP157 overlay, architecture write-up.
- Userspace verification tools (`read_raw`, `stream_buffer`, `smoke_test.sh`)
  and a GitHub Actions build matrix (Linux 6.1 / 6.6) with checkpatch.

### Notes
- Written against the IIO/PM API baseline of Linux >= 5.15 / 6.1; a
  compatibility shim for the STM32MP157 ST 5.4 BSP is described in the README.
- Hardware bring-up validation on STM32MP157 is tracked separately in the
  implementation plan under `docs/superpowers/plans/`.

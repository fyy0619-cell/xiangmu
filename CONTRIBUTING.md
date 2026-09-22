# Contributing

This is a personal learning / portfolio project, but it follows upstream
kernel conventions on purpose — treat it like you would a patch to
`drivers/iio/`.

## Coding style

- Follow `Documentation/process/coding-style.rst` (Linux kernel style):
  tabs, 80-column soft limit, kernel-doc on exported symbols.
- Every source file starts with an `SPDX-License-Identifier` line.
- Run checkpatch before opening a PR:

  ```bash
  ./checkpatch.pl --no-tree --file drivers/iio/imu/mpu6050/*.c
  ```

## Commits

- One logical change per commit; imperative subject
  (`iio: mpu6050: add hwfifo watermark`).
- Explain *why* in the body, not just *what*.
- Sign off if you like (`git commit -s`).

## Before you push

- `make -C drivers/iio/imu/mpu6050 KDIR=<kernel>` builds clean (no new
  warnings).
- Userspace tools compile: `gcc -Wall -Wextra test/read_raw.c -lm`.
- If you touched sysfs behaviour, update
  `Documentation/ABI/testing/sysfs-bus-iio-mpu6050`.
- If you touched DT, update the binding and keep the example valid.

## Testing on hardware

On-target checks live in `test/smoke_test.sh`. Note in your PR which board
and kernel you validated on (e.g. STM32MP157, Linux 6.6).

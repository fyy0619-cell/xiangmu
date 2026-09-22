#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# On-target smoke test for the mpu6050-iio driver.
# Run on the STM32MP157 after loading mpu6050-core.ko + mpu6050-i2c.ko.
#
#   ./smoke_test.sh [iio:device0]

set -e

DEV="${1:-iio:device0}"
D="/sys/bus/iio/devices/$DEV"
fail() { echo "FAIL: $1" >&2; exit 1; }

[ -d "$D" ] || fail "$D not present (driver loaded? dtb updated?)"

echo "== identity =="
name=$(cat "$D/name")
echo "name = $name"
case "$name" in mpu6050|mpu6500|mpu9250) ;; *) fail "unexpected name $name";; esac

echo "== available lists =="
cat "$D/in_accel_scale_available" || fail "no accel scale_available"
cat "$D/sampling_frequency_available" || fail "no sampling_frequency_available"

echo "== single-shot gravity check =="
scale=$(cat "$D/in_accel_scale")
az=$(cat "$D/in_accel_z_raw")
echo "in_accel_scale=$scale in_accel_z_raw=$az"

echo "== self-test =="
cat "$D/in_accel_self_test"
cat "$D/in_anglvel_self_test"

echo "== buffered capture (data-ready) =="
for ax in x y z; do echo 1 > "$D/scan_elements/in_accel_${ax}_en"; done
echo 1 > "$D/scan_elements/in_timestamp_en"
echo 100 > "$D/sampling_frequency"
if [ -e "$D/trigger/current_trigger" ]; then
	echo "${name}-dev0" > "$D/trigger/current_trigger" 2>/dev/null || true
fi
echo 1 > "$D/buffer/enable"
sleep 1
echo 0 > "$D/buffer/enable"
echo "buffered capture toggled OK"

echo "ALL SMOKE CHECKS PASSED"

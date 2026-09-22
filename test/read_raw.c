// SPDX-License-Identifier: GPL-2.0-only
/*
 * read_raw — read accel/gyro via sysfs and convert to SI units.
 *
 * Validates the driver's scale handling: at rest the accelerometer
 * magnitude must be ~9.8 m/s^2.
 *
 *   arm-linux-gnueabihf-gcc -O2 -o read_raw read_raw.c
 *   ./read_raw [/sys/bus/iio/devices/iio:device0]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double read_attr(const char *base, const char *name)
{
	char path[256];
	double v = 0.0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", base, name);
	f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	if (fscanf(f, "%lf", &v) != 1) {
		fprintf(stderr, "failed to parse %s\n", path);
		exit(EXIT_FAILURE);
	}
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	const char *base = (argc > 1) ? argv[1]
				      : "/sys/bus/iio/devices/iio:device0";
	double as = read_attr(base, "in_accel_scale");
	double gs = read_attr(base, "in_anglvel_scale");
	double ax = read_attr(base, "in_accel_x_raw") * as;
	double ay = read_attr(base, "in_accel_y_raw") * as;
	double az = read_attr(base, "in_accel_z_raw") * as;
	double gx = read_attr(base, "in_anglvel_x_raw") * gs;
	double gy = read_attr(base, "in_anglvel_y_raw") * gs;
	double gz = read_attr(base, "in_anglvel_z_raw") * gs;
	double mag = sqrt(ax * ax + ay * ay + az * az);

	printf("accel [m/s^2]  x=%+.3f y=%+.3f z=%+.3f  |a|=%.3f\n",
	       ax, ay, az, mag);
	printf("gyro  [rad/s]  x=%+.4f y=%+.4f z=%+.4f\n", gx, gy, gz);

	if (mag < 9.0 || mag > 10.6) {
		fprintf(stderr, "FAIL: |a|=%.3f outside [9.0, 10.6] at rest\n",
			mag);
		return EXIT_FAILURE;
	}
	printf("PASS: gravity magnitude within tolerance\n");
	return EXIT_SUCCESS;
}

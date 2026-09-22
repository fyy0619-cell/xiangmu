// SPDX-License-Identifier: GPL-2.0-only
/*
 * stream_buffer — read raw scan frames from /dev/iio:deviceN.
 *
 * Enable channels + a trigger + buffer/enable via sysfs first, then run
 * this to dump the interleaved scan payload the kernel pushes.
 *
 *   arm-linux-gnueabihf-gcc -O2 -o stream_buffer stream_buffer.c
 *   ./stream_buffer [/dev/iio:device0] [frames]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/iio:device0";
	int frames = (argc > 2) ? atoi(argv[2]) : 10;
	unsigned char buf[64];
	int fd, n;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror(dev);
		return EXIT_FAILURE;
	}

	for (n = 0; n < frames; n++) {
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r <= 0) {
			perror("read");
			break;
		}
		printf("frame %2d (%zd B):", n, r);
		for (ssize_t i = 0; i < r; i++)
			printf(" %02x", buf[i]);
		putchar('\n');
	}

	close(fd);
	return EXIT_SUCCESS;
}

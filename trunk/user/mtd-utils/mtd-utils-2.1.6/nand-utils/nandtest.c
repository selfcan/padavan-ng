#define PROGRAM_NAME "nandtest"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <getopt.h>

#include <asm/types.h>
#include "mtd/mtd-user.h"
#include "common.h"

static NORETURN void usage(int status)
{
	fprintf(status ? stderr : stdout,
		"usage: %s [OPTIONS] <device>\n\n"
		"  -h, --help           Display this help output\n"
		"  -V, --version        Display version information and exit\n"
		"  -m, --markbad        Mark blocks bad if they appear so\n"
		"  -s, --seed           Supply random seed\n"
		"  -p, --passes         Number of passes\n"
		"  -r <n>, --reads=<n>  Read & check <n> times per pass\n"
		"  -o, --offset         Start offset on flash\n"
		"  -l, --length         Length of flash to test\n"
		"  -k, --keep           Restore existing contents after test\n",
		PROGRAM_NAME);
	exit(status);
}

struct mtd_info_user meminfo;
struct mtd_ecc_stats oldstats, newstats;
int fd;
int markbad=0;
int seed;

static int read_and_compare(loff_t ofs, unsigned char *data,
			    unsigned char *rbuf)
{
	ssize_t len;
	int i;

	len = pread(fd, rbuf, meminfo.erasesize, ofs);
	if (len < meminfo.erasesize) {
		printf("\n");
		if (len)
			fprintf(stderr, "Short read (%zd bytes)\n", len);
		else
			perror("read");
		exit(1);
	}

	if (ioctl(fd, ECCGETSTATS, &newstats)) {
		printf("\n");
		perror("ECCGETSTATS");
		close(fd);
		exit(1);
	}

	if (newstats.corrected > oldstats.corrected) {
		printf("\n %d bit(s) ECC corrected at %08x\n",
				newstats.corrected - oldstats.corrected,
				(unsigned) ofs);
		oldstats.corrected = newstats.corrected;
	}
	if (newstats.failed > oldstats.failed) {
		printf("\nECC failed at %08x\n", (unsigned) ofs);
		oldstats.failed = newstats.failed;
	}

	printf("\r%08x: checking...", (unsigned)ofs);
	fflush(stdout);

	if (memcmp(data, rbuf, meminfo.erasesize)) {
		printf("\n");
		fprintf(stderr, "compare failed. seed %d\n", seed);
		for (i=0; i<meminfo.erasesize; i++) {
			if (data[i] != rbuf[i])
				printf("Byte 0x%x is %02x should be %02x\n",
				       i, rbuf[i], data[i]);
		}
		return 1;
	}
	return 0;
}

static int erase_and_write(loff_t ofs, unsigned char *data, unsigned char *rbuf,
			   int nr_reads)
{
	struct erase_info_user er;
	ssize_t len;
	int i, read_errs = 0;

	printf("\r%08x: erasing... ", (unsigned)ofs);
	fflush(stdout);

	er.start = ofs;
	er.length = meminfo.erasesize;

	if (ioctl(fd, MEMERASE, &er)) {
		perror("MEMERASE");
		if (markbad) {
			printf("Mark block bad at %08lx\n", (long)ofs);
			ioctl(fd, MEMSETBADBLOCK, &ofs);
		}
		return 1;
	}

	printf("\r%08x: writing...", (unsigned)ofs);
	fflush(stdout);

	len = pwrite(fd, data, meminfo.erasesize, ofs);
	if (len < 0) {
		printf("\n");
		perror("write");
		if (markbad) {
			printf("Mark block bad at %08lx\n", (long)ofs);
			ioctl(fd, MEMSETBADBLOCK, &ofs);
		}
		return 1;
	}
	if (len < meminfo.erasesize) {
		printf("\n");
		fprintf(stderr, "Short write (%zd bytes)\n", len);
		exit(1);
	}

	for (i=1; i<=nr_reads; i++) {
		printf("\r%08x: reading (%d of %d)...", (unsigned)ofs, i, nr_reads);
		fflush(stdout);
		if (read_and_compare(ofs, data, rbuf))
			read_errs++;
	}
	if (read_errs) {
		fprintf(stderr, "read/check %d of %d failed. seed %d\n", read_errs, nr_reads, seed);
		return 1;
	}
	return 0;
}

static uint64_t get_mem_size(const char* device)
{
	const char* p = strrchr(device, '/');
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "/sys/class/mtd/%s/size", p);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		char buffer[32];
		ssize_t n = read(fd, buffer, sizeof(buffer));
		close(fd);
		if (n > 0) {
			return strtoull(buffer, NULL, 0);
		}
	}

	fprintf(stderr, "Can't read size from %s\n", path);
	exit(1);
}

/*
 * Main program
 */
int main(int argc, char **argv)
{
	int i;
	unsigned char *wbuf, *rbuf, *kbuf;
	int pass;
	int nr_passes = 1;
	int nr_reads = 4;
	int keep_contents = 0;
	uint64_t offset = 0;
	uint64_t length = -1;
	uint64_t mem_size = 0;
	int error = 0;

	seed = time(NULL);

	for (;;) {
		static const char short_options[] = "hkl:mo:p:r:s:V";
		static const struct option long_options[] = {
			{ "help", no_argument, 0, 'h' },
			{ "version", no_argument, 0, 'V' },
			{ "markbad", no_argument, 0, 'm' },
			{ "seed", required_argument, 0, 's' },
			{ "passes", required_argument, 0, 'p' },
			{ "offset", required_argument, 0, 'o' },
			{ "length", required_argument, 0, 'l' },
			{ "reads", required_argument, 0, 'r' },
			{ "keep", no_argument, 0, 'k' },
			{0, 0, 0, 0},
		};
		int option_index = 0;
		int c = getopt_long(argc, argv, short_options, long_options, &option_index);
		if (c == EOF)
			break;

		switch (c) {
		case 'h':
			usage(EXIT_SUCCESS);
		case 'V':
			common_print_version();
			exit(EXIT_SUCCESS);
			break;
		case '?':
			usage(EXIT_FAILURE);

		case 'm':
			markbad = 1;
			break;

		case 'k':
			keep_contents = 1;
			break;

		case 's':
			seed = atol(optarg);
			break;

		case 'p':
			nr_passes = atol(optarg);
			break;

		case 'r':
			nr_reads = atol(optarg);
			break;

		case 'o':
			offset = simple_strtoull(optarg, &error);
			break;

		case 'l':
			length = simple_strtoull(optarg, &error);
			break;

		}
	}
	if (argc - optind != 1)
		usage(EXIT_FAILURE);
	if (error)
		errmsg_die("Try --help for more information");

	fd = open(argv[optind], O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	if (ioctl(fd, MEMGETINFO, &meminfo)) {
		perror("MEMGETINFO");
		close(fd);
		exit(1);
	}

	mem_size = get_mem_size(argv[optind]);

	if (length == -1)
		length = mem_size;

	if (offset % meminfo.erasesize) {
		fprintf(stderr, "Offset %" PRIx64
			" not multiple of erase size %x\n",
			offset, meminfo.erasesize);
		exit(1);
	}
	if (length % meminfo.erasesize) {
		fprintf(stderr, "Length %" PRIx64
			" not multiple of erase size %x\n",
			length, meminfo.erasesize);
		exit(1);
	}
	if (length + offset > mem_size) {
		fprintf(stderr, "Length %" PRIx64 " + offset %" PRIx64
			" exceeds device size %" PRIx64 "\n",
			length, offset, mem_size);
		exit(1);
	}

	wbuf = malloc(meminfo.erasesize * 3);
	if (!wbuf) {
		fprintf(stderr, "Could not allocate %d bytes for buffer\n",
			meminfo.erasesize * 3);
		exit(1);
	}
	rbuf = wbuf + meminfo.erasesize;
	kbuf = rbuf + meminfo.erasesize;

	if (ioctl(fd, ECCGETSTATS, &oldstats)) {
		perror("ECCGETSTATS");
		close(fd);
		exit(1);
	}

	printf("ECC corrections: %d\n", oldstats.corrected);
	printf("ECC failures   : %d\n", oldstats.failed);
	printf("Bad blocks     : %d\n", oldstats.badblocks);
	printf("BBT blocks     : %d\n", oldstats.bbtblocks);

	srand(seed);

	for (pass = 0; pass < nr_passes; pass++) {
		loff_t test_ofs;

		for (test_ofs = offset; test_ofs < offset+length; test_ofs += meminfo.erasesize) {
			ssize_t len;

			seed = rand();
			srand(seed);

			if (ioctl(fd, MEMGETBADBLOCK, &test_ofs)) {
				printf("\rBad block at 0x%08x\n", (unsigned)test_ofs);
				continue;
			}

			for (i=0; i<meminfo.erasesize; i++)
				wbuf[i] = rand();

			if (keep_contents) {
				printf("\r%08x: reading... ", (unsigned)test_ofs);
				fflush(stdout);

				len = pread(fd, kbuf, meminfo.erasesize, test_ofs);
				if (len < meminfo.erasesize) {
					printf("\n");
					if (len)
						fprintf(stderr, "Short read (%zd bytes)\n", len);
					else
						perror("read");
					exit(1);
				}
			}
			if (erase_and_write(test_ofs, wbuf, rbuf, nr_reads))
				continue;
			if (keep_contents)
				erase_and_write(test_ofs, kbuf, rbuf, 1);
		}
		printf("\nFinished pass %d successfully\n", pass+1);
	}
	/* Return happy */
	return 0;
}

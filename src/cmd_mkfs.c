/*
 * s5fs mkfs -- create an empty 2.9BSD-family (s5fs) filesystem image.
 *
 * A thin front-end over the s5fs writer core (shared with the dump->dsk /
 * tar->dsk / tree->dsk front-ends to come).  Size comes from a known device
 * (-d, see `s5fs devices`), filesystem blocks (-b), or 512-byte sectors (-s).
 *
 * usage: s5fs mkfs [-B 512|1024] [-d device | -b blocks | -s sectors]
 *                  [-r release] [-m m] [-n n] [-t mtime] [-i ninode] image
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_core.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static void usage(void)
{
	fprintf(stderr,
	    "usage: s5fs mkfs [-B 512|1024|2048] [-a pdp11|le|be] [-d device | -b blocks | -s sectors]\n"
	    "                 [-P part | -o START:LEN] [-r release] [-m m] [-n n] [-t mtime] [-i ninode] image\n"
	    "\n"
	    "  -a arch  on-disk byte order: pdp11 (default), le (vax/x86), be (m68k)\n"
	    "  -d dev   size the image for a known disk ('s5fs devices')\n"
	    "  -P part  lay the fs into partition 'part' of a whole-disk image (needs -d)\n"
	    "  -o s:l   ..or a raw window: START:LEN in 512-byte blocks\n"
	    "  -r rel   OPTIONAL target release (v7|2.8|2.9|2.10) -- only enables a\n"
	    "           gentle driver-availability note; the image is release-agnostic\n");
	exit(2);
}

static unsigned long must_num(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*s == '\0' || *end != '\0') {
		fprintf(stderr, "s5fs mkfs: bad %s: %s\n", what, s);
		exit(2);
	}
	return v;
}

int cmd_mkfs(int argc, char **argv)
{
	s5fs_opts opts;
	S5FS fs;
	const char *path;
	const disk_dev *dev = NULL;
	bsd_rel target = REL_NONE;
	unsigned long blocks = 0, sectors = 0;
	const char *ospec = NULL;
	char part = 0;
	long long base = 0;
	uint32_t plen = 0;
	int fd, c;

	memset(&opts, 0, sizeof opts);
	opts.mtime = -1;		/* default: now */

	while ((c = getopt(argc, argv, "B:a:d:r:b:s:m:n:t:i:P:o:")) != -1) {
		switch (c) {
		case 'B': opts.bsize  = (uint32_t)must_num(optarg, "block size"); break;
		case 'a':
			opts.endian = s5_endian_parse(optarg);
			if (opts.endian == S5_NENDIAN) {
				fprintf(stderr, "s5fs mkfs: unknown byte order '%s' "
				        "(use pdp11|le|be)\n", optarg);
				return 2;
			}
			break;
		case 'b': blocks      = must_num(optarg, "block count");          break;
		case 's': sectors     = must_num(optarg, "sector count");         break;
		case 'm': opts.m      = (int32_t)must_num(optarg, "m");           break;
		case 'n': opts.n      = (int32_t)must_num(optarg, "n");           break;
		case 't': opts.mtime  = (int64_t)must_num(optarg, "mtime");       break;
		case 'i': opts.ninode = (uint32_t)must_num(optarg, "ninode");     break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg;   break;
		case 'd':
			dev = device_find(optarg);
			if (!dev) {
				fprintf(stderr, "s5fs mkfs: unknown device '%s' "
				        "(see 's5fs devices')\n", optarg);
				return 2;
			}
			break;
		case 'r':
			target = release_parse(optarg);
			if (target == REL_NONE) {
				fprintf(stderr, "s5fs mkfs: unknown release '%s' "
				        "(use v7|2.8|2.9|2.10)\n", optarg);
				return 2;
			}
			break;
		default:  usage();
		}
	}
	if (optind != argc - 1)
		usage();
	path = argv[optind];

	if (dev) {
		if (blocks || sectors) {
			fprintf(stderr, "s5fs mkfs: give -d, -b, or -s -- not more than one\n");
			return 2;
		}
		sectors = dev->blocks;		/* device table is in 512-blocks */
		device_advise(dev, target);	/* advisory only; never blocks */
	}

	if (opts.bsize == 0)
		opts.bsize = 1024;
	if (!P11_BSIZE_OK(opts.bsize)) {
		fprintf(stderr, "s5fs mkfs: block size must be 512, 1024, or 2048\n");
		return 2;
	}

	/* sectors are 512 bytes (SIMH's unit); convert to filesystem blocks */
	if (sectors) {
		unsigned long per = opts.bsize / 512;
		if (blocks) {
			fprintf(stderr, "s5fs mkfs: give -b or -s, not both\n");
			return 2;
		}
		blocks = sectors / per;
	}
	/* partition mode: lay the fs into one window of a whole-disk image */
	if (device_resolve_part(dev ? dev->name : NULL, part, ospec, &base, &plen) < 0)
		return 2;
	if (base != 0 || plen != 0) {
		unsigned long per = opts.bsize / 512;
		off_t need;
		struct stat st;
		if (plen == 0) {
			fprintf(stderr, "s5fs mkfs: partition needs a length "
			        "(use -d dev -P letter, or -o START:LEN)\n");
			return 2;
		}
		opts.base = base;
		blocks = plen / per;			/* the fs fills exactly the partition */
		fd = open(path, O_RDWR | O_CREAT, 0666);	/* do NOT truncate a whole-disk file */
		if (fd < 0) {
			fprintf(stderr, "s5fs mkfs: %s: %s\n", path, strerror(errno));
			return 1;
		}
		need = base + (off_t)plen * 512;		/* end of this partition */
		if (dev && (off_t)dev->blocks * 512 > need)
			need = (off_t)dev->blocks * 512;	/* size to the whole drive */
		if (fstat(fd, &st) == 0 && st.st_size < need && ftruncate(fd, need) < 0) {
			fprintf(stderr, "s5fs mkfs: %s: %s\n", path, strerror(errno));
			close(fd);
			return 1;
		}
	} else {
		if (blocks == 0) {
			fprintf(stderr, "s5fs mkfs: need a size (-d, -b, or -s)\n");
			usage();
		}
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			fprintf(stderr, "s5fs mkfs: %s: %s\n", path, strerror(errno));
			return 1;
		}
	}

	if (s5fs_mkfs(&fs, fd, (uint32_t)blocks, &opts) < 0) {
		fprintf(stderr, "s5fs mkfs: %s\n", fs.err);
		close(fd);
		return 1;
	}
	if (close(fd) < 0) {
		fprintf(stderr, "s5fs mkfs: %s: %s\n", path, strerror(errno));
		return 1;
	}

	printf("%s: %s, %lu %u-byte blocks, i-list %u blocks (%u inodes), "
	       "%d free blocks, %d free inodes\n",
	       path, fs.bo->name, blocks, fs.bsize, fs.s_isize - 2,
	       (fs.s_isize - 2) * fs.inopb, fs.s_tfree, fs.s_tinode);
	return 0;
}

/*
 * s5fs vhd -- wrap/unwrap a raw image as a fixed-size VHD (Virtual Hard Disk).
 *
 * A fixed VHD is just a raw sector image with a 512-byte footer appended at the
 * end (cookie "conectix", CHS geometry, size, type=fixed, checksum).  SIMH
 * (and QEMU/Hyper-V/VirtualBox) auto-detect it from that footer, so a wrapped
 * image can be `attach`ed as a .vhd container.  Because the footer sits *after*
 * the filesystem, all the other s5fs commands read and even edit a wrapped
 * image transparently -- the footer only needs adding once and stripping if a
 * tool wants a bare image back.
 *
 *   s5fs vhd wrap   SRC [DST]   raw  -> VHD  (append footer; in place if no DST)
 *   s5fs vhd unwrap SRC [DST]   VHD  -> raw  (strip footer;  in place if no DST)
 *   s5fs vhd info   FILE        print the footer fields
 *
 * VHD footer fields are big-endian regardless of the image's own byte order.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>

#define VHD_COOKIE "conectix"
#define VHD_EPOCH 946684800UL /* 2000-01-01 00:00:00 UTC, in Unix time */

static void
be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void
be64(uint8_t *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
rd64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

/* VHD spec CHS calculation (Appendix): map total sectors to C/H/S */
static void
vhd_chs(uint64_t total, uint16_t *C, uint8_t *H, uint8_t *S)
{
	uint64_t cth;
	uint32_t spt, heads;
	if (total > 65535ULL * 16 * 255)
		total = 65535ULL * 16 * 255;
	if (total >= 65535ULL * 16 * 63) {
		spt = 255;
		heads = 16;
		cth = total / spt;
	}
	else {
		spt = 17;
		cth = total / spt;
		heads = (uint32_t)((cth + 1023) / 1024);
		if (heads < 4)
			heads = 4;
		if (cth >= (uint64_t)heads * 1024 || heads > 16) {
			spt = 31;
			heads = 16;
			cth = total / spt;
		}
		if (cth >= (uint64_t)heads * 1024) {
			spt = 63;
			heads = 16;
			cth = total / spt;
		}
	}
	*C = (uint16_t)(cth / heads);
	*H = (uint8_t)heads;
	*S = (uint8_t)spt;
}

static void
vhd_footer(uint8_t f[512], uint64_t size)
{
	uint16_t C;
	uint8_t H, S;
	uint32_t sum = 0;
	int i;
	memset(f, 0, 512);
	memcpy(f + 0, VHD_COOKIE, 8);
	be32(f + 8, 0x00000002);	     /* features: reserved bit */
	be32(f + 12, 0x00010000);	     /* file format version 1.0 */
	be64(f + 16, 0xFFFFFFFFFFFFFFFFULL); /* data offset: none (fixed) */
	be32(f + 24, (uint32_t)(time(NULL) - VHD_EPOCH));
	memcpy(f + 28, "s5fs", 4); /* creator application */
	be32(f + 32, 0x00010000);  /* creator version */
	memcpy(f + 36, "Wi2k", 4); /* creator host OS (cosmetic) */
	be64(f + 40, size);	   /* original size */
	be64(f + 48, size);	   /* current size */
	vhd_chs(size / 512, &C, &H, &S);
	be16(f + 56, C);
	f[58] = H;
	f[59] = S;	 /* disk geometry (C/H/S) */
	be32(f + 60, 2); /* disk type: 2 = fixed */
	/* f+64 checksum (computed below); f+68 unique id left zero (SIMH ignores) */
	for (i = 0; i < 512; i++)
		sum += f[i];
	be32(f + 64, ~sum); /* ones-complement checksum */
}

/* read the trailing 512 bytes; return 1 and fill foot[] if it's a VHD footer */
static int
read_footer(int fd, off_t size, uint8_t foot[512])
{
	if (size < 512)
		return 0;
	if (lseek(fd, size - 512, SEEK_SET) < 0 || read(fd, foot, 512) != 512)
		return 0;
	return memcmp(foot, VHD_COOKIE, 8) == 0;
}

static int
copy_prefix(const char *src, const char *dst, off_t nbytes)
{
	int in = open(src, O_RDONLY), out;
	uint8_t buf[65536];
	off_t done = 0;
	if (in < 0) {
		fprintf(stderr, "s5fs vhd: %s: %s\n", src, strerror(errno));
		return -1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0) {
		fprintf(stderr, "s5fs vhd: %s: %s\n", dst, strerror(errno));
		close(in);
		return -1;
	}
	while (done < nbytes) {
		long want = nbytes - done;
		ssize_t n;
		if (want > (long)sizeof buf)
			want = sizeof buf;
		n = read(in, buf, want);
		if (n <= 0)
			break;
		if (write(out, buf, n) != n) {
			fprintf(stderr, "s5fs vhd: %s: write: %s\n", dst, strerror(errno));
			close(in);
			close(out);
			return -1;
		}
		done += n;
	}
	close(in);
	close(out);
	return 0;
}

static int
do_wrap(const char *src, const char *dst)
{
	struct stat st;
	uint8_t foot[512];
	int fd;
	if (stat(src, &st) < 0) {
		fprintf(stderr, "s5fs vhd: %s: %s\n", src, strerror(errno));
		return 1;
	}
	fd = open(src, O_RDONLY);
	if (fd >= 0) {
		if (read_footer(fd, st.st_size, foot)) {
			close(fd);
			fprintf(stderr, "s5fs vhd: %s already has a VHD footer\n", src);
			return 1;
		}
		close(fd);
	}
	if (dst && copy_prefix(src, dst, st.st_size) < 0)
		return 1;
	{
		const char *target = dst ? dst : src;
		int wfd = open(target, O_WRONLY);
		if (wfd < 0) {
			fprintf(stderr, "s5fs vhd: %s: %s\n", target, strerror(errno));
			return 1;
		}
		lseek(wfd, 0, SEEK_END); /* append after the data */
		vhd_footer(foot, (uint64_t)st.st_size);
		if (write(wfd, foot, 512) != 512) {
			fprintf(stderr, "s5fs vhd: %s: write: %s\n", target, strerror(errno));
			close(wfd);
			return 1;
		}
		close(wfd);
		printf("%s: fixed VHD (%lld data bytes + 512 footer)\n", target, (long long)st.st_size);
	}
	return 0;
}

static int
do_unwrap(const char *src, const char *dst)
{
	struct stat st;
	uint8_t foot[512];
	int fd = open(src, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "s5fs vhd: %s: %s\n", src, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (!read_footer(fd, st.st_size, foot)) {
		fprintf(stderr, "s5fs vhd: %s: not a VHD (no 'conectix' footer)\n", src);
		close(fd);
		return 1;
	}
	close(fd);
	if (dst) {
		if (copy_prefix(src, dst, st.st_size - 512) < 0)
			return 1;
		printf("%s: raw image (%lld bytes) -> %s\n", src, (long long)(st.st_size - 512), dst);
	}
	else {
		if (truncate(src, st.st_size - 512) < 0) {
			fprintf(stderr, "s5fs vhd: %s: %s\n", src, strerror(errno));
			return 1;
		}
		printf("%s: stripped VHD footer (%lld bytes)\n", src, (long long)(st.st_size - 512));
	}
	return 0;
}

static int
do_info(const char *file)
{
	struct stat st;
	uint8_t f[512];
	int fd = open(file, O_RDONLY);
	uint32_t ts, type;
	time_t t;
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "s5fs vhd: %s: %s\n", file, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	if (!read_footer(fd, st.st_size, f)) {
		fprintf(stderr, "s5fs vhd: %s: not a VHD (no 'conectix' footer)\n", file);
		close(fd);
		return 1;
	}
	close(fd);
	type = rd32(f + 60);
	ts = rd32(f + 24);
	t = (time_t)(ts + VHD_EPOCH);
	printf("%s: VHD footer\n", file);
	printf("  type:      %u (%s)\n", type, type == 2 ? "fixed" : type == 3 ? "dynamic"
							     : type == 4       ? "differencing"
									       : "?");
	printf("  size:      %llu bytes (%llu sectors)\n",
	       (unsigned long long)rd64(f + 48), (unsigned long long)(rd64(f + 48) / 512));
	printf("  geometry:  C/H/S = %u/%u/%u\n", (unsigned)(f[56] << 8 | f[57]), f[58], f[59]);
	printf("  creator:   %.4s v%u.%u on %.4s\n", f + 28,
	       (unsigned)(f[32] << 8 | f[33]), (unsigned)(f[34] << 8 | f[35]), f + 36);
	printf("  created:   %s", ctime(&t));
	if (type != 2)
		printf("  note: only FIXED VHD is fully supported; dynamic/differencing need the BAT.\n");
	return 0;
}

int
cmd_vhd(int argc, char **argv)
{
	const char *sub = argc >= 2 ? argv[1] : "";
	if (!strcmp(sub, "wrap") && argc >= 3)
		return do_wrap(argv[2], argc >= 4 ? argv[3] : NULL);
	if (!strcmp(sub, "unwrap") && argc >= 3)
		return do_unwrap(argv[2], argc >= 4 ? argv[3] : NULL);
	if (!strcmp(sub, "info") && argc == 3)
		return do_info(argv[2]);
	fprintf(stderr,
		"usage: s5fs vhd wrap   SRC [DST]   raw -> fixed VHD (in place if no DST)\n"
		"       s5fs vhd unwrap SRC [DST]   fixed VHD -> raw (in place if no DST)\n"
		"       s5fs vhd info   FILE        show the VHD footer\n");
	return 2;
}

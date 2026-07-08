/*
 * s5fs dump -- write a 2.9BSD dump(8) tape from an s5fs image.
 *
 * The reverse of `s5fs restore`.  Reads the image with the fsread reader and
 * emits the old dump format (dumprestor.h): a TS_TAPE header, the TS_CLRI and
 * TS_BITS inode maps, one TS_INODE record + data per allocated inode (with
 * TS_ADDR continuations for indirect blocks, exactly as dumptraverse.c's
 * icat/indir/dmpspc do), then TS_END.  The written tape is read back by
 * `s5fs restore` (and by native restor(8)).
 *
 * The dump inherits the image's byte order (a PDP-11 image -> a PDP-11 dump),
 * so `s5fs restore` round-trips it and the metadata/data copy through verbatim.
 *
 * usage: s5fs dump [-B 512|1024] [-A pdp11|le|be] image dumpfile
 */

#define _POSIX_C_SOURCE 200809L

#include "fsread.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#define DMAGIC     60011
#define DCHECKSUM  84446
#define TS_TAPE    1
#define TS_INODE   2
#define TS_BITS    3
#define TS_ADDR    4
#define TS_END     5
#define TS_CLRI    6
#define NTREC      10

/* struct spcl offsets */
#define C_TYPE     0
#define C_DATE     2
#define C_DDATE    6
#define C_VOLUME   10
#define C_INUM     16
#define C_MAGIC    18
#define C_CHECKSUM 20
#define C_DINODE   22
#define C_COUNT    86
#define C_ADDR     88

static FSR      R;
static int      OUT;
static uint8_t  spcl[P11_MAXBSIZE];	/* current record            */
static uint8_t  dino[P11_DINODESZ];	/* current inode's dinode    */
static long     ncount;			/* records written           */

/* SIMH .tap framing: group NTREC blocks into one tape record wrapped by a
 * 4-byte little-endian length before and after (so it reads either direction);
 * a lone 4-byte 0 is a tape mark.  This is what dump(8) writes to a real tape
 * (one write() of NTREC*BSIZE per tape record) and what restor(8) reads with
 * `read(mt, tbf, NTREC*BSIZE)` -- so the tape attaches to a SIMH TM/TS drive. */
static int      TAP;			/* -T: emit SIMH .tap instead of flat */
static uint8_t  tapbuf[NTREC * P11_MAXBSIZE];
static uint32_t tapn;			/* blocks buffered in tapbuf          */

static void wput(const void *b, size_t n)
{
	if (write(OUT, b, n) != (ssize_t)n) { perror("s5fs dump: write"); exit(1); }
}

static void tap_flush(void)		/* emit the buffered blocks as one tape record */
{
	uint8_t m[4];
	uint32_t len = tapn * R.bsize;
	if (tapn == 0) return;
	m[0] = len; m[1] = len >> 8; m[2] = len >> 16; m[3] = len >> 24;
	wput(m, 4); wput(tapbuf, len); wput(m, 4);
	tapn = 0;
}

static void taprec(const uint8_t *b)
{
	if (TAP) {
		memcpy(tapbuf + tapn * R.bsize, b, R.bsize);
		if (++tapn == NTREC) tap_flush();
	} else {
		wput(b, R.bsize);
	}
	ncount++;
}

/* finalize the header in spcl[] (inumber, magic, checksum) and write it */
static void spclrec(uint32_t ino)
{
	uint32_t s = 0, i, nw = R.bsize / 2;
	R.bo->put16(spcl + C_INUM, (uint16_t)ino);
	R.bo->put16(spcl + C_MAGIC, DMAGIC);
	R.bo->put16(spcl + C_CHECKSUM, 0);
	for (i = 0; i < nw; i++)
		s += R.bo->get16(spcl + 2 * i);
	R.bo->put16(spcl + C_CHECKSUM, (uint16_t)((DCHECKSUM - s) & 0xffff));
	taprec(spcl);
}

/* an indirect subtree: level 0 = single (leaf) indirect, 1 = double, 2 = triple */
static void indir(uint32_t blk, int level, uint32_t ino)
{
	uint8_t ib[P11_MAXBSIZE], db[P11_MAXBSIZE];
	uint32_t j;
	fsr_bread(&R, blk, ib);
	if (level == 0) {
		R.bo->put16(spcl + C_TYPE, TS_ADDR);
		R.bo->put16(spcl + C_COUNT, (uint16_t)R.nindir);
		for (j = 0; j < R.nindir; j++)
			spcl[C_ADDR + j] = R.bo->get32(ib + 4 * j) ? 1 : 0;
		spclrec(ino);
		for (j = 0; j < R.nindir; j++) {
			uint32_t d = R.bo->get32(ib + 4 * j);
			if (d) { fsr_bread(&R, d, db); taprec(db); }
		}
	} else {
		for (j = 0; j < R.nindir; j++) {
			uint32_t d = R.bo->get32(ib + 4 * j);
			if (d) indir(d, level - 1, ino);
		}
	}
}

/* dump one allocated inode: TS_INODE header (+ data), TS_ADDR for indirects */
static void dump_inode(uint32_t ino, int *ndir, int *nreg, int *nspec)
{
	uint32_t iblk = (ino + 2 * R.inopb - 1) / R.inopb;
	uint32_t ioff = ((ino + 2 * R.inopb - 1) % R.inopb) * P11_DINODESZ;
	uint8_t buf[P11_MAXBSIZE];
	uint16_t mode;
	uint32_t i;
	int f;

	fsr_bread(&R, iblk, buf);
	memcpy(dino, buf + ioff, P11_DINODESZ);
	mode = R.bo->get16(dino + P11_DI_MODE);
	if (mode == 0)
		return;

	memset(spcl + C_ADDR, 0, R.bsize - C_ADDR);
	memcpy(spcl + C_DINODE, dino, P11_DINODESZ);
	R.bo->put16(spcl + C_TYPE, TS_INODE);
	R.bo->put16(spcl + C_COUNT, 0);

	f = mode & P11_IFMT;
	if (f != P11_IFDIR && f != P11_IFREG) {		/* special: header only */
		spclrec(ino);
		(*nspec)++;
		return;
	}
	if (f == P11_IFDIR) (*ndir)++; else (*nreg)++;

	/* TS_INODE header covering the direct slots, then the direct data */
	R.bo->put16(spcl + C_COUNT, (uint16_t)R.laddr);
	for (i = 0; i < R.laddr; i++)
		spcl[C_ADDR + i] = R.bo->get24(dino + P11_DI_ADDR + 3 * i) ? 1 : 0;
	spclrec(ino);
	for (i = 0; i < R.laddr; i++) {
		uint32_t d = R.bo->get24(dino + P11_DI_ADDR + 3 * i);
		if (d) { fsr_bread(&R, d, buf); taprec(buf); }
	}
	for (i = R.laddr; i < R.naddr; i++) {
		uint32_t d = R.bo->get24(dino + P11_DI_ADDR + 3 * i);
		if (d) indir(d, (int)(i - R.laddr), ino);
	}
}

/* write a TS_BITS / TS_CLRI record + its map data blocks */
static void write_map(const uint8_t *map, uint32_t maxino, int typ)
{
	uint32_t nshort = maxino / 16;
	uint32_t c_count = (nshort * 2 + R.bsize) / R.bsize, i;
	memset(spcl, 0, R.bsize);
	R.bo->put16(spcl + C_TYPE, (uint16_t)typ);
	R.bo->put16(spcl + C_COUNT, (uint16_t)c_count);
	spclrec(0);
	for (i = 0; i < c_count; i++)
		taprec(map + i * R.bsize);
}

int cmd_dump(int argc, char **argv)
{
	const char *image, *dumpfile, *dev = NULL, *ospec = NULL;
	uint32_t bsize = 0, nino, ino, maxino = 0, c_count, i, plen = 0;
	int forced = -1, c, ndir = 0, nreg = 0, nspec = 0;
	char part = 0; long long base = 0;
	uint8_t *map;

	while ((c = getopt(argc, argv, "B:A:d:P:o:T")) != -1) {
		switch (c) {
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) { fprintf(stderr, "s5fs dump: bad -A\n"); return 2; }
			forced = (int)e; break;
		}
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		case 'T': TAP = 1; break;
		default: fprintf(stderr, "usage: s5fs dump [-B 512|1024] [-A pdp11|le|be] [-d dev -P part | -o blk] [-T] image dumpfile\n"); return 2;
		}
	}
	if (optind != argc - 2) { fprintf(stderr, "usage: s5fs dump [-B 512|1024] [-A pdp11|le|be] [-d dev -P part | -o blk] image dumpfile\n"); return 2; }
	image = argv[optind]; dumpfile = argv[optind + 1];
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;

	if (fsr_open(&R, image, bsize, forced, base) < 0) {
		fprintf(stderr, "s5fs dump: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", image);
		return 1;
	}
	OUT = open(dumpfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (OUT < 0) { fprintf(stderr, "s5fs dump: %s: %s\n", dumpfile, strerror(errno)); fsr_close(&R); return 1; }

	/* pass 1: which inodes are allocated (the dump/clri map), and the max */
	nino = (R.isize - 2) * R.inopb;
	c_count = (nino / 16 * 2 + R.bsize) / R.bsize;
	map = calloc((size_t)c_count * R.bsize, 1);
	if (!map) { fprintf(stderr, "s5fs dump: out of memory\n"); return 1; }
	for (ino = P11_ROOTINO; ino <= nino; ino++) {
		fsr_inode in;
		if (fsr_iget(&R, ino, &in) < 0) break;
		if (in.mode == 0) continue;
		map[ino / 8] |= (uint8_t)(1 << (ino % 8));	/* LSB-first bit array */
		maxino = ino;
	}
	if (maxino == 0) { fprintf(stderr, "s5fs dump: no allocated inodes\n"); return 1; }

	/* TS_TAPE, then the maps, then the inodes, then TS_END */
	memset(spcl, 0, R.bsize);
	R.bo->put16(spcl + C_TYPE, TS_TAPE);
	R.bo->put32(spcl + C_DATE, (uint32_t)time(NULL));
	R.bo->put32(spcl + C_DDATE, 0);
	R.bo->put16(spcl + C_VOLUME, 1);
	spclrec(maxino);
	write_map(map, maxino, TS_CLRI);
	write_map(map, maxino, TS_BITS);
	for (ino = P11_ROOTINO; ino <= maxino; ino++)
		dump_inode(ino, &ndir, &nreg, &nspec);
	memset(spcl, 0, R.bsize);
	R.bo->put16(spcl + C_TYPE, TS_END);
	for (i = 0; i < NTREC; i++)
		spclrec(maxino);

	if (TAP) {			/* flush partial record + two tape marks (EOF, EOT) */
		uint8_t mk[4] = { 0, 0, 0, 0 };
		tap_flush();
		wput(mk, 4);
		wput(mk, 4);
	}

	free(map);
	close(OUT);
	fsr_close(&R);
	printf("%s -> %s: %d dirs, %d files, %d special (max inode #%u), %ld records%s\n",
	       image, dumpfile, ndir, nreg, nspec, maxino, ncount,
	       TAP ? " [SIMH .tap]" : "");
	return 0;
}

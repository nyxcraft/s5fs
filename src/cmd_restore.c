/*
 * s5fs restore -- restore a 2.9BSD dump(8) tape into an s5fs image.
 *
 * (Named for what it does: it *reads* a dump and writes a filesystem, the job
 * of restor(8)/restore(8).  A future `s5fs dump` would go the other way,
 * writing a dump tape from an image.)
 *
 * Reads the old dump format (dumprestor.h: MAGIC 60011, struct spcl records of
 * one BSIZE block).  This is a *faithful* restore: it keeps the original inode
 * numbers and metadata (mode/uid/gid/nlink/size + the device number for
 * specials) and copies directory data verbatim, so the namespace, link counts,
 * and layout are reproduced exactly -- which is also why the result passes
 * `s5fs fsck` (its recomputed link counts match).
 *
 * Tape layout: after each header (TS_TAPE/TS_BITS/TS_CLRI/TS_INODE) come the
 * data blocks it maps (c_count present/hole flags in c_addr, then one record
 * per present block, with TS_ADDR continuation headers for big files).  Walking
 * that map advances to the next header.
 *
 * v1 reads a PDP-11 dump with BSIZE-byte records (our rootdump; typical 2.9
 * UCB dumps).  -B sets that record size AND the target block size (a faithful
 * restore keeps the block size); target byte order is PDP-11 to match.
 *
 * usage: s5fs restore [-B 512|1024] [-d device | -b blocks | -s sectors] dumpfile image
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_core.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* dumprestor.h */
#define DMAGIC 60011
#define TS_TAPE 1
#define TS_INODE 2
#define TS_BITS 3
#define TS_ADDR 4
#define TS_END 5
#define TS_CLRI 6

/* struct spcl byte offsets (PDP-11: 16-bit int, 32-bit middle-endian long) */
#define C_TYPE 0
#define C_INUM 16
#define C_MAGIC 18
#define C_DINODE 22 /* struct dinode (64 bytes)          */
#define C_COUNT 86
#define C_ADDR 88 /* per-block present(1)/hole(0) flags */
#define D_MODE (C_DINODE + P11_DI_MODE)
#define D_NLINK (C_DINODE + P11_DI_NLINK)
#define D_UID (C_DINODE + P11_DI_UID)
#define D_GID (C_DINODE + P11_DI_GID)
#define D_SIZE (C_DINODE + P11_DI_SIZE)
#define D_ATIME (C_DINODE + P11_DI_ATIME)
#define D_MTIME (C_DINODE + P11_DI_MTIME)
#define D_CTIME (C_DINODE + P11_DI_CTIME)
#define D_ADDR (C_DINODE + P11_DI_ADDR)

typedef struct {
	int fd;
	long pos;
	uint32_t rec;
	const s5_codec *bo;
} DR;

static void
usage(void)
{
	fprintf(stderr,
		"usage: s5fs restore [-B 512|1024] [-d device | -b blocks | -s sectors]\n"
		"                    dumpfile image\n");
	exit(2);
}

static unsigned long
must_num(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0') {
		fprintf(stderr, "s5fs restore: bad %s: %s\n", what, s);
		exit(2);
	}
	return v;
}

static int
rec_read(DR *dr, uint8_t *buf)
{
	ssize_t r;
	if (lseek(dr->fd, dr->pos, SEEK_SET) < 0)
		return 0;
	r = read(dr->fd, buf, dr->rec);
	if (r != (ssize_t)dr->rec)
		return 0;
	dr->pos += dr->rec;
	return 1;
}

static int
gethead(DR *dr, uint8_t *hdr)
{
	if (!rec_read(dr, hdr))
		return 0;
	return (int)dr->bo->get16(hdr + C_MAGIC) == DMAGIC;
}

static int
htype(DR *dr, const uint8_t *hdr)
{
	return (int)dr->bo->get16(hdr + C_TYPE);
}

/*
 * TS_TAPE/TS_BITS/TS_CLRI are each followed by c_count map blocks that are
 * present UNCONDITIONALLY (the c_addr flags are not used for these, unlike a
 * file), so skip that many records, then read the next header.  1 ok, 0 EOF.
 */
static int
skip_map(DR *dr, uint8_t *hdr)
{
	uint8_t tmp[P11_MAXBSIZE];
	uint16_t n = dr->bo->get16(hdr + C_COUNT);
	while (n--)
		if (!rec_read(dr, tmp))
			return 0;
	return gethead(dr, hdr);
}

/*
 * Walk the block map that starts in `hdr` (its c_addr flags, plus any TS_ADDR
 * continuations), consuming one data record per present block.  If fs/da are
 * given, allocate a target block per present block (da[b]=block, 0=hole) up to
 * `nblk`; pass da=NULL / nblk=ALLBLK to just skip.  On return, `hdr` holds the
 * next non-TS_ADDR header.  Returns 1 if a next header is ready, 0 at EOF.
 */
static int
read_blocks(S5FS *fs, DR *dr, uint8_t *hdr, int32_t *da, uint32_t nblk)
{
	uint8_t ab[P11_MAXBSIZE], rec[P11_MAXBSIZE];
	uint32_t b = 0;

	memcpy(ab, hdr, dr->rec);
	for (;;) {
		uint16_t count = dr->bo->get16(ab + C_COUNT), i;
		for (i = 0; i < count && b < nblk; i++, b++) {
			if (ab[C_ADDR + i]) {
				if (!rec_read(dr, rec))
					return 0;
				if (fs && da) {
					da[b] = s5fs_alloc(fs);
					s5fs_wtblk(fs, (uint32_t)da[b], rec);
				}
			}
			else if (da) {
				da[b] = 0; /* hole */
			}
		}
		if (b >= nblk) { /* inode satisfied: skip trailing TS_ADDR */
			do {
				if (!gethead(dr, hdr))
					return 0;
			}
			while (htype(dr, hdr) == TS_ADDR);
			return 1;
		}
		if (!gethead(dr, hdr))
			return 0;
		if (htype(dr, hdr) != TS_ADDR)
			return 1; /* next real header */
		memcpy(ab, hdr, dr->rec);
	}
}

int
cmd_restore(int argc, char **argv)
{
	s5fs_opts opts;
	S5FS fs;
	DR dr;
	const char *dumpfile, *image, *ospec = NULL;
	const disk_dev *dev = NULL;
	uint8_t hdr[P11_MAXBSIZE];
	unsigned long blocks = 0, sectors = 0, per;
	uint32_t maxino = 0, nrestored = 0, plen = 0;
	char part = 0;
	long long base = 0;
	int c, fd;

	memset(&opts, 0, sizeof opts);
	opts.mtime = -1;
	opts.endian = S5_PDP11;

	while ((c = getopt(argc, argv, "B:d:b:s:P:o:")) != -1) {
		switch (c) {
		case 'B':
			opts.bsize = (uint32_t)must_num(optarg, "block size");
			break;
		case 'b':
			blocks = must_num(optarg, "block count");
			break;
		case 's':
			sectors = must_num(optarg, "sector count");
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		case 'd':
			dev = device_find(optarg);
			if (!dev) {
				fprintf(stderr, "s5fs restore: unknown device '%s'\n", optarg);
				return 2;
			}
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 2)
		usage();
	dumpfile = argv[optind];
	image = argv[optind + 1];

	if (opts.bsize == 0)
		opts.bsize = 1024;
	if (!P11_BSIZE_OK(opts.bsize)) {
		fprintf(stderr, "s5fs restore: block size must be 512, 1024, or 2048\n");
		return 2;
	}
	per = opts.bsize / 512;
	if (dev) {
		if (blocks || sectors) {
			fprintf(stderr, "s5fs restore: give one of -d/-b/-s\n");
			return 2;
		}
		sectors = dev->blocks;
	}
	if (sectors) {
		if (blocks) {
			fprintf(stderr, "s5fs restore: give -b or -s, not both\n");
			return 2;
		}
		blocks = sectors / per;
	}
	if (device_resolve_part(dev ? dev->name : NULL, part, ospec, &base, &plen) < 0)
		return 2;
	if (base != 0 || plen != 0) { /* restore into a partition window */
		if (plen == 0) {
			fprintf(stderr, "s5fs restore: partition needs a length (use -d -P, or -o START:LEN)\n");
			return 2;
		}
		opts.base = base;
		blocks = plen / per; /* the restored fs fills the partition */
	}
	if (blocks == 0) {
		fprintf(stderr, "s5fs restore: need a target size (-d, -b, or -s)\n");
		usage();
	}

	dr.fd = open(dumpfile, O_RDONLY);
	if (dr.fd < 0) {
		fprintf(stderr, "s5fs restore: %s: %s\n", dumpfile, strerror(errno));
		return 1;
	}
	dr.rec = opts.bsize;
	dr.bo = s5_codec_for(S5_PDP11);

	/* pass 1: highest inode number (to size the i-list) */
	dr.pos = 0;
	if (!gethead(&dr, hdr)) {
		fprintf(stderr, "s5fs restore: %s: not a dump (bad magic)\n", dumpfile);
		close(dr.fd);
		return 1;
	}
	for (;;) {
		int t = htype(&dr, hdr);
		if (t == TS_END)
			break;
		if (t == TS_INODE) {
			uint32_t inum = dr.bo->get16(hdr + C_INUM);
			long size = (int32_t)dr.bo->get32(hdr + D_SIZE);
			uint32_t nblk = (uint32_t)((size + opts.bsize - 1) / opts.bsize);
			if (inum > maxino)
				maxino = inum;
			if (!read_blocks(NULL, &dr, hdr, NULL, nblk))
				break;
		}
		else if (!skip_map(&dr, hdr)) {
			break;
		}
	}
	if (maxino == 0) {
		fprintf(stderr, "s5fs restore: no inodes found\n");
		close(dr.fd);
		return 1;
	}
	opts.ninode = maxino + 8;

	/* pass 2: create the fs and restore each inode at its original number.
	 * In partition mode, do NOT truncate the whole-disk file -- open it, grow
	 * it enough to hold the partition (and the whole drive, if -d), keep the
	 * rest intact. */
	if (opts.base != 0 || plen != 0) {
		off_t need = opts.base + (off_t)plen * 512;
		struct stat st;
		if (dev && (off_t)dev->blocks * 512 > need)
			need = (off_t)dev->blocks * 512;
		fd = open(image, O_RDWR | O_CREAT, 0666);
		if (fd < 0) {
			fprintf(stderr, "s5fs restore: %s: %s\n", image, strerror(errno));
			close(dr.fd);
			return 1;
		}
		if (fstat(fd, &st) == 0 && st.st_size < need && ftruncate(fd, need) < 0) {
			fprintf(stderr, "s5fs restore: %s: %s\n", image, strerror(errno));
			close(fd);
			close(dr.fd);
			return 1;
		}
	}
	else {
		fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			fprintf(stderr, "s5fs restore: %s: %s\n", image, strerror(errno));
			close(dr.fd);
			return 1;
		}
	}
	if (s5fs_begin(&fs, fd, (uint32_t)blocks, &opts) < 0) {
		fprintf(stderr, "s5fs restore: %s\n", fs.err);
		close(fd);
		close(dr.fd);
		return 1;
	}
	fs.s_tfree = 0;
	s5fs_freelist(&fs); /* inode 1 (bad-block holder) + free list */

	dr.pos = 0;
	gethead(&dr, hdr);
	for (;;) {
		int t = htype(&dr, hdr);
		s5fs_inode in;
		uint32_t inum, nblk, dev0;
		uint16_t mode;
		long size;
		int32_t *da;

		if (t == TS_END || fs.error)
			break;
		if (t != TS_INODE) {
			if (!skip_map(&dr, hdr))
				break;
			continue;
		}
		inum = dr.bo->get16(hdr + C_INUM);
		mode = dr.bo->get16(hdr + D_MODE);
		size = (int32_t)dr.bo->get32(hdr + D_SIZE);
		dev0 = dr.bo->get24(hdr + D_ADDR);
		nblk = (uint32_t)((size + opts.bsize - 1) / opts.bsize);
		da = nblk ? malloc(nblk * sizeof *da) : NULL;

		memset(&in, 0, sizeof in);
		in.number = (uint16_t)inum;
		in.mode = mode;
		in.nlink = (int16_t)dr.bo->get16(hdr + D_NLINK);
		in.uid = (int16_t)dr.bo->get16(hdr + D_UID);
		in.gid = (int16_t)dr.bo->get16(hdr + D_GID);
		in.atime = (int32_t)dr.bo->get32(hdr + D_ATIME);
		in.mtime = (int32_t)dr.bo->get32(hdr + D_MTIME);
		in.ctime = (int32_t)dr.bo->get32(hdr + D_CTIME);

		{
			int ok = read_blocks(&fs, &dr, hdr, da, nblk); /* fills da, advances hdr */
			switch (mode & P11_IFMT) {
			case P11_IFCHR:
			case P11_IFBLK:
				in.addr[0] = (int32_t)dev0; /* special: preserve device number */
				break;
			default:
				s5fs_setblocks(&fs, &in, da, nblk);
				in.size = (int32_t)size;
				break;
			}
			s5fs_writeinode(&fs, &in);
			free(da);
			nrestored++;
			if (!ok)
				break; /* EOF / truncated dump */
		}
	}
	s5fs_finish(&fs);

	if (close(fd) < 0 || fs.error) {
		fprintf(stderr, "s5fs restore: %s\n", fs.error ? fs.err : strerror(errno));
		close(dr.fd);
		return 1;
	}
	close(dr.fd);
	printf("%s: %s, %lu %u-byte blocks; restored %u inodes (max #%u); %d free blocks left\n",
	       image, fs.bo->name, blocks, fs.bsize, nrestored, maxino, fs.s_tfree);
	return 0;
}

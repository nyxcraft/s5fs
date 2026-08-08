/*
 * s5fs fsck -- read-side consistency checker for an s5fs disk image.
 *
 * The independent counterpart to the s5fs writer: it decodes the superblock,
 * walks every allocated inode's block tree (direct + single/double/triple
 * indirect), traverses the chained free list, and confirms the whole volume
 * partitions exactly once into reserved / inode-referenced / free -- the check
 * icheck(8) performs.  Because it shares no logic with the writer (only the
 * structural constants in pdp11fs.h and the byte-order codecs in s5endian.h),
 * it is a genuine cross-check of `s5fs mkfs` output.
 *
 * It reads any s5fs image the writer can make: either block size, and any of
 * the three byte orders -- PDP-11, VAX/LE, or big-endian -- auto-detected from
 * the superblock (override with -A).  Block mapping and directory walking live
 * here too (they are the reader the FUSE mount will reuse); -l lists the tree.
 *
 * usage: s5fs fsck [-B 512|1024|2048] [-A pdp11|le|be] [-l] image
 */

#define _POSIX_C_SOURCE 200809L

#include "pdp11fs.h"
#include "s5endian.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

typedef struct {
	int fd;
	int64_t base;	    /* byte offset of the fs in the file (partition) */
	const s5_codec *bo; /* on-disk byte order */
	uint32_t bsize, inopb, naddr, laddr, nindir, ndirect;
	uint32_t isize, fsize;
	int32_t nfree, tfree, tinode;
	uint16_t ninode;
	uint8_t *use; /* per-block state: 0x7f refs (saturating) | 0x80 free */
	int errors;
} CK;

static void
die(const char *m)
{
	fprintf(stderr, "s5fs fsck: %s\n", m);
	exit(1);
}

static void
rdblk(CK *c, uint32_t bno, uint8_t *buf)
{
	off_t off = (off_t)(c->base + (int64_t)bno * c->bsize);
	if (lseek(c->fd, off, SEEK_SET) < 0 ||
	    read(c->fd, buf, c->bsize) != (ssize_t)c->bsize) {
		fprintf(stderr, "s5fs fsck: read error at block %u\n", bno);
		exit(1);
	}
}

static void
wtblk(CK *c, uint32_t bno, const uint8_t *buf)
{
	off_t off = (off_t)(c->base + (int64_t)bno * c->bsize);
	if (lseek(c->fd, off, SEEK_SET) < 0 ||
	    write(c->fd, buf, c->bsize) != (ssize_t)c->bsize) {
		fprintf(stderr, "s5fs fsck: write error at block %u\n", bno);
		exit(1);
	}
}

/* free-list rebuild (salvage): push a block onto the in-core cache, spilling a
 * full cache to disk as a chain block -- mirrors the writer's bfree. */
static void
rep_bfree(CK *c, int32_t *fr, int *nf, int32_t *tf, int32_t bno)
{
	if (bno != 0)
		(*tf)++;
	if (*nf >= P11_NICFREE) {
		uint8_t fb[P11_MAXBSIZE];
		int i;
		memset(fb, 0, c->bsize);
		c->bo->put16(fb + P11_FB_NFREE, (uint16_t)*nf);
		for (i = 0; i < P11_NICFREE; i++)
			c->bo->put32(fb + P11_FB_FREE + 4 * i, (uint32_t)fr[i]);
		wtblk(c, (uint32_t)bno, fb);
		*nf = 0;
	}
	fr[(*nf)++] = bno;
}

/* Pick the byte order whose superblock decode is self-consistent against the
 * image's actual size.  For a wrong regime, s_isize/s_fsize come out garbage
 * (isize >= fsize, or fsize past end of file), so this discriminates cleanly. */
static const s5_codec *
detect_bo(int fd, uint32_t bsize, const uint8_t *sb)
{
	struct stat st;
	uint32_t fileblocks;
	int e;

	if (fstat(fd, &st) < 0)
		return NULL;
	fileblocks = (uint32_t)(st.st_size / bsize);
	for (e = 0; e < S5_NENDIAN; e++) {
		const s5_codec *cod = s5_codec_for((s5_endian)e);
		uint32_t isize = cod->get16(sb + P11_SB_ISIZE);
		uint32_t fsize = cod->get32(sb + P11_SB_FSIZE);
		if (isize >= P11_ILISTSTART && isize < fsize &&
		    fsize > 0 && fsize <= fileblocks)
			return cod;
	}
	return NULL;
}

/* Mark a data/meta block used; flag reserved-range and double-allocation. */
static void
mark(CK *c, uint32_t bno, const char *what)
{
	if (bno == 0)
		return;
	if (bno < c->isize || bno >= c->fsize) {
		fprintf(stderr, "  ! %s block %u out of range [%u,%u)\n",
			what, bno, c->isize, c->fsize);
		c->errors++;
		return;
	}
	if ((c->use[bno] & 0x7f) == 0)
		c->use[bno] |= 1;
	else {
		if ((c->use[bno] & 0x7f) < 0x7f)
			c->use[bno]++;
		fprintf(stderr, "  ! block %u multiply referenced (%s)\n", bno, what);
		c->errors++;
	}
}

/* Recursively mark an indirect block and everything it points to.
 * level 1 = single, 2 = double, 3 = triple. */
static void
mark_indirect(CK *c, uint32_t bno, int level)
{
	uint8_t *buf;
	uint32_t i;

	if (bno == 0)
		return;
	mark(c, bno, level == 1 ? "single-indirect" : level == 2 ? "double-indirect"
								 : "triple-indirect");
	if (bno < c->isize || bno >= c->fsize)
		return;
	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");
	rdblk(c, bno, buf);
	for (i = 0; i < c->nindir; i++) {
		uint32_t e = c->bo->get32(buf + 4 * i);
		if (level == 1)
			mark(c, e, "data");
		else
			mark_indirect(c, e, level - 1);
	}
	free(buf);
}

/* Read on-disk inode `ino` into caller-supplied field outs. */
static int
read_inode(CK *c, uint32_t ino, uint16_t *mode, uint16_t *nlink,
	   int32_t *size, int32_t addr[P11_MAXNADDR])
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t blk = (ino + 2 * c->inopb - 1) / c->inopb;
	uint32_t off = ((ino + 2 * c->inopb - 1) % c->inopb) * P11_DINODESZ;
	uint8_t *dp;
	uint32_t i;

	if (blk >= c->isize)
		return -1;
	rdblk(c, blk, buf);
	dp = buf + off;
	*mode = c->bo->get16(dp + P11_DI_MODE);
	*nlink = c->bo->get16(dp + P11_DI_NLINK);
	*size = (int32_t)c->bo->get32(dp + P11_DI_SIZE);
	for (i = 0; i < c->naddr; i++)
		addr[i] = (int32_t)c->bo->get24(dp + P11_DI_ADDR + 3 * i);
	return 0;
}

/* Map logical block `lbn` of an inode to its physical block (0 = hole). */
static uint32_t
bmap(CK *c, const int32_t *addr, uint32_t lbn)
{
	uint8_t *buf;
	uint32_t phys, per = c->nindir;

	if (lbn < c->laddr)
		return (uint32_t)addr[lbn];
	lbn -= c->laddr;

	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");

	if (lbn < per) { /* single indirect */
		phys = (uint32_t)addr[c->laddr];
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * lbn);
		}
	}
	else if (lbn < per * per) { /* double indirect */
		lbn -= per;
		phys = (uint32_t)addr[c->laddr + 1];
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * (lbn / per));
		}
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * (lbn % per));
		}
	}
	else { /* triple indirect */
		lbn -= per * per;
		phys = (uint32_t)addr[c->laddr + 2];
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * (lbn / (per * per)));
		}
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * ((lbn / per) % per));
		}
		if (phys) {
			rdblk(c, phys, buf);
			phys = c->bo->get32(buf + 4 * (lbn % per));
		}
	}
	free(buf);
	return phys;
}

/* ncheck-style recursive directory listing.  `seen` is the cycle guard: a
 * looped image (a corrupt one, or one this tool's own -p just reconnected)
 * would otherwise recurse until the stack is exhausted.  Phase 3 already keeps
 * a reached[] array for the same reason; this walker needs its own. */
static void
walk_dir(CK *c, uint32_t ino, const char *path, uint8_t *seen, uint32_t nino)
{
	uint16_t mode, nlink;
	int32_t size, addr[P11_MAXNADDR];
	uint32_t nblk, b;
	uint8_t *buf;

	if (ino == 0 || ino > nino || seen[ino])
		return;
	seen[ino] = 1;
	if (read_inode(c, ino, &mode, &nlink, &size, addr) < 0)
		return;
	if ((mode & P11_IFMT) != P11_IFDIR)
		return;

	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");
	nblk = ((uint32_t)size + c->bsize - 1) / c->bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t phys = bmap(c, addr, b), e;
		if (phys == 0)
			continue;
		rdblk(c, phys, buf);
		for (e = 0; e < c->ndirect; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint16_t di = c->bo->get16(d);
			char name[P11_DIRSIZ + 1];
			char child[1024];
			/* stop at the directory's real size: the tail of the
			 * last block is stale bytes, not entries */
			if ((b * c->ndirect + e) * P11_DIRENTSZ >= (uint32_t)size)
				break;
			if (di == 0)
				continue;
			memcpy(name, d + 2, P11_DIRSIZ);
			name[P11_DIRSIZ] = '\0';
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
				continue;
			snprintf(child, sizeof child, "%s%s%s",
				 path, strcmp(path, "/") ? "/" : "", name);
			printf("%5u %s\n", di, child);
			walk_dir(c, di, child, seen, nino);
		}
	}
	free(buf);
}

/* mode has a valid s5fs file type */
static int
good_mode(uint16_t mode)
{
	switch (mode & P11_IFMT) {
	case P11_IFREG:
	case P11_IFDIR:
	case P11_IFCHR:
	case P11_IFBLK:
		return 1;
	default:
		return 0;
	}
}

/* Phase 3 connectivity: DFS from a directory, marking every reachable inode.
 * Catches orphan directories (whose own "." keeps their link count nonzero). */
static void
mark_reached(CK *c, uint32_t ino, uint8_t *reached, uint32_t nino)
{
	uint16_t mode, nlink;
	int32_t size, addr[P11_MAXNADDR];
	uint32_t nblk, b;
	uint8_t *buf;

	if (ino == 0 || ino > nino || reached[ino])
		return;
	reached[ino] = 1;
	if (read_inode(c, ino, &mode, &nlink, &size, addr) < 0)
		return;
	if ((mode & P11_IFMT) != P11_IFDIR)
		return;
	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");
	nblk = ((uint32_t)size + c->bsize - 1) / c->bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t phys = bmap(c, addr, b), e;
		if (!phys)
			continue;
		rdblk(c, phys, buf);
		for (e = 0; e < c->ndirect; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint32_t di = c->bo->get16(d);
			char nm[P11_DIRSIZ + 1];
			if (di == 0 || di > nino)
				continue;
			memcpy(nm, d + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (!strcmp(nm, ".") || !strcmp(nm, ".."))
				continue;
			mark_reached(c, di, reached, nino);
		}
	}
	free(buf);
}

/* find `name` in directory `dino`; 0 if absent */
static uint32_t
find_in_dir(CK *c, uint32_t dino, const char *name)
{
	uint16_t mode, nlink;
	int32_t size, addr[P11_MAXNADDR];
	uint32_t nblk, b, res = 0;
	uint8_t *buf;

	if (read_inode(c, dino, &mode, &nlink, &size, addr) < 0)
		return 0;
	if ((mode & P11_IFMT) != P11_IFDIR)
		return 0;
	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");
	nblk = ((uint32_t)size + c->bsize - 1) / c->bsize;
	for (b = 0; b < nblk && !res; b++) {
		uint32_t phys = bmap(c, addr, b), e;
		if (!phys)
			continue;
		rdblk(c, phys, buf);
		for (e = 0; e < c->ndirect; e++) {
			uint32_t di = c->bo->get16(buf + e * P11_DIRENTSZ);
			char nm[P11_DIRSIZ + 1];
			if (di == 0)
				continue;
			memcpy(nm, buf + e * P11_DIRENTSZ + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (!strcmp(nm, name)) {
				res = di;
				break;
			}
		}
	}
	free(buf);
	return res;
}

/* patch one inode's di_nlink */
static void
ck_set_nlink(CK *c, uint32_t ino, uint16_t nlink)
{
	uint8_t b[P11_MAXBSIZE];
	uint32_t blk = (ino + 2 * c->inopb - 1) / c->inopb;
	uint32_t o = ((ino + 2 * c->inopb - 1) % c->inopb) * P11_DINODESZ;
	rdblk(c, blk, b);
	c->bo->put16(b + o + P11_DI_NLINK, nlink);
	wtblk(c, blk, b);
}

/* point directory `dino`'s ".." entry (slot 1 of its first block) at `parent` */
static void
ck_set_dotdot(CK *c, uint32_t dino, uint32_t parent)
{
	uint16_t mode, nlink;
	int32_t size, addr[P11_MAXNADDR];
	uint8_t *b;
	uint32_t phys;
	if (read_inode(c, dino, &mode, &nlink, &size, addr) < 0)
		return;
	phys = bmap(c, addr, 0);
	if (!phys)
		return;
	b = malloc(c->bsize);
	if (!b)
		die("out of memory");
	rdblk(c, phys, b);
	c->bo->put16(b + 1 * P11_DIRENTSZ, (uint16_t)parent);
	wtblk(c, phys, b);
	free(b);
}

/* put `ino`->`name` into a free slot of directory `dir`; 0 if it's full */
static int
dir_reuse_slot(CK *c, uint32_t dir, uint32_t ino, const char *name)
{
	uint16_t mode, nlink;
	int32_t size, addr[P11_MAXNADDR];
	uint32_t nblk, b, done = 0;
	uint8_t *buf;
	size_t l = strlen(name);
	if (l > P11_DIRSIZ)
		l = P11_DIRSIZ;
	if (read_inode(c, dir, &mode, &nlink, &size, addr) < 0)
		return 0;
	buf = malloc(c->bsize);
	if (!buf)
		die("out of memory");
	nblk = ((uint32_t)size + c->bsize - 1) / c->bsize;
	for (b = 0; b < nblk && !done; b++) {
		uint32_t phys = bmap(c, addr, b), e;
		if (!phys)
			continue;
		rdblk(c, phys, buf);
		for (e = 0; e < c->ndirect; e++) {
			if ((b * c->ndirect + e) * P11_DIRENTSZ >= (uint32_t)size)
				break;
			if (c->bo->get16(buf + e * P11_DIRENTSZ) == 0) {
				c->bo->put16(buf + e * P11_DIRENTSZ, (uint16_t)ino);
				memset(buf + e * P11_DIRENTSZ + 2, 0, P11_DIRSIZ);
				memcpy(buf + e * P11_DIRENTSZ + 2, name, l);
				wtblk(c, phys, buf);
				done = 1;
				break;
			}
		}
	}
	free(buf);
	return (int)done;
}

/* recompute every inode's directory reference count (after repairs) */
static void
ck_recount(CK *c, uint32_t *linkcnt, uint32_t nino)
{
	uint32_t ino;
	uint8_t *db = malloc(c->bsize);
	if (!db)
		die("out of memory");
	for (ino = 0; ino <= nino; ino++)
		linkcnt[ino] = 0;
	for (ino = 1; ino <= nino; ino++) {
		uint16_t mode, nlink;
		int32_t size, addr[P11_MAXNADDR];
		uint32_t nblk, b;
		if (read_inode(c, ino, &mode, &nlink, &size, addr) < 0)
			continue;
		if ((mode & P11_IFMT) != P11_IFDIR)
			continue;
		nblk = ((uint32_t)size + c->bsize - 1) / c->bsize;
		for (b = 0; b < nblk; b++) {
			uint32_t phys = bmap(c, addr, b), e;
			if (!phys)
				continue;
			rdblk(c, phys, db);
			for (e = 0; e < c->ndirect; e++) {
				uint32_t di = c->bo->get16(db + e * P11_DIRENTSZ);
				if (di != 0 && di <= nino)
					linkcnt[di]++;
			}
		}
	}
	free(db);
}

/* which checks fsck_run performs; icheck=blocks, dcheck=links, fsck=both */
#define CHK_BLOCKS 1
#define CHK_LINKS 2

static int
fsck_run(const char *path, uint32_t bsize, s5_endian forced,
	 int checks, int list, int repair, int64_t base)
{
	CK c;
	uint8_t sb[P11_MAXBSIZE];
	uint32_t ino, nino, used = 0, files = 0, i;
	uint32_t *dnlink = NULL, *linkcnt = NULL; /* dcheck: di_nlink vs refs */
	uint8_t *isdir = NULL, *alloc = NULL, *reached = NULL;
	int sysv; /* System V superblock dialect */
	uint32_t tfree_off, tinode_off;

	memset(&c, 0, sizeof c);
	if (!P11_BSIZE_OK(bsize))
		die("block size must be 512, 1024, or 2048");

	c.fd = open(path, repair ? O_RDWR : O_RDONLY);
	if (c.fd < 0) {
		fprintf(stderr, "s5fs fsck: %s: %s\n", path, strerror(errno));
		return 1;
	}

	c.base = base;
	c.bsize = bsize;
	c.inopb = bsize / P11_DINODESZ;
	c.naddr = (bsize == 1024) ? 7 : 13;
	c.laddr = c.naddr - 3;
	c.nindir = bsize / 4;
	c.ndirect = bsize / P11_DIRENTSZ;

	rdblk(&c, P11_SUPERBLK, sb);
	c.bo = forced != S5_NENDIAN ? s5_codec_for(forced) : detect_bo(c.fd, bsize, sb);
	if (!c.bo)
		die("cannot determine byte order (try -A pdp11|le|be, or check -B)");

	c.isize = c.bo->get16(sb + P11_SB_ISIZE);
	c.fsize = c.bo->get32(sb + P11_SB_FSIZE);
	c.nfree = (int16_t)c.bo->get16(sb + P11_SB_NFREE);
	c.ninode = c.bo->get16(sb + P11_SB_NINODE);
	sysv = (c.bo->get32(sb + P11_SB_MAGIC) == (uint32_t)P11_FS_MAGIC);
	tfree_off = sysv ? P11_SB_SVTFREE : P11_SB_TFREE; /* SysV moves the totals */
	tinode_off = sysv ? P11_SB_SVTINODE : P11_SB_TINODE;
	c.tfree = (int32_t)c.bo->get32(sb + tfree_off);
	c.tinode = c.bo->get16(sb + tinode_off);

	if (c.isize < P11_ILISTSTART || c.isize >= c.fsize || c.fsize == 0)
		die("superblock looks wrong (bad isize/fsize; try -B 512 or -A)");

	printf("image: %s   %s  block=%u  isize=%u  fsize=%u  m/n=%u/%u\n",
	       path, c.bo->name, c.bsize, c.isize, c.fsize,
	       c.bo->get16(sb + P11_SB_DINFO), c.bo->get16(sb + P11_SB_DINFO + 2));

	c.use = calloc(c.fsize, 1);
	nino = (c.isize - 2) * c.inopb;
	dnlink = calloc((size_t)nino + 1, sizeof *dnlink);
	linkcnt = calloc((size_t)nino + 1, sizeof *linkcnt);
	isdir = calloc((size_t)nino + 1, 1);
	alloc = calloc((size_t)nino + 1, 1);
	if (!c.use || !dnlink || !linkcnt || !isdir || !alloc)
		die("out of memory");

	/* 1) every allocated inode's block tree; note link counts + dir flags */
	for (ino = 1; ino <= nino; ino++) {
		uint16_t mode, nlink;
		int32_t size, addr[P11_MAXNADDR];
		if (read_inode(&c, ino, &mode, &nlink, &size, addr) < 0)
			break;
		if (mode == 0)
			continue;
		files++;
		alloc[ino] = 1;
		dnlink[ino] = nlink;
		if ((mode & P11_IFMT) == P11_IFDIR)
			isdir[ino] = 1;
		if ((checks & CHK_LINKS) && !good_mode(mode)) {
			fprintf(stderr, "  ! inode %u has invalid mode 0%o (unknown file type)\n",
				ino, mode);
			c.errors++;
		}
		/* block accounting (icheck): only regular files and directories have
		 * block maps -- for special files addr[0] is the device number. */
		if (!(checks & CHK_BLOCKS))
			continue;
		if ((mode & P11_IFMT) != P11_IFREG && (mode & P11_IFMT) != P11_IFDIR)
			continue;
		for (i = 0; i < c.laddr; i++)
			mark(&c, (uint32_t)addr[i], "direct");
		mark_indirect(&c, (uint32_t)addr[c.laddr], 1);
		if (c.naddr > c.laddr + 1)
			mark_indirect(&c, (uint32_t)addr[c.laddr + 1], 2);
		if (c.naddr > c.laddr + 2)
			mark_indirect(&c, (uint32_t)addr[c.laddr + 2], 3);
	}

	/* 2) the chained free list, traversed exactly as alloc() would */
	if (checks & CHK_BLOCKS) {
		int32_t nfree = c.nfree, free_[P11_NICFREE];
		uint8_t *fb = malloc(c.bsize);
		if (!fb)
			die("out of memory");
		for (i = 0; i < P11_NICFREE; i++)
			free_[i] = (int32_t)c.bo->get32(sb + P11_SB_FREE + 4 * i);
		for (;;) {
			int32_t bno;
			if (nfree <= 0) {
				fprintf(stderr, "  ! free list count underflow\n");
				c.errors++;
				break;
			}
			bno = free_[--nfree];
			if (bno == 0)
				break; /* list terminator */
			if ((uint32_t)bno < c.isize || (uint32_t)bno >= c.fsize) {
				fprintf(stderr, "  ! free block %d out of range\n", bno);
				c.errors++;
				continue;
			}
			if (c.use[bno] & 0x7f) {
				fprintf(stderr, "  ! block %d free AND used\n", bno);
				c.errors++;
			}
			c.use[bno] |= 0x80;
			if (nfree <= 0) { /* this block chains the list */
				uint32_t k;
				rdblk(&c, (uint32_t)bno, fb);
				nfree = (int16_t)c.bo->get16(fb + P11_FB_NFREE);
				for (k = 0; k < P11_NICFREE; k++)
					free_[k] = (int32_t)c.bo->get32(fb + P11_FB_FREE + 4 * k);
			}
		}
		free(fb);
	}

	/* 3) partition check over the data region */
	if (checks & CHK_BLOCKS) {
		uint32_t missing = 0, both = 0, freec = 0;
		for (i = c.isize; i < c.fsize; i++) {
			int u = (c.use[i] & 0x7f) != 0;
			int f = (c.use[i] & 0x80) != 0;
			if (u)
				used++;
			if (f)
				freec++;
			if (u && f)
				both++;
			if (!u && !f)
				missing++;
		}
		printf("files=%u  used-blocks=%u  free-blocks=%u  (superblock tfree=%d, tinode=%d)\n",
		       files, used, freec, c.tfree, c.tinode);
		if (missing) {
			fprintf(stderr, "  ! %u data blocks neither used nor free\n", missing);
			c.errors++;
		}
		if (both) {
			fprintf(stderr, "  ! %u data blocks both used and free\n", both);
			c.errors++;
		}
		if ((int32_t)freec != c.tfree)
			fprintf(stderr, "  note: counted free %u != superblock tfree %d\n", freec, c.tfree);
	}

	/* 4) link counts (dcheck): tally directory references to each inode and
	 * compare with di_nlink -- an independent check of the writer's nlink. */
	if (checks & CHK_LINKS) {
		uint8_t *db = malloc(c.bsize);
		uint32_t bad = 0;
		if (!db)
			die("out of memory");
		for (ino = 1; ino <= nino; ino++) {
			uint16_t mode, nlink;
			int32_t size, addr[P11_MAXNADDR];
			uint32_t nblk, b;
			if (!isdir[ino])
				continue;
			if (read_inode(&c, ino, &mode, &nlink, &size, addr) < 0)
				continue;
			nblk = ((uint32_t)size + c.bsize - 1) / c.bsize;
			for (b = 0; b < nblk; b++) {
				uint32_t phys = bmap(&c, addr, b), e;
				if (!phys)
					continue;
				rdblk(&c, phys, db);
				for (e = 0; e < c.ndirect; e++) {
					uint8_t *d = db + e * P11_DIRENTSZ;
					uint32_t di = c.bo->get16(d);
					char nm[P11_DIRSIZ + 1];
					if (di == 0)
						continue;
					memcpy(nm, d + 2, P11_DIRSIZ);
					nm[P11_DIRSIZ] = '\0';
					if (di > nino) {
						fprintf(stderr, "  ! dir %u entry '%s' -> out-of-range inode %u\n", ino, nm, di);
						c.errors++;
						continue;
					}
					linkcnt[di]++;
					if (!alloc[di]) { /* Phase 2: dangling reference */
						fprintf(stderr, "  ! dir %u entry '%s' -> unallocated inode %u (dangling)\n", ino, nm, di);
						c.errors++;
					}
					if (!strcmp(nm, ".") && di != ino) {
						fprintf(stderr, "  ! dir %u '.' points to %u, not itself\n", ino, di);
						c.errors++;
					}
				}
			}
		}
		free(db);
		for (ino = 1; ino <= nino; ino++) {
			if (linkcnt[ino] == dnlink[ino])
				continue;
			bad++;
			if (dnlink[ino] == 0)
				fprintf(stderr, "  ! inode %u is free but %u director%s reference it\n",
					ino, linkcnt[ino], linkcnt[ino] == 1 ? "y" : "ies");
			else
				fprintf(stderr, "  ! inode %u di_nlink=%u but %u director%s reference it\n",
					ino, dnlink[ino], linkcnt[ino], linkcnt[ino] == 1 ? "y" : "ies");
		}
		if (bad)
			c.errors++;
		else
			printf("link counts OK (%u inodes)\n", files);

		/* Phase 3: connectivity.  Anything allocated but unreachable from root
		 * is an orphan (an orphan directory still has a nonzero link count from
		 * its own ".", so reachability -- not link count -- is what finds it). */
		reached = calloc((size_t)nino + 1, 1);
		if (!reached)
			die("out of memory");
		mark_reached(&c, P11_ROOTINO, reached, nino);
		reached[P11_BADBLKINO] = 1; /* inode 1 (bad-block list) is legitimately unlinked */
		{
			uint32_t orph = 0;
			for (ino = 1; ino <= nino; ino++)
				if (alloc[ino] && !reached[ino]) {
					orph++;
					fprintf(stderr, "  ! inode %u (%s) allocated but unreachable from root (orphan)\n",
						ino, isdir[ino] ? "dir" : "file");
				}
			if (orph) {
				c.errors++;
				if (!repair)
					fprintf(stderr, "    (fsck -p reconnects orphans into /lost+found)\n");
			}
		}
	}

	/* 5) repair: fix link counts (dcheck) and/or salvage the free list (icheck) */
	if (repair) {
		uint32_t fixed = 0, zapped = 0, recon = 0;
		int32_t tf = 0;

		if (checks & CHK_LINKS) {
			uint32_t lf = find_in_dir(&c, P11_ROOTINO, "lost+found");
			uint8_t *bb = malloc(c.bsize);
			if (!bb)
				die("out of memory");

			/* (a) zero directory entries pointing at free/out-of-range inodes */
			for (ino = 1; ino <= nino; ino++) {
				uint16_t mode, nlink;
				int32_t size, addr[P11_MAXNADDR];
				uint32_t nblk, b;
				if (!isdir[ino] || read_inode(&c, ino, &mode, &nlink, &size, addr) < 0)
					continue;
				nblk = ((uint32_t)size + c.bsize - 1) / c.bsize;
				for (b = 0; b < nblk; b++) {
					uint32_t phys = bmap(&c, addr, b), e;
					int dirty = 0;
					if (!phys)
						continue;
					rdblk(&c, phys, bb);
					for (e = 0; e < c.ndirect; e++) {
						uint32_t di = c.bo->get16(bb + e * P11_DIRENTSZ);
						if (di == 0)
							continue;
						if (di > nino || !alloc[di]) {
							c.bo->put16(bb + e * P11_DIRENTSZ, 0);
							dirty = 1;
							zapped++;
						}
					}
					if (dirty)
						wtblk(&c, phys, bb);
				}
			}
			/* (b) reconnect orphans (unreachable-from-root) into /lost+found,
			 * reusing its preallocated empty slots (mklost leaves plenty) */
			if (lf) {
				for (ino = P11_ROOTINO + 1; ino <= nino; ino++) {
					char nm[16];
					if (!alloc[ino] || reached[ino])
						continue;
					snprintf(nm, sizeof nm, "#%u", ino);
					if (!dir_reuse_slot(&c, lf, ino, nm)) {
						fprintf(stderr, "  ! /lost+found is full; some orphans not reconnected\n");
						break;
					}
					if (isdir[ino])
						ck_set_dotdot(&c, ino, lf);   /* its ".." now -> lost+found */
					mark_reached(&c, ino, reached, nino); /* and its whole subtree */
					recon++;
				}
			}
			else if (reached) {
				for (ino = P11_ROOTINO + 1; ino <= nino; ino++)
					if (alloc[ino] && !reached[ino]) {
						fprintf(stderr, "  ! no /lost+found -- cannot reconnect orphan inode %u\n", ino);
						break;
					}
			}
			free(bb);

			/* (c) recompute link counts after the structural repairs, fix di_nlink */
			ck_recount(&c, linkcnt, nino);
			for (ino = 1; ino <= nino; ino++) {
				uint16_t mode, nlink;
				int32_t size, addr[P11_MAXNADDR];
				if (!alloc[ino] || read_inode(&c, ino, &mode, &nlink, &size, addr) < 0)
					continue;
				if (nlink == linkcnt[ino])
					continue;
				ck_set_nlink(&c, ino, (uint16_t)linkcnt[ino]);
				fixed++;
			}
		}
		if (checks & CHK_BLOCKS) {
			int32_t fr[P11_NICFREE];
			int nf = 0;
			uint32_t bno;
			rep_bfree(&c, fr, &nf, &tf, 0); /* free-list terminator */
			for (bno = c.fsize; bno-- > c.isize;)
				if (!(c.use[bno] & 0x7f))
					rep_bfree(&c, fr, &nf, &tf, (int32_t)bno);
			c.bo->put16(sb + P11_SB_NFREE, (uint16_t)nf);
			for (i = 0; i < P11_NICFREE; i++)
				c.bo->put32(sb + P11_SB_FREE + 4 * i, (uint32_t)fr[i]);
			c.bo->put32(sb + tfree_off, (uint32_t)tf);
			c.bo->put16(sb + tinode_off, (uint16_t)((c.isize - 2) * c.inopb - files));
			wtblk(&c, P11_SUPERBLK, sb);
		}
		close(c.fd);
		printf("repaired:");
		if (checks & CHK_LINKS)
			printf(" %u link count(s), %u orphan(s) -> lost+found, %u dangling entr%s zapped;",
			       fixed, recon, zapped, zapped == 1 ? "y" : "ies");
		if (checks & CHK_BLOCKS)
			printf(" free list rebuilt (%d free)", tf);
		printf("\n");
		return 0;
	}

	if (list) {
		printf("--- tree from root (inode %u) ---\n", P11_ROOTINO);
		printf("%5u /\n", P11_ROOTINO);
		{
			uint8_t *seen = calloc((size_t)nino + 1, 1);
			if (!seen)
				die("out of memory");
			walk_dir(&c, P11_ROOTINO, "/", seen, nino);
			free(seen);
		}
	}

	close(c.fd);
	printf("%s\n", c.errors ? "FILESYSTEM HAS ERRORS" : "clean");
	return c.errors ? 1 : 0;
}

/* -------- subcommands: fsck (both), icheck (blocks), dcheck (links), clri -------- */

int
cmd_fsck(int argc, char **argv)
{
	uint32_t bsize = 1024, plen = 0;
	s5_endian forced = S5_NENDIAN;
	int list = 0, repair = 0, opt;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	while ((opt = getopt(argc, argv, "B:A:lpd:P:o:")) != -1) {
		switch (opt) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'l':
			list = 1;
			break;
		case 'p':
			repair = 1;
			break;
		case 'A':
			forced = s5_endian_parse(optarg);
			if (forced == S5_NENDIAN)
				die("bad -A byte order (use pdp11|le|be)");
			break;
		case 'd':
			dev = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		default:
			fprintf(stderr, "usage: s5fs fsck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] [-l] [-p] image\n");
			return 2;
		}
	}
	if (optind != argc - 1) {
		fprintf(stderr, "usage: s5fs fsck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] [-l] [-p] image\n");
		return 2;
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	return fsck_run(argv[optind], bsize, forced, CHK_BLOCKS | CHK_LINKS, list, repair, base);
}

int
cmd_icheck(int argc, char **argv) /* block/free-list check (-s salvages) */
{
	uint32_t bsize = 1024, plen = 0;
	s5_endian forced = S5_NENDIAN;
	int repair = 0, opt;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	while ((opt = getopt(argc, argv, "B:A:sd:P:o:")) != -1) {
		switch (opt) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 's':
			repair = 1;
			break;
		case 'A':
			forced = s5_endian_parse(optarg);
			if (forced == S5_NENDIAN)
				die("bad -A byte order (use pdp11|le|be)");
			break;
		case 'd':
			dev = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		default:
			fprintf(stderr, "usage: s5fs icheck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] [-s] image\n");
			return 2;
		}
	}
	if (optind != argc - 1) {
		fprintf(stderr, "usage: s5fs icheck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] [-s] image\n");
		return 2;
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	return fsck_run(argv[optind], bsize, forced, CHK_BLOCKS, 0, repair, base);
}

int
cmd_dcheck(int argc, char **argv) /* directory link-count check */
{
	uint32_t bsize = 1024, plen = 0;
	s5_endian forced = S5_NENDIAN;
	int opt;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	while ((opt = getopt(argc, argv, "B:A:d:P:o:")) != -1) {
		switch (opt) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A':
			forced = s5_endian_parse(optarg);
			if (forced == S5_NENDIAN)
				die("bad -A byte order (use pdp11|le|be)");
			break;
		case 'd':
			dev = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		default:
			fprintf(stderr, "usage: s5fs dcheck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] image\n");
			return 2;
		}
	}
	if (optind != argc - 1) {
		fprintf(stderr, "usage: s5fs dcheck [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] image\n");
		return 2;
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	return fsck_run(argv[optind], bsize, forced, CHK_LINKS, 0, 0, base);
}

int
cmd_clri(int argc, char **argv) /* clear (zero) an inode by number */
{
	CK c;
	uint8_t sb[P11_MAXBSIZE];
	uint32_t bsize = 1024, plen = 0;
	s5_endian forced = S5_NENDIAN;
	int opt, i, n = 0;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;

	memset(&c, 0, sizeof c);
	while ((opt = getopt(argc, argv, "B:A:d:P:o:")) != -1) {
		switch (opt) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A':
			forced = s5_endian_parse(optarg);
			if (forced == S5_NENDIAN)
				die("bad -A byte order (use pdp11|le|be)");
			break;
		case 'd':
			dev = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		default:
			fprintf(stderr, "usage: s5fs clri [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] image inode...\n");
			return 2;
		}
	}
	if (optind > argc - 2) {
		fprintf(stderr, "usage: s5fs clri [-B 512|1024|2048] [-A pdp11|le|be] [-d dev -P part | -o blk] image inode...\n");
		return 2;
	}
	if (!P11_BSIZE_OK(bsize))
		die("block size must be 512, 1024, or 2048");
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	c.base = base;
	c.fd = open(argv[optind], O_RDWR);
	if (c.fd < 0) {
		fprintf(stderr, "s5fs clri: %s: %s\n", argv[optind], strerror(errno));
		return 1;
	}
	c.bsize = bsize;
	c.inopb = bsize / P11_DINODESZ;
	c.naddr = (bsize == 1024) ? 7 : 13;
	c.laddr = c.naddr - 3;
	c.nindir = bsize / 4;
	c.ndirect = bsize / P11_DIRENTSZ;
	rdblk(&c, P11_SUPERBLK, sb);
	c.bo = (forced != S5_NENDIAN) ? s5_codec_for(forced) : detect_bo(c.fd, bsize, sb);
	if (!c.bo)
		die("cannot determine byte order (try -A/-B)");
	c.isize = c.bo->get16(sb + P11_SB_ISIZE);
	c.fsize = c.bo->get32(sb + P11_SB_FSIZE);
	for (i = optind + 1; i < argc; i++) {
		uint8_t b[P11_MAXBSIZE];
		uint32_t inum = (uint32_t)strtoul(argv[i], NULL, 0);
		uint32_t blk = (inum + 2 * c.inopb - 1) / c.inopb;
		uint32_t o = ((inum + 2 * c.inopb - 1) % c.inopb) * P11_DINODESZ;
		if (inum == 0 || blk >= c.isize) {
			fprintf(stderr, "s5fs clri: inode %u out of range\n", inum);
			continue;
		}
		rdblk(&c, blk, b);
		memset(b + o, 0, P11_DINODESZ);
		wtblk(&c, blk, b);
		n++;
	}
	close(c.fd);
	printf("cleared %d inode(s); run 's5fs fsck -p' to reclaim their blocks\n", n);
	return 0;
}

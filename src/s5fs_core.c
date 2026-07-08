/*
 * s5fs_core.c -- s5fs (traditional Unix) filesystem writer.
 *
 * Ported from 2.9BSD cmd/mkfs.c (SCCS "@(#)mkfs.c 2.5").  The layout maths,
 * the rotational free-list interleave (bflist), the allocator (alloc/bfree),
 * and the inode/directory writers (iput/entry/newblk/mklost) follow the
 * original line-for-line so that the images we produce are the ones 2.9BSD's
 * own fsck and kernel expect.  What changed for a modern host:
 *
 *   - No struct overlay on disk bytes; every field goes through a byte-order
 *     codec (fs->bo, see s5endian.h) so the same writer emits PDP-11, VAX/LE,
 *     or big-endian s5fs images.
 *   - mkfs's file-scope globals are gathered into an S5FS handle.
 *   - The block size is a runtime profile (512 or 1024) rather than a compile
 *     -time constant, but both profiles reproduce the corresponding 2.9 build.
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

static void s5fs_fail(S5FS *fs, const char *msg)
{
	if (!fs->error) {			/* keep the first error */
		snprintf(fs->err, sizeof fs->err, "%s", msg);
		fs->error = 1;
	}
}

/* inode number -> i-list block, and offset within that block.  The i-list
 * begins at block 2, INOPB inodes per block, so inode i (1-based) lives at
 * block 2 + (i-1)/INOPB.  Written to match mkfs's own itoo macro. */
static uint32_t s5fs_itod(const S5FS *fs, uint32_t ino)
{
	return (ino + 2 * fs->inopb - 1) / fs->inopb;
}

static uint32_t s5fs_itoo(const S5FS *fs, uint32_t ino)
{
	return (ino + 2 * fs->inopb - 1) % fs->inopb;
}

/* ------------------------------------------------------------------ *
 * raw block I/O  (mkfs rdfs / wtfs)
 * ------------------------------------------------------------------ */

void s5fs_rdblk(S5FS *fs, uint32_t bno, uint8_t *buf)
{
	off_t off = (off_t)(fs->base + (int64_t)bno * fs->bsize);

	if (fs->error)
		return;
	if (lseek(fs->fd, off, SEEK_SET) < 0 ||
	    read(fs->fd, buf, fs->bsize) != (ssize_t)fs->bsize)
		s5fs_fail(fs, "read error");
}

void s5fs_wtblk(S5FS *fs, uint32_t bno, const uint8_t *buf)
{
	off_t off = (off_t)(fs->base + (int64_t)bno * fs->bsize);

	if (fs->error)
		return;
	if (lseek(fs->fd, off, SEEK_SET) < 0 ||
	    write(fs->fd, buf, fs->bsize) != (ssize_t)fs->bsize)
		s5fs_fail(fs, "write error");
}

/* ------------------------------------------------------------------ *
 * free-list block (de)serialisation  (struct fblk)
 * ------------------------------------------------------------------ */

static void s5fs_put_fblk(S5FS *fs, int32_t bno)
{
	uint8_t blk[P11_MAXBSIZE];
	int i;

	memset(blk, 0, fs->bsize);
	fs->bo->put16(blk + P11_FB_NFREE, (uint16_t)fs->s_nfree);
	for (i = 0; i < P11_NICFREE; i++)
		fs->bo->put32(blk + P11_FB_FREE + 4 * i, (uint32_t)fs->s_free[i]);
	s5fs_wtblk(fs, (uint32_t)bno, blk);
}

static void s5fs_get_fblk(S5FS *fs, int32_t bno)
{
	uint8_t blk[P11_MAXBSIZE];
	int i;

	s5fs_rdblk(fs, (uint32_t)bno, blk);
	fs->s_nfree = (int16_t)fs->bo->get16(blk + P11_FB_NFREE);
	for (i = 0; i < P11_NICFREE; i++)
		fs->s_free[i] = (int32_t)fs->bo->get32(blk + P11_FB_FREE + 4 * i);
}

/* ------------------------------------------------------------------ *
 * allocator  (mkfs alloc / bfree)
 * ------------------------------------------------------------------ */

int32_t s5fs_alloc(S5FS *fs)
{
	int32_t bno;

	fs->s_tfree--;
	bno = fs->s_free[--fs->s_nfree];
	if (bno == 0) {
		s5fs_fail(fs, "out of free space");
		return 0;
	}
	if (fs->s_nfree <= 0)
		s5fs_get_fblk(fs, bno);		/* this block chained the list */
	return bno;
}

void s5fs_bfree(S5FS *fs, int32_t bno)
{
	if (bno != 0)
		fs->s_tfree++;
	if (fs->s_nfree >= P11_NICFREE) {
		s5fs_put_fblk(fs, bno);		/* spill a full cache into bno */
		fs->s_nfree = 0;
	}
	fs->s_free[fs->s_nfree++] = bno;
}

/* ------------------------------------------------------------------ *
 * directory + data blocks  (mkfs entry / newblk)
 * ------------------------------------------------------------------ */

void s5fs_newblk(S5FS *fs, int *dbc, uint8_t *db, int *ibc, int32_t *ib)
{
	int32_t bno = s5fs_alloc(fs);

	s5fs_wtblk(fs, (uint32_t)bno, db);
	memset(db, 0, fs->bsize);
	*dbc = 0;
	ib[*ibc] = bno;
	(*ibc)++;
	if ((uint32_t)*ibc >= fs->nindir) {
		s5fs_fail(fs, "indirect block full");
		*ibc = 0;
	}
}

void s5fs_entry(S5FS *fs, int *dbc, uint8_t *db, int *ibc, int32_t *ib,
                uint16_t inum, const char *name)
{
	uint8_t *dp = db + (size_t)*dbc * P11_DIRENTSZ;
	int i;

	fs->bo->put16(dp, inum);
	memset(dp + 2, 0, P11_DIRSIZ);
	for (i = 0; i < P11_DIRSIZ && name[i]; i++)
		dp[2 + i] = (uint8_t)name[i];
	(*dbc)++;
	if ((uint32_t)*dbc >= fs->ndirect)
		s5fs_newblk(fs, dbc, db, ibc, ib);
}

/* ------------------------------------------------------------------ *
 * inode writer  (mkfs iput)
 * ------------------------------------------------------------------ */

void s5fs_iput(S5FS *fs, s5fs_inode *in, int ibc, int32_t *ib)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t d, off;
	uint8_t *dp;
	int i;

	fs->s_tinode--;
	d = s5fs_itod(fs, in->number);
	if (d >= fs->s_isize) {
		s5fs_fail(fs, "ilist too small");
		return;
	}
	s5fs_rdblk(fs, d, buf);
	off = s5fs_itoo(fs, in->number) * P11_DINODESZ;
	dp = buf + off;

	fs->bo->put16(dp + P11_DI_MODE,  in->mode);
	fs->bo->put16(dp + P11_DI_NLINK, (uint16_t)in->nlink);
	fs->bo->put16(dp + P11_DI_UID,   (uint16_t)in->uid);
	fs->bo->put16(dp + P11_DI_GID,   (uint16_t)in->gid);
	fs->bo->put32(dp + P11_DI_SIZE,  (uint32_t)in->size);

	switch (in->mode & P11_IFMT) {
	case P11_IFDIR:
	case P11_IFREG:
		for (i = 0; i < ibc; i++) {
			if ((uint32_t)i >= fs->laddr)
				break;
			in->addr[i] = ib[i];
		}
		if ((uint32_t)ibc >= fs->laddr) {
			/* one level of single indirection, like mkfs */
			uint8_t iblk[P11_MAXBSIZE];
			int j;

			in->addr[fs->laddr] = s5fs_alloc(fs);
			for (j = 0; (uint32_t)j < fs->nindir - fs->laddr; j++) {
				ib[j] = ib[j + fs->laddr];
				ib[j + fs->laddr] = 0;
			}
			memset(iblk, 0, fs->bsize);
			for (j = 0; (uint32_t)j < fs->nindir; j++)
				fs->bo->put32(iblk + 4 * j, (uint32_t)ib[j]);
			s5fs_wtblk(fs, (uint32_t)in->addr[fs->laddr], iblk);
		}
		/* FALLTHROUGH -- pack the addresses, as mkfs does */
	case P11_IFBLK:
	case P11_IFCHR:
		for (i = 0; (uint32_t)i < fs->naddr; i++)
			fs->bo->put24(dp + P11_DI_ADDR + 3 * i, (uint32_t)in->addr[i]);
		break;
	default:
		s5fs_fail(fs, "bad inode mode");
		return;
	}

	fs->bo->put32(dp + P11_DI_ATIME, (uint32_t)(in->atime ? in->atime : fs->s_time));
	fs->bo->put32(dp + P11_DI_MTIME, (uint32_t)(in->mtime ? in->mtime : fs->s_time));
	fs->bo->put32(dp + P11_DI_CTIME, (uint32_t)(in->ctime ? in->ctime : fs->s_time));

	s5fs_wtblk(fs, d, buf);
}

/* ------------------------------------------------------------------ *
 * general inode primitives (for front-ends: tree/dump/tar -> dsk)
 *
 * Unlike mkfs's iput (direct + one single-indirect, sized to its tiny files),
 * these build the full direct + single/double/triple indirect map, so a
 * front-end can store real files of any size.  s5fs_iput stays as-is so the
 * empty-fs mkfs path still reproduces native mkfs byte-for-byte.
 * ------------------------------------------------------------------ */

/* next free inode number (1-based); caller ensures the i-list is big enough */
uint32_t s5fs_ialloc(S5FS *fs)
{
	return ++fs->ino;
}

/* Build one indirect block at `level` (1=single,2=double,3=triple) mapping
 * `count` data blocks from da[]; returns the indirect block's number. */
static int32_t s5fs_build_ind(S5FS *fs, const int32_t *da, uint32_t count, int level)
{
	uint8_t *blk;
	int32_t bno;
	uint32_t per = 1, e;
	int k;

	for (k = 1; k < level; k++)		/* data blocks covered per entry */
		per *= fs->nindir;
	blk = malloc(fs->bsize);
	if (!blk) { s5fs_fail(fs, "out of memory"); return 0; }
	memset(blk, 0, fs->bsize);
	bno = s5fs_alloc(fs);
	for (e = 0; e * per < count && e < fs->nindir; e++) {
		uint32_t off = e * per;
		uint32_t sub = count - off;
		int32_t child;

		if (sub > per) sub = per;
		child = (level == 1) ? da[off]
		                     : s5fs_build_ind(fs, da + off, sub, level - 1);
		fs->bo->put32(blk + 4 * e, (uint32_t)child);
	}
	s5fs_wtblk(fs, (uint32_t)bno, blk);
	free(blk);
	return bno;
}

/* Install the physical block numbers da[0..n-1] into inode `in` as its file
 * map: laddr direct slots, then single/double/triple indirect. */
int s5fs_setblocks(S5FS *fs, s5fs_inode *in, const int32_t *da, uint32_t n)
{
	uint32_t i, per = fs->nindir, cap, c;
	const int32_t *p = da;
	uint32_t rest = n;

	for (i = 0; i < fs->naddr; i++)
		in->addr[i] = 0;

	for (i = 0; i < fs->laddr && rest; i++) {	/* direct */
		in->addr[i] = *p++;
		rest--;
	}
	if (!rest) return 0;

	cap = per;					/* single indirect */
	c = rest < cap ? rest : cap;
	in->addr[fs->laddr] = s5fs_build_ind(fs, p, c, 1);
	p += c; rest -= c;
	if (!rest) return 0;

	cap = per * per;				/* double indirect */
	c = rest < cap ? rest : cap;
	in->addr[fs->laddr + 1] = s5fs_build_ind(fs, p, c, 2);
	p += c; rest -= c;
	if (!rest) return 0;

	cap = per * per * per;				/* triple indirect */
	c = rest < cap ? rest : cap;
	in->addr[fs->laddr + 2] = s5fs_build_ind(fs, p, c, 3);
	rest -= c;
	if (rest) { s5fs_fail(fs, "file too large for filesystem"); return -1; }
	return 0;
}

/* Serialise inode `in` (addresses already final via s5fs_setblocks). */
void s5fs_writeinode(S5FS *fs, const s5fs_inode *in)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t d, off, i;
	uint8_t *dp;

	fs->s_tinode--;
	d = s5fs_itod(fs, in->number);
	if (d >= fs->s_isize) {
		s5fs_fail(fs, "ilist too small");
		return;
	}
	s5fs_rdblk(fs, d, buf);
	off = s5fs_itoo(fs, in->number) * P11_DINODESZ;
	dp = buf + off;

	fs->bo->put16(dp + P11_DI_MODE,  in->mode);
	fs->bo->put16(dp + P11_DI_NLINK, (uint16_t)in->nlink);
	fs->bo->put16(dp + P11_DI_UID,   (uint16_t)in->uid);
	fs->bo->put16(dp + P11_DI_GID,   (uint16_t)in->gid);
	fs->bo->put32(dp + P11_DI_SIZE,  (uint32_t)in->size);
	for (i = 0; i < fs->naddr; i++)
		fs->bo->put24(dp + P11_DI_ADDR + 3 * i, (uint32_t)in->addr[i]);
	fs->bo->put32(dp + P11_DI_ATIME, (uint32_t)(in->atime ? in->atime : fs->s_time));
	fs->bo->put32(dp + P11_DI_MTIME, (uint32_t)(in->mtime ? in->mtime : fs->s_time));
	fs->bo->put32(dp + P11_DI_CTIME, (uint32_t)(in->ctime ? in->ctime : fs->s_time));

	s5fs_wtblk(fs, d, buf);
}

/* Increment the on-disk link count of an already-written inode (for a hard
 * link: a second directory entry pointing at an existing inode). */
void s5fs_bumplink(S5FS *fs, uint32_t ino)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t d;
	uint8_t *dp;

	d = s5fs_itod(fs, ino);
	if (d >= fs->s_isize) {
		s5fs_fail(fs, "ilist too small");
		return;
	}
	s5fs_rdblk(fs, d, buf);
	dp = buf + s5fs_itoo(fs, ino) * P11_DINODESZ;
	fs->bo->put16(dp + P11_DI_NLINK,
	              (uint16_t)(fs->bo->get16(dp + P11_DI_NLINK) + 1));
	s5fs_wtblk(fs, d, buf);
}

/* ------------------------------------------------------------------ *
 * free list construction  (mkfs bflist)
 * ------------------------------------------------------------------ */

void s5fs_freelist(S5FS *fs)
{
	s5fs_inode in;
	int32_t ib[P11_MAXNINDIR];
	int ibc = 0;
	uint8_t flg[P11_MAXFN];
	int32_t d, f;
	int i, j, fn = fs->s_n, fm = fs->s_m;

	/* rotational permutation of block offsets within a rotation group */
	for (i = 0; i < fn; i++)
		flg[i] = 0;
	i = 0;
	for (j = 0; j < fn; j++) {
		while (flg[i])
			i = (i + 1) % fn;
		fs->adr[j] = i + 1;
		flg[i]++;
		i = (i + fm) % fn;
	}

	/* the reserved bad-block inode holds no data here */
	fs->ino++;				/* inode 1 */
	memset(&in, 0, sizeof in);
	in.number = (uint16_t)fs->ino;
	in.mode = P11_IFREG;

	memset(ib, 0, sizeof ib);

	s5fs_bfree(fs, 0);			/* block 0 terminates the list */
	d = (int32_t)fs->s_fsize - 1;
	while (d % fn)
		d++;
	for (; d > 0; d -= fn)
		for (i = 0; i < fn; i++) {
			f = d - fs->adr[i];
			if (f < (int32_t)fs->s_fsize && f >= (int32_t)fs->s_isize)
				s5fs_bfree(fs, f);	/* badblk() is always 0 */
		}
	s5fs_iput(fs, &in, ibc, ib);
}

/* ------------------------------------------------------------------ *
 * root + lost+found  (mkfs cfile size-mode + mklost)
 * ------------------------------------------------------------------ */

static void s5fs_mklost(S5FS *fs, s5fs_inode *par)
{
	s5fs_inode in;
	uint8_t db[P11_MAXBSIZE];
	int32_t ib[P11_MAXNINDIR];
	int dbc = 0, ibc = 0, i, n;

	memset(&in, 0, sizeof in);
	memset(db, 0, fs->bsize);
	memset(ib, 0, sizeof ib);

	in.mode = P11_IFDIR | P11_ISVTX | 0777;
	in.number = (uint16_t)(++fs->ino);		/* inode 3 */
	in.nlink = 2;
	n = (int)fs->naddr - 4 + 1;			/* pre-allocated slots */
	in.size = (int32_t)(fs->bsize * (uint32_t)n);
	par->nlink++;

	s5fs_entry(fs, &dbc, db, &ibc, ib, in.number, ".");
	s5fs_entry(fs, &dbc, db, &ibc, ib, par->number, "..");
	for (i = 0; i < n; i++)
		s5fs_newblk(fs, &dbc, db, &ibc, ib);
	s5fs_iput(fs, &in, ibc, ib);
}

static void s5fs_root(S5FS *fs)
{
	s5fs_inode in;
	uint8_t db[P11_MAXBSIZE];
	int32_t ib[P11_MAXNINDIR];
	int dbc = 0, ibc = 0;

	memset(&in, 0, sizeof in);
	memset(db, 0, fs->bsize);
	memset(ib, 0, sizeof ib);

	in.number = (uint16_t)(++fs->ino);		/* inode 2 */
	in.mode = P11_IFDIR | 0777;

	/* mkfs's link dance for the root, whose parent is itself */
	in.nlink = 1;
	in.nlink--;			/* par == 0 branch */
	in.nlink++;			/* IFDIR: par->i_nlink++ (par is self) */
	in.nlink++;			/* IFDIR: in.i_nlink++ */

	s5fs_entry(fs, &dbc, db, &ibc, ib, in.number, ".");
	s5fs_entry(fs, &dbc, db, &ibc, ib, in.number, "..");
	in.size = 2 * P11_DIRENTSZ;

	/* size-mode proto is "d--777 0 0 $": no children before lost+found */
	s5fs_entry(fs, &dbc, db, &ibc, ib, (uint16_t)(fs->ino + 1), "lost+found");
	in.size += P11_DIRENTSZ;
	s5fs_mklost(fs, &in);

	if (dbc != 0)
		s5fs_newblk(fs, &dbc, db, &ibc, ib);
	s5fs_iput(fs, &in, ibc, ib);
}

/* ------------------------------------------------------------------ *
 * setup / teardown
 * ------------------------------------------------------------------ */

int s5fs_begin(S5FS *fs, int fd, uint32_t nblocks, const s5fs_opts *opts)
{
	s5fs_opts def = {0};
	uint32_t n, ninodeblks;
	uint8_t zero[P11_MAXBSIZE];

	if (!opts)
		opts = &def;

	memset(fs, 0, sizeof *fs);
	fs->fd = fd;
	fs->base = opts->base;

	/* on-disk byte order (default S5_PDP11 == 0) */
	fs->bo = s5_codec_for(opts->endian);
	if (!fs->bo) {
		s5fs_fail(fs, "bad byte-order profile");
		return -1;
	}

	/* block-size profile */
	fs->bsize = opts->bsize ? opts->bsize : 1024;
	if (fs->bsize != 512 && fs->bsize != 1024) {
		s5fs_fail(fs, "bsize must be 512 or 1024");
		return -1;
	}
	fs->clsize  = fs->bsize / 512;
	fs->inopb   = fs->bsize / P11_DINODESZ;			/* 8 or 16 */
	fs->naddr   = (fs->bsize == 1024) ? 7 : 13;		/* UCB_NKB */
	fs->laddr   = fs->naddr - 3;

	/* block numbers are 3-byte (24-bit) in the inode -- anything beyond 2^24
	 * blocks would be silently truncated by the l3 packing, so refuse. */
	if (nblocks > P11_MAXFSBLKS) {
		snprintf(fs->err, sizeof fs->err,
		    "too large: %lu blocks; s5fs max is 2^24 = 16777216 blocks "
		    "(inode addresses are 3-byte; 16G at 1K, 8G at 512B)",
		    (unsigned long)nblocks);
		fs->error = 1;
		return -1;
	}
	fs->nindir  = fs->bsize / 4;
	fs->ndirect = fs->bsize / P11_DIRENTSZ;

	if (nblocks < 8) {
		s5fs_fail(fs, "filesystem too small");
		return -1;
	}
	fs->s_fsize = nblocks;

	/* interleave (mkfs defaults m=5, n=10; clamp like mkfs) */
	fs->s_m = opts->m ? (int16_t)opts->m : 5;
	fs->s_n = opts->n ? (int16_t)opts->n : 10;
	if (fs->s_n <= 0 || fs->s_n >= P11_MAXFN)
		fs->s_n = P11_MAXFN;
	if (fs->s_m <= 0 || fs->s_m > fs->s_n)
		fs->s_m = 3;

	fs->s_time = (opts->mtime < 0) ? (int32_t)time(NULL)
	                               : (int32_t)opts->mtime;

	/* i-list size: mkfs's size-mode heuristic, or an explicit minimum. */
	if (opts->ninode) {
		ninodeblks = (opts->ninode + fs->inopb - 1) / fs->inopb;
	} else {
		n = nblocks;
		if (n <= 5000u / fs->clsize)
			n /= 50;
		else
			n /= 25;
		if (n == 0)
			n = 1;
		if (n > 65500u / fs->inopb)
			n = 65500u / fs->inopb;
		ninodeblks = n;
	}
	fs->s_isize = ninodeblks + 2;
	if (fs->s_isize >= fs->s_fsize) {
		s5fs_fail(fs, "bad size/inode ratio");
		return -1;
	}

	/* materialise the volume so unallocated data blocks read as 0.  For a bare
	 * image size it exactly; for a partition (base != 0) the file is a larger
	 * whole disk the caller already sized -- only grow it, never shrink. */
	if (fs->base == 0) {
		if (ftruncate(fd, (off_t)nblocks * fs->bsize) < 0) {
			s5fs_fail(fs, "cannot size image");
			return -1;
		}
	} else {
		off_t need = fs->base + (off_t)nblocks * fs->bsize;
		struct stat st;
		if (fstat(fd, &st) == 0 && st.st_size < need && ftruncate(fd, need) < 0) {
			s5fs_fail(fs, "cannot size image");
			return -1;
		}
	}

	/* zero the i-list (blocks 2 .. isize-1) and count total free inodes */
	memset(zero, 0, fs->bsize);
	for (n = P11_ILISTSTART; n < fs->s_isize; n++) {
		s5fs_wtblk(fs, n, zero);
		fs->s_tinode += (int32_t)fs->inopb;
	}
	fs->ino = 0;
	return fs->error ? -1 : 0;
}

int s5fs_mount(S5FS *fs, int fd, uint32_t bsize, const s5_codec *bo, int64_t base)
{
	uint8_t sb[P11_MAXBSIZE];
	int i;

	memset(fs, 0, sizeof *fs);
	fs->fd = fd;
	fs->base = base;
	fs->bo = bo;
	fs->bsize = bsize ? bsize : 1024;
	if (fs->bsize != 512 && fs->bsize != 1024) { s5fs_fail(fs, "bsize must be 512 or 1024"); return -1; }
	fs->clsize  = fs->bsize / 512;
	fs->inopb   = fs->bsize / P11_DINODESZ;
	fs->naddr   = (fs->bsize == 1024) ? 7 : 13;
	fs->laddr   = fs->naddr - 3;
	fs->nindir  = fs->bsize / 4;
	fs->ndirect = fs->bsize / P11_DIRENTSZ;

	s5fs_rdblk(fs, P11_SUPERBLK, sb);
	if (fs->error) return -1;
	fs->s_isize  = bo->get16(sb + P11_SB_ISIZE);
	fs->s_fsize  = bo->get32(sb + P11_SB_FSIZE);
	fs->s_nfree  = (int16_t)bo->get16(sb + P11_SB_NFREE);
	for (i = 0; i < P11_NICFREE; i++)
		fs->s_free[i] = (int32_t)bo->get32(sb + P11_SB_FREE + 4 * i);
	fs->s_ninode = (int16_t)bo->get16(sb + P11_SB_NINODE);
	for (i = 0; i < P11_NICINOD; i++)
		fs->s_inode[i] = bo->get16(sb + P11_SB_INODE + 2 * i);
	fs->s_tfree  = (int32_t)bo->get32(sb + P11_SB_TFREE);
	fs->s_tinode = (int16_t)bo->get16(sb + P11_SB_TINODE);
	fs->s_time   = (int32_t)bo->get32(sb + P11_SB_TIME);
	fs->s_m = (int16_t)bo->get16(sb + P11_SB_DINFO);
	fs->s_n = (int16_t)bo->get16(sb + P11_SB_DINFO + 2);
	if (fs->s_isize < P11_ILISTSTART || fs->s_isize >= fs->s_fsize) {
		s5fs_fail(fs, "bad superblock");
		return -1;
	}
	return 0;
}

void s5fs_finish(S5FS *fs)
{
	uint8_t sb[P11_MAXBSIZE];
	int i;

	memset(sb, 0, fs->bsize);
	fs->bo->put16(sb + P11_SB_ISIZE, (uint16_t)fs->s_isize);
	fs->bo->put32(sb + P11_SB_FSIZE, (uint32_t)fs->s_fsize);
	fs->bo->put16(sb + P11_SB_NFREE, (uint16_t)fs->s_nfree);
	for (i = 0; i < P11_NICFREE; i++)
		fs->bo->put32(sb + P11_SB_FREE + 4 * i, (uint32_t)fs->s_free[i]);
	fs->bo->put16(sb + P11_SB_NINODE, (uint16_t)fs->s_ninode);
	for (i = 0; i < P11_NICINOD; i++)
		fs->bo->put16(sb + P11_SB_INODE + 2 * i, fs->s_inode[i]);
	/* s_flock/s_ilock/s_fmod/s_ronly stay 0 */
	fs->bo->put32(sb + P11_SB_TIME,   (uint32_t)fs->s_time);
	fs->bo->put32(sb + P11_SB_TFREE,  (uint32_t)fs->s_tfree);
	fs->bo->put16(sb + P11_SB_TINODE, (uint16_t)fs->s_tinode);
	fs->bo->put16(sb + P11_SB_DINFO + 0, (uint16_t)fs->s_m);
	fs->bo->put16(sb + P11_SB_DINFO + 2, (uint16_t)fs->s_n);
	/* s_fsmnt/s_lasti/s_nbehind stay 0 */

	s5fs_wtblk(fs, P11_SUPERBLK, sb);
}

int s5fs_mkfs(S5FS *fs, int fd, uint32_t nblocks, const s5fs_opts *opts)
{
	if (s5fs_begin(fs, fd, nblocks, opts) < 0)
		return -1;

	fs->s_tfree = 0;
	s5fs_freelist(fs);	/* inode 1 + the whole free list */
	s5fs_root(fs);		/* inode 2 (root) + inode 3 (lost+found) */
	s5fs_finish(fs);	/* superblock */

	return fs->error ? -1 : 0;
}

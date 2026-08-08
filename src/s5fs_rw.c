/*
 * s5fs_rw.c -- the shared s5fs file-mutation engine.  See s5fs_rw.h.
 *
 * The directory-slot, inode-allocation, and block-map logic here was proven in
 * the FUSE read-write layer; it is factored out so the batch commands and the
 * interactive shell (which must work with no FUSE) run the same code.
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_rw.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------ *
 * open / close
 * ------------------------------------------------------------------ */

int
rw_open(RW *h, const char *path, uint32_t bsize, int forced_bo, int writable, int64_t base)
{
	memset(h, 0, sizeof *h);
	if (fsr_open(&h->r, path, bsize, forced_bo, base) < 0)
		return -1;
	if (writable) {
		int wfd = open(path, O_RDWR);
		if (wfd < 0 || s5fs_mount(&h->w, wfd, h->r.bsize, h->r.bo, base) < 0) {
			if (wfd >= 0)
				close(wfd);
			fsr_close(&h->r);
			return -1;
		}
		h->writable = 1;
	}
	return 0;
}

void
rw_sync(RW *h)
{
	if (h->writable)
		s5fs_finish(&h->w);
}

void
rw_close(RW *h)
{
	if (h->writable) {
		s5fs_finish(&h->w);
		close(h->w.fd);
	}
	fsr_close(&h->r);
	h->writable = 0;
}

uint32_t
rw_namei(RW *h, const char *path)
{
	return fsr_namei(&h->r, path);
}

int
rw_iget(RW *h, uint32_t ino, fsr_inode *o)
{
	return fsr_iget(&h->r, ino, o);
}

/* ------------------------------------------------------------------ *
 * mid-level primitives (writer side)
 * ------------------------------------------------------------------ */

void
rw_ino_loc(RW *h, uint32_t ino, uint32_t *blk, uint32_t *off)
{
	*blk = (ino + 2 * h->w.inopb - 1) / h->w.inopb;
	*off = ((ino + 2 * h->w.inopb - 1) % h->w.inopb) * P11_DINODESZ;
}

static uint32_t
per_at(RW *h, int levels)
{
	uint32_t p = 1;
	int k;
	for (k = 1; k < levels; k++)
		p *= h->w.nindir;
	return p;
}

static uint32_t
ind_map(RW *h, uint32_t iblk, uint32_t idx, int levels, int alloc)
{
	uint8_t ib[P11_MAXBSIZE];
	uint32_t phys;

	s5fs_rdblk(&h->w, iblk, ib);
	if (levels == 1) {
		phys = h->w.bo->get32(ib + 4 * idx);
		if (!phys && alloc) {
			phys = (uint32_t)s5fs_alloc(&h->w);
			h->w.bo->put32(ib + 4 * idx, phys);
			s5fs_wtblk(&h->w, iblk, ib);
		}
		return phys;
	}
	{
		uint32_t per = per_at(h, levels), e = idx / per;
		uint32_t child = h->w.bo->get32(ib + 4 * e);
		if (!child) {
			uint8_t z[P11_MAXBSIZE];
			if (!alloc)
				return 0;
			child = (uint32_t)s5fs_alloc(&h->w);
			memset(z, 0, h->w.bsize);
			s5fs_wtblk(&h->w, child, z);
			h->w.bo->put32(ib + 4 * e, child);
			s5fs_wtblk(&h->w, iblk, ib);
		}
		return ind_map(h, child, idx % per, levels - 1, alloc);
	}
}

uint32_t
rw_bmap(RW *h, uint8_t *ds, uint32_t lbn, int alloc, int *dirty)
{
	uint32_t per = h->w.nindir, iblk, slot, idx;
	int lvl;

	if (lbn < h->w.laddr) {
		uint32_t phys = h->w.bo->get24(ds + P11_DI_ADDR + 3 * lbn);
		if (!phys && alloc) {
			phys = (uint32_t)s5fs_alloc(&h->w);
			h->w.bo->put24(ds + P11_DI_ADDR + 3 * lbn, phys);
			*dirty = 1;
		}
		return phys;
	}
	lbn -= h->w.laddr;
	if (lbn < per) {
		lvl = 1;
		slot = h->w.laddr;
		idx = lbn;
	}
	else if (lbn < per * per) {
		lbn -= per;
		lvl = 2;
		slot = h->w.laddr + 1;
		idx = lbn;
	}
	else if (lbn < per * per * per) {
		lbn -= per * per;
		lvl = 3;
		slot = h->w.laddr + 2;
		idx = lbn;
	}
	else
		return 0;

	iblk = h->w.bo->get24(ds + P11_DI_ADDR + 3 * slot);
	if (!iblk) {
		uint8_t z[P11_MAXBSIZE];
		if (!alloc)
			return 0;
		iblk = (uint32_t)s5fs_alloc(&h->w);
		memset(z, 0, h->w.bsize);
		s5fs_wtblk(&h->w, iblk, z);
		h->w.bo->put24(ds + P11_DI_ADDR + 3 * slot, iblk);
		*dirty = 1;
	}
	return ind_map(h, iblk, idx, lvl, alloc);
}

static void
bfree_at(RW *h, uint8_t *ds, uint32_t lbn)
{
	uint32_t per = h->w.nindir, slot, idx, iblk, b;
	uint8_t ib[P11_MAXBSIZE];
	int lvl, L;

	if (lbn < h->w.laddr) {
		b = h->w.bo->get24(ds + P11_DI_ADDR + 3 * lbn);
		if (b) {
			s5fs_bfree(&h->w, (int32_t)b);
			h->w.bo->put24(ds + P11_DI_ADDR + 3 * lbn, 0);
		}
		return;
	}
	lbn -= h->w.laddr;
	if (lbn < per) {
		lvl = 1;
		slot = h->w.laddr;
		idx = lbn;
	}
	else if (lbn < per * per) {
		lbn -= per;
		lvl = 2;
		slot = h->w.laddr + 1;
		idx = lbn;
	}
	else if (lbn < per * per * per) {
		lbn -= per * per;
		lvl = 3;
		slot = h->w.laddr + 2;
		idx = lbn;
	}
	else
		return;
	iblk = h->w.bo->get24(ds + P11_DI_ADDR + 3 * slot);
	for (L = lvl; L > 1 && iblk; L--) {
		uint32_t d = per_at(h, L), e;
		s5fs_rdblk(&h->w, iblk, ib);
		e = idx / d;
		iblk = h->w.bo->get32(ib + 4 * e);
		idx %= d;
	}
	if (!iblk)
		return;
	s5fs_rdblk(&h->w, iblk, ib);
	b = h->w.bo->get32(ib + 4 * idx);
	if (b) {
		s5fs_bfree(&h->w, (int32_t)b);
		h->w.bo->put32(ib + 4 * idx, 0);
		s5fs_wtblk(&h->w, iblk, ib);
	}
}

/* A directory entry holds exactly P11_DIRSIZ name bytes and no terminator, so a
 * longer name simply cannot be represented.  Truncating it silently is worse
 * than refusing: the write path would store the first 14 bytes while namei
 * still compares the full name, so the file would be unreachable under the name
 * the caller used, a second such name would produce a DUPLICATE on-disk entry,
 * and fsck would call the result clean.  Refuse instead. */
static int
name_ok(const char *leaf)
{
	size_t l = strlen(leaf);

	return l > 0 && l <= P11_DIRSIZ;
}

/* parent directory inode of `path`, with the leaf name copied to `leaf` */
static uint32_t
parent_of(RW *h, const char *path, char *leaf, size_t leafsz)
{
	char buf[2048], *slash;
	strncpy(buf, path, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	slash = strrchr(buf, '/');
	if (!slash)
		return 0;
	strncpy(leaf, slash + 1, leafsz - 1);
	leaf[leafsz - 1] = '\0';
	if (slash == buf)
		return fsr_namei(&h->r, "/");
	*slash = '\0';
	return fsr_namei(&h->r, buf);
}

static uint32_t
ialloc_scan(RW *h)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, e;
	for (blk = P11_ILISTSTART; blk < h->w.s_isize; blk++) {
		s5fs_rdblk(&h->w, blk, pb);
		for (e = 0; e < h->w.inopb; e++) {
			uint32_t ino = (blk - 2) * h->w.inopb + e + 1;
			if (ino < P11_ROOTINO)
				continue;
			if (h->w.bo->get16(pb + e * P11_DINODESZ + P11_DI_MODE) == 0)
				return ino;
		}
	}
	return 0;
}

static void
write_new_inode(RW *h, uint32_t ino, uint16_t mode, uint32_t addr0,
		int16_t nlink, int32_t size)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, off;
	uint8_t *ds;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	memset(ds, 0, P11_DINODESZ);
	h->w.bo->put16(ds + P11_DI_MODE, mode);
	h->w.bo->put16(ds + P11_DI_NLINK, (uint16_t)nlink);
	h->w.bo->put32(ds + P11_DI_SIZE, (uint32_t)size);
	if (addr0)
		h->w.bo->put24(ds + P11_DI_ADDR, addr0);
	h->w.bo->put32(ds + P11_DI_ATIME, (uint32_t)h->w.s_time);
	h->w.bo->put32(ds + P11_DI_MTIME, (uint32_t)h->w.s_time);
	h->w.bo->put32(ds + P11_DI_CTIME, (uint32_t)h->w.s_time);
	s5fs_wtblk(&h->w, blk, pb);
	h->w.s_tinode--;
}

static int
dir_add(RW *h, uint32_t pino, const char *name, uint32_t cino)
{
	uint8_t pb[P11_MAXBSIZE], db[P11_MAXBSIZE];
	uint32_t pblk, poff, psize, nblk, b, e, dbn, deo;
	uint8_t *ds;
	int dirty = 0;
	size_t l = strlen(name);

	if (!name_ok(name)) /* defence in depth; callers check first */
		return -ENAMETOOLONG;

	rw_ino_loc(h, pino, &pblk, &poff);
	s5fs_rdblk(&h->w, pblk, pb);
	ds = pb + poff;
	psize = h->w.bo->get32(ds + P11_DI_SIZE);
	nblk = (psize + h->w.bsize - 1) / h->w.bsize;

	for (b = 0; b < nblk; b++) {
		uint32_t phys = rw_bmap(h, ds, b, 0, &dirty);
		if (!phys)
			continue;
		s5fs_rdblk(&h->w, phys, db);
		for (e = 0; e < h->w.ndirect; e++) {
			if ((b * h->w.ndirect + e) * P11_DIRENTSZ >= psize)
				break;
			if (h->w.bo->get16(db + e * P11_DIRENTSZ) == 0) {
				h->w.bo->put16(db + e * P11_DIRENTSZ, (uint16_t)cino);
				memset(db + e * P11_DIRENTSZ + 2, 0, P11_DIRSIZ);
				memcpy(db + e * P11_DIRENTSZ + 2, name, l);
				s5fs_wtblk(&h->w, phys, db);
				return 0;
			}
		}
	}
	dbn = psize / h->w.bsize;
	deo = psize % h->w.bsize;
	{
		uint32_t phys = rw_bmap(h, ds, dbn, 1, &dirty);
		if (!phys)
			return -ENOSPC;
		s5fs_rdblk(&h->w, phys, db);
		h->w.bo->put16(db + deo, (uint16_t)cino);
		memset(db + deo + 2, 0, P11_DIRSIZ);
		memcpy(db + deo + 2, name, l);
		s5fs_wtblk(&h->w, phys, db);
	}
	h->w.bo->put32(ds + P11_DI_SIZE, psize + P11_DIRENTSZ);
	s5fs_wtblk(&h->w, pblk, pb);
	return 0;
}

static uint32_t
dir_remove(RW *h, uint32_t pino, const char *name)
{
	uint8_t pb[P11_MAXBSIZE], db[P11_MAXBSIZE];
	uint32_t pblk, poff, psize, nblk, b, e;
	uint8_t *ds;
	int dirty = 0;

	rw_ino_loc(h, pino, &pblk, &poff);
	s5fs_rdblk(&h->w, pblk, pb);
	ds = pb + poff;
	psize = h->w.bo->get32(ds + P11_DI_SIZE);
	nblk = (psize + h->w.bsize - 1) / h->w.bsize;
	for (b = 0; b < nblk; b++) {
		uint32_t phys = rw_bmap(h, ds, b, 0, &dirty);
		if (!phys)
			continue;
		s5fs_rdblk(&h->w, phys, db);
		for (e = 0; e < h->w.ndirect; e++) {
			uint8_t *d = db + e * P11_DIRENTSZ;
			uint16_t di;
			char nm[P11_DIRSIZ + 1];
			if ((b * h->w.ndirect + e) * P11_DIRENTSZ >= psize)
				break;
			di = h->w.bo->get16(d);
			if (di == 0)
				continue;
			memcpy(nm, d + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (strcmp(nm, name) == 0) {
				h->w.bo->put16(d, 0);
				s5fs_wtblk(&h->w, phys, db);
				return di;
			}
		}
	}
	return 0;
}

static void
ind_free(RW *h, uint32_t iblk, int levels)
{
	uint8_t ib[P11_MAXBSIZE];
	uint32_t i;
	if (!iblk)
		return;
	s5fs_rdblk(&h->w, iblk, ib);
	for (i = 0; i < h->w.nindir; i++) {
		uint32_t b = h->w.bo->get32(ib + 4 * i);
		if (!b)
			continue;
		if (levels == 1)
			s5fs_bfree(&h->w, (int32_t)b);
		else
			ind_free(h, b, levels - 1);
	}
	s5fs_bfree(&h->w, (int32_t)iblk);
}

static void
ifree_blocks(RW *h, uint8_t *ds)
{
	uint32_t i;
	for (i = 0; i < h->w.laddr; i++) {
		uint32_t b = h->w.bo->get24(ds + P11_DI_ADDR + 3 * i);
		if (b)
			s5fs_bfree(&h->w, (int32_t)b);
	}
	ind_free(h, h->w.bo->get24(ds + P11_DI_ADDR + 3 * (h->w.laddr + 0)), 1);
	ind_free(h, h->w.bo->get24(ds + P11_DI_ADDR + 3 * (h->w.laddr + 1)), 2);
	ind_free(h, h->w.bo->get24(ds + P11_DI_ADDR + 3 * (h->w.laddr + 2)), 3);
}

static int
nonempty_cb(void *arg, uint32_t ino, const char *name)
{
	(void)ino;
	if (strcmp(name, ".") && strcmp(name, "..")) {
		*(int *)arg = 1;
		return 1;
	}
	return 0;
}

static void
bump_nlink(RW *h, uint32_t ino, int delta)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, off;
	uint8_t *ds;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	h->w.bo->put16(ds + P11_DI_NLINK, (uint16_t)(h->w.bo->get16(ds + P11_DI_NLINK) + delta));
	s5fs_wtblk(&h->w, blk, pb);
}

static void
set_dotdot(RW *h, uint32_t dino, uint32_t newparent)
{
	uint8_t pb[P11_MAXBSIZE], db[P11_MAXBSIZE];
	uint32_t blk, off, phys;
	uint8_t *ds;
	int d = 0;
	rw_ino_loc(h, dino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	phys = rw_bmap(h, ds, 0, 0, &d);
	if (!phys)
		return;
	s5fs_rdblk(&h->w, phys, db);
	h->w.bo->put16(db + 1 * P11_DIRENTSZ, (uint16_t)newparent);
	s5fs_wtblk(&h->w, phys, db);
}

static int
dotdot_cb(void *arg, uint32_t ino, const char *name)
{
	if (strcmp(name, "..") == 0) {
		*(uint32_t *)arg = ino;
		return 1;
	}
	return 0;
}

/* Is `anc` equal to, or an ancestor of, directory `dino`?  Walks ".." to the
 * root.
 *
 * Renaming a directory into its own subtree has to be refused (POSIX gives it
 * EINVAL): the subtree would keep its own internal links but nothing would
 * reference it from the root any more, so the whole thing silently detaches --
 * fsck then reports every inode under it as an orphan, and reconnecting them
 * leaves a directory cycle behind.  The walk is bounded, so a cycle that is
 * ALREADY on disk cannot spin here; it just answers "yes" and the rename is
 * refused, which is the safe direction. */
static int
is_ancestor(RW *h, uint32_t anc, uint32_t dino)
{
	uint32_t cur = dino, guard;

	for (guard = 0; guard <= h->r.isize * h->r.inopb; guard++) {
		fsr_inode in;
		uint32_t up = 0;

		if (cur == anc)
			return 1;
		if (cur == P11_ROOTINO || cur == 0)
			return 0;
		if (fsr_iget(&h->r, cur, &in) < 0)
			return 0;
		fsr_readdir(&h->r, &in, dotdot_cb, &up);
		if (!up || up == cur)
			return 0;
		cur = up;
	}
	return 1; /* ran long: assume a cycle and refuse */
}

static void
unref_inode(RW *h, uint32_t ino, int is_dir, uint32_t parent)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, off;
	uint8_t *ds;
	int16_t nl;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	if (is_dir) {
		ifree_blocks(h, ds);
		memset(ds, 0, P11_DINODESZ);
		h->w.s_tinode++;
		bump_nlink(h, parent, -1);
	}
	else {
		nl = (int16_t)h->w.bo->get16(ds + P11_DI_NLINK);
		if (--nl <= 0) {
			ifree_blocks(h, ds);
			memset(ds, 0, P11_DINODESZ);
			h->w.s_tinode++;
		}
		else
			h->w.bo->put16(ds + P11_DI_NLINK, (uint16_t)nl);
	}
	s5fs_wtblk(&h->w, blk, pb);
}

/* ------------------------------------------------------------------ *
 * high-level operations
 * ------------------------------------------------------------------ */

int
rw_creat(RW *h, const char *path, unsigned perm, uint32_t *out_ino)
{
	char leaf[256];
	uint32_t pino, cino;
	int rc;
	if (!h->writable)
		return -EROFS;
	pino = parent_of(h, path, leaf, sizeof leaf);
	if (!pino)
		return -ENOENT;
	if (!name_ok(leaf)) /* check BEFORE allocating, so failure leaks nothing */
		return -ENAMETOOLONG;
	if (fsr_namei(&h->r, path))
		return -EEXIST;
	cino = ialloc_scan(h);
	if (!cino)
		return -ENOSPC;
	write_new_inode(h, cino, (uint16_t)(P11_IFREG | (perm & 07777)), 0, 1, 0);
	rc = dir_add(h, pino, leaf, cino);
	if (rc < 0)
		return rc;
	if (out_ino)
		*out_ino = cino;
	rw_sync(h);
	return 0;
}

int
rw_mkdir(RW *h, const char *path, unsigned perm)
{
	char leaf[256];
	uint32_t pino, cino, dblk;
	uint8_t db[P11_MAXBSIZE];
	int rc;
	if (!h->writable)
		return -EROFS;
	pino = parent_of(h, path, leaf, sizeof leaf);
	if (!pino)
		return -ENOENT;
	if (!name_ok(leaf)) /* check BEFORE allocating, so failure leaks nothing */
		return -ENAMETOOLONG;
	if (fsr_namei(&h->r, path))
		return -EEXIST;
	cino = ialloc_scan(h);
	if (!cino)
		return -ENOSPC;
	dblk = (uint32_t)s5fs_alloc(&h->w);
	memset(db, 0, h->w.bsize);
	h->w.bo->put16(db + 0 * P11_DIRENTSZ, (uint16_t)cino);
	db[2] = '.';
	h->w.bo->put16(db + 1 * P11_DIRENTSZ, (uint16_t)pino);
	db[P11_DIRENTSZ + 2] = '.';
	db[P11_DIRENTSZ + 3] = '.';
	s5fs_wtblk(&h->w, dblk, db);
	write_new_inode(h, cino, (uint16_t)(P11_IFDIR | (perm & 07777)), dblk, 2, 2 * P11_DIRENTSZ);
	rc = dir_add(h, pino, leaf, cino);
	if (rc < 0)
		return rc;
	bump_nlink(h, pino, +1); /* the new dir's ".." */
	rw_sync(h);
	return 0;
}

int
rw_unlink(RW *h, const char *path)
{
	char leaf[256];
	uint32_t pino, cino, blk, o;
	uint8_t pb[P11_MAXBSIZE];
	uint8_t *ds;
	fsr_inode in;
	int16_t nl;
	if (!h->writable)
		return -EROFS;
	pino = parent_of(h, path, leaf, sizeof leaf);
	if (!pino)
		return -ENOENT;
	cino = fsr_namei(&h->r, path);
	if (!cino)
		return -ENOENT;
	if (fsr_iget(&h->r, cino, &in) == 0 && (in.mode & P11_IFMT) == P11_IFDIR)
		return -EISDIR;
	if (dir_remove(h, pino, leaf) == 0)
		return -ENOENT;
	rw_ino_loc(h, cino, &blk, &o);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + o;
	nl = (int16_t)h->w.bo->get16(ds + P11_DI_NLINK);
	if (--nl <= 0) {
		ifree_blocks(h, ds);
		memset(ds, 0, P11_DINODESZ);
		h->w.s_tinode++;
	}
	else {
		h->w.bo->put16(ds + P11_DI_NLINK, (uint16_t)nl);
	}
	s5fs_wtblk(&h->w, blk, pb);
	rw_sync(h);
	return 0;
}

int
rw_rmdir(RW *h, const char *path)
{
	char leaf[256];
	uint32_t pino, cino, blk, o;
	uint8_t pb[P11_MAXBSIZE];
	uint8_t *ds;
	fsr_inode in;
	int ne = 0;
	if (!h->writable)
		return -EROFS;
	pino = parent_of(h, path, leaf, sizeof leaf);
	if (!pino)
		return -ENOENT;
	cino = fsr_namei(&h->r, path);
	if (!cino)
		return -ENOENT;
	if (fsr_iget(&h->r, cino, &in) < 0)
		return -ENOENT;
	if ((in.mode & P11_IFMT) != P11_IFDIR)
		return -ENOTDIR;
	fsr_readdir(&h->r, &in, nonempty_cb, &ne);
	if (ne)
		return -ENOTEMPTY;
	if (dir_remove(h, pino, leaf) == 0)
		return -ENOENT;
	rw_ino_loc(h, cino, &blk, &o);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + o;
	ifree_blocks(h, ds);
	memset(ds, 0, P11_DINODESZ);
	h->w.s_tinode++;
	s5fs_wtblk(&h->w, blk, pb);
	bump_nlink(h, pino, -1); /* lost the child's ".." */
	rw_sync(h);
	return 0;
}

int
rw_rename(RW *h, const char *from, const char *to)
{
	char fleaf[256], tleaf[256];
	uint32_t fpino, tpino, cino, tino;
	fsr_inode cin;
	int sdir, rc;

	if (!h->writable)
		return -EROFS;
	fpino = parent_of(h, from, fleaf, sizeof fleaf);
	if (!fpino)
		return -ENOENT;
	cino = fsr_namei(&h->r, from);
	if (!cino)
		return -ENOENT;
	tpino = parent_of(h, to, tleaf, sizeof tleaf);
	if (!tpino)
		return -ENOENT;
	if (!name_ok(tleaf))
		return -ENAMETOOLONG;
	if (fsr_iget(&h->r, cino, &cin) < 0)
		return -ENOENT;
	sdir = (cin.mode & P11_IFMT) == P11_IFDIR;
	if (sdir && is_ancestor(h, cino, tpino))
		return -EINVAL; /* would detach the subtree from the root */

	tino = fsr_namei(&h->r, to);
	if (tino) {
		fsr_inode tin;
		int tdir;
		if (tino == cino)
			return 0;
		if (fsr_iget(&h->r, tino, &tin) < 0)
			return -ENOENT;
		tdir = (tin.mode & P11_IFMT) == P11_IFDIR;
		if (tdir != sdir)
			return tdir ? -EISDIR : -ENOTDIR;
		if (tdir) {
			int ne = 0;
			fsr_readdir(&h->r, &tin, nonempty_cb, &ne);
			if (ne)
				return -ENOTEMPTY;
		}
		dir_remove(h, tpino, tleaf);
		unref_inode(h, tino, tdir, tpino);
	}
	rc = dir_add(h, tpino, tleaf, cino);
	if (rc < 0)
		return rc;
	dir_remove(h, fpino, fleaf);
	if (sdir && fpino != tpino) {
		set_dotdot(h, cino, tpino);
		bump_nlink(h, fpino, -1);
		bump_nlink(h, tpino, +1);
	}
	rw_sync(h);
	return 0;
}

int
rw_chmod(RW *h, const char *path, unsigned perm)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t ino, blk, off;
	uint8_t *ds;
	uint16_t m;
	if (!h->writable)
		return -EROFS;
	ino = fsr_namei(&h->r, path);
	if (!ino)
		return -ENOENT;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	m = h->w.bo->get16(ds + P11_DI_MODE);
	h->w.bo->put16(ds + P11_DI_MODE, (uint16_t)((m & P11_IFMT) | (perm & 07777)));
	s5fs_wtblk(&h->w, blk, pb);
	rw_sync(h);
	return 0;
}

int
rw_chown(RW *h, const char *path, int uid, int gid)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t ino, blk, off;
	uint8_t *ds;
	if (!h->writable)
		return -EROFS;
	ino = fsr_namei(&h->r, path);
	if (!ino)
		return -ENOENT;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	if (uid >= 0)
		h->w.bo->put16(ds + P11_DI_UID, (uint16_t)uid);
	if (gid >= 0)
		h->w.bo->put16(ds + P11_DI_GID, (uint16_t)gid);
	s5fs_wtblk(&h->w, blk, pb);
	rw_sync(h);
	return 0;
}

int
rw_utimes(RW *h, const char *path, int32_t atime, int32_t mtime)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t ino, blk, off;
	uint8_t *ds;
	if (!h->writable)
		return -EROFS;
	ino = fsr_namei(&h->r, path);
	if (!ino)
		return -ENOENT;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	h->w.bo->put32(ds + P11_DI_ATIME, (uint32_t)atime);
	h->w.bo->put32(ds + P11_DI_MTIME, (uint32_t)mtime);
	s5fs_wtblk(&h->w, blk, pb);
	rw_sync(h);
	return 0;
}

/* byte-range write into inode `ino`; grows the file.  Does NOT sync (callers
 * that loop should sync once at the end; single-shot callers call rw_sync). */
long
rw_pwrite(RW *h, uint32_t ino, const void *vbuf, long size, long off)
{
	const uint8_t *buf = vbuf;
	uint8_t pb[P11_MAXBSIZE], dblk[P11_MAXBSIZE];
	uint32_t blk, o;
	uint8_t *ds;
	uint16_t mode;
	int32_t fsize;
	long done = 0;
	int dirty = 0;

	if (!h->writable)
		return -EROFS;
	rw_ino_loc(h, ino, &blk, &o);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + o;
	mode = h->w.bo->get16(ds + P11_DI_MODE);
	if ((mode & P11_IFMT) == P11_IFDIR)
		return -EISDIR;
	fsize = (int32_t)h->w.bo->get32(ds + P11_DI_SIZE);
	while (done < size) {
		uint32_t lbn = (uint32_t)((off + done) / h->w.bsize);
		uint32_t boff = (uint32_t)((off + done) % h->w.bsize);
		uint32_t want = h->w.bsize - boff, phys;
		if ((long)want > size - done)
			want = (uint32_t)(size - done);
		phys = rw_bmap(h, ds, lbn, 1, &dirty);
		if (!phys)
			break;
		s5fs_rdblk(&h->w, phys, dblk);
		memcpy(dblk + boff, buf + done, want);
		s5fs_wtblk(&h->w, phys, dblk);
		done += want;
	}
	if (done == 0 && size > 0)
		return -EFBIG;
	if (off + done > fsize) {
		h->w.bo->put32(ds + P11_DI_SIZE, (uint32_t)(off + done));
		dirty = 1;
	}
	if (dirty)
		s5fs_wtblk(&h->w, blk, pb);
	return done;
}

int
rw_truncate(RW *h, uint32_t ino, long len)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, o;
	uint8_t *ds;
	uint16_t mode;
	int32_t fsize;
	if (!h->writable)
		return -EROFS;
	rw_ino_loc(h, ino, &blk, &o);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + o;
	mode = h->w.bo->get16(ds + P11_DI_MODE);
	if ((mode & P11_IFMT) == P11_IFDIR)
		return -EISDIR;
	fsize = (int32_t)h->w.bo->get32(ds + P11_DI_SIZE);
	if (len < fsize) {
		uint32_t newnblk = (uint32_t)((len + h->w.bsize - 1) / h->w.bsize);
		uint32_t oldnblk = (uint32_t)((fsize + h->w.bsize - 1) / h->w.bsize);
		uint32_t lb;
		for (lb = newnblk; lb < oldnblk; lb++)
			bfree_at(h, ds, lb);
	}
	h->w.bo->put32(ds + P11_DI_SIZE, (uint32_t)len);
	s5fs_wtblk(&h->w, blk, pb);
	rw_sync(h);
	return 0;
}

/* set an existing inode's perm + a/m times (used by put/copy) */
static void
set_meta(RW *h, uint32_t ino, unsigned perm, int32_t atime, int32_t mtime)
{
	uint8_t pb[P11_MAXBSIZE];
	uint32_t blk, off;
	uint8_t *ds;
	uint16_t m;
	rw_ino_loc(h, ino, &blk, &off);
	s5fs_rdblk(&h->w, blk, pb);
	ds = pb + off;
	m = h->w.bo->get16(ds + P11_DI_MODE);
	h->w.bo->put16(ds + P11_DI_MODE, (uint16_t)((m & P11_IFMT) | (perm & 07777)));
	h->w.bo->put32(ds + P11_DI_ATIME, (uint32_t)atime);
	h->w.bo->put32(ds + P11_DI_MTIME, (uint32_t)mtime);
	s5fs_wtblk(&h->w, blk, pb);
}

/* create-or-replace `path` as a regular file with the bytes read from srcfd */
int
rw_put_fd(RW *h, const char *path, int srcfd, unsigned perm, int32_t mtime)
{
	uint32_t ino;
	fsr_inode in;
	long off = 0;
	uint8_t buf[65536];
	ssize_t n;

	if (!h->writable)
		return -EROFS;
	ino = fsr_namei(&h->r, path);
	if (ino) {
		if (fsr_iget(&h->r, ino, &in) < 0)
			return -EIO;
		if ((in.mode & P11_IFMT) == P11_IFDIR)
			return -EISDIR;
		if ((in.mode & P11_IFMT) != P11_IFREG)
			return -EPERM;
		rw_truncate(h, ino, 0); /* replace contents */
	}
	else {
		int rc = rw_creat(h, path, perm, &ino);
		if (rc < 0)
			return rc;
	}
	for (;;) {
		long w;
		n = read(srcfd, buf, sizeof buf);
		if (n < 0)
			return -EIO;
		if (n == 0)
			break;
		w = rw_pwrite(h, ino, buf, n, off);
		if (w < 0)
			return (int)w;
		if (w < n)
			return -ENOSPC;
		off += w;
	}
	set_meta(h, ino, perm, mtime, mtime);
	rw_sync(h);
	return 0;
}

/* copy a regular file `src` to `dst`, both inside the image */
/* copy a regular file from src handle `sh` (path `src`) to dst handle `dh`
 * (path `dst`); sh and dh may be the same image or two different ones */
int
rw_copy_between(RW *sh, const char *src, RW *dh, const char *dst)
{
	uint32_t sino, dino;
	fsr_inode sin;
	long off = 0;
	uint8_t buf[65536];

	if (!dh->writable)
		return -EROFS;
	sino = fsr_namei(&sh->r, src);
	if (!sino)
		return -ENOENT;
	if (fsr_iget(&sh->r, sino, &sin) < 0)
		return -EIO;
	if ((sin.mode & P11_IFMT) == P11_IFDIR)
		return -EISDIR;
	if ((sin.mode & P11_IFMT) != P11_IFREG)
		return -EPERM;

	dino = fsr_namei(&dh->r, dst);
	if (dino) {
		fsr_inode din;
		if (fsr_iget(&dh->r, dino, &din) < 0)
			return -EIO;
		if ((din.mode & P11_IFMT) == P11_IFDIR)
			return -EISDIR;
		if ((din.mode & P11_IFMT) != P11_IFREG)
			return -EPERM;
		rw_truncate(dh, dino, 0);
	}
	else {
		int rc = rw_creat(dh, dst, sin.mode & 07777, &dino);
		if (rc < 0)
			return rc;
	}
	while (off < sin.size) {
		long want = sin.size - off;
		long w;
		if (want > (long)sizeof buf)
			want = sizeof buf;
		if (fsr_readfile(&sh->r, &sin, buf, want, off) != want)
			return -EIO;
		w = rw_pwrite(dh, dino, buf, want, off);
		if (w < 0)
			return (int)w;
		if (w < want)
			return -ENOSPC;
		off += want;
	}
	set_meta(dh, dino, sin.mode & 07777, sin.atime, sin.mtime);
	rw_sync(dh);
	return 0;
}

int
rw_copy(RW *h, const char *src, const char *dst)
{
	return rw_copy_between(h, src, h, dst);
}

/*
 * fsread.c -- read-only s5fs reader.  See fsread.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "fsread.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int
rdblk(FSR *r, uint32_t bno, uint8_t *buf)
{
	off_t off = (off_t)(r->base + (int64_t)bno * r->bsize);
	if (lseek(r->fd, off, SEEK_SET) < 0 ||
	    read(r->fd, buf, r->bsize) != (ssize_t)r->bsize)
		return -1;
	return 0;
}

int
fsr_bread(FSR *r, uint32_t bno, uint8_t *buf)
{
	return rdblk(r, bno, buf);
}

/* choose the byte order whose superblock decode is sane against the file size */
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
		const s5_codec *c = s5_codec_for((s5_endian)e);
		uint32_t isize = c->get16(sb + P11_SB_ISIZE);
		uint32_t fsize = c->get32(sb + P11_SB_FSIZE);
		if (isize >= P11_ILISTSTART && isize < fsize && fsize > 0 && fsize <= fileblocks)
			return c;
	}
	return NULL;
}

int
fsr_open(FSR *r, const char *path, uint32_t bsize, int forced_bo, int64_t base)
{
	uint8_t sb[P11_MAXBSIZE];

	memset(r, 0, sizeof *r);
	if (bsize == 0)
		bsize = 1024;
	if (!P11_BSIZE_OK(bsize))
		return -1;
	r->fd = open(path, O_RDONLY);
	if (r->fd < 0)
		return -1;

	r->base = base;
	r->bsize = bsize;
	r->inopb = bsize / P11_DINODESZ;
	r->naddr = (bsize == 1024) ? 7 : 13;
	r->laddr = r->naddr - 3;
	r->nindir = bsize / 4;
	r->ndirect = bsize / P11_DIRENTSZ;

	if (rdblk(r, P11_SUPERBLK, sb) < 0) {
		close(r->fd);
		return -1;
	}
	r->bo = (forced_bo >= 0) ? s5_codec_for((s5_endian)forced_bo)
				 : detect_bo(r->fd, bsize, sb);
	if (!r->bo) {
		close(r->fd);
		return -1;
	}
	r->isize = r->bo->get16(sb + P11_SB_ISIZE);
	r->fsize = r->bo->get32(sb + P11_SB_FSIZE);
	if (r->isize < P11_ILISTSTART || r->isize >= r->fsize || r->fsize == 0) {
		close(r->fd);
		return -1;
	}
	return 0;
}

void
fsr_close(FSR *r)
{
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
}

int
fsr_iget(FSR *r, uint32_t ino, fsr_inode *out)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t blk = (ino + 2 * r->inopb - 1) / r->inopb;
	uint32_t off = ((ino + 2 * r->inopb - 1) % r->inopb) * P11_DINODESZ;
	uint8_t *dp;
	uint32_t i;

	if (ino == 0 || blk >= r->isize)
		return -1;
	if (rdblk(r, blk, buf) < 0)
		return -1;
	dp = buf + off;
	out->mode = r->bo->get16(dp + P11_DI_MODE);
	out->nlink = (int16_t)r->bo->get16(dp + P11_DI_NLINK);
	out->uid = (int16_t)r->bo->get16(dp + P11_DI_UID);
	out->gid = (int16_t)r->bo->get16(dp + P11_DI_GID);
	out->size = (int32_t)r->bo->get32(dp + P11_DI_SIZE);
	out->atime = (int32_t)r->bo->get32(dp + P11_DI_ATIME);
	out->mtime = (int32_t)r->bo->get32(dp + P11_DI_MTIME);
	out->ctime = (int32_t)r->bo->get32(dp + P11_DI_CTIME);
	for (i = 0; i < r->naddr; i++)
		out->addr[i] = (int32_t)r->bo->get24(dp + P11_DI_ADDR + 3 * i);
	return 0;
}

/* logical block -> physical (0 = hole) */
static uint32_t
bmap(FSR *r, const int32_t *addr, uint32_t lbn)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t phys, per = r->nindir;

	if (lbn < r->laddr)
		return (uint32_t)addr[lbn];
	lbn -= r->laddr;
	if (lbn < per) {
		phys = (uint32_t)addr[r->laddr];
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * lbn);
		else
			phys = 0;
	}
	else if (lbn < per * per) {
		lbn -= per;
		phys = (uint32_t)addr[r->laddr + 1];
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * (lbn / per));
		else
			phys = 0;
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * (lbn % per));
		else
			phys = 0;
	}
	else {
		lbn -= per * per;
		phys = (uint32_t)addr[r->laddr + 2];
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * (lbn / (per * per)));
		else
			phys = 0;
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * ((lbn / per) % per));
		else
			phys = 0;
		if (phys && rdblk(r, phys, buf) == 0)
			phys = r->bo->get32(buf + 4 * (lbn % per));
		else
			phys = 0;
	}
	return phys;
}

int
fsr_readdir(FSR *r, const fsr_inode *dir, fsr_direntcb cb, void *arg)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t nblk = ((uint32_t)dir->size + r->bsize - 1) / r->bsize, b, e;

	if ((dir->mode & P11_IFMT) != P11_IFDIR)
		return -1;
	for (b = 0; b < nblk; b++) {
		uint32_t phys = bmap(r, dir->addr, b);
		if (phys == 0 || rdblk(r, phys, buf) < 0)
			continue;
		for (e = 0; e < r->ndirect; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint16_t di = r->bo->get16(d);
			char name[P11_DIRSIZ + 1];
			if (di == 0)
				continue;
			memcpy(name, d + 2, P11_DIRSIZ);
			name[P11_DIRSIZ] = '\0';
			if (cb(arg, di, name))
				return 0;
		}
	}
	return 0;
}

struct lookctx {
	const char *name;
	uint32_t ino;
};

static int
look_cb(void *arg, uint32_t ino, const char *name)
{
	struct lookctx *l = arg;
	if (strcmp(l->name, name) == 0) {
		l->ino = ino;
		return 1;
	}
	return 0;
}

uint32_t
fsr_namei(FSR *r, const char *path)
{
	char buf[2048], *tok, *save;
	uint32_t ino = P11_ROOTINO;

	strncpy(buf, path, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		fsr_inode in;
		struct lookctx l;
		if (fsr_iget(r, ino, &in) < 0)
			return 0;
		if ((in.mode & P11_IFMT) != P11_IFDIR)
			return 0;
		l.name = tok;
		l.ino = 0;
		fsr_readdir(r, &in, look_cb, &l);
		if (l.ino == 0)
			return 0;
		ino = l.ino;
	}
	return ino;
}

long
fsr_readfile(FSR *r, const fsr_inode *in, void *vbuf, long size, long off)
{
	uint8_t *buf = vbuf, blk[P11_MAXBSIZE];
	long done = 0;

	if (off >= in->size)
		return 0;
	if (off + size > in->size)
		size = in->size - off;
	while (done < size) {
		uint32_t lbn = (uint32_t)((off + done) / r->bsize);
		uint32_t boff = (uint32_t)((off + done) % r->bsize);
		uint32_t want = r->bsize - boff;
		uint32_t phys;
		if ((long)want > size - done)
			want = (uint32_t)(size - done);
		phys = bmap(r, in->addr, lbn);
		if (phys == 0)
			memset(buf + done, 0, want); /* hole */
		else {
			if (rdblk(r, phys, blk) < 0)
				break;
			memcpy(buf + done, blk + boff, want);
		}
		done += want;
	}
	return done;
}

/* ------------------------------------------------------------------ *
 * visited set for recursive directory walks (see fsread.h)
 * ------------------------------------------------------------------ */

int
fsr_walkset_init(fsr_walkset *w, const FSR *r)
{
	/* inode numbers run 1 .. (isize-2)*inopb; index the array by number, so
	 * allocate one extra and leave [0] unused. */
	w->n = (r->isize > P11_ILISTSTART) ? (r->isize - P11_ILISTSTART) * r->inopb : 0;
	w->seen = calloc((size_t)w->n + 1, 1);
	return w->seen ? 0 : -1;
}

void
fsr_walkset_free(fsr_walkset *w)
{
	free(w->seen);
	w->seen = NULL;
	w->n = 0;
}

int
fsr_walk_enter(fsr_walkset *w, uint32_t ino)
{
	if (!w->seen || ino == 0 || ino > w->n)
		return 0; /* out of range: a garbage entry, do not follow it */
	if (w->seen[ino])
		return 0; /* already entered: a cycle */
	w->seen[ino] = 1;
	return 1;
}

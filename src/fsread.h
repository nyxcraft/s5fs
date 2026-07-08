/*
 * fsread.h -- a small read-only reader for s5fs images.
 *
 * Factored out so the FUSE mount (cmd_mount.c) can walk an image the same way
 * fsck does: decode the superblock (auto-detecting byte order), read inodes,
 * map logical->physical blocks (direct + 1/2/3 indirect), iterate directories,
 * and read file byte ranges.  Read-only; the writer core is separate.
 */
#ifndef FSREAD_H
#define FSREAD_H

#include <stdint.h>
#include "pdp11fs.h"
#include "s5endian.h"

typedef struct {
	int      fd;
	const s5_codec *bo;
	int64_t  base;		/* byte offset of the fs within the file (partition) */
	uint32_t bsize, inopb, naddr, laddr, nindir, ndirect;
	uint32_t isize, fsize;
} FSR;

typedef struct {
	uint16_t mode;
	int16_t  nlink, uid, gid;
	int32_t  size, atime, mtime, ctime;
	int32_t  addr[P11_MAXNADDR];
} fsr_inode;

/* Open image read-only. bsize is 512/1024 (0 => 1024). forced_bo is an
 * s5_endian, or -1 to auto-detect from the superblock. `base` is the byte
 * offset of the filesystem within the file (0 for a bare image; a partition's
 * start for a whole-disk image). 0 ok, -1 on error. */
int  fsr_open(FSR *r, const char *path, uint32_t bsize, int forced_bo, int64_t base);
void fsr_close(FSR *r);

/* Read inode `ino` (1-based). 0 ok, -1 out of range. */
int  fsr_iget(FSR *r, uint32_t ino, fsr_inode *out);

/* Resolve an absolute path to an inode number (0 if not found). */
uint32_t fsr_namei(FSR *r, const char *path);

/* Iterate directory `dir`'s entries; cb returns nonzero to stop early. */
typedef int (*fsr_direntcb)(void *arg, uint32_t ino, const char *name);
int  fsr_readdir(FSR *r, const fsr_inode *dir, fsr_direntcb cb, void *arg);

/* Read up to `size` bytes of file `in` at byte offset `off`; returns count. */
long fsr_readfile(FSR *r, const fsr_inode *in, void *buf, long size, long off);

/* Read a raw filesystem block by number into buf (bsize bytes). 0 ok, -1. */
int  fsr_bread(FSR *r, uint32_t bno, uint8_t *buf);

#endif /* FSREAD_H */

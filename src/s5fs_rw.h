/*
 * s5fs_rw.h -- a read/write view of an s5fs image, and the file-mutation
 * engine built on it.
 *
 * Reads go through the fsread reader (`r`); writes/allocations go through the
 * s5fs_core writer (`w`, loaded from the same image via s5fs_mount).  The two
 * fds are page-cache coherent, so `r` sees `w`'s writes.  This is the single
 * mutation engine shared by the batch file commands (cmd_fs.c), the
 * interactive explorer (cmd_shell.c), and the FUSE mount (cmd_mount.c) -- so
 * every front-end runs the exact same, validated directory/inode/block logic.
 *
 * Block allocation is in-place over direct + single/double/triple indirect.
 * All high-level ops flush the superblock (rw_sync) before returning, so the
 * image on disk is consistent after each call.  Ops return 0 or a negative
 * errno; paths are absolute within the image ("/bin/sh").
 */
#ifndef S5FS_RW_H
#define S5FS_RW_H

#include <stdint.h>
#include "fsread.h"
#include "s5fs_core.h"

typedef struct {
	FSR  r;			/* read side (always open)        */
	S5FS w;			/* write side (valid if writable) */
	int  writable;
} RW;

/* Open `path`; writable=1 also loads the writer (needs O_RDWR access).
 * bsize is 512/1024 (0 => 1024); forced_bo is an s5_endian or -1 to detect. */
int  rw_open(RW *h, const char *path, uint32_t bsize, int forced_bo, int writable, int64_t base);
void rw_close(RW *h);
void rw_sync(RW *h);				/* flush the superblock */

/* lookups (read side) */
uint32_t rw_namei(RW *h, const char *path);	/* inode number, 0 if not found */
int      rw_iget(RW *h, uint32_t ino, fsr_inode *out);

/* directory / inode ops (absolute image paths) -- 0 or -errno */
int rw_mkdir (RW *h, const char *path, unsigned perm);
int rw_rmdir (RW *h, const char *path);
int rw_unlink(RW *h, const char *path);
int rw_rename(RW *h, const char *from, const char *to);
int rw_creat (RW *h, const char *path, unsigned perm, uint32_t *out_ino);
int rw_chmod (RW *h, const char *path, unsigned perm);
int rw_chown (RW *h, const char *path, int uid, int gid);
int rw_utimes(RW *h, const char *path, int32_t atime, int32_t mtime);

/* file contents */
long rw_pwrite  (RW *h, uint32_t ino, const void *buf, long size, long off);
int  rw_truncate(RW *h, uint32_t ino, long len);
int  rw_put_fd  (RW *h, const char *path, int srcfd, unsigned perm, int32_t mtime);
int  rw_copy    (RW *h, const char *src, const char *dst);   /* image -> image */
int  rw_copy_between(RW *sh, const char *src, RW *dh, const char *dst); /* across images */

/* mid-level, for the FUSE random-write path */
void     rw_ino_loc(RW *h, uint32_t ino, uint32_t *blk, uint32_t *off);
uint32_t rw_bmap   (RW *h, uint8_t *ds, uint32_t lbn, int alloc, int *dirty);

#endif /* S5FS_RW_H */

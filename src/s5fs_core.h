/*
 * s5fs_core.h -- s5fs filesystem writer.
 *
 * A faithful C99 host port of 2.9BSD cmd/mkfs.c (SCCS 2.5): the superblock
 * layout, the s5-style chained free-block list with rotational interleave,
 * and the inode / directory / indirect-block writers.  It is the core that
 * every disk-image front-end (dump->dsk, tar->dsk, tree->dsk) builds on: they
 * lay down an empty filesystem with s5fs_mkfs(), then populate it through the
 * same allocator and inode primitives the kernel and restor(8) use.
 *
 * mkfs's globals are encapsulated in a single S5FS handle so the code is
 * re-entrant and front-ends can drive it directly.
 */
#ifndef S5FS_H
#define S5FS_H

#include <stdint.h>
#include "pdp11fs.h"
#include "s5endian.h"

/* In-core image of one inode while a file is being built (mkfs's struct
 * inode, trimmed to what the writer needs).  Block numbers are host ints;
 * they are packed to 3-byte on-disk addresses only in s5fs_iput(). */
typedef struct {
	uint16_t number;			/* inode number (1-based) */
	uint16_t mode;				/* IF* | permission bits  */
	int16_t  nlink;
	int16_t  uid;
	int16_t  gid;
	int32_t  size;				/* bytes */
	int32_t  atime, mtime, ctime;		/* 0 => stamp fs->s_time (now) */
	int32_t  addr[P11_MAXNADDR];		/* direct + indirect block nums */
} s5fs_inode;

/* Tunables for a fresh filesystem.  Zero-initialise then override. */
typedef struct {
	uint32_t  bsize;	/* 512 or 1024; 0 => 1024 (UCB_NKB default) */
	int32_t   m, n;		/* free-list interleave; 0 => 5, 10        */
	int64_t   mtime;	/* inode/superblock timestamp; <0 => now    */
	uint32_t  ninode;	/* minimum inodes; 0 => mkfs size heuristic */
	int       boot;		/* reserved: leave block 0 for a boot image */
	s5_endian endian;	/* on-disk byte order; 0 => S5_PDP11        */
	int64_t   base;		/* byte offset to write the fs at (partition) */
	int       sysv;		/* also write the System V superblock magic/type */
} s5fs_opts;

/* The writer handle.  Fields after `error` are internal but exposed so
 * front-ends in the same tree can reuse the primitives. */
typedef struct {
	int      fd;		/* image file, opened read/write */
	int64_t  base;		/* byte offset of the fs in the file (partition) */
	int      sysv;		/* superblock carries the System V magic/type */
	char     err[128];	/* last error message            */
	int      error;		/* sticky error flag             */
	const s5_codec *bo;	/* on-disk byte-order codec      */

	/* block-size profile (derived from opts.bsize) */
	uint32_t bsize;		/* filesystem block size          */
	uint32_t inopb;		/* inodes per block (= NIPB)      */
	uint32_t naddr;		/* di_addr slots (7 or 13)        */
	uint32_t laddr;		/* naddr-3: direct slots in inode */
	uint32_t nindir;	/* daddr_t per indirect block     */
	uint32_t ndirect;	/* directory entries per block    */
	uint32_t clsize;	/* bsize / 512                    */

	/* decoded superblock */
	uint32_t s_isize;	/* first data block (= 2 + i-list blocks) */
	uint32_t s_fsize;	/* total blocks in the volume             */
	int32_t  s_nfree;
	int32_t  s_free[P11_NICFREE];
	int32_t  s_ninode;
	uint16_t s_inode[P11_NICINOD];
	int32_t  s_tfree;
	int32_t  s_tinode;
	int16_t  s_m, s_n;
	int32_t  s_time;

	uint32_t ino;		/* last inode number handed out */
	int      adr[P11_MAXFN];	/* interleave permutation */
} S5FS;

/* -------- top level -------- */

/* Build a complete empty filesystem of `nblocks` blocks on `fd` (already open
 * O_RDWR).  Creates root and lost+found exactly as mkfs's size mode does.
 * Returns 0 on success, -1 on error (message in fs->err). */
int s5fs_mkfs(S5FS *fs, int fd, uint32_t nblocks, const s5fs_opts *opts);

/* -------- staged primitives (used by s5fs_mkfs and by front-ends) -------- */

/* Initialise the handle + superblock geometry, size the i-list, extend the
 * image to full length and zero the i-list.  Leaves an empty free list. */
int  s5fs_begin(S5FS *fs, int fd, uint32_t nblocks, const s5fs_opts *opts);

/* Build the chained free list over every data block (mkfs's bflist), and
 * write the reserved bad-block inode (P11_BADBLKINO). */
void s5fs_freelist(S5FS *fs);

/* Load an EXISTING image's superblock into `fs` for live read/write (the
 * inverse of s5fs_finish).  `bo` is the byte order (from the reader's detect),
 * `bsize` the block size, `base` the byte offset of the fs in the file (0 for a
 * bare image; a partition start for a whole-disk image).  Afterwards
 * s5fs_alloc/bfree/writeinode operate on the loaded free list; call s5fs_finish
 * to flush the superblock.  0 ok, -1 error. */
int s5fs_mount(S5FS *fs, int fd, uint32_t bsize, const s5_codec *bo, int64_t base);

/* Serialise the decoded superblock to block 1.  Call last (or to sync). */
void s5fs_finish(S5FS *fs);

/* Block I/O in filesystem blocks (bsize bytes). */
void s5fs_rdblk(S5FS *fs, uint32_t bno, uint8_t *buf);
void s5fs_wtblk(S5FS *fs, uint32_t bno, const uint8_t *buf);

/* Allocate / free one data block via the superblock free list. */
int32_t s5fs_alloc(S5FS *fs);
void    s5fs_bfree(S5FS *fs, int32_t bno);

/* Add a directory entry to the in-core block `db`; flushes a full block. */
void s5fs_entry(S5FS *fs, int *dbc, uint8_t *db, int *ibc, int32_t *ib,
                uint16_t inum, const char *name);

/* Flush the current directory/data block `db` and record it in `ib`. */
void s5fs_newblk(S5FS *fs, int *dbc, uint8_t *db, int *ibc, int32_t *ib);

/* Write an inode to the i-list, packing `ibc` block numbers from `ib`
 * (direct slots, then one level of single indirection like mkfs). */
void s5fs_iput(S5FS *fs, s5fs_inode *in, int ibc, int32_t *ib);

/* -------- general primitives for front-ends (tree/dump/tar -> dsk) -------- */

/* Hand out the next inode number. */
uint32_t s5fs_ialloc(S5FS *fs);

/* Map physical blocks da[0..n-1] into inode `in` (direct + single/double/
 * triple indirect, allocating indirect blocks as needed). 0 ok, -1 on error. */
int s5fs_setblocks(S5FS *fs, s5fs_inode *in, const int32_t *da, uint32_t n);

/* Serialise inode `in` to the i-list (addresses already set). */
void s5fs_writeinode(S5FS *fs, const s5fs_inode *in);

/* Increment an existing on-disk inode's link count (for hard links). */
void s5fs_bumplink(S5FS *fs, uint32_t ino);

#endif /* S5FS_H */

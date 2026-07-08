/*
 * pdp11fs.h -- on-disk *structure* of the s5fs filesystem (the traditional
 * V7 / System V filesystem), described for a modern LP64 host.
 *
 * These are the byte-order-INDEPENDENT facts: field offsets, sizes, mode bits,
 * and the fixed constants.  The layout is identical across every host that ran
 * s5fs (V7/2.xBSD on the PDP-11, 3BSD/4.0/4.1 on the VAX, and other SysV
 * boxes); only the encoding of multi-byte fields differs, and that lives in
 * the pluggable codecs of s5endian.h.  We are a *host* tool, so we never
 * overlay a host struct on the disk bytes -- every field goes through a codec,
 * exactly as the das/ld/nm host ports in this toolchain do.
 *
 * Constants originate from the 2.9BSD headers (<sys/param.h>, <sys/ino.h>,
 * <sys/inode.h>, <sys/filsys.h>, <sys/dir.h>, <sys/fblk.h>, UCB_NKB config)
 * and were confirmed identical in V7 and 4.0/4.1.  The P11_ prefix is
 * historical; the values are the shared s5fs structural constants.
 */
#ifndef PDP11FS_H
#define PDP11FS_H

#include <stdint.h>

/* Multi-byte field encodings live in s5endian.h (s5_codec).  This header is
 * pure structure -- no byte order. */

/* ------------------------------------------------------------------ *
 * Fixed structural constants (independent of block size)
 * ------------------------------------------------------------------ */

#define P11_NICFREE	50	/* free-block cache in the superblock  */
#define P11_NICINOD	100	/* free-inode cache in the superblock  */
#define P11_DIRSIZ	14	/* bytes of name in a directory entry  */
#define P11_DIRENTSZ	16	/* sizeof(struct direct): 2 + DIRSIZ   */
#define P11_DINODESZ	64	/* sizeof on-disk struct dinode        */
#define P11_MAXNADDR	13	/* di_addr slots in the 512-byte profile */
#define P11_MAXFN	500	/* cap on the free-list interleave n   */
#define P11_MAXBSIZE	2048	/* largest block size we support       */
#define P11_MAXNINDIR	(P11_MAXBSIZE / 4)	/* daddr_t per indirect block */

/* Logical block sizes s5fs allows: 512 (V7), 1024 (2.x BSD UCB), 2048 (SysV
 * Fs4b).  2048 is the family maximum -- the superblock's block-size code only
 * defines up to Fs4b; bigger blocks are FFS, a different filesystem. */
#define P11_BSIZE_OK(b)	((b) == 512 || (b) == 1024 || (b) == 2048)
#define P11_MAXFSBLKS	(1UL << 24)	/* max blocks: di_addr is a 3-byte (24-bit)
					 * block number, so a filesystem can hold at
					 * most 2^24 blocks (16G @1K, 8G @512) */

/* i_mode / di_mode bits (from <sys/inode.h>) */
#define P11_IFMT	0170000	/* file type mask        */
#define P11_IFDIR	0040000	/* directory             */
#define P11_IFCHR	0020000	/* character special     */
#define P11_IFBLK	0060000	/* block special         */
#define P11_IFREG	0100000	/* regular               */
#define P11_ISUID	0004000	/* set-uid               */
#define P11_ISGID	0002000	/* set-gid               */
#define P11_ISVTX	0001000	/* sticky / save text    */
#define P11_IREAD	0000400
#define P11_IWRITE	0000200
#define P11_IEXEC	0000100

/* First two blocks are boot (0) and superblock (1); the i-list starts at 2. */
#define P11_BOOTBLK	0
#define P11_SUPERBLK	1
#define P11_ILISTSTART	2

/* Reserved inode numbers. */
#define P11_BADBLKINO	1	/* holds mkfs's bad-block list         */
#define P11_ROOTINO	2	/* root directory                      */

/* ------------------------------------------------------------------ *
 * On-disk struct dinode: byte offsets within a P11_DINODESZ slot
 * ------------------------------------------------------------------ */

#define P11_DI_MODE	0	/* u_short */
#define P11_DI_NLINK	2	/* short   */
#define P11_DI_UID	4	/* short   */
#define P11_DI_GID	6	/* short   */
#define P11_DI_SIZE	8	/* off_t   */
#define P11_DI_ADDR	12	/* char[40]: up to 13 x 3-byte block numbers */
#define P11_DI_ATIME	52	/* time_t  */
#define P11_DI_MTIME	56	/* time_t  */
#define P11_DI_CTIME	60	/* time_t  */

/* ------------------------------------------------------------------ *
 * Superblock (struct filsys): byte offsets within block 1
 * ------------------------------------------------------------------ */

#define P11_SB_ISIZE	0				/* u_short  */
#define P11_SB_FSIZE	2				/* daddr_t  */
#define P11_SB_NFREE	6				/* short    */
#define P11_SB_FREE	8				/* daddr_t[NICFREE] */
#define P11_SB_NINODE	(P11_SB_FREE + 4 * P11_NICFREE)	 /* 208, short */
#define P11_SB_INODE	(P11_SB_NINODE + 2)		 /* 210, ino_t[NICINOD] */
#define P11_SB_FLOCK	(P11_SB_INODE + 2 * P11_NICINOD) /* 410, 4x char */
#define P11_SB_TIME	(P11_SB_FLOCK + 4)		 /* 414, time_t */
#define P11_SB_TFREE	(P11_SB_TIME + 4)		 /* 418, daddr_t */
#define P11_SB_TINODE	(P11_SB_TFREE + 4)		 /* 422, ino_t */
#define P11_SB_DINFO	(P11_SB_TINODE + 2)		 /* 424, short[2] = m,n */
#define P11_SB_FSMNT	(P11_SB_DINFO + 4)		 /* 428, char[12] */
#define P11_SB_LASTI	(P11_SB_FSMNT + 12)		 /* 440, ino_t */
#define P11_SB_NBEHIND	(P11_SB_LASTI + 2)		 /* 442, ino_t; ends 444 */

/* System V (s5) superblock dialect.  The common part (0..417) is identical, but
 * SysV inserts s_dinfo[4] at 418, pushing the free/inode totals later, and adds
 * a magic + block-size type at the tail so a SysV kernel auto-detects the fs.
 * Opt-in (`mkfs -F sysv`); these all live in the V7 layout's zeroed tail, so a
 * SysV-flavoured image still reads fine as a plain image (isize/fsize/inodes/
 * dirs/free-list are dialect-independent). */
#define P11_SB_SVDINFO	418	/* SysV: short[4] device info            */
#define P11_SB_SVTFREE	426	/* SysV: daddr_t total free blocks       */
#define P11_SB_SVTINODE	430	/* SysV: ino_t total free inodes         */
#define P11_SB_STATE	500	/* SysV: long fs state (clean marker)    */
#define P11_SB_MAGIC	504	/* SysV: long 0xfd187e20                 */
#define P11_SB_TYPE	508	/* SysV: long 1/2/3 = 512/1024/2048      */
#define P11_FS_MAGIC	0xfd187e20UL	/* SysV FsMAGIC                  */
#define P11_FS_CLEAN	0x7c269d38UL	/* s_state = FS_CLEAN - s_time => clean */
#define P11_FS_TYPE(b)	((b) == 512 ? 1 : (b) == 1024 ? 2 : 3)	/* Fs1b/2b/4b */

/* Free-block list block (struct fblk): count then the addresses. */
#define P11_FB_NFREE	0				/* short */
#define P11_FB_FREE	2				/* daddr_t[NICFREE] */

#endif /* PDP11FS_H */

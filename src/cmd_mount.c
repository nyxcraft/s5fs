/*
 * s5fs mount / umount -- FUSE view of an s5fs image.
 *
 * Built only with `make FUSE=1` (needs libfuse3); the default build keeps the
 * core dependency-free and cmd_util.c provides "planned" stubs instead.
 *
 * This is now a thin FUSE front-end over the shared s5fs_rw engine (s5fs_rw.c)
 * -- the same directory/inode/block code the batch commands (cmd_fs.c) and the
 * interactive shell (cmd_shell.c) use.  Reads go through the fsread reader
 * (H.r); every mutation is an rw_* call.  Read-only unless mounted with -w.
 *
 * usage: s5fs mount [-B 512|1024] [-A pdp11|le|be] [-w] [-f] image mountpoint
 *        s5fs umount mountpoint
 */

#ifdef HAVE_FUSE

#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include "s5fs_rw.h"
#include "s5endian.h"
#include "device.h"
#include "cmds.h"

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static RW  H;			/* the shared read/write engine handle */
static int g_rw;		/* mounted read-write? (H.writable mirrors it) */

/* ---------------- read side (fsread) ---------------- */

static int s5_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	uint32_t ino = rw_namei(&H, path);
	fsr_inode in;
	(void)fi;
	if (!ino || rw_iget(&H, ino, &in) < 0)
		return -ENOENT;
	memset(st, 0, sizeof *st);
	st->st_ino   = ino;
	st->st_mode  = in.mode;			/* s5fs IFMT == host S_IFMT */
	st->st_nlink = (nlink_t)(uint16_t)in.nlink;
	st->st_uid   = (uid_t)(uint16_t)in.uid;
	st->st_gid   = (gid_t)(uint16_t)in.gid;
	st->st_size  = in.size;
	st->st_atime = (time_t)in.atime;
	st->st_mtime = (time_t)in.mtime;
	st->st_ctime = (time_t)in.ctime;
	if ((in.mode & P11_IFMT) == P11_IFCHR || (in.mode & P11_IFMT) == P11_IFBLK)
		st->st_rdev = makedev((in.addr[0] >> 8) & 0377, in.addr[0] & 0377);
	st->st_blksize = H.r.bsize;
	st->st_blocks  = (in.size + 511) / 512;
	return 0;
}

struct fillc { void *buf; fuse_fill_dir_t filler; };
static int fill_cb(void *arg, uint32_t ino, const char *name)
{
	struct fillc *f = arg;
	(void)ino;
	f->filler(f->buf, name, NULL, 0, 0);
	return 0;
}

static int s5_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	uint32_t ino = rw_namei(&H, path);
	fsr_inode in;
	struct fillc c;
	(void)off; (void)fi; (void)flags;
	if (!ino || rw_iget(&H, ino, &in) < 0)
		return -ENOENT;
	if ((in.mode & P11_IFMT) != P11_IFDIR)
		return -ENOTDIR;
	c.buf = buf; c.filler = filler;
	fsr_readdir(&H.r, &in, fill_cb, &c);
	return 0;
}

static int s5_open(const char *path, struct fuse_file_info *fi)
{
	if (!rw_namei(&H, path))
		return -ENOENT;
	if (!g_rw && (fi->flags & O_ACCMODE) != O_RDONLY)
		return -EROFS;
	return 0;
}

static int s5_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	uint32_t ino = rw_namei(&H, path);
	fsr_inode in;
	(void)fi;
	if (!ino || rw_iget(&H, ino, &in) < 0)
		return -ENOENT;
	if ((in.mode & P11_IFMT) == P11_IFDIR)
		return -EISDIR;
	return (int)fsr_readfile(&H.r, &in, buf, (long)size, (long)off);
}

/* ---------------- write side (shared engine) ---------------- */

static int s5_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi; return rw_chmod(&H, path, mode);
}

static int s5_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	(void)fi;
	return rw_chown(&H, path,
	                uid == (uid_t)-1 ? -1 : (int)uid,
	                gid == (gid_t)-1 ? -1 : (int)gid);
}

static int s5_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	(void)fi; return rw_utimes(&H, path, (int32_t)tv[0].tv_sec, (int32_t)tv[1].tv_sec);
}

static int s5_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi; return rw_creat(&H, path, mode, NULL);
}

static int s5_mkdir(const char *path, mode_t mode) { return rw_mkdir(&H, path, mode); }
static int s5_unlink(const char *path)             { return rw_unlink(&H, path); }
static int s5_rmdir(const char *path)              { return rw_rmdir(&H, path); }

static int s5_rename(const char *from, const char *to, unsigned int flags)
{
	if (flags) return -EINVAL;		/* no RENAME_NOREPLACE/EXCHANGE */
	return rw_rename(&H, from, to);
}

static int s5_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	uint32_t ino = rw_namei(&H, path);
	long r;
	(void)fi;
	if (!ino) return -ENOENT;
	r = rw_pwrite(&H, ino, buf, (long)size, (long)off);
	if (r >= 0) rw_sync(&H);
	return (int)r;
}

static int s5_truncate(const char *path, off_t len, struct fuse_file_info *fi)
{
	uint32_t ino = rw_namei(&H, path);
	(void)fi;
	if (!ino) return -ENOENT;
	return rw_truncate(&H, ino, (long)len);
}

static void s5_destroy(void *p) { (void)p; rw_sync(&H); }

static const struct fuse_operations s5_ops = {
	.getattr  = s5_getattr,
	.readdir  = s5_readdir,
	.open     = s5_open,
	.read     = s5_read,
	.write    = s5_write,
	.truncate = s5_truncate,
	.create   = s5_create,
	.mkdir    = s5_mkdir,
	.unlink   = s5_unlink,
	.rmdir    = s5_rmdir,
	.chmod    = s5_chmod,
	.chown    = s5_chown,
	.utimens  = s5_utimens,
	.rename   = s5_rename,
	.destroy  = s5_destroy,
};

int cmd_mount(int argc, char **argv)
{
	uint32_t bsize = 0, plen = 0;
	int forced = -1, foreground = 0, c;
	const char *image, *mnt, *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	char *fargv[12];
	int fc = 0;

	while ((c = getopt(argc, argv, "B:A:fwd:P:o:")) != -1) {
		switch (c) {
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'f': foreground = 1; break;
		case 'w': g_rw = 1; break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) { fprintf(stderr, "s5fs mount: bad -A byte order\n"); return 2; }
			forced = (int)e;
			break;
		}
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		default:
			fprintf(stderr, "usage: s5fs mount [-B 512|1024] [-A pdp11|le|be] [-w] [-f] [-d dev -P part | -o blk] image mountpoint\n");
			return 2;
		}
	}
	if (optind != argc - 2) {
		fprintf(stderr, "usage: s5fs mount [-B 512|1024] [-A pdp11|le|be] [-w] [-f] [-d dev -P part | -o blk] image mountpoint\n");
		return 2;
	}
	image = argv[optind];
	mnt   = argv[optind + 1];
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;

	if (rw_open(&H, image, bsize, forced, g_rw, base) < 0) {
		fprintf(stderr, "s5fs mount: %s: cannot open %s (try -B/-A)\n",
		        image, g_rw ? "read-write" : "for reading");
		return 1;
	}
	printf("mounting %s (%s, %u-byte blocks, %s) at %s%s\n",
	       image, H.r.bo->name, H.r.bsize, g_rw ? "read-write" : "read-only",
	       mnt, foreground ? "" : " [background]");

	fargv[fc++] = "s5fs";
	fargv[fc++] = (char *)mnt;
	fargv[fc++] = "-s";			/* single-threaded: shared fds */
	fargv[fc++] = "-o";
	fargv[fc++] = g_rw ? "rw,fsname=s5fs" : "ro,fsname=s5fs";
	if (foreground)
		fargv[fc++] = "-f";
	fargv[fc] = NULL;

	return fuse_main(fc, fargv, &s5_ops, NULL);
}

int cmd_umount(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: s5fs umount mountpoint\n"); return 2; }
	execlp("fusermount3", "fusermount3", "-u", argv[1], (char *)NULL);
	execlp("fusermount",  "fusermount",  "-u", argv[1], (char *)NULL);
	fprintf(stderr, "s5fs umount: cannot exec fusermount3: %s\n", strerror(errno));
	return 1;
}

#endif /* HAVE_FUSE */

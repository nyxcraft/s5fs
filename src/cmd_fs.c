/*
 * cmd_fs.c -- batch file commands over an s5fs image, without a mount.
 *
 * These give a FUSE-free (e.g. Windows) way to inspect and edit an image:
 *
 *   s5fs ls    image [-l] [-a] [path]      list a directory (or a file)
 *   s5fs cat   image path...               write file contents to stdout
 *   s5fs get   image imgpath [hostpath]    copy a file OUT of the image
 *   s5fs put   image hostpath [imgpath]    copy a host file INTO the image
 *   s5fs cp    image src dst               copy a file within the image
 *   s5fs mv    image src dst               rename/move within the image
 *   s5fs rm    image path...               remove file(s)
 *   s5fs mkdir image [-p] path...          create directory(ies)
 *   s5fs rmdir image path...               remove empty directory(ies)
 *   s5fs chmod image mode path...          change permission bits
 *
 * All share the s5fs_rw engine (the same code the FUSE mount uses).  Each
 * command opens the image, does its work, flushes, and closes -- so the image
 * is consistent afterwards and passes `s5fs fsck`.
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_rw.h"
#include "fsutil.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <libgen.h>

/* ------------------------------------------------------------------ *
 * shared helpers (declared in fsutil.h)
 * ------------------------------------------------------------------ */

const char *
fs_errstr(int rc)
{
	return strerror(-rc);
}

void
fs_modestr(uint16_t mode, char out[11])
{
	static const char *rwx = "rwxrwxrwx";
	int t = mode & P11_IFMT, i;
	out[0] = t == P11_IFDIR ? 'd' : t == P11_IFCHR ? 'c'
				: t == P11_IFBLK       ? 'b'
				: t == P11_IFREG       ? '-'
						       : '?';
	for (i = 0; i < 9; i++)
		out[1 + i] = (mode & (0400 >> i)) ? rwx[i] : '-';
	if (mode & 04000)
		out[3] = (out[3] == 'x') ? 's' : 'S';
	if (mode & 02000)
		out[6] = (out[6] == 'x') ? 's' : 'S';
	if (mode & 01000)
		out[9] = (out[9] == 'x') ? 't' : 'T';
	out[10] = '\0';
}

int
fs_host_path(const char **p)
{
	if ((*p)[0] == '@') {
		(*p)++;
		return 1;
	}
	return 0;
}

/* image file -> open host fd for writing (append basename if dst is a dir) */
static int
host_out(const char *dst, unsigned perm, const char *srcbase, char *nb, size_t nbsz)
{
	struct stat st;
	const char *target = dst;
	if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(nb, nbsz, "%s/%s", dst, srcbase);
		target = nb;
	}
	return open(target, O_WRONLY | O_CREAT | O_TRUNC, perm ? perm : 0644);
}

int
fs_copy(RW *h, const char *src, int src_host, const char *dst, int dst_host)
{
	char nb[2048];

	/* image -> image */
	if (!src_host && !dst_host)
		return rw_copy(h, src, dst);

	/* host -> image */
	if (src_host && !dst_host) {
		int ifd = open(src, O_RDONLY), rc;
		struct stat st;
		char ib[1200];
		const char *target = dst;
		uint32_t d;
		fsr_inode di;
		if (ifd < 0 || fstat(ifd, &st) < 0) {
			if (ifd >= 0)
				close(ifd);
			return -errno;
		}
		d = rw_namei(h, dst);
		if (d && rw_iget(h, d, &di) == 0 && (di.mode & P11_IFMT) == P11_IFDIR) {
			char tmp[1024];
			snprintf(tmp, sizeof tmp, "%s", src);
			snprintf(ib, sizeof ib, "%s/%s", strcmp(dst, "/") ? dst : "", basename(tmp));
			target = ib;
		}
		rc = rw_put_fd(h, target, ifd, st.st_mode & 0777, (int32_t)st.st_mtime);
		close(ifd);
		return rc;
	}

	/* image -> host */
	if (!src_host && dst_host) {
		fsr_inode in;
		uint32_t ino = rw_namei(h, src);
		long off = 0;
		int ofd;
		const char *b = strrchr(src, '/');
		if (!ino || rw_iget(h, ino, &in) < 0)
			return -ENOENT;
		if ((in.mode & P11_IFMT) == P11_IFDIR)
			return -EISDIR;
		ofd = host_out(dst, in.mode & 0777, b ? b + 1 : src, nb, sizeof nb);
		if (ofd < 0)
			return -errno;
		while (off < in.size) {
			uint8_t buf[65536];
			long want = in.size - off;
			if (want > (long)sizeof buf)
				want = sizeof buf;
			if (fsr_readfile(&h->r, &in, buf, want, off) != want) {
				close(ofd);
				return -EIO;
			}
			if (write(ofd, buf, want) != want) {
				close(ofd);
				return -errno;
			}
			off += want;
		}
		close(ofd);
		return 0;
	}

	/* host -> host */
	{
		int ifd = open(src, O_RDONLY), ofd;
		struct stat st;
		ssize_t n;
		const char *b = strrchr(src, '/');
		if (ifd < 0)
			return -errno;
		if (fstat(ifd, &st) < 0) {
			close(ifd);
			return -errno;
		}
		ofd = host_out(dst, st.st_mode & 0777, b ? b + 1 : src, nb, sizeof nb);
		if (ofd < 0) {
			close(ifd);
			return -errno;
		}
		{
			uint8_t buf[65536];
			while ((n = read(ifd, buf, sizeof buf)) > 0)
				if (write(ofd, buf, n) != n) {
					close(ifd);
					close(ofd);
					return -errno;
				}
		}
		close(ifd);
		close(ofd);
		return 0;
	}
}

/* strtol, but the whole field must be a number (and non-empty) */
static int
all_num(const char *s, int base, long *out)
{
	char *end;
	long v;

	if (!*s)
		return -1;
	errno = 0;
	v = strtol(s, &end, base);
	if (*end != '\0' || errno == ERANGE || v < 0)
		return -1;
	*out = v;
	return 0;
}

int
fs_parse_owner(const char *spec, int *uid, int *gid)
{
	char buf[64], *colon;
	long v;

	*uid = -1;
	*gid = -1;
	snprintf(buf, sizeof buf, "%s", spec);
	colon = strchr(buf, ':');
	if (colon) {
		*colon = '\0';
		if (colon[1]) {
			if (all_num(colon + 1, 10, &v) < 0)
				return -1;
			*gid = (int)v;
		}
	}
	if (buf[0]) {
		if (all_num(buf, 10, &v) < 0)
			return -1;
		*uid = (int)v;
	}
	return 0;
}

int
fs_parse_mode(const char *spec, unsigned *perm)
{
	long v;

	if (all_num(spec, 8, &v) < 0 || v > 07777)
		return -1;
	*perm = (unsigned)v;
	return 0;
}

void
fs_resolve(const char *cwd, const char *arg, char *out, size_t outsz)
{
	char tmp[2048], *comps[256], *save, *t;
	int nc = 0, i;
	size_t p = 0;

	if (arg[0] == '/')
		snprintf(tmp, sizeof tmp, "%s", arg);
	else
		snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
	for (t = strtok_r(tmp, "/", &save); t && nc < 256; t = strtok_r(NULL, "/", &save)) {
		if (!strcmp(t, "."))
			continue;
		if (!strcmp(t, "..")) {
			if (nc > 0)
				nc--;
			continue;
		}
		comps[nc++] = t;
	}
	if (nc == 0) {
		snprintf(out, outsz, "/");
		return;
	}
	out[0] = '\0';
	for (i = 0; i < nc && p < outsz; i++)
		p += snprintf(out + p, outsz - p, "/%s", comps[i]);
}

/* ------------------------------------------------------------------ *
 * open helper: parse leading -B/-A, open image at argv[optind].
 * On success returns the index of the first path arg; else -1 (msg printed).
 * ------------------------------------------------------------------ */

static int
fs_open(int argc, char **argv, RW *h, int writable, const char *extra)
{
	uint32_t bsize = 0, plen = 0;
	int forced = -1, c;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "B:A:d:P:o:")) != -1) {
		switch (c) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) {
				fprintf(stderr, "%s: bad -A byte order\n", argv[0]);
				return -1;
			}
			forced = (int)e;
			break;
		}
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
			goto usage;
		}
	}
	if (optind >= argc)
		goto usage;
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return -1;
	if (rw_open(h, argv[optind], bsize, forced, writable, base) < 0) {
		fprintf(stderr, "%s: %s: not a%s s5fs image (try -B/-A, or -d/-P for a partition)\n",
			argv[0], argv[optind], writable ? " writable" : " readable");
		return -1;
	}
	return optind + 1;
usage:
	fprintf(stderr, "usage: s5fs %s [-B 512|1024|2048] [-A pdp11|le|be] "
			"[-d dev -P part | -o blk] image %s\n",
		argv[0], extra);
	return -1;
}

/* getopt with -B/-A plus extra flags; sets *flag chars seen. Simpler commands
 * (ls, mkdir) reuse fs_open; ls/mkdir parse their own extra flags below. */

/* ------------------------------------------------------------------ *
 * ls
 * ------------------------------------------------------------------ */

struct lsctx {
	RW *h;
	int longf;
	int all;
};

static int
ls_one(void *arg, uint32_t ino, const char *name)
{
	struct lsctx *c = arg;
	fsr_inode in;
	char m[11], ts[20];

	if (!c->all && name[0] == '.')
		return 0;
	if (!c->longf) {
		printf("%s\n", name);
		return 0;
	}
	if (rw_iget(c->h, ino, &in) < 0)
		return 0;
	fs_modestr(in.mode, m);
	{
		time_t tt = (time_t)in.mtime;
		struct tm *tm = localtime(&tt);
		if (tm)
			strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm);
		else
			snprintf(ts, sizeof ts, "?");
	}
	if ((in.mode & P11_IFMT) == P11_IFCHR || (in.mode & P11_IFMT) == P11_IFBLK)
		printf("%s %2d %4d %4d %3u,%3u %s %s\n", m, (uint16_t)in.nlink,
		       (uint16_t)in.uid, (uint16_t)in.gid,
		       (in.addr[0] >> 8) & 0377, in.addr[0] & 0377, ts, name);
	else
		printf("%s %2d %4d %4d %8d %s %s\n", m, (uint16_t)in.nlink,
		       (uint16_t)in.uid, (uint16_t)in.gid, in.size, ts, name);
	return 0;
}

int
cmd_ls(int argc, char **argv)
{
	RW h;
	int longf = 0, all = 0, c, i;
	uint32_t bsize = 0, plen = 0;
	int forced = -1;
	const char *path = "/", *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	struct lsctx ctx;
	fsr_inode in;
	uint32_t ino;

	optind = 1;
	while ((c = getopt(argc, argv, "laB:A:d:P:o:")) != -1) {
		switch (c) {
		case 'l':
			longf = 1;
			break;
		case 'a':
			all = 1;
			break;
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) {
				fprintf(stderr, "ls: bad -A\n");
				return 2;
			}
			forced = (int)e;
			break;
		}
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
			fprintf(stderr, "usage: s5fs ls [-l] [-a] [-B ..] [-A ..] [-d dev -P part | -o blk] image [path]\n");
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: s5fs ls [-l] [-a] image [path]\n");
		return 2;
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	if (rw_open(&h, argv[optind], bsize, forced, 0, base) < 0) {
		fprintf(stderr, "ls: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", argv[optind]);
		return 1;
	}
	if (optind + 1 < argc)
		path = argv[optind + 1];

	ino = rw_namei(&h, path);
	if (!ino || rw_iget(&h, ino, &in) < 0) {
		fprintf(stderr, "ls: %s: no such file or directory\n", path);
		rw_close(&h);
		return 1;
	}
	ctx.h = &h;
	ctx.longf = longf;
	ctx.all = all;
	if ((in.mode & P11_IFMT) == P11_IFDIR)
		fsr_readdir(&h.r, &in, ls_one, &ctx);
	else { /* a single file */
		const char *leaf = strrchr(path, '/'); /* not `base`: that is the
							* partition byte offset in
							* this same function */
		ls_one(&ctx, ino, leaf ? leaf + 1 : path);
	}
	rw_close(&h);
	(void)i;
	return 0;
}

/* ------------------------------------------------------------------ *
 * cat
 * ------------------------------------------------------------------ */

int
cmd_cat(int argc, char **argv)
{
	RW h;
	int i, first, rc = 0;
	first = fs_open(argc, argv, &h, 0, "path...");
	if (first < 0)
		return 1;
	if (first >= argc) {
		fprintf(stderr, "cat: need a path\n");
		rw_close(&h);
		return 2;
	}
	for (i = first; i < argc; i++) {
		fsr_inode in;
		long off = 0;
		uint32_t ino = rw_namei(&h, argv[i]);
		if (!ino || rw_iget(&h, ino, &in) < 0) {
			fprintf(stderr, "cat: %s: no such file\n", argv[i]);
			rc = 1;
			continue;
		}
		if ((in.mode & P11_IFMT) == P11_IFDIR) {
			fprintf(stderr, "cat: %s: is a directory\n", argv[i]);
			rc = 1;
			continue;
		}
		while (off < in.size) {
			uint8_t buf[8192];
			long want = in.size - off;
			if (want > (long)sizeof buf)
				want = sizeof buf;
			if (fsr_readfile(&h.r, &in, buf, want, off) != want)
				break;
			if (fwrite(buf, 1, want, stdout) != (size_t)want)
				break;
			off += want;
		}
	}
	rw_close(&h);
	return rc;
}

/* ------------------------------------------------------------------ *
 * get (image -> host)
 * ------------------------------------------------------------------ */

int
cmd_get(int argc, char **argv)
{
	RW h;
	int first;
	const char *imgpath, *hostpath;
	char hbuf[1024];
	fsr_inode in;
	uint32_t ino;
	long off = 0;
	int ofd, rc = 0;

	first = fs_open(argc, argv, &h, 0, "imgpath [hostpath]");
	if (first < 0)
		return 1;
	if (first >= argc) {
		fprintf(stderr, "get: need an image path\n");
		rw_close(&h);
		return 2;
	}
	imgpath = argv[first];
	if (first + 1 < argc)
		hostpath = argv[first + 1];
	else {
		const char *b = strrchr(imgpath, '/');
		snprintf(hbuf, sizeof hbuf, "%s", b ? b + 1 : imgpath);
		hostpath = hbuf;
	}
	ino = rw_namei(&h, imgpath);
	if (!ino || rw_iget(&h, ino, &in) < 0) {
		fprintf(stderr, "get: %s: no such file\n", imgpath);
		rw_close(&h);
		return 1;
	}
	if ((in.mode & P11_IFMT) == P11_IFDIR) {
		fprintf(stderr, "get: %s: is a directory (files only)\n", imgpath);
		rw_close(&h);
		return 1;
	}
	if (!strcmp(hostpath, "-"))
		ofd = STDOUT_FILENO;
	else
		ofd = open(hostpath, O_WRONLY | O_CREAT | O_TRUNC, in.mode & 0777);
	if (ofd < 0) {
		fprintf(stderr, "get: %s: %s\n", hostpath, strerror(errno));
		rw_close(&h);
		return 1;
	}
	while (off < in.size) {
		uint8_t buf[65536];
		long want = in.size - off;
		if (want > (long)sizeof buf)
			want = sizeof buf;
		if (fsr_readfile(&h.r, &in, buf, want, off) != want) {
			rc = 1;
			break;
		}
		if (write(ofd, buf, want) != want) {
			fprintf(stderr, "get: %s: write: %s\n", hostpath, strerror(errno));
			rc = 1;
			break;
		}
		off += want;
	}
	if (ofd != STDOUT_FILENO)
		close(ofd);
	if (!rc && ofd != STDOUT_FILENO)
		printf("%s -> %s (%d bytes)\n", imgpath, hostpath, in.size);
	rw_close(&h);
	return rc;
}

/* ------------------------------------------------------------------ *
 * put (host -> image)
 * ------------------------------------------------------------------ */

int
cmd_put(int argc, char **argv)
{
	RW h;
	int first, ifd, rc;
	const char *hostpath, *imgpath;
	char ibuf[1200];
	struct stat st;

	first = fs_open(argc, argv, &h, 1, "hostpath [imgpath]");
	if (first < 0)
		return 1;
	if (first >= argc) {
		fprintf(stderr, "put: need a host path\n");
		rw_close(&h);
		return 2;
	}
	hostpath = argv[first];
	ifd = open(hostpath, O_RDONLY);
	if (ifd < 0 || fstat(ifd, &st) < 0) {
		fprintf(stderr, "put: %s: %s\n", hostpath, strerror(errno));
		if (ifd >= 0)
			close(ifd);
		rw_close(&h);
		return 1;
	}
	if (first + 1 < argc) {
		imgpath = argv[first + 1];
		/* if target is an existing directory, put INTO it with the basename */
		uint32_t d = rw_namei(&h, imgpath);
		fsr_inode din;
		if (d && rw_iget(&h, d, &din) == 0 && (din.mode & P11_IFMT) == P11_IFDIR) {
			char tmp[1024];
			snprintf(tmp, sizeof tmp, "%s", hostpath);
			snprintf(ibuf, sizeof ibuf, "%s/%s", strcmp(imgpath, "/") ? imgpath : "", basename(tmp));
			imgpath = ibuf;
		}
	}
	else {
		char tmp[1024];
		snprintf(tmp, sizeof tmp, "%s", hostpath);
		snprintf(ibuf, sizeof ibuf, "/%s", basename(tmp));
		imgpath = ibuf;
	}
	rc = rw_put_fd(&h, imgpath, ifd, st.st_mode & 0777, (int32_t)st.st_mtime);
	close(ifd);
	if (rc < 0) {
		fprintf(stderr, "put: %s: %s\n", imgpath, fs_errstr(rc));
		rw_close(&h);
		return 1;
	}
	printf("%s -> %s (%lld bytes)\n", hostpath, imgpath, (long long)st.st_size);
	rw_close(&h);
	return 0;
}

/* ------------------------------------------------------------------ *
 * cp / mv (within the image)
 * ------------------------------------------------------------------ */

int
cmd_cp(int argc, char **argv)
{
	RW h;
	uint32_t bsize = 0, plen = 0;
	int forced = -1, c, rc, sh, dh;
	const char *image, *src, *dst, *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "B:A:d:P:o:")) != -1) {
		switch (c) {
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) {
				fprintf(stderr, "cp: bad -A\n");
				return 2;
			}
			forced = (int)e;
			break;
		}
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
			goto usage;
		}
	}
	if (optind + 3 != argc)
		goto usage;
	image = argv[optind];
	src = argv[optind + 1];
	sh = fs_host_path(&src);
	dst = argv[optind + 2];
	dh = fs_host_path(&dst);
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	/* need write access only when the destination is inside the image */
	if (rw_open(&h, image, bsize, forced, !dh, base) < 0) {
		fprintf(stderr, "cp: %s: cannot open %s (try -B/-A, or -d/-P)\n", image, dh ? "for reading" : "read-write");
		return 1;
	}
	rc = fs_copy(&h, src, sh, dst, dh);
	if (rc < 0) {
		fprintf(stderr, "cp: %s\n", fs_errstr(rc));
		rw_close(&h);
		return 1;
	}
	rw_close(&h);
	return 0;
usage:
	fprintf(stderr,
		"usage: s5fs cp [-B ..] [-A ..] image src dst\n"
		"       paths are in the image; prefix a HOST path with '@':\n"
		"         s5fs cp img @./file.c /src/file.c   (host  -> image)\n"
		"         s5fs cp img /etc/passwd @./passwd   (image -> host)\n"
		"         s5fs cp img /a /b                   (image -> image)\n");
	return 2;
}

int
cmd_mv(int argc, char **argv)
{
	RW h;
	int first, rc;
	first = fs_open(argc, argv, &h, 1, "src dst");
	if (first < 0)
		return 1;
	if (first + 2 != argc) {
		fprintf(stderr, "usage: s5fs mv image src dst\n");
		rw_close(&h);
		return 2;
	}
	rc = rw_rename(&h, argv[first], argv[first + 1]);
	if (rc < 0) {
		fprintf(stderr, "mv: %s -> %s: %s\n", argv[first], argv[first + 1], fs_errstr(rc));
		rw_close(&h);
		return 1;
	}
	rw_close(&h);
	return 0;
}

/* ------------------------------------------------------------------ *
 * rm / rmdir
 * ------------------------------------------------------------------ */

int
cmd_rm(int argc, char **argv)
{
	RW h;
	int first, i, rc = 0;
	first = fs_open(argc, argv, &h, 1, "path...");
	if (first < 0)
		return 1;
	if (first >= argc) {
		fprintf(stderr, "rm: need a path\n");
		rw_close(&h);
		return 2;
	}
	for (i = first; i < argc; i++) {
		int r = rw_unlink(&h, argv[i]);
		if (r < 0) {
			fprintf(stderr, "rm: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

int
cmd_rmdir(int argc, char **argv)
{
	RW h;
	int first, i, rc = 0;
	first = fs_open(argc, argv, &h, 1, "dir...");
	if (first < 0)
		return 1;
	if (first >= argc) {
		fprintf(stderr, "rmdir: need a directory\n");
		rw_close(&h);
		return 2;
	}
	for (i = first; i < argc; i++) {
		int r = rw_rmdir(&h, argv[i]);
		if (r < 0) {
			fprintf(stderr, "rmdir: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

/* ------------------------------------------------------------------ *
 * mkdir (-p)
 * ------------------------------------------------------------------ */

static int
mkdir_p(RW *h, const char *path)
{
	char buf[2048];
	size_t i;
	int rc;
	uint32_t ino = rw_namei(h, path);
	if (ino)
		return 0; /* already exists */
	snprintf(buf, sizeof buf, "%s", path);
	for (i = 1; buf[i]; i++) { /* create each intermediate component */
		if (buf[i] == '/') {
			buf[i] = '\0';
			if (!rw_namei(h, buf)) {
				rc = rw_mkdir(h, buf, 0755);
				if (rc < 0 && rc != -EEXIST)
					return rc;
			}
			buf[i] = '/';
		}
	}
	rc = rw_mkdir(h, buf, 0755);
	return (rc == -EEXIST) ? 0 : rc;
}

int
cmd_mkdir(int argc, char **argv)
{
	RW h;
	int parents = 0, c, i, rc = 0;
	uint32_t bsize = 0, plen = 0;
	int forced = -1;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "pB:A:d:P:o:")) != -1) {
		switch (c) {
		case 'p':
			parents = 1;
			break;
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) {
				fprintf(stderr, "mkdir: bad -A\n");
				return 2;
			}
			forced = (int)e;
			break;
		}
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
			fprintf(stderr, "usage: s5fs mkdir [-p] image dir...\n");
			return 2;
		}
	}
	if (optind + 1 >= argc) {
		fprintf(stderr, "usage: s5fs mkdir [-p] image dir...\n");
		return 2;
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return 2;
	if (rw_open(&h, argv[optind], bsize, forced, 1, base) < 0) {
		fprintf(stderr, "mkdir: %s: not a writable s5fs image\n", argv[optind]);
		return 1;
	}
	for (i = optind + 1; i < argc; i++) {
		int r = parents ? mkdir_p(&h, argv[i]) : rw_mkdir(&h, argv[i], 0755);
		if (r < 0) {
			fprintf(stderr, "mkdir: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

/* ------------------------------------------------------------------ *
 * chmod
 * ------------------------------------------------------------------ */

int
cmd_chmod(int argc, char **argv)
{
	RW h;
	int first, i, rc = 0;
	unsigned perm;
	first = fs_open(argc, argv, &h, 1, "mode path...");
	if (first < 0)
		return 1;
	if (first + 1 >= argc) {
		fprintf(stderr, "usage: s5fs chmod image mode path...\n");
		rw_close(&h);
		return 2;
	}
	if (fs_parse_mode(argv[first], &perm) < 0) {
		fprintf(stderr, "chmod: %s: not an octal mode\n", argv[first]);
		return 2;
	}
	for (i = first + 1; i < argc; i++) {
		int r = rw_chmod(&h, argv[i], perm);
		if (r < 0) {
			fprintf(stderr, "chmod: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

/* ------------------------------------------------------------------ *
 * chown / chgrp
 * ------------------------------------------------------------------ */

int
cmd_chown(int argc, char **argv)
{
	RW h;
	int first, i, rc = 0, uid, gid;
	first = fs_open(argc, argv, &h, 1, "uid[:gid] path...");
	if (first < 0)
		return 1;
	if (first + 1 >= argc) {
		fprintf(stderr, "usage: s5fs chown image uid[:gid] path...\n");
		rw_close(&h);
		return 2;
	}
	if (fs_parse_owner(argv[first], &uid, &gid) < 0) {
		fprintf(stderr, "chown: %s: expected uid[:gid] as numbers\n", argv[first]);
		return 2;
	}
	for (i = first + 1; i < argc; i++) {
		int r = rw_chown(&h, argv[i], uid, gid);
		if (r < 0) {
			fprintf(stderr, "chown: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

int
cmd_chgrp(int argc, char **argv)
{
	RW h;
	int first, i, rc = 0, gid;
	first = fs_open(argc, argv, &h, 1, "gid path...");
	if (first < 0)
		return 1;
	if (first + 1 >= argc) {
		fprintf(stderr, "usage: s5fs chgrp image gid path...\n");
		rw_close(&h);
		return 2;
	}
	{
		long g;
		char *e;
		errno = 0;
		g = strtol(argv[first], &e, 10);
		if (!argv[first][0] || *e || errno == ERANGE || g < 0) {
			fprintf(stderr, "chgrp: %s: not a numeric group id\n", argv[first]);
			return 2;
		}
		gid = (int)g;
	}
	for (i = first + 1; i < argc; i++) {
		int r = rw_chown(&h, argv[i], -1, gid);
		if (r < 0) {
			fprintf(stderr, "chgrp: %s: %s\n", argv[i], fs_errstr(r));
			rc = 1;
		}
	}
	rw_close(&h);
	return rc;
}

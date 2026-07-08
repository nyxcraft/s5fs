/*
 * s5fs tar -- build an s5fs image from a (ustar/GNU) tar archive.
 *
 * Reads the archive into the in-memory tree (tree.c), which tolerates entries
 * in any order and materializes missing parent directories, then serializes it
 * to the image.  File contents stream straight from the archive at (offset,
 * size), so the whole archive is not held in memory.  Metadata (mode, uid,
 * gid) is taken from the tar header, so a faithful root tarball reproduces its
 * ownership.  Hard links ('1') share one inode; device nodes ('3'/'4') use the
 * archive's major/minor (meaningful only if it came from a compatible system).
 * Symlinks and fifos have no s5fs equivalent and are skipped (reported).
 *
 * Compressed archives (.tar.gz/.tgz, .tar.bz2, .tar.Z) are detected by magic
 * and decompressed to a temp file first (via gzip/bzip2 -- no build dependency,
 * just those tools at runtime), since the reader needs to seek.
 *
 * usage: s5fs tar [-B 512|1024] [-a pdp11|le|be]
 *                 [-d device | -b blocks | -s sectors] [-t mtime]
 *                 archive[.gz|.bz2|.Z] image
 */

#define _POSIX_C_SOURCE 200809L

#include "tree.h"
#include "fsread.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static unsigned long g_reg, g_dir, g_dev, g_lnk, g_skip;

static void usage(void)
{
	fprintf(stderr,
	    "usage: s5fs tar [-B 512|1024] [-a pdp11|le|be]\n"
	    "                [-d device | -b blocks | -s sectors] [-t mtime]\n"
	    "                archive.tar image\n");
	exit(2);
}

static unsigned long must_num(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0') { fprintf(stderr, "s5fs tar: bad %s: %s\n", what, s); exit(2); }
	return v;
}

/* octal field -> long (space/NUL terminated, leading blanks allowed) */
static long octal(const uint8_t *p, int len)
{
	long v = 0;
	int i = 0;
	while (i < len && (p[i] == ' ' || p[i] == '\0')) i++;
	for (; i < len && p[i] >= '0' && p[i] <= '7'; i++)
		v = v * 8 + (p[i] - '0');
	return v;
}

static int all_zero(const uint8_t *h)
{
	int i;
	for (i = 0; i < 512; i++)
		if (h[i]) return 0;
	return 1;
}

static long header_chksum(const uint8_t *h)
{
	long s = 0;
	int i;
	for (i = 0; i < 512; i++)
		s += (i >= 148 && i < 156) ? ' ' : h[i];	/* chksum field read as spaces */
	return s;
}

/* Open `path`; if it's gzip/bzip2/compress, decompress to a temp file and
 * return a seekable fd on that (its path goes in `tmp` for later unlink).
 * For a plain tar, returns the file's fd and leaves tmp empty. -1 on error. */
static int open_archive(const char *path, char *tmp, size_t tmpsz)
{
	unsigned char m[3] = {0, 0, 0};
	const char *prog = NULL, *tdir;
	int fd, tfd, rfd, status;
	pid_t pid;

	tmp[0] = '\0';
	fd = open(path, O_RDONLY);
	if (fd < 0) { fprintf(stderr, "s5fs tar: %s: %s\n", path, strerror(errno)); return -1; }
	if (read(fd, m, 3) < 0) { close(fd); return -1; }
	lseek(fd, 0, SEEK_SET);

	if (m[0] == 0x1f && m[1] == 0x8b)               prog = "gzip";   /* .gz  */
	else if (m[0] == 0x1f && m[1] == 0x9d)          prog = "gzip";   /* .Z   */
	else if (m[0] == 'B' && m[1] == 'Z' && m[2] == 'h') prog = "bzip2"; /* .bz2 */
	if (!prog)
		return fd;			/* plain, seekable tar */

	tdir = getenv("TMPDIR");
	if (!tdir || !*tdir) tdir = "/tmp";
	snprintf(tmp, tmpsz, "%s/s5fstar.XXXXXX", tdir);
	tfd = mkstemp(tmp);
	if (tfd < 0) { close(fd); tmp[0] = '\0'; fprintf(stderr, "s5fs tar: mkstemp: %s\n", strerror(errno)); return -1; }

	pid = fork();
	if (pid < 0) { close(fd); close(tfd); unlink(tmp); tmp[0] = '\0'; return -1; }
	if (pid == 0) {				/* child: prog -dc < archive > tmp */
		dup2(fd, 0);
		dup2(tfd, 1);
		close(fd); close(tfd);
		execlp(prog, prog, "-dc", (char *)NULL);
		_exit(127);
	}
	close(fd); close(tfd);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "s5fs tar: %s failed to decompress %s (is it installed?)\n", prog, path);
		unlink(tmp); tmp[0] = '\0'; return -1;
	}
	rfd = open(tmp, O_RDONLY);		/* fresh fd at offset 0, seekable */
	if (rfd < 0) { unlink(tmp); tmp[0] = '\0'; return -1; }
	return rfd;
}

static int readblk(int fd, long pos, uint8_t *b)
{
	ssize_t r;
	if (lseek(fd, pos, SEEK_SET) < 0) return -1;
	r = read(fd, b, 512);
	if (r == 0) return 0;			/* clean EOF */
	return (r == 512) ? 512 : -1;
}

/* parse the tar at `fd` into `root`; return 0 ok, -1 on a malformed archive */
static int parse_tar(int fd, tnode *root)
{
	uint8_t h[512];
	char longname[2048];
	int haslong = 0;
	long pos = 0;

	for (;;) {
		char name[2048], link[128];
		long size, off;
		int rc = readblk(fd, pos, h);
		int type;
		tnode *n;

		if (rc == 0) break;
		if (rc < 0) { fprintf(stderr, "s5fs tar: read error at %ld\n", pos); return -1; }
		if (all_zero(h)) break;			/* end-of-archive */
		if (header_chksum(h) != octal(h + 148, 8)) {
			fprintf(stderr, "s5fs tar: bad header checksum at %ld (not a tar?)\n", pos);
			return -1;
		}

		size = octal(h + 124, 12);
		off  = pos + 512;
		type = h[156];

		if (type == 'L') {			/* GNU long name in the data */
			int m = size < (long)sizeof longname - 1 ? (int)size : (int)sizeof longname - 1;
			if (lseek(fd, off, SEEK_SET) < 0 || read(fd, longname, m) != m) return -1;
			longname[m] = '\0';
			haslong = 1;
			pos = off + (size + 511) / 512 * 512;
			continue;
		}
		if (type == 'x' || type == 'g' || type == 'K') {  /* pax / long-link: skip */
			pos = off + (size + 511) / 512 * 512;
			continue;
		}

		if (haslong) {
			snprintf(name, sizeof name, "%s", longname);
			haslong = 0;
		} else if (h[345]) {			/* ustar prefix + name */
			snprintf(name, sizeof name, "%.155s/%.100s", (char *)h + 345, (char *)h);
		} else {
			snprintf(name, sizeof name, "%.100s", (char *)h);
		}

		switch (type) {
		case '5':				/* directory */
			n = tree_insert(root, name);
			n->kind = TN_DIR;
			n->perm = (uint16_t)octal(h + 100, 8);
			n->uid  = (int16_t)octal(h + 108, 8);
			n->gid  = (int16_t)octal(h + 116, 8);
			n->atime = n->mtime = n->ctime = (int32_t)octal(h + 136, 12);
			g_dir++;
			break;
		case '0': case '\0': case '7':		/* regular file */
			n = tree_insert(root, name);
			n->kind = TN_REG;
			n->perm = (uint16_t)octal(h + 100, 8);
			n->uid  = (int16_t)octal(h + 108, 8);
			n->gid  = (int16_t)octal(h + 116, 8);
			n->atime = n->mtime = n->ctime = (int32_t)octal(h + 136, 12);
			n->src_fd = fd;
			n->src_off = off;
			n->size = (uint32_t)size;
			g_reg++;
			break;
		case '1':				/* hard link */
			snprintf(link, sizeof link, "%.100s", (char *)h + 157);
			n = tree_insert(root, name);
			n->kind = TN_LINK;
			n->linkto = tree_find(root, link);
			if (!n->linkto)
				fprintf(stderr, "s5fs tar: hard link %s -> %s: target not seen (skipped)\n", name, link);
			g_lnk++;
			break;
		case '3': case '4':			/* char / block device */
			n = tree_insert(root, name);
			n->kind = TN_DEV;
			n->isblk = (type == '4');
			n->perm  = (uint16_t)octal(h + 100, 8);
			n->uid   = (int16_t)octal(h + 108, 8);
			n->gid   = (int16_t)octal(h + 116, 8);
			n->atime = n->mtime = n->ctime = (int32_t)octal(h + 136, 12);
			n->major = (int)octal(h + 329, 8);
			n->minor = (int)octal(h + 337, 8);
			g_dev++;
			break;
		default:				/* symlink '2', fifo '6', ... */
			g_skip++;
			break;
		}
		pos = off + (size + 511) / 512 * 512;
	}
	return 0;
}

static int tar_extract(int argc, char **argv)
{
	s5fs_opts opts;
	S5FS fs;
	tnode *root;
	const char *tarpath, *image, *ospec = NULL;
	const disk_dev *dev = NULL;
	unsigned long blocks = 0, sectors = 0, per;
	uint32_t plen = 0;
	char part = 0;
	long long base = 0;
	char tmp[1024];
	int tarfd, fd, c;

	memset(&opts, 0, sizeof opts);
	opts.mtime = -1;

	while ((c = getopt(argc, argv, "B:a:d:b:s:t:P:o:")) != -1) {
		switch (c) {
		case 'B': opts.bsize = (uint32_t)must_num(optarg, "block size"); break;
		case 'b': blocks     = must_num(optarg, "block count");         break;
		case 's': sectors    = must_num(optarg, "sector count");        break;
		case 't': opts.mtime = (int64_t)must_num(optarg, "mtime");      break;
		case 'P': part = optarg[0];                                     break;
		case 'o': ospec = optarg;                                       break;
		case 'a':
			opts.endian = s5_endian_parse(optarg);
			if (opts.endian == S5_NENDIAN) { fprintf(stderr, "s5fs tar: bad -a\n"); return 2; }
			break;
		case 'd':
			dev = device_find(optarg);
			if (!dev) { fprintf(stderr, "s5fs tar: unknown device '%s'\n", optarg); return 2; }
			break;
		default: usage();
		}
	}
	if (optind != argc - 2) usage();
	tarpath = argv[optind];
	image   = argv[optind + 1];

	if (opts.bsize == 0) opts.bsize = 1024;
	if (opts.bsize != 512 && opts.bsize != 1024) { fprintf(stderr, "s5fs tar: block size must be 512 or 1024\n"); return 2; }
	per = opts.bsize / 512;
	if (dev) { if (blocks || sectors) { fprintf(stderr, "s5fs tar: give one of -d/-b/-s\n"); return 2; } sectors = dev->blocks; }
	if (sectors) { if (blocks) { fprintf(stderr, "s5fs tar: give -b or -s, not both\n"); return 2; } blocks = sectors / per; }
	if (device_resolve_part(dev ? dev->name : NULL, part, ospec, &base, &plen) < 0) return 2;
	if (base != 0 || plen != 0) {
		if (plen == 0) { fprintf(stderr, "s5fs tar: partition needs a length (use -d -P, or -o START:LEN)\n"); return 2; }
		opts.base = base;
		blocks = plen / per;
	}
	if (blocks == 0) { fprintf(stderr, "s5fs tar: need a size (-d, -b, or -s)\n"); usage(); }

	tarfd = open_archive(tarpath, tmp, sizeof tmp);
	if (tarfd < 0) return 1;

	root = tree_root();
	if (parse_tar(tarfd, root) < 0) { close(tarfd); if (tmp[0]) unlink(tmp); return 1; }

	opts.ninode = tree_count_inodes(root) + 8;

	if (opts.base != 0 || plen != 0) {		/* into a partition: grow, don't truncate */
		off_t need = opts.base + (off_t)plen * 512;
		struct stat st;
		if (dev && (off_t)dev->blocks * 512 > need) need = (off_t)dev->blocks * 512;
		fd = open(image, O_RDWR | O_CREAT, 0666);
		if (fd < 0) { fprintf(stderr, "s5fs tar: %s: %s\n", image, strerror(errno)); close(tarfd); if (tmp[0]) unlink(tmp); return 1; }
		if (fstat(fd, &st) == 0 && st.st_size < need && ftruncate(fd, need) < 0) {
			fprintf(stderr, "s5fs tar: %s: %s\n", image, strerror(errno)); close(fd); close(tarfd); if (tmp[0]) unlink(tmp); return 1;
		}
	} else {
		fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) { fprintf(stderr, "s5fs tar: %s: %s\n", image, strerror(errno)); close(tarfd); if (tmp[0]) unlink(tmp); return 1; }
	}

	if (s5fs_begin(&fs, fd, (uint32_t)blocks, &opts) < 0) {
		fprintf(stderr, "s5fs tar: %s\n", fs.err); close(fd); close(tarfd); if (tmp[0]) unlink(tmp); return 1;
	}
	fs.s_tfree = 0;
	s5fs_freelist(&fs);
	tree_serialize(&fs, root);	/* streams file data from tarfd */
	s5fs_finish(&fs);

	if (close(fd) < 0 || fs.error) {
		fprintf(stderr, "s5fs tar: %s\n", fs.error ? fs.err : strerror(errno));
		close(tarfd); if (tmp[0]) unlink(tmp); return 1;
	}
	close(tarfd);
	if (tmp[0]) unlink(tmp);

	printf("%s: %s, %lu %u-byte blocks; %lu files, %lu dirs, %lu dev nodes, "
	       "%lu hard links; %d free blocks left\n",
	       image, fs.bo->name, blocks, fs.bsize, g_reg, g_dir, g_dev, g_lnk, fs.s_tfree);
	if (g_skip)
		fprintf(stderr, "  skipped %lu unsupported entries (symlinks/fifos/etc.)\n", g_skip);
	return 0;
}

/* ============================ create (image -> tar) ============================ */

/* hard-link table: s5fs inode -> the first archived path */
static struct thl { uint32_t ino; char *path; } *g_thl;
static size_t g_nthl, g_thlcap;
static const char *thl_find(uint32_t ino)
{
	size_t i;
	for (i = 0; i < g_nthl; i++) if (g_thl[i].ino == ino) return g_thl[i].path;
	return NULL;
}
static void thl_add(uint32_t ino, const char *path)
{
	if (g_nthl == g_thlcap) {
		g_thlcap = g_thlcap ? g_thlcap * 2 : 64;
		g_thl = realloc(g_thl, g_thlcap * sizeof *g_thl);
		if (!g_thl) { perror("s5fs tar"); exit(1); }
	}
	g_thl[g_nthl].ino = ino;
	g_thl[g_nthl].path = strdup(path);
	g_nthl++;
}

static void octfield(uint8_t *p, long v, int w)		/* w-byte octal + NUL */
{
	char t[16];
	snprintf(t, sizeof t, "%0*lo", w - 1, (unsigned long)v);
	memcpy(p, t, w - 1);
	p[w - 1] = '\0';
}

static void ustar_hdr(uint8_t *h, const char *name, unsigned mode, int uid, int gid,
                      long size, long mtime, char type, const char *link, int maj, int min_)
{
	unsigned sum;
	int i;
	size_t nl = strlen(name);

	memset(h, 0, 512);
	if (nl <= 100) {
		memcpy(h, name, nl);
	} else {					/* split name/prefix at a '/' */
		const char *s = name + nl - 100;
		while (*s && *s != '/') s++;
		if (*s == '/' && (size_t)(s - name) <= 155) {
			memcpy(h + 345, name, s - name);
			memcpy(h, s + 1, strlen(s + 1));
		} else {
			memcpy(h, name, 100);
		}
	}
	octfield(h + 100, mode & 07777, 8);
	octfield(h + 108, uid, 8);
	octfield(h + 116, gid, 8);
	octfield(h + 124, size, 12);
	octfield(h + 136, mtime, 12);
	h[156] = type;
	if (link) { size_t l = strlen(link); memcpy(h + 157, link, l > 100 ? 100 : l); }
	memcpy(h + 257, "ustar", 5);			/* magic (+NUL @262) */
	h[263] = '0'; h[264] = '0';			/* version "00" */
	memcpy(h + 265, "root", 4);			/* uname */
	memcpy(h + 297, "root", 4);			/* gname */
	if (type == '3' || type == '4') { octfield(h + 329, maj, 8); octfield(h + 337, min_, 8); }
	memset(h + 148, ' ', 8);			/* chksum field = spaces */
	sum = 0; for (i = 0; i < 512; i++) sum += h[i];
	snprintf((char *)h + 148, 8, "%06o", sum);
	h[154] = '\0'; h[155] = ' ';
}

static void wr(int fd, const void *b, size_t n)
{
	if (write(fd, b, n) != (ssize_t)n) { perror("s5fs tar: write"); exit(1); }
}

/* emit one entry (and its data) to the archive */
static void tar_emit(FSR *r, int out, uint32_t ino, const char *path, const fsr_inode *in)
{
	uint8_t h[512];
	unsigned mode = in->mode & 07777;
	int uid = (uint16_t)in->uid, gid = (uint16_t)in->gid;

	switch (in->mode & P11_IFMT) {
	case P11_IFDIR: {
		char d[1216];
		size_t pl = strlen(path);
		if (pl > sizeof d - 2) pl = sizeof d - 2;
		memcpy(d, path, pl); d[pl] = '/'; d[pl + 1] = '\0';
		ustar_hdr(h, d, mode, uid, gid, 0, in->mtime, '5', NULL, 0, 0);
		wr(out, h, 512);
		g_dir++;
		break;
	}
	case P11_IFREG: {
		const char *lp = (in->nlink > 1) ? thl_find(ino) : NULL;
		if (lp) {
			ustar_hdr(h, path, mode, uid, gid, 0, in->mtime, '1', lp, 0, 0);
			wr(out, h, 512);
			g_lnk++;
		} else {
			long off = 0, size = in->size;
			ustar_hdr(h, path, mode, uid, gid, size, in->mtime, '0', NULL, 0, 0);
			wr(out, h, 512);
			while (off < size) {
				uint8_t blk[512];
				long want = size - off; if (want > 512) want = 512;
				memset(blk, 0, 512);
				fsr_readfile(r, in, blk, want, off);
				wr(out, blk, 512);
				off += want;
			}
			if (in->nlink > 1) thl_add(ino, path);
			g_reg++;
		}
		break;
	}
	case P11_IFCHR: case P11_IFBLK:
		ustar_hdr(h, path, mode, uid, gid, 0, in->mtime,
		          (in->mode & P11_IFMT) == P11_IFBLK ? '4' : '3', NULL,
		          (in->addr[0] >> 8) & 0377, in->addr[0] & 0377);
		wr(out, h, 512);
		g_dev++;
		break;
	}
}

static int walk_cb(void *arg, uint32_t ino, const char *name);
struct walkctx { FSR *r; int out; const char *prefix; };

static void tar_walk(FSR *r, int out, uint32_t dirino, const char *prefix)
{
	fsr_inode in;
	struct walkctx c;
	if (fsr_iget(r, dirino, &in) < 0) return;
	c.r = r; c.out = out; c.prefix = prefix;
	fsr_readdir(r, &in, walk_cb, &c);
}

static int walk_cb(void *arg, uint32_t ino, const char *name)
{
	struct walkctx *c = arg;
	char path[1200];
	fsr_inode in;
	if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
	if (c->prefix[0]) snprintf(path, sizeof path, "%s/%s", c->prefix, name);
	else              snprintf(path, sizeof path, "%s", name);
	if (fsr_iget(c->r, ino, &in) < 0) return 0;
	tar_emit(c->r, c->out, ino, path, &in);
	if ((in.mode & P11_IFMT) == P11_IFDIR)
		tar_walk(c->r, c->out, ino, path);
	return 0;
}

static int tar_create(int argc, char **argv)
{
	FSR r;
	const char *image, *archive, *dev = NULL, *ospec = NULL;
	uint32_t bsize = 0, plen = 0;
	int forced = -1, out, c;
	char part = 0; long long base = 0;
	uint8_t zero[1024];

	while ((c = getopt(argc, argv, "B:A:d:P:o:")) != -1) {
		switch (c) {
		case 'B': bsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'A': {
			s5_endian e = s5_endian_parse(optarg);
			if (e == S5_NENDIAN) { fprintf(stderr, "s5fs tar c: bad -A\n"); return 2; }
			forced = (int)e; break;
		}
		case 'd': dev = optarg; break;
		case 'P': part = optarg[0]; break;
		case 'o': ospec = optarg; break;
		default: fprintf(stderr, "usage: s5fs tar c [-B 512|1024] [-A pdp11|le|be] [-d dev -P part | -o blk] image archive\n"); return 2;
		}
	}
	if (optind != argc - 2) { fprintf(stderr, "usage: s5fs tar c [-B 512|1024] [-A pdp11|le|be] [-d dev -P part | -o blk] image archive\n"); return 2; }
	image = argv[optind]; archive = argv[optind + 1];
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0) return 2;

	if (fsr_open(&r, image, bsize, forced, base) < 0) {
		fprintf(stderr, "s5fs tar c: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", image);
		return 1;
	}
	out = open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) { fprintf(stderr, "s5fs tar c: %s: %s\n", archive, strerror(errno)); fsr_close(&r); return 1; }

	tar_walk(&r, out, P11_ROOTINO, "");
	memset(zero, 0, sizeof zero);
	wr(out, zero, sizeof zero);			/* two zero blocks end the archive */
	close(out);
	fsr_close(&r);
	printf("%s: %lu files, %lu dirs, %lu dev nodes, %lu hard links -> %s\n",
	       image, g_reg, g_dir, g_dev, g_lnk, archive);
	return 0;
}

int cmd_tar(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "x")) return tar_extract(argc - 1, argv + 1);
	if (argc >= 2 && !strcmp(argv[1], "c")) return tar_create(argc - 1, argv + 1);
	fprintf(stderr,
	    "usage: s5fs tar c [opts] image archive   (create archive from image)\n"
	    "       s5fs tar x [opts] archive image   (extract archive into image)\n");
	return 2;
}

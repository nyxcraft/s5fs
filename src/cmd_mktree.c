/*
 * s5fs mktree -- build a populated s5fs image from a host directory tree.
 *
 * Creates a fresh filesystem (like mkfs) sized for a device, then copies a
 * host directory into it: directories become s5fs directories, regular files
 * are stored with the full direct/single/double/triple-indirect map, all
 * owned by root (uid/gid 0) with the host permission bits.  This is the
 * writer front-end the "rebuild 2.9BSD from source -> image" pipeline uses to
 * lay a built root tree onto a .dsk.
 *
 * v1 limits (each will grow): symlinks don't exist in s5fs and are skipped;
 * device/special files are skipped (a /dev spec pass comes later); hard links
 * are not coalesced (each becomes its own inode).  All are reported.
 *
 * usage: s5fs mktree [-B 512|1024|2048] [-a pdp11|le|be]
 *                    [-d device | -b blocks | -s sectors] [-t mtime]
 *                    rootdir image
 */

#define _POSIX_C_SOURCE 200809L

#include "s5fs_core.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>

/* run stats */
static unsigned long g_files, g_dirs, g_devs, g_links, g_skip_link, g_skip_spec, g_skip_open,
	g_skip_long;

/* hard-link table: host (dev,ino) -> the single s5fs inode we gave it.  Only
 * multiply-linked files are recorded, so it stays small. */
static struct hlink {
	dev_t dev;
	ino_t ino;
	uint32_t s5ino;
} *g_hl;

static size_t g_nhl, g_hlcap;

static uint32_t
hl_find(dev_t dev, ino_t ino)
{
	size_t i;
	for (i = 0; i < g_nhl; i++)
		if (g_hl[i].dev == dev && g_hl[i].ino == ino)
			return g_hl[i].s5ino;
	return 0;
}

static void
hl_add(dev_t dev, ino_t ino, uint32_t s5ino)
{
	if (g_nhl == g_hlcap) {
		g_hlcap = g_hlcap ? g_hlcap * 2 : 64;
		g_hl = realloc(g_hl, g_hlcap * sizeof *g_hl);
		if (!g_hl) {
			perror("s5fs mktree");
			exit(1);
		}
	}
	g_hl[g_nhl].dev = dev;
	g_hl[g_nhl].ino = ino;
	g_hl[g_nhl].s5ino = s5ino;
	g_nhl++;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: s5fs mktree [-B 512|1024|2048] [-a pdp11|le|be]\n"
		"                   [-d device | -b blocks | -s sectors] [-t mtime]\n"
		"                   [-D devspec] rootdir image\n"
		"\n"
		"  -D file  build /dev from a spec: lines \"name c|b major minor [mode]\"\n"
		"  (a lost+found directory is always created at the root)\n");
	exit(2);
}

static unsigned long
must_num(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0') {
		fprintf(stderr, "s5fs mktree: bad %s: %s\n", what, s);
		exit(2);
	}
	return v;
}

/* count regular files + directories under `host` (what we will store) */
static void
count_tree(const char *host, uint32_t *n)
{
	DIR *d;
	struct dirent *de;
	char child[4096];
	struct stat st;

	(*n)++; /* this directory */
	d = opendir(host);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(child, sizeof child, "%s/%s", host, de->d_name);
		if (lstat(child, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			count_tree(child, n);
		else if (S_ISREG(st.st_mode))
			(*n)++;
	}
	closedir(d);
}

/* store `len` bytes of `data` as inode `in`'s file content */
static void
store_bytes(S5FS *fs, s5fs_inode *in, const uint8_t *data, uint32_t len)
{
	uint32_t nblk = (len + fs->bsize - 1) / fs->bsize, b;
	int32_t *da = nblk ? malloc(nblk * sizeof *da) : NULL;
	uint8_t *buf = malloc(fs->bsize);

	if ((nblk && !da) || !buf) {
		free(da);
		free(buf);
		return;
	}
	for (b = 0; b < nblk; b++) {
		uint32_t off = b * fs->bsize, want = len - off;
		if (want > fs->bsize)
			want = fs->bsize;
		memset(buf, 0, fs->bsize);
		memcpy(buf, data + off, want);
		da[b] = s5fs_alloc(fs);
		s5fs_wtblk(fs, (uint32_t)da[b], buf);
	}
	s5fs_setblocks(fs, in, da, nblk);
	in->size = (int32_t)len;
	free(buf);
	free(da);
}

/* store the contents of open fd `src` (of `size` bytes) as inode `in` */
static void
store_fd(S5FS *fs, s5fs_inode *in, int src, uint32_t size)
{
	uint32_t nblk = (size + fs->bsize - 1) / fs->bsize, b;
	int32_t *da = nblk ? malloc(nblk * sizeof *da) : NULL;
	uint8_t *buf = malloc(fs->bsize);

	if ((nblk && !da) || !buf) {
		free(da);
		free(buf);
		return;
	}
	for (b = 0; b < nblk; b++) {
		uint32_t want = size - b * fs->bsize, got = 0;
		if (want > fs->bsize)
			want = fs->bsize;
		memset(buf, 0, fs->bsize);
		while (got < want) {
			ssize_t r = read(src, buf + got, want - got);
			if (r <= 0)
				break;
			got += (uint32_t)r;
		}
		da[b] = s5fs_alloc(fs);
		s5fs_wtblk(fs, (uint32_t)da[b], buf);
	}
	s5fs_setblocks(fs, in, da, nblk);
	in->size = (int32_t)size;
	free(buf);
	free(da);
}

static void
store_regfile(S5FS *fs, const char *host, uint32_t ino, mode_t hmode)
{
	s5fs_inode in;
	struct stat st;
	int fd = open(host, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "s5fs mktree: %s: %s (skipped)\n", host, strerror(errno));
		if (fd >= 0)
			close(fd);
		g_skip_open++;
		return;
	}
	memset(&in, 0, sizeof in);
	in.number = (uint16_t)ino;
	in.mode = (uint16_t)(P11_IFREG | (hmode & 07777));
	in.nlink = 1;
	in.atime = (int32_t)st.st_atime; /* preserve host file times */
	in.mtime = (int32_t)st.st_mtime;
	in.ctime = (int32_t)st.st_ctime;
	store_fd(fs, &in, fd, (uint32_t)st.st_size);
	s5fs_writeinode(fs, &in);
	close(fd);
	g_files++;
}

/* write a directory inode from an in-memory entry list */
static void
write_dir(S5FS *fs, uint32_t ino, uint32_t parent, uint16_t mode,
	  int16_t nlink, const uint16_t *cino, char (*cnm)[16], uint32_t nc)
{
	s5fs_inode in;
	uint32_t nent = nc + 2, len = nent * P11_DIRENTSZ, i;
	uint8_t *db = calloc(1, ((len + fs->bsize - 1) / fs->bsize) * fs->bsize);

	if (!db)
		return;
	fs->bo->put16(db + 0 * P11_DIRENTSZ, (uint16_t)ino);
	db[2] = '.';
	fs->bo->put16(db + 1 * P11_DIRENTSZ, (uint16_t)parent);
	db[P11_DIRENTSZ + 2] = '.';
	db[P11_DIRENTSZ + 3] = '.';
	for (i = 0; i < nc; i++) {
		uint8_t *e = db + (i + 2) * P11_DIRENTSZ;
		size_t l = strlen(cnm[i]);
		if (l > P11_DIRSIZ)
			l = P11_DIRSIZ;
		fs->bo->put16(e, cino[i]);
		memcpy((char *)e + 2, cnm[i], l);
	}
	memset(&in, 0, sizeof in);
	in.number = (uint16_t)ino;
	in.mode = mode;
	in.nlink = nlink;
	store_bytes(fs, &in, db, len);
	s5fs_writeinode(fs, &in);
	free(db);
	g_dirs++;
}

/* store one device special file (major/minor packed like mkfs: (maj<<8)|min) */
static void
store_devnode(S5FS *fs, uint32_t ino, int isblk, int maj, int min_, unsigned mode)
{
	s5fs_inode in;
	memset(&in, 0, sizeof in);
	in.number = (uint16_t)ino;
	in.mode = (uint16_t)((isblk ? P11_IFBLK : P11_IFCHR) | (mode & 07777));
	in.nlink = 1;
	in.addr[0] = ((maj & 0377) << 8) | (min_ & 0377);
	s5fs_writeinode(fs, &in);
	g_devs++;
}

/* an empty lost+found directory */
static void
build_lostfound(S5FS *fs, uint32_t ino, uint32_t parent)
{
	write_dir(fs, ino, parent, (uint16_t)(P11_IFDIR | 0700), 2, NULL, NULL, 0);
}

/* build /dev from a spec file: lines "name c|b major minor [octal-mode]" */
static void
build_devdir(S5FS *fs, uint32_t ino, uint32_t parent, const char *spec)
{
	struct dn {
		uint32_t ino;
		int isblk, maj, min_;
		unsigned mode;
	};
	struct dn *nodes = NULL;
	uint16_t *cino = NULL;
	char(*cnm)[16] = NULL;
	size_t nn = 0, cap = 0, i;
	FILE *f = fopen(spec, "r");
	char line[256];

	if (!f) {
		fprintf(stderr, "s5fs mktree: %s: %s (no /dev nodes)\n", spec, strerror(errno));
		write_dir(fs, ino, parent, (uint16_t)(P11_IFDIR | 0755), 2, NULL, NULL, 0);
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char nm[64], ty;
		int maj, min_;
		unsigned mode = 0600, got;
		char *p = line;
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p == '\0' || *p == '#')
			continue;
		got = sscanf(p, "%63s %c %d %d %o", nm, &ty, &maj, &min_, &mode);
		if (got < 4) {
			fprintf(stderr, "s5fs mktree: bad /dev line: %s", line);
			continue;
		}
		if (nn == cap) {
			cap = cap ? cap * 2 : 32;
			nodes = realloc(nodes, cap * sizeof *nodes);
			cino = realloc(cino, cap * sizeof *cino);
			cnm = realloc(cnm, cap * sizeof *cnm);
			if (!nodes || !cino || !cnm) {
				fclose(f);
				return;
			}
		}
		nodes[nn].ino = s5fs_ialloc(fs);
		nodes[nn].isblk = (ty == 'b' || ty == 'B');
		nodes[nn].maj = maj;
		nodes[nn].min_ = min_;
		nodes[nn].mode = mode;
		cino[nn] = (uint16_t)nodes[nn].ino;
		{
			size_t l = strlen(nm);
			if (l > 15)
				l = 15;
			memcpy(cnm[nn], nm, l);
			cnm[nn][l] = '\0';
		}
		nn++;
	}
	fclose(f);

	write_dir(fs, ino, parent, (uint16_t)(P11_IFDIR | 0755), 2, cino, cnm, (uint32_t)nn);
	for (i = 0; i < nn; i++)
		store_devnode(fs, nodes[i].ino, nodes[i].isblk, nodes[i].maj, nodes[i].min_, nodes[i].mode);
	free(nodes);
	free(cino);
	free(cnm);
}

/* build directory `host` as inode `ino` (parent `parent`); at the root level
 * (`root`) synthesize lost+found and, if `devspec`, a /dev built from it. */
static void
build_dir(S5FS *fs, const char *host, uint32_t ino, uint32_t parent,
	  const char *devspec, int root)
{
	enum {
		KID_HOST,
		KID_LOSTFOUND,
		KID_DEVDIR,
		KID_HARDLINK
	};

	struct kid {
		uint32_t ino;
		int kind;
		int isdir;
		mode_t hmode;
		char *host;
		char nm[16];
	};
	struct kid *kids = NULL;
	uint16_t *cino = NULL;
	char(*cnm)[16] = NULL;
	size_t nk = 0, cap = 0, i;
	int nsub = 0;
	DIR *d;
	struct dirent *de;
	struct stat st;

	d = opendir(host);
	if (!d) {
		fprintf(stderr, "s5fs mktree: %s: %s (skipped)\n", host, strerror(errno));
		g_skip_open++;
		return;
	}
	while ((de = readdir(d))) {
		char child[4096];
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (root && !strcmp(de->d_name, "lost+found"))
			continue; /* synthesized below */
		if (root && devspec && !strcmp(de->d_name, "dev"))
			continue; /* synthesized from the spec */
		snprintf(child, sizeof child, "%s/%s", host, de->d_name);
		if (lstat(child, &st) < 0)
			continue;
		if (S_ISLNK(st.st_mode)) {
			g_skip_link++;
			continue;
		}
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
			g_skip_spec++;
			continue;
		}
		/* A directory entry holds 14 name bytes and no terminator, so a
		 * longer host name cannot be stored.  Skip it loudly: truncating
		 * would silently collide with any sibling sharing its first 14
		 * characters, producing two entries with the same on-disk name
		 * that fsck still calls clean. */
		if (strlen(de->d_name) > P11_DIRSIZ) {
			fprintf(stderr, "s5fs mktree: %s: name longer than %d characters (skipped)\n",
				child, P11_DIRSIZ);
			g_skip_long++;
			continue;
		}
		if (nk == cap) {
			cap = cap ? cap * 2 : 16;
			kids = realloc(kids, cap * sizeof *kids);
			if (!kids) {
				closedir(d);
				return;
			}
		}
		kids[nk].isdir = S_ISDIR(st.st_mode);
		kids[nk].hmode = st.st_mode;
		kids[nk].host = strdup(child);
		strncpy(kids[nk].nm, de->d_name, sizeof kids[nk].nm - 1);
		kids[nk].nm[sizeof kids[nk].nm - 1] = '\0';
		if (S_ISREG(st.st_mode) && st.st_nlink > 1) {
			uint32_t seen = hl_find(st.st_dev, st.st_ino);
			if (seen) { /* another link to a stored inode */
				kids[nk].ino = seen;
				kids[nk].kind = KID_HARDLINK;
				nk++;
				continue;
			}
			kids[nk].ino = s5fs_ialloc(fs);
			hl_add(st.st_dev, st.st_ino, kids[nk].ino);
		}
		else {
			kids[nk].ino = s5fs_ialloc(fs);
		}
		kids[nk].kind = KID_HOST;
		if (kids[nk].isdir)
			nsub++;
		nk++;
	}
	closedir(d);

	if (root) { /* synthesize lost+found (+ /dev) */
		int add = devspec ? 2 : 1, j;
		if (nk + (size_t)add > cap) {
			cap = nk + add;
			kids = realloc(kids, cap * sizeof *kids);
			if (!kids)
				return;
		}
		for (j = 0; j < add; j++) {
			kids[nk].ino = s5fs_ialloc(fs);
			kids[nk].isdir = 1;
			kids[nk].host = NULL;
			if (j == 0) {
				kids[nk].kind = KID_LOSTFOUND;
				strcpy(kids[nk].nm, "lost+found");
			}
			else {
				kids[nk].kind = KID_DEVDIR;
				strcpy(kids[nk].nm, "dev");
			}
			nsub++;
			nk++;
		}
	}

	/* directory data (via write_dir): gather child (ino,name) arrays */
	cino = malloc((nk ? nk : 1) * sizeof *cino);
	cnm = malloc((nk ? nk : 1) * sizeof *cnm);
	if (!cino || !cnm) {
		free(cino);
		free(cnm);
		free(kids);
		return;
	}
	for (i = 0; i < nk; i++) {
		cino[i] = (uint16_t)kids[i].ino;
		strncpy(cnm[i], kids[i].nm, 15);
		cnm[i][15] = '\0';
	}
	if (stat(host, &st) < 0)
		st.st_mode = 0755;
	write_dir(fs, ino, parent, (uint16_t)(P11_IFDIR | (st.st_mode & 07777)),
		  (int16_t)(2 + nsub), cino, cnm, (uint32_t)nk);
	free(cino);
	free(cnm);

	for (i = 0; i < nk; i++) {
		if (fs->error)
			break;
		if (kids[i].kind == KID_HARDLINK) {
			s5fs_bumplink(fs, kids[i].ino);
			g_links++;
		}
		else if (kids[i].kind == KID_LOSTFOUND)
			build_lostfound(fs, kids[i].ino, ino);
		else if (kids[i].kind == KID_DEVDIR)
			build_devdir(fs, kids[i].ino, ino, devspec);
		else if (kids[i].isdir)
			build_dir(fs, kids[i].host, kids[i].ino, ino, NULL, 0);
		else
			store_regfile(fs, kids[i].host, kids[i].ino, kids[i].hmode);
		free(kids[i].host);
	}
	for (; i < nk; i++)
		free(kids[i].host);
	free(kids);
}

int
cmd_mktree(int argc, char **argv)
{
	s5fs_opts opts;
	S5FS fs;
	const char *rootdir, *image, *devspec = NULL, *ospec = NULL;
	const disk_dev *dev = NULL;
	unsigned long blocks = 0, sectors = 0, per;
	uint32_t ninode = 0, devn = 0, plen = 0;
	char part = 0;
	long long base = 0;
	struct stat st;
	int fd, c;

	memset(&opts, 0, sizeof opts);
	opts.mtime = -1;

	while ((c = getopt(argc, argv, "B:a:d:b:s:t:D:P:o:")) != -1) {
		switch (c) {
		case 'B':
			opts.bsize = (uint32_t)must_num(optarg, "block size");
			break;
		case 'b':
			blocks = must_num(optarg, "block count");
			break;
		case 's':
			sectors = must_num(optarg, "sector count");
			break;
		case 't':
			opts.mtime = (int64_t)must_num(optarg, "mtime");
			break;
		case 'D':
			devspec = optarg;
			break;
		case 'P':
			part = optarg[0];
			break;
		case 'o':
			ospec = optarg;
			break;
		case 'a':
			opts.endian = s5_endian_parse(optarg);
			if (opts.endian == S5_NENDIAN) {
				fprintf(stderr, "s5fs mktree: bad -a\n");
				return 2;
			}
			break;
		case 'd':
			dev = device_find(optarg);
			if (!dev) {
				fprintf(stderr, "s5fs mktree: unknown device '%s'\n", optarg);
				return 2;
			}
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 2)
		usage();
	rootdir = argv[optind];
	image = argv[optind + 1];

	if (stat(rootdir, &st) < 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "s5fs mktree: %s: not a directory\n", rootdir);
		return 1;
	}
	if (opts.bsize == 0)
		opts.bsize = 1024;
	if (!P11_BSIZE_OK(opts.bsize)) {
		fprintf(stderr, "s5fs mktree: block size must be 512, 1024, or 2048\n");
		return 2;
	}
	per = opts.bsize / 512;

	if (dev) {
		if (blocks || sectors) {
			fprintf(stderr, "s5fs mktree: give one of -d/-b/-s\n");
			return 2;
		}
		sectors = dev->blocks;
	}
	if (sectors) {
		if (blocks) {
			fprintf(stderr, "s5fs mktree: give -b or -s, not both\n");
			return 2;
		}
		blocks = sectors / per;
	}
	if (device_resolve_part(dev ? dev->name : NULL, part, ospec, &base, &plen) < 0)
		return 2;
	if (base != 0 || plen != 0) {
		if (plen == 0) {
			fprintf(stderr, "s5fs mktree: partition needs a length (use -d -P, or -o START:LEN)\n");
			return 2;
		}
		opts.base = base;
		blocks = plen / per;
	}
	if (blocks == 0) {
		fprintf(stderr, "s5fs mktree: need a size (-d, -b, or -s)\n");
		usage();
	}

	/* count /dev nodes so the i-list has room for them too */
	if (devspec) {
		FILE *sf = fopen(devspec, "r");
		char ln[256];
		if (sf) {
			while (fgets(ln, sizeof ln, sf)) {
				char *p = ln;
				while (*p && isspace((unsigned char)*p))
					p++;
				if (*p && *p != '#')
					devn++;
			}
			fclose(sf);
		}
	}
	/* size the i-list: tree + /dev nodes + (freelist, root, lost+found, dev) + margin */
	{
		uint32_t n = 0;
		count_tree(rootdir, &n);
		ninode = n + devn + 16;
	}
	opts.ninode = ninode;

	if (opts.base != 0 || plen != 0) { /* into a partition: grow, don't truncate */
		off_t need = opts.base + (off_t)plen * 512;
		if (dev && (off_t)dev->blocks * 512 > need)
			need = (off_t)dev->blocks * 512;
		fd = open(image, O_RDWR | O_CREAT, 0666);
		if (fd < 0) {
			fprintf(stderr, "s5fs mktree: %s: %s\n", image, strerror(errno));
			return 1;
		}
		if (fstat(fd, &st) == 0 && st.st_size < need && ftruncate(fd, need) < 0) {
			fprintf(stderr, "s5fs mktree: %s: %s\n", image, strerror(errno));
			close(fd);
			return 1;
		}
	}
	else {
		fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			fprintf(stderr, "s5fs mktree: %s: %s\n", image, strerror(errno));
			return 1;
		}
	}

	if (s5fs_begin(&fs, fd, (uint32_t)blocks, &opts) < 0) {
		fprintf(stderr, "s5fs mktree: %s\n", fs.err);
		close(fd);
		return 1;
	}
	fs.s_tfree = 0;
	s5fs_freelist(&fs);						    /* inode 1 + free list */
	build_dir(&fs, rootdir, s5fs_ialloc(&fs), P11_ROOTINO, devspec, 1); /* root = inode 2 */
	s5fs_finish(&fs);

	if (close(fd) < 0 || fs.error) {
		fprintf(stderr, "s5fs mktree: %s\n", fs.error ? fs.err : strerror(errno));
		return 1;
	}

	printf("%s: %s, %lu %u-byte blocks; %lu files, %lu dirs, %lu dev nodes, "
	       "%lu hard links; %d free blocks left\n",
	       image, fs.bo->name, blocks, fs.bsize, g_files, g_dirs, g_devs,
	       g_links, fs.s_tfree);
	if (g_skip_link || g_skip_spec || g_skip_open || g_skip_long)
		fprintf(stderr, "  skipped: %lu symlinks, %lu special files, %lu unreadable, "
				"%lu names over %d characters\n",
			g_skip_link, g_skip_spec, g_skip_open, g_skip_long, P11_DIRSIZ);
	return 0;
}

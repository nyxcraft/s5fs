/*
 * cmd_shell.c -- interactive explorer for s5fs images, with a mount table.
 *
 *   s5fs shell [-r] [-B ..] [-A ..] [-d dev -P part | -o blk] [image]
 *
 * You mount any number of disks/partitions at paths inside one namespace, then
 * do natural file operations -- `cp /a/etc/passwd /b/tmp` copies between two
 * different images because each path is routed (longest mountpoint prefix wins)
 * to the image mounted there.  A launch `image` is auto-mounted at "/".
 *
 *   mount hostfile mnt [-r] [-B n] [-A bo] [-d dev -P part] [-o blk]
 *   umount mnt ; mounts
 *   ls [-l] [-a] [path]  cd [path]  pwd  cat  stat
 *   get imgpath [host]   put host [imgpath]     ('@' prefix = a host path)
 *   cp [@]src [@]dst      mv src dst
 *   rm  mkdir  rmdir  chmod  chown  chgrp   help   quit
 *
 * Each mount is read/write unless mounted -r; a write to a read-only mount just
 * returns "Read-only file system".  Overlapping mounts are fine: the deepest
 * mountpoint that prefixes a path serves it.
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

struct mount {
	char *at;
	char *img;
	RW h;
	int ro;
};
static struct mount *mtab;
static int nmnt, capmnt;
static char cwd[2048] = "/";

/* absolute, normalised path for `arg` relative to cwd (static buffer) */
static const char *
rel(const char *arg)
{
	static char out[2048];
	fs_resolve(cwd, arg ? arg : ".", out, sizeof out);
	return out;
}

static void
parentof(const char *p, char *out, size_t n)
{
	const char *s = strrchr(p, '/');
	if (!s || s == p) {
		snprintf(out, n, "/");
		return;
	}
	{
		size_t L = (size_t)(s - p);
		if (L >= n)
			L = n - 1;
		memcpy(out, p, L);
		out[L] = '\0';
	}
}

/* route an absolute path to the mount that serves it (longest prefix); the
 * remainder within that image (starting "/") is written to `sub`. */
static struct mount *
route(const char *abspath, char *sub, size_t subsz)
{
	struct mount *best = NULL;
	size_t bestlen = 0;
	const char *bestrem = NULL;
	int i;
	for (i = 0; i < nmnt; i++) {
		struct mount *m = &mtab[i];
		size_t L = strlen(m->at);
		const char *rem = NULL;
		if (strcmp(m->at, "/") == 0)
			rem = abspath;
		else if (strcmp(abspath, m->at) == 0)
			rem = "/";
		else if (!strncmp(abspath, m->at, L) && abspath[L] == '/')
			rem = abspath + L;
		if (rem && (best == NULL || L > bestlen)) {
			best = m;
			bestlen = L;
			bestrem = rem;
		}
	}
	if (best)
		snprintf(sub, subsz, "%s", bestrem);
	return best;
}

/* is `p` a proper ancestor of some mountpoint (a browsable virtual directory)? */
static int
is_virtual_dir(const char *p)
{
	size_t L = strlen(p);
	int i;
	for (i = 0; i < nmnt; i++) {
		const char *a = mtab[i].at;
		if (!strcmp(p, "/")) {
			if (strcmp(a, "/"))
				return 1;
		}
		else if (!strncmp(a, p, L) && a[L] == '/')
			return 1;
	}
	return 0;
}

/* ------------ mount table ------------ */
static void
do_mount(int argc, char **argv)
{
	const char *hostfile = NULL, *mpt = NULL, *dev = NULL, *ospec = NULL;
	uint32_t bsize = 0, plen = 0;
	int forced = -1, ro = 0, i;
	char part = 0, at[2048];
	long long base = 0;
	RW h;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r"))
			ro = 1;
		else if (!strcmp(argv[i], "-B") && i + 1 < argc)
			bsize = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-A") && i + 1 < argc)
			forced = (int)s5_endian_parse(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-P") && i + 1 < argc)
			part = argv[++i][0];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			ospec = argv[++i];
		else if (!hostfile)
			hostfile = argv[i];
		else if (!mpt)
			mpt = argv[i];
	}
	if (!hostfile || !mpt) {
		printf("usage: mount [-r] [-B n] [-A bo] [-d dev -P part] [-o blk] hostfile mountpoint\n");
		return;
	}
	if (forced == S5_NENDIAN) {
		printf("mount: bad -A byte order\n");
		return;
	}
	snprintf(at, sizeof at, "%s", rel(mpt));
	for (i = 0; i < nmnt; i++)
		if (!strcmp(mtab[i].at, at)) {
			printf("mount: %s already in use\n", at);
			return;
		}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return;
	if (rw_open(&h, hostfile, bsize, forced, !ro, base) < 0) {
		if (ro || rw_open(&h, hostfile, bsize, forced, 0, base) < 0) {
			printf("mount: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", hostfile);
			return;
		}
		ro = 1;
	}
	if (nmnt == capmnt) {
		capmnt = capmnt ? capmnt * 2 : 8;
		mtab = realloc(mtab, (size_t)capmnt * sizeof *mtab);
		if (!mtab) {
			perror("mount");
			exit(1);
		}
	}
	mtab[nmnt].at = strdup(at);
	mtab[nmnt].img = strdup(hostfile);
	if (!mtab[nmnt].at || !mtab[nmnt].img) {
		perror("s5fs shell");
		exit(1);
	}
	mtab[nmnt].h = h;
	mtab[nmnt].ro = !h.writable;
	nmnt++;
	printf("mounted %s at %s (%s, %u-byte blocks, %s)\n", hostfile, at,
	       h.r.bo->name, h.r.bsize, h.writable ? "read-write" : "read-only");
}

static void
do_umount(const char *arg)
{
	char at[2048];
	int i, j;
	snprintf(at, sizeof at, "%s", rel(arg));
	for (i = 0; i < nmnt; i++)
		if (!strcmp(mtab[i].at, at)) {
			rw_close(&mtab[i].h);
			free(mtab[i].at);
			free(mtab[i].img);
			for (j = i; j < nmnt - 1; j++)
				mtab[j] = mtab[j + 1];
			nmnt--;
			if (!strncmp(cwd, at, strlen(at)))
				snprintf(cwd, sizeof cwd, "/");
			printf("unmounted %s\n", at);
			return;
		}
	printf("umount: %s: not mounted\n", at);
}

static void
do_mounts(void)
{
	int i;
	if (!nmnt) {
		printf("(nothing mounted)\n");
		return;
	}
	for (i = 0; i < nmnt; i++)
		printf("  %-16s %s  (%s)\n", mtab[i].at, mtab[i].img, mtab[i].ro ? "ro" : "rw");
}

/* resolve `arg` to (mount, subpath); prints a message and returns NULL if the
 * path isn't under any mount */
static struct mount *
rpath(const char *arg, char *sub, size_t subsz)
{
	char ap[2048];
	struct mount *m;
	snprintf(ap, sizeof ap, "%s", rel(arg));
	m = route(ap, sub, subsz);
	if (!m)
		printf("%s: no filesystem mounted there\n", ap);
	return m;
}

/* ------------ listing ------------ */
static void
ls_long(const char *name, const fsr_inode *in)
{
	char m[11], ts[20];
	time_t tt = in->mtime;
	struct tm *tm = localtime(&tt);
	fs_modestr(in->mode, m);
	if (tm)
		strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", tm);
	else
		snprintf(ts, sizeof ts, "?");
	printf("%s %2d %4d %4d %8d %s %s\n", m, (uint16_t)in->nlink,
	       (uint16_t)in->uid, (uint16_t)in->gid, in->size, ts, name);
}

static void
list_dir(RW *h, const fsr_inode *dir, int longf, int all)
{
	uint8_t buf[P11_MAXBSIZE];
	uint32_t nblk = ((uint32_t)dir->size + h->r.bsize - 1) / h->r.bsize, b, e;
	for (b = 0; b < nblk; b++) {
		long got = fsr_readfile(&h->r, dir, buf, h->r.bsize, (long)b * h->r.bsize);
		if (got <= 0)
			continue;
		for (e = 0; e * P11_DIRENTSZ < (uint32_t)got; e++) {
			uint8_t *d = buf + e * P11_DIRENTSZ;
			uint16_t di = h->r.bo->get16(d);
			char nm[P11_DIRSIZ + 1];
			fsr_inode ein;
			if (di == 0)
				continue;
			memcpy(nm, d + 2, P11_DIRSIZ);
			nm[P11_DIRSIZ] = '\0';
			if (!all && nm[0] == '.')
				continue;
			if (!longf) {
				printf("%s\n", nm);
				continue;
			}
			if (rw_iget(h, di, &ein) < 0)
				continue;
			ls_long(nm, &ein);
		}
	}
}

static void
do_ls(int argc, char **argv)
{
	int longf = 0, all = 0, i, listed = 0;
	char ap[2048], sub[2048];
	const char *arg = NULL;
	struct mount *m;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strchr(argv[i], 'l'))
				longf = 1;
			if (strchr(argv[i], 'a'))
				all = 1;
		}
		else
			arg = argv[i];
	}
	snprintf(ap, sizeof ap, "%s", rel(arg ? arg : cwd));
	m = route(ap, sub, sizeof sub);
	if (m) {
		fsr_inode in;
		uint32_t ino = rw_namei(&m->h, sub);
		if (ino && rw_iget(&m->h, ino, &in) == 0) {
			if ((in.mode & P11_IFMT) == P11_IFDIR) {
				list_dir(&m->h, &in, longf, all);
			}
			else {
				const char *bn = strrchr(ap, '/');
				if (longf)
					ls_long(bn ? bn + 1 : ap, &in);
				else
					printf("%s\n", bn ? bn + 1 : ap);
			}
			listed = 1;
		}
	}
	/* also show mountpoints that live directly under `ap` */
	for (i = 0; i < nmnt; i++) {
		char par[2048];
		parentof(mtab[i].at, par, sizeof par);
		if (!strcmp(par, ap)) {
			const char *bn = strrchr(mtab[i].at, '/');
			printf("%-14s  [mount: %s]\n", bn ? bn + 1 : mtab[i].at, mtab[i].img);
			listed = 1;
		}
	}
	if (!listed && !is_virtual_dir(ap))
		printf("%s: not found\n", ap);
}

static void
do_cd(const char *arg)
{
	char ap[2048], sub[2048];
	struct mount *m;
	snprintf(ap, sizeof ap, "%s", rel(arg));
	m = route(ap, sub, sizeof sub);
	if (m) {
		fsr_inode in;
		uint32_t ino = rw_namei(&m->h, sub);
		if (ino && rw_iget(&m->h, ino, &in) == 0 && (in.mode & P11_IFMT) == P11_IFDIR) {
			snprintf(cwd, sizeof cwd, "%s", ap);
			return;
		}
	}
	if (!strcmp(ap, "/") || is_virtual_dir(ap)) {
		snprintf(cwd, sizeof cwd, "%s", ap);
		return;
	}
	printf("cd: %s: not a directory\n", ap);
}

static void
do_cat(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++) {
		char sub[2048];
		struct mount *m = rpath(argv[i], sub, sizeof sub);
		fsr_inode in;
		long off = 0;
		uint32_t ino;
		if (!m)
			continue;
		ino = rw_namei(&m->h, sub);
		if (!ino || rw_iget(&m->h, ino, &in) < 0) {
			printf("cat: %s: not found\n", argv[i]);
			continue;
		}
		if ((in.mode & P11_IFMT) == P11_IFDIR) {
			printf("cat: %s: is a directory\n", argv[i]);
			continue;
		}
		while (off < in.size) {
			uint8_t b[8192];
			long w = in.size - off;
			if (w > (long)sizeof b)
				w = sizeof b;
			if (fsr_readfile(&m->h.r, &in, b, w, off) != w)
				break;
			fwrite(b, 1, w, stdout);
			off += w;
		}
	}
}

static void
do_stat(const char *arg)
{
	char sub[2048];
	struct mount *m = rpath(arg, sub, sizeof sub);
	fsr_inode in;
	uint32_t ino;
	char md[11];
	time_t t;
	if (!m)
		return;
	ino = rw_namei(&m->h, sub);
	if (!ino || rw_iget(&m->h, ino, &in) < 0) {
		printf("stat: %s: not found\n", arg);
		return;
	}
	fs_modestr(in.mode, md);
	t = in.mtime;
	printf("  %s\n  mount %s (%s)  inode %u\n  mode %s (0%o)  links %d  uid %d gid %d\n  size %d  mtime %s",
	       rel(arg), m->at, m->img, ino, md, in.mode & 07777, (uint16_t)in.nlink,
	       (uint16_t)in.uid, (uint16_t)in.gid, in.size, ctime(&t));
}

/* ------------ host <-> image transfer ------------ */
struct loc {
	int host;
	const char *hp;
	RW *rw;
	char ip[2048];
};

static int
loc_of(const char *arg, struct loc *l)
{
	char ap[2048], sub[2048];
	struct mount *m;
	if (arg[0] == '@') {
		l->host = 1;
		l->hp = arg + 1;
		return 0;
	}
	snprintf(ap, sizeof ap, "%s", rel(arg));
	m = route(ap, sub, sizeof sub);
	if (!m) {
		printf("%s: no filesystem mounted there\n", ap);
		return -1;
	}
	l->host = 0;
	l->rw = &m->h;
	snprintf(l->ip, sizeof l->ip, "%s", sub);
	return 0;
}

static void
do_cp(const char *as, const char *bs)
{
	struct loc a, b;
	int rc;
	if (loc_of(as, &a) < 0 || loc_of(bs, &b) < 0)
		return;
	if (!a.host && !b.host)
		rc = (a.rw == b.rw) ? rw_copy(a.rw, a.ip, b.ip)
				    : rw_copy_between(a.rw, a.ip, b.rw, b.ip);
	else if (a.host && !b.host)
		rc = fs_copy(b.rw, a.hp, 1, b.ip, 0); /* host -> image */
	else if (!a.host && b.host)
		rc = fs_copy(a.rw, a.ip, 0, b.hp, 1); /* image -> host */
	else
		rc = fs_copy(NULL, a.hp, 1, b.hp, 1); /* host -> host  */
	if (rc < 0)
		printf("cp: %s\n", strerror(-rc));
}

static void
do_mv(const char *as, const char *bs)
{
	struct loc a, b;
	int rc;
	if (loc_of(as, &a) < 0 || loc_of(bs, &b) < 0)
		return;
	if (a.host || b.host) {
		printf("mv: image paths only (use cp for host <-> image)\n");
		return;
	}
	if (a.rw == b.rw) {
		rc = rw_rename(a.rw, a.ip, b.ip);
		if (rc < 0)
			printf("mv: %s\n", strerror(-rc));
		return;
	}
	{
		fsr_inode in;
		uint32_t ino = rw_namei(a.rw, a.ip); /* cross-mount move */
		if (!ino || rw_iget(a.rw, ino, &in) < 0) {
			printf("mv: %s: not found\n", as);
			return;
		}
		if ((in.mode & P11_IFMT) == P11_IFDIR) {
			printf("mv: cross-mount move of a directory is unsupported\n");
			return;
		}
	}
	rc = rw_copy_between(a.rw, a.ip, b.rw, b.ip);
	if (rc < 0) {
		printf("mv: %s\n", strerror(-rc));
		return;
	}
	rc = rw_unlink(a.rw, a.ip);
	if (rc < 0)
		printf("mv: removed nothing at source: %s\n", strerror(-rc));
}

static void
do_get(int argc, char **argv)
{
	char sub[2048];
	struct mount *m;
	fsr_inode in;
	uint32_t ino;
	long off = 0;
	int ofd;
	const char *hostp;
	char hbuf[2048];
	if (argc < 2) {
		printf("usage: get imgpath [hostpath]\n");
		return;
	}
	m = rpath(argv[1], sub, sizeof sub);
	if (!m)
		return;
	ino = rw_namei(&m->h, sub);
	if (!ino || rw_iget(&m->h, ino, &in) < 0) {
		printf("get: %s: not found\n", argv[1]);
		return;
	}
	if ((in.mode & P11_IFMT) == P11_IFDIR) {
		printf("get: %s: is a directory\n", argv[1]);
		return;
	}
	if (argc >= 3)
		hostp = argv[2];
	else {
		const char *bn = strrchr(sub, '/');
		snprintf(hbuf, sizeof hbuf, "%s", bn ? bn + 1 : sub);
		hostp = hbuf;
	}
	ofd = open(hostp, O_WRONLY | O_CREAT | O_TRUNC, in.mode & 0777);
	if (ofd < 0) {
		printf("get: %s: %s\n", hostp, strerror(errno));
		return;
	}
	while (off < in.size) {
		uint8_t b[65536];
		long w = in.size - off;
		if (w > (long)sizeof b)
			w = sizeof b;
		if (fsr_readfile(&m->h.r, &in, b, w, off) != w)
			break;
		if (write(ofd, b, w) != w)
			break;
		off += w;
	}
	close(ofd);
	printf("%s -> %s (%d bytes)\n", argv[1], hostp, in.size);
}

static void
do_put(int argc, char **argv)
{
	char sub[2048], ibuf[2048];
	struct mount *m;
	int ifd, rc;
	struct stat st;
	const char *imgarg;
	if (argc < 2) {
		printf("usage: put hostpath [imgpath]\n");
		return;
	}
	ifd = open(argv[1], O_RDONLY);
	if (ifd < 0 || fstat(ifd, &st) < 0) {
		printf("put: %s: %s\n", argv[1], strerror(errno));
		if (ifd >= 0)
			close(ifd);
		return;
	}
	if (argc >= 3)
		imgarg = argv[2];
	else {
		const char *bn = strrchr(argv[1], '/');
		snprintf(ibuf, sizeof ibuf, "%s", bn ? bn + 1 : argv[1]);
		imgarg = ibuf;
	}
	m = rpath(imgarg, sub, sizeof sub);
	if (!m) {
		close(ifd);
		return;
	}
	rc = rw_put_fd(&m->h, sub, ifd, st.st_mode & 0777, (int32_t)st.st_mtime);
	close(ifd);
	if (rc < 0)
		printf("put: %s: %s\n", imgarg, strerror(-rc));
	else
		printf("%s -> %s\n", argv[1], imgarg);
}

static void
help(void)
{
	printf(
		"  mount hostfile mnt [-r] [-B n] [-A bo] [-d dev -P part] [-o blk]\n"
		"  umount mnt            mounts            (list mounts)\n"
		"  ls [-l] [-a] [path]   cd [path]         pwd\n"
		"  cat file...           stat path\n"
		"  cp [@]src [@]dst      mv src dst        ('@' prefix = a host path)\n"
		"  get imgpath [host]    put host [imgpath]\n"
		"  rm file...   mkdir dir...   rmdir dir...\n"
		"  chmod mode path...   chown uid[:gid] path...   chgrp gid path...\n"
		"  help    quit | exit\n");
}

/* apply a one-arg mutating op across argv[1..], routing each path */
static void
each_path(int argc, char **argv, int (*op)(RW *, const char *, unsigned), unsigned arg)
{
	int i;
	for (i = 1; i < argc; i++) {
		char sub[2048];
		struct mount *m = rpath(argv[i], sub, sizeof sub);
		int rc;
		if (!m)
			continue;
		rc = op(&m->h, sub, arg);
		if (rc < 0)
			printf("%s: %s: %s\n", argv[0], argv[i], strerror(-rc));
	}
}

static int
op_unlink(RW *h, const char *p, unsigned a)
{
	(void)a;
	return rw_unlink(h, p);
}

static int
op_rmdir(RW *h, const char *p, unsigned a)
{
	(void)a;
	return rw_rmdir(h, p);
}

static int
op_mkdir(RW *h, const char *p, unsigned a)
{
	(void)a;
	return rw_mkdir(h, p, 0755);
}

int
cmd_shell(int argc, char **argv)
{
	uint32_t bsize = 0, plen = 0;
	int forced = -1, ro = 0, c, interactive;
	const char *dev = NULL, *ospec = NULL;
	char part = 0;
	long long base = 0;
	char line[4096];

	optind = 1;
	while ((c = getopt(argc, argv, "rB:A:d:P:o:")) != -1) {
		switch (c) {
		case 'r':
			ro = 1;
			break;
		case 'B':
			bsize = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'A':
			forced = (int)s5_endian_parse(optarg);
			if (forced == S5_NENDIAN) {
				fprintf(stderr, "shell: bad -A\n");
				return 2;
			}
			break;
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
			fprintf(stderr, "usage: s5fs shell [-r] [-B ..] [-A ..] [-d dev -P part | -o blk] [image]\n");
			return 2;
		}
	}
	if (optind < argc) { /* auto-mount the launch image at "/" */
		RW h;
		if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
			return 2;
		if (rw_open(&h, argv[optind], bsize, forced, !ro, base) < 0) {
			if (ro || rw_open(&h, argv[optind], bsize, forced, 0, base) < 0) {
				fprintf(stderr, "shell: %s: not a readable s5fs image (try -B/-A)\n", argv[optind]);
				return 1;
			}
		}
		capmnt = 8;
		mtab = malloc((size_t)capmnt * sizeof *mtab);
		if (!mtab) {
			perror("shell");
			return 1;
		}
		mtab[0].at = strdup("/");
		mtab[0].img = strdup(argv[optind]);
		if (!mtab[0].at || !mtab[0].img) {
			perror("s5fs shell");
			exit(1);
		}
		mtab[0].h = h;
		mtab[0].ro = !h.writable;
		nmnt = 1;
	}
	interactive = isatty(STDIN_FILENO);
	printf("s5fs shell -- 'mount hostfile /mnt' to add images, 'help' for commands.\n");
	if (nmnt)
		printf("mounted %s at / (%s)\n", mtab[0].img, mtab[0].ro ? "read-only" : "read-write");

	for (;;) {
		char *tok[64];
		int n = 0;
		if (interactive) {
			printf("%s> ", cwd);
			fflush(stdout);
		}
		if (!fgets(line, sizeof line, stdin)) {
			printf("\n");
			break;
		}
		{
			char *save, *t;
			for (t = strtok_r(line, " \t\r\n", &save); t && n < 63; t = strtok_r(NULL, " \t\r\n", &save))
				tok[n++] = t;
		}
		if (n == 0)
			continue;
		tok[n] = NULL;

		if (!strcmp(tok[0], "quit") || !strcmp(tok[0], "exit"))
			break;
		else if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?"))
			help();
		else if (!strcmp(tok[0], "pwd"))
			printf("%s\n", cwd);
		else if (!strcmp(tok[0], "mount"))
			do_mount(n, tok);
		else if (!strcmp(tok[0], "umount") || !strcmp(tok[0], "unmount")) {
			if (n >= 2)
				do_umount(tok[1]);
			else
				printf("usage: umount mnt\n");
		}
		else if (!strcmp(tok[0], "mounts"))
			do_mounts();
		else if (!strcmp(tok[0], "ls"))
			do_ls(n, tok);
		else if (!strcmp(tok[0], "cd"))
			do_cd(n >= 2 ? tok[1] : "/");
		else if (!strcmp(tok[0], "cat"))
			do_cat(n, tok);
		else if (!strcmp(tok[0], "stat")) {
			if (n >= 2)
				do_stat(tok[1]);
			else
				printf("usage: stat path\n");
		}
		else if (!strcmp(tok[0], "get"))
			do_get(n, tok);
		else if (!strcmp(tok[0], "put"))
			do_put(n, tok);
		else if (!strcmp(tok[0], "cp")) {
			if (n >= 3)
				do_cp(tok[1], tok[2]);
			else
				printf("usage: cp [@]src [@]dst\n");
		}
		else if (!strcmp(tok[0], "mv")) {
			if (n >= 3)
				do_mv(tok[1], tok[2]);
			else
				printf("usage: mv src dst\n");
		}
		else if (!strcmp(tok[0], "rm"))
			each_path(n, tok, op_unlink, 0);
		else if (!strcmp(tok[0], "rmdir"))
			each_path(n, tok, op_rmdir, 0);
		else if (!strcmp(tok[0], "mkdir"))
			each_path(n, tok, op_mkdir, 0);
		else if (!strcmp(tok[0], "chmod")) {
			if (n < 3)
				printf("usage: chmod mode path...\n");
			else {
				unsigned pm = (unsigned)strtoul(tok[1], NULL, 8);
				int i;
				for (i = 2; i < n; i++) {
					char sub[2048];
					struct mount *m = rpath(tok[i], sub, sizeof sub);
					int rc;
					if (!m)
						continue;
					rc = rw_chmod(&m->h, sub, pm);
					if (rc < 0)
						printf("chmod: %s: %s\n", tok[i], strerror(-rc));
				}
			}
		}
		else if (!strcmp(tok[0], "chown") || !strcmp(tok[0], "chgrp")) {
			int grp = !strcmp(tok[0], "chgrp"), uid = -1, gid = -1, i;
			if (n < 3)
				printf("usage: %s %s path...\n", tok[0], grp ? "gid" : "uid[:gid]");
			else {
				if (grp)
					gid = (int)strtol(tok[1], NULL, 10);
				else
					fs_parse_owner(tok[1], &uid, &gid);
				for (i = 2; i < n; i++) {
					char sub[2048];
					struct mount *m = rpath(tok[i], sub, sizeof sub);
					int rc;
					if (!m)
						continue;
					rc = rw_chown(&m->h, sub, uid, gid);
					if (rc < 0)
						printf("%s: %s: %s\n", tok[0], tok[i], strerror(-rc));
				}
			}
		}
		else
			printf("%s: unknown command (try 'help')\n", tok[0]);
	}
	{
		int i;
		for (i = 0; i < nmnt; i++)
			rw_close(&mtab[i].h);
	}
	return 0;
}

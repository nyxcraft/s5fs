/*
 * cmd_manifest.c -- `s5fs manifest` and `s5fs verify`.
 *
 *   s5fs manifest [opts] image            emit one line per file to stdout
 *   s5fs verify   [opts] image manifest   diff an image against a manifest
 *
 * The manifest is an mtree-style fingerprint of a filesystem: for every path,
 * its type, permission bits, uid, gid, size, mtime, and a CRC32 of the file
 * contents (for a device node, its major.minor instead).  `verify` walks a
 * second image and reports what changed -- so you can prove a rebuilt 2.9 world
 * matches a reference, or regression-check any image transformation.
 *
 * Line format (mtime is informational; `verify` ignores it and does not
 * checksum directories, whose on-disk size/order legitimately varies):
 *
 *   <type> <mode> <uid> <gid> <size> <mtime> <cksum> <path>
 *   f 755 0 1 74958 482601600 8b1e4f27 /unix
 *   d 755 0 1 512 447840045 - /bin
 *   c 640 0 0 0 0 3.1 /dev/tty
 */

#define _POSIX_C_SOURCE 200809L

#include "fsread.h"
#include "device.h"
#include "cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- CRC32 (zlib/Ethernet polynomial) ---- */
static uint32_t
crc32_upd(uint32_t crc, const uint8_t *p, long n)
{
	static uint32_t tab[256];
	static int init = 0;
	if (!init) {
		uint32_t i, j, c;
		for (i = 0; i < 256; i++) {
			c = i;
			for (j = 0; j < 8; j++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
		init = 1;
	}
	crc = ~crc;
	while (n--)
		crc = tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t
file_crc(FSR *r, const fsr_inode *in)
{
	uint32_t crc = 0;
	long off = 0;
	uint8_t buf[8192];
	while (off < in->size) {
		long want = in->size - off;
		if (want > (long)sizeof buf)
			want = sizeof buf;
		if (fsr_readfile(r, in, buf, want, off) != want)
			break;
		crc = crc32_upd(crc, buf, want);
		off += want;
	}
	return crc;
}

/* one filesystem entry, handed to a per-entry callback */
struct ent {
	char type;     /* f d c b */
	unsigned mode; /* permission bits */
	int uid, gid;
	int32_t size, mtime;
	char cks[24]; /* CRC32 hex (file) / "-" (dir) / "maj.min" (dev) */
	const char *path;
};

typedef void (*ent_cb)(void *arg, const struct ent *e);

static void walk(FSR *r, uint32_t ino, const char *path, ent_cb cb, void *arg);

/* One-shot walk guard.  A directory cycle is representable on disk (a corrupt
 * image, or fsck -p reconnecting an orphaned loop), and without this the
 * recursion below runs until the stack is exhausted. */
static fsr_walkset g_ws;

struct wctx {
	FSR *r;
	const char *prefix;
	ent_cb cb;
	void *arg;
};

static int
walk_cb(void *a, uint32_t ino, const char *name)
{
	struct wctx *w = a;
	char child[1200];
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	if (w->prefix[1] == '\0') /* prefix == "/" */
		snprintf(child, sizeof child, "/%s", name);
	else
		snprintf(child, sizeof child, "%s/%s", w->prefix, name);
	if (fsr_walk_enter(&g_ws, ino))
		walk(w->r, ino, child, w->cb, w->arg);
	return 0;
}

static void
walk(FSR *r, uint32_t ino, const char *path, ent_cb cb, void *arg)
{
	fsr_inode in;
	struct ent e;
	if (fsr_iget(r, ino, &in) < 0)
		return;
	e.mode = in.mode & 07777;
	e.uid = (uint16_t)in.uid;
	e.gid = (uint16_t)in.gid;
	e.size = in.size;
	e.mtime = in.mtime;
	e.path = path;
	switch (in.mode & P11_IFMT) {
	case P11_IFDIR:
		e.type = 'd';
		strcpy(e.cks, "-");
		cb(arg, &e);
		{
			struct wctx w;
			w.r = r;
			w.prefix = path;
			w.cb = cb;
			w.arg = arg;
			fsr_readdir(r, &in, walk_cb, &w);
		}
		return;
	case P11_IFREG:
		e.type = 'f';
		snprintf(e.cks, sizeof e.cks, "%08x", file_crc(r, &in));
		break;
	case P11_IFCHR:
	case P11_IFBLK:
		e.type = (in.mode & P11_IFMT) == P11_IFBLK ? 'b' : 'c';
		snprintf(e.cks, sizeof e.cks, "%u.%u", (in.addr[0] >> 8) & 0377, in.addr[0] & 0377);
		break;
	default:
		return; /* nothing else exists in s5fs */
	}
	cb(arg, &e);
}

/* ---- shared option parsing: -B/-A + -d/-P/-o -> open FSR ---- */
static int
open_image(int argc, char **argv, FSR *r, const char *usage)
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
				fprintf(stderr, "%s: bad -A\n", argv[0]);
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
			fprintf(stderr, "%s\n", usage);
			return -1;
		}
	}
	if (device_resolve_part(dev, part, ospec, &base, &plen) < 0)
		return -1;
	if (optind >= argc) {
		fprintf(stderr, "%s\n", usage);
		return -1;
	}
	if (fsr_open(r, argv[optind], bsize, forced, base) < 0) {
		fprintf(stderr, "%s: %s: not a readable s5fs image (try -B/-A, or -d/-P)\n", argv[0], argv[optind]);
		return -1;
	}
	return optind;
}

/* ---- manifest ---- */
static void
print_cb(void *arg, const struct ent *e)
{
	FILE *out = arg;
	fprintf(out, "%c %o %d %d %d %d %s %s\n",
		e->type, e->mode, e->uid, e->gid, e->size, e->mtime, e->cks, e->path);
}

int
cmd_manifest(int argc, char **argv)
{
	FSR r;
	int idx = open_image(argc, argv, &r,
			     "usage: s5fs manifest [-B ..] [-A ..] [-d dev -P part | -o blk] image");
	if (idx < 0)
		return 2;
	printf("# s5fs manifest: %s\n", argv[idx]);
	if (fsr_walkset_init(&g_ws, &r) < 0) {
		fprintf(stderr, "s5fs: out of memory\n");
		fsr_close(&r);
		return 1;
	}
	walk(&r, P11_ROOTINO, "/", print_cb, stdout);
	fsr_walkset_free(&g_ws);
	fsr_close(&r);
	return 0;
}

/* ---- verify ---- */
struct ment {
	struct ent e;
	char *path;
	int seen;
};

struct vctx {
	struct ment *m;
	int n;
	int extra, differ;
};

static struct ment *
find_m(struct vctx *v, const char *path)
{
	int i;
	for (i = 0; i < v->n; i++)
		if (!strcmp(v->m[i].path, path))
			return &v->m[i];
	return NULL;
}

static void
verify_cb(void *arg, const struct ent *e)
{
	struct vctx *v = arg;
	struct ment *m = find_m(v, e->path);
	int bad = 0;
	if (!m) {
		printf("+ %s   (in image, not in manifest)\n", e->path);
		v->extra++;
		return;
	}
	m->seen = 1;
	if (m->e.type != e->type) {
		printf("! %s: type %c != %c\n", e->path, e->type, m->e.type);
		bad = 1;
	}
	if (m->e.mode != e->mode) {
		printf("! %s: mode %o != %o\n", e->path, e->mode, m->e.mode);
		bad = 1;
	}
	if (m->e.uid != e->uid || m->e.gid != e->gid) {
		printf("! %s: owner %d:%d != %d:%d\n", e->path, e->uid, e->gid, m->e.uid, m->e.gid);
		bad = 1;
	}
	if (e->type == 'f') { /* size + content for files */
		if (m->e.size != e->size) {
			printf("! %s: size %d != %d\n", e->path, e->size, m->e.size);
			bad = 1;
		}
		if (strcmp(m->e.cks, e->cks)) {
			printf("! %s: content %s != %s\n", e->path, e->cks, m->e.cks);
			bad = 1;
		}
	}
	else if (e->type == 'c' || e->type == 'b') { /* device numbers */
		if (strcmp(m->e.cks, e->cks)) {
			printf("! %s: device %s != %s\n", e->path, e->cks, m->e.cks);
			bad = 1;
		}
	}
	if (bad)
		v->differ++;
}

int
cmd_verify(int argc, char **argv)
{
	FSR r;
	FILE *mf;
	char line[1600];
	struct vctx v;
	int idx, i, missing = 0, cap = 64;

	idx = open_image(argc, argv, &r,
			 "usage: s5fs verify [-B ..] [-A ..] [-d dev -P part | -o blk] image manifest");
	if (idx < 0)
		return 2;
	if (idx + 1 >= argc) {
		fprintf(stderr, "s5fs verify: need a manifest file\n");
		fsr_close(&r);
		return 2;
	}
	mf = fopen(argv[idx + 1], "r");
	if (!mf) {
		fprintf(stderr, "s5fs verify: %s: cannot open\n", argv[idx + 1]);
		fsr_close(&r);
		return 1;
	}

	v.m = malloc((size_t)cap * sizeof *v.m);
	v.n = 0;
	v.extra = 0;
	v.differ = 0;
	if (!v.m) {
		fprintf(stderr, "s5fs verify: out of memory\n");
		return 1;
	}
	while (fgets(line, sizeof line, mf)) {
		char ty[8], cks[24], path[1200];
		unsigned long mode;
		long uid, gid, size, mtime;
		struct ment *e;
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%1s %lo %ld %ld %ld %ld %23s %1199[^\n]",
			   ty, &mode, &uid, &gid, &size, &mtime, cks, path) != 8)
			continue;
		if (v.n == cap) {
			cap *= 2;
			v.m = realloc(v.m, (size_t)cap * sizeof *v.m);
			if (!v.m) {
				fprintf(stderr, "s5fs verify: out of memory\n");
				return 1;
			}
		}
		e = &v.m[v.n++];
		e->e.type = ty[0];
		e->e.mode = (unsigned)mode;
		e->e.uid = (int)uid;
		e->e.gid = (int)gid;
		e->e.size = (int32_t)size;
		e->e.mtime = (int32_t)mtime;
		snprintf(e->e.cks, sizeof e->e.cks, "%s", cks);
		e->path = strdup(path);
		e->seen = 0;
		if (!e->path) {
			fprintf(stderr, "s5fs verify: out of memory\n");
			return 1;
		}
	}
	fclose(mf);

	if (fsr_walkset_init(&g_ws, &r) < 0) {
		fprintf(stderr, "s5fs: out of memory\n");
		fsr_close(&r);
		return 1;
	}
	walk(&r, P11_ROOTINO, "/", verify_cb, &v);
	fsr_walkset_free(&g_ws);
	fsr_close(&r);

	for (i = 0; i < v.n; i++)
		if (!v.m[i].seen) {
			printf("- %s   (in manifest, not in image)\n", v.m[i].path);
			missing++;
		}

	printf("verify: %d entries, %d changed, %d extra, %d missing\n",
	       v.n, v.differ, v.extra, missing);
	return (v.differ || v.extra || missing) ? 1 : 0;
}

/*
 * tree.c -- in-memory filesystem tree + serializer to an s5fs image.
 * See tree.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * building the tree
 * ------------------------------------------------------------------ */

static tnode *
node_new(const char *name, int kind)
{
	tnode *n = calloc(1, sizeof *n);
	if (!n) {
		perror("s5fs: calloc");
		exit(1);
	}
	n->name = name ? strdup(name) : NULL;
	n->kind = kind;
	n->perm = 0755;
	return n;
}

static void
dir_add(tnode *dir, tnode *child)
{
	if (dir->nkids == dir->kidcap) {
		dir->kidcap = dir->kidcap ? dir->kidcap * 2 : 16;
		dir->kids = realloc(dir->kids, dir->kidcap * sizeof *dir->kids);
		if (!dir->kids) {
			perror("s5fs: realloc");
			exit(1);
		}
	}
	child->parent = dir;
	dir->kids[dir->nkids++] = child;
}

static tnode *
dir_child(tnode *dir, const char *name)
{
	size_t i;
	if (dir->kind != TN_DIR)
		return NULL;
	for (i = 0; i < dir->nkids; i++)
		if (dir->kids[i]->name && !strcmp(dir->kids[i]->name, name))
			return dir->kids[i];
	return NULL;
}

tnode *
tree_root(void)
{
	tnode *r = node_new(NULL, TN_DIR);
	r->parent = r; /* root's ".." is itself */
	return r;
}

/* walk `path` component by component; `create` makes missing dirs. */
static tnode *
walk(tnode *root, const char *path, int create)
{
	char buf[2048];
	char *tok, *save;
	tnode *cur = root;

	strncpy(buf, path, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	for (tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		tnode *child;
		if (!strcmp(tok, ".") || !strcmp(tok, ""))
			continue;
		if (!strcmp(tok, "..")) { /* shouldn't appear; treat as parent */
			cur = cur->parent;
			continue;
		}
		/* A directory entry holds P11_DIRSIZ name bytes with no
		 * terminator.  Refuse rather than truncate: two siblings sharing
		 * their first 14 characters would otherwise become two entries
		 * with the SAME on-disk name, both unreachable under the name the
		 * archive used, on an image fsck still calls clean. */
		if (strlen(tok) > P11_DIRSIZ)
			return NULL;
		child = dir_child(cur, tok);
		if (!child) {
			if (!create)
				return NULL;
			child = node_new(tok, TN_DIR);
			dir_add(cur, child);
		}
		cur = child;
	}
	return cur;
}

tnode *
tree_insert(tnode *root, const char *path)
{
	return walk(root, path, 1);
}

tnode *
tree_find(tnode *root, const char *path)
{
	return walk(root, path, 0);
}

uint32_t
tree_count_inodes(const tnode *root)
{
	uint32_t n = 0;
	size_t i;
	if (root->kind == TN_LINK)
		return 0; /* hard links reuse an inode */
	n = 1;
	if (root->kind == TN_DIR)
		for (i = 0; i < root->nkids; i++)
			n += tree_count_inodes(root->kids[i]);
	return n;
}

/* ------------------------------------------------------------------ *
 * serialize
 * ------------------------------------------------------------------ */

/* pass 1: hand out inode numbers, pre-order (root first). */
static void
assign_inodes(S5FS *fs, tnode *n)
{
	size_t i;
	if (n->kind == TN_LINK)
		return; /* uses linkto's inode */
	n->ino = s5fs_ialloc(fs);
	if (n->kind == TN_DIR)
		for (i = 0; i < n->nkids; i++)
			assign_inodes(fs, n->kids[i]);
}

/* pass 2: link count = number of directory entries that will point at a node */
static void
count_links(tnode *n)
{
	size_t i;
	if (n->kind != TN_DIR)
		return;
	n->nlink++;	    /* this dir's "." -> itself   */
	n->parent->nlink++; /* this dir's ".." -> parent  */
	for (i = 0; i < n->nkids; i++) {
		tnode *c = n->kids[i];
		tnode *t = (c->kind == TN_LINK) ? c->linkto : c;
		if (t)
			t->nlink++; /* the named entry            */
	}
	for (i = 0; i < n->nkids; i++)
		count_links(n->kids[i]);
}

/* stream a regular file's content into inode `in` */
static void
write_reg(S5FS *fs, tnode *n, s5fs_inode *in)
{
	uint32_t nblk = (n->size + fs->bsize - 1) / fs->bsize, b;
	int32_t *da = nblk ? malloc(nblk * sizeof *da) : NULL;
	uint8_t *buf = malloc(fs->bsize);

	if ((nblk && !da) || !buf) {
		free(da);
		free(buf);
		return;
	}
	if (nblk)
		lseek(n->src_fd, n->src_off, SEEK_SET);
	for (b = 0; b < nblk; b++) {
		uint32_t want = n->size - b * fs->bsize, got = 0;
		if (want > fs->bsize)
			want = fs->bsize;
		memset(buf, 0, fs->bsize);
		while (got < want) {
			ssize_t r = read(n->src_fd, buf + got, want - got);
			if (r <= 0)
				break;
			got += (uint32_t)r;
		}
		da[b] = s5fs_alloc(fs);
		s5fs_wtblk(fs, (uint32_t)da[b], buf);
	}
	s5fs_setblocks(fs, in, da, nblk);
	in->size = (int32_t)n->size;
	free(buf);
	free(da);
}

/* build and store a directory's data block(s): ".", "..", then children */
static void
write_dir(S5FS *fs, tnode *n, s5fs_inode *in)
{
	uint32_t nent = (uint32_t)n->nkids + 2;
	uint32_t len = nent * P11_DIRENTSZ;
	uint8_t *db = calloc(1, ((len + fs->bsize - 1) / fs->bsize) * fs->bsize);
	uint32_t nblk, b;
	int32_t *da;
	size_t i;

	if (!db)
		return;
	fs->bo->put16(db + 0 * P11_DIRENTSZ, (uint16_t)n->ino);
	db[2] = '.';
	fs->bo->put16(db + 1 * P11_DIRENTSZ, (uint16_t)n->parent->ino);
	db[P11_DIRENTSZ + 2] = '.';
	db[P11_DIRENTSZ + 3] = '.';
	for (i = 0; i < n->nkids; i++) {
		tnode *c = n->kids[i];
		tnode *t = (c->kind == TN_LINK) ? c->linkto : c;
		uint8_t *e = db + (i + 2) * P11_DIRENTSZ;
		size_t l;
		if (!t)
			continue; /* dangling hard link */
		l = strlen(c->name);
		if (l > P11_DIRSIZ) /* tree_insert refuses these; belt and braces */
			continue;
		fs->bo->put16(e, (uint16_t)t->ino);
		memcpy((char *)e + 2, c->name, l);
	}
	/* store the directory data as this inode's content */
	nblk = (len + fs->bsize - 1) / fs->bsize;
	da = nblk ? malloc(nblk * sizeof *da) : NULL;
	if (nblk && !da) { /* write_reg checks this; so must we */
		free(db);
		return;
	}
	for (b = 0; b < nblk; b++) {
		da[b] = s5fs_alloc(fs);
		s5fs_wtblk(fs, (uint32_t)da[b], db + b * fs->bsize);
	}
	s5fs_setblocks(fs, in, da, nblk);
	in->size = (int32_t)len;
	free(da);
	free(db);
}

/* pass 3: write each node's inode (and data), recursing into directories */
static void
write_node(S5FS *fs, tnode *n)
{
	s5fs_inode in;
	size_t i;

	if (n->kind == TN_LINK)
		return; /* only the named entry, no inode */

	memset(&in, 0, sizeof in);
	in.number = (uint16_t)n->ino;
	in.uid = n->uid;
	in.gid = n->gid;
	in.nlink = n->nlink;
	in.atime = n->atime;
	in.mtime = n->mtime;
	in.ctime = n->ctime;

	switch (n->kind) {
	case TN_DIR:
		in.mode = (uint16_t)(P11_IFDIR | (n->perm & 07777));
		write_dir(fs, n, &in);
		break;
	case TN_REG:
		in.mode = (uint16_t)(P11_IFREG | (n->perm & 07777));
		write_reg(fs, n, &in);
		break;
	case TN_DEV:
		in.mode = (uint16_t)((n->isblk ? P11_IFBLK : P11_IFCHR) | (n->perm & 07777));
		in.addr[0] = ((n->major & 0377) << 8) | (n->minor & 0377);
		break;
	}
	s5fs_writeinode(fs, &in);

	if (n->kind == TN_DIR)
		for (i = 0; i < n->nkids; i++)
			write_node(fs, n->kids[i]);
}

int
tree_serialize(S5FS *fs, tnode *root)
{
	assign_inodes(fs, root);
	count_links(root);
	write_node(fs, root);
	return fs->error ? -1 : 0;
}

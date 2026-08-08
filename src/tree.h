/*
 * tree.h -- an in-memory filesystem tree, then serialized to an s5fs image.
 *
 * The dump->dsk and tar->dsk front-ends read a flat, arbitrary-order stream of
 * entries (a directory can arrive before or after its children), so unlike
 * mktree's live host walk they can't build the image in one forward pass.
 * They instead insert every entry into this tree -- materializing missing
 * parent directories on demand -- and then call tree_serialize(), which:
 *
 *   1. assigns inode numbers (root = inode 2, pre-order),
 *   2. computes each inode's link count from the directory entries it will
 *      emit (so nothing is patched afterwards), and
 *   3. writes the directories, files (streamed from their source), device
 *      nodes, and hard links (extra name -> same inode) into the filesystem.
 *
 * File contents are referenced by (fd, offset, size) so large archives stream
 * through rather than being held in memory.
 */
#ifndef TREE_H
#define TREE_H

#include <stdint.h>
#include <stddef.h>
#include "s5fs_core.h"

enum {
	TN_DIR,
	TN_REG,
	TN_DEV,
	TN_LINK
};

typedef struct tnode {
	char *name; /* leaf path component            */
	int kind;
	uint16_t perm; /* permission bits (07777)        */
	int16_t uid, gid;
	int32_t atime, mtime, ctime; /* 0 => stamp now at serialize    */

	/* TN_REG: content streamed from this source at serialize time */
	int src_fd;
	long src_off;
	uint32_t size;

	/* TN_DEV */
	int isblk, major, minor;

	/* TN_LINK: another name for linkto's inode */
	struct tnode *linkto;

	/* TN_DIR */
	struct tnode **kids;
	size_t nkids, kidcap;

	/* filled by tree_serialize */
	uint32_t ino;
	int16_t nlink;
	struct tnode *parent;
} tnode;

/* Create the root directory node. */
tnode *tree_root(void);

/* Insert `path` (creating intermediate directories); returns the leaf node,
 * whose kind/perm/uid/gid/etc. the caller then fills in.  Returns the root for
 * an empty path.  Auto-created parents are TN_DIR, perm 0755, uid/gid 0. */
tnode *tree_insert(tnode *root, const char *path);

/* Look up an existing node by path (for hard-link targets); NULL if absent. */
tnode *tree_find(tnode *root, const char *path);

/* Write the whole tree into `fs` (root becomes inode 2).  Returns 0, or -1 on
 * a filesystem error (message in fs->err). */
int tree_serialize(S5FS *fs, tnode *root);

/* Count of regular files + directories (to size the i-list before mkfs). */
uint32_t tree_count_inodes(const tnode *root);

#endif /* TREE_H */

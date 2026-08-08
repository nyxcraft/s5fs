/*
 * fsutil.h -- small presentation helpers shared by the batch file commands
 * (cmd_fs.c) and the interactive explorer (cmd_shell.c).  Implemented in
 * cmd_fs.c.
 */
#ifndef FSUTIL_H
#define FSUTIL_H

#include <stddef.h>
#include <stdint.h>
#include "s5fs_rw.h"

/* strerror() for a negative-errno return from the s5fs_rw ops */
const char *fs_errstr(int negerrno);

/* "drwxr-xr-x" (11 bytes incl. NUL) from an s5fs mode word */
void fs_modestr(uint16_t mode, char out[11]);

/* normalize cwd + arg into an absolute image path (handles "." and "..") */
void fs_resolve(const char *cwd, const char *arg, char *out, size_t outsz);

/* parse a numeric "uid", "uid:gid", or ":gid" spec; an omitted field => -1
 * (leave unchanged).  Names aren't resolved -- the image is a foreign system. */
/* Parse "uid[:gid]".  Returns 0, or -1 if either field is not a plain number.
 * s5fs stores numeric ids only and there is no passwd file in the image to
 * resolve a name against, so a name has to be an error: silently parsing
 * "root" as 0 turns a typo into a chown to uid 0 that reports success. */
int fs_parse_owner(const char *spec, int *uid, int *gid);

/* Parse an octal permission string. Returns 0, or -1 if it is not octal --
 * `chmod rwx` would otherwise be read as 0 and strip every permission bit. */
int fs_parse_mode(const char *spec, unsigned *perm);

/* `cp` host marker: a leading '@' means the path is on the host filesystem.
 * If *p starts with '@', advance past it and return 1; else return 0.  ('@' is
 * safe: real host paths -- incl. Windows "C:\..." -- never start with it, and
 * image paths default to the image.) */
int fs_host_path(const char **p);

/* copy src -> dst where each side may be a host path (src_host/dst_host) or an
 * absolute image path.  Handles all four combinations, appending the source
 * basename when the destination is an existing directory.  0 or -errno. */
int fs_copy(RW *h, const char *src, int src_host, const char *dst, int dst_host);

#endif /* FSUTIL_H */

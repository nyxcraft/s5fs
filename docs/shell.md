# The interactive shell: a multi-mount VFS

`s5fs shell` is not a single-image browser. It is a small virtual filesystem —
any number of images mounted at arbitrary paths in one namespace, with every
path routed to the image that serves it. That is what makes `cp` between two
different disk images a single natural command rather than a two-step export and
import.

Code: `src/cmd_shell.c`. It is a front-end: every mutation goes through
[`s5fs_rw`](mutation-engine.md), so the shell cannot diverge from the batch
commands.

---

## 1. The mount table

```c
struct mount { char *at; char *img; RW h; int ro; };
```

Four fields: where it is mounted, what file it came from, its open read/write
handle, and whether it is read-only. A launch image is auto-mounted at `/`, and
starting with no image is fine — you mount things afterwards.

```
mount hostfile mnt [-r] [-B n] [-A bo] [-d dev -P part] [-o blk]
umount mnt
mounts
```

Each mount carries its own block size, byte order, and partition base, so
**images with different geometry coexist in one namespace**. A 512-byte
PDP-11 RL02 and a 2048-byte little-endian SysV volume can be mounted side by
side and copied between; each `RW` handle holds its own decoded parameters.

A write to a mount opened `-r` returns `Read-only file system` rather than
failing some other way.

---

## 2. Routing: longest prefix wins

Every path operation resolves through one function:

```c
static struct mount *route(const char *abspath, char *sub, size_t subsz);
```

It scans the table and keeps the mount with the **longest matching mountpoint
prefix**, writing the remainder — the path *within that image*, always starting
`/` — into `sub`.

The matching has three cases, and all three are needed:

```c
if (strcmp(m->at, "/") == 0)                                rem = abspath;
else if (strcmp(abspath, m->at) == 0)                       rem = "/";
else if (!strncmp(abspath, m->at, L) && abspath[L] == '/')  rem = abspath + L;
```

- Root is special because it is the only mountpoint that is a prefix of
  everything without contributing a separator.
- An exact match on the mountpoint itself resolves to that image's root.
- Otherwise the next character **must be `/`**. Without that test, a mount at
  `/usr` would capture `/usrlocal`, which is a different path entirely.

Longest-prefix is what makes overlapping mounts behave: mount one image at
`/usr` and another at `/usr/local`, and paths under `/usr/local` go to the
deeper one while the rest of `/usr` goes to the shallower. Deeper wins,
exactly as a real VFS shadows.

---

## 3. Virtual directories

You can `cd` into a path that no mount claims, as long as it is an **ancestor of
some mountpoint**:

```c
static int is_virtual_dir(const char *p);
```

Mount an image at `/a/b/c` and `/a` and `/a/b` become navigable even though no
image backs them. Without this, mounting at a nested path would create a
namespace you could not walk into — you would have to know the full path in
advance and could never `ls` your way there.

Virtual directories are navigable and listable, not writable. There is nowhere
to put a file.

---

## 4. Cross-image copy and the host escape

`cp` and `mv` route **each side independently**. That is the whole trick:

```
mount a.dsk /a
mount b.dsk /b
cp /a/etc/passwd /b/tmp/passwd     # two different images, one command
```

Source and destination resolve to different `RW` handles, and
`rw_copy_between` streams from one to the other without a host temporary.

A leading `@` escapes to the **host** filesystem:

```
cp @./hostfile /a/tmp/x            # host  → image
cp /a/etc/motd @./motd.txt         # image → host
```

So `cp` spans three namespaces — image, other image, host — through one
syntax. `get` and `put` remain as the explicit forms for people who prefer
them.

`mv` is a rename **within one image**; a cross-image `mv` is not a rename and is
not silently emulated as copy-then-delete.

---

## 5. Why the shell is a front-end

Every operation the shell offers — `mkdir`, `rm`, `chmod`, `put` — is an
`rw_*` call. The shell contributes path resolution, the mount table, and
command parsing, and contributes **no filesystem logic at all**.

This is what makes the test suite's shell checks meaningful: a shell session
that creates a directory, `cd`s into it, and puts a file must end `fsck clean`,
and it does so for the same reason the batch commands do — it *is* the batch
command underneath.

If you extend the shell, respect `route()`, `parentof()`, and `rpath()`.
Special-casing "the single mount" is the failure mode, because it works
perfectly until someone mounts a second image.

---

## 6. For a maintainer

- **Never special-case a single mount.** Everything routes, including `/`.
- **The prefix test needs the `/` check.** `/usr` must not capture
  `/usrlocal`.
- **Longest prefix wins**, so deeper mounts shadow shallower ones. Preserve the
  ordering, not the table order.
- **Resolve each side of `cp`/`mv` independently.** That is what cross-image
  copy is.
- **`@` is resolved here, in the front-end.** Host paths never reach
  `s5fs_rw`.
- **No filesystem logic in this file.** If you are writing a directory-entry
  offset, you are in the wrong layer.

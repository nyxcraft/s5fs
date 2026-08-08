#!/usr/bin/env python3
"""Corruption fuzzer for the s5fs readers.  Optional: `make fuzz`.

This is NOT part of `make test` and CI does not run it.  The regression suite is
deliberately sh + coreutils with no dependencies; this needs python3, and it is a
search rather than a check -- it either finds something or it does not.

Why it exists: every parser here consumes a disk image, and an image is
untrusted input the moment it comes from anywhere but this tool.  Targeted tests
cover the corruptions somebody thought of.  This covers the ones nobody did --
it is how the out-of-range s_nfree was found (fsck indexing free_[223] of an
int32_t[50], invisible on a normal build because an over-read does not fault).

Build a sanitized binary first, or findings will be silent:

    cc -std=c99 -O1 -g -fsanitize=address,undefined -o bin/s5fs-fuzz src/*.c
    python3 tests/fuzz.py --bin bin/s5fs-fuzz --iters 200

A finding is written out as fuzz-crash-<n>.dsk so it can be replayed.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

# Read-only commands: each walks a different part of the structure.
READERS = [
    ["fsck"], ["fsck", "-l"], ["icheck"], ["dcheck"], ["du"], ["ncheck"],
    ["quot"], ["df"], ["manifest"], ["scavenge"], ["labelit"], ["ls", "/"],
]
# Mutating commands, run on a throwaway copy: these mount the image, so they
# exercise the writer's own decode of the same fields.
WRITERS = [["mkdir", "/fuzzdir"], ["rm", "/a"], ["cp", "/a", "/b"]]


def build_seed(s5, workdir):
    """A structurally complete image: nested dirs, a small file, a file big
    enough to need indirect blocks."""
    tree = os.path.join(workdir, "tree")
    os.makedirs(os.path.join(tree, "sub"), exist_ok=True)
    with open(os.path.join(tree, "a"), "w") as f:
        f.write("hello\n")
    with open(os.path.join(tree, "big"), "w") as f:
        for i in range(20000):
            f.write("%d\n" % i)
    with open(os.path.join(tree, "sub", "bin"), "wb") as f:
        f.write(bytes(range(256)) * 12)
    seed = os.path.join(workdir, "seed.dsk")
    subprocess.run([s5, "mktree", "-B", "512", "-b", "3000", tree, seed],
                   capture_output=True, check=True)
    return seed


def corrupt(data, rnd, sb_off, isize_bytes):
    """Corrupt the metadata, not the payload.

    Random bytes across a whole image almost always land in file data and prove
    nothing -- the parsers live in the superblock and the i-list."""
    d = bytearray(data)
    mode = rnd.randrange(4)
    if mode == 0:                                  # shred the superblock
        for off in range(sb_off, sb_off + 512):
            if rnd.random() < 0.30:
                d[off] = rnd.randrange(256)
    elif mode == 1:                                # extremes in the sizing fields
        for off in (sb_off, sb_off + 2, sb_off + 6):   # isize, fsize, nfree
            for k in range(4):
                d[off + k] = rnd.choice([0x00, 0xFF, 0x7F, 0x80])
    elif mode == 2:                                # shred the head of the i-list
        for off in range(sb_off + 512, min(sb_off + 512 + 2048, isize_bytes)):
            if rnd.random() < 0.30:
                d[off] = rnd.randrange(256)
    else:                                          # scattered metadata bytes
        for _ in range(rnd.randint(1, 12)):
            d[rnd.randrange(0, isize_bytes)] = rnd.randrange(256)
    return bytes(d), mode


def run(binary, args, timeout):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1")
    try:
        p = subprocess.run([binary] + args, capture_output=True,
                           timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return "HANG", ""
    if p.returncode < 0 or p.returncode > 128:
        err = p.stderr.decode("utf-8", "replace")
        for line in err.splitlines():
            if "ERROR:" in line or "runtime error" in line:
                return "CRASH", line.strip()
        return "CRASH", "signal %d" % abs(p.returncode)
    return None, ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default="bin/s5fs", help="binary to fuzz (build it sanitized)")
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=20)
    args = ap.parse_args()

    s5 = os.path.abspath(args.bin)
    if not os.access(s5, os.X_OK):
        sys.exit("%s: not executable -- build it first" % s5)

    work = tempfile.mkdtemp(prefix="s5fs-fuzz.")
    try:
        seed_img = build_seed(s5, work)
        data = open(seed_img, "rb").read()
        sb_off, isize_bytes = 512, min(len(data), 512 * 400)
        rnd = random.Random(args.seed)
        target = os.path.join(work, "c.dsk")
        findings = 0

        for it in range(args.iters):
            bad, mode = corrupt(data, rnd, sb_off, isize_bytes)
            for cmd in READERS + WRITERS:
                open(target, "wb").write(bad)
                argv = [cmd[0], "-B", "512", target] + cmd[1:]
                if cmd[0] == "cp":       # cp takes its operands after the image
                    argv = ["cp", "-B", "512", target] + cmd[1:]
                kind, detail = run(s5, argv, args.timeout)
                if kind:
                    out = "fuzz-crash-%d.dsk" % findings
                    shutil.copy(target, out)
                    print("%-5s iter=%d mode=%d  %-22s %s\n      saved: %s"
                          % (kind, it, mode, " ".join(cmd), detail, out))
                    findings += 1
        print("iterations=%d findings=%d" % (args.iters, findings))
        return 1 if findings else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

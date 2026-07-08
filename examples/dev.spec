# Example /dev spec for `s5fs mktree -D`.
#
#   name  type  major  minor  [octal-mode]
#     type  = c (character) or b (block)
#     mode  = permission bits, default 0600
#
# IMPORTANT: the major numbers must match the TARGET kernel's cdevsw[]/bdevsw[]
# (they are configuration-specific -- read the kernel's c.c).  The values below
# are illustrative of a small 2.9BSD /dev; adjust for your kernel.

# character devices
console c 0 0 0600
tty     c 1 0 0666
mem     c 2 0 0640
kmem    c 2 1 0640
null    c 2 2 0666
rrl0    c 8 0 0600     # raw RL01/02 drive 0
rrp0    c 9 0 0600     # raw RP0x drive 0

# block devices
rl0     b 5 0 0600     # RL01/02 drive 0
rp0     b 2 0 0600     # RP0x drive 0
swap    b 1 0 0600

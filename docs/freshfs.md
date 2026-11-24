# FreshFS on-disk format (version 1)

FreshFS is Fresh OS's persistent file system. The implementation in
`lib/freshfs/` is portable C used both by the kernel and by the host tool
`ffstool`. All integers are little-endian. The block size is 4096 bytes.

## Layout

| Blocks | Contents |
|---|---|
| 0 | superblock |
| `ibitmap_start` (= 1) ... | inode bitmap, 1 bit per inode, 1 = in use |
| `bbitmap_start` ... | block bitmap, 1 bit per block, 1 = in use |
| `itable_start` ... | inode table, 128-byte inodes, 32 per block |
| `data_start` ... `total-2` | data blocks |
| `total-1` | backup copy of the superblock |

`mkfs` creates one inode per 4 blocks (at least 64). Bits for metadata blocks,
the backup superblock and padding past the end are always set.

## Superblock (128 bytes)

| Offset | Field |
|---|---|
| 0 | magic `"FRESHFS1"` |
| 8 | version (1), block size (4096) |
| 16 | total blocks, inode count |
| 24 | inode bitmap start/blocks, block bitmap start/blocks, inode table start/blocks |
| 48 | data start |
| 52 | free blocks, free inodes (exact only when clean) |
| 60 | state: 1 clean, 2 dirty |
| 64 | mount count, reserved |
| 72 | creation time, last mount time (Unix seconds) |
| 88 | label (32 bytes) |
| 120 | CRC-32 of the superblock with this field zero, reserved |

## Inode (128 bytes)

| Offset | Field |
|---|---|
| 0 | type: 0 free, 1 file, 2 directory |
| 4 | flags (unused) |
| 8 | size in bytes |
| 16 | creation time, modification time |
| 32 | 12 direct block pointers |
| 80 | single-indirect block (1024 pointers) |
| 84 | double-indirect block |

Inode 0 is reserved, inode 1 is the root directory. A zero block pointer is a
hole that reads as zeros. Maximum file size is just over 4 GiB.

## Directories

A directory's data is an array of 64-byte entries in whole blocks (its size is
always a multiple of 4096):

| Offset | Field |
|---|---|
| 0 | inode number (0 = free slot) |
| 4 | type (1 file, 2 directory) |
| 5 | name length (1-57) |
| 6 | name (not NUL terminated) |

There are no `.` and `..` entries; the VFS resolves them from the path. Names
may contain any byte except `/` and NUL.

## Crash consistency

Writes are ordered so that a crash can only leak space, never leave a
reference to something unallocated:

1. allocation bitmaps are updated first,
2. then data and indirect blocks,
3. then the inode,
4. and directory entries last (removal goes in the reverse order).

Mounting read-write marks the superblock dirty; a clean unmount (shutdown,
reboot) marks it clean. If the kernel finds a dirty file system at boot it runs
`ffs_fsck` in repair mode before mounting. The checker walks the tree from the
root, validates every inode, pointer and entry, removes anything invalid,
frees unreachable inodes, rebuilds both bitmaps from what is reachable and
rewrites the superblock (and restores it from the backup copy if the primary is
damaged).

## Host tool

```bash
build/tools/ffstool fsck build/fresh-os.img       # check (whole disk images are understood)
build/tools/ffstool fsck -y build/fresh-os.img    # repair
build/tools/ffstool ls build/fresh-os.img /
build/tools/ffstool cat build/fresh-os.img /README.txt
build/tools/ffstool put build/fresh-os.img local.txt /local.txt
```

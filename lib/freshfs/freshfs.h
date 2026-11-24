/* FreshFS - the Fresh OS on-disk file system.
 * Portable core shared by the kernel and the host tools (ffstool).
 *
 * Disk layout (4096-byte blocks):
 *   block 0                    superblock
 *   ibitmap_start..            inode bitmap   (1 bit per inode, 1 = used)
 *   bbitmap_start..            block bitmap   (1 bit per block, 1 = used)
 *   itable_start..             inode table    (128-byte inodes, 32 per block)
 *   data_start..total-2        data blocks
 *   total-1                    backup superblock
 * Inode 0 is reserved, inode 1 is the root directory. Files use 12 direct,
 * one single-indirect and one double-indirect block pointer. Directories
 * are arrays of 64-byte entries (ino 0 = free slot) in whole blocks.
 *
 * Consistency: writes are ordered (bitmaps -> data -> inode -> directory)
 * so that a crash can only leak resources, never create a reference to an
 * unallocated block or inode. The superblock carries a clean/dirty flag;
 * a dirty file system is repaired by ffs_fsck() before it is mounted. */
#ifndef FRESHFS_H
#define FRESHFS_H

#include <stdint.h>
#include <stddef.h>

#define FFS_MAGIC 0x3153464853455246ULL /* "FRESHFS1" */
#define FFS_VERSION 1
#define FFS_BLOCK 4096
#define FFS_ROOT_INO 1
#define FFS_INODE_SIZE 128
#define FFS_INODES_PER_BLOCK (FFS_BLOCK / FFS_INODE_SIZE)
#define FFS_NDIRECT 12
#define FFS_PTRS (FFS_BLOCK / 4)
#define FFS_DIRENT_SIZE 64
#define FFS_DIRENTS_PER_BLOCK (FFS_BLOCK / FFS_DIRENT_SIZE)
#define FFS_NAME_MAX 57
#define FFS_BITS_PER_BLOCK (FFS_BLOCK * 8)
#define FFS_MAX_FILE_BLOCKS ((uint64_t)FFS_NDIRECT + FFS_PTRS + (uint64_t)FFS_PTRS * FFS_PTRS)

#define FFS_T_FREE 0
#define FFS_T_FILE 1
#define FFS_T_DIR 2

#define FFS_STATE_CLEAN 1
#define FFS_STATE_DIRTY 2

/* error codes (negated on return); values match Fresh OS errno */
#define FFS_EIO 5
#define FFS_ENOENT 2
#define FFS_ENOMEM 12
#define FFS_EEXIST 17
#define FFS_ENOTDIR 20
#define FFS_EISDIR 21
#define FFS_EINVAL 22
#define FFS_EFBIG 27
#define FFS_ENOSPC 28
#define FFS_EROFS 30
#define FFS_ENAMETOOLONG 36
#define FFS_ENOTEMPTY 39

struct ffs_super {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t ibitmap_start, ibitmap_blocks;
    uint32_t bbitmap_start, bbitmap_blocks;
    uint32_t itable_start, itable_blocks;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t state;
    uint32_t mount_count;
    uint32_t reserved0;
    uint64_t created;
    uint64_t last_mount;
    char label[32];
    uint32_t checksum;
    uint32_t reserved1;
};

struct ffs_inode {
    uint16_t type;
    uint16_t reserved16;
    uint32_t flags;
    uint64_t size;
    uint64_t ctime;
    uint64_t mtime;
    uint32_t direct[FFS_NDIRECT];
    uint32_t indirect;
    uint32_t dindirect;
    uint8_t pad[FFS_INODE_SIZE - 88];
};

struct ffs_dirent {
    uint32_t ino;
    uint8_t type;
    uint8_t name_len;
    char name[58];
};

/* block device: callbacks return 0 on success */
typedef struct ffs_dev {
    void *ctx;
    int (*read)(void *ctx, uint32_t blk, void *buf);
    int (*write)(void *ctx, uint32_t blk, const void *buf);
    uint32_t nblocks;
} ffs_dev;

typedef struct ffs_fs {
    ffs_dev dev;
    struct ffs_super sb;
    int readonly;
    uint32_t hint_blk;
    uint32_t hint_ino;
} ffs_fs;

struct ffs_stat {
    uint32_t ino;
    uint32_t type;
    uint64_t size;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t blocks;
};

struct ffs_entry {
    uint32_t ino;
    uint32_t type;
    char name[FFS_NAME_MAX + 1];
};

struct ffs_fsck_report {
    uint32_t errors;     /* problems found */
    uint32_t fixed;      /* problems repaired */
    uint32_t files, dirs;
    uint32_t used_blocks, free_blocks;
    uint32_t used_inodes, free_inodes;
    int used_backup_super;
};

/* platform hooks */
void *ffs_malloc(size_t n);
void ffs_free(void *p);

uint32_t ffs_crc32(const void *data, size_t len);
int ffs_super_valid(const struct ffs_super *sb, uint32_t dev_blocks);

int ffs_mkfs(ffs_dev *dev, const char *label, uint64_t now);
int ffs_mount(ffs_fs *fs, const ffs_dev *dev, int readonly, uint64_t now);
int ffs_sync(ffs_fs *fs);
int ffs_unmount(ffs_fs *fs);

int ffs_lookup(ffs_fs *fs, uint32_t dir, const char *name, uint32_t *ino);
int ffs_stat(ffs_fs *fs, uint32_t ino, struct ffs_stat *st);
int64_t ffs_read(ffs_fs *fs, uint32_t ino, uint64_t off, void *buf, uint64_t n);
int64_t ffs_write(ffs_fs *fs, uint32_t ino, uint64_t off, const void *buf, uint64_t n, uint64_t now);
int ffs_truncate(ffs_fs *fs, uint32_t ino, uint64_t size, uint64_t now);
int ffs_create(ffs_fs *fs, uint32_t dir, const char *name, int type, uint64_t now, uint32_t *ino);
int ffs_unlink(ffs_fs *fs, uint32_t dir, const char *name);
int ffs_rename(ffs_fs *fs, uint32_t odir, const char *oname, uint32_t ndir, const char *nname, uint64_t now);
/* *cursor starts at 0; returns 1 with an entry, 0 at end, <0 on error */
int ffs_readdir(ffs_fs *fs, uint32_t dir, uint64_t *cursor, struct ffs_entry *out);
int ffs_name_valid(const char *name);

/* check (repair != 0: fix) a file system. Returns 0 clean, 1 errors were
 * fixed, 2 errors remain, <0 if the file system is unusable. */
int ffs_fsck(ffs_dev *dev, int repair, struct ffs_fsck_report *rep, void (*log)(const char *msg));

#endif

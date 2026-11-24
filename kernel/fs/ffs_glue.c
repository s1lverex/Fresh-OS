/* Fresh OS kernel - FreshFS on a disk partition, mounted at /data. */
#include "vfs.h"
#include "../mm/mm.h"
#include "../dev/dev.h"
#include "freshfs/freshfs.h"

_Static_assert(FFS_T_FILE == FT_FILE && FFS_T_DIR == FT_DIR, "type codes");
_Static_assert(FFS_ENOENT == ENOENT && FFS_ENOSPC == ENOSPC && FFS_ENOTEMPTY == ENOTEMPTY, "errno codes");

static const uint8_t FRESHFS_GUID[16] = {0xC0, 0xA1, 0xE5, 0xF7, 0x2D, 0x3B, 0x6F, 0x4E,
                                         0x9A, 0x8B, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B};

#define CACHE_N 256

struct part {
    struct blkdev *dev;
    uint64_t start;
};

struct cache_ent {
    uint32_t blk;
    bool valid;
    uint8_t *data;
};

static struct part g_part;
static ffs_fs g_fs;
static bool g_mounted;
static struct cache_ent cache[CACHE_N];

void *ffs_malloc(size_t n) { return kmalloc(n); }
void ffs_free(void *p) { kfree(p); }

static int part_read(void *ctx, uint32_t blk, void *buf)
{
    struct part *p = ctx;
    struct cache_ent *c = &cache[blk % CACHE_N];
    if (c->valid && c->blk == blk) {
        memcpy(buf, c->data, FFS_BLOCK);
        return 0;
    }
    int rc = p->dev->read(p->dev, p->start + (uint64_t)blk * 8, 8, buf);
    if (rc)
        return rc;
    if (!c->data)
        c->data = kmalloc(FFS_BLOCK);
    if (c->data) {
        memcpy(c->data, buf, FFS_BLOCK);
        c->blk = blk;
        c->valid = true;
    }
    return 0;
}

static int part_write(void *ctx, uint32_t blk, const void *buf)
{
    struct part *p = ctx;
    struct cache_ent *c = &cache[blk % CACHE_N];
    int rc = p->dev->write(p->dev, p->start + (uint64_t)blk * 8, 8, buf);
    if (rc) {
        if (c->blk == blk)
            c->valid = false;
        return rc;
    }
    if (!c->data)
        c->data = kmalloc(FFS_BLOCK);
    if (c->data) {
        memcpy(c->data, buf, FFS_BLOCK);
        c->blk = blk;
        c->valid = true;
    }
    return 0;
}

static ffs_fs *F(struct mount *m) { return m->priv; }

static int g_lookup(struct mount *m, uint64_t dir, const char *name, uint64_t *ino)
{
    uint32_t i;
    int rc = ffs_lookup(F(m), (uint32_t)dir, name, &i);
    if (!rc)
        *ino = i;
    return rc;
}

static int g_stat(struct mount *m, uint64_t ino, struct fresh_stat *st)
{
    struct ffs_stat s;
    int rc = ffs_stat(F(m), (uint32_t)ino, &s);
    if (rc)
        return rc;
    memset(st, 0, sizeof(*st));
    st->type = s.type;
    st->size = s.type == FFS_T_DIR ? 0 : s.size;
    st->ino = ino;
    st->mtime = s.mtime;
    st->ctime = s.ctime;
    return 0;
}

static long g_read(struct mount *m, uint64_t ino, uint64_t off, void *buf, size_t n)
{
    return (long)ffs_read(F(m), (uint32_t)ino, off, buf, n);
}

static long g_write(struct mount *m, uint64_t ino, uint64_t off, const void *buf, size_t n)
{
    return (long)ffs_write(F(m), (uint32_t)ino, off, buf, n, rtc_unix_time());
}

static int g_truncate(struct mount *m, uint64_t ino, uint64_t size)
{
    return ffs_truncate(F(m), (uint32_t)ino, size, rtc_unix_time());
}

static int g_create(struct mount *m, uint64_t dir, const char *name, int type, uint64_t *ino)
{
    uint32_t i = 0;
    int rc = ffs_create(F(m), (uint32_t)dir, name, type, rtc_unix_time(), &i);
    if (!rc && ino)
        *ino = i;
    return rc;
}

static int g_unlink(struct mount *m, uint64_t dir, const char *name)
{
    return ffs_unlink(F(m), (uint32_t)dir, name);
}

static int g_rename(struct mount *m, uint64_t odir, const char *oname, uint64_t ndir, const char *nname)
{
    return ffs_rename(F(m), (uint32_t)odir, oname, (uint32_t)ndir, nname, rtc_unix_time());
}

static int g_readdir(struct mount *m, uint64_t dir, uint64_t *cursor, struct fresh_dirent *out)
{
    struct ffs_entry e;
    int rc = ffs_readdir(F(m), (uint32_t)dir, cursor, &e);
    if (rc == 1) {
        out->ino = e.ino;
        out->type = e.type;
        strlcpy(out->name, e.name, sizeof(out->name));
    }
    return rc;
}

static int g_statfs(struct mount *m, struct fresh_statfs *st)
{
    ffs_fs *fs = F(m);
    memset(st, 0, sizeof(*st));
    st->block_size = FFS_BLOCK;
    st->total_blocks = fs->sb.total_blocks - fs->sb.data_start - 1;
    st->free_blocks = fs->sb.free_blocks;
    st->total_inodes = fs->sb.inode_count - 1;
    st->free_inodes = fs->sb.free_inodes;
    strlcpy(st->fs_name, "freshfs", sizeof(st->fs_name));
    return 0;
}

static int g_sync(struct mount *m)
{
    int rc = ffs_sync(F(m));
    if (g_part.dev->flush)
        g_part.dev->flush(g_part.dev);
    return rc;
}

static int g_unmount(struct mount *m)
{
    int rc = ffs_unmount(F(m));
    if (g_part.dev->flush)
        g_part.dev->flush(g_part.dev);
    return rc;
}

static const struct fs_ops ops = {
    .lookup = g_lookup,
    .stat = g_stat,
    .read = g_read,
    .write = g_write,
    .truncate = g_truncate,
    .create = g_create,
    .unlink = g_unlink,
    .rename = g_rename,
    .readdir = g_readdir,
    .statfs = g_statfs,
    .sync = g_sync,
    .unmount = g_unmount,
};

static void fsck_log(const char *msg) { kprintf("fsck: %s\n", msg); }

int data_fs_mount(void)
{
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        uint64_t start, count;
        if (gpt_find(d, FRESHFS_GUID, &start, &count))
            continue;
        g_part.dev = d;
        g_part.start = start;
        ffs_dev dev = {&g_part, part_read, part_write, (uint32_t)MIN(count / 8, 0xFFFFFFFFULL)};

        uint8_t *b = kmalloc(FFS_BLOCK);
        if (!b)
            return -ENOMEM;
        bool clean = false;
        if (part_read(&g_part, 0, b) == 0) {
            struct ffs_super *sb = (struct ffs_super *)b;
            clean = ffs_super_valid(sb, dev.nblocks) && sb->state == FFS_STATE_CLEAN;
        }
        kfree(b);
        if (!clean) {
            kprintf("data: %s was not cleanly unmounted, checking file system...\n", d->name);
            struct ffs_fsck_report rep;
            int rc = ffs_fsck(&dev, 1, &rep, fsck_log);
            if (rc < 0) {
                kprintf("data: file system on %s is damaged beyond repair (%d)\n", d->name, rc);
                continue;
            }
            kprintf("data: check done: %u problems fixed, %u files, %u dirs\n", rep.fixed, rep.files, rep.dirs);
        }
        int rc = ffs_mount(&g_fs, &dev, 0, rtc_unix_time());
        if (rc) {
            kprintf("data: mount failed on %s (%d)\n", d->name, rc);
            continue;
        }
        vfs_mount("/data", &ops, &g_fs, FFS_ROOT_INO);
        g_mounted = true;
        kprintf("data: FreshFS '%s' on %s mounted at /data (%u MiB free)\n", g_fs.sb.label, d->name,
                (unsigned)((uint64_t)g_fs.sb.free_blocks * FFS_BLOCK >> 20));
        return 0;
    }
    kprintf("data: no FreshFS partition found, /data is not persistent\n");
    return -ENOENT;
}

bool data_fs_mounted(void) { return g_mounted; }

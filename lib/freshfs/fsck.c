/* FreshFS consistency checker and repairer.
 * Walks the tree from the root, validates every inode, block pointer and
 * directory entry, then rebuilds both allocation bitmaps from what is
 * actually reachable. With repair enabled every problem is fixed so that
 * the result is consistent (data in damaged structures may be dropped). */
#include "freshfs.h"

#ifdef FRESH_KERNEL
#include "fresh/str.h"
#else
#include <stdio.h>
#include <string.h>
#endif

struct ck {
    ffs_dev *dev;
    struct ffs_super sb;
    int repair;
    struct ffs_fsck_report *rep;
    void (*log)(const char *);
    uint8_t *seen;  /* inode bitmap */
    uint8_t *used;  /* block bitmap */
    uint32_t *queue;
    uint32_t qhead, qtail;
    uint32_t logged;
    /* scratch buffers (kept off the stack: the kernel stack is small) */
    uint8_t *ibuf;
    uint32_t *t, *t2, *mt;
    struct ffs_dirent *ents;
};

#define BIT_GET(m, i) ((m)[(i) >> 3] & (1u << ((i) & 7)))
#define BIT_SET(m, i) ((m)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))

static void report(struct ck *c, const char *fmt, uint32_t a, uint32_t b)
{
    c->rep->errors++;
    if (c->repair)
        c->rep->fixed++;
    if (c->log && c->logged < 50) {
        char msg[160];
        snprintf(msg, sizeof(msg), fmt, a, b);
        c->log(msg);
        c->logged++;
        if (c->logged == 50)
            c->log("(further messages suppressed)");
    }
}

static int rdb(struct ck *c, uint32_t b, void *buf) { return c->dev->read(c->dev->ctx, b, buf) ? -FFS_EIO : 0; }
static int wrb(struct ck *c, uint32_t b, const void *buf) { return c->dev->write(c->dev->ctx, b, buf) ? -FFS_EIO : 0; }

static int load_inode(struct ck *c, uint32_t ino, struct ffs_inode *in)
{
    uint8_t *buf = c->ibuf;
    int rc = rdb(c, c->sb.itable_start + ino / FFS_INODES_PER_BLOCK, buf);
    if (!rc)
        memcpy(in, buf + (ino % FFS_INODES_PER_BLOCK) * FFS_INODE_SIZE, sizeof(*in));
    return rc;
}

static int store_inode(struct ck *c, uint32_t ino, const struct ffs_inode *in)
{
    uint8_t *buf = c->ibuf;
    uint32_t blk = c->sb.itable_start + ino / FFS_INODES_PER_BLOCK;
    int rc = rdb(c, blk, buf);
    if (!rc) {
        memcpy(buf + (ino % FFS_INODES_PER_BLOCK) * FFS_INODE_SIZE, in, sizeof(*in));
        rc = wrb(c, blk, buf);
    }
    return rc;
}

/* 1 = claimed ok, 0 = invalid or already used */
static int claim(struct ck *c, uint32_t b)
{
    if (b < c->sb.data_start || b >= c->sb.total_blocks - 1)
        return 0;
    if (BIT_GET(c->used, b))
        return 0;
    BIT_SET(c->used, b);
    return 1;
}

/* validate the block pointers of one inode; *mod set if the inode changed */
static int check_blocks(struct ck *c, uint32_t ino, struct ffs_inode *in, int *mod)
{
    if (in->type == FFS_T_DIR && in->size % FFS_BLOCK) {
        report(c, "inode %u: directory size %u not block aligned", ino, (uint32_t)in->size);
        in->size -= in->size % FFS_BLOCK;
        *mod = 1;
    }
    uint64_t need = (in->size + FFS_BLOCK - 1) / FFS_BLOCK;
    if (need > FFS_MAX_FILE_BLOCKS) {
        report(c, "inode %u: size too large", ino, 0);
        in->size = 0;
        need = 0;
        *mod = 1;
    }
    for (uint32_t i = 0; i < FFS_NDIRECT; i++) {
        uint32_t b = in->direct[i];
        if (!b)
            continue;
        if (i >= need) {
            report(c, "inode %u: block pointer %u beyond end of file", ino, i);
        } else if (!claim(c, b)) {
            report(c, "inode %u: bad or shared block %u", ino, b);
        } else {
            continue;
        }
        in->direct[i] = 0;
        *mod = 1;
    }
    uint32_t *t = c->t, *t2 = c->t2;
    int rc;
    if (in->indirect) {
        if (need <= FFS_NDIRECT || !claim(c, in->indirect)) {
            report(c, "inode %u: bad indirect block %u", ino, in->indirect);
            in->indirect = 0;
            *mod = 1;
        } else {
            if ((rc = rdb(c, in->indirect, t)) != 0)
                return rc;
            int tm = 0;
            for (uint32_t j = 0; j < FFS_PTRS; j++) {
                if (!t[j])
                    continue;
                if (FFS_NDIRECT + j >= need || !claim(c, t[j])) {
                    report(c, "inode %u: bad indirect entry %u", ino, j);
                    t[j] = 0;
                    tm = 1;
                }
            }
            if (tm && c->repair && (rc = wrb(c, in->indirect, t)) != 0)
                return rc;
        }
    }
    if (in->dindirect) {
        if (need <= FFS_NDIRECT + FFS_PTRS || !claim(c, in->dindirect)) {
            report(c, "inode %u: bad double indirect block %u", ino, in->dindirect);
            in->dindirect = 0;
            *mod = 1;
        } else {
            if ((rc = rdb(c, in->dindirect, t)) != 0)
                return rc;
            int tm = 0;
            for (uint32_t k = 0; k < FFS_PTRS; k++) {
                if (!t[k])
                    continue;
                uint64_t base = FFS_NDIRECT + FFS_PTRS + (uint64_t)k * FFS_PTRS;
                if (base >= need || !claim(c, t[k])) {
                    report(c, "inode %u: bad level-2 block %u", ino, t[k]);
                    t[k] = 0;
                    tm = 1;
                    continue;
                }
                if ((rc = rdb(c, t[k], t2)) != 0)
                    return rc;
                int t2m = 0;
                for (uint32_t j = 0; j < FFS_PTRS; j++) {
                    if (!t2[j])
                        continue;
                    if (base + j >= need || !claim(c, t2[j])) {
                        report(c, "inode %u: bad double indirect entry %u", ino, j);
                        t2[j] = 0;
                        t2m = 1;
                    }
                }
                if (t2m && c->repair && (rc = wrb(c, t[k], t2)) != 0)
                    return rc;
            }
            if (tm && c->repair && (rc = wrb(c, in->dindirect, t)) != 0)
                return rc;
        }
    }
    return 0;
}

/* physical block of logical block lb (pointers already validated) */
static int map_block(struct ck *c, const struct ffs_inode *in, uint64_t lb, uint32_t *pb)
{
    uint32_t *t = c->mt;
    *pb = 0;
    if (lb < FFS_NDIRECT) {
        *pb = in->direct[lb];
        return 0;
    }
    lb -= FFS_NDIRECT;
    if (lb < FFS_PTRS) {
        if (!in->indirect)
            return 0;
        int rc = rdb(c, in->indirect, t);
        if (!rc)
            *pb = t[lb];
        return rc;
    }
    lb -= FFS_PTRS;
    if (!in->dindirect)
        return 0;
    int rc = rdb(c, in->dindirect, t);
    if (rc || !t[lb / FFS_PTRS])
        return rc;
    uint32_t l2 = t[lb / FFS_PTRS];
    rc = rdb(c, l2, t);
    if (!rc)
        *pb = t[lb % FFS_PTRS];
    return rc;
}

static int name_ok(const struct ffs_dirent *e)
{
    if (e->name_len == 0 || e->name_len > FFS_NAME_MAX)
        return 0;
    for (int i = 0; i < e->name_len; i++)
        if (e->name[i] == '/' || e->name[i] == 0)
            return 0;
    if (e->name_len == 1 && e->name[0] == '.')
        return 0;
    if (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.')
        return 0;
    return 1;
}

static int scan_dir(struct ck *c, uint32_t dino, const struct ffs_inode *dir)
{
    struct ffs_dirent *ents = c->ents;
    for (uint64_t lb = 0; lb < dir->size / FFS_BLOCK; lb++) {
        uint32_t pb;
        int rc = map_block(c, dir, lb, &pb);
        if (rc)
            return rc;
        if (!pb)
            continue;
        if ((rc = rdb(c, pb, ents)) != 0)
            return rc;
        int dirty = 0;
        for (int i = 0; i < FFS_DIRENTS_PER_BLOCK; i++) {
            struct ffs_dirent *e = &ents[i];
            if (!e->ino)
                continue;
            int bad = 0;
            struct ffs_inode child;
            if (!name_ok(e) || e->ino < 2 || e->ino >= c->sb.inode_count) {
                bad = 1;
            } else {
                if ((rc = load_inode(c, e->ino, &child)) != 0)
                    return rc;
                if (child.type != FFS_T_FILE && child.type != FFS_T_DIR)
                    bad = 1;
                else if (BIT_GET(c->seen, e->ino))
                    bad = 1;
            }
            if (bad) {
                report(c, "directory %u: removing invalid entry -> inode %u", dino, e->ino);
                memset(e, 0, sizeof(*e));
                dirty = 1;
                continue;
            }
            if (e->type != child.type) {
                report(c, "directory %u: wrong type in entry for inode %u", dino, e->ino);
                e->type = (uint8_t)child.type;
                dirty = 1;
            }
            BIT_SET(c->seen, e->ino);
            c->queue[c->qtail++] = e->ino;
        }
        if (dirty && c->repair && (rc = wrb(c, pb, ents)) != 0)
            return rc;
    }
    return 0;
}

static int sync_bitmap(struct ck *c, uint32_t start, uint32_t nblocks, uint8_t *expect, const char *what)
{
    uint8_t *buf = c->ibuf;
    for (uint32_t i = 0; i < nblocks; i++) {
        int rc = rdb(c, start + i, buf);
        if (rc)
            return rc;
        uint8_t *want = expect + (uint64_t)i * FFS_BLOCK;
        if (memcmp(buf, want, FFS_BLOCK) == 0)
            continue;
        uint32_t diff = 0;
        for (int k = 0; k < FFS_BLOCK; k++) {
            uint8_t x = buf[k] ^ want[k];
            while (x) {
                diff += x & 1;
                x >>= 1;
            }
        }
        if (c->log && c->logged < 50) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s bitmap block %u: %u bits wrong", what, i, diff);
            c->log(msg);
            c->logged++;
        }
        c->rep->errors++;
        if (c->repair) {
            c->rep->fixed++;
            if ((rc = wrb(c, start + i, want)) != 0)
                return rc;
        }
    }
    return 0;
}

int ffs_fsck(ffs_dev *dev, int repair, struct ffs_fsck_report *rep, void (*log)(const char *msg))
{
    struct ck c;
    memset(&c, 0, sizeof(c));
    memset(rep, 0, sizeof(*rep));
    c.dev = dev;
    c.repair = repair;
    c.rep = rep;
    c.log = log;

    uint8_t *blk = ffs_malloc(FFS_BLOCK);
    if (!blk)
        return -FFS_ENOMEM;
    int rc = rdb(&c, 0, blk);
    if (rc) {
        ffs_free(blk);
        return rc;
    }
    memcpy(&c.sb, blk, sizeof(c.sb));
    int super_dirty = 0;
    if (!ffs_super_valid(&c.sb, dev->nblocks)) {
        report(&c, "primary superblock invalid", 0, 0);
        int found = 0;
        /* the backup lives in the last block of the file system; the file
         * system normally spans the whole device */
        if (dev->nblocks >= 16 && rdb(&c, dev->nblocks - 1, blk) == 0) {
            memcpy(&c.sb, blk, sizeof(c.sb));
            if (ffs_super_valid(&c.sb, dev->nblocks) && c.sb.total_blocks == dev->nblocks)
                found = 1;
        }
        if (!found) {
            ffs_free(blk);
            if (log)
                log("no valid superblock found");
            return -FFS_EINVAL;
        }
        if (log)
            log("using backup superblock");
        rep->used_backup_super = 1;
        super_dirty = 1;
    }
    ffs_free(blk);

    uint64_t ibytes = (uint64_t)c.sb.ibitmap_blocks * FFS_BLOCK;
    uint64_t bbytes = (uint64_t)c.sb.bbitmap_blocks * FFS_BLOCK;
    c.seen = ffs_malloc(ibytes);
    c.used = ffs_malloc(bbytes);
    c.queue = ffs_malloc((uint64_t)c.sb.inode_count * sizeof(uint32_t));
    c.ibuf = ffs_malloc(FFS_BLOCK);
    c.t = ffs_malloc(FFS_BLOCK);
    c.t2 = ffs_malloc(FFS_BLOCK);
    c.mt = ffs_malloc(FFS_BLOCK);
    c.ents = ffs_malloc(FFS_BLOCK);
    if (!c.seen || !c.used || !c.queue || !c.ibuf || !c.t || !c.t2 || !c.mt || !c.ents) {
        rc = -FFS_ENOMEM;
        goto out;
    }
    memset(c.seen, 0, ibytes);
    memset(c.used, 0, bbytes);
    /* padding bits and reserved entries count as used */
    for (uint64_t i = 0; i < ibytes * 8; i++)
        if (i == 0 || i >= c.sb.inode_count)
            BIT_SET(c.seen, i);
    for (uint64_t b = 0; b < bbytes * 8; b++)
        if (b < c.sb.data_start || b >= (uint64_t)c.sb.total_blocks - 1)
            BIT_SET(c.used, b);

    /* root */
    struct ffs_inode in;
    if ((rc = load_inode(&c, FFS_ROOT_INO, &in)) != 0)
        goto out;
    if (in.type != FFS_T_DIR) {
        report(&c, "root inode is not a directory", 0, 0);
        memset(&in, 0, sizeof(in));
        in.type = FFS_T_DIR;
        if (repair && (rc = store_inode(&c, FFS_ROOT_INO, &in)) != 0)
            goto out;
    }
    BIT_SET(c.seen, FFS_ROOT_INO);
    c.queue[c.qtail++] = FFS_ROOT_INO;

    while (c.qhead < c.qtail) {
        uint32_t ino = c.queue[c.qhead++];
        if ((rc = load_inode(&c, ino, &in)) != 0)
            goto out;
        if (ino == FFS_ROOT_INO && in.type != FFS_T_DIR) { /* not repaired (check-only mode) */
            memset(&in, 0, sizeof(in));
            in.type = FFS_T_DIR;
        }
        int mod = 0;
        if ((rc = check_blocks(&c, ino, &in, &mod)) != 0)
            goto out;
        if (mod && repair && (rc = store_inode(&c, ino, &in)) != 0)
            goto out;
        if (in.type == FFS_T_DIR) {
            rep->dirs++;
            if ((rc = scan_dir(&c, ino, &in)) != 0)
                goto out;
        } else {
            rep->files++;
        }
    }

    /* orphaned or garbage inodes */
    {
        uint8_t *buf = c.ibuf;
        for (uint32_t tb = 0; tb < c.sb.itable_blocks; tb++) {
            if ((rc = rdb(&c, c.sb.itable_start + tb, buf)) != 0)
                goto out;
            int dirty = 0;
            for (uint32_t k = 0; k < FFS_INODES_PER_BLOCK; k++) {
                uint32_t ino = tb * FFS_INODES_PER_BLOCK + k;
                if (ino == 0 || BIT_GET(c.seen, ino))
                    continue;
                struct ffs_inode *p = (struct ffs_inode *)(buf + k * FFS_INODE_SIZE);
                if (p->type != FFS_T_FREE) {
                    report(&c, "inode %u: allocated but not referenced (type %u)", ino, p->type);
                    memset(p, 0, sizeof(*p));
                    dirty = 1;
                }
            }
            if (dirty && repair && (rc = wrb(&c, c.sb.itable_start + tb, buf)) != 0)
                goto out;
        }
    }

    if ((rc = sync_bitmap(&c, c.sb.ibitmap_start, c.sb.ibitmap_blocks, c.seen, "inode")) != 0)
        goto out;
    if ((rc = sync_bitmap(&c, c.sb.bbitmap_start, c.sb.bbitmap_blocks, c.used, "block")) != 0)
        goto out;

    uint32_t used_i = 0, used_b = 0;
    for (uint32_t i = 1; i < c.sb.inode_count; i++)
        if (BIT_GET(c.seen, i))
            used_i++;
    for (uint32_t b = c.sb.data_start; b < c.sb.total_blocks - 1; b++)
        if (BIT_GET(c.used, b))
            used_b++;
    rep->used_inodes = used_i;
    rep->free_inodes = c.sb.inode_count - 1 - used_i;
    rep->used_blocks = used_b;
    rep->free_blocks = c.sb.total_blocks - c.sb.data_start - 1 - used_b;
    if (c.sb.free_inodes != rep->free_inodes || c.sb.free_blocks != rep->free_blocks) {
        if (c.sb.state == FFS_STATE_CLEAN)
            report(&c, "superblock free counts wrong (%u/%u)", c.sb.free_inodes, c.sb.free_blocks);
        super_dirty = 1;
    }
    if (c.sb.state != FFS_STATE_CLEAN)
        super_dirty = 1;
    if (repair && (super_dirty || rep->errors)) {
        c.sb.free_inodes = rep->free_inodes;
        c.sb.free_blocks = rep->free_blocks;
        c.sb.state = FFS_STATE_CLEAN;
        /* write primary and backup superblock */
        uint8_t *sbb = ffs_malloc(FFS_BLOCK);
        if (!sbb) {
            rc = -FFS_ENOMEM;
            goto out;
        }
        memset(sbb, 0, FFS_BLOCK);
        c.sb.checksum = 0;
        c.sb.checksum = ffs_crc32(&c.sb, sizeof(c.sb));
        memcpy(sbb, &c.sb, sizeof(c.sb));
        rc = wrb(&c, 0, sbb);
        if (!rc)
            rc = wrb(&c, c.sb.total_blocks - 1, sbb);
        ffs_free(sbb);
        if (rc)
            goto out;
    }
    rc = rep->errors ? (repair ? 1 : 2) : 0;
out:
    ffs_free(c.seen);
    ffs_free(c.used);
    ffs_free(c.queue);
    ffs_free(c.ibuf);
    ffs_free(c.t);
    ffs_free(c.t2);
    ffs_free(c.mt);
    ffs_free(c.ents);
    return rc;
}

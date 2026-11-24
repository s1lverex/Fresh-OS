/* FreshFS core: format, mount, files and directories. See freshfs.h. */
#include "freshfs.h"

#ifdef FRESH_KERNEL
#include "fresh/str.h"
#else
#include <string.h>
#endif

_Static_assert(sizeof(struct ffs_super) == 128, "superblock layout");
_Static_assert(sizeof(struct ffs_inode) == FFS_INODE_SIZE, "inode layout");
_Static_assert(sizeof(struct ffs_dirent) == FFS_DIRENT_SIZE, "dirent layout");

/* ------------------------------------------------------------ helpers */
uint32_t ffs_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t super_checksum(const struct ffs_super *sb)
{
    struct ffs_super tmp = *sb;
    tmp.checksum = 0;
    return ffs_crc32(&tmp, sizeof(tmp));
}

int ffs_super_valid(const struct ffs_super *sb, uint32_t dev_blocks)
{
    if (sb->magic != FFS_MAGIC || sb->version != FFS_VERSION || sb->block_size != FFS_BLOCK)
        return 0;
    if (sb->checksum != super_checksum(sb))
        return 0;
    if (sb->total_blocks < 16 || sb->total_blocks > dev_blocks)
        return 0;
    if (sb->inode_count < 2 || sb->inode_count != sb->itable_blocks * FFS_INODES_PER_BLOCK)
        return 0;
    if (sb->ibitmap_start != 1 || sb->ibitmap_blocks != (sb->inode_count + FFS_BITS_PER_BLOCK - 1) / FFS_BITS_PER_BLOCK)
        return 0;
    if (sb->bbitmap_start != sb->ibitmap_start + sb->ibitmap_blocks ||
        sb->bbitmap_blocks != (sb->total_blocks + FFS_BITS_PER_BLOCK - 1) / FFS_BITS_PER_BLOCK)
        return 0;
    if (sb->itable_start != sb->bbitmap_start + sb->bbitmap_blocks)
        return 0;
    if (sb->data_start != sb->itable_start + sb->itable_blocks || sb->data_start + 2 > sb->total_blocks)
        return 0;
    return 1;
}

static void *blkbuf(void) { return ffs_malloc(FFS_BLOCK); }

static int rd(ffs_fs *fs, uint32_t b, void *buf)
{
    if (b >= fs->sb.total_blocks)
        return -FFS_EIO;
    return fs->dev.read(fs->dev.ctx, b, buf) ? -FFS_EIO : 0;
}

static int wr(ffs_fs *fs, uint32_t b, const void *buf)
{
    if (fs->readonly)
        return -FFS_EROFS;
    if (b >= fs->sb.total_blocks)
        return -FFS_EIO;
    return fs->dev.write(fs->dev.ctx, b, buf) ? -FFS_EIO : 0;
}

static int write_super(ffs_fs *fs, int backup)
{
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    memset(b, 0, FFS_BLOCK);
    fs->sb.checksum = super_checksum(&fs->sb);
    memcpy(b, &fs->sb, sizeof(fs->sb));
    int rc = fs->dev.write(fs->dev.ctx, 0, b) ? -FFS_EIO : 0;
    if (!rc && backup)
        rc = fs->dev.write(fs->dev.ctx, fs->sb.total_blocks - 1, b) ? -FFS_EIO : 0;
    ffs_free(b);
    return rc;
}

int ffs_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n == 0)
        return -FFS_EINVAL;
    if (n > FFS_NAME_MAX)
        return -FFS_ENAMETOOLONG;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return -FFS_EINVAL;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/')
            return -FFS_EINVAL;
    return 0;
}

/* ------------------------------------------------------------ inodes */
static int read_inode(ffs_fs *fs, uint32_t ino, struct ffs_inode *out)
{
    if (ino == 0 || ino >= fs->sb.inode_count)
        return -FFS_EINVAL;
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    int rc = rd(fs, fs->sb.itable_start + ino / FFS_INODES_PER_BLOCK, b);
    if (!rc)
        memcpy(out, b + (ino % FFS_INODES_PER_BLOCK) * FFS_INODE_SIZE, sizeof(*out));
    ffs_free(b);
    return rc;
}

static int write_inode(ffs_fs *fs, uint32_t ino, const struct ffs_inode *in)
{
    if (ino == 0 || ino >= fs->sb.inode_count)
        return -FFS_EINVAL;
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    uint32_t blk = fs->sb.itable_start + ino / FFS_INODES_PER_BLOCK;
    int rc = rd(fs, blk, b);
    if (!rc) {
        memcpy(b + (ino % FFS_INODES_PER_BLOCK) * FFS_INODE_SIZE, in, sizeof(*in));
        rc = wr(fs, blk, b);
    }
    ffs_free(b);
    return rc;
}

/* ------------------------------------------------------------ bitmaps */
static int bitmap_alloc(ffs_fs *fs, uint32_t start, uint32_t nblocks, uint32_t lo, uint32_t hi, uint32_t *hint,
                        uint32_t *out)
{
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    uint32_t span = hi - lo;
    uint32_t first = (*hint >= lo && *hint < hi) ? *hint : lo;
    int rc = -FFS_ENOSPC;
    uint32_t cached = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t idx = lo + (first - lo + k) % span;
        uint32_t bb = idx / FFS_BITS_PER_BLOCK;
        if (bb >= nblocks)
            break;
        if (bb != cached) {
            if ((rc = rd(fs, start + bb, b)) != 0)
                goto out;
            rc = -FFS_ENOSPC;
            cached = bb;
        }
        uint32_t bit = idx % FFS_BITS_PER_BLOCK;
        if (b[bit / 8] == 0xFF && (bit % 8) == 0 && k + 8 <= span && (first - lo + k) % span + 8 <= span) {
            k += 7;
            continue;
        }
        if (!(b[bit / 8] & (1u << (bit % 8)))) {
            b[bit / 8] |= (uint8_t)(1u << (bit % 8));
            rc = wr(fs, start + bb, b);
            if (!rc) {
                *out = idx;
                *hint = idx + 1;
            }
            goto out;
        }
    }
out:
    ffs_free(b);
    return rc;
}

static int bitmap_clear(ffs_fs *fs, uint32_t start, uint32_t idx)
{
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    uint32_t bb = idx / FFS_BITS_PER_BLOCK, bit = idx % FFS_BITS_PER_BLOCK;
    int rc = rd(fs, start + bb, b);
    if (!rc) {
        b[bit / 8] &= (uint8_t)~(1u << (bit % 8));
        rc = wr(fs, start + bb, b);
    }
    ffs_free(b);
    return rc;
}

static int alloc_block(ffs_fs *fs, uint32_t *out)
{
    if (fs->readonly)
        return -FFS_EROFS;
    int rc = bitmap_alloc(fs, fs->sb.bbitmap_start, fs->sb.bbitmap_blocks, fs->sb.data_start,
                          fs->sb.total_blocks - 1, &fs->hint_blk, out);
    if (!rc && fs->sb.free_blocks)
        fs->sb.free_blocks--;
    return rc;
}

static int free_block(ffs_fs *fs, uint32_t b)
{
    if (b < fs->sb.data_start || b >= fs->sb.total_blocks - 1)
        return -FFS_EIO;
    int rc = bitmap_clear(fs, fs->sb.bbitmap_start, b);
    if (!rc)
        fs->sb.free_blocks++;
    return rc;
}

static int alloc_inode(ffs_fs *fs, uint32_t *out)
{
    if (fs->readonly)
        return -FFS_EROFS;
    int rc = bitmap_alloc(fs, fs->sb.ibitmap_start, fs->sb.ibitmap_blocks, 2, fs->sb.inode_count, &fs->hint_ino,
                          out);
    if (!rc && fs->sb.free_inodes)
        fs->sb.free_inodes--;
    return rc;
}

static int free_inode(ffs_fs *fs, uint32_t ino)
{
    int rc = bitmap_clear(fs, fs->sb.ibitmap_start, ino);
    if (!rc)
        fs->sb.free_inodes++;
    return rc;
}

static int zero_block(ffs_fs *fs, uint32_t b)
{
    uint8_t *z = blkbuf();
    if (!z)
        return -FFS_ENOMEM;
    memset(z, 0, FFS_BLOCK);
    int rc = wr(fs, b, z);
    ffs_free(z);
    return rc;
}

/* ------------------------------------------------------------ block map */
/* Map logical block lb of inode to a physical block. With alloc, missing
 * blocks are allocated (*fresh set to 1 for a new data block, whose content
 * on disk is undefined). Returns 0 and *pb = 0 for a hole without alloc. */
static int get_slot(ffs_fs *fs, uint32_t *slot, int alloc, int *modified)
{
    if (*slot)
        return 0;
    if (!alloc)
        return 0;
    uint32_t nb;
    int rc = alloc_block(fs, &nb);
    if (rc)
        return rc;
    rc = zero_block(fs, nb);
    if (rc) {
        free_block(fs, nb);
        return rc;
    }
    *slot = nb;
    *modified = 1;
    return 0;
}

static int bmap(ffs_fs *fs, struct ffs_inode *in, uint64_t lb, int alloc, uint32_t *pb, int *fresh)
{
    int rc = 0, dummy = 0;
    *pb = 0;
    if (fresh)
        *fresh = 0;
    if (lb >= FFS_MAX_FILE_BLOCKS)
        return -FFS_EFBIG;
    if (lb < FFS_NDIRECT) {
        if (!in->direct[lb] && alloc) {
            uint32_t nb;
            if ((rc = alloc_block(fs, &nb)) != 0)
                return rc;
            in->direct[lb] = nb;
            if (fresh)
                *fresh = 1;
        }
        *pb = in->direct[lb];
        return 0;
    }
    lb -= FFS_NDIRECT;
    uint32_t *tbl = blkbuf();
    if (!tbl)
        return -FFS_ENOMEM;
    uint32_t parent;
    uint64_t idx;
    if (lb < FFS_PTRS) {
        if ((rc = get_slot(fs, &in->indirect, alloc, &dummy)) != 0 || !in->indirect)
            goto out;
        parent = in->indirect;
        idx = lb;
    } else {
        lb -= FFS_PTRS;
        if ((rc = get_slot(fs, &in->dindirect, alloc, &dummy)) != 0 || !in->dindirect)
            goto out;
        if ((rc = rd(fs, in->dindirect, tbl)) != 0)
            goto out;
        int mod = 0;
        uint32_t *slot = &tbl[lb / FFS_PTRS];
        if ((rc = get_slot(fs, slot, alloc, &mod)) != 0)
            goto out;
        if (!*slot)
            goto out;
        if (mod && (rc = wr(fs, in->dindirect, tbl)) != 0)
            goto out;
        parent = *slot;
        idx = lb % FFS_PTRS;
    }
    if ((rc = rd(fs, parent, tbl)) != 0)
        goto out;
    if (!tbl[idx] && alloc) {
        uint32_t nb;
        if ((rc = alloc_block(fs, &nb)) != 0)
            goto out;
        tbl[idx] = nb;
        if ((rc = wr(fs, parent, tbl)) != 0) {
            free_block(fs, nb);
            goto out;
        }
        if (fresh)
            *fresh = 1;
    }
    *pb = tbl[idx];
out:
    ffs_free(tbl);
    return rc;
}

/* free every data block with logical index >= first */
static int free_from(ffs_fs *fs, struct ffs_inode *in, uint64_t first)
{
    int rc = 0;
    for (uint64_t i = 0; i < FFS_NDIRECT; i++) {
        if (i >= first && in->direct[i]) {
            free_block(fs, in->direct[i]);
            in->direct[i] = 0;
        }
    }
    uint32_t *t = blkbuf(), *t2 = blkbuf();
    if (!t || !t2) {
        rc = -FFS_ENOMEM;
        goto out;
    }
    if (in->indirect) {
        if ((rc = rd(fs, in->indirect, t)) != 0)
            goto out;
        int changed = 0;
        for (uint64_t j = 0; j < FFS_PTRS; j++) {
            if (FFS_NDIRECT + j >= first && t[j]) {
                free_block(fs, t[j]);
                t[j] = 0;
                changed = 1;
            }
        }
        if (first <= FFS_NDIRECT) {
            free_block(fs, in->indirect);
            in->indirect = 0;
        } else if (changed && (rc = wr(fs, in->indirect, t)) != 0) {
            goto out;
        }
    }
    if (in->dindirect) {
        if ((rc = rd(fs, in->dindirect, t)) != 0)
            goto out;
        int changed = 0;
        for (uint64_t k = 0; k < FFS_PTRS; k++) {
            if (!t[k])
                continue;
            uint64_t base = FFS_NDIRECT + FFS_PTRS + k * FFS_PTRS;
            if (base + FFS_PTRS <= first)
                continue;
            if ((rc = rd(fs, t[k], t2)) != 0)
                goto out;
            int c2 = 0;
            for (uint64_t j = 0; j < FFS_PTRS; j++) {
                if (base + j >= first && t2[j]) {
                    free_block(fs, t2[j]);
                    t2[j] = 0;
                    c2 = 1;
                }
            }
            if (base >= first) {
                free_block(fs, t[k]);
                t[k] = 0;
                changed = 1;
            } else if (c2 && (rc = wr(fs, t[k], t2)) != 0) {
                goto out;
            }
        }
        if (first <= FFS_NDIRECT + FFS_PTRS) {
            free_block(fs, in->dindirect);
            in->dindirect = 0;
        } else if (changed && (rc = wr(fs, in->dindirect, t)) != 0) {
            goto out;
        }
    }
out:
    ffs_free(t);
    ffs_free(t2);
    return rc;
}

/* ------------------------------------------------------------ format */
int ffs_mkfs(ffs_dev *dev, const char *label, uint64_t now)
{
    uint32_t total = dev->nblocks;
    if (total < 64)
        return -FFS_EINVAL;
    uint64_t inodes = total / 4;
    if (inodes < 64)
        inodes = 64;
    if (inodes > (1u << 22))
        inodes = 1u << 22;
    inodes = (inodes + FFS_INODES_PER_BLOCK - 1) / FFS_INODES_PER_BLOCK * FFS_INODES_PER_BLOCK;

    ffs_fs fs;
    memset(&fs, 0, sizeof(fs));
    fs.dev = *dev;
    struct ffs_super *sb = &fs.sb;
    sb->magic = FFS_MAGIC;
    sb->version = FFS_VERSION;
    sb->block_size = FFS_BLOCK;
    sb->total_blocks = total;
    sb->inode_count = (uint32_t)inodes;
    sb->ibitmap_start = 1;
    sb->ibitmap_blocks = (uint32_t)((inodes + FFS_BITS_PER_BLOCK - 1) / FFS_BITS_PER_BLOCK);
    sb->bbitmap_start = sb->ibitmap_start + sb->ibitmap_blocks;
    sb->bbitmap_blocks = (total + FFS_BITS_PER_BLOCK - 1) / FFS_BITS_PER_BLOCK;
    sb->itable_start = sb->bbitmap_start + sb->bbitmap_blocks;
    sb->itable_blocks = (uint32_t)(inodes / FFS_INODES_PER_BLOCK);
    sb->data_start = sb->itable_start + sb->itable_blocks;
    if (sb->data_start + 8 > total)
        return -FFS_EINVAL;
    sb->state = FFS_STATE_CLEAN;
    sb->created = now;
    if (label) {
        size_t n = strlen(label);
        if (n > sizeof(sb->label) - 1)
            n = sizeof(sb->label) - 1;
        memcpy(sb->label, label, n);
    }

    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    int rc = 0;
    /* inode bitmap: inode 0 and 1 used, bits past inode_count used */
    for (uint32_t i = 0; i < sb->ibitmap_blocks && !rc; i++) {
        memset(b, 0, FFS_BLOCK);
        for (uint32_t bit = 0; bit < FFS_BITS_PER_BLOCK; bit++) {
            uint64_t ino = (uint64_t)i * FFS_BITS_PER_BLOCK + bit;
            if (ino <= FFS_ROOT_INO || ino >= sb->inode_count)
                b[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        rc = dev->write(dev->ctx, sb->ibitmap_start + i, b) ? -FFS_EIO : 0;
    }
    /* block bitmap: metadata, backup superblock and padding bits used */
    for (uint32_t i = 0; i < sb->bbitmap_blocks && !rc; i++) {
        memset(b, 0, FFS_BLOCK);
        for (uint32_t bit = 0; bit < FFS_BITS_PER_BLOCK; bit++) {
            uint64_t blk = (uint64_t)i * FFS_BITS_PER_BLOCK + bit;
            if (blk < sb->data_start || blk >= (uint64_t)total - 1)
                b[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        rc = dev->write(dev->ctx, sb->bbitmap_start + i, b) ? -FFS_EIO : 0;
    }
    memset(b, 0, FFS_BLOCK);
    for (uint32_t i = 0; i < sb->itable_blocks && !rc; i++) {
        if (i == 0) {
            struct ffs_inode root;
            memset(&root, 0, sizeof(root));
            root.type = FFS_T_DIR;
            root.ctime = root.mtime = now;
            memcpy(b + FFS_ROOT_INO * FFS_INODE_SIZE, &root, sizeof(root));
        }
        rc = dev->write(dev->ctx, sb->itable_start + i, b) ? -FFS_EIO : 0;
        if (i == 0)
            memset(b, 0, FFS_BLOCK);
    }
    ffs_free(b);
    if (rc)
        return rc;
    sb->free_inodes = sb->inode_count - 2;
    sb->free_blocks = total - sb->data_start - 1;
    return write_super(&fs, 1);
}

/* ------------------------------------------------------------ mount */
int ffs_mount(ffs_fs *fs, const ffs_dev *dev, int readonly, uint64_t now)
{
    memset(fs, 0, sizeof(*fs));
    fs->dev = *dev;
    uint8_t *b = blkbuf();
    if (!b)
        return -FFS_ENOMEM;
    int rc = dev->read(dev->ctx, 0, b) ? -FFS_EIO : 0;
    if (!rc) {
        memcpy(&fs->sb, b, sizeof(fs->sb));
        if (!ffs_super_valid(&fs->sb, dev->nblocks))
            rc = -FFS_EINVAL;
    }
    ffs_free(b);
    if (rc)
        return rc;
    fs->readonly = readonly;
    fs->hint_blk = fs->sb.data_start;
    fs->hint_ino = 2;
    if (!readonly) {
        fs->sb.state = FFS_STATE_DIRTY;
        fs->sb.mount_count++;
        fs->sb.last_mount = now;
        rc = write_super(fs, 0);
    }
    return rc;
}

int ffs_sync(ffs_fs *fs)
{
    if (fs->readonly)
        return 0;
    return write_super(fs, 0);
}

int ffs_unmount(ffs_fs *fs)
{
    if (fs->readonly)
        return 0;
    fs->sb.state = FFS_STATE_CLEAN;
    int rc = write_super(fs, 0);
    fs->readonly = 1;
    return rc;
}

/* ------------------------------------------------------------ directories */
static int dir_find(ffs_fs *fs, struct ffs_inode *dir, const char *name, uint32_t *ino, uint64_t *slot_out)
{
    size_t nlen = strlen(name);
    struct ffs_dirent *ents = blkbuf();
    if (!ents)
        return -FFS_ENOMEM;
    int rc = -FFS_ENOENT;
    uint64_t nblk = dir->size / FFS_BLOCK;
    for (uint64_t lb = 0; lb < nblk; lb++) {
        uint32_t pb;
        int r = bmap(fs, dir, lb, 0, &pb, NULL);
        if (r) {
            rc = r;
            break;
        }
        if (!pb)
            continue;
        if ((r = rd(fs, pb, ents)) != 0) {
            rc = r;
            break;
        }
        for (int i = 0; i < FFS_DIRENTS_PER_BLOCK; i++) {
            struct ffs_dirent *e = &ents[i];
            if (e->ino && e->name_len == nlen && !memcmp(e->name, name, nlen)) {
                *ino = e->ino;
                if (slot_out)
                    *slot_out = lb * FFS_DIRENTS_PER_BLOCK + (uint64_t)i;
                rc = 0;
                goto out;
            }
        }
    }
out:
    ffs_free(ents);
    return rc;
}

static int dir_set_slot(ffs_fs *fs, struct ffs_inode *dir, uint64_t slot, const struct ffs_dirent *e)
{
    uint32_t pb;
    int rc = bmap(fs, dir, slot / FFS_DIRENTS_PER_BLOCK, 0, &pb, NULL);
    if (rc)
        return rc;
    if (!pb)
        return -FFS_EIO;
    struct ffs_dirent *ents = blkbuf();
    if (!ents)
        return -FFS_ENOMEM;
    rc = rd(fs, pb, ents);
    if (!rc) {
        ents[slot % FFS_DIRENTS_PER_BLOCK] = *e;
        rc = wr(fs, pb, ents);
    }
    ffs_free(ents);
    return rc;
}

static int dir_add(ffs_fs *fs, uint32_t dino, struct ffs_inode *dir, const char *name, uint32_t ino, int type,
                   uint64_t now)
{
    struct ffs_dirent ne;
    memset(&ne, 0, sizeof(ne));
    ne.ino = ino;
    ne.type = (uint8_t)type;
    ne.name_len = (uint8_t)strlen(name);
    memcpy(ne.name, name, ne.name_len);

    struct ffs_dirent *ents = blkbuf();
    if (!ents)
        return -FFS_ENOMEM;
    int rc = 0;
    uint64_t nblk = dir->size / FFS_BLOCK;
    for (uint64_t lb = 0; lb < nblk; lb++) {
        uint32_t pb;
        if ((rc = bmap(fs, dir, lb, 0, &pb, NULL)) != 0)
            goto out;
        if (!pb)
            continue;
        if ((rc = rd(fs, pb, ents)) != 0)
            goto out;
        for (int i = 0; i < FFS_DIRENTS_PER_BLOCK; i++) {
            if (!ents[i].ino) {
                ents[i] = ne;
                rc = wr(fs, pb, ents);
                if (!rc) {
                    dir->mtime = now;
                    rc = write_inode(fs, dino, dir);
                }
                goto out;
            }
        }
    }
    /* append a new block */
    {
        uint32_t pb;
        int fresh;
        struct ffs_inode saved = *dir;
        if ((rc = bmap(fs, dir, nblk, 1, &pb, &fresh)) != 0)
            goto out;
        memset(ents, 0, FFS_BLOCK);
        ents[0] = ne;
        if ((rc = wr(fs, pb, ents)) != 0) {
            *dir = saved;
            goto out;
        }
        dir->size += FFS_BLOCK;
        dir->mtime = now;
        rc = write_inode(fs, dino, dir);
    }
out:
    ffs_free(ents);
    return rc;
}

static int dir_empty(ffs_fs *fs, struct ffs_inode *dir)
{
    struct ffs_dirent *ents = blkbuf();
    if (!ents)
        return -FFS_ENOMEM;
    int rc = 1;
    for (uint64_t lb = 0; lb < dir->size / FFS_BLOCK && rc == 1; lb++) {
        uint32_t pb;
        int r = bmap(fs, dir, lb, 0, &pb, NULL);
        if (r) {
            rc = r;
            break;
        }
        if (!pb)
            continue;
        if ((r = rd(fs, pb, ents)) != 0) {
            rc = r;
            break;
        }
        for (int i = 0; i < FFS_DIRENTS_PER_BLOCK; i++)
            if (ents[i].ino) {
                rc = 0;
                break;
            }
    }
    ffs_free(ents);
    return rc;
}

static int load_dir(ffs_fs *fs, uint32_t dir, struct ffs_inode *in)
{
    int rc = read_inode(fs, dir, in);
    if (rc)
        return rc;
    if (in->type != FFS_T_DIR)
        return -FFS_ENOTDIR;
    return 0;
}

int ffs_lookup(ffs_fs *fs, uint32_t dir, const char *name, uint32_t *ino)
{
    struct ffs_inode d;
    int rc = load_dir(fs, dir, &d);
    if (rc)
        return rc;
    if (strlen(name) > FFS_NAME_MAX)
        return -FFS_ENAMETOOLONG;
    return dir_find(fs, &d, name, ino, NULL);
}

int ffs_stat(ffs_fs *fs, uint32_t ino, struct ffs_stat *st)
{
    struct ffs_inode in;
    int rc = read_inode(fs, ino, &in);
    if (rc)
        return rc;
    if (in.type != FFS_T_FILE && in.type != FFS_T_DIR)
        return -FFS_ENOENT;
    st->ino = ino;
    st->type = in.type;
    st->size = in.size;
    st->ctime = in.ctime;
    st->mtime = in.mtime;
    st->blocks = (in.size + FFS_BLOCK - 1) / FFS_BLOCK;
    return 0;
}

int ffs_readdir(ffs_fs *fs, uint32_t dir, uint64_t *cursor, struct ffs_entry *out)
{
    struct ffs_inode d;
    int rc = load_dir(fs, dir, &d);
    if (rc)
        return rc;
    struct ffs_dirent *ents = blkbuf();
    if (!ents)
        return -FFS_ENOMEM;
    uint64_t nslots = d.size / FFS_DIRENT_SIZE;
    rc = 0;
    uint64_t loaded = (uint64_t)-1;
    for (uint64_t s = *cursor; s < nslots; s++) {
        uint64_t lb = s / FFS_DIRENTS_PER_BLOCK;
        if (lb != loaded) {
            uint32_t pb;
            if ((rc = bmap(fs, &d, lb, 0, &pb, NULL)) != 0)
                break;
            if (!pb) {
                s = (lb + 1) * FFS_DIRENTS_PER_BLOCK - 1;
                continue;
            }
            if ((rc = rd(fs, pb, ents)) != 0)
                break;
            loaded = lb;
        }
        struct ffs_dirent *e = &ents[s % FFS_DIRENTS_PER_BLOCK];
        if (!e->ino)
            continue;
        out->ino = e->ino;
        out->type = e->type;
        uint8_t n = e->name_len > FFS_NAME_MAX ? FFS_NAME_MAX : e->name_len;
        memcpy(out->name, e->name, n);
        out->name[n] = 0;
        *cursor = s + 1;
        rc = 1;
        break;
    }
    if (rc == 0)
        *cursor = nslots;
    ffs_free(ents);
    return rc;
}

/* ------------------------------------------------------------ files */
int64_t ffs_read(ffs_fs *fs, uint32_t ino, uint64_t off, void *buf, uint64_t n)
{
    struct ffs_inode in;
    int rc = read_inode(fs, ino, &in);
    if (rc)
        return rc;
    if (in.type == FFS_T_DIR)
        return -FFS_EISDIR;
    if (in.type != FFS_T_FILE)
        return -FFS_ENOENT;
    if (off >= in.size)
        return 0;
    if (n > in.size - off)
        n = in.size - off;
    uint8_t *blk = blkbuf();
    if (!blk)
        return -FFS_ENOMEM;
    uint8_t *out = buf;
    uint64_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t lb = pos / FFS_BLOCK, bo = pos % FFS_BLOCK;
        uint64_t chunk = FFS_BLOCK - bo;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t pb;
        if ((rc = bmap(fs, &in, lb, 0, &pb, NULL)) != 0)
            break;
        if (!pb) {
            memset(out + done, 0, chunk);
        } else {
            if ((rc = rd(fs, pb, blk)) != 0)
                break;
            memcpy(out + done, blk + bo, chunk);
        }
        done += chunk;
    }
    ffs_free(blk);
    if (rc && done == 0)
        return rc;
    return (int64_t)done;
}

int64_t ffs_write(ffs_fs *fs, uint32_t ino, uint64_t off, const void *buf, uint64_t n, uint64_t now)
{
    if (fs->readonly)
        return -FFS_EROFS;
    struct ffs_inode in;
    int rc = read_inode(fs, ino, &in);
    if (rc)
        return rc;
    if (in.type == FFS_T_DIR)
        return -FFS_EISDIR;
    if (in.type != FFS_T_FILE)
        return -FFS_ENOENT;
    if (n == 0)
        return 0;
    if (off + n < off || (off + n + FFS_BLOCK - 1) / FFS_BLOCK > FFS_MAX_FILE_BLOCKS)
        return -FFS_EFBIG;
    uint8_t *blk = blkbuf();
    if (!blk)
        return -FFS_ENOMEM;
    const uint8_t *src = buf;
    uint64_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t lb = pos / FFS_BLOCK, bo = pos % FFS_BLOCK;
        uint64_t chunk = FFS_BLOCK - bo;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t pb;
        int fresh;
        if ((rc = bmap(fs, &in, lb, 1, &pb, &fresh)) != 0)
            break;
        if (chunk < FFS_BLOCK) {
            if (fresh)
                memset(blk, 0, FFS_BLOCK);
            else if ((rc = rd(fs, pb, blk)) != 0)
                break;
        }
        memcpy(blk + bo, src + done, chunk);
        if ((rc = wr(fs, pb, blk)) != 0)
            break;
        done += chunk;
    }
    ffs_free(blk);
    if (off + done > in.size)
        in.size = off + done;
    if (rc) /* release blocks allocated past the new end of file */
        free_from(fs, &in, (in.size + FFS_BLOCK - 1) / FFS_BLOCK);
    in.mtime = now;
    int wrc = write_inode(fs, ino, &in);
    if (done == 0)
        return rc ? rc : wrc;
    if (wrc)
        return wrc;
    return (int64_t)done;
}

int ffs_truncate(ffs_fs *fs, uint32_t ino, uint64_t size, uint64_t now)
{
    if (fs->readonly)
        return -FFS_EROFS;
    struct ffs_inode in;
    int rc = read_inode(fs, ino, &in);
    if (rc)
        return rc;
    if (in.type == FFS_T_DIR)
        return -FFS_EISDIR;
    if (in.type != FFS_T_FILE)
        return -FFS_ENOENT;
    if ((size + FFS_BLOCK - 1) / FFS_BLOCK > FFS_MAX_FILE_BLOCKS)
        return -FFS_EFBIG;
    if (size < in.size) {
        uint64_t keep = (size + FFS_BLOCK - 1) / FFS_BLOCK;
        if ((rc = free_from(fs, &in, keep)) != 0)
            return rc;
        if (size % FFS_BLOCK) { /* clear the tail of the last block */
            uint32_t pb;
            if ((rc = bmap(fs, &in, size / FFS_BLOCK, 0, &pb, NULL)) != 0)
                return rc;
            if (pb) {
                uint8_t *blk = blkbuf();
                if (!blk)
                    return -FFS_ENOMEM;
                rc = rd(fs, pb, blk);
                if (!rc) {
                    memset(blk + size % FFS_BLOCK, 0, FFS_BLOCK - size % FFS_BLOCK);
                    rc = wr(fs, pb, blk);
                }
                ffs_free(blk);
                if (rc)
                    return rc;
            }
        }
    }
    in.size = size;
    in.mtime = now;
    return write_inode(fs, ino, &in);
}

int ffs_create(ffs_fs *fs, uint32_t dir, const char *name, int type, uint64_t now, uint32_t *ino_out)
{
    if (fs->readonly)
        return -FFS_EROFS;
    if (type != FFS_T_FILE && type != FFS_T_DIR)
        return -FFS_EINVAL;
    int rc = ffs_name_valid(name);
    if (rc)
        return rc;
    struct ffs_inode d;
    if ((rc = load_dir(fs, dir, &d)) != 0)
        return rc;
    uint32_t existing;
    rc = dir_find(fs, &d, name, &existing, NULL);
    if (rc == 0)
        return -FFS_EEXIST;
    if (rc != -FFS_ENOENT)
        return rc;
    uint32_t ino;
    if ((rc = alloc_inode(fs, &ino)) != 0)
        return rc;
    struct ffs_inode in;
    memset(&in, 0, sizeof(in));
    in.type = (uint16_t)type;
    in.ctime = in.mtime = now;
    if ((rc = write_inode(fs, ino, &in)) != 0 || (rc = dir_add(fs, dir, &d, name, ino, type, now)) != 0) {
        memset(&in, 0, sizeof(in));
        write_inode(fs, ino, &in);
        free_inode(fs, ino);
        return rc;
    }
    if (ino_out)
        *ino_out = ino;
    return 0;
}

int ffs_unlink(ffs_fs *fs, uint32_t dir, const char *name)
{
    if (fs->readonly)
        return -FFS_EROFS;
    struct ffs_inode d;
    int rc = load_dir(fs, dir, &d);
    if (rc)
        return rc;
    uint32_t ino;
    uint64_t slot;
    if ((rc = dir_find(fs, &d, name, &ino, &slot)) != 0)
        return rc;
    struct ffs_inode in;
    if ((rc = read_inode(fs, ino, &in)) != 0)
        return rc;
    if (in.type == FFS_T_DIR) {
        int e = dir_empty(fs, &in);
        if (e < 0)
            return e;
        if (!e)
            return -FFS_ENOTEMPTY;
    }
    struct ffs_dirent empty;
    memset(&empty, 0, sizeof(empty));
    if ((rc = dir_set_slot(fs, &d, slot, &empty)) != 0)
        return rc;
    free_from(fs, &in, 0);
    memset(&in, 0, sizeof(in));
    if ((rc = write_inode(fs, ino, &in)) != 0)
        return rc;
    return free_inode(fs, ino);
}

int ffs_rename(ffs_fs *fs, uint32_t odir, const char *oname, uint32_t ndir, const char *nname, uint64_t now)
{
    if (fs->readonly)
        return -FFS_EROFS;
    int rc = ffs_name_valid(nname);
    if (rc)
        return rc;
    struct ffs_inode od, nd;
    if ((rc = load_dir(fs, odir, &od)) != 0 || (rc = load_dir(fs, ndir, &nd)) != 0)
        return rc;
    uint32_t ino, target;
    uint64_t oslot;
    if ((rc = dir_find(fs, &od, oname, &ino, &oslot)) != 0)
        return rc;
    struct ffs_inode src;
    if ((rc = read_inode(fs, ino, &src)) != 0)
        return rc;
    rc = dir_find(fs, &nd, nname, &target, NULL);
    if (rc == 0) {
        if (target == ino)
            return 0;
        struct ffs_inode t;
        if ((rc = read_inode(fs, target, &t)) != 0)
            return rc;
        if (t.type == FFS_T_DIR || src.type == FFS_T_DIR)
            return -FFS_EEXIST;
        if ((rc = ffs_unlink(fs, ndir, nname)) != 0)
            return rc;
        if ((rc = load_dir(fs, ndir, &nd)) != 0)
            return rc;
    } else if (rc != -FFS_ENOENT) {
        return rc;
    }
    if ((rc = dir_add(fs, ndir, &nd, nname, ino, src.type, now)) != 0)
        return rc;
    if (odir == ndir) /* dir_add may have changed the directory inode */
        od = nd;
    struct ffs_dirent empty;
    memset(&empty, 0, sizeof(empty));
    rc = dir_set_slot(fs, &od, oslot, &empty);
    if (!rc && odir != ndir) {
        od.mtime = now;
        rc = write_inode(fs, odir, &od);
    }
    return rc;
}

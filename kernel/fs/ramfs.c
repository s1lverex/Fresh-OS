/* Fresh OS kernel - ramfs: in-memory file system used for the root (/),
 * populated from the initrd at boot. */
#include "vfs.h"
#include "../mm/mm.h"

#define RAMFS_MAX_FILE (256ULL << 20)
#define END_CURSOR ((uint64_t)-1)

struct rnode {
    bool used;
    uint8_t type;
    char name[NAME_MAX + 1];
    uint64_t parent, first_child, last_child, next;
    uint8_t *data;
    uint64_t size, cap;
    uint64_t ctime, mtime;
};

struct ramfs {
    struct rnode *n;
    uint64_t count;
    uint64_t hint;
};

static struct rnode *node(struct ramfs *r, uint64_t ino)
{
    if (ino == 0 || ino >= r->count || !r->n[ino].used)
        return NULL;
    return &r->n[ino];
}

static int64_t node_alloc(struct ramfs *r)
{
    for (uint64_t i = r->hint; i < r->count; i++) {
        if (!r->n[i].used) {
            r->hint = i + 1;
            memset(&r->n[i], 0, sizeof(struct rnode));
            r->n[i].used = true;
            return (int64_t)i;
        }
    }
    uint64_t ncount = r->count ? r->count * 2 : 64;
    struct rnode *nn = krealloc(r->n, ncount * sizeof(struct rnode));
    if (!nn)
        return -ENOMEM;
    memset(nn + r->count, 0, (ncount - r->count) * sizeof(struct rnode));
    uint64_t idx = r->count;
    r->n = nn;
    r->count = ncount;
    r->n[idx].used = true;
    r->hint = idx + 1;
    return (int64_t)idx;
}

void *ramfs_new(uint64_t *root)
{
    struct ramfs *r = kzalloc(sizeof(*r));
    if (!r)
        return NULL;
    r->hint = 1;
    node_alloc(r);              /* index 0: unused sentinel */
    r->n[0].used = false;
    r->hint = 1;
    int64_t rt = node_alloc(r); /* root = 1 */
    r->n[rt].type = FT_DIR;
    r->n[rt].ctime = r->n[rt].mtime = rtc_unix_time();
    *root = (uint64_t)rt;
    return r;
}

static int name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || !strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;
    if (n > NAME_MAX)
        return -ENAMETOOLONG;
    return strchr(name, '/') ? -EINVAL : 0;
}

static uint64_t find_child(struct ramfs *r, uint64_t dir, const char *name)
{
    for (uint64_t c = r->n[dir].first_child; c; c = r->n[c].next)
        if (!strcmp(r->n[c].name, name))
            return c;
    return 0;
}

static void link_child(struct ramfs *r, uint64_t dir, uint64_t c)
{
    r->n[c].parent = dir;
    r->n[c].next = 0;
    if (r->n[dir].last_child)
        r->n[r->n[dir].last_child].next = c;
    else
        r->n[dir].first_child = c;
    r->n[dir].last_child = c;
}

static void unlink_child(struct ramfs *r, uint64_t dir, uint64_t c)
{
    uint64_t prev = 0;
    for (uint64_t x = r->n[dir].first_child; x; prev = x, x = r->n[x].next) {
        if (x != c)
            continue;
        if (prev)
            r->n[prev].next = r->n[x].next;
        else
            r->n[dir].first_child = r->n[x].next;
        if (r->n[dir].last_child == c)
            r->n[dir].last_child = prev;
        r->n[c].next = 0;
        return;
    }
}

static int r_lookup(struct mount *m, uint64_t dir, const char *name, uint64_t *ino)
{
    struct ramfs *r = m->priv;
    struct rnode *d = node(r, dir);
    if (!d)
        return -ENOENT;
    if (d->type != FT_DIR)
        return -ENOTDIR;
    uint64_t c = find_child(r, dir, name);
    if (!c)
        return -ENOENT;
    *ino = c;
    return 0;
}

static int r_stat(struct mount *m, uint64_t ino, struct fresh_stat *st)
{
    struct rnode *x = node(m->priv, ino);
    if (!x)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->type = x->type;
    st->size = x->type == FT_DIR ? 0 : x->size;
    st->ino = ino;
    st->mtime = x->mtime;
    st->ctime = x->ctime;
    return 0;
}

static long r_read(struct mount *m, uint64_t ino, uint64_t off, void *buf, size_t n)
{
    struct rnode *x = node(m->priv, ino);
    if (!x)
        return -ENOENT;
    if (x->type == FT_DIR)
        return -EISDIR;
    if (off >= x->size)
        return 0;
    size_t k = (size_t)MIN((uint64_t)n, x->size - off);
    memcpy(buf, x->data + off, k);
    return (long)k;
}

static int grow(struct rnode *x, uint64_t need)
{
    if (need > RAMFS_MAX_FILE)
        return -EFBIG;
    if (need <= x->cap)
        return 0;
    uint64_t cap = x->cap ? x->cap : 64;
    while (cap < need)
        cap *= 2;
    uint8_t *d = krealloc(x->data, cap);
    if (!d)
        return -ENOSPC;
    x->data = d;
    x->cap = cap;
    return 0;
}

static long r_write(struct mount *m, uint64_t ino, uint64_t off, const void *buf, size_t n)
{
    struct rnode *x = node(m->priv, ino);
    if (!x)
        return -ENOENT;
    if (x->type == FT_DIR)
        return -EISDIR;
    if (n == 0)
        return 0;
    if (off + n < off)
        return -EFBIG;
    int rc = grow(x, off + n);
    if (rc)
        return rc;
    if (off > x->size)
        memset(x->data + x->size, 0, off - x->size);
    memcpy(x->data + off, buf, n);
    if (off + n > x->size)
        x->size = off + n;
    x->mtime = rtc_unix_time();
    return (long)n;
}

static int r_truncate(struct mount *m, uint64_t ino, uint64_t size)
{
    struct rnode *x = node(m->priv, ino);
    if (!x)
        return -ENOENT;
    if (x->type == FT_DIR)
        return -EISDIR;
    if (size > x->size) {
        int rc = grow(x, size);
        if (rc)
            return rc;
        memset(x->data + x->size, 0, size - x->size);
    }
    x->size = size;
    x->mtime = rtc_unix_time();
    return 0;
}

static int r_create(struct mount *m, uint64_t dir, const char *name, int type, uint64_t *ino)
{
    struct ramfs *r = m->priv;
    int rc = name_ok(name);
    if (rc)
        return rc;
    struct rnode *d = node(r, dir);
    if (!d)
        return -ENOENT;
    if (d->type != FT_DIR)
        return -ENOTDIR;
    if (find_child(r, dir, name))
        return -EEXIST;
    int64_t c = node_alloc(r); /* may move the node array */
    if (c < 0)
        return (int)c;
    struct rnode *x = &r->n[c];
    x->type = (uint8_t)type;
    strlcpy(x->name, name, sizeof(x->name));
    x->ctime = x->mtime = rtc_unix_time();
    link_child(r, dir, (uint64_t)c);
    if (ino)
        *ino = (uint64_t)c;
    return 0;
}

static int r_unlink(struct mount *m, uint64_t dir, const char *name)
{
    struct ramfs *r = m->priv;
    if (!node(r, dir))
        return -ENOENT;
    uint64_t c = find_child(r, dir, name);
    if (!c)
        return -ENOENT;
    struct rnode *x = &r->n[c];
    if (x->type == FT_DIR && x->first_child)
        return -ENOTEMPTY;
    unlink_child(r, dir, c);
    kfree(x->data);
    memset(x, 0, sizeof(*x));
    if (c < r->hint)
        r->hint = c;
    return 0;
}

static int r_rename(struct mount *m, uint64_t odir, const char *oname, uint64_t ndir, const char *nname)
{
    struct ramfs *r = m->priv;
    int rc = name_ok(nname);
    if (rc)
        return rc;
    struct rnode *nd = node(r, ndir);
    if (!node(r, odir) || !nd)
        return -ENOENT;
    if (nd->type != FT_DIR)
        return -ENOTDIR;
    uint64_t src = find_child(r, odir, oname);
    if (!src)
        return -ENOENT;
    uint64_t dst = find_child(r, ndir, nname);
    if (dst == src)
        return 0;
    if (dst) {
        if (r->n[dst].type == FT_DIR || r->n[src].type == FT_DIR)
            return -EEXIST;
        if ((rc = r_unlink(m, ndir, nname)) != 0)
            return rc;
    }
    unlink_child(r, odir, src);
    strlcpy(r->n[src].name, nname, sizeof(r->n[src].name));
    link_child(r, ndir, src);
    return 0;
}

static int r_readdir(struct mount *m, uint64_t dir, uint64_t *cursor, struct fresh_dirent *out)
{
    struct ramfs *r = m->priv;
    struct rnode *d = node(r, dir);
    if (!d)
        return -ENOENT;
    if (d->type != FT_DIR)
        return -ENOTDIR;
    if (*cursor == END_CURSOR)
        return 0;
    uint64_t c = *cursor ? *cursor : d->first_child;
    if (*cursor && (!node(r, c) || r->n[c].parent != dir)) {
        *cursor = END_CURSOR;
        return 0;
    }
    if (!c) {
        *cursor = END_CURSOR;
        return 0;
    }
    out->ino = c;
    out->type = r->n[c].type;
    strlcpy(out->name, r->n[c].name, sizeof(out->name));
    *cursor = r->n[c].next ? r->n[c].next : END_CURSOR;
    return 1;
}

static int r_statfs(struct mount *m, struct fresh_statfs *st)
{
    struct ramfs *r = m->priv;
    memset(st, 0, sizeof(*st));
    st->block_size = PAGE_SIZE;
    st->total_blocks = pmm_total_pages();
    st->free_blocks = pmm_free_pages();
    uint64_t used = 0;
    for (uint64_t i = 1; i < r->count; i++)
        if (r->n[i].used)
            used++;
    st->total_inodes = r->count;
    st->free_inodes = r->count - used;
    strlcpy(st->fs_name, "ramfs", sizeof(st->fs_name));
    return 0;
}

static const struct fs_ops ops = {
    .lookup = r_lookup,
    .stat = r_stat,
    .read = r_read,
    .write = r_write,
    .truncate = r_truncate,
    .create = r_create,
    .unlink = r_unlink,
    .rename = r_rename,
    .readdir = r_readdir,
    .statfs = r_statfs,
    .sync = NULL,
    .unmount = NULL,
};

const struct fs_ops *ramfs_ops(void) { return &ops; }

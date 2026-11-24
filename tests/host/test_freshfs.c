/* FreshFS model-based random test (host, ASan/UBSan).
 * Applies thousands of random operations to a RAM-backed file system and to
 * a simple in-memory model, checking after each step that both agree and
 * periodically that fsck finds no errors and a remount sees the same tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freshfs/freshfs.h"

void *ffs_malloc(size_t n) { return malloc(n); }
void ffs_free(void *p) { free(p); }

static uint8_t *disk;
static uint32_t disk_blocks;
static int fail_writes_after = -1; /* simulate I/O errors */

static int ram_read(void *ctx, uint32_t b, void *buf)
{
    (void)ctx;
    if (b >= disk_blocks)
        return -1;
    memcpy(buf, disk + (size_t)b * FFS_BLOCK, FFS_BLOCK);
    return 0;
}

static int ram_write(void *ctx, uint32_t b, const void *buf)
{
    (void)ctx;
    if (b >= disk_blocks)
        return -1;
    if (fail_writes_after == 0)
        return -1;
    if (fail_writes_after > 0)
        fail_writes_after--;
    memcpy(disk + (size_t)b * FFS_BLOCK, buf, FFS_BLOCK);
    return 0;
}

static int failures;
#define CHECK(c, ...)                                                                                        \
    do {                                                                                                    \
        if (!(c)) {                                                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                     \
            printf(__VA_ARGS__);                                                                            \
            printf("\n");                                                                                   \
            failures++;                                                                                     \
            if (failures > 20)                                                                              \
                exit(1);                                                                                    \
        }                                                                                                   \
    } while (0)

/* ------------------------------------------------------------ model */
#define MAXN 300
struct node {
    int used, is_dir, parent;
    char name[FFS_NAME_MAX + 1];
    uint8_t *data;
    size_t size;
    uint32_t ino;
};
static struct node model[MAXN];

static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int pick(int want_dir)
{
    int cand[MAXN], n = 0;
    for (int i = 0; i < MAXN; i++)
        if (model[i].used && (want_dir < 0 || model[i].is_dir == want_dir))
            cand[n++] = i;
    return n ? cand[rnd() % (uint64_t)n] : -1;
}

static int find_child(int dir, const char *name)
{
    for (int i = 0; i < MAXN; i++)
        if (model[i].used && model[i].parent == dir && i != dir && !strcmp(model[i].name, name))
            return i;
    return -1;
}

static int has_children(int dir)
{
    for (int i = 1; i < MAXN; i++)
        if (model[i].used && model[i].parent == dir && i != dir)
            return 1;
    return 0;
}

static int is_ancestor(int a, int b) /* a is ancestor of b (or equal) */
{
    while (1) {
        if (a == b)
            return 1;
        if (b == 0)
            return 0;
        b = model[b].parent;
    }
}

static void verify_all(ffs_fs *fs, const char *when)
{
    uint8_t *buf = malloc(1 << 20);
    for (int i = 0; i < MAXN; i++) {
        if (!model[i].used)
            continue;
        struct ffs_stat st;
        int rc = ffs_stat(fs, model[i].ino, &st);
        CHECK(rc == 0, "%s: stat node %d (%s)", when, i, model[i].name);
        if (rc)
            continue;
        CHECK((int)st.type == (model[i].is_dir ? FFS_T_DIR : FFS_T_FILE), "%s: type of %s", when, model[i].name);
        if (i) {
            uint32_t ino;
            CHECK(ffs_lookup(fs, model[model[i].parent].ino, model[i].name, &ino) == 0 && ino == model[i].ino,
                  "%s: lookup %s", when, model[i].name);
        }
        if (!model[i].is_dir) {
            CHECK(st.size == model[i].size, "%s: size of %s %llu != %zu", when, model[i].name,
                  (unsigned long long)st.size, model[i].size);
            size_t n = model[i].size < (1 << 20) ? model[i].size : (1 << 20);
            int64_t r = ffs_read(fs, model[i].ino, 0, buf, n);
            CHECK(r == (int64_t)n && (n == 0 || !memcmp(buf, model[i].data, n)), "%s: content of %s", when,
                  model[i].name);
        } else {
            int count = 0, expect = 0;
            uint64_t cur = 0;
            struct ffs_entry e;
            while (ffs_readdir(fs, model[i].ino, &cur, &e) == 1)
                count++;
            for (int k = 1; k < MAXN; k++)
                if (model[k].used && model[k].parent == i && k != i)
                    expect++;
            CHECK(count == expect, "%s: dir %s has %d entries, model %d", when, model[i].name, count, expect);
        }
    }
    free(buf);
}

static void fsck_clean(const char *when)
{
    ffs_dev d = {NULL, ram_read, ram_write, disk_blocks};
    struct ffs_fsck_report r;
    int rc = ffs_fsck(&d, 0, &r, NULL);
    CHECK(rc == 0 && r.errors == 0, "%s: fsck rc=%d errors=%u", when, rc, r.errors);
}

static void run(uint32_t blocks, int ops, uint64_t seed)
{
    disk_blocks = blocks;
    disk = calloc(blocks, FFS_BLOCK);
    rs = seed;
    memset(model, 0, sizeof(model));
    ffs_dev d = {NULL, ram_read, ram_write, blocks};
    CHECK(ffs_mkfs(&d, "test", 1) == 0, "mkfs");
    ffs_fs fs;
    CHECK(ffs_mount(&fs, &d, 0, 1) == 0, "mount");
    model[0].used = model[0].is_dir = 1;
    model[0].ino = FFS_ROOT_INO;
    int enospc = 0;
    for (int op = 0; op < ops; op++) {
        int kind = (int)(rnd() % 10);
        char name[FFS_NAME_MAX + 1];
        snprintf(name, sizeof(name), "n%llu%s", (unsigned long long)(rnd() % 40),
                 rnd() % 7 == 0 ? "_with_a_much_longer_name_to_fill_dir_slots" : "");
        if (kind <= 2) { /* create */
            int dir = pick(1), slot = -1;
            for (int i = 1; i < MAXN; i++)
                if (!model[i].used) {
                    slot = i;
                    break;
                }
            if (slot < 0)
                continue;
            int isdir = rnd() % 4 == 0;
            uint32_t ino;
            int rc = ffs_create(&fs, model[dir].ino, name, isdir ? FFS_T_DIR : FFS_T_FILE, (uint64_t)op, &ino);
            int exists = find_child(dir, name) >= 0;
            if (exists) {
                CHECK(rc == -FFS_EEXIST, "create existing returned %d", rc);
            } else if (rc == -FFS_ENOSPC) {
                enospc++;
            } else {
                CHECK(rc == 0, "create %s rc=%d", name, rc);
                if (!rc) {
                    struct node *n = &model[slot];
                    memset(n, 0, sizeof(*n));
                    n->used = 1;
                    n->is_dir = isdir;
                    n->parent = dir;
                    strcpy(n->name, name);
                    n->ino = ino;
                }
            }
        } else if (kind <= 5) { /* write */
            int f = pick(0);
            if (f < 0)
                continue;
            struct node *n = &model[f];
            size_t off = (size_t)(rnd() % 3 == 0 ? rnd() % (n->size + 1) : rnd() % 70000);
            if (rnd() % 20 == 0)
                off = (size_t)(rnd() % 5000000); /* reach double indirect */
            size_t len = (size_t)(rnd() % 3 == 0 ? rnd() % 20000 : rnd() % 600);
            uint8_t *src = malloc(len + 1);
            for (size_t k = 0; k < len; k++)
                src[k] = (uint8_t)rnd();
            int64_t w = ffs_write(&fs, n->ino, off, src, len, (uint64_t)op);
            if (w == -FFS_ENOSPC || (w >= 0 && (size_t)w < len)) {
                enospc++;
                len = w > 0 ? (size_t)w : 0;
            } else {
                CHECK(w == (int64_t)len, "write %s returned %lld", n->name, (long long)w);
            }
            if (len) {
                if (off + len > n->size) {
                    n->data = realloc(n->data, off + len);
                    memset(n->data + n->size, 0, off + len - n->size);
                    n->size = off + len;
                }
                memcpy(n->data + off, src, len);
            }
            free(src);
        } else if (kind == 6) { /* truncate */
            int f = pick(0);
            if (f < 0)
                continue;
            struct node *n = &model[f];
            size_t ns = (size_t)(rnd() % (n->size + 10000));
            int rc = ffs_truncate(&fs, n->ino, ns, (uint64_t)op);
            CHECK(rc == 0, "truncate rc=%d", rc);
            if (!rc) {
                if (ns > n->size) {
                    n->data = realloc(n->data, ns);
                    memset(n->data + n->size, 0, ns - n->size);
                }
                n->size = ns;
            }
        } else if (kind <= 8) { /* unlink */
            int f = pick(-1);
            if (f <= 0)
                continue;
            struct node *n = &model[f];
            int rc = ffs_unlink(&fs, model[n->parent].ino, n->name);
            if (n->is_dir && has_children(f)) {
                CHECK(rc == -FFS_ENOTEMPTY, "rmdir non-empty rc=%d", rc);
            } else {
                CHECK(rc == 0, "unlink %s rc=%d", n->name, rc);
                if (!rc) {
                    free(n->data);
                    memset(n, 0, sizeof(*n));
                }
            }
        } else { /* rename */
            int f = pick(-1), dir = pick(1);
            if (f <= 0 || dir < 0)
                continue;
            struct node *n = &model[f];
            if (n->is_dir && is_ancestor(f, dir))
                continue; /* the VFS forbids this */
            int target = find_child(dir, name);
            int rc = ffs_rename(&fs, model[n->parent].ino, n->name, model[dir].ino, name, (uint64_t)op);
            if (target == f) {
                CHECK(rc == 0, "rename onto itself rc=%d", rc);
            } else if (target >= 0 && (model[target].is_dir || n->is_dir)) {
                CHECK(rc == -FFS_EEXIST, "rename over dir rc=%d", rc);
            } else if (rc == -FFS_ENOSPC) {
                enospc++;
            } else {
                CHECK(rc == 0, "rename rc=%d", rc);
                if (!rc) {
                    if (target >= 0) {
                        free(model[target].data);
                        memset(&model[target], 0, sizeof(model[target]));
                    }
                    n->parent = dir;
                    strcpy(n->name, name);
                }
            }
        }
        if (op % 500 == 499) {
            verify_all(&fs, "periodic");
            ffs_unmount(&fs);
            fsck_clean("periodic");
            CHECK(ffs_mount(&fs, &d, 0, (uint64_t)op) == 0, "remount");
            verify_all(&fs, "after remount");
        }
    }
    verify_all(&fs, "final");
    /* free counts tracked in memory must match a full recount */
    uint32_t fb = fs.sb.free_blocks, fi = fs.sb.free_inodes;
    ffs_unmount(&fs);
    ffs_dev d2 = {NULL, ram_read, ram_write, blocks};
    struct ffs_fsck_report r;
    CHECK(ffs_fsck(&d2, 0, &r, NULL) == 0, "final fsck");
    CHECK(r.free_blocks == fb && r.free_inodes == fi, "free counts %u/%u vs recount %u/%u", fb, fi, r.free_blocks,
          r.free_inodes);
    printf("  run blocks=%u ops=%d seed=%llu: ok (%d ENOSPC hits, %u files, %u dirs)\n", blocks, ops,
           (unsigned long long)seed, enospc, r.files, r.dirs);
    for (int i = 0; i < MAXN; i++)
        free(model[i].data);
    free(disk);
}

static void test_mount_rejects_garbage(void)
{
    disk_blocks = 256;
    disk = calloc(disk_blocks, FFS_BLOCK);
    ffs_dev d = {NULL, ram_read, ram_write, disk_blocks};
    ffs_fs fs;
    CHECK(ffs_mount(&fs, &d, 0, 0) == -FFS_EINVAL, "mount of zeroed disk must fail");
    ffs_mkfs(&d, "x", 0);
    disk[100] ^= 0xFF; /* corrupt superblock: checksum must catch it */
    CHECK(ffs_mount(&fs, &d, 0, 0) == -FFS_EINVAL, "mount with bad checksum must fail");
    struct ffs_fsck_report r;
    CHECK(ffs_fsck(&d, 1, &r, NULL) == 1 && r.used_backup_super, "fsck restores the backup superblock");
    CHECK(ffs_mount(&fs, &d, 0, 0) == 0, "mount after restore");
    ffs_unmount(&fs);
    CHECK(ffs_mkfs(&d, "x", 0) == 0, "mkfs");
    CHECK(ffs_mount(&fs, &d, 0, 0) == 0, "mount");
    CHECK(ffs_create(&fs, FFS_ROOT_INO, "", FFS_T_FILE, 0, NULL) == -FFS_EINVAL, "empty name");
    CHECK(ffs_create(&fs, FFS_ROOT_INO, "..", FFS_T_FILE, 0, NULL) == -FFS_EINVAL, "dotdot name");
    CHECK(ffs_create(&fs, FFS_ROOT_INO, "a/b", FFS_T_FILE, 0, NULL) == -FFS_EINVAL, "slash in name");
    char longn[80];
    memset(longn, 'x', 58);
    longn[58] = 0;
    CHECK(ffs_create(&fs, FFS_ROOT_INO, longn, FFS_T_FILE, 0, NULL) == -FFS_ENAMETOOLONG, "long name");
    longn[57] = 0;
    CHECK(ffs_create(&fs, FFS_ROOT_INO, longn, FFS_T_FILE, 0, NULL) == 0, "57 char name ok");
    uint32_t ino;
    CHECK(ffs_lookup(&fs, FFS_ROOT_INO, longn, &ino) == 0, "lookup long name");
    CHECK(ffs_lookup(&fs, ino, "x", &ino) == -FFS_ENOTDIR, "lookup in file");
    /* write errors leave the fs consistent */
    uint32_t f;
    ffs_create(&fs, FFS_ROOT_INO, "io", FFS_T_FILE, 0, &f);
    static uint8_t big[200000];
    fail_writes_after = 20;
    ffs_write(&fs, f, 0, big, sizeof(big), 0);
    fail_writes_after = -1;
    ffs_unmount(&fs);
    CHECK(ffs_fsck(&d, 1, &r, NULL) >= 0, "fsck after I/O errors");
    CHECK(ffs_fsck(&d, 0, &r, NULL) == 0, "clean after repair");
    free(disk);
}

int main(void)
{
    printf("test_freshfs:\n");
    test_mount_rejects_garbage();
    run(4096, 6000, 1);    /* 16 MiB */
    run(600, 6000, 2);     /* tiny: lots of ENOSPC */
    run(16384, 4000, 3);   /* 64 MiB */
    run(2000, 8000, 99);
    run(300, 3000, 12345); /* very tiny */
    if (failures) {
        printf("test_freshfs: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_freshfs: PASS\n");
    return 0;
}

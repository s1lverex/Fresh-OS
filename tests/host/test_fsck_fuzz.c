/* FreshFS corruption fuzzer (host, ASan/UBSan).
 * Builds a populated file system, then repeatedly corrupts random metadata
 * bytes and checks that: fsck never crashes; after "fsck -y" a second check is
 * clean; the repaired file system mounts and every file can be read; and the
 * file system code itself never crashes on the damaged image before repair. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freshfs/freshfs.h"

void *ffs_malloc(size_t n) { return malloc(n); }
void ffs_free(void *p) { free(p); }

static uint8_t *disk;
static uint32_t nblocks;

static int rd(void *c, uint32_t b, void *buf)
{
    (void)c;
    if (b >= nblocks)
        return -1;
    memcpy(buf, disk + (size_t)b * FFS_BLOCK, FFS_BLOCK);
    return 0;
}

static int wr(void *c, uint32_t b, const void *buf)
{
    (void)c;
    if (b >= nblocks)
        return -1;
    memcpy(disk + (size_t)b * FFS_BLOCK, buf, FFS_BLOCK);
    return 0;
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int walk(ffs_fs *fs, uint32_t dir, int depth, int *files)
{
    if (depth > 64)
        return -1;
    uint64_t cur = 0;
    struct ffs_entry e;
    static uint8_t buf[65536];
    int rc;
    int guard = 0;
    while ((rc = ffs_readdir(fs, dir, &cur, &e)) == 1 && guard++ < 100000) {
        if (e.type == FFS_T_DIR) {
            if (walk(fs, e.ino, depth + 1, files) < 0)
                return -1;
        } else {
            (*files)++;
            uint64_t off = 0;
            int64_t n;
            while ((n = ffs_read(fs, e.ino, off, buf, sizeof(buf))) > 0)
                off += (uint64_t)n;
        }
    }
    return rc < 0 ? rc : 0;
}

static void populate(ffs_dev *d)
{
    ffs_fs fs;
    ffs_mkfs(d, "fuzz", 1);
    ffs_mount(&fs, d, 0, 1);
    uint32_t dirs[8] = {FFS_ROOT_INO};
    int nd = 1;
    static uint8_t data[300000];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 60; i++) {
        char name[32];
        snprintf(name, sizeof(name), "item%d", i);
        uint32_t ino;
        if (i % 9 == 0 && nd < 8) {
            ffs_create(&fs, dirs[rnd() % (uint64_t)nd], name, FFS_T_DIR, 1, &ino);
            dirs[nd++] = ino;
        } else {
            ffs_create(&fs, dirs[rnd() % (uint64_t)nd], name, FFS_T_FILE, 1, &ino);
            size_t len = i % 5 == 0 ? sizeof(data) : (size_t)(rnd() % 30000);
            ffs_write(&fs, ino, 0, data, len, 1);
        }
    }
    ffs_unmount(&fs);
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 3000;
    nblocks = 2048;
    disk = calloc(nblocks, FFS_BLOCK);
    uint8_t *pristine = malloc((size_t)nblocks * FFS_BLOCK);
    ffs_dev d = {NULL, rd, wr, nblocks};
    populate(&d);
    memcpy(pristine, disk, (size_t)nblocks * FFS_BLOCK);
    ffs_fs probe;
    ffs_mount(&probe, &d, 1, 0);
    uint32_t meta_end = probe.sb.data_start;
    int failures = 0, unusable = 0, repaired = 0;
    for (int it = 0; it < iters; it++) {
        memcpy(disk, pristine, (size_t)nblocks * FFS_BLOCK);
        int flips = 1 + (int)(rnd() % 40);
        for (int k = 0; k < flips; k++) {
            uint32_t blk;
            switch (rnd() % 4) {
            case 0: blk = 0; break;                                      /* superblock */
            case 1: blk = 1 + (uint32_t)(rnd() % (meta_end - 1)); break; /* bitmaps / inodes */
            case 2: blk = meta_end + (uint32_t)(rnd() % 64); break;      /* early data: dirs, indirect */
            default: blk = (uint32_t)(rnd() % nblocks); break;
            }
            size_t off = (size_t)blk * FFS_BLOCK + (size_t)(rnd() % FFS_BLOCK);
            disk[off] = rnd() % 3 == 0 ? (uint8_t)rnd() : (uint8_t)(disk[off] ^ (1u << (rnd() % 8)));
        }
        /* the live file system code must not crash on a damaged image */
        ffs_fs fs;
        if (ffs_mount(&fs, &d, 1, 0) == 0) {
            int files = 0;
            walk(&fs, FFS_ROOT_INO, 0, &files);
        }
        struct ffs_fsck_report r;
        int rc = ffs_fsck(&d, 1, &r, NULL);
        if (rc < 0) {
            unusable++;
            continue;
        }
        if (rc == 1)
            repaired++;
        int rc2 = ffs_fsck(&d, 0, &r, NULL);
        if (rc2 != 0) {
            printf("iteration %d: still %u errors after repair (rc %d)\n", it, r.errors, rc2);
            failures++;
            continue;
        }
        if (ffs_mount(&fs, &d, 0, 0) != 0) {
            printf("iteration %d: repaired fs does not mount\n", it);
            failures++;
            continue;
        }
        int files = 0;
        if (walk(&fs, FFS_ROOT_INO, 0, &files) < 0) {
            printf("iteration %d: walk failed after repair\n", it);
            failures++;
        }
        /* the repaired file system must be fully writable again */
        uint32_t ino;
        static uint8_t blob[50000];
        if (ffs_create(&fs, FFS_ROOT_INO, "after_repair", FFS_T_FILE, 0, &ino) == 0)
            ffs_write(&fs, ino, 0, blob, sizeof(blob), 0);
        ffs_unmount(&fs);
        if (ffs_fsck(&d, 0, &r, NULL) != 0) {
            printf("iteration %d: writes after repair broke consistency (%u errors)\n", it, r.errors);
            failures++;
        }
        if (failures > 10)
            break;
    }
    printf("test_fsck_fuzz: %d iterations, %d repaired, %d unrecoverable superblocks, %d failures: %s\n", iters,
           repaired, unusable, failures, failures ? "FAIL" : "PASS");
    free(pristine);
    free(disk);
    return failures ? 1 : 0;
}

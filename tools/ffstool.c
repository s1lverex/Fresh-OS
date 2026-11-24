/* ffstool - host utility for FreshFS images.
 *   ffstool mkfs  IMAGE SIZE_BYTES [LABEL]
 *   ffstool fsck  [-y] IMAGE
 *   ffstool ls    IMAGE [PATH]
 *   ffstool cat   IMAGE PATH
 *   ffstool put   IMAGE HOSTFILE PATH
 *   ffstool mkdir IMAGE PATH
 *   ffstool rm    IMAGE PATH
 *   ffstool info  IMAGE
 * IMAGE may be a bare FreshFS image or a whole GPT disk image, in which case
 * the partition with the FreshFS type GUID is used. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "freshfs/freshfs.h"

void *ffs_malloc(size_t n) { return malloc(n); }
void ffs_free(void *p) { free(p); }

static const uint8_t FRESHFS_GUID[16] = {0xC0, 0xA1, 0xE5, 0xF7, 0x2D, 0x3B, 0x6F, 0x4E,
                                         0x9A, 0x8B, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B};

struct img {
    FILE *f;
    uint64_t base; /* byte offset of the file system */
};

static int img_read(void *ctx, uint32_t blk, void *buf)
{
    struct img *im = ctx;
    if (fseeko(im->f, (off_t)(im->base + (uint64_t)blk * FFS_BLOCK), SEEK_SET))
        return -1;
    size_t n = fread(buf, 1, FFS_BLOCK, im->f);
    if (n < FFS_BLOCK)
        memset((char *)buf + n, 0, FFS_BLOCK - n);
    return 0;
}

static int img_write(void *ctx, uint32_t blk, const void *buf)
{
    struct img *im = ctx;
    if (fseeko(im->f, (off_t)(im->base + (uint64_t)blk * FFS_BLOCK), SEEK_SET))
        return -1;
    return fwrite(buf, 1, FFS_BLOCK, im->f) == FFS_BLOCK ? 0 : -1;
}

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "ffstool: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(3);
}

static ffs_dev open_dev(const char *path, struct img *im, const char *mode)
{
    im->f = fopen(path, mode);
    if (!im->f)
        die("cannot open %s", path);
    fseeko(im->f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(im->f);
    im->base = 0;
    uint64_t fs_bytes = size;
    uint8_t sec[512];
    if (size >= 34 * 512 && fseeko(im->f, 512, SEEK_SET) == 0 && fread(sec, 1, 512, im->f) == 512 &&
        !memcmp(sec, "EFI PART", 8)) {
        uint64_t ent_lba, n_ent = 0, esz = 0;
        memcpy(&ent_lba, sec + 72, 8);
        memcpy(&n_ent, sec + 80, 4);
        memcpy(&esz, sec + 84, 4);
        int found = 0;
        for (uint64_t i = 0; i < n_ent && i < 256 && esz >= 128; i++) {
            uint8_t e[128];
            if (fseeko(im->f, (off_t)(ent_lba * 512 + i * esz), SEEK_SET) || fread(e, 1, 128, im->f) != 128)
                break;
            if (!memcmp(e, FRESHFS_GUID, 16)) {
                uint64_t first, last;
                memcpy(&first, e + 32, 8);
                memcpy(&last, e + 40, 8);
                im->base = first * 512;
                fs_bytes = (last - first + 1) * 512;
                found = 1;
                break;
            }
        }
        if (!found)
            die("%s: GPT disk without a FreshFS partition", path);
    }
    ffs_dev d = {im, img_read, img_write, (uint32_t)(fs_bytes / FFS_BLOCK)};
    return d;
}

static void mount_or_die(ffs_fs *fs, ffs_dev *d, int ro)
{
    int rc = ffs_mount(fs, d, ro, (uint64_t)time(NULL));
    if (rc)
        die("mount failed (%s)", strerror(-rc));
}

static uint32_t resolve(ffs_fs *fs, const char *path, int parent, char *last)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    uint32_t ino = FFS_ROOT_INO;
    char *save = NULL;
    char *comps[128];
    int n = 0;
    for (char *t = strtok_r(buf, "/", &save); t && n < 128; t = strtok_r(NULL, "/", &save))
        comps[n++] = t;
    int upto = parent ? n - 1 : n;
    for (int i = 0; i < upto; i++) {
        uint32_t next;
        if (ffs_lookup(fs, ino, comps[i], &next))
            die("no such file or directory: %s", path);
        ino = next;
    }
    if (parent) {
        if (n == 0)
            die("bad path: %s", path);
        snprintf(last, FFS_NAME_MAX + 1, "%s", comps[n - 1]);
    }
    return ino;
}

static void log_line(const char *m) { printf("  %s\n", m); }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ffstool mkfs|fsck|ls|cat|put|mkdir|rm|info IMAGE ...\n");
        return 3;
    }
    const char *cmd = argv[1];
    struct img im;
    ffs_fs fs;
    uint64_t now = (uint64_t)time(NULL);

    if (!strcmp(cmd, "mkfs")) {
        if (argc < 4)
            die("usage: mkfs IMAGE SIZE [LABEL]%s", "");
        FILE *f = fopen(argv[2], "r+b");
        if (!f)
            f = fopen(argv[2], "w+b");
        if (!f)
            die("cannot create %s", argv[2]);
        uint64_t size = strtoull(argv[3], NULL, 0);
        if (ftruncate(fileno(f), (off_t)size))
            die("cannot size %s", argv[2]);
        fclose(f);
        ffs_dev d = open_dev(argv[2], &im, "r+b");
        int rc = ffs_mkfs(&d, argc > 4 ? argv[4] : "", now);
        if (rc)
            die("mkfs failed (%s)", strerror(-rc));
        fclose(im.f);
        return 0;
    }
    if (!strcmp(cmd, "fsck")) {
        int repair = 0;
        int ai = 2;
        if (!strcmp(argv[2], "-y")) {
            repair = 1;
            ai = 3;
        }
        if (ai >= argc)
            die("usage: fsck [-y] IMAGE%s", "");
        ffs_dev d = open_dev(argv[ai], &im, repair ? "r+b" : "rb");
        struct ffs_fsck_report r;
        int rc = ffs_fsck(&d, repair, &r, log_line);
        fclose(im.f);
        if (rc < 0) {
            printf("fsck: file system unusable (%s)\n", strerror(-rc));
            return 4;
        }
        printf("fsck: %u files, %u dirs, %u/%u blocks used, %u/%u inodes used, %u errors, %u fixed\n", r.files,
               r.dirs, r.used_blocks, r.used_blocks + r.free_blocks, r.used_inodes, r.used_inodes + r.free_inodes,
               r.errors, r.fixed);
        return rc;
    }

    int writes = !strcmp(cmd, "put") || !strcmp(cmd, "mkdir") || !strcmp(cmd, "rm");
    ffs_dev d = open_dev(argv[2], &im, writes ? "r+b" : "rb");
    mount_or_die(&fs, &d, !writes);
    int rc = 0;
    char last[FFS_NAME_MAX + 1];
    if (!strcmp(cmd, "info")) {
        printf("label=%s blocks=%u free_blocks=%u inodes=%u free_inodes=%u state=%s mounts=%u\n", fs.sb.label,
               fs.sb.total_blocks, fs.sb.free_blocks, fs.sb.inode_count, fs.sb.free_inodes,
               fs.sb.state == FFS_STATE_CLEAN ? "clean" : "dirty", fs.sb.mount_count);
    } else if (!strcmp(cmd, "ls")) {
        uint32_t dir = resolve(&fs, argc > 3 ? argv[3] : "/", 0, NULL);
        uint64_t cur = 0;
        struct ffs_entry e;
        while ((rc = ffs_readdir(&fs, dir, &cur, &e)) == 1) {
            struct ffs_stat st;
            ffs_stat(&fs, e.ino, &st);
            printf("%s %10llu %s\n", e.type == FFS_T_DIR ? "d" : "-", (unsigned long long)st.size, e.name);
        }
        if (rc < 0)
            die("readdir failed%s", "");
        rc = 0;
    } else if (!strcmp(cmd, "cat")) {
        if (argc < 4)
            die("usage: cat IMAGE PATH%s", "");
        uint32_t ino = resolve(&fs, argv[3], 0, NULL);
        char buf[65536];
        uint64_t off = 0;
        int64_t n;
        while ((n = ffs_read(&fs, ino, off, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, (size_t)n, stdout);
            off += (uint64_t)n;
        }
        if (n < 0)
            die("read failed%s", "");
    } else if (!strcmp(cmd, "put")) {
        if (argc < 5)
            die("usage: put IMAGE HOSTFILE PATH%s", "");
        FILE *in = fopen(argv[3], "rb");
        if (!in)
            die("cannot open %s", argv[3]);
        uint32_t dir = resolve(&fs, argv[4], 1, last);
        uint32_t ino;
        if (ffs_lookup(&fs, dir, last, &ino) == 0) {
            if ((rc = ffs_truncate(&fs, ino, 0, now)) != 0)
                die("truncate failed%s", "");
        } else if ((rc = ffs_create(&fs, dir, last, FFS_T_FILE, now, &ino)) != 0) {
            die("create failed: %s", strerror(-rc));
        }
        char buf[65536];
        uint64_t off = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            int64_t w = ffs_write(&fs, ino, off, buf, n, now);
            if (w != (int64_t)n)
                die("write failed (%s)", w < 0 ? strerror((int)-w) : "short write");
            off += n;
        }
        fclose(in);
    } else if (!strcmp(cmd, "mkdir")) {
        if (argc < 4)
            die("usage: mkdir IMAGE PATH%s", "");
        uint32_t dir = resolve(&fs, argv[3], 1, last);
        rc = ffs_create(&fs, dir, last, FFS_T_DIR, now, NULL);
        if (rc && rc != -FFS_EEXIST)
            die("mkdir failed: %s", strerror(-rc));
        rc = 0;
    } else if (!strcmp(cmd, "rm")) {
        if (argc < 4)
            die("usage: rm IMAGE PATH%s", "");
        uint32_t dir = resolve(&fs, argv[3], 1, last);
        rc = ffs_unlink(&fs, dir, last);
        if (rc)
            die("rm failed: %s", strerror(-rc));
    } else {
        die("unknown command %s", cmd);
    }
    if (writes)
        ffs_unmount(&fs);
    fclose(im.f);
    return rc;
}

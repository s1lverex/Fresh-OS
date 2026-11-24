/* fstest DIR [--fill] - file system stress test inside DIR.
 * Creates many files of varied sizes, verifies contents, renames, deletes,
 * writes a large file, fills a directory past one block of entries and
 * checks that free space returns to its starting value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

static int errors;
#define FAIL(...)                                                                                            \
    do {                                                                                                    \
        printf("fstest: FAIL: " __VA_ARGS__);                                                               \
        printf("\n");                                                                                       \
        errors++;                                                                                           \
    } while (0)

static unsigned char pat(long i, long seed) { return (unsigned char)(i * 13 + seed * 7 + (i >> 11)); }

static int write_file(const char *path, long size, long seed)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    static unsigned char buf[32768];
    long done = 0;
    while (done < size) {
        long n = size - done < (long)sizeof(buf) ? size - done : (long)sizeof(buf);
        for (long i = 0; i < n; i++)
            buf[i] = pat(done + i, seed);
        long w = write(fd, buf, (size_t)n);
        if (w != n) {
            close(fd);
            return w < 0 ? (int)w : -EIO;
        }
        done += n;
    }
    close(fd);
    return 0;
}

static int check_file(const char *path, long size, long seed)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    static unsigned char buf[32768];
    long done = 0, n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            if (buf[i] != pat(done + i, seed)) {
                close(fd);
                return -EIO;
            }
        done += n;
    }
    close(fd);
    return done == size ? 0 : -EIO;
}

static long free_blocks(const char *dir)
{
    struct fresh_statfs st;
    return statfs(dir, &st) == 0 ? (long)st.free_blocks : -1;
}

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "/data";
    int fill = argc > 2 && !strcmp(argv[2], "--fill");
    char dir[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/fstest.tmp", base);
    sync();
    long free0 = free_blocks(base);
    int r = mkdir(dir);
    if (r < 0)
        FAIL("mkdir %s: %s", dir, strerror(r));

    /* 1: many files of varied sizes (0 .. ~200 KiB) */
    const int N = 120;
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof(p), "%s/f%03d", dir, i);
        long size = (long)((i * 7919) % 200000);
        if (i % 10 == 0)
            size = 0;
        if ((r = write_file(p, size, i)) < 0)
            FAIL("write %s: %s", p, strerror(r));
    }
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof(p), "%s/f%03d", dir, i);
        long size = (long)((i * 7919) % 200000);
        if (i % 10 == 0)
            size = 0;
        if ((r = check_file(p, size, i)) < 0)
            FAIL("verify %s", p);
    }
    /* 2: directory listing sees all entries */
    int fd = open(dir, O_RDONLY), count = 0;
    struct fresh_dirent d;
    while (fd >= 0 && readdir(fd, &d) == 1)
        count++;
    close(fd);
    if (count != N)
        FAIL("readdir found %d entries, expected %d", count, N);
    /* 3: rename half, delete a third, verify the rest */
    for (int i = 0; i < N; i += 2) {
        snprintf(p, sizeof(p), "%s/f%03d", dir, i);
        snprintf(q, sizeof(q), "%s/renamed_%03d", dir, i);
        if ((r = rename(p, q)) < 0)
            FAIL("rename %s: %s", p, strerror(r));
    }
    for (int i = 0; i < N; i += 3) {
        snprintf(p, sizeof(p), i % 2 ? "%s/f%03d" : "%s/renamed_%03d", dir, i);
        if ((r = unlink(p)) < 0)
            FAIL("unlink %s: %s", p, strerror(r));
    }
    for (int i = 0; i < N; i++) {
        if (i % 3 == 0)
            continue;
        snprintf(p, sizeof(p), i % 2 ? "%s/f%03d" : "%s/renamed_%03d", dir, i);
        long size = (long)((i * 7919) % 200000);
        if (i % 10 == 0)
            size = 0;
        if (check_file(p, size, i) < 0)
            FAIL("verify after rename %s", p);
    }
    /* 4: large file (8 MiB: exercises double indirect blocks) + partial overwrite */
    snprintf(p, sizeof(p), "%s/big", dir);
    long big = 8L << 20;
    if ((r = write_file(p, big, 99)) < 0)
        FAIL("write big: %s", strerror(r));
    if (check_file(p, big, 99) < 0)
        FAIL("verify big");
    fd = open(p, O_RDWR);
    lseek(fd, 5000000, SEEK_SET);
    write(fd, "HELLO", 5);
    lseek(fd, 5000000, SEEK_SET);
    char hb[6] = {0};
    read(fd, hb, 5);
    if (strcmp(hb, "HELLO"))
        FAIL("overwrite in the middle");
    if (lseek(fd, 0, SEEK_END) != big)
        FAIL("size changed by overwrite");
    close(fd);
    if ((r = truncate(p, 12345)) < 0 || check_file(p, 12345, 99) < 0)
        FAIL("truncate big");
    /* 5: sparse write past end of file reads back zeros */
    snprintf(q, sizeof(q), "%s/sparse", dir);
    fd = open(q, O_RDWR | O_CREAT);
    lseek(fd, 100000, SEEK_SET);
    write(fd, "Z", 1);
    lseek(fd, 50000, SEEK_SET);
    char z[16];
    read(fd, z, sizeof(z));
    for (int i = 0; i < 16; i++)
        if (z[i])
            FAIL("sparse region not zero");
    close(fd);
    /* 6: optional: fill the disk until ENOSPC, then free it */
    if (fill) {
        snprintf(q, sizeof(q), "%s/filler", dir);
        r = write_file(q, 1L << 40, 5);
        if (r != -ENOSPC)
            FAIL("expected ENOSPC when filling, got %d", r);
        if (free_blocks(base) > 2)
            FAIL("disk not full after ENOSPC (%ld blocks free)", free_blocks(base));
        if (unlink(q) < 0)
            FAIL("remove filler");
    }
    /* 7: error cases */
    if (mkdir(dir) != -EEXIST)
        FAIL("mkdir existing");
    if (unlink(dir) != -ENOTEMPTY)
        FAIL("rmdir non-empty");
    snprintf(q, sizeof(q), "%s/sub", dir);
    mkdir(q);
    snprintf(p, sizeof(p), "%s/sub/inner", dir);
    if (rename(q, p) != -EINVAL)
        FAIL("rename dir into itself");
    char longname[80];
    memset(longname, 'a', 70);
    longname[70] = 0;
    snprintf(p, sizeof(p), "%s/%s", dir, longname);
    if (open(p, O_WRONLY | O_CREAT) != -ENAMETOOLONG)
        FAIL("long file name accepted");
    /* 8: clean up everything, space must come back */
    fd = open(dir, O_RDONLY);
    char names[200][64];
    int nn = 0;
    while (fd >= 0 && nn < 200 && readdir(fd, &d) == 1)
        strlcpy(names[nn++], d.name, 64);
    close(fd);
    for (int i = 0; i < nn; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        if ((r = unlink(p)) < 0)
            FAIL("cleanup %s: %s", p, strerror(r));
    }
    if ((r = unlink(dir)) < 0)
        FAIL("remove %s: %s", dir, strerror(r));
    sync();
    long free1 = free_blocks(base);
    if (free0 >= 0 && free0 != free1)
        FAIL("free blocks %ld before, %ld after", free0, free1);
    printf("fstest: %s: %d errors: %s\n", base, errors, errors ? "FAIL" : "PASS");
    return errors ? 1 : 0;
}

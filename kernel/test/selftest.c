/* Fresh OS kernel - in-kernel self tests (run via SYS_TEST / TEST_SELFTEST).
 * Each check prints PASS/FAIL; the return value is the number of failures. */
#include "../kernel.h"
#include "../mm/mm.h"
#include "../fs/vfs.h"
#include "../proc/proc.h"

static int failures, checks;

#define CHECK(cond, name)                                                                                    \
    do {                                                                                                    \
        checks++;                                                                                           \
        if (cond) {                                                                                         \
            kprintf("ktest: PASS %s\n", name);                                                              \
        } else {                                                                                            \
            failures++;                                                                                     \
            kprintf("ktest: FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);                                  \
        }                                                                                                   \
    } while (0)

static void test_string(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%d|%5s|%-4x|%05u|%p|%c|%lld", -42, "ab", 0xbe, 7u, (void *)0x10, 'z',
             -9000000000LL);
    CHECK(!strcmp(buf, "-42|   ab|be  |00007|0x10|z|-9000000000"), "snprintf formats");
    CHECK(snprintf(buf, 4, "hello") == 5 && !strcmp(buf, "hel"), "snprintf truncation");
    char a[16] = "abcdef";
    memmove(a + 2, a, 4);
    CHECK(!memcmp(a, "ababcd", 6), "memmove overlap");
}

static void test_pmm(void)
{
    uint64_t before = pmm_free_pages();
    uint64_t pages[64];
    bool ok = true;
    for (int i = 0; i < 64; i++) {
        pages[i] = pmm_alloc();
        if (!pages[i])
            ok = false;
        else if (*(uint64_t *)phys_to_virt(pages[i]) != 0)
            ok = false;
    }
    for (int i = 0; i < 64 && ok; i++)
        for (int j = i + 1; j < 64; j++)
            if (pages[i] == pages[j])
                ok = false;
    CHECK(ok, "pmm allocates distinct zeroed pages");
    for (int i = 0; i < 64; i++)
        if (pages[i])
            pmm_free(pages[i]);
    uint64_t c = pmm_alloc_contig(16, 0x100000000ULL);
    CHECK(c && c + 16 * PAGE_SIZE <= 0x100000000ULL, "pmm contiguous allocation below 4 GiB");
    if (c)
        pmm_free_contig(c, 16);
    CHECK(pmm_free_pages() == before, "pmm free count restored");
}

static void test_heap(void)
{
    uint64_t before = heap_used_bytes();
    void *ptrs[200];
    bool ok = true;
    for (int i = 0; i < 200; i++) {
        size_t sz = (size_t)(i * 37 % 5000) + 1;
        ptrs[i] = kmalloc(sz);
        if (!ptrs[i]) {
            ok = false;
            continue;
        }
        memset(ptrs[i], i & 0xFF, sz);
    }
    for (int i = 0; i < 200 && ok; i++) {
        size_t sz = (size_t)(i * 37 % 5000) + 1;
        uint8_t *p = ptrs[i];
        for (size_t k = 0; k < sz; k++)
            if (p[k] != (uint8_t)(i & 0xFF)) {
                ok = false;
                break;
            }
    }
    CHECK(ok, "heap blocks keep their contents");
    for (int i = 0; i < 200; i++)
        kfree(ptrs[i]);
    CHECK(heap_used_bytes() == before, "heap usage restored after free");
    char *r = kmalloc(10);
    strcpy(r, "fresh");
    r = krealloc(r, 10000);
    CHECK(r && !strcmp(r, "fresh"), "krealloc preserves data");
    kfree(r);
}

static void test_vfs_on(const char *base, const char *label)
{
    char dir[PATH_MAX], file[PATH_MAX], file2[PATH_MAX], name[96];
    snprintf(dir, sizeof(dir), "%s/ktest_dir", base);
    snprintf(file, sizeof(file), "%s/ktest_dir/a.bin", base);
    snprintf(file2, sizeof(file2), "%s/ktest_dir/b.bin", base);
    vfs_unlink(file);
    vfs_unlink(file2);
    vfs_unlink(dir);

    snprintf(name, sizeof(name), "%s: mkdir", label);
    CHECK(vfs_mkdir(dir) == 0, name);
    snprintf(name, sizeof(name), "%s: mkdir existing fails", label);
    CHECK(vfs_mkdir(dir) == -EEXIST, name);

    uint64_t size = 300 * 1024 + 123; /* spans direct + indirect blocks */
    uint8_t *data = kmalloc(size);
    for (uint64_t i = 0; i < size; i++)
        data[i] = (uint8_t)(i * 7 + (i >> 9));
    snprintf(name, sizeof(name), "%s: write file", label);
    CHECK(vfs_write_all(file, data, size) == 0, name);
    uint8_t *back = NULL;
    uint64_t bsize = 0;
    snprintf(name, sizeof(name), "%s: read back identical", label);
    CHECK(vfs_read_all(file, &back, &bsize, 1 << 30) == 0 && bsize == size && !memcmp(back, data, size), name);
    kfree(back);

    snprintf(name, sizeof(name), "%s: truncate", label);
    CHECK(vfs_truncate(file, 1000) == 0, name);
    struct fresh_stat st;
    snprintf(name, sizeof(name), "%s: stat after truncate", label);
    CHECK(vfs_stat(file, &st) == 0 && st.size == 1000 && st.type == FT_FILE, name);

    snprintf(name, sizeof(name), "%s: rename", label);
    CHECK(vfs_rename(file, file2) == 0 && vfs_stat(file, &st) == -ENOENT && vfs_stat(file2, &st) == 0, name);
    snprintf(name, sizeof(name), "%s: rmdir non-empty fails", label);
    CHECK(vfs_unlink(dir) == -ENOTEMPTY, name);
    snprintf(name, sizeof(name), "%s: unlink + rmdir", label);
    CHECK(vfs_unlink(file2) == 0 && vfs_unlink(dir) == 0 && vfs_stat(dir, &st) == -ENOENT, name);
    kfree(data);
}

static void test_pipe(void)
{
    struct file *r, *w;
    CHECK(pipe_create(&r, &w) == 0, "pipe create");
    CHECK(file_write(w, "hello", 5) == 5, "pipe write");
    char buf[8] = {0};
    CHECK(file_read(r, buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5), "pipe read");
    file_close(w);
    CHECK(file_read(r, buf, sizeof(buf)) == 0, "pipe EOF after writer closed");
    file_close(r);
}

static void test_paths(void)
{
    char out[PATH_MAX];
    CHECK(vfs_resolve("/a/b", "../c/./d", out) == 0 && !strcmp(out, "/a/c/d"), "path: relative with ..");
    CHECK(vfs_resolve("/", "/../../x//y/", out) == 0 && !strcmp(out, "/x/y"), "path: clamp at root");
    CHECK(vfs_resolve("/data", ".", out) == 0 && !strcmp(out, "/data"), "path: dot");
}

int ktest_run(void)
{
    failures = checks = 0;
    kprintf("ktest: starting kernel self tests\n");
    test_string();
    test_pmm();
    test_heap();
    test_paths();
    test_pipe();
    /* run the file system tests twice: the first round may grow caches (block
     * cache, heap size classes), the second must leave memory unchanged */
    uint64_t pages1 = 0, heap1 = 0;
    for (int round = 0; round < 2; round++) {
        test_vfs_on("", "ramfs");
        if (data_fs_mounted())
            test_vfs_on("/data", "freshfs");
        test_pipe();
        if (round == 0) {
            pages1 = pmm_free_pages();
            heap1 = heap_used_bytes();
        }
    }
    CHECK(pmm_free_pages() == pages1, "no page leak in steady state");
    CHECK(heap_used_bytes() == heap1, "no heap leak in steady state");
    kprintf("ktest: %d checks, %d failures\n", checks, failures);
    return failures;
}

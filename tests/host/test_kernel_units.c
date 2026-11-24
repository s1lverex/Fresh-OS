/* Host unit tests for kernel components that do not need hardware:
 * the kernel heap (kernel/mm/heap.c) and the ELF loader (kernel/proc/elf.c),
 * built with FRESH_HOST_TEST and stubbed page allocation. */
#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
#include "mm/mm.h"
#include "proc/proc.h"

struct boot_info g_boot;
uint64_t g_hhdm_offset = 0; /* identity: "physical" == host virtual */
static int failures;
#define CHECK(c, msg)                                                                                        \
    do {                                                                                                    \
        if (!(c)) {                                                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                            \
            failures++;                                                                                     \
        }                                                                                                   \
    } while (0)

void kprintf(const char *fmt, ...) { (void)fmt; }
void panic(const char *fmt, ...)
{
    printf("panic: %s\n", fmt);
    abort();
}
uint64_t pmm_alloc(void)
{
    void *p = aligned_alloc(4096, 4096);
    memset(p, 0, 4096);
    return (uint64_t)p;
}
uint64_t pmm_alloc_contig(uint64_t pages, uint64_t max)
{
    (void)max;
    void *p = aligned_alloc(4096, pages * 4096);
    memset(p, 0, pages * 4096);
    return (uint64_t)p;
}
void pmm_free(uint64_t p) { free((void *)p); }
void pmm_free_contig(uint64_t p, uint64_t pages)
{
    (void)pages;
    free((void *)p);
}

/* ---- fake user address space for the ELF loader */
#define MAXMAP 4096
static struct {
    uint64_t va;
    uint8_t *page;
    uint64_t flags;
} maps[MAXMAP];
static int nmaps;

static int find_map(uint64_t va)
{
    for (int i = 0; i < nmaps; i++)
        if (maps[i].va == va)
            return i;
    return -1;
}

uint64_t vmm_map_user_page(uint64_t *pml4, uint64_t va, uint64_t flags)
{
    (void)pml4;
    if (va >= USER_TOP || (va & 4095))
        return 0;
    int i = find_map(va);
    if (i >= 0) {
        maps[i].flags |= flags & PTE_W;
        return (uint64_t)maps[i].page;
    }
    if (nmaps >= MAXMAP)
        return 0; /* "out of memory" */
    maps[nmaps].va = va;
    maps[nmaps].page = calloc(1, 4096);
    maps[nmaps].flags = flags;
    return (uint64_t)maps[nmaps++].page;
}

int elf_copy_to_space(uint64_t *pml4, uint64_t va, const void *src, uint64_t n)
{
    (void)pml4;
    const uint8_t *s = src;
    while (n) {
        int i = find_map(va & ~4095ULL);
        if (i < 0)
            return -EFAULT;
        uint64_t off = va & 4095, chunk = 4096 - off < n ? 4096 - off : n;
        memcpy(maps[i].page + off, s, chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

static void reset_maps(void)
{
    for (int i = 0; i < nmaps; i++)
        free(maps[i].page);
    nmaps = 0;
}

static void test_heap(void)
{
    heap_init();
    uint64_t base = heap_used_bytes();
    void *p[1000];
    uint64_t s = 1;
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 1000; i++) {
            s = s * 6364136223846793005ULL + 1;
            size_t n = (size_t)(s >> 40) % (i % 10 == 0 ? 50000 : 3000) + 1;
            p[i] = kmalloc(n);
            CHECK(p[i] != NULL, "kmalloc");
            CHECK(((uint64_t)p[i] & 15) == 0, "16-byte alignment");
            memset(p[i], i & 0xFF, n);
        }
        for (int i = 0; i < 1000; i += 2)
            p[i] = krealloc(p[i], 100 + (size_t)i * 7);
        for (int i = 0; i < 1000; i++)
            kfree(p[i]);
    }
    CHECK(heap_used_bytes() == base, "heap accounting returns to zero");
    char *z = kzalloc(5000);
    int allzero = 1;
    for (int i = 0; i < 5000; i++)
        if (z[i])
            allzero = 0;
    CHECK(allzero, "kzalloc zeroes");
    kfree(z);
    char *d = kstrdup("fresh os");
    CHECK(!strcmp(d, "fresh os"), "kstrdup");
    kfree(d);
    CHECK(kmalloc((size_t)1 << 33) == NULL, "absurd size refused");
}

static void test_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return;
    }
    static uint8_t img[1 << 20], mut[1 << 20];
    size_t n = fread(img, 1, sizeof(img), f);
    fclose(f);
    uint64_t pml4[512];
    struct elf_image ei;
    CHECK(elf_load(pml4, img, n, &ei) == 0, "real program loads");
    CHECK(ei.entry >= USER_BASE && ei.brk > ei.entry && (ei.brk & 4095) == 0, "entry/brk sane");
    reset_maps();
    CHECK(elf_load(pml4, img, 10, &ei) == -ENOEXEC, "truncated header rejected");
    reset_maps();
    /* random mutations: must never crash, only succeed or fail cleanly */
    uint64_t s = 42;
    int ok = 0, rejected = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(mut, img, n);
        int flips = 1 + it % 8;
        for (int k = 0; k < flips; k++) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t off = (size_t)(s >> 33) % (it % 2 ? 64 + 56 * 4 : n);
            mut[off] = (uint8_t)(s >> 17);
        }
        size_t len = it % 7 == 0 ? (size_t)(s >> 40) % n : n;
        int rc = elf_load(pml4, mut, len, &ei);
        if (rc == 0) {
            ok++;
            CHECK(ei.entry >= USER_BASE && ei.entry < USER_HEAP_LIMIT, "accepted entry in user range");
        } else {
            rejected++;
            CHECK(rc == -ENOEXEC || rc == -ENOMEM, "only ENOEXEC/ENOMEM errors");
        }
        for (int i = 0; i < nmaps; i++)
            CHECK(maps[i].va >= USER_BASE && maps[i].va < USER_HEAP_LIMIT, "mapping inside user range");
        reset_maps();
    }
    printf("  elf fuzz: %d accepted, %d rejected\n", ok, rejected);
}

int main(int argc, char **argv)
{
    printf("test_kernel_units:\n");
    test_heap();
    test_elf(argc > 1 ? argv[1] : "../../build/rootfs/bin/echo");
    printf("test_kernel_units: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

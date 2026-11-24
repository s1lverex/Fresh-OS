/* Fresh OS kernel - heap.
 * Small blocks (<= 2 KiB incl. header) come from power-of-two size classes
 * carved out of single pages; larger requests take contiguous pages. Every
 * block has a 16-byte header so kfree can find its size and catch misuse. */
#include "mm.h"

#define MAGIC_USED 0xA110CA7Eu
#define MAGIC_FREE 0xF4EEF4EEu
#define NCLASSES 7 /* 32 .. 2048 */
#define CLASS_BIG 0xFFu

struct hdr {
    uint32_t magic;
    uint32_t cls;
    uint64_t size; /* requested size (small) or page count (big) */
};

struct free_block {
    struct hdr h;
    struct free_block *next;
};

static struct free_block *free_lists[NCLASSES];
static uint64_t used_bytes;
static uint64_t cache_pages; /* pages carved into small-block size classes */

static int class_of(size_t total)
{
    size_t sz = 32;
    for (int c = 0; c < NCLASSES; c++, sz <<= 1)
        if (total <= sz)
            return c;
    return -1;
}

void heap_init(void)
{
    for (int i = 0; i < NCLASSES; i++)
        free_lists[i] = NULL;
}

static bool refill(int c)
{
    uint64_t p = pmm_alloc();
    if (!p)
        return false;
    cache_pages++;
    size_t sz = (size_t)32 << c;
    uint8_t *base = phys_to_virt(p);
    for (size_t off = 0; off + sz <= PAGE_SIZE; off += sz) {
        struct free_block *b = (struct free_block *)(base + off);
        b->h.magic = MAGIC_FREE;
        b->h.cls = (uint32_t)c;
        b->next = free_lists[c];
        free_lists[c] = b;
    }
    return true;
}

void *kmalloc(size_t n)
{
    if (n == 0)
        n = 1;
    if (n > (1ULL << 32))
        return NULL;
    size_t total = n + sizeof(struct hdr);
    int c = class_of(total);
    uint64_t f = irq_save();
    void *ret = NULL;
    if (c >= 0) {
        if (free_lists[c] || refill(c)) {
            struct free_block *b = free_lists[c];
            free_lists[c] = b->next;
            if (b->h.magic != MAGIC_FREE)
                panic("kmalloc: heap corruption at %p", (void *)b);
            b->h.magic = MAGIC_USED;
            b->h.size = n;
            used_bytes += (size_t)32 << c;
            ret = (uint8_t *)b + sizeof(struct hdr);
        }
    } else {
        uint64_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t p = pmm_alloc_contig(pages, 0);
        if (p) {
            struct hdr *h = phys_to_virt(p);
            h->magic = MAGIC_USED;
            h->cls = CLASS_BIG;
            h->size = pages;
            used_bytes += pages * PAGE_SIZE;
            ret = (uint8_t *)h + sizeof(struct hdr);
        }
    }
    irq_restore(f);
    return ret;
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

static size_t capacity(struct hdr *h)
{
    if (h->cls == CLASS_BIG)
        return h->size * PAGE_SIZE - sizeof(struct hdr);
    return ((size_t)32 << h->cls) - sizeof(struct hdr);
}

void kfree(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)((uint8_t *)p - sizeof(struct hdr));
    if (h->magic == MAGIC_FREE)
        panic("kfree: double free of %p", p);
    if (h->magic != MAGIC_USED)
        panic("kfree: bad pointer %p", p);
    uint64_t f = irq_save();
    if (h->cls == CLASS_BIG) {
        uint64_t pages = h->size;
        h->magic = MAGIC_FREE;
        used_bytes -= pages * PAGE_SIZE;
        pmm_free_contig(virt_to_phys(h), pages);
    } else if (h->cls < NCLASSES) {
        struct free_block *b = (struct free_block *)h;
        b->h.magic = MAGIC_FREE;
        used_bytes -= (size_t)32 << h->cls;
        b->next = free_lists[h->cls];
        free_lists[h->cls] = b;
    } else {
        panic("kfree: corrupt header at %p", p);
    }
    irq_restore(f);
}

void *krealloc(void *p, size_t n)
{
    if (!p)
        return kmalloc(n);
    struct hdr *h = (struct hdr *)((uint8_t *)p - sizeof(struct hdr));
    if (h->magic != MAGIC_USED)
        panic("krealloc: bad pointer %p", p);
    size_t cap = capacity(h);
    if (n <= cap) {
        if (h->cls != CLASS_BIG)
            h->size = n;
        return p;
    }
    void *q = kmalloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, cap);
    kfree(p);
    return q;
}

char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

uint64_t heap_used_bytes(void) { return used_bytes; }
uint64_t heap_cache_pages(void) { return cache_pages; }

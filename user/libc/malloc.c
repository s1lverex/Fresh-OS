/* Fresh OS libc - malloc: address-ordered free list with coalescing on top
 * of sbrk. Blocks are 16-byte aligned with a 16-byte header. */
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

struct block {
    size_t size; /* total bytes including header, multiple of 16 */
    struct block *next;
};

#define HDR sizeof(struct block)
#define MIN_GROW (64 * 1024)
#define USED_MARK ((struct block *)0x55534544ULL)

static struct block *free_list;
static size_t in_use;

static void insert_free(struct block *b)
{
    struct block **pp = &free_list;
    while (*pp && *pp < b)
        pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    /* merge with next */
    if (b->next && (char *)b + b->size == (char *)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    /* merge with previous */
    if (pp != &free_list) {
        struct block *prev = (struct block *)((char *)pp - offsetof(struct block, next));
        if ((char *)prev + prev->size == (char *)b) {
            prev->size += b->size;
            prev->next = b->next;
        }
    }
}

static int grow(size_t need)
{
    size_t sz = need < MIN_GROW ? MIN_GROW : (need + 4095) & ~(size_t)4095;
    void *p = sbrk((long)sz);
    if ((long)p < 0)
        return -1;
    struct block *b = p;
    b->size = sz;
    insert_free(b);
    return 0;
}

void *malloc(size_t n)
{
    if (n == 0)
        n = 1;
    if (n > ((size_t)1 << 40))
        return NULL;
    size_t need = (n + HDR + 15) & ~(size_t)15;
    for (int attempt = 0; attempt < 2; attempt++) {
        struct block **pp = &free_list;
        while (*pp) {
            struct block *b = *pp;
            if (b->size >= need) {
                if (b->size - need >= HDR + 32) {
                    struct block *rest = (struct block *)((char *)b + need);
                    rest->size = b->size - need;
                    rest->next = b->next;
                    *pp = rest;
                    b->size = need;
                } else {
                    *pp = b->next;
                }
                b->next = USED_MARK;
                in_use += b->size;
                return (char *)b + HDR;
            }
            pp = &b->next;
        }
        if (grow(need))
            return NULL;
    }
    return NULL;
}

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)((char *)p - HDR);
    if (b->next != USED_MARK) {
        static const char msg[] = "free(): invalid pointer or double free\n";
        write(2, msg, sizeof(msg) - 1);
        sys_exit(134);
    }
    in_use -= b->size;
    insert_free(b);
}

void *calloc(size_t n, size_t sz)
{
    if (sz && n > ((size_t)-1) / sz)
        return NULL;
    void *p = malloc(n * sz);
    if (p)
        memset(p, 0, n * sz);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    struct block *b = (struct block *)((char *)p - HDR);
    size_t cap = b->size - HDR;
    if (n <= cap)
        return p;
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, cap);
    free(p);
    return q;
}

size_t malloc_in_use(void) { return in_use; }

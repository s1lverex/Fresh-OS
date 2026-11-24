/* Fresh OS kernel - physical page allocator (bitmap, 1 bit per 4 KiB page). */
#include "mm.h"

static uint8_t *bitmap;       /* 1 = used */
static uint64_t npages;       /* pages covered by the bitmap */
static uint64_t free_pages;
static uint64_t total_pages;  /* usable pages at boot */
static uint64_t hint;

static inline bool test_bit(uint64_t i) { return bitmap[i >> 3] & (1u << (i & 7)); }
static inline void set_bit(uint64_t i) { bitmap[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void clear_bit(uint64_t i) { bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return blen && alen && a < b + blen && b < a + alen;
}

/* ranges that must never be handed out */
static bool is_protected(uint64_t base, uint64_t len)
{
    if (base < 0x100000)
        return true;
    if (overlaps(base, len, g_boot.kernel_phys, ALIGN_UP(g_boot.kernel_size, 0x200000)))
        return true;
    if (overlaps(base, len, g_boot.initrd_phys, g_boot.initrd_size))
        return true;
    if (overlaps(base, len, g_boot.scratch_phys, g_boot.scratch_size))
        return true;
    return false;
}

void pmm_reserve_range(uint64_t base, uint64_t len)
{
    uint64_t first = base / PAGE_SIZE, last = (base + len + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = first; i < last && i < npages; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            free_pages--;
        }
    }
}

void pmm_init(void)
{
    uint64_t max = 0;
    for (uint32_t i = 0; i < g_mmap_count; i++) {
        struct boot_mmap_entry *e = &g_mmap[i];
        if (e->type == BOOT_MMAP_USABLE && e->base + e->length > max)
            max = e->base + e->length;
    }
    if (max > 0x8000000000ULL)
        max = 0x8000000000ULL;
    npages = max / PAGE_SIZE;
    uint64_t bytes = ALIGN_UP((npages + 7) / 8, PAGE_SIZE);

    /* place the bitmap in the first usable spot below 4 GiB that is free */
    uint64_t where = 0;
    for (uint32_t i = 0; i < g_mmap_count && !where; i++) {
        struct boot_mmap_entry *e = &g_mmap[i];
        if (e->type != BOOT_MMAP_USABLE)
            continue;
        uint64_t end = e->base + e->length;
        for (uint64_t c = ALIGN_UP(MAX(e->base, 0x100000), PAGE_SIZE); c + bytes <= end && c + bytes <= 0x100000000ULL;
             c += PAGE_SIZE) {
            if (!is_protected(c, bytes)) {
                where = c;
                break;
            }
        }
    }
    if (!where)
        panic("pmm: no memory for the page bitmap");
    bitmap = phys_to_virt(where);
    memset(bitmap, 0xFF, bytes);

    for (uint32_t i = 0; i < g_mmap_count; i++) {
        struct boot_mmap_entry *e = &g_mmap[i];
        if (e->type != BOOT_MMAP_USABLE)
            continue;
        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = first; p < last && p < npages; p++) {
            if (test_bit(p) && !is_protected(p * PAGE_SIZE, PAGE_SIZE)) {
                clear_bit(p);
                free_pages++;
            }
        }
    }
    pmm_reserve_range(where, bytes);
    total_pages = free_pages;
    hint = 0x100000 / PAGE_SIZE;
    kprintf("pmm: %llu MiB usable, bitmap at %#llx\n", (unsigned long long)(free_pages * PAGE_SIZE >> 20),
            (unsigned long long)where);
}

/* The bitmap pointer is a virtual address that changes when the kernel page
 * tables go live; vmm_init calls this to relocate it. */
void pmm_relocate(uint64_t old_offset, uint64_t new_offset)
{
    bitmap = (uint8_t *)((uint64_t)bitmap - old_offset + new_offset);
}

uint64_t pmm_alloc(void)
{
    uint64_t f = irq_save();
    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass ? 0 : hint;
        uint64_t end = pass ? hint : npages;
        for (uint64_t i = start; i < end; i++) {
            if (bitmap[i >> 3] == 0xFF) {
                i |= 7;
                continue;
            }
            if (!test_bit(i)) {
                set_bit(i);
                free_pages--;
                hint = i + 1;
                irq_restore(f);
                uint64_t phys = i * PAGE_SIZE;
                memset(phys_to_virt(phys), 0, PAGE_SIZE);
                return phys;
            }
        }
    }
    irq_restore(f);
    return 0;
}

uint64_t pmm_alloc_contig(uint64_t pages, uint64_t max_phys)
{
    if (!pages)
        return 0;
    uint64_t f = irq_save();
    uint64_t limit = max_phys ? MIN(npages, max_phys / PAGE_SIZE) : npages;
    uint64_t run = 0;
    for (uint64_t i = 0x100000 / PAGE_SIZE; i < limit; i++) {
        if (test_bit(i)) {
            run = 0;
            continue;
        }
        if (++run == pages) {
            uint64_t first = i + 1 - pages;
            for (uint64_t j = first; j <= i; j++)
                set_bit(j);
            free_pages -= pages;
            irq_restore(f);
            memset(phys_to_virt(first * PAGE_SIZE), 0, pages * PAGE_SIZE);
            return first * PAGE_SIZE;
        }
    }
    irq_restore(f);
    return 0;
}

void pmm_free(uint64_t phys)
{
    uint64_t i = phys / PAGE_SIZE;
    if (phys & (PAGE_SIZE - 1) || i >= npages)
        panic("pmm_free: bad address %#llx", (unsigned long long)phys);
    uint64_t f = irq_save();
    if (!test_bit(i))
        panic("pmm_free: double free of %#llx", (unsigned long long)phys);
    clear_bit(i);
    free_pages++;
    if (i < hint)
        hint = i;
    irq_restore(f);
}

void pmm_free_contig(uint64_t phys, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++)
        pmm_free(phys + i * PAGE_SIZE);
}

/* Return a boot-time protected range (e.g. the initrd) to the allocator.
 * Only pages inside usable memory map entries are released. */
void pmm_release_range(uint64_t base, uint64_t len)
{
    uint64_t f = irq_save();
    for (uint64_t p = ALIGN_UP(base, PAGE_SIZE); p + PAGE_SIZE <= base + len; p += PAGE_SIZE) {
        uint64_t i = p / PAGE_SIZE;
        if (i >= npages || p < 0x100000 || !test_bit(i))
            continue;
        if (overlaps(p, PAGE_SIZE, g_boot.kernel_phys, g_boot.kernel_size))
            continue;
        bool usable = false;
        for (uint32_t k = 0; k < g_mmap_count; k++)
            if (g_mmap[k].type == BOOT_MMAP_USABLE && p >= g_mmap[k].base && p + PAGE_SIZE <= g_mmap[k].base + g_mmap[k].length)
                usable = true;
        if (usable) {
            clear_bit(i);
            free_pages++;
            total_pages++;
        }
    }
    irq_restore(f);
}

uint64_t pmm_free_pages(void) { return free_pages; }
uint64_t pmm_total_pages(void) { return total_pages; }

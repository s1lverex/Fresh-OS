/* Fresh OS kernel - page tables.
 * Kernel half (shared by every address space):
 *   HHDM_BASE + phys   direct map of physical memory (2 MiB pages)
 *   KERNEL_VMA + off   kernel image
 * Lower half: per-process user memory, 4 KiB pages. */
#include "mm.h"
#include "../arch/arch.h"

uint64_t g_hhdm_offset = 0;
static uint64_t kernel_pml4_phys;
static uint64_t hhdm_limit;

#define TWO_MB 0x200000ULL

static inline uint64_t *pml4_virt(void) { return phys_to_virt(kernel_pml4_phys); }

static uint64_t *next_table(uint64_t *table, int idx, bool alloc, uint64_t flags)
{
    if (!(table[idx] & PTE_P)) {
        if (!alloc)
            return NULL;
        uint64_t p = pmm_alloc();
        if (!p)
            return NULL;
        table[idx] = p | flags;
    } else if (table[idx] & PTE_PS) {
        return NULL;
    }
    return phys_to_virt(table[idx] & PTE_ADDR);
}

static void map_2m(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *pdpt = next_table(pml4, (va >> 39) & 511, true, PTE_P | PTE_W);
    if (!pdpt)
        panic("vmm: out of memory (pdpt)");
    uint64_t *pd = next_table(pdpt, (va >> 30) & 511, true, PTE_P | PTE_W);
    if (!pd)
        panic("vmm: out of memory (pd)");
    pd[(va >> 21) & 511] = pa | flags | PTE_PS;
}

static bool range_has_ram(uint64_t base, uint64_t len)
{
    for (uint32_t i = 0; i < g_mmap_count; i++) {
        struct boot_mmap_entry *e = &g_mmap[i];
        if (e->type == BOOT_MMAP_RESERVED || e->type == BOOT_MMAP_BAD)
            continue;
        if (base < e->base + e->length && e->base < base + len)
            return true;
    }
    return false;
}

void vmm_init(void)
{
    kernel_pml4_phys = pmm_alloc();
    if (!kernel_pml4_phys)
        panic("vmm: no memory");
    uint64_t *pml4 = pml4_virt();

    uint64_t limit = 0x100000000ULL;
    for (uint32_t i = 0; i < g_mmap_count; i++) {
        uint64_t end = g_mmap[i].base + g_mmap[i].length;
        if (g_mmap[i].type != BOOT_MMAP_RESERVED && end > limit)
            limit = end;
    }
    limit = ALIGN_UP(limit, 0x40000000ULL);
    if (limit > 0x8000000000ULL)
        limit = 0x8000000000ULL;
    hhdm_limit = limit;

    uint64_t nx = cpu_has_nx ? PTE_NX : 0;
    uint64_t g = cpu_has_pge ? PTE_G : 0;
    for (uint64_t pa = 0; pa < limit; pa += TWO_MB) {
        uint64_t flags = PTE_P | PTE_W | g | nx;
        if (!range_has_ram(pa, TWO_MB))
            flags |= PTE_PCD | PTE_PWT; /* uncached MMIO */
        map_2m(pml4, HHDM_BASE + pa, pa, flags);
    }
    uint64_t ksize = ALIGN_UP(g_boot.kernel_size, TWO_MB);
    for (uint64_t off = 0; off < ksize; off += TWO_MB)
        map_2m(pml4, KERNEL_VMA + off, g_boot.kernel_phys + off, PTE_P | PTE_W | g);

    write_cr3(kernel_pml4_phys);
    uint64_t old = g_hhdm_offset;
    g_hhdm_offset = HHDM_BASE;
    pmm_relocate(old, HHDM_BASE);
    kprintf("vmm: kernel page tables active, direct map %llu GiB\n", (unsigned long long)(limit >> 30));
}

uint64_t vmm_kernel_cr3(void) { return kernel_pml4_phys; }

void *vmm_map_mmio(uint64_t phys, uint64_t size, bool write_combine)
{
    if (phys + size > 0x8000000000ULL || size == 0)
        return NULL;
    uint64_t start = ALIGN_DOWN(phys, TWO_MB), end = ALIGN_UP(phys + size, TWO_MB);
    uint64_t flags = PTE_P | PTE_W | (cpu_has_pge ? PTE_G : 0) | (cpu_has_nx ? PTE_NX : 0);
    flags |= write_combine ? PTE_PWT : (PTE_PCD | PTE_PWT);
    uint64_t *pml4 = pml4_virt();
    for (uint64_t pa = start; pa < end; pa += TWO_MB) {
        map_2m(pml4, HHDM_BASE + pa, pa, flags);
        invlpg(HHDM_BASE + pa);
    }
    return (void *)(HHDM_BASE + phys);
}

/* ------------------------------------------------------------ user spaces */
uint64_t *vmm_new_space(void)
{
    uint64_t p = pmm_alloc();
    if (!p)
        return NULL;
    uint64_t *pml4 = phys_to_virt(p);
    uint64_t *k = pml4_virt();
    for (int i = 256; i < 512; i++)
        pml4[i] = k[i];
    return pml4;
}

static uint64_t *walk(uint64_t *pml4, uint64_t va, bool alloc)
{
    uint64_t f = PTE_P | PTE_W | PTE_U;
    uint64_t *pdpt = next_table(pml4, (va >> 39) & 511, alloc, f);
    if (!pdpt)
        return NULL;
    uint64_t *pd = next_table(pdpt, (va >> 30) & 511, alloc, f);
    if (!pd)
        return NULL;
    uint64_t *pt = next_table(pd, (va >> 21) & 511, alloc, f);
    if (!pt)
        return NULL;
    return &pt[(va >> 12) & 511];
}

uint64_t vmm_map_user_page(uint64_t *pml4, uint64_t va, uint64_t flags)
{
    if (va >= USER_TOP || (va & (PAGE_SIZE - 1)))
        return 0;
    uint64_t *pte = walk(pml4, va, true);
    if (!pte)
        return 0;
    if (*pte & PTE_P) {
        if (flags & PTE_W)
            *pte |= PTE_W;
        if (!(flags & PTE_NX))
            *pte &= ~PTE_NX;
        return *pte & PTE_ADDR;
    }
    uint64_t p = pmm_alloc();
    if (!p)
        return 0;
    if (!cpu_has_nx)
        flags &= ~PTE_NX;
    *pte = p | PTE_P | PTE_U | (flags & (PTE_W | PTE_NX));
    return p;
}

int vmm_unmap_user_page(uint64_t *pml4, uint64_t va)
{
    uint64_t *pte = walk(pml4, va, false);
    if (!pte || !(*pte & PTE_P))
        return -1;
    pmm_free(*pte & PTE_ADDR);
    *pte = 0;
    if ((read_cr3() & PTE_ADDR) == virt_to_phys(pml4))
        invlpg(va);
    return 0;
}

uint64_t vmm_user_phys(uint64_t *pml4, uint64_t va)
{
    if (va >= USER_TOP)
        return 0;
    uint64_t *pte = walk(pml4, va, false);
    if (!pte || !(*pte & PTE_P))
        return 0;
    return (*pte & PTE_ADDR) | (va & (PAGE_SIZE - 1));
}

void vmm_free_space(uint64_t *pml4)
{
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_P))
            continue;
        uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P))
                continue;
            uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P))
                    continue;
                uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR);
                for (int i1 = 0; i1 < 512; i1++)
                    if (pt[i1] & PTE_P)
                        pmm_free(pt[i1] & PTE_ADDR);
                pmm_free(pd[i2] & PTE_ADDR);
            }
            pmm_free(pdpt[i3] & PTE_ADDR);
        }
        pmm_free(pml4[i4] & PTE_ADDR);
    }
    pmm_free(virt_to_phys(pml4));
}

uint64_t vmm_count_user_pages(uint64_t *pml4)
{
    uint64_t n = 0;
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_P))
            continue;
        uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P))
                continue;
            uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P))
                    continue;
                uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR);
                for (int i1 = 0; i1 < 512; i1++)
                    if (pt[i1] & PTE_P)
                        n++;
            }
        }
    }
    return n;
}

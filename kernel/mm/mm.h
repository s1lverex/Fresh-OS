/* Fresh OS kernel - memory management. */
#ifndef MM_H
#define MM_H

#include "../kernel.h"

/* physical <-> virtual (HHDM). Before the kernel page tables are active the
 * offset is 0 (identity mapping from the loader). */
extern uint64_t g_hhdm_offset;
static inline void *phys_to_virt(uint64_t p) { return (void *)(p + g_hhdm_offset); }
static inline uint64_t virt_to_phys(const void *v)
{
    uint64_t a = (uint64_t)v;
    if (a >= KERNEL_VMA)
        return a - KERNEL_VMA + g_boot.kernel_phys;
    return a - g_hhdm_offset;
}

/* ---- physical page allocator */
void pmm_init(void);
uint64_t pmm_alloc(void);                       /* one zeroed page, 0 on OOM */
uint64_t pmm_alloc_contig(uint64_t pages, uint64_t max_phys); /* zeroed */
void pmm_free(uint64_t phys);
void pmm_free_contig(uint64_t phys, uint64_t pages);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
void pmm_reserve_range(uint64_t base, uint64_t len);
void pmm_release_range(uint64_t base, uint64_t len);
void pmm_relocate(uint64_t old_offset, uint64_t new_offset);

/* ---- page tables */
#define PTE_P (1ULL << 0)
#define PTE_W (1ULL << 1)
#define PTE_U (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_A (1ULL << 5)
#define PTE_D (1ULL << 6)
#define PTE_PS (1ULL << 7)
#define PTE_G (1ULL << 8)
#define PTE_DEVICE (1ULL << 9) /* software bit: device memory, never freed */
#define PTE_NX (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

void vmm_init(void);
uint64_t vmm_kernel_cr3(void);
/* map physical MMIO into the kernel (HHDM); returns virtual address */
void *vmm_map_mmio(uint64_t phys, uint64_t size, bool write_combine);

/* user address spaces */
uint64_t *vmm_new_space(void);                  /* returns PML4 (virtual) */
void vmm_free_space(uint64_t *pml4);
/* map one zeroed user page, returns physical or 0 */
uint64_t vmm_map_user_page(uint64_t *pml4, uint64_t va, uint64_t flags);
int vmm_unmap_user_page(uint64_t *pml4, uint64_t va);
/* map device memory (e.g. the framebuffer) into user space */
int vmm_map_user_device(uint64_t *pml4, uint64_t va, uint64_t phys, uint64_t flags);
void vmm_unmap_user_device(uint64_t *pml4, uint64_t va);
uint64_t vmm_user_phys(uint64_t *pml4, uint64_t va); /* 0 if unmapped */
uint64_t vmm_count_user_pages(uint64_t *pml4);

/* ---- kernel heap */
void heap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
char *kstrdup(const char *s);
uint64_t heap_used_bytes(void);
uint64_t heap_cache_pages(void); /* pages kept by the heap's size classes */

#endif

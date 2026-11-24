/* Fresh OS kernel - CPU feature setup: SSE, PAT, NX, global pages, syscall. */
#include "arch.h"

bool cpu_has_nx;
bool cpu_has_pat;
bool cpu_has_pge;
char cpu_brand[48];

extern void syscall_entry(void);

void cpu_init(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    cpu_has_pat = d & (1u << 16);
    cpu_has_pge = d & (1u << 13);
    bool has_fxsr = d & (1u << 24), has_sse = d & (1u << 25);
    if (!has_fxsr || !has_sse)
        panic("CPU without SSE/FXSR is not supported");

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t maxext = a;
    if (maxext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        cpu_has_nx = d & (1u << 20);
    }
    memset(cpu_brand, 0, sizeof(cpu_brand));
    if (maxext >= 0x80000004) {
        uint32_t *p = (uint32_t *)cpu_brand;
        for (uint32_t l = 0; l < 3; l++)
            cpuid(0x80000002 + l, 0, &p[l * 4], &p[l * 4 + 1], &p[l * 4 + 2], &p[l * 4 + 3]);
        cpu_brand[47] = 0;
    } else {
        strcpy(cpu_brand, "x86-64 CPU");
    }

    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);  /* EM off */
    cr0 |= (1ULL << 1);   /* MP     */
    cr0 |= (1ULL << 16);  /* WP: kernel honours read-only pages */
    cr0 &= ~(1ULL << 3);  /* TS off */
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR, OSXMMEXCPT */
    if (cpu_has_pge)
        cr4 |= (1ULL << 7);
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));
    __asm__ volatile("fninit");

    if (cpu_has_pat) /* PA0 WB, PA1 WC, PA2 UC-, PA3 UC (x2) */
        wrmsr(0x277, 0x0007010600070106ULL);
    uint64_t efer = rdmsr(0xC0000080);
    if (cpu_has_nx)
        efer |= (1ULL << 11);
    efer |= 1; /* SCE: syscall/sysret */
    wrmsr(0xC0000080, efer);
}

void syscall_init(void)
{
    wrmsr(0xC0000081, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32)); /* STAR  */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                       /* LSTAR */
    wrmsr(0xC0000084, 0x47700);                                       /* FMASK: TF IF DF IOPL NT AC */
}

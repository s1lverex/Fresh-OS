/* Fresh OS kernel - GDT and TSS. */
#include "arch.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt[7] __attribute__((aligned(16)));
static struct tss tss __attribute__((aligned(16)));
static uint8_t ist_stacks[2][16384] __attribute__((aligned(16)));

extern void gdt_load(void *gdtr, uint16_t tss_sel);

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* kernel code 64 */
    gdt[2] = 0x00CF92000000FFFFULL; /* kernel data    */
    gdt[3] = 0x00CFF2000000FFFFULL; /* user data      */
    gdt[4] = 0x00AFFA000000FFFFULL; /* user code 64   */
    uint64_t base = (uint64_t)&tss, limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((limit >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: ring 3 port access faults */
    tss.ist[0] = (uint64_t)&ist_stacks[0][sizeof(ist_stacks[0])];
    tss.ist[1] = (uint64_t)&ist_stacks[1][sizeof(ist_stacks[1])];
    struct gdtr g = {sizeof(gdt) - 1, (uint64_t)gdt};
    gdt_load(&g, SEL_TSS);
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
    cpu_kstack_top = rsp0;
}

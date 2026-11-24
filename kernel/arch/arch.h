/* Fresh OS kernel - x86-64 architecture interfaces. */
#ifndef ARCH_H
#define ARCH_H

#include "../kernel.h"

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B /* 0x18 | RPL3 */
#define SEL_UCODE 0x23 /* 0x20 | RPL3 */
#define SEL_TSS 0x28

/* Register frame pushed by every interrupt/exception stub and by the
 * syscall entry path. Layout must match kernel/arch/isr.asm. */
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define VEC_SYSCALL 0x80
#define IRQ_BASE 32

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);
void idt_init(void);
void cpu_init(void);       /* SSE, PAT, NX, syscall MSRs */
void syscall_init(void);

extern bool cpu_has_nx;
extern bool cpu_has_pat;
extern bool cpu_has_pge;
extern char cpu_brand[48];

typedef void (*irq_handler_t)(struct trapframe *tf);
void irq_register(int irq, irq_handler_t h);
void pic_init(void);
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_eoi(int irq);

/* assembly helpers (isr.asm) */
void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void user_entry_trampoline(void);
extern uint64_t cpu_kstack_top;
void fpu_save(void *area);
void fpu_restore(void *area);

/* user memory access with fault recovery; return 0 or -EFAULT */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
/* copies a NUL terminated string, returns length or -EFAULT/-ENAMETOOLONG */
long strncpy_from_user(char *dst, const char *usrc, size_t max);
bool user_range_ok(uint64_t addr, uint64_t len);

#endif

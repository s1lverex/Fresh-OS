/* Fresh OS kernel - IDT, exception handling and IRQ dispatch. */
#include "arch.h"
#include "../proc/proc.h"
#include "../mm/mm.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t type;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256] __attribute__((aligned(16)));
static irq_handler_t irq_handlers[16];

extern uint64_t isr_stub_table[48];
extern uint64_t isr_stub_spurious;
extern char copy_user_begin[], copy_user_end[], copy_user_fault[];
bool pic_irq_real(int irq);

static const char *exc_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved", "x87 FPU error", "alignment check",
    "machine check", "SIMD floating point error", "virtualization", "control protection", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "hypervisor injection", "VMM communication",
    "security exception", "reserved"};

static void set_gate(int v, uint64_t handler, uint8_t ist)
{
    idt[v].off_lo = handler & 0xFFFF;
    idt[v].sel = SEL_KCODE;
    idt[v].ist = ist;
    idt[v].type = 0x8E;
    idt[v].off_mid = (handler >> 16) & 0xFFFF;
    idt[v].off_hi = (uint32_t)(handler >> 32);
    idt[v].zero = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        set_gate(i, isr_stub_table[i], 0);
    for (int i = 48; i < 256; i++)
        set_gate(i, isr_stub_spurious, 0);
    set_gate(2, isr_stub_table[2], 2);  /* NMI          */
    set_gate(8, isr_stub_table[8], 1);  /* double fault */
    set_gate(18, isr_stub_table[18], 2); /* machine check */
    struct idtr r = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(r));
}

void irq_register(int irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
    pic_unmask(irq);
}

static void dump_frame(struct trapframe *tf)
{
    kprintf("  rip=%016llx cs=%llx rflags=%llx rsp=%016llx ss=%llx\n", (unsigned long long)tf->rip,
            (unsigned long long)tf->cs, (unsigned long long)tf->rflags, (unsigned long long)tf->rsp,
            (unsigned long long)tf->ss);
    kprintf("  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", (unsigned long long)tf->rax,
            (unsigned long long)tf->rbx, (unsigned long long)tf->rcx, (unsigned long long)tf->rdx);
    kprintf("  rsi=%016llx rdi=%016llx rbp=%016llx r8 =%016llx\n", (unsigned long long)tf->rsi,
            (unsigned long long)tf->rdi, (unsigned long long)tf->rbp, (unsigned long long)tf->r8);
    kprintf("  r9 =%016llx r10=%016llx r11=%016llx r12=%016llx\n", (unsigned long long)tf->r9,
            (unsigned long long)tf->r10, (unsigned long long)tf->r11, (unsigned long long)tf->r12);
    kprintf("  r13=%016llx r14=%016llx r15=%016llx cr2=%016llx\n", (unsigned long long)tf->r13,
            (unsigned long long)tf->r14, (unsigned long long)tf->r15, (unsigned long long)read_cr2());
}

static void exception(struct trapframe *tf)
{
    uint64_t v = tf->vector;
    bool user = (tf->cs & 3) == 3;
    if (v == 14) {
        uint64_t addr = read_cr2();
        bool in_copy = !user && tf->rip >= (uint64_t)copy_user_begin && tf->rip < (uint64_t)copy_user_end;
        /* not-present fault in the user stack window: grow the stack */
        if ((user || in_copy) && !(tf->error & 1) && proc_demand_page(addr))
            return;
        if (in_copy) {
            tf->rip = (uint64_t)copy_user_fault;
            return;
        }
    }
    if (user) {
        proc_fault(tf, exc_names[v & 31]);
        return;
    }
    kprintf("\n*** KERNEL PANIC: %s (vector %llu, error %#llx)\n", exc_names[v & 31], (unsigned long long)v,
            (unsigned long long)tf->error);
    dump_frame(tf);
    panic("unhandled CPU exception in kernel mode");
}

void isr_dispatch(struct trapframe *tf)
{
    uint64_t v = tf->vector;
    if (v < 32) {
        exception(tf);
    } else if (v < 48) {
        int irq = (int)(v - IRQ_BASE);
        if (pic_irq_real(irq)) {
            pic_eoi(irq);
            if (irq_handlers[irq])
                irq_handlers[irq](tf);
        }
    }
    if ((tf->cs & 3) == 3)
        proc_return_check(tf);
}

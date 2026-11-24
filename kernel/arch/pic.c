/* Fresh OS kernel - 8259 PIC (remapped to vectors 32..47). */
#include "arch.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static uint16_t mask = 0xFFFF;

static void apply(void)
{
    outb(PIC1_DATA, (uint8_t)mask);
    outb(PIC2_DATA, (uint8_t)(mask >> 8));
}

void pic_init(void)
{
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();
    outb(PIC1_DATA, IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);
    io_wait();
    outb(PIC1_DATA, 4);
    io_wait();
    outb(PIC2_DATA, 2);
    io_wait();
    outb(PIC1_DATA, 1);
    io_wait();
    outb(PIC2_DATA, 1);
    io_wait();
    mask = 0xFFFF & ~(1 << 2); /* cascade line stays open */
    apply();
}

void pic_unmask(int irq)
{
    uint64_t f = irq_save();
    mask &= (uint16_t)~(1u << irq);
    apply();
    irq_restore(f);
}

void pic_mask(int irq)
{
    uint64_t f = irq_save();
    mask |= (uint16_t)(1u << irq);
    apply();
    irq_restore(f);
}

void pic_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* true if the IRQ is really in service (filters spurious IRQ 7/15) */
bool pic_irq_real(int irq)
{
    if (irq != 7 && irq != 15)
        return true;
    uint16_t port = irq == 7 ? PIC1_CMD : PIC2_CMD;
    outb(port, 0x0B);
    uint8_t isr = inb(port);
    if (isr & 0x80)
        return true;
    if (irq == 15)
        outb(PIC1_CMD, 0x20); /* master still saw the cascade */
    return false;
}

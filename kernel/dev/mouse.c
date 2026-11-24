/* Fresh OS kernel - PS/2 mouse (standard 3-byte packets). */
#include "dev.h"
#include "../arch/arch.h"

static uint8_t pkt[3];
static int idx;
static bool present;

void ps2_drain(void);

/* called by the PS/2 controller code for every byte from the aux port */
void mouse_byte(uint8_t b)
{
    if (idx == 0 && !(b & 0x08)) /* byte 0 always has bit 3 set: resync */
        return;
    pkt[idx++] = b;
    if (idx < 3)
        return;
    idx = 0;
    if (pkt[0] & 0xC0) /* overflow */
        return;
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    uint32_t buttons = 0;
    if (pkt[0] & 1)
        buttons |= MOUSE_LEFT;
    if (pkt[0] & 2)
        buttons |= MOUSE_RIGHT;
    if (pkt[0] & 4)
        buttons |= MOUSE_MIDDLE;
    if (input_grabbed())
        input_mouse(dx, -dy, buttons);
}

static void mouse_irq(struct trapframe *tf)
{
    UNUSED(tf);
    ps2_drain();
}

static bool wait_in(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2))
            return true;
    return false;
}

static int read_aux(void)
{
    for (int i = 0; i < 200000; i++) {
        uint8_t st = inb(0x64);
        if (st & 1) {
            uint8_t b = inb(0x60);
            if (st & 0x20)
                return b;
        }
    }
    return -1;
}

static bool mouse_cmd(uint8_t cmd)
{
    if (!wait_in())
        return false;
    outb(0x64, 0xD4);
    if (!wait_in())
        return false;
    outb(0x60, cmd);
    return read_aux() == 0xFA;
}

void mouse_init(void)
{
    if (inb(0x64) == 0xFF)
        return;
    uint64_t f = irq_save();
    if (wait_in())
        outb(0x64, 0xA8); /* enable the aux port */
    if (wait_in()) {
        outb(0x64, 0x20);
        for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++)
            ;
        uint8_t cfg = inb(0x60);
        cfg |= 0x02;           /* aux IRQ 12 */
        cfg &= (uint8_t)~0x20; /* aux clock on */
        if (wait_in()) {
            outb(0x64, 0x60);
            if (wait_in())
                outb(0x60, cfg);
        }
    }
    present = mouse_cmd(0xF6) && mouse_cmd(0xF4); /* defaults, enable reporting */
    idx = 0;
    irq_restore(f);
    if (!present) {
        kprintf("mouse: no PS/2 mouse\n");
        return;
    }
    irq_register(12, mouse_irq);
    kprintf("mouse: PS/2 mouse ready\n");
}

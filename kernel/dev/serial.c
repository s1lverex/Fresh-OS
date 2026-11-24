/* Fresh OS kernel - 16550 UART on COM1 (115200 8N1), console mirror + input. */
#include "dev.h"
#include "../arch/arch.h"

#define COM1 0x3F8
static bool present;

static void serial_irq(struct trapframe *tf)
{
    UNUSED(tf);
    int guard = 64;
    while ((inb(COM1 + 5) & 1) && guard--) {
        char c = (char)inb(COM1);
        tty_input(c);
    }
}

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupts while configuring */
    outb(COM1 + 3, 0x80); /* DLAB */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x1E); /* loopback test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        present = false;
        return;
    }
    outb(COM1 + 4, 0x0F); /* normal operation, OUT2 enables the IRQ line */
    present = true;
}

void serial_enable_irq(void)
{
    if (!present)
        return;
    irq_register(4, serial_irq);
    outb(COM1 + 1, 0x01); /* receive data available interrupt */
}

bool serial_present(void) { return present; }

static void putc_raw(char c)
{
    int guard = 100000;
    while (!(inb(COM1 + 5) & 0x20) && --guard)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    if (!present)
        return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            putc_raw('\r');
        putc_raw(s[i]);
    }
}

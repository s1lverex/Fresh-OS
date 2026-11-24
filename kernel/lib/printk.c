/* Fresh OS kernel - kernel log output and panic. */
#include "../kernel.h"
#include "../dev/dev.h"

static bool to_console;
static char buf[1024];

void klog_set_console(bool on) { to_console = on; }

void kvprintf(const char *fmt, va_list ap)
{
    uint64_t f = irq_save();
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    serial_write(buf, (size_t)n);
    if (to_console)
        console_write(buf, (size_t)n);
    irq_restore(f);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void panic(const char *fmt, ...)
{
    __asm__ volatile("cli");
    static int nested;
    if (nested++) {
        serial_write("\n*** nested panic\n", 18);
        cpu_halt_forever();
    }
    to_console = true;
    kprintf("\n\033[1;31m*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\033[0m\nSystem halted. Please restart your computer.\n");
    if (g_test_mode)
        outb(0xF4, 0x7F); /* QEMU isa-debug-exit: report failure to the harness */
    cpu_halt_forever();
}

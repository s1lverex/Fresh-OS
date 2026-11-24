/* Fresh OS kernel - PIT system timer (100 Hz) and TSC based delays. */
#include "arch.h"
#include "../proc/proc.h"

volatile uint64_t g_ticks;
static uint64_t tsc_per_us;

#define PIT_HZ 1193182

static void timer_irq(struct trapframe *tf)
{
    g_ticks++;
    sched_tick(tf);
}

/* measure TSC frequency with PIT channel 2 (no interrupts needed) */
static void calibrate_tsc(void)
{
    uint16_t count = PIT_HZ / 100; /* 10 ms */
    outb(0x61, (inb(0x61) & 0xFD) | 1); /* gate on, speaker off */
    outb(0x43, 0xB0);                  /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    uint8_t g = inb(0x61) & 0xFE;      /* restart gate */
    outb(0x61, g);
    outb(0x61, g | 1);
    uint64_t t0 = rdtsc();
    uint64_t guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 100000000ULL)
        ;
    uint64_t t1 = rdtsc();
    tsc_per_us = (t1 - t0) / 10000;
    if (tsc_per_us == 0)
        tsc_per_us = 1000;
}

void timer_init(void)
{
    calibrate_tsc();
    uint16_t div = PIT_HZ / TIMER_HZ;
    outb(0x43, 0x34); /* ch0, lobyte/hibyte, mode 2 */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, timer_irq);
    kprintf("timer: PIT %d Hz, TSC %llu MHz\n", TIMER_HZ, (unsigned long long)tsc_per_us);
}

uint64_t uptime_ms(void) { return g_ticks * (1000 / TIMER_HZ); }

void udelay(uint64_t us)
{
    uint64_t end = rdtsc() + us * tsc_per_us;
    while (rdtsc() < end)
        cpu_relax();
}

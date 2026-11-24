/* Fresh OS kernel - core definitions. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include "fresh/str.h"
#include "fresh/abi.h"
#include "fresh/bootinfo.h"

#define KERNEL_VMA 0xFFFFFFFF80000000ULL
#define HHDM_BASE 0xFFFF800000000000ULL
#define PAGE_SIZE 4096ULL
#define USER_TOP 0x0000800000000000ULL

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define UNUSED(x) ((void)(x))

/* ---- port I/O and CPU helpers */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t inw(uint16_t p)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint32_t inl(uint16_t p)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* interrupt flag save/restore */
#ifdef FRESH_HOST_TEST /* kernel code compiled into host unit tests */
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t f) { (void)f; }
#else
static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void irq_restore(uint64_t f)
{
    if (f & 0x200)
        __asm__ volatile("sti" ::: "memory");
}
#endif
static inline void cpu_relax(void) { __asm__ volatile("pause"); }

/* ---- logging / panic (kernel/lib/printk.c) */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
void klog_set_console(bool on);
__attribute__((noreturn)) void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define KASSERT(c)                                                                    \
    do {                                                                              \
        if (!(c))                                                                     \
            panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__);            \
    } while (0)

/* ---- boot information (copied out of loader memory at startup) */
extern struct boot_info g_boot;
extern struct boot_mmap_entry g_mmap[];
extern uint32_t g_mmap_count;
bool cmdline_has(const char *word);
extern bool g_test_mode;

/* ---- timer (kernel/arch/timer.c) */
#define TIMER_HZ 100
extern volatile uint64_t g_ticks;
uint64_t uptime_ms(void);
void timer_init(void);
void udelay(uint64_t us);

/* ---- misc */
void cpu_halt_forever(void) __attribute__((noreturn));
uint64_t rtc_unix_time(void);
void rtc_init(void);

#endif

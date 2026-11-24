/* crashtest MODE - misbehave on purpose; the kernel must kill only this
 * process. Modes that test error returns exit 0 when the kernel behaved. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fresh.h>

static volatile int forever = 1;
/* addresses come from volatile variables so compilers cannot prove (and
 * complain about) the deliberate bad accesses */
static volatile uintptr_t addr_null = 0, addr_8 = 8, addr_kdata = 0xFFFF800000100000ULL,
                          addr_kimage = 0xFFFFFFFF80000000ULL, addr_noncanon = 0x8000000000000000ULL;
#define AT(a) (*(volatile uint64_t *)(a))
static int depth(int n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    if (!forever)
        return 0;
    return depth(n + 1) + pad[0];
}

static int check(const char *what, long got, long want)
{
    if (got != want) {
        printf("crashtest: %s returned %ld, expected %ld\n", what, got, want);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: crashtest null|read0|kwrite|kread|nx|priv|io|div0|ud|stack|gp|badptr|bigalloc|exitcode|spin|oom\n");
        return 2;
    }
    const char *m = argv[1];
    fflush(stdout);
    if (!strcmp(m, "null"))
        AT(addr_null) = 1;
    else if (!strcmp(m, "read0"))
        printf("%llx\n", (unsigned long long)AT(addr_8));
    else if (!strcmp(m, "kwrite"))
        AT(addr_kdata) = 1;
    else if (!strcmp(m, "kread"))
        printf("%llx\n", (unsigned long long)AT(addr_kimage));
    else if (!strcmp(m, "nx")) {
        static unsigned char code[16] = {0xC3};
        ((void (*)(void))code)();
    } else if (!strcmp(m, "priv"))
        __asm__ volatile("cli; hlt");
    else if (!strcmp(m, "io"))
        __asm__ volatile("outb %%al, $0x80" ::"a"(0));
    else if (!strcmp(m, "div0"))
        __asm__ volatile("xor %%ecx, %%ecx; mov $10, %%eax; xor %%edx, %%edx; div %%ecx" ::: "eax", "ecx", "edx");
    else if (!strcmp(m, "ud"))
        __asm__ volatile("ud2");
    else if (!strcmp(m, "stack"))
        return depth(0);
    else if (!strcmp(m, "gp"))
        printf("%llx\n", (unsigned long long)AT(addr_noncanon));
    else if (!strcmp(m, "badptr")) {
        int bad = 0;
        char buf[16];
        bad |= check("read(kernel ptr)", read(0, (void *)0xFFFF800000000000ULL, 16), -EFAULT);
        bad |= check("write(null)", write(1, NULL, 16), -EFAULT);
        bad |= check("write(unmapped)", write(1, (void *)0x10000000ULL, 16), -EFAULT);
        bad |= check("open(kernel ptr)", open((const char *)0xFFFFFFFF80000000ULL, O_RDONLY), -EFAULT);
        bad |= check("open(unmapped)", open((const char *)0x20000000ULL, O_RDONLY), -EFAULT);
        bad |= check("stat(null)", stat(NULL, (struct fresh_stat *)buf), -EFAULT);
        bad |= check("stat(out=kernel)", stat("/", (struct fresh_stat *)0xFFFF800000000000ULL), -EFAULT);
        bad |= check("getcwd(kernel)", getcwd((char *)0xFFFF800000001000ULL, 64), -EFAULT);
        bad |= check("pipe(kernel)", pipe((int *)0xFFFF800000000000ULL), -EFAULT);
        bad |= check("spawn(bad argv)", spawn("/bin/true", (char *const *)0x30000000ULL, NULL, 0), -EFAULT);
        bad |= check("read(bad fd)", read(99, buf, 1), -EBADF);
        bad |= check("syscall 999", fresh_syscall(999, 0, 0, 0, 0, 0, 0), -ENOSYS);
        bad |= check("syscall -1", fresh_syscall(-1, 0, 0, 0, 0, 0, 0), -ENOSYS);
        bad |= check("write(overflow len)", write(1, buf, (size_t)-1), -EFAULT);
        bad |= check("kill(init)", kill(1), -EPERM);
        char *ro = (char *)"read only string";
        bad |= check("read(into rodata)", (long)(read(-5, ro, 4)), -EBADF);
        printf("crashtest: badptr %s\n", bad ? "FAILED" : "OK");
        return bad;
    } else if (!strcmp(m, "bigalloc")) {
        void *p = malloc((size_t)1 << 39);
        long r = (long)sbrk((long)1 << 46);
        int good = !p && r == -ENOMEM;
        free(p);
        printf("crashtest: bigalloc %s\n", good ? "OK" : "FAILED");
        return good ? 0 : 1;
    } else if (!strcmp(m, "exitcode")) {
        return 42;
    } else if (!strcmp(m, "spin")) { /* burn CPU forever: tests preemption */
        for (volatile unsigned long i = 0;; i++)
            ;
    } else if (!strcmp(m, "oom")) { /* take all memory, touch it, give it back */
        size_t total = 0;
        void *chunks[16384];
        int n = 0;
        while (n < 16384) {
            char *p = malloc(1 << 20);
            if (!p)
                break;
            for (size_t k = 0; k < (1 << 20); k += 4096)
                p[k] = 1;
            chunks[n++] = p;
            total += 1 << 20;
        }
        for (int i = 0; i < n; i++)
            free(chunks[i]);
        printf("crashtest: oom got %zu MiB before malloc failed: OK\n", total >> 20);
        return 0;
    }
    printf("crashtest: still alive after '%s'\n", m);
    return 99;
}

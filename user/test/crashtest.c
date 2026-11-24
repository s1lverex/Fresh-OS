/* crashtest MODE - misbehave on purpose; the kernel must kill only this
 * process. Modes that test error returns exit 0 when the kernel behaved. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fresh.h>

static volatile int forever = 1;
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
        printf("usage: crashtest null|read0|kwrite|kread|nx|priv|io|div0|ud|stack|gp|badptr|bigalloc|exitcode\n");
        return 2;
    }
    const char *m = argv[1];
    fflush(stdout);
    if (!strcmp(m, "null"))
        *(volatile int *)0 = 1;
    else if (!strcmp(m, "read0"))
        printf("%d\n", *(volatile int *)8);
    else if (!strcmp(m, "kwrite"))
        *(volatile uint64_t *)0xFFFF800000100000ULL = 1;
    else if (!strcmp(m, "kread"))
        printf("%llx\n", (unsigned long long)*(volatile uint64_t *)0xFFFFFFFF80000000ULL);
    else if (!strcmp(m, "nx")) {
        static unsigned char code[16] = {0xC3};
        ((void (*)(void))code)();
    } else if (!strcmp(m, "priv"))
        __asm__ volatile("cli; hlt");
    else if (!strcmp(m, "io"))
        __asm__ volatile("outb %%al, $0x80" ::"a"(0));
    else if (!strcmp(m, "div0")) {
        volatile int z = 0;
        printf("%d\n", 10 / z);
    } else if (!strcmp(m, "ud"))
        __asm__ volatile("ud2");
    else if (!strcmp(m, "stack"))
        return depth(0);
    else if (!strcmp(m, "gp"))
        printf("%llx\n", (unsigned long long)*(volatile uint64_t *)0x8000000000000000ULL);
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
        printf("crashtest: bigalloc %s\n", (!p && r == -ENOMEM) ? "OK" : "FAILED");
        return (!p && r == -ENOMEM) ? 0 : 1;
    } else if (!strcmp(m, "exitcode"))
        return 42;
    printf("crashtest: still alive after '%s'\n", m);
    return 99;
}

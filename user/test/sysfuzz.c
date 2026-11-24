/* sysfuzz [iterations] [seed] - call system calls with random arguments.
 * Calls that would end the test (exit, reboot, ...) or block forever are
 * skipped. The kernel must survive and keep working. */
#include <stdio.h>
#include <stdlib.h>
#include <fresh.h>

static uint64_t state;
static uint64_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static long rarg(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return (long)(rnd() % 64);
    case 2: return -(long)(rnd() % 64);
    case 3: return (long)0xFFFF800000000000ULL + (long)(rnd() % 0x100000);
    case 4: return (long)(0x400000 + rnd() % 0x20000); /* our own image */
    case 5: return (long)(0x7FFFFFFF0000ULL - rnd() % 0x10000); /* our stack */
    case 6: return (long)rnd();
    default: return (long)(rnd() % 0x100000000ULL);
    }
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    state = argc > 2 ? (uint64_t)atol(argv[2]) : 0x1234567;
    if (!state)
        state = 1;
    long counts[SYS_COUNT + 2] = {0};
    for (long i = 0; i < iters; i++) {
        long n = (long)(rnd() % (SYS_COUNT + 2));
        long a = rarg(), b = rarg(), c = rarg();
        switch (n) {
        case SYS_EXIT: case SYS_SPAWN: case SYS_KILL: case SYS_REBOOT: case SYS_POWEROFF:
        case SYS_TEST: case SYS_PIPE: case SYS_TTYMODE: case SYS_TTYFG: case SYS_CHDIR: case SYS_DUP:
            continue;
        case SYS_SLEEP:
            a = (long)(rnd() % 3);
            break;
        case SYS_READ:
        case SYS_WRITE:
        case SYS_CLOSE:
            if (a >= 0 && a <= 2)
                continue;
            break;
        case SYS_WAIT:
            c = WAIT_NOHANG;
            break;
        case SYS_SBRK:
            if (a > 0 && a > (1L << 26))
                a = 1L << 20;
            if (a < 0)
                a = 0;
            break;
        default:
            break;
        }
        fresh_syscall(n, a, b, c, rarg(), rarg(), rarg());
        counts[n]++;
    }
    /* afterwards the system must still work normally */
    /* paths are built only now so that random pointers into our image can
     * never hit an absolute path string while fuzzing */
    char sh[16], motd[16];
    snprintf(sh, sizeof(sh), "%cbin%csh", '/', '/');
    snprintf(motd, sizeof(motd), "%cetc%cmotd", '/', '/');
    struct fresh_stat st;
    int ok = stat(sh, &st) == 0 && st.type == FT_FILE;
    int fd = open(motd, O_RDONLY);
    char b2[8];
    ok = ok && fd >= 0 && read(fd, b2, sizeof(b2)) == sizeof(b2);
    printf("sysfuzz: %ld random calls survived, system %s\n", iters, ok ? "healthy: PASS" : "BROKEN: FAIL");
    return ok ? 0 : 1;
}

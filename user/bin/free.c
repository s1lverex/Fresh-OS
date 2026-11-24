/* free - show memory usage */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    struct fresh_sysinfo si;
    if (sysinfo(&si) < 0)
        return 1;
    unsigned long long t = si.mem_total >> 10, f = si.mem_free >> 10;
    printf("          total       used       free   (KiB)\n");
    printf("Mem: %10llu %10llu %10llu\n", t, t - f, f);
    return 0;
}

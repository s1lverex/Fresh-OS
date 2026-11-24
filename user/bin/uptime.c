/* uptime - time since boot */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    uint64_t ms = uptime_ms();
    uint64_t s = ms / 1000;
    struct fresh_sysinfo si;
    sysinfo(&si);
    printf("up %llu:%02llu:%02llu, %u processes\n", (unsigned long long)(s / 3600),
           (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60), si.nprocs);
    return 0;
}

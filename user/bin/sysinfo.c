/* sysinfo - describe the system */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    struct fresh_sysinfo si;
    if (sysinfo(&si) < 0)
        return 1;
    uint64_t s = si.uptime_ms / 1000;
    printf("Fresh OS %s\n", si.version);
    printf("  CPU       : %s\n", si.cpu);
    printf("  Memory    : %llu MiB total, %llu MiB free\n", (unsigned long long)(si.mem_total >> 20),
           (unsigned long long)(si.mem_free >> 20));
    printf("  Firmware  : %s\n", si.boot_type == 2 ? "UEFI" : "BIOS");
    if (si.fb_width)
        printf("  Display   : %ux%u framebuffer\n", si.fb_width, si.fb_height);
    else
        printf("  Display   : text mode\n");
    printf("  Disks     : %u\n", si.ndisks);
    printf("  /data     : %s\n", si.data_mounted ? "FreshFS (persistent)" : "not available (RAM only)");
    printf("  Processes : %u\n", si.nprocs);
    printf("  Uptime    : %llu:%02llu:%02llu\n", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
           (unsigned long long)(s % 60));
    return 0;
}

/* df - show file system space */
#include <stdio.h>
#include <fresh.h>

static void show(const char *path)
{
    struct fresh_statfs st;
    if (statfs(path, &st) < 0)
        return;
    unsigned long long total = st.total_blocks * st.block_size >> 10;
    unsigned long long freek = st.free_blocks * st.block_size >> 10;
    printf("%-8s %-6s %10llu %10llu %10llu %4llu%%\n", st.fs_name, path, total, total - freek, freek,
           total ? (total - freek) * 100 / total : 0);
}

int main(void)
{
    printf("%-8s %-6s %10s %10s %10s %5s\n", "FS", "MOUNT", "SIZE(K)", "USED(K)", "FREE(K)", "USE");
    show("/");
    struct fresh_sysinfo si;
    if (sysinfo(&si) == 0 && si.data_mounted)
        show("/data");
    return 0;
}

/* ps - list processes */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    static const char *states[] = {"?", "ready", "run", "wait", "zombie"};
    printf("  PID  PPID  STATE     MEM(K)  CPU(ms)  NAME\n");
    struct fresh_procinfo pi;
    for (int i = 0; procinfo(i, &pi) == 1; i++)
        printf("%5d %5d  %-7s %8llu %8llu  %s\n", pi.pid, pi.ppid, states[pi.state < 5 ? pi.state : 0],
               (unsigned long long)(pi.mem_bytes / 1024), (unsigned long long)pi.cpu_ms, pi.name);
    return 0;
}

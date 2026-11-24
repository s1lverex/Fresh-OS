/* init - first user process: runs /etc/rc, then keeps a shell running on
 * the console and reaps orphaned processes. */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static int start_shell(void)
{
    char *argv[] = {"sh", NULL};
    int pid = spawn("/bin/sh", argv, NULL, 0);
    if (pid < 0)
        printf("init: cannot start /bin/sh: %s\n", strerror(pid));
    return pid;
}

int main(void)
{
    struct fresh_stat st;
    if (stat("/etc/rc", &st) == 0) {
        char *argv[] = {"sh", "/etc/rc", NULL};
        int pid = spawn("/bin/sh", argv, NULL, 0);
        int status;
        if (pid > 0)
            while (wait_pid(-1, &status) != pid)
                ;
    }
    struct fresh_sysinfo si;
    if (sysinfo(&si) == 0 && si.data_mounted)
        chdir("/data");
    int shell = start_shell();
    for (;;) {
        int status;
        int pid = wait_pid(-1, &status);
        if (pid < 0) {
            if (shell < 0) {
                sleep_ms(1000);
                shell = start_shell();
            }
            continue;
        }
        if (pid == shell) {
            printf("\n");
            shell = start_shell();
        }
    }
}

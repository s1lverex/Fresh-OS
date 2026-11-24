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
    int have_si = sysinfo(&si) == 0;
    if (have_si && si.data_mounted)
        chdir("/data");
    int shell = start_shell();
    /* graphical desktop on the screen; the text console keeps running on the
     * serial port and comes back when the desktop exits */
    struct fresh_stat st2;
    if (have_si && si.fb_width && stat("/etc/nogui", &st2) != 0) {
        char *dargv[] = {"desktop", NULL};
        int d = spawn("/bin/desktop", dargv, NULL, 0);
        if (d < 0)
            printf("init: cannot start the desktop: %s\n", strerror(d));
    }
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

/* help - list the available commands */
#include <stdio.h>

static const char *cmds[][2] = {
    {"cat FILE...", "show file contents"},
    {"cd DIR", "change directory (shell builtin)"},
    {"clear", "clear the screen"},
    {"cp SRC DST", "copy files"},
    {"date", "show date and time"},
    {"desktop", "start the graphical desktop"},
    {"df", "show disk space"},
    {"echo TEXT", "print text"},
    {"edit FILE", "full-screen text editor (Ctrl+S save, Ctrl+Q quit)"},
    {"free", "show memory usage"},
    {"grep TEXT FILE", "find lines containing TEXT"},
    {"head / tail FILE", "first / last lines of a file"},
    {"hexdump FILE", "hex view of a file"},
    {"kill PID", "stop a process"},
    {"ls [-l] [DIR]", "list files"},
    {"mkdir [-p] DIR", "create a directory"},
    {"mv SRC DST", "move / rename"},
    {"ps", "list processes"},
    {"pwd", "show current directory (shell builtin)"},
    {"reboot / shutdown", "restart / power off safely"},
    {"rm [-r] PATH", "delete files"},
    {"seq N", "print numbers 1..N"},
    {"sleep SEC", "wait"},
    {"stat FILE", "file details"},
    {"sync", "flush data to disk"},
    {"sysinfo", "system overview"},
    {"touch FILE", "create an empty file"},
    {"uptime", "time since boot"},
    {"wc FILE", "count lines, words, bytes"},
};

int main(void)
{
    printf("Fresh OS commands:\n\n");
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        printf("  %-20s %s\n", cmds[i][0], cmds[i][1]);
    printf("\nShell: pipes (a | b), redirection (> >> <), ; && ||, & for background,\n"
           "wildcards (*.txt), variables (X=1; echo $X), history (Up/Down), Tab completion.\n"
           "Files in /data are saved to disk; everything else is lost at power off.\n");
    return 0;
}

/* kill - terminate processes by PID */
#include <stdio.h>
#include <stdlib.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: kill pid...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        int r = kill(atoi(argv[i]));
        if (r < 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(r));
            rc = 1;
        }
    }
    return rc;
}

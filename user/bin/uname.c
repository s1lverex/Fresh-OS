/* uname - print the system name */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-a"))
        printf("Fresh OS %s x86_64\n", FRESH_VERSION);
    else
        printf("Fresh\n");
    return 0;
}

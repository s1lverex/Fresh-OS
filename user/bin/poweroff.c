/* poweroff - power off the computer */
#include <stdio.h>
#include <fresh.h>
int main(void)
{
    poweroff();
    fprintf(stderr, "power off failed\n");
    return 1;
}

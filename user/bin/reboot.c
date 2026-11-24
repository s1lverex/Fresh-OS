/* reboot - restart the computer */
#include <stdio.h>
#include <fresh.h>
int main(void)
{
    reboot();
    fprintf(stderr, "reboot failed\n");
    return 1;
}

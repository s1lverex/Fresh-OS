/* shutdown - power off the computer (also available as 'poweroff') */
#include <stdio.h>
#include <fresh.h>
int main(void)
{
    poweroff();
    fprintf(stderr, "power off failed\n");
    return 1;
}

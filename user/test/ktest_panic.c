/* ktest_panic - ask the kernel to panic (only honoured in test mode) */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    long r = fresh_test(TEST_PANIC, 0);
    printf("ktest_panic: refused (%s)\n", strerror((int)r));
    return 1;
}

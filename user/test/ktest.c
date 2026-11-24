/* ktest - run the in-kernel self tests */
#include <stdio.h>
#include <fresh.h>

int main(void)
{
    long failures = fresh_test(TEST_SELFTEST, 0);
    printf("ktest: %s (%ld failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}

/* date - print the current date and time (UTC) */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    uint64_t t = unix_time();
    if (argc > 1 && !strcmp(argv[1], "+%s")) {
        printf("%llu\n", (unsigned long long)t);
        return 0;
    }
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm_simple tm;
    unix_to_tm(t, &tm);
    printf("%s %s %2d %02d:%02d:%02d UTC %d\n", days[tm.wday], mon[(tm.month - 1) % 12], tm.day, tm.hour, tm.min,
           tm.sec, tm.year);
    return 0;
}

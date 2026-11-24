/* stat - show file information */
#include <stdio.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct fresh_stat st;
        int r = stat(argv[i], &st);
        if (r < 0) {
            fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(r));
            rc = 1;
            continue;
        }
        struct tm_simple tm;
        unix_to_tm(st.mtime, &tm);
        printf("  File: %s\n  Type: %s\n  Size: %llu\n  Inode: %llu\n  Modified: %04d-%02d-%02d %02d:%02d:%02d\n",
               argv[i], st.type == FT_DIR ? "directory" : "file", (unsigned long long)st.size,
               (unsigned long long)st.ino, tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec);
    }
    return rc;
}

/* rmdir - remove empty directories */
#include <stdio.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir dir...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        struct fresh_stat st;
        int r = stat(argv[i], &st);
        if (r == 0 && st.type != FT_DIR)
            r = -ENOTDIR;
        if (r == 0)
            r = unlink(argv[i]);
        if (r < 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(r));
            rc = 1;
        }
    }
    return rc;
}

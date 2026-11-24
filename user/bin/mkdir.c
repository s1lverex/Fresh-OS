/* mkdir - create directories.  mkdir [-p] dir... */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    strlcpy(buf, path, sizeof(buf));
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            int r = mkdir(buf);
            if (r < 0 && r != -EEXIST)
                return r;
            *p = '/';
        }
    }
    int r = mkdir(buf);
    return r == -EEXIST ? 0 : r;
}

int main(int argc, char **argv)
{
    int parents = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) {
            parents = 1;
            continue;
        }
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i]);
        if (r < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(r));
            rc = 1;
        }
    }
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    return rc;
}

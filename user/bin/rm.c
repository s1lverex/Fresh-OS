/* rm - remove files.  rm [-r] [-f] path... */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static int recursive, force;

static int rm_path(const char *path)
{
    struct fresh_stat st;
    int r = stat(path, &st);
    if (r < 0) {
        if (!force)
            fprintf(stderr, "rm: %s: %s\n", path, strerror(r));
        return force ? 0 : 1;
    }
    if (st.type == FT_DIR) {
        if (!recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        int rc = 0;
        for (;;) { /* re-open after each removal: entries shift */
            int fd = open(path, O_RDONLY);
            if (fd < 0)
                break;
            struct fresh_dirent d;
            int got = readdir(fd, &d);
            close(fd);
            if (got != 1)
                break;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, d.name);
            if (rm_path(child)) {
                rc = 1;
                break;
            }
        }
        if (rc)
            return rc;
    }
    r = unlink(path);
    if (r < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, strerror(r));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (char *p = argv[i] + 1; *p; p++) {
                if (*p == 'r' || *p == 'R')
                    recursive = 1;
                else if (*p == 'f')
                    force = 1;
            }
            continue;
        }
        n++;
        rc |= rm_path(argv[i]);
    }
    if (!n && !force) {
        fprintf(stderr, "usage: rm [-rf] path...\n");
        return 2;
    }
    return rc;
}

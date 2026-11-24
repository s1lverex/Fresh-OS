/* mv - move or rename files.  mv SRC... DST */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static char buf[65536];

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int copy_then_remove(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return in;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return out;
    }
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) {
            rc = -EIO;
            break;
        }
    if (n < 0)
        rc = (int)n;
    close(in);
    close(out);
    if (rc == 0)
        rc = unlink(src);
    else
        unlink(dst);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mv SRC... DST\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct fresh_stat st;
    int dst_dir = stat(dst, &st) == 0 && st.type == FT_DIR;
    if (argc > 3 && !dst_dir) {
        fprintf(stderr, "mv: %s: not a directory\n", dst);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[PATH_MAX];
        if (dst_dir)
            snprintf(target, sizeof(target), "%s/%s", dst, base(argv[i]));
        else
            strlcpy(target, dst, sizeof(target));
        int r = rename(argv[i], target);
        if (r == -EXDEV) {
            struct fresh_stat s2;
            if (stat(argv[i], &s2) == 0 && s2.type == FT_FILE)
                r = copy_then_remove(argv[i], target);
        }
        if (r < 0) {
            fprintf(stderr, "mv: %s: %s\n", argv[i], strerror(r));
            rc = 1;
        }
    }
    return rc;
}

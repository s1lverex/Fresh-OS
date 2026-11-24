/* cp - copy files.  cp SRC... DST  (DST may be a directory) */
#include <stdio.h>
#include <string.h>
#include <fresh.h>

static char buf[65536];

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int copy_file(const char *src, const char *dst)
{
    struct fresh_stat st;
    int r = stat(src, &st);
    if (r < 0) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror(r));
        return 1;
    }
    if (st.type == FT_DIR) {
        fprintf(stderr, "cp: %s: is a directory\n", src);
        return 1;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror(in));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(out));
        close(in);
        return 1;
    }
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        long w = write(out, buf, (size_t)n);
        if (w != n) {
            fprintf(stderr, "cp: %s: %s\n", dst, w < 0 ? strerror((int)w) : "short write");
            rc = 1;
            break;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror((int)n));
        rc = 1;
    }
    close(in);
    close(out);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cp SRC... DST\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct fresh_stat st;
    int dst_dir = stat(dst, &st) == 0 && st.type == FT_DIR;
    if (argc > 3 && !dst_dir) {
        fprintf(stderr, "cp: %s: not a directory\n", dst);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[PATH_MAX];
        if (dst_dir)
            snprintf(target, sizeof(target), "%s/%s", dst, base(argv[i]));
        else
            strlcpy(target, dst, sizeof(target));
        rc |= copy_file(argv[i], target);
    }
    return rc;
}

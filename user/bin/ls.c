/* ls - list directory contents.  ls [-l] [-a] [path...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fresh.h>

static int opt_long, opt_all, color;

struct ent {
    char name[NAME_MAX + 3];
    struct fresh_stat st;
};

static int cmp(const void *a, const void *b) { return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name); }

static void fmt_time(uint64_t t, char *out, size_t n)
{
    struct tm_simple tm;
    unix_to_tm(t, &tm);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tm.year, tm.month, tm.day, tm.hour, tm.min);
}

static void print_list(struct ent *e, int n)
{
    if (opt_long) {
        for (int i = 0; i < n; i++) {
            char t[32];
            fmt_time(e[i].st.mtime, t, sizeof(t));
            int dir = e[i].st.type == FT_DIR;
            printf("%c %10llu  %s  %s%s%s\n", dir ? 'd' : '-', (unsigned long long)e[i].st.size, t,
                   dir && color ? "\033[1;34m" : "", e[i].name, dir && color ? "\033[0m/" : (dir ? "/" : ""));
        }
        return;
    }
    if (!isatty(1)) {
        for (int i = 0; i < n; i++)
            printf("%s\n", e[i].name);
        return;
    }
    int cols = 80;
    ttysize(NULL, &cols);
    size_t w = 1;
    for (int i = 0; i < n; i++)
        if (strlen(e[i].name) + 1 > w)
            w = strlen(e[i].name) + 1;
    int per = (int)((size_t)cols / (w + 2));
    if (per < 1)
        per = 1;
    int rows = (n + per - 1) / per;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < per; c++) {
            int i = c * rows + r;
            if (i >= n)
                continue;
            int dir = e[i].st.type == FT_DIR;
            size_t len = strlen(e[i].name) + (size_t)dir;
            printf("%s%s%s%s", dir && color ? "\033[1;34m" : "", e[i].name, dir && color ? "\033[0m" : "",
                   dir ? "/" : "");
            if (c + 1 < per && (c + 1) * rows + r < n)
                for (size_t k = len; k < w + 2; k++)
                    putchar(' ');
        }
        putchar('\n');
    }
}

static int list(const char *path, int show_header)
{
    struct fresh_stat st;
    int r = stat(path, &st);
    if (r < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(r));
        return 1;
    }
    if (st.type != FT_DIR) {
        struct ent e;
        strlcpy(e.name, path, sizeof(e.name));
        e.st = st;
        print_list(&e, 1);
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }
    int cap = 64, n = 0;
    struct ent *ents = malloc(sizeof(struct ent) * (size_t)cap);
    struct fresh_dirent d;
    while (readdir(fd, &d) == 1) {
        if (!opt_all && d.name[0] == '.')
            continue;
        if (n == cap) {
            cap *= 2;
            ents = realloc(ents, sizeof(struct ent) * (size_t)cap);
        }
        strlcpy(ents[n].name, d.name, sizeof(ents[n].name));
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, d.name);
        if (stat(full, &ents[n].st) < 0) {
            memset(&ents[n].st, 0, sizeof(ents[n].st));
            ents[n].st.type = d.type;
        }
        n++;
    }
    close(fd);
    qsort(ents, (size_t)n, sizeof(struct ent), cmp);
    if (show_header)
        printf("%s:\n", path);
    print_list(ents, n);
    free(ents);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1, rc = 0, npaths = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (char *p = argv[i] + 1; *p; p++) {
                if (*p == 'l')
                    opt_long = 1;
                else if (*p == 'a')
                    opt_all = 1;
                else {
                    fprintf(stderr, "usage: ls [-la] [path...]\n");
                    return 2;
                }
            }
        } else {
            npaths++;
        }
    }
    color = isatty(1);
    if (npaths == 0)
        return list(".", 0);
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        if (!first)
            putchar('\n');
        rc |= list(argv[i], npaths > 1);
        first = 0;
    }
    return rc;
}

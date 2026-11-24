/* grep - print lines containing a text.  grep [-i] [-v] [-n] [-c] TEXT [file...]
 * TEXT is matched literally; '^' and '$' anchor to line start/end. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fresh.h>

static int opt_i, opt_v, opt_n, opt_c;

static int match_at(const char *line, const char *pat, size_t plen)
{
    for (size_t k = 0; k < plen; k++) {
        char a = line[k], b = pat[k];
        if (!a)
            return 0;
        if (opt_i ? tolower((unsigned char)a) != tolower((unsigned char)b) : a != b)
            return 0;
    }
    return 1;
}

static int matches(const char *line, const char *pat)
{
    int anchor_start = pat[0] == '^';
    if (anchor_start)
        pat++;
    size_t plen = strlen(pat);
    int anchor_end = plen && pat[plen - 1] == '$';
    if (anchor_end)
        plen--;
    size_t llen = strlen(line);
    if (anchor_start) {
        if (!match_at(line, pat, plen))
            return 0;
        return !anchor_end || llen == plen;
    }
    if (anchor_end)
        return llen >= plen && match_at(line + llen - plen, pat, plen);
    for (size_t i = 0; i + plen <= llen; i++)
        if (match_at(line + i, pat, plen))
            return 1;
    return plen == 0;
}

static int grep(FILE *f, const char *pat, const char *name, int show_name)
{
    char line[4096];
    long lineno = 0, count = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n')
            line[--l] = 0;
        if (matches(line, pat) != opt_v) {
            count++;
            if (opt_c)
                continue;
            if (show_name)
                printf("%s:", name);
            if (opt_n)
                printf("%ld:", lineno);
            printf("%s\n", line);
        }
    }
    if (opt_c) {
        if (show_name)
            printf("%s:", name);
        printf("%ld\n", count);
    }
    return count > 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'i')
                opt_i = 1;
            else if (*p == 'v')
                opt_v = 1;
            else if (*p == 'n')
                opt_n = 1;
            else if (*p == 'c')
                opt_c = 1;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-ivnc] TEXT [file...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    int found = 0;
    if (i >= argc)
        return grep(stdin, pat, "stdin", 0) ? 0 : 1;
    int many = argc - i > 1;
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "grep: %s: cannot open\n", argv[i]);
            continue;
        }
        found |= grep(f, pat, argv[i], many);
        fclose(f);
    }
    return found ? 0 : 1;
}

/* sh - the Fresh OS shell.
 * Features: line editing with history and Tab completion, pipes (|),
 * redirection (< > >>), command lists (; && ||), background jobs (&),
 * quoting ('...' "..." \x), variables (NAME=value, $NAME, ${NAME}, $?, $$),
 * wildcards (* ? [..]), scripts (sh FILE, sh -c CMD) and the builtins
 * cd pwd exit set unset history source true false. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fresh.h>

#define MAX_LINE 1024
#define MAX_TOKENS 256
#define MAX_WORDS 256
#define HIST_MAX 64
#define MAX_VARS 64

enum { T_WORD, T_PIPE, T_LT, T_GT, T_GTGT, T_SEMI, T_AND, T_OR, T_AMP, T_END };

struct token {
    int type;
    char *text;
    int glob;
};

struct var {
    char name[32];
    char *value;
};

static int last_status;
static int interactive;
static char *history[HIST_MAX];
static int hist_count;
static struct var vars[MAX_VARS];
static int nvars;
static int should_exit = -1;

/* ------------------------------------------------------------ variables */
static const char *var_get(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            return vars[i].value;
    return NULL;
}

static int var_set(const char *name, const char *value)
{
    if (strlen(name) >= sizeof(vars[0].name))
        return -1;
    for (int i = 0; i < nvars; i++) {
        if (!strcmp(vars[i].name, name)) {
            free(vars[i].value);
            vars[i].value = strdup(value);
            return 0;
        }
    }
    if (nvars >= MAX_VARS)
        return -1;
    strcpy(vars[nvars].name, name);
    vars[nvars].value = strdup(value);
    nvars++;
    return 0;
}

static void var_unset(const char *name)
{
    for (int i = 0; i < nvars; i++) {
        if (!strcmp(vars[i].name, name)) {
            free(vars[i].value);
            vars[i] = vars[--nvars];
            return;
        }
    }
}

/* ------------------------------------------------------------ lexer */
struct sbuf {
    char *s;
    size_t len, cap;
};

static void sb_putc(struct sbuf *b, char c)
{
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->s = realloc(b->s, b->cap);
    }
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

static void sb_puts(struct sbuf *b, const char *s)
{
    while (*s)
        sb_putc(b, *s++);
}

static const char *expand_var(const char *p, struct sbuf *b)
{
    /* p points just after '$' */
    char name[32];
    int n = 0;
    char num[24];
    if (*p == '?') {
        snprintf(num, sizeof(num), "%d", last_status);
        sb_puts(b, num);
        return p + 1;
    }
    if (*p == '$') {
        snprintf(num, sizeof(num), "%d", getpid());
        sb_puts(b, num);
        return p + 1;
    }
    if (*p == '{') {
        const char *q = p + 1;
        while (*q && *q != '}' && n < 31)
            name[n++] = *q++;
        name[n] = 0;
        if (*q == '}') {
            const char *v = var_get(name);
            if (v)
                sb_puts(b, v);
            return q + 1;
        }
        sb_putc(b, '$');
        return p;
    }
    while ((isalnum((unsigned char)*p) || *p == '_') && n < 31)
        name[n++] = *p++;
    name[n] = 0;
    if (n == 0) {
        sb_putc(b, '$');
        return p;
    }
    const char *v = var_get(name);
    if (v)
        sb_puts(b, v);
    return p;
}

static int is_op_char(char c) { return c == '|' || c == '&' || c == ';' || c == '<' || c == '>'; }

static void free_tokens(struct token *t, int n)
{
    for (int i = 0; i < n; i++)
        free(t[i].text);
}

static int tokenize(const char *line, struct token *toks)
{
    int n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p || *p == '#')
            break;
        if (n >= MAX_TOKENS - 1) {
            fprintf(stderr, "sh: too many tokens\n");
            free_tokens(toks, n);
            return -1;
        }
        struct token *t = &toks[n];
        t->text = NULL;
        t->glob = 0;
        if (is_op_char(*p)) {
            if (p[0] == '|' && p[1] == '|') {
                t->type = T_OR;
                p += 2;
            } else if (p[0] == '|') {
                t->type = T_PIPE;
                p++;
            } else if (p[0] == '&' && p[1] == '&') {
                t->type = T_AND;
                p += 2;
            } else if (p[0] == '&') {
                t->type = T_AMP;
                p++;
            } else if (p[0] == ';') {
                t->type = T_SEMI;
                p++;
            } else if (p[0] == '<') {
                t->type = T_LT;
                p++;
            } else if (p[0] == '>' && p[1] == '>') {
                t->type = T_GTGT;
                p += 2;
            } else {
                t->type = T_GT;
                p++;
            }
            n++;
            continue;
        }
        struct sbuf b = {0, 0, 0};
        sb_putc(&b, 0);
        b.len = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && !is_op_char(*p)) {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'')
                    sb_putc(&b, *p++);
                if (*p != '\'') {
                    fprintf(stderr, "sh: unterminated quote\n");
                    free(b.s);
                    free_tokens(toks, n);
                    return -1;
                }
                p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        sb_putc(&b, p[1]);
                        p += 2;
                    } else if (*p == '$') {
                        p = expand_var(p + 1, &b);
                    } else {
                        sb_putc(&b, *p++);
                    }
                }
                if (*p != '"') {
                    fprintf(stderr, "sh: unterminated quote\n");
                    free(b.s);
                    free_tokens(toks, n);
                    return -1;
                }
                p++;
            } else if (*p == '\\' && p[1]) {
                sb_putc(&b, p[1]);
                p += 2;
            } else if (*p == '$') {
                p = expand_var(p + 1, &b);
            } else {
                if (*p == '*' || *p == '?' || *p == '[')
                    t->glob = 1;
                sb_putc(&b, *p++);
            }
        }
        t->type = T_WORD;
        t->text = b.s;
        n++;
    }
    toks[n].type = T_END;
    toks[n].text = NULL;
    return n;
}

/* ------------------------------------------------------------ globbing */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int glob_expand(const char *word, char **out, int max)
{
    char dir[PATH_MAX], prefix[PATH_MAX];
    const char *pat;
    const char *slash = strrchr(word, '/');
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0) {
            strcpy(dir, "/");
            strcpy(prefix, "/");
        } else {
            memcpy(dir, word, dl);
            dir[dl] = 0;
            snprintf(prefix, sizeof(prefix), "%s/", dir);
        }
        pat = slash + 1;
    } else {
        strcpy(dir, ".");
        prefix[0] = 0;
        pat = word;
    }
    if (strchr(dir, '*') || strchr(dir, '?'))
        return 0; /* only the last component may contain wildcards */
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = 0;
    struct fresh_dirent e;
    while (n < max && readdir(fd, &e) == 1) {
        if (e.name[0] == '.' && pat[0] != '.')
            continue;
        if (glob_match(pat, e.name)) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s%s", prefix, e.name);
            out[n++] = strdup(full);
        }
    }
    close(fd);
    qsort(out, (size_t)n, sizeof(char *), cmp_str);
    return n;
}

/* ------------------------------------------------------------ execution */
struct cmd {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in, *out;
    int append;
    char *owned[MAX_WORDS];
    int nowned;
};

static void cmd_free(struct cmd *c)
{
    for (int i = 0; i < c->nowned; i++)
        free(c->owned[i]);
    c->nowned = 0;
}

static int decode_status(int st)
{
    if (st & 0x200)
        return 128 + 11;
    if (st & 0x100)
        return 130;
    return st & 0xFF;
}

static int run_source(const char *path);
static int run_line(const char *line);

static int builtin(struct cmd *c, int outfd, int *handled)
{
    *handled = 1;
    const char *name = c->argv[0];
    if (!strcmp(name, "cd")) {
        const char *dir = c->argc > 1 ? c->argv[1] : var_get("HOME");
        if (!dir)
            dir = "/";
        int r = chdir(dir);
        if (r < 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(r));
            return 1;
        }
        return 0;
    }
    if (!strcmp(name, "pwd")) {
        char buf[PATH_MAX];
        getcwd(buf, sizeof(buf));
        dprintf(outfd, "%s\n", buf);
        return 0;
    }
    if (!strcmp(name, "exit")) {
        should_exit = c->argc > 1 ? atoi(c->argv[1]) : last_status;
        return should_exit;
    }
    if (!strcmp(name, "set")) {
        for (int i = 0; i < nvars; i++)
            dprintf(outfd, "%s=%s\n", vars[i].name, vars[i].value);
        return 0;
    }
    if (!strcmp(name, "unset")) {
        for (int i = 1; i < c->argc; i++)
            var_unset(c->argv[i]);
        return 0;
    }
    if (!strcmp(name, "history")) {
        for (int i = 0; i < hist_count; i++)
            dprintf(outfd, "%4d  %s\n", i + 1, history[i]);
        return 0;
    }
    if (!strcmp(name, "true"))
        return 0;
    if (!strcmp(name, "false"))
        return 1;
    if (!strcmp(name, "source") || !strcmp(name, ".")) {
        if (c->argc < 2) {
            fprintf(stderr, "source: file name required\n");
            return 1;
        }
        return run_source(c->argv[1]);
    }
    *handled = 0;
    return 0;
}

static int is_assignment(const char *w)
{
    const char *eq = strchr(w, '=');
    if (!eq || eq == w)
        return 0;
    for (const char *p = w; p < eq; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return 0;
    return !isdigit((unsigned char)w[0]);
}

/* parse one pipeline from toks[*pos]; returns number of commands or -1 */
static int parse_pipeline(struct token *toks, int *pos, struct cmd *cmds, int maxc)
{
    int nc = 0;
    int i = *pos;
    for (;;) {
        if (nc >= maxc) {
            fprintf(stderr, "sh: pipeline too long\n");
            return -1;
        }
        struct cmd *c = &cmds[nc];
        memset(c, 0, sizeof(*c));
        while (toks[i].type == T_WORD || toks[i].type == T_LT || toks[i].type == T_GT || toks[i].type == T_GTGT) {
            if (toks[i].type != T_WORD) {
                int t = toks[i].type;
                if (toks[i + 1].type != T_WORD) {
                    fprintf(stderr, "sh: syntax error: missing file name after redirection\n");
                    return -1;
                }
                if (t == T_LT)
                    c->in = toks[i + 1].text;
                else {
                    c->out = toks[i + 1].text;
                    c->append = t == T_GTGT;
                }
                i += 2;
                continue;
            }
            if (toks[i].glob) {
                char *matches[MAX_ARGS];
                int m = glob_expand(toks[i].text, matches, MAX_ARGS);
                if (m > 0) {
                    for (int k = 0; k < m; k++) {
                        if (c->argc >= MAX_ARGS || c->nowned >= MAX_WORDS) {
                            free(matches[k]);
                            continue;
                        }
                        c->argv[c->argc++] = matches[k];
                        c->owned[c->nowned++] = matches[k];
                    }
                    i++;
                    continue;
                }
            }
            if (c->argc >= MAX_ARGS) {
                fprintf(stderr, "sh: too many arguments\n");
                return -1;
            }
            c->argv[c->argc++] = toks[i].text;
            i++;
        }
        c->argv[c->argc] = NULL;
        nc++;
        if (c->argc == 0) {
            if (toks[i].type == T_PIPE || nc > 1 || c->in || c->out) {
                fprintf(stderr, "sh: syntax error near '%s'\n", toks[i].type == T_PIPE ? "|" : "newline");
                return -1;
            }
        }
        if (toks[i].type != T_PIPE)
            break;
        i++;
    }
    *pos = i;
    return nc;
}

static void reap_background(void)
{
    int st;
    int pid;
    while ((pid = wait_pid_opt(-1, &st, WAIT_NOHANG)) > 0)
        if (interactive)
            fprintf(stderr, "[%d] done (status %d)\n", pid, decode_status(st));
}

static int run_pipeline(struct cmd *cmds, int nc, int background)
{
    if (nc == 1 && cmds[0].argc == 0)
        return last_status;
    if (nc == 1 && cmds[0].argc == 1 && is_assignment(cmds[0].argv[0]) && !cmds[0].out && !cmds[0].in) {
        char *w = cmds[0].argv[0];
        char *eq = strchr(w, '=');
        *eq = 0;
        var_set(w, eq + 1);
        *eq = '=';
        return 0;
    }
    if (nc == 1 && !background) {
        int outfd = 1;
        if (cmds[0].out) {
            outfd = open(cmds[0].out, O_WRONLY | O_CREAT | (cmds[0].append ? O_APPEND : O_TRUNC));
            if (outfd < 0) {
                fprintf(stderr, "sh: %s: %s\n", cmds[0].out, strerror(outfd));
                return 1;
            }
        }
        int handled;
        fflush(stdout);
        int r = builtin(&cmds[0], outfd, &handled);
        if (outfd != 1)
            close(outfd);
        if (handled)
            return r;
    }

    int pipes[16][2];
    int pids[16];
    int npids = 0, pgid = 0, status = 0;
    int extra_fds[34];
    int nextra = 0;
    if (nc > 16) {
        fprintf(stderr, "sh: pipeline too long\n");
        return 1;
    }
    for (int k = 0; k < nc - 1; k++) {
        if (pipe(pipes[k]) < 0) {
            fprintf(stderr, "sh: cannot create pipe\n");
            for (int j = 0; j < k; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return 1;
        }
    }
    int failed = 0;
    for (int k = 0; k < nc; k++) {
        struct cmd *c = &cmds[k];
        int map[3] = {-1, -1, -1};
        if (k > 0)
            map[0] = pipes[k - 1][0];
        if (k < nc - 1)
            map[1] = pipes[k][1];
        if (c->in) {
            int fd = open(c->in, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->in, strerror(fd));
                failed = 1;
                break;
            }
            map[0] = fd;
            extra_fds[nextra++] = fd;
        }
        if (c->out) {
            int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC));
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->out, strerror(fd));
                failed = 1;
                break;
            }
            map[1] = fd;
            extra_fds[nextra++] = fd;
        }
        if (c->argc == 0)
            continue;
        fflush(stdout);
        int pid = spawn_cmd(c->argv[0], c->argv, map, interactive ? (pgid ? pgid : -1) : 0);
        if (pid < 0) {
            if (pid == -ENOENT)
                fprintf(stderr, "sh: %s: command not found\n", c->argv[0]);
            else
                fprintf(stderr, "sh: %s: %s\n", c->argv[0], strerror(pid));
            status = pid == -ENOENT ? 127 : 126;
            if (k == nc - 1)
                failed = 2;
            continue;
        }
        if (!pgid)
            pgid = pid;
        pids[npids++] = pid;
    }
    for (int k = 0; k < nc - 1; k++) {
        close(pipes[k][0]);
        close(pipes[k][1]);
    }
    for (int k = 0; k < nextra; k++)
        close(extra_fds[k]);
    if (background) {
        for (int k = 0; k < npids; k++)
            fprintf(stderr, "[%d]\n", pids[k]);
        return 0;
    }
    if (interactive && pgid) {
        ttymode(TTY_MODE_COOKED);
        ttyfg(pgid);
    }
    int last_st = 0;
    for (int k = 0; k < npids; k++) {
        int st = 0;
        if (wait_pid(pids[k], &st) == pids[k] && k == npids - 1)
            last_st = st;
    }
    if (interactive && pgid)
        ttyfg(0);
    if (failed == 1)
        return 1;
    if (failed == 2)
        return status;
    return decode_status(last_st);
}

static int run_tokens(struct token *toks)
{
    int pos = 0;
    int skip = 0;
    struct cmd *cmds = malloc(sizeof(struct cmd) * 16);
    if (!cmds)
        return 1;
    while (toks[pos].type != T_END && should_exit < 0) {
        int nc = parse_pipeline(toks, &pos, cmds, 16);
        if (nc < 0) {
            last_status = 2;
            break;
        }
        int sep = toks[pos].type;
        if (sep != T_END && sep != T_SEMI && sep != T_AMP && sep != T_AND && sep != T_OR) {
            fprintf(stderr, "sh: syntax error\n");
            last_status = 2;
            for (int k = 0; k < nc; k++)
                cmd_free(&cmds[k]);
            break;
        }
        if (!skip)
            last_status = run_pipeline(cmds, nc, sep == T_AMP);
        for (int k = 0; k < nc; k++)
            cmd_free(&cmds[k]);
        if (sep == T_AND)
            skip = last_status != 0;
        else if (sep == T_OR)
            skip = last_status == 0;
        else
            skip = 0;
        if (sep != T_END)
            pos++;
    }
    free(cmds);
    return last_status;
}

static int run_line(const char *line)
{
    struct token *toks = malloc(sizeof(struct token) * MAX_TOKENS);
    if (!toks)
        return 1;
    int n = tokenize(line, toks);
    if (n < 0) {
        free(toks);
        last_status = 2;
        return 2;
    }
    run_tokens(toks);
    free_tokens(toks, n);
    free(toks);
    return last_status;
}

static int run_file(FILE *f)
{
    char line[MAX_LINE];
    while (should_exit < 0 && fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n')
            line[l - 1] = 0;
        run_line(line);
    }
    return last_status;
}

static int run_source(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "sh: cannot open %s\n", path);
        return 1;
    }
    int saved = interactive;
    interactive = 0;
    run_file(f);
    interactive = saved;
    fclose(f);
    return last_status;
}

/* ------------------------------------------------------------ line editor */
static char prompt_buf[PATH_MAX + 64];

static void make_prompt(void)
{
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    snprintf(prompt_buf, sizeof(prompt_buf), "\033[1;32mfresh\033[0m:\033[1;34m%s\033[0m%s ", cwd,
             last_status ? "\033[31m$\033[0m" : "$");
}

static void out(const char *s, size_t n) { write(1, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }

static void redraw(const char *buf, int len, int pos)
{
    char seq[32];
    outs("\r");
    outs(prompt_buf);
    out(buf, (size_t)len);
    outs("\033[K");
    if (pos < len) {
        snprintf(seq, sizeof(seq), "\033[%dD", len - pos);
        outs(seq);
    }
}

static void complete(char *buf, int *len, int *pos)
{
    int start = *pos;
    while (start > 0 && buf[start - 1] != ' ')
        start--;
    char word[PATH_MAX];
    int wl = *pos - start;
    if (wl >= (int)sizeof(word))
        return;
    memcpy(word, buf + start, (size_t)wl);
    word[wl] = 0;
    int first_word = 1;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ')
            first_word = 0;
    char dir[PATH_MAX], base[PATH_MAX], prefix[PATH_MAX];
    char *slash = strrchr(word, '/');
    if (slash) {
        size_t dl = (size_t)(slash - word);
        if (dl == 0)
            strcpy(dir, "/");
        else {
            memcpy(dir, word, dl);
            dir[dl] = 0;
        }
        strcpy(base, slash + 1);
        memcpy(prefix, word, dl + 1);
        prefix[dl + 1] = 0;
    } else {
        strcpy(dir, first_word ? "/bin" : ".");
        strcpy(base, word);
        prefix[0] = 0;
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return;
    char *matches[128];
    int types[128];
    int n = 0;
    struct fresh_dirent e;
    size_t bl = strlen(base);
    while (n < 128 && readdir(fd, &e) == 1) {
        if (!strncmp(e.name, base, bl) && (base[0] == '.' || e.name[0] != '.')) {
            types[n] = (int)e.type;
            matches[n++] = strdup(e.name);
        }
    }
    close(fd);
    if (n == 0)
        return;
    size_t common = strlen(matches[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < common && matches[i][k] == matches[0][k])
            k++;
        common = k;
    }
    char add[PATH_MAX];
    size_t al = 0;
    for (size_t k = bl; k < common && al < sizeof(add) - 2; k++)
        add[al++] = matches[0][k];
    if (n == 1 && al < sizeof(add) - 2)
        add[al++] = types[0] == FT_DIR ? '/' : ' ';
    add[al] = 0;
    if (n > 1 && al == 0) {
        qsort(matches, (size_t)n, sizeof(char *), cmp_str);
        outs("\n");
        for (int i = 0; i < n; i++) {
            outs(matches[i]);
            outs(types[i] == FT_DIR ? "/  " : "  ");
        }
        outs("\n");
    }
    if (*len + (int)al < MAX_LINE - 1) {
        memmove(buf + *pos + al, buf + *pos, (size_t)(*len - *pos));
        memcpy(buf + *pos, add, al);
        *len += (int)al;
        *pos += (int)al;
    }
    for (int i = 0; i < n; i++)
        free(matches[i]);
    (void)prefix;
}

static void hist_add(const char *line)
{
    if (!line[0])
        return;
    if (hist_count && !strcmp(history[hist_count - 1], line))
        return;
    if (hist_count == HIST_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HIST_MAX - 1));
        hist_count--;
    }
    history[hist_count++] = strdup(line);
}

/* returns length, or -1 on end of input */
static int read_line(char *buf)
{
    int len = 0, pos = 0;
    int hidx = hist_count;
    char saved[MAX_LINE];
    saved[0] = 0;
    int esc = 0;
    char escbuf[8];
    int escn = 0;
    buf[0] = 0;
    ttymode(TTY_MODE_RAW);
    make_prompt();
    outs(prompt_buf);
    for (;;) {
        char in[32];
        long r = read(0, in, sizeof(in));
        if (r <= 0)
            return -1;
        for (long i = 0; i < r; i++) {
            char c = in[i];
            if (esc == 1) {
                esc = c == '[' || c == 'O' ? 2 : 0;
                escn = 0;
                continue;
            }
            if (esc == 2) {
                if (escn < 7)
                    escbuf[escn++] = c;
                if (!((c >= 0x40 && c <= 0x7E)))
                    continue;
                esc = 0;
                escbuf[escn] = 0;
                if (c == 'A' || c == 'B') {
                    if (c == 'A' && hidx > 0) {
                        if (hidx == hist_count)
                            strcpy(saved, buf);
                        hidx--;
                    } else if (c == 'B' && hidx < hist_count) {
                        hidx++;
                    } else {
                        continue;
                    }
                    const char *src = hidx == hist_count ? saved : history[hidx];
                    strlcpy(buf, src, MAX_LINE);
                    len = pos = (int)strlen(buf);
                } else if (c == 'C') {
                    if (pos < len)
                        pos++;
                } else if (c == 'D') {
                    if (pos > 0)
                        pos--;
                } else if (c == 'H') {
                    pos = 0;
                } else if (c == 'F') {
                    pos = len;
                } else if (c == '~' && escbuf[0] == '3') {
                    if (pos < len) {
                        memmove(buf + pos, buf + pos + 1, (size_t)(len - pos));
                        len--;
                    }
                }
                redraw(buf, len, pos);
                continue;
            }
            switch (c) {
            case 0x1B:
                esc = 1;
                break;
            case '\n':
            case '\r':
                buf[len] = 0;
                outs("\n");
                ttymode(TTY_MODE_COOKED);
                return len;
            case 3: /* Ctrl+C */
                outs("^C\n");
                len = pos = 0;
                buf[0] = 0;
                hidx = hist_count;
                last_status = 130;
                make_prompt();
                outs(prompt_buf);
                break;
            case 4: /* Ctrl+D */
                if (len == 0) {
                    outs("\n");
                    ttymode(TTY_MODE_COOKED);
                    return -1;
                }
                if (pos < len) {
                    memmove(buf + pos, buf + pos + 1, (size_t)(len - pos));
                    len--;
                    redraw(buf, len, pos);
                }
                break;
            case 1: /* Ctrl+A */
                pos = 0;
                redraw(buf, len, pos);
                break;
            case 5: /* Ctrl+E */
                pos = len;
                redraw(buf, len, pos);
                break;
            case 12: /* Ctrl+L */
                outs("\033[2J\033[H");
                redraw(buf, len, pos);
                break;
            case 21: /* Ctrl+U */
                memmove(buf, buf + pos, (size_t)(len - pos));
                len -= pos;
                pos = 0;
                buf[len] = 0;
                redraw(buf, len, pos);
                break;
            case '\t':
                buf[len] = 0;
                complete(buf, &len, &pos);
                buf[len] = 0;
                redraw(buf, len, pos);
                break;
            case 0x7F:
            case '\b':
                if (pos > 0) {
                    memmove(buf + pos - 1, buf + pos, (size_t)(len - pos));
                    pos--;
                    len--;
                    buf[len] = 0;
                    redraw(buf, len, pos);
                }
                break;
            default:
                if ((unsigned char)c < 32 || len >= MAX_LINE - 1)
                    break;
                memmove(buf + pos + 1, buf + pos, (size_t)(len - pos));
                buf[pos++] = c;
                len++;
                buf[len] = 0;
                if (pos == len)
                    out(&c, 1);
                else
                    redraw(buf, len, pos);
                break;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        run_line(argv[2]);
        return should_exit >= 0 ? should_exit : last_status;
    }
    if (argc >= 2) {
        char num[16];
        for (int i = 1; i < argc && i < 10; i++) {
            snprintf(num, sizeof(num), "%d", i - 1);
            var_set(num, argv[i]);
        }
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "sh: cannot open %s\n", argv[1]);
            return 127;
        }
        run_file(f);
        fclose(f);
        return should_exit >= 0 ? should_exit : last_status;
    }
    interactive = isatty(0);
    var_set("HOME", "/data");
    if (!interactive) {
        run_file(stdin);
        return should_exit >= 0 ? should_exit : last_status;
    }
    char *line = malloc(MAX_LINE);
    while (should_exit < 0) {
        reap_background();
        int n = read_line(line);
        if (n < 0)
            break;
        hist_add(line);
        run_line(line);
    }
    ttymode(TTY_MODE_COOKED);
    return should_exit >= 0 ? should_exit : last_status;
}

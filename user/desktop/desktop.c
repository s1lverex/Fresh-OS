/* desktop - the Fresh OS graphical desktop.
 *
 * Takes over the screen, keyboard and mouse (SYS_FB_ACQUIRE) and provides
 * movable windows, a taskbar with a start menu and clock, desktop icons and
 * four applications: Terminal (a real shell on a pseudo-terminal), Files,
 * System Monitor and About. Keyboard input goes to the focused window.
 * Shortcuts: Alt+Tab next window, Alt+F4 close window, Ctrl+Alt+T terminal.
 * "Exit to console" in the start menu returns to the text console.
 * Actions are logged to /tmp/desktop.log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fresh.h>
#include "gfx.h"
#include "term.h"

#define MAXWIN 16
#define TASKBAR_H 34
#define TITLE_H 26
#define PAD 4
#define TERM_COLS 80
#define TERM_ROWS 25
#define MAX_ENTS 256
#define DOUBLE_CLICK_MS 450

/* colours */
#define C_WALL_TOP 0x0B1A2A
#define C_WALL_BOT 0x0F3B3A
#define C_TASKBAR 0x111820
#define C_TASKLINE 0x2A3644
#define C_ACCENT 0x1F7A5C
#define C_ACCENT_HI 0x7EE2A8
#define C_WIN_BG 0x161B22
#define C_WIN_BORDER 0x3A4656
#define C_TITLE_FOCUS 0x1F7A5C
#define C_TITLE_BLUR 0x2A3340
#define C_TEXT 0xE6EDF3
#define C_TEXT_DIM 0x9AA4B2
#define C_CLOSE 0xC9443E
#define C_SELECT 0x24405A
#define C_MENU_BG 0x1A222C

enum { APP_TERM, APP_FILES, APP_MONITOR, APP_ABOUT };

struct fent {
    char name[NAME_MAX + 3];
    uint32_t type;
    uint64_t size;
};

typedef struct window {
    int used, kind;
    char title[64];
    int x, y, w, h;
    /* terminal */
    int master, pid, exited, autoclose;
    term_t term;
    uint16_t cells[TERM_COLS * TERM_ROWS];
    /* files */
    char path[PATH_MAX];
    struct fent *ents;
    int nents, sel, scroll;
    uint32_t list_hash;
    /* system monitor */
    struct fresh_sysinfo si;
    struct fresh_procinfo procs[64];
    int nprocs;
} window_t;

static window_t wins[MAXWIN];
static int zorder[MAXWIN], nz; /* zorder[nz-1] is the top window */
static int focused = -1;
static int mx, my, mbuttons;
static int drag_win = -1, drag_dx, drag_dy, drag_moved;
static int menu_open;
static int scene_dirty = 1;
static int icon_sel = -1;
static int last_click_target = -1;
static uint64_t last_click_ms;
static char clock_text[16];
static int reap_pids[64], nreap;
static uint32_t pal[16];

static const uint32_t palette_rgb[16] = {
    0x0E1116, 0xE5534B, 0x57C785, 0xE3B341, 0x4F8FE6, 0xB07CE8, 0x39C5CF, 0xCDD5DF,
    0x636E7B, 0xFF6F66, 0x7EE2A8, 0xF2CC60, 0x79AEF2, 0xCB9DF5, 0x6FDDE5, 0xF5F7FA,
};

/* ------------------------------------------------------------ helpers */
static void dlog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    int fd = open("/tmp/desktop.log", O_WRONLY | O_CREAT | O_APPEND);
    if (fd >= 0) {
        write(fd, buf, (size_t)n);
        close(fd);
    }
}

static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

static rect_t client_rect(window_t *w)
{
    rect_t r = {w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1};
    return r;
}

static int top_window(void) { return nz ? zorder[nz - 1] : -1; }

static void raise_window(int id)
{
    int k = -1;
    for (int i = 0; i < nz; i++)
        if (zorder[i] == id)
            k = i;
    if (k < 0)
        return;
    memmove(&zorder[k], &zorder[k + 1], sizeof(int) * (size_t)(nz - k - 1));
    zorder[nz - 1] = id;
    if (focused != id)
        dlog("focus %s", wins[id].title);
    focused = id;
    scene_dirty = 1;
}

static void remove_z(int id)
{
    int k = -1;
    for (int i = 0; i < nz; i++)
        if (zorder[i] == id)
            k = i;
    if (k < 0)
        return;
    memmove(&zorder[k], &zorder[k + 1], sizeof(int) * (size_t)(nz - k - 1));
    nz--;
    focused = top_window();
}

static int alloc_window(int kind, const char *title, int w, int h)
{
    for (int i = 0; i < MAXWIN; i++) {
        if (wins[i].used)
            continue;
        window_t *win = &wins[i];
        memset(win, 0, sizeof(*win));
        win->used = 1;
        win->kind = kind;
        win->master = -1;
        strlcpy(win->title, title, sizeof(win->title));
        win->w = w;
        win->h = h;
        /* cascade new windows */
        int n = nz;
        win->x = 150 + (n % 6) * 34;
        win->y = 40 + (n % 6) * 30;
        if (win->x + w > scr_w)
            win->x = scr_w - w > 0 ? scr_w - w : 0;
        if (win->y + h > scr_h - TASKBAR_H)
            win->y = scr_h - TASKBAR_H - h > 0 ? scr_h - TASKBAR_H - h : 0;
        zorder[nz++] = i;
        focused = i;
        scene_dirty = 1;
        dlog("opened %s", title);
        return i;
    }
    return -1;
}

static void close_window(int id)
{
    window_t *w = &wins[id];
    if (!w->used)
        return;
    if (w->kind == APP_TERM) {
        if (w->master >= 0)
            close(w->master); /* hang up: the shell sees end of input */
        if (w->pid > 0 && !w->exited) {
            kill(w->pid);
            if (nreap < 64)
                reap_pids[nreap++] = w->pid;
        }
    }
    free(w->ents);
    dlog("closed %s", w->title);
    w->used = 0;
    if (drag_win == id)
        drag_win = -1;
    remove_z(id);
    scene_dirty = 1;
}

/* ------------------------------------------------------------ terminal app */
static int open_terminal(char *const argv[], int autoclose, const char *title)
{
    int w = TERM_COLS * 8 + 2 * PAD + 2, h = TITLE_H + TERM_ROWS * 16 + 2 * PAD + 1;
    int id = alloc_window(APP_TERM, title, w, h);
    if (id < 0)
        return -1;
    window_t *win = &wins[id];
    term_init(&win->term, TERM_COLS, TERM_ROWS, win->cells);
    win->autoclose = autoclose;
    int fds[2];
    if (pty_open(fds) < 0) {
        term_write(&win->term, "desktop: cannot open a terminal\n", 32);
        win->exited = 1;
        return id;
    }
    win->master = fds[0];
    pty_resize(fds[0], TERM_ROWS, TERM_COLS);
    int map[3] = {fds[1], fds[1], fds[1]};
    char *sh_argv[] = {"sh", NULL};
    char *const *av = argv ? argv : sh_argv;
    win->pid = spawn_cmd(av[0], av, map, -1);
    close(fds[1]);
    if (win->pid < 0) {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "cannot start %s: %s\n", av[0], strerror(win->pid));
        term_write(&win->term, msg, (size_t)n);
        win->exited = 1;
    }
    return id;
}

static void term_key(window_t *w, const struct fresh_input_event *e)
{
    if (w->exited) {
        close_window((int)(w - wins));
        return;
    }
    if (w->master < 0)
        return;
    const char *seq = NULL;
    switch (e->key) {
    case KEY_UP: seq = "\033[A"; break;
    case KEY_DOWN: seq = "\033[B"; break;
    case KEY_RIGHT: seq = "\033[C"; break;
    case KEY_LEFT: seq = "\033[D"; break;
    case KEY_HOME: seq = "\033[H"; break;
    case KEY_END: seq = "\033[F"; break;
    case KEY_PGUP: seq = "\033[5~"; break;
    case KEY_PGDN: seq = "\033[6~"; break;
    case KEY_DELETE: seq = "\033[3~"; break;
    case KEY_INSERT: seq = "\033[2~"; break;
    default: break;
    }
    if (seq) {
        write(w->master, seq, strlen(seq));
        return;
    }
    if (e->ch) {
        char c = e->ch == '\n' ? '\r' : (char)e->ch;
        write(w->master, &c, 1);
    }
}

static void term_tick(int id)
{
    window_t *w = &wins[id];
    if (w->master >= 0) {
        char buf[2000]; /* < 2 KiB: the kernel's bounce buffer stays small */
        for (int i = 0; i < 32; i++) {
            long n = read(w->master, buf, sizeof(buf));
            if (n <= 0)
                break;
            term_write(&w->term, buf, (size_t)n);
        }
        if (w->term.dirty) {
            w->term.dirty = 0;
            scene_dirty = 1;
        }
    }
    if (w->pid > 0 && !w->exited) {
        int st;
        if (wait_pid_opt(w->pid, &st, WAIT_NOHANG) == w->pid) {
            w->exited = 1;
            dlog("%s: program exited (status %d)", w->title, st);
            if (w->autoclose) {
                close_window(id);
                return;
            }
            term_write(&w->term, "\n[process exited - press a key to close]", 41);
            scene_dirty = 1;
        }
    }
}

static void term_draw(window_t *w, rect_t c)
{
    term_t *t = &w->term;
    int ox = c.x + PAD, oy = c.y + PAD;
    int is_focus = focused == (int)(w - wins);
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols; x++) {
            uint16_t cell = term_cell(t, x, y);
            uint8_t a = (uint8_t)(cell >> 8);
            uint32_t fg = pal[a & 15], bg = pal[a >> 4];
            int cx = t->cx < t->cols ? t->cx : t->cols - 1;
            if (is_focus && t->cursor_on && !w->exited && x == cx && y == t->cy) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
                if (fg == bg)
                    bg = pal[7];
            }
            glyph_cell(ox + x * 8, oy + y * 16, (unsigned char)(cell & 0xFF), fg, bg);
        }
    }
}

/* ------------------------------------------------------------ files app */
static int ent_cmp(const void *a, const void *b)
{
    const struct fent *x = a, *y = b;
    if (x->type != y->type)
        return x->type == FT_DIR ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void files_load(window_t *w)
{
    if (!w->ents)
        w->ents = malloc(sizeof(struct fent) * MAX_ENTS);
    if (!w->ents)
        return;
    int fd = open(w->path, O_RDONLY);
    int n = 0;
    uint32_t hash = 2166136261u;
    struct fresh_dirent d;
    while (fd >= 0 && n < MAX_ENTS && readdir(fd, &d) == 1) {
        strlcpy(w->ents[n].name, d.name, sizeof(w->ents[n].name));
        w->ents[n].type = d.type;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", strcmp(w->path, "/") ? w->path : "", d.name);
        struct fresh_stat st;
        w->ents[n].size = stat(full, &st) == 0 ? st.size : 0;
        for (const char *p = d.name; *p; p++)
            hash = (hash ^ (uint8_t)*p) * 16777619u;
        hash = (hash ^ (uint32_t)w->ents[n].size) * 16777619u;
        n++;
    }
    if (fd >= 0)
        close(fd);
    qsort(w->ents, (size_t)n, sizeof(struct fent), ent_cmp);
    if (hash != w->list_hash || n != w->nents) {
        w->nents = n;
        w->list_hash = hash;
        if (w->sel >= n)
            w->sel = n ? n - 1 : 0;
        scene_dirty = 1;
    }
}

static void open_files(const char *path)
{
    int id = alloc_window(APP_FILES, "Files", 460, 420);
    if (id < 0)
        return;
    strlcpy(wins[id].path, path, sizeof(wins[id].path));
    files_load(&wins[id]);
}

static void files_open_entry(window_t *w, int i)
{
    if (i < 0 || i >= w->nents)
        return;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", strcmp(w->path, "/") ? w->path : "", w->ents[i].name);
    if (w->ents[i].type == FT_DIR) {
        strlcpy(w->path, full, sizeof(w->path));
        w->sel = w->scroll = 0;
        w->list_hash = 0;
        files_load(w);
        dlog("files: entered %s", w->path);
    } else {
        char title[64];
        snprintf(title, sizeof(title), "Edit: %s", w->ents[i].name);
        char *argv[] = {"edit", full, NULL};
        open_terminal(argv, 1, title);
    }
    scene_dirty = 1;
}

static void files_up(window_t *w)
{
    char *s = strrchr(w->path, '/');
    if (!s)
        return;
    if (s == w->path)
        w->path[1] = 0;
    else
        *s = 0;
    w->sel = w->scroll = 0;
    w->list_hash = 0;
    files_load(w);
    scene_dirty = 1;
}

#define FILES_TOOLBAR 34
#define FILES_ROW 20

static int files_visible(window_t *w) { return (client_rect(w).h - FILES_TOOLBAR - 4) / FILES_ROW; }

static void files_draw(window_t *w, rect_t c)
{
    fill(c.x, c.y, c.w, FILES_TOOLBAR, 0x1C232D);
    fill(c.x + 6, c.y + 6, 50, 22, 0x2A3644);
    text(c.x + 15, c.y + 9, "Up", C_TEXT);
    text_n(c.x + 66, c.y + 9, w->path, (c.w - 72) / 8, C_TEXT_DIM);
    int vis = files_visible(w);
    if (w->sel < w->scroll)
        w->scroll = w->sel;
    if (w->sel >= w->scroll + vis)
        w->scroll = w->sel - vis + 1;
    for (int r = 0; r < vis && w->scroll + r < w->nents; r++) {
        struct fent *e = &w->ents[w->scroll + r];
        int y = c.y + FILES_TOOLBAR + 2 + r * FILES_ROW;
        if (w->scroll + r == w->sel)
            fill(c.x + 2, y, c.w - 4, FILES_ROW, C_SELECT);
        if (e->type == FT_DIR) {
            fill(c.x + 10, y + 5, 7, 3, 0xE3B341);
            fill(c.x + 10, y + 7, 16, 10, 0xE3B341);
        } else {
            fill(c.x + 12, y + 3, 12, 15, 0xCDD5DF);
            fill(c.x + 14, y + 7, 8, 1, 0x636E7B);
            fill(c.x + 14, y + 10, 8, 1, 0x636E7B);
            fill(c.x + 14, y + 13, 8, 1, 0x636E7B);
        }
        text_n(c.x + 34, y + 2, e->name, (c.w - 130) / 8, C_TEXT);
        if (e->type != FT_DIR) {
            char sz[24];
            if (e->size >= 1024 * 1024)
                snprintf(sz, sizeof(sz), "%llu MB", (unsigned long long)(e->size >> 20));
            else if (e->size >= 1024)
                snprintf(sz, sizeof(sz), "%llu KB", (unsigned long long)(e->size >> 10));
            else
                snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)e->size);
            text(c.x + c.w - 10 - (int)strlen(sz) * 8, y + 2, sz, C_TEXT_DIM);
        }
    }
    if (w->nents == 0)
        text(c.x + 14, c.y + FILES_TOOLBAR + 8, "(empty folder)", C_TEXT_DIM);
}

static void files_click(window_t *w, int lx, int ly, int dbl)
{
    if (ly < FILES_TOOLBAR) {
        if (inside(lx, ly, 6, 6, 50, 22))
            files_up(w);
        return;
    }
    int i = w->scroll + (ly - FILES_TOOLBAR - 2) / FILES_ROW;
    if (i < 0 || i >= w->nents)
        return;
    w->sel = i;
    scene_dirty = 1;
    if (dbl)
        files_open_entry(w, i);
}

static void files_key(window_t *w, const struct fresh_input_event *e)
{
    if (e->key == KEY_UP && w->sel > 0)
        w->sel--;
    else if (e->key == KEY_DOWN && w->sel + 1 < w->nents)
        w->sel++;
    else if (e->key == '\n')
        files_open_entry(w, w->sel);
    else if (e->key == 0x7F)
        files_up(w);
    scene_dirty = 1;
}

/* ------------------------------------------------------------ monitor app */
static void monitor_load(window_t *w)
{
    sysinfo(&w->si);
    w->nprocs = 0;
    while (w->nprocs < 64 && procinfo(w->nprocs, &w->procs[w->nprocs]) == 1)
        w->nprocs++;
    scene_dirty = 1;
}

static void open_monitor(void)
{
    int id = alloc_window(APP_MONITOR, "System Monitor", 470, 430);
    if (id >= 0)
        monitor_load(&wins[id]);
}

static void monitor_draw(window_t *w, rect_t c)
{
    struct fresh_sysinfo *s = &w->si;
    char line[128];
    int x = c.x + 12, y = c.y + 10;
    snprintf(line, sizeof(line), "Fresh OS %s on %s", s->version, s->boot_type == 2 ? "UEFI" : "BIOS");
    text(x, y, line, C_TEXT);
    y += 20;
    text_n(x, y, s->cpu, (c.w - 24) / 8, C_TEXT_DIM);
    y += 26;
    uint64_t used = s->mem_total - s->mem_free;
    snprintf(line, sizeof(line), "Memory: %llu MB used of %llu MB", (unsigned long long)(used >> 20),
             (unsigned long long)(s->mem_total >> 20));
    text(x, y, line, C_TEXT);
    y += 20;
    int bw = c.w - 24;
    fill(x, y, bw, 14, 0x2A3340);
    int uw = s->mem_total ? (int)((uint64_t)bw * used / s->mem_total) : 0;
    fill(x, y, uw < 2 ? 2 : uw, 14, C_ACCENT);
    y += 24;
    uint64_t sec = s->uptime_ms / 1000;
    snprintf(line, sizeof(line), "Uptime: %llu:%02llu:%02llu    Processes: %u", (unsigned long long)(sec / 3600),
             (unsigned long long)(sec / 60 % 60), (unsigned long long)(sec % 60), s->nprocs);
    text(x, y, line, C_TEXT);
    y += 28;
    text(x, y, "  PID  NAME              STATE    MEMORY", C_ACCENT_HI);
    y += 20;
    static const char *states[] = {"?", "ready", "run", "wait", "zombie"};
    for (int i = 0; i < w->nprocs && y + 16 < c.y + c.h; i++, y += 18) {
        struct fresh_procinfo *p = &w->procs[i];
        snprintf(line, sizeof(line), "%5d  %-16.16s  %-7s %6llu KB", p->pid, p->name, states[p->state < 5 ? p->state : 0],
                 (unsigned long long)(p->mem_bytes >> 10));
        text(x, y, line, C_TEXT);
    }
}

/* ------------------------------------------------------------ about app */
static void open_about(void) { alloc_window(APP_ABOUT, "About Fresh OS", 480, 330); }

static void about_draw(window_t *w, rect_t c)
{
    (void)w;
    const char *logo = "Fresh OS";
    int scale = 4, lw = (int)strlen(logo) * 8 * scale;
    int lx = c.x + (c.w - lw) / 2;
    for (int i = 0; logo[i]; i++)
        glyph_scaled(lx + i * 8 * scale, c.y + 18, (unsigned char)logo[i], scale, C_ACCENT_HI);
    static const char *lines[] = {
        ("Version " FRESH_VERSION),
        "",
        "A new operating system for 64-bit PCs,",
        "written from scratch: boot loaders, kernel,",
        "the FreshFS file system, C library, shell,",
        "programs and this desktop.",
        "",
        "Your files live in /data and are saved to disk.",
    };
    int y = c.y + 18 + 16 * scale + 20;
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += 18)
        text(c.x + (c.w - (int)strlen(lines[i]) * 8) / 2, y, lines[i], i == 0 ? C_TEXT : C_TEXT_DIM);
}

/* ------------------------------------------------------------ start menu */
enum { M_TERM, M_FILES, M_MONITOR, M_EDITOR, M_ABOUT, M_EXIT, M_RESTART, M_SHUTDOWN, M_COUNT };
static const char *menu_items[M_COUNT] = {"Terminal", "Files", "System Monitor", "Text Editor", "About Fresh OS",
                                          "Exit to console", "Restart", "Shut down"};
#define MENU_W 230
#define MENU_ITEM_H 28
#define MENU_SEP 10 /* gap before "Exit to console" */

static rect_t menu_rect(void)
{
    int h = M_COUNT * MENU_ITEM_H + MENU_SEP + 12;
    rect_t r = {4, scr_h - TASKBAR_H - 4 - h, MENU_W, h};
    return r;
}

static int menu_item_y(int i)
{
    rect_t r = menu_rect();
    return r.y + 6 + i * MENU_ITEM_H + (i >= M_EXIT ? MENU_SEP : 0);
}

static void quit_to_console(int code)
{
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used)
            close_window(i);
    dlog("exit to console");
    fb_release();
    exit(code);
}

static void menu_action(int item)
{
    menu_open = 0;
    scene_dirty = 1;
    dlog("menu: %s", menu_items[item]);
    switch (item) {
    case M_TERM: open_terminal(NULL, 0, "Terminal"); break;
    case M_FILES: open_files("/data"); break;
    case M_MONITOR: open_monitor(); break;
    case M_EDITOR: {
        char *argv[] = {"edit", "/data/notes.txt", NULL};
        open_terminal(argv, 1, "Text Editor");
        break;
    }
    case M_ABOUT: open_about(); break;
    case M_EXIT: quit_to_console(0); break;
    case M_RESTART: reboot(); break;
    case M_SHUTDOWN: poweroff(); break;
    default: break;
    }
}

/* ------------------------------------------------------------ desktop icons */
struct icon {
    const char *label;
    int item;
};
static const struct icon icons[] = {{"Terminal", M_TERM}, {"Files", M_FILES}, {"Monitor", M_MONITOR},
                                    {"About", M_ABOUT}};
#define NICONS ((int)(sizeof(icons) / sizeof(icons[0])))
#define ICON_X 22
#define ICON_Y 20
#define ICON_STEP 92
#define ICON_W 84
#define ICON_H 80

static void draw_icon_art(int i, int x, int y)
{
    switch (icons[i].item) {
    case M_TERM:
        fill(x, y, 48, 38, 0x0E1116);
        frame(x, y, 48, 38, 0x7EE2A8);
        text(x + 6, y + 10, ">_", 0x7EE2A8);
        break;
    case M_FILES:
        fill(x, y + 4, 18, 6, 0xE3B341);
        fill(x, y + 8, 48, 30, 0xE3B341);
        fill(x + 2, y + 14, 44, 22, 0xF2CC60);
        break;
    case M_MONITOR:
        fill(x, y, 48, 38, 0x1A222C);
        frame(x, y, 48, 38, 0x79AEF2);
        fill(x + 8, y + 24, 6, 10, 0x57C785);
        fill(x + 18, y + 14, 6, 20, 0x79AEF2);
        fill(x + 28, y + 18, 6, 16, 0xE3B341);
        fill(x + 38, y + 8, 4, 26, 0xE5534B);
        break;
    default:
        fill(x + 4, y, 40, 38, 0x1F7A5C);
        glyph_scaled(x + 16, y + 3, 'i', 2, 0xFFFFFF);
        break;
    }
}

static void draw_icons(void)
{
    for (int i = 0; i < NICONS; i++) {
        int x = ICON_X, y = ICON_Y + i * ICON_STEP;
        if (icon_sel == i)
            fill(x, y, ICON_W, ICON_H, 0x2A4E66);
        draw_icon_art(i, x + (ICON_W - 48) / 2, y + 8);
        int lw = (int)strlen(icons[i].label) * 8;
        text(x + (ICON_W - lw) / 2, y + 56, icons[i].label, C_TEXT);
    }
}

/* ------------------------------------------------------------ drawing */
static void draw_wallpaper(void)
{
    gradient_v(0, 0, scr_w, scr_h - TASKBAR_H, C_WALL_TOP, C_WALL_BOT);
    const char *mark = "Fresh OS";
    int scale = 6, w = (int)strlen(mark) * 8 * scale;
    for (int i = 0; mark[i]; i++)
        glyph_scaled(scr_w - w - 40 + i * 8 * scale, scr_h - TASKBAR_H - 16 * scale - 30, (unsigned char)mark[i],
                     scale, 0x16504A);
    save_wallpaper();
}

static void draw_window(int id)
{
    window_t *w = &wins[id];
    int focus = id == focused;
    fill(w->x + 5, w->y + 5, w->w, w->h, 0x050A0E); /* shadow */
    fill(w->x, w->y, w->w, w->h, C_WIN_BG);
    frame(w->x, w->y, w->w, w->h, focus ? C_ACCENT : C_WIN_BORDER);
    fill(w->x + 1, w->y + 1, w->w - 2, TITLE_H - 1, focus ? C_TITLE_FOCUS : C_TITLE_BLUR);
    text_n(w->x + 10, w->y + 5, w->title, (w->w - 50) / 8, focus ? 0xFFFFFF : C_TEXT_DIM);
    int bx = w->x + w->w - 24, by = w->y + 4;
    fill(bx, by, 18, 18, focus ? C_CLOSE : 0x4A5463);
    text(bx + 5, by + 1, "x", 0xFFFFFF);
    rect_t c = client_rect(w);
    gfx_clip(c);
    fill(c.x, c.y, c.w, c.h, w->kind == APP_TERM ? palette_rgb[0] : C_WIN_BG);
    switch (w->kind) {
    case APP_TERM: term_draw(w, c); break;
    case APP_FILES: files_draw(w, c); break;
    case APP_MONITOR: monitor_draw(w, c); break;
    default: about_draw(w, c); break;
    }
    gfx_noclip();
}

#define START_W 96
#define TASK_BTN_W 150

static void draw_taskbar(void)
{
    int y = scr_h - TASKBAR_H;
    fill(0, y, scr_w, TASKBAR_H, C_TASKBAR);
    fill(0, y, scr_w, 1, C_TASKLINE);
    fill(4, y + 4, START_W - 8, TASKBAR_H - 8, menu_open ? C_ACCENT_HI : C_ACCENT);
    fill(12, y + 11, 12, 12, menu_open ? C_ACCENT : C_ACCENT_HI);
    text(30, y + 9, "Fresh", menu_open ? 0x0B1A2A : 0xFFFFFF);
    int x = START_W + 6; /* window buttons in creation order */
    for (int id = 0; id < MAXWIN && x + TASK_BTN_W < scr_w - 220; id++) {
        if (!wins[id].used)
            continue;
        fill(x, y + 4, TASK_BTN_W - 6, TASKBAR_H - 8, id == focused ? 0x2A4E66 : 0x1C2530);
        text_n(x + 8, y + 9, wins[id].title, (TASK_BTN_W - 20) / 8, id == focused ? 0xFFFFFF : C_TEXT_DIM);
        x += TASK_BTN_W;
    }
    struct fresh_sysinfo si;
    char mem[32];
    if (sysinfo(&si) == 0) {
        snprintf(mem, sizeof(mem), "%llu MB free", (unsigned long long)(si.mem_free >> 20));
        text(scr_w - 90 - (int)strlen(mem) * 8, y + 9, mem, C_TEXT_DIM);
    }
    text(scr_w - 8 - (int)strlen(clock_text) * 8, y + 9, clock_text, C_TEXT);
}

static void draw_menu(void)
{
    rect_t r = menu_rect();
    fill(r.x + 4, r.y + 4, r.w, r.h, 0x050A0E);
    fill(r.x, r.y, r.w, r.h, C_MENU_BG);
    frame(r.x, r.y, r.w, r.h, C_WIN_BORDER);
    for (int i = 0; i < M_COUNT; i++) {
        int y = menu_item_y(i);
        if (inside(mx, my, r.x, y, r.w, MENU_ITEM_H))
            fill(r.x + 2, y, r.w - 4, MENU_ITEM_H, C_SELECT);
        if (i == M_EXIT)
            fill(r.x + 10, y - MENU_SEP / 2 - 1, r.w - 20, 1, C_WIN_BORDER);
        text(r.x + 16, y + 6, menu_items[i], i >= M_EXIT ? C_TEXT_DIM : C_TEXT);
    }
}

static void render(void)
{
    restore_wallpaper();
    draw_icons();
    for (int i = 0; i < nz; i++)
        draw_window(zorder[i]);
    draw_taskbar();
    if (menu_open)
        draw_menu();
    present_all();
    cursor_draw(mx, my);
    scene_dirty = 0;
}

/* ------------------------------------------------------------ input */
static int window_at(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--) {
        window_t *w = &wins[zorder[i]];
        if (inside(x, y, w->x, w->y, w->w, w->h))
            return zorder[i];
    }
    return -1;
}

static int is_double(int target)
{
    uint64_t now = uptime_ms();
    int dbl = target == last_click_target && now - last_click_ms < DOUBLE_CLICK_MS;
    last_click_target = dbl ? -1 : target;
    last_click_ms = now;
    return dbl;
}

static void press(int x, int y)
{
    /* start menu */
    if (menu_open) {
        rect_t r = menu_rect();
        if (inside(x, y, r.x, r.y, r.w, r.h)) {
            for (int i = 0; i < M_COUNT; i++)
                if (inside(x, y, r.x, menu_item_y(i), r.w, MENU_ITEM_H))
                    menu_action(i);
            return;
        }
        menu_open = 0;
        scene_dirty = 1;
        if (inside(x, y, 0, scr_h - TASKBAR_H, START_W, TASKBAR_H))
            return;
    }
    /* taskbar */
    if (y >= scr_h - TASKBAR_H) {
        if (x < START_W) {
            menu_open = 1;
            dlog("menu opened");
            scene_dirty = 1;
            return;
        }
        int bx = START_W + 6;
        for (int id = 0; id < MAXWIN && bx + TASK_BTN_W < scr_w - 220; id++) {
            if (!wins[id].used)
                continue;
            if (x >= bx && x < bx + TASK_BTN_W - 6)
                raise_window(id);
            bx += TASK_BTN_W;
        }
        return;
    }
    /* windows */
    int id = window_at(x, y);
    if (id >= 0) {
        window_t *w = &wins[id];
        raise_window(id);
        icon_sel = -1;
        if (y < w->y + TITLE_H) {
            if (inside(x, y, w->x + w->w - 24, w->y + 4, 18, 18)) {
                close_window(id);
                return;
            }
            drag_win = id;
            drag_dx = x - w->x;
            drag_dy = y - w->y;
            drag_moved = 0;
            return;
        }
        rect_t c = client_rect(w);
        int dbl = is_double(1000 + id);
        if (w->kind == APP_FILES)
            files_click(w, x - c.x, y - c.y, dbl);
        return;
    }
    /* desktop icons */
    for (int i = 0; i < NICONS; i++) {
        if (inside(x, y, ICON_X, ICON_Y + i * ICON_STEP, ICON_W, ICON_H)) {
            icon_sel = i;
            scene_dirty = 1;
            if (is_double(i))
                menu_action(icons[i].item);
            return;
        }
    }
    icon_sel = -1;
    scene_dirty = 1;
}

static void mouse_event(const struct fresh_input_event *e)
{
    int ox = mx, oy = my;
    mx += e->dx;
    my += e->dy;
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    if (mx > scr_w - 1)
        mx = scr_w - 1;
    if (my > scr_h - 1)
        my = scr_h - 1;
    int pressed = (int)e->buttons & ~mbuttons, released = mbuttons & ~(int)e->buttons;
    mbuttons = (int)e->buttons;
    if (drag_win >= 0 && (mx != ox || my != oy)) {
        window_t *w = &wins[drag_win];
        int nx = mx - drag_dx, ny = my - drag_dy;
        if (ny < 0)
            ny = 0;
        if (ny > scr_h - TASKBAR_H - TITLE_H)
            ny = scr_h - TASKBAR_H - TITLE_H;
        if (nx < 60 - w->w)
            nx = 60 - w->w;
        if (nx > scr_w - 60)
            nx = scr_w - 60;
        w->x = nx;
        w->y = ny;
        drag_moved = 1;
        scene_dirty = 1;
    }
    if (pressed & MOUSE_LEFT)
        press(mx, my);
    if ((released & MOUSE_LEFT) && drag_win >= 0) {
        if (drag_moved)
            dlog("moved %s to %d,%d", wins[drag_win].title, wins[drag_win].x, wins[drag_win].y);
        drag_win = -1;
    }
    if (menu_open && (mx != ox || my != oy))
        scene_dirty = 1; /* hover highlight */
    if (!scene_dirty && (mx != ox || my != oy)) {
        cursor_erase(ox, oy);
        cursor_draw(mx, my);
    }
}

static void key_event(const struct fresh_input_event *e)
{
    if ((e->mods & MOD_ALT) && e->key == '\t') {
        if (nz > 1)
            raise_window(zorder[0]);
        return;
    }
    if ((e->mods & MOD_ALT) && e->key == KEY_F1 + 3) {
        if (focused >= 0)
            close_window(focused);
        return;
    }
    if ((e->mods & MOD_CTRL) && (e->mods & MOD_ALT) && e->key == 't') {
        open_terminal(NULL, 0, "Terminal");
        return;
    }
    if (menu_open && e->key == 27) {
        menu_open = 0;
        scene_dirty = 1;
        return;
    }
    if (focused < 0)
        return;
    window_t *w = &wins[focused];
    if (w->kind == APP_TERM)
        term_key(w, e);
    else if (w->kind == APP_FILES)
        files_key(w, e);
}

/* ------------------------------------------------------------ main loop */
static void update_clock(void)
{
    struct tm_simple tm;
    unix_to_tm(unix_time(), &tm);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.hour, tm.min);
    if (strcmp(buf, clock_text)) {
        strcpy(clock_text, buf);
        scene_dirty = 1;
    }
}

static void reap(void)
{
    for (int i = 0; i < nreap;) {
        int st;
        if (wait_pid_opt(reap_pids[i], &st, WAIT_NOHANG) != 0) {
            reap_pids[i] = reap_pids[--nreap];
            continue;
        }
        i++;
    }
}

int main(void)
{
    struct fresh_fbinfo fi;
    int r = fb_acquire(&fi);
    if (r < 0) {
        fprintf(stderr, "desktop: no graphical screen available (%s)\n", strerror(r));
        return 1;
    }
    if (gfx_init(&fi) < 0) {
        fb_release();
        fprintf(stderr, "desktop: not enough memory\n");
        return 1;
    }
    for (int i = 0; i < 16; i++)
        pal[i] = px(palette_rgb[i]);
    unlink("/tmp/desktop.log");
    dlog("started %dx%d", scr_w, scr_h);
    draw_wallpaper();
    mx = scr_w / 2;
    my = scr_h / 2;
    update_clock();
    open_terminal(NULL, 0, "Terminal");
    uint64_t last_slow = 0, last_render = 0;
    struct fresh_input_event ev[64];
    for (;;) {
        int n = input_read(ev, 64, 15);
        for (int i = 0; i < n; i++) {
            if (ev[i].type == INPUT_MOUSE)
                mouse_event(&ev[i]);
            else if (ev[i].type == INPUT_KEY)
                key_event(&ev[i]);
        }
        for (int id = 0; id < MAXWIN; id++)
            if (wins[id].used && wins[id].kind == APP_TERM)
                term_tick(id);
        uint64_t now = uptime_ms();
        if (now - last_slow >= 1000) {
            last_slow = now;
            update_clock();
            reap();
            for (int id = 0; id < MAXWIN; id++) {
                if (!wins[id].used)
                    continue;
                if (wins[id].kind == APP_MONITOR)
                    monitor_load(&wins[id]);
                else if (wins[id].kind == APP_FILES)
                    files_load(&wins[id]);
            }
        }
        if (scene_dirty && now - last_render >= 30) {
            render();
            last_render = now;
        }
    }
}

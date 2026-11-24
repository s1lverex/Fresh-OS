/* Fresh OS kernel - terminals (line discipline) and pseudo-terminals.
 *
 * The console terminal takes input from the PS/2 keyboard (unless a GUI owns
 * the input devices) and the serial port, and writes to the screen console and
 * the serial port. A pseudo-terminal (pty) has the same line discipline, but
 * its "screen" is a buffer read by the master side (e.g. a GUI terminal
 * window) and its "keyboard" is what the master writes.
 *
 * Cooked mode: line editing (Backspace, Ctrl+U), echo, Ctrl+C kills the
 * foreground process group, Ctrl+D is end-of-file. Raw mode: bytes pass
 * through unchanged. */
#include "dev.h"
#include "../proc/proc.h"
#include "../mm/mm.h"

#define INQ_SIZE 4096
#define LINE_MAX 1024
#define PTY_OUT_SIZE 16384

struct tty {
    char inq[INQ_SIZE];
    uint32_t in_r, in_w;
    char line[LINE_MAX];
    int line_len;
    int mode;
    int fg;
    int eof_pending;
    int esc_skip; /* cooked mode swallows escape sequences */
    /* pseudo-terminal state */
    bool is_pty;
    char *out;
    uint32_t out_r, out_w;
    int rows, cols;
    int slave_refs;
    bool master_open;
    bool hung_up;
};

static struct tty console;

struct tty *tty_console(void) { return &console; }

static void push(struct tty *t, char c)
{
    if (t->in_w - t->in_r < INQ_SIZE)
        t->inq[t->in_w++ % INQ_SIZE] = c;
}

/* terminal output; for a pty this never blocks (used for echo) */
static void emit(struct tty *t, const char *s, size_t n)
{
    if (!t->is_pty) {
        console_write(s, n);
        serial_write(s, n);
        return;
    }
    if (t->hung_up)
        return;
    for (size_t i = 0; i < n && t->out_w - t->out_r < PTY_OUT_SIZE; i++)
        t->out[t->out_w++ % PTY_OUT_SIZE] = s[i];
}

void tty_init(void)
{
    memset(&console, 0, sizeof(console));
    console.mode = TTY_MODE_COOKED;
}

void tty_input_on(struct tty *t, char c)
{
    uint64_t f = irq_save();
    if (c == '\r')
        c = '\n';
    if (t->mode == TTY_MODE_RAW) {
        push(t, c);
        wakeup(t->inq);
        irq_restore(f);
        return;
    }
    if (t->esc_skip) {
        if (t->esc_skip == 1)
            t->esc_skip = (c == '[') ? 2 : 0;
        else if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E)
            t->esc_skip = 0;
        irq_restore(f);
        return;
    }
    switch (c) {
    case 0x1B:
        t->esc_skip = 1;
        break;
    case 3: /* Ctrl+C */
        t->line_len = 0;
        emit(t, "^C\n", 3);
        if (t->fg > 1)
            proc_kill_group(t->fg, EXIT_KILLED);
        wakeup(t->inq);
        break;
    case 4: /* Ctrl+D */
        if (t->line_len == 0) {
            t->eof_pending++;
        } else {
            for (int i = 0; i < t->line_len; i++)
                push(t, t->line[i]);
            t->line_len = 0;
        }
        wakeup(t->inq);
        break;
    case 0x7F:
    case '\b':
        if (t->line_len) {
            t->line_len--;
            emit(t, "\b \b", 3);
        }
        break;
    case 0x15: /* Ctrl+U */
        while (t->line_len) {
            t->line_len--;
            emit(t, "\b \b", 3);
        }
        break;
    case '\n':
        emit(t, "\n", 1);
        for (int i = 0; i < t->line_len; i++)
            push(t, t->line[i]);
        push(t, '\n');
        t->line_len = 0;
        wakeup(t->inq);
        break;
    default:
        if ((unsigned char)c < 32 && c != '\t')
            break;
        if (t->line_len < LINE_MAX - 2) {
            t->line[t->line_len++] = c;
            emit(t, &c, 1);
        }
        break;
    }
    irq_restore(f);
}

void tty_input(char c) { tty_input_on(&console, c); }

void tty_input_str(const char *s)
{
    while (*s)
        tty_input(*s++);
}

long tty_read_on(struct tty *t, char *buf, size_t n)
{
    if (n == 0)
        return 0;
    uint64_t f = irq_save();
    while (t->in_r == t->in_w) {
        if (t->mode == TTY_MODE_COOKED && t->eof_pending) {
            t->eof_pending--;
            irq_restore(f);
            return 0;
        }
        if (t->hung_up) {
            irq_restore(f);
            return 0;
        }
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        sleep_on(t->inq);
    }
    size_t got = 0;
    while (got < n && t->in_r != t->in_w) {
        char c = t->inq[t->in_r++ % INQ_SIZE];
        buf[got++] = c;
        if (t->mode == TTY_MODE_COOKED && c == '\n')
            break;
    }
    irq_restore(f);
    return (long)got;
}

long tty_write_on(struct tty *t, const char *buf, size_t n)
{
    if (!t->is_pty) {
        console_write(buf, n);
        serial_write(buf, n);
        return (long)n;
    }
    size_t done = 0;
    uint64_t f = irq_save();
    while (done < n) {
        if (t->hung_up) {
            irq_restore(f);
            return done ? (long)done : -EIO;
        }
        if (current->killed) {
            irq_restore(f);
            return done ? (long)done : -EINTR;
        }
        if (t->out_w - t->out_r == PTY_OUT_SIZE) {
            sleep_on(&t->out); /* the master reads to make room */
            continue;
        }
        while (done < n && t->out_w - t->out_r < PTY_OUT_SIZE)
            t->out[t->out_w++ % PTY_OUT_SIZE] = buf[done++];
    }
    irq_restore(f);
    return (long)done;
}

void tty_set_mode_on(struct tty *t, int mode)
{
    uint64_t f = irq_save();
    if (mode != t->mode) {
        /* keep characters typed ahead in cooked mode */
        if (mode == TTY_MODE_RAW)
            for (int i = 0; i < t->line_len; i++)
                push(t, t->line[i]);
        t->mode = mode;
        t->line_len = 0;
        t->esc_skip = 0;
        t->eof_pending = 0;
    }
    irq_restore(f);
}

int tty_get_mode_on(struct tty *t) { return t->mode; }
void tty_set_fg_on(struct tty *t, int pgid) { t->fg = pgid; }

void tty_size_on(struct tty *t, int *rows, int *cols)
{
    if (t->is_pty) {
        *rows = t->rows;
        *cols = t->cols;
    } else {
        *rows = console_rows();
        *cols = console_cols();
    }
}

/* ------------------------------------------------------------ pty */
struct tty *pty_create(void)
{
    struct tty *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->out = kmalloc(PTY_OUT_SIZE);
    if (!t->out) {
        kfree(t);
        return NULL;
    }
    t->is_pty = true;
    t->mode = TTY_MODE_COOKED;
    t->rows = 25;
    t->cols = 80;
    t->master_open = true;
    t->slave_refs = 1;
    return t;
}

static void pty_maybe_free(struct tty *t)
{
    if (!t->master_open && t->slave_refs == 0) {
        kfree(t->out);
        kfree(t);
    }
}

bool tty_is_pty(struct tty *t) { return t->is_pty; }

void pty_slave_closed(struct tty *t)
{
    uint64_t f = irq_save();
    t->slave_refs--;
    irq_restore(f);
    pty_maybe_free(t);
}

void pty_master_closed(struct tty *t)
{
    uint64_t f = irq_save();
    t->master_open = false;
    t->hung_up = true; /* slave reads see end of file, writes fail */
    wakeup(t->inq);
    wakeup(&t->out);
    irq_restore(f);
    pty_maybe_free(t);
}

/* never blocks: returns -EAGAIN when there is nothing to read, 0 once no
 * slave is open any more and everything was read */
long pty_master_read(struct tty *t, char *buf, size_t n)
{
    uint64_t f = irq_save();
    if (t->out_r == t->out_w) {
        irq_restore(f);
        return t->slave_refs == 0 ? 0 : -EAGAIN;
    }
    size_t got = 0;
    while (got < n && t->out_r != t->out_w)
        buf[got++] = t->out[t->out_r++ % PTY_OUT_SIZE];
    wakeup(&t->out);
    irq_restore(f);
    return (long)got;
}

long pty_master_write(struct tty *t, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        tty_input_on(t, buf[i]);
    return (long)n;
}

void pty_resize(struct tty *t, int rows, int cols)
{
    t->rows = rows;
    t->cols = cols;
}

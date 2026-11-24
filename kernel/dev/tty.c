/* Fresh OS kernel - terminal line discipline.
 * Input arrives from the PS/2 keyboard and the serial port; output goes to
 * both the screen console and the serial port.
 * Cooked mode: line editing (Backspace, Ctrl+U), echo, Ctrl+C kills the
 * foreground process, Ctrl+D is end-of-file. Raw mode: bytes pass through. */
#include "dev.h"
#include "../proc/proc.h"

#define INQ_SIZE 4096
#define LINE_MAX 1024

static char inq[INQ_SIZE];
static uint32_t in_r, in_w;
static char line[LINE_MAX];
static int line_len;
static int tmode = TTY_MODE_COOKED;
static int fg_pid;
static int eof_pending;
static int esc_skip; /* cooked mode swallows escape sequences */

static void push(char c)
{
    if (in_w - in_r < INQ_SIZE)
        inq[in_w++ % INQ_SIZE] = c;
}

static void echo(const char *s, size_t n)
{
    console_write(s, n);
    serial_write(s, n);
}

void tty_init(void)
{
    in_r = in_w = 0;
    line_len = 0;
}

void tty_input(char c)
{
    uint64_t f = irq_save();
    if (c == '\r')
        c = '\n';
    if (tmode == TTY_MODE_RAW) {
        push(c);
        wakeup(inq);
        irq_restore(f);
        return;
    }
    if (esc_skip) {
        if (esc_skip == 1)
            esc_skip = (c == '[') ? 2 : 0;
        else if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E)
            esc_skip = 0;
        irq_restore(f);
        return;
    }
    switch (c) {
    case 0x1B:
        esc_skip = 1;
        break;
    case 3: /* Ctrl+C */
        line_len = 0;
        echo("^C\n", 3);
        if (fg_pid > 1)
            proc_kill_group(fg_pid, EXIT_KILLED);
        wakeup(inq);
        break;
    case 4: /* Ctrl+D */
        if (line_len == 0) {
            eof_pending++;
        } else {
            for (int i = 0; i < line_len; i++)
                push(line[i]);
            line_len = 0;
        }
        wakeup(inq);
        break;
    case 0x7F:
    case '\b':
        if (line_len) {
            line_len--;
            echo("\b \b", 3);
        }
        break;
    case 0x15: /* Ctrl+U */
        while (line_len) {
            line_len--;
            echo("\b \b", 3);
        }
        break;
    case '\n':
        echo("\n", 1);
        for (int i = 0; i < line_len; i++)
            push(line[i]);
        push('\n');
        line_len = 0;
        wakeup(inq);
        break;
    default:
        if ((unsigned char)c < 32 && c != '\t')
            break;
        if (line_len < LINE_MAX - 2) {
            line[line_len++] = c;
            echo(&c, 1);
        }
        break;
    }
    irq_restore(f);
}

void tty_input_str(const char *s)
{
    while (*s)
        tty_input(*s++);
}

long tty_read(char *buf, size_t n)
{
    if (n == 0)
        return 0;
    uint64_t f = irq_save();
    while (in_r == in_w) {
        if (tmode == TTY_MODE_COOKED && eof_pending) {
            eof_pending--;
            irq_restore(f);
            return 0;
        }
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        sleep_on(inq);
    }
    size_t got = 0;
    while (got < n && in_r != in_w) {
        char c = inq[in_r++ % INQ_SIZE];
        buf[got++] = c;
        if (tmode == TTY_MODE_COOKED && c == '\n')
            break;
    }
    irq_restore(f);
    return (long)got;
}

long tty_write(const char *buf, size_t n)
{
    console_write(buf, n);
    serial_write(buf, n);
    return (long)n;
}

void tty_set_mode(int mode)
{
    uint64_t f = irq_save();
    if (mode != tmode) {
        /* keep characters typed ahead in cooked mode */
        if (mode == TTY_MODE_RAW)
            for (int i = 0; i < line_len; i++)
                push(line[i]);
        tmode = mode;
        line_len = 0;
        esc_skip = 0;
        eof_pending = 0;
    }
    irq_restore(f);
}

int tty_get_mode(void) { return tmode; }

void tty_set_fg(int pid) { fg_pid = pid; }

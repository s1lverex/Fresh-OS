/* Fresh OS kernel - input events for the program that owns the screen.
 * While a GUI holds the framebuffer (fb.c), keyboard and mouse input are
 * delivered here as events instead of going to the console terminal. */
#include "dev.h"
#include "../proc/proc.h"

#define QSIZE 512

static struct fresh_input_event q[QSIZE];
static uint32_t qr, qw;
static int owner;

bool input_grabbed(void) { return owner > 0; }

static void push(const struct fresh_input_event *e)
{
    if (qw - qr >= QSIZE)
        qr++; /* full: drop the oldest event */
    q[qw++ % QSIZE] = *e;
    wakeup(q);
}

void input_key(uint32_t key, uint32_t ch, uint32_t mods)
{
    uint64_t f = irq_save();
    struct fresh_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = INPUT_KEY;
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    push(&e);
    irq_restore(f);
}

void input_mouse(int dx, int dy, uint32_t buttons)
{
    uint64_t f = irq_save();
    /* merge with a pending motion event that has the same buttons */
    if (qw != qr) {
        struct fresh_input_event *last = &q[(qw - 1) % QSIZE];
        if (last->type == INPUT_MOUSE && last->buttons == buttons) {
            last->dx += dx;
            last->dy += dy;
            irq_restore(f);
            wakeup(q);
            return;
        }
    }
    struct fresh_input_event e;
    memset(&e, 0, sizeof(e));
    e.type = INPUT_MOUSE;
    e.dx = dx;
    e.dy = dy;
    e.buttons = buttons;
    push(&e);
    irq_restore(f);
}

void input_grab(int pid)
{
    uint64_t f = irq_save();
    owner = pid;
    qr = qw = 0;
    irq_restore(f);
}

void input_release(int pid)
{
    uint64_t f = irq_save();
    if (owner == pid)
        owner = 0;
    irq_restore(f);
}

long input_read(void *events, size_t max, uint64_t timeout_ms)
{
    if (current->pid != owner)
        return -EPERM;
    uint64_t f = irq_save();
    uint64_t deadline = g_ticks + (timeout_ms * TIMER_HZ + 999) / 1000;
    while (qr == qw) {
        if (timeout_ms == 0 || g_ticks >= deadline) {
            irq_restore(f);
            return 0;
        }
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        current->wake_tick = deadline;
        sleep_on(q);
        current->wake_tick = 0;
    }
    size_t n = 0;
    struct fresh_input_event *out = events;
    while (n < max && qr != qw)
        out[n++] = q[qr++ % QSIZE];
    irq_restore(f);
    return (long)n;
}

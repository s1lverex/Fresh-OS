/* Fresh OS kernel - PS/2 keyboard (scan code set 1, US layout).
 * Special keys are delivered as ANSI escape sequences. */
#include "dev.h"
#include "../arch/arch.h"

static const char normal_map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const char shift_map[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static bool shift_l, shift_r, ctrl, alt, caps, e0;

static void key_event(uint8_t sc)
{
    if (sc == 0xE0) {
        e0 = true;
        return;
    }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    if (e0) {
        e0 = false;
        if (code == 0x1D) {
            ctrl = !release;
            return;
        }
        if (code == 0x38) {
            alt = !release;
            return;
        }
        if (code == 0x2A || code == 0x36) /* fake shifts */
            return;
        if (release)
            return;
        switch (code) {
        case 0x48: tty_input_str("\033[A"); break;
        case 0x50: tty_input_str("\033[B"); break;
        case 0x4D: tty_input_str("\033[C"); break;
        case 0x4B: tty_input_str("\033[D"); break;
        case 0x47: tty_input_str("\033[H"); break;
        case 0x4F: tty_input_str("\033[F"); break;
        case 0x49: tty_input_str("\033[5~"); break;
        case 0x51: tty_input_str("\033[6~"); break;
        case 0x52: tty_input_str("\033[2~"); break;
        case 0x53: tty_input_str("\033[3~"); break;
        case 0x1C: tty_input('\n'); break;
        case 0x35: tty_input('/'); break;
        default: break;
        }
        return;
    }
    switch (code) {
    case 0x2A: shift_l = !release; return;
    case 0x36: shift_r = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A:
        if (!release)
            caps = !caps;
        return;
    default: break;
    }
    if (release)
        return;
    bool shift = shift_l || shift_r;
    char c = shift ? shift_map[code] : normal_map[code];
    if (!c)
        return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else if (c == '[')
            c = 27;
        else
            return;
    }
    if (c == '\b')
        c = 0x7F;
    tty_input(c);
}

static void kbd_irq(struct trapframe *tf)
{
    UNUSED(tf);
    int guard = 16;
    while ((inb(0x64) & 1) && guard--) {
        uint8_t st = inb(0x64);
        uint8_t sc = inb(0x60);
        if (st & 0x20) /* mouse byte */
            continue;
        key_event(sc);
    }
}

static bool wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1)
            return true;
    return false;
}

void keyboard_init(void)
{
    if (inb(0x64) == 0xFF) { /* no controller */
        kprintf("kbd: no PS/2 controller\n");
        return;
    }
    for (int i = 0; i < 32 && (inb(0x64) & 1); i++)
        inb(0x60);
    /* enable IRQ1 and scan code translation in the controller config */
    if (wait_write()) {
        outb(0x64, 0x20);
        if (wait_read()) {
            uint8_t cfg = inb(0x60);
            cfg |= 0x01 | 0x40;
            cfg &= (uint8_t)~0x10; /* clock of port 1 enabled */
            if (wait_write()) {
                outb(0x64, 0x60);
                if (wait_write())
                    outb(0x60, cfg);
            }
        }
    }
    if (wait_write())
        outb(0x64, 0xAE); /* enable first port */
    for (int i = 0; i < 32 && (inb(0x64) & 1); i++)
        inb(0x60);
    irq_register(1, kbd_irq);
    kprintf("kbd: PS/2 keyboard ready\n");
}

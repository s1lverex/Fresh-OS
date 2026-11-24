/* Fresh OS kernel - PS/2 keyboard (scan code set 1, US layout).
 * Keys go to the console terminal (special keys as ANSI escape sequences),
 * or as events to the GUI while one owns the screen (see input.c). */
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

void mouse_byte(uint8_t b);

static uint32_t mods(void)
{
    return ((shift_l || shift_r) ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0);
}

/* a special (non-character) key: GUI event or ANSI sequence for the terminal */
static void special(uint32_t key, const char *ansi)
{
    if (input_grabbed())
        input_key(key, 0, mods());
    else
        tty_input_str(ansi);
}

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
        case 0x48: special(KEY_UP, "\033[A"); break;
        case 0x50: special(KEY_DOWN, "\033[B"); break;
        case 0x4D: special(KEY_RIGHT, "\033[C"); break;
        case 0x4B: special(KEY_LEFT, "\033[D"); break;
        case 0x47: special(KEY_HOME, "\033[H"); break;
        case 0x4F: special(KEY_END, "\033[F"); break;
        case 0x49: special(KEY_PGUP, "\033[5~"); break;
        case 0x51: special(KEY_PGDN, "\033[6~"); break;
        case 0x52: special(KEY_INSERT, "\033[2~"); break;
        case 0x53: special(KEY_DELETE, "\033[3~"); break;
        case 0x1C:
            if (input_grabbed())
                input_key('\n', '\n', mods());
            else
                tty_input('\n');
            break;
        case 0x35:
            if (input_grabbed())
                input_key('/', '/', mods());
            else
                tty_input('/');
            break;
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
    if (input_grabbed() && code >= 0x3B && code <= 0x44) { /* F1-F10 */
        input_key(KEY_F1 + (code - 0x3B), 0, mods());
        return;
    }
    if (input_grabbed() && (code == 0x57 || code == 0x58)) { /* F11, F12 */
        input_key(KEY_F1 + 10 + (code - 0x57), 0, mods());
        return;
    }
    bool shift = shift_l || shift_r;
    char c = shift ? shift_map[code] : normal_map[code];
    if (!c)
        return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    char plain = c;
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else if (c == '[')
            c = 27;
        else if (!input_grabbed())
            return;
    }
    if (c == '\b')
        c = 0x7F;
    if (input_grabbed()) {
        /* key: the plain key (letters lower case); ch: what it types */
        uint32_t key = (plain >= 'A' && plain <= 'Z') ? (uint32_t)(plain + 32) : (uint8_t)plain;
        if (plain == '\b')
            key = 0x7F;
        input_key(key, (uint8_t)c, mods());
        return;
    }
    tty_input(c);
}

/* read everything the PS/2 controller has: keyboard and mouse bytes */
void ps2_drain(void)
{
    int guard = 64;
    while ((inb(0x64) & 1) && guard--) {
        uint8_t st = inb(0x64);
        uint8_t b = inb(0x60);
        if (st & 0x20)
            mouse_byte(b);
        else
            key_event(b);
    }
}

static void kbd_irq(struct trapframe *tf)
{
    UNUSED(tf);
    ps2_drain();
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

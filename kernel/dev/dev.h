/* Fresh OS kernel - device drivers. */
#ifndef DEV_H
#define DEV_H

#include "../kernel.h"

/* serial.c - COM1 */
void serial_init(void);
bool serial_present(void);
void serial_enable_irq(void);
void serial_write(const char *s, size_t n);

/* console.c - framebuffer / VGA text console with ANSI escapes */
void console_init(void);
void console_write(const char *s, size_t n);
int console_cols(void);
int console_rows(void);
bool console_has_display(void);

/* keyboard.c - PS/2 */
void keyboard_init(void);

/* tty.c - terminals: the console (screen + serial) and pseudo-terminals */
struct tty;
void tty_init(void);
struct tty *tty_console(void);
void tty_input(char c);          /* console input, also from IRQ handlers */
void tty_input_str(const char *s);
void tty_input_on(struct tty *t, char c);
long tty_read_on(struct tty *t, char *buf, size_t n);
long tty_write_on(struct tty *t, const char *buf, size_t n);
void tty_set_mode_on(struct tty *t, int mode);
int tty_get_mode_on(struct tty *t);
void tty_set_fg_on(struct tty *t, int pgid);
void tty_size_on(struct tty *t, int *rows, int *cols);
bool tty_is_pty(struct tty *t);
struct tty *pty_create(void);
void pty_slave_closed(struct tty *t);
void pty_master_closed(struct tty *t);
long pty_master_read(struct tty *t, char *buf, size_t n);
long pty_master_write(struct tty *t, const char *buf, size_t n);
void pty_resize(struct tty *t, int rows, int cols);

/* input.c - keyboard/mouse events for the program that owns the screen */
bool input_grabbed(void);
void input_key(uint32_t key, uint32_t ch, uint32_t mods);
void input_mouse(int dx, int dy, uint32_t buttons);
void input_grab(int pid);
void input_release(int pid);
long input_read(void *events, size_t max, uint64_t timeout_ms);

/* mouse.c - PS/2 mouse */
void mouse_init(void);

/* fb.c - handing the framebuffer to a user program */
struct proc;
long fb_acquire(uint64_t uinfo);
long fb_release(void);
void fb_process_exit(struct proc *p);
void console_suspend(bool on);

/* pci.c */
struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
};
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
int pci_scan(struct pci_dev *out, int max);
uint64_t pci_bar(const struct pci_dev *d, int bar, bool *is_io);
void pci_enable_bus_master(const struct pci_dev *d);

/* block devices (blk.c) */
struct blkdev {
    char name[16];
    uint64_t sectors;  /* 512-byte sectors */
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *d);
    void *priv;
    char model[41];
};
#define MAX_BLKDEVS 8
int blk_register(struct blkdev *d);
int blk_count(void);
struct blkdev *blk_get(int i);
/* find a GPT partition by type GUID: returns 0 and fills start/count */
int gpt_find(struct blkdev *d, const uint8_t type_guid[16], uint64_t *start, uint64_t *count);

void ahci_init(void);
void ata_init(void);

/* acpi.c */
void acpi_init(void);
__attribute__((noreturn)) void acpi_poweroff(void);
__attribute__((noreturn)) void acpi_reboot(void);

#endif

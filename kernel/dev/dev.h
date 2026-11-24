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

/* tty.c - line discipline over console + serial */
void tty_init(void);
void tty_input(char c);          /* from IRQ handlers */
void tty_input_str(const char *s);
long tty_read(char *buf, size_t n);
long tty_write(const char *buf, size_t n);
void tty_set_mode(int mode);
int tty_get_mode(void);
void tty_set_fg(int pid);

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

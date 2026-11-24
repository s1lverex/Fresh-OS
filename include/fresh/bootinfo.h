/* Fresh OS - boot handoff structure shared by the BIOS loader, the UEFI loader
 * and the kernel. Field offsets are fixed: boot/bios/stage2.asm writes them
 * directly, so do not reorder. */
#ifndef FRESH_BOOTINFO_H
#define FRESH_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC   0xF2E5B007C0DE0001ULL
#define BOOTINFO_VERSION 1

#define BOOT_TYPE_BIOS 1
#define BOOT_TYPE_UEFI 2

#define BOOT_MMAP_USABLE       1
#define BOOT_MMAP_RESERVED     2
#define BOOT_MMAP_ACPI_RECLAIM 3
#define BOOT_MMAP_ACPI_NVS     4
#define BOOT_MMAP_BAD          5

#define BOOT_FB_NONE 0 /* no linear framebuffer (VGA text mode may exist) */
#define BOOT_FB_RGB  1 /* linear framebuffer, 32 bits per pixel           */

/* Kernel image format (KERNEL.BIN): this header followed by a flat image
 * that must be placed at a 2 MiB aligned physical address and mapped at
 * virt_base. */
#define KERNEL_MAGIC 0x4C4E524B48535246ULL /* "FRSHKRNL" */
struct kernel_header {
    uint64_t magic;
    uint64_t entry;     /* virtual entry point                   */
    uint64_t load_size; /* bytes of image data after this header */
    uint64_t mem_size;  /* bytes the image occupies incl. bss    */
    uint64_t virt_base; /* virtual address of image byte 0       */
    uint64_t reserved[3];
};

struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
}; /* 24 bytes, same layout as BIOS E820 */

struct boot_info {
    uint64_t magic;           /*   0 */
    uint32_t version;         /*   8 */
    uint32_t boot_type;       /*  12 */
    uint64_t kernel_phys;     /*  16 */
    uint64_t kernel_size;     /*  24 */
    uint64_t initrd_phys;     /*  32 */
    uint64_t initrd_size;     /*  40 */
    uint64_t fb_addr;         /*  48 */
    uint32_t fb_width;        /*  56 */
    uint32_t fb_height;       /*  60 */
    uint32_t fb_pitch;        /*  64 */
    uint32_t fb_bpp;          /*  68 */
    uint8_t fb_red_pos;       /*  72 */
    uint8_t fb_red_size;      /*  73 */
    uint8_t fb_green_pos;     /*  74 */
    uint8_t fb_green_size;    /*  75 */
    uint8_t fb_blue_pos;      /*  76 */
    uint8_t fb_blue_size;     /*  77 */
    uint8_t fb_type;          /*  78 */
    uint8_t pad0;             /*  79 */
    uint64_t rsdp;            /*  80 */
    uint64_t mmap_addr;       /*  88 */
    uint32_t mmap_count;      /*  96 */
    uint32_t mmap_entry_size; /* 100 */
    uint64_t scratch_phys;    /* 104: loader memory the kernel must keep */
    uint64_t scratch_size;    /* 112 */
    char cmdline[128];        /* 120 */
};                            /* 248 */

#endif

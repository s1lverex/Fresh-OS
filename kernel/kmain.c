/* Fresh OS kernel - entry point and boot sequence. */
#include "kernel.h"
#include "arch/arch.h"
#include "mm/mm.h"
#include "dev/dev.h"
#include "fs/vfs.h"
#include "proc/proc.h"

struct boot_info g_boot;
struct boot_mmap_entry g_mmap[256];
uint32_t g_mmap_count;
bool g_test_mode;

bool cmdline_has(const char *word)
{
    size_t n = strlen(word);
    const char *p = g_boot.cmdline;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *s = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - s) == n && !strncmp(s, word, n))
            return true;
    }
    return false;
}

static void copy_boot_info(struct boot_info *bi)
{
    if (bi->magic != BOOTINFO_MAGIC)
        cpu_halt_forever();
    memcpy(&g_boot, bi, sizeof(g_boot));
    g_boot.cmdline[sizeof(g_boot.cmdline) - 1] = 0;
    uint32_t n = MIN(bi->mmap_count, (uint32_t)ARRAY_SIZE(g_mmap));
    uint32_t esz = bi->mmap_entry_size ? bi->mmap_entry_size : sizeof(struct boot_mmap_entry);
    for (uint32_t i = 0; i < n; i++)
        memcpy(&g_mmap[i], (uint8_t *)bi->mmap_addr + (uint64_t)i * esz, sizeof(struct boot_mmap_entry));
    g_mmap_count = n;
}

static void banner(void)
{
    kprintf("\n\033[1;32m  ______             _        ____   _____ \n"
            " |  ____|           | |      / __ \\ / ____|\n"
            " | |__ _ __ ___  ___| |__   | |  | | (___  \n"
            " |  __| '__/ _ \\/ __| '_ \\  | |  | |\\___ \\ \n"
            " | |  | | |  __/\\__ \\ | | | | |__| |____) |\n"
            " |_|  |_|  \\___||___/_| |_|  \\____/|_____/ \033[0m\n\n");
    kprintf("Fresh OS %s - a new operating system for x86-64 PCs\n", FRESH_VERSION);
}

static void start_init(void)
{
    struct file *tty = file_new(FILE_TTY);
    if (!tty)
        panic("cannot allocate console file");
    tty->flags = O_RDWR;
    tty->tty = tty_console();
    struct file *fds[3] = {tty, tty, tty};
    char *argv[] = {"/bin/init", NULL};
    int pid = proc_spawn("/bin/init", argv, 1, fds, "/", 0);
    file_close(tty); /* the process holds its own references */
    if (pid != 1)
        panic("cannot start /bin/init (error %d)", pid);
}

void kmain(struct boot_info *bi)
{
    copy_boot_info(bi);
    g_test_mode = cmdline_has("testmode");
    serial_init();
    kprintf("\nFresh OS %s booting (%s), cmdline '%s'\n", FRESH_VERSION,
            g_boot.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "BIOS", g_boot.cmdline);

    cpu_init();
    gdt_init();
    idt_init();
    pic_init();
    pmm_init();
    vmm_init();
    heap_init();
    console_init();
    klog_set_console(true);
    banner();
    kprintf("cpu: %s\n", cpu_brand);
    if (console_has_display())
        kprintf("console: %dx%d text on %s\n", console_cols(), console_rows(),
                g_boot.fb_type ? "framebuffer" : "VGA text mode");

    syscall_init();
    proc_init();
    timer_init();
    rtc_init();
    tty_init();
    keyboard_init();
    mouse_init();
    serial_enable_irq();
    __asm__ volatile("sti");
    acpi_init();

    uint64_t root;
    void *rfs = ramfs_new(&root);
    if (!rfs)
        panic("cannot create root file system");
    vfs_mount("/", ramfs_ops(), rfs, root);
    if (!g_boot.initrd_size || initrd_unpack(phys_to_virt(g_boot.initrd_phys), g_boot.initrd_size))
        panic("no usable initrd - cannot continue");
    pmm_release_range(g_boot.initrd_phys, g_boot.initrd_size);
    vfs_mkdir("/data");
    vfs_mkdir("/tmp");

    ahci_init();
    ata_init();
    data_fs_mount();

    kprintf("memory: %llu MiB free of %llu MiB\n", (unsigned long long)(pmm_free_pages() * PAGE_SIZE >> 20),
            (unsigned long long)(pmm_total_pages() * PAGE_SIZE >> 20));
    start_init();
    idle_loop();
}

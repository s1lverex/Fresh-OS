/* Fresh OS kernel - lend the framebuffer to a user program (the desktop).
 * The owner gets the framebuffer mapped into its address space and receives
 * all keyboard/mouse input as events. The text console keeps running in the
 * background (and on the serial port) and is redrawn when the owner releases
 * the screen or exits. */
#include "dev.h"
#include "../mm/mm.h"
#include "../proc/proc.h"

#define FB_USER_BASE 0x0000710000000000ULL

static int owner_pid;
static uint64_t mapped_bytes;

static uint64_t fb_bytes(void) { return ALIGN_UP((uint64_t)g_boot.fb_pitch * g_boot.fb_height, PAGE_SIZE); }

static void unmap(struct proc *p)
{
    if (!p->pml4)
        return;
    for (uint64_t off = 0; off < mapped_bytes; off += PAGE_SIZE)
        vmm_unmap_user_device(p->pml4, FB_USER_BASE + off);
}

static void give_back(void)
{
    owner_pid = 0;
    mapped_bytes = 0;
    input_grab(0); /* input goes back to the console terminal */
    console_suspend(false);
}

long fb_acquire(uint64_t uinfo)
{
    if (g_boot.fb_type != BOOT_FB_RGB || g_boot.fb_bpp != 32)
        return -ENODEV;
    if (owner_pid && owner_pid != current->pid) {
        struct proc *o = proc_find(owner_pid);
        if (o && o->state != P_ZOMBIE)
            return -EBUSY;
        give_back();
    }
    if (!user_range_ok(uinfo, sizeof(struct fresh_fbinfo)))
        return -EFAULT;
    uint64_t bytes = fb_bytes();
    if (owner_pid != current->pid) {
        for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
            if (vmm_map_user_device(current->pml4, FB_USER_BASE + off, g_boot.fb_addr + off,
                                    PTE_W | PTE_NX | PTE_PWT) != 0) {
                mapped_bytes = off;
                unmap(current);
                mapped_bytes = 0;
                return -ENOMEM;
            }
        }
    }
    struct fresh_fbinfo info;
    memset(&info, 0, sizeof(info));
    info.addr = FB_USER_BASE;
    info.width = g_boot.fb_width;
    info.height = g_boot.fb_height;
    info.pitch = g_boot.fb_pitch;
    info.bpp = 32;
    info.red_pos = g_boot.fb_red_pos;
    info.green_pos = g_boot.fb_green_pos;
    info.blue_pos = g_boot.fb_blue_pos;
    if (copy_to_user((void *)uinfo, &info, sizeof(info))) {
        if (owner_pid != current->pid) {
            mapped_bytes = bytes;
            unmap(current);
            mapped_bytes = 0;
        }
        return -EFAULT;
    }
    if (owner_pid != current->pid) {
        owner_pid = current->pid;
        mapped_bytes = bytes;
        console_suspend(true);
        input_grab(current->pid);
        kprintf("fb: screen handed to %s[%d]\n", current->name, current->pid);
    }
    return 0;
}

long fb_release(void)
{
    if (owner_pid != current->pid)
        return -EPERM;
    unmap(current);
    give_back();
    return 0;
}

/* called when a process exits: its page tables are about to be freed (the
 * framebuffer pages are marked as device memory and are never freed) */
void fb_process_exit(struct proc *p)
{
    if (owner_pid && owner_pid == p->pid)
        give_back();
}

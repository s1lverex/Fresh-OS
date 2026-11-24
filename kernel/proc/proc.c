/* Fresh OS kernel - processes and the round-robin scheduler.
 * The kernel is not preemptible: a process switches only when it blocks,
 * exits, or when the timer fires while it runs in user mode. */
#include "proc.h"
#include "../mm/mm.h"
#include "../fs/vfs.h"

struct proc g_procs[MAX_PROCS] __attribute__((aligned(16)));
struct proc *current;
static int next_pid = 1;
static uint8_t fpu_template[512] __attribute__((aligned(16)));

#define SLICE_TICKS 2

void proc_init(void)
{
    memset(g_procs, 0, sizeof(g_procs));
    struct proc *idle = &g_procs[0];
    idle->pid = 0;
    idle->state = P_RUNNING;
    strcpy(idle->name, "idle");
    strcpy(idle->cwd, "/");
    idle->cr3 = vmm_kernel_cr3();
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
    fpu_save(fpu_template);
    memcpy(idle->fpu, fpu_template, sizeof(fpu_template));
    current = idle;
}

struct proc *proc_find(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].state != P_UNUSED && g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

int proc_count(void)
{
    int n = 0;
    for (int i = 1; i < MAX_PROCS; i++)
        if (g_procs[i].state != P_UNUSED)
            n++;
    return n;
}

/* ------------------------------------------------------------ scheduler */
void schedule(void)
{
    uint64_t f = irq_save();
    struct proc *prev = current;
    if (prev->state == P_RUNNING)
        prev->state = P_RUNNABLE;
    int start = (int)(prev - g_procs);
    struct proc *next = NULL;
    for (int k = 1; k <= MAX_PROCS; k++) {
        int i = (start + k) % MAX_PROCS;
        if (i == 0)
            continue;
        if (g_procs[i].state == P_RUNNABLE) {
            next = &g_procs[i];
            break;
        }
    }
    if (!next)
        next = &g_procs[0];
    next->state = P_RUNNING;
    next->slice = 0;
    if (next != prev) {
        fpu_save(prev->fpu);
        fpu_restore(next->fpu);
        current = next;
        if (next->kstack_top)
            tss_set_rsp0(next->kstack_top);
        if (next->cr3 != prev->cr3)
            write_cr3(next->cr3);
        switch_context(&prev->ksp, next->ksp);
    }
    irq_restore(f);
}

void sleep_on(void *chan)
{
    current->wait_chan = chan;
    current->state = P_BLOCKED;
    schedule();
    current->wait_chan = NULL;
}

void wakeup(void *chan)
{
    uint64_t f = irq_save();
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state == P_BLOCKED && p->wait_chan == chan) {
            p->state = P_RUNNABLE;
            p->wait_chan = NULL;
        }
    }
    irq_restore(f);
}

void sched_tick(struct trapframe *tf)
{
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state == P_BLOCKED && p->wake_tick && g_ticks >= p->wake_tick) {
            p->wake_tick = 0;
            p->state = P_RUNNABLE;
        }
    }
    current->cpu_ticks++;
    if (current != &g_procs[0] && (tf->cs & 3) == 3 && ++current->slice >= SLICE_TICKS)
        schedule();
}

int proc_sleep_ms(uint64_t ms)
{
    uint64_t target = g_ticks + (ms * TIMER_HZ + 999) / 1000;
    uint64_t f = irq_save();
    while (g_ticks < target) {
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        current->wake_tick = target;
        current->state = P_BLOCKED;
        current->wait_chan = NULL;
        schedule();
    }
    current->wake_tick = 0;
    irq_restore(f);
    return 0;
}

void idle_loop(void)
{
    for (;;) {
        __asm__ volatile("sti; hlt");
        schedule();
    }
}

/* ------------------------------------------------------------ helpers */
static int copy_to_space(uint64_t *pml4, uint64_t va, const void *src, uint64_t n)
{
    const uint8_t *s = src;
    while (n) {
        uint64_t phys = vmm_user_phys(pml4, va);
        if (!phys)
            return -EFAULT;
        uint64_t chunk = MIN(n, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        memcpy(phys_to_virt(phys), s, chunk);
        va += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

int elf_copy_to_space(uint64_t *pml4, uint64_t va, const void *src, uint64_t n)
{
    return copy_to_space(pml4, va, src, n);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ------------------------------------------------------------ lifecycle */
int proc_spawn(const char *path, char *const argv[], int argc, struct file *fds[3], const char *cwd, int pgid)
{
    uint8_t *data;
    uint64_t size;
    long rc = vfs_read_all(path, &data, &size, 64ULL << 20);
    if (rc)
        return (int)rc;
    struct proc *p = NULL;
    for (int i = 1; i < MAX_PROCS; i++) {
        if (g_procs[i].state == P_UNUSED) {
            p = &g_procs[i];
            break;
        }
    }
    if (!p) {
        kfree(data);
        return -EAGAIN;
    }
    uint64_t *pml4 = vmm_new_space();
    if (!pml4) {
        kfree(data);
        return -ENOMEM;
    }
    struct elf_image img;
    rc = elf_load(pml4, data, size, &img);
    kfree(data);
    if (rc) {
        vmm_free_space(pml4);
        return (int)rc;
    }

    /* user stack: top 4 pages mapped now, the rest on demand */
    for (uint64_t va = USER_STACK_TOP - 4 * PAGE_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        if (!vmm_map_user_page(pml4, va, PTE_W | PTE_NX)) {
            vmm_free_space(pml4);
            return -ENOMEM;
        }
    }
    uint64_t sp = USER_STACK_TOP;
    uint64_t uargv[MAX_ARGS + 1];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        copy_to_space(pml4, sp, argv[i], len);
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp &= ~15ULL;
    sp -= (uint64_t)(argc + 1) * 8;
    sp &= ~15ULL;
    copy_to_space(pml4, sp, uargv, (uint64_t)(argc + 1) * 8);
    uint64_t argv_ptr = sp;

    uint64_t kphys = pmm_alloc_contig(KSTACK_PAGES, 0);
    if (!kphys) {
        vmm_free_space(pml4);
        return -ENOMEM;
    }
    uint64_t f = irq_save();
    memset(p, 0, sizeof(*p));
    p->pid = next_pid++;
    p->ppid = current->pid;
    p->pgid = pgid < 0 ? p->pid : pgid > 0 ? pgid : (current->pid ? current->pgid : p->pid);
    strlcpy(p->name, base_name(path), sizeof(p->name));
    p->pml4 = pml4;
    p->cr3 = virt_to_phys(pml4);
    p->kstack_phys = kphys;
    p->kstack_top = (uint64_t)phys_to_virt(kphys) + KSTACK_SIZE;
    p->brk_start = p->brk = img.brk;
    strlcpy(p->cwd, cwd ? cwd : "/", sizeof(p->cwd));
    memcpy(p->fpu, fpu_template, sizeof(p->fpu));
    p->start_tick = g_ticks;
    for (int i = 0; i < 3; i++) {
        p->fds[i] = fds[i];
        file_ref(fds[i]);
    }
    struct trapframe *tf = (struct trapframe *)(p->kstack_top - sizeof(struct trapframe));
    memset(tf, 0, sizeof(*tf));
    tf->rip = img.entry;
    tf->cs = SEL_UCODE;
    tf->rflags = 0x202;
    tf->rsp = argv_ptr;
    tf->ss = SEL_UDATA;
    tf->rdi = (uint64_t)argc;
    tf->rsi = argv_ptr;
    uint64_t *ks = (uint64_t *)tf;
    *--ks = (uint64_t)user_entry_trampoline;
    for (int i = 0; i < 6; i++)
        *--ks = 0;
    p->ksp = (uint64_t)ks;
    p->state = P_RUNNABLE;
    int pid = p->pid;
    irq_restore(f);
    return pid;
}

void proc_exit(int status)
{
    struct proc *p = current;
    if (p->pid == 1)
        panic("init exited with status %d", status);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            file_close(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
    uint64_t f = irq_save();
    write_cr3(vmm_kernel_cr3());
    p->cr3 = vmm_kernel_cr3();
    vmm_free_space(p->pml4);
    p->pml4 = NULL;
    struct proc *init = proc_find(1);
    bool wake_init = false;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *c = &g_procs[i];
        if (c->state != P_UNUSED && c->ppid == p->pid) {
            c->ppid = 1;
            if (c->state == P_ZOMBIE)
                wake_init = true;
        }
    }
    p->exit_status = status;
    p->state = P_ZOMBIE;
    struct proc *parent = proc_find(p->ppid);
    if (parent)
        wakeup(parent);
    if (wake_init && init)
        wakeup(init);
    schedule();
    irq_restore(f);
    panic("zombie process %d was scheduled", p->pid);
}

int proc_wait(int pid, int *status, int options)
{
    uint64_t f = irq_save();
    for (;;) {
        bool have = false;
        for (int i = 1; i < MAX_PROCS; i++) {
            struct proc *c = &g_procs[i];
            if (c->state == P_UNUSED || c->ppid != current->pid)
                continue;
            if (pid != -1 && c->pid != pid)
                continue;
            have = true;
            if (c->state == P_ZOMBIE) {
                int cpid = c->pid;
                if (status)
                    *status = c->exit_status;
                pmm_free_contig(c->kstack_phys, KSTACK_PAGES);
                memset(c, 0, sizeof(*c));
                c->state = P_UNUSED;
                irq_restore(f);
                return cpid;
            }
        }
        if (!have) {
            irq_restore(f);
            return -ECHILD;
        }
        if (options & WAIT_NOHANG) {
            irq_restore(f);
            return 0;
        }
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        sleep_on(current);
    }
}

int proc_kill(int pid, int status)
{
    if (pid <= 1)
        return -EPERM;
    uint64_t f = irq_save();
    struct proc *p = proc_find(pid);
    if (!p || p->state == P_ZOMBIE) {
        irq_restore(f);
        return -ESRCH;
    }
    /* the target (possibly the caller) exits when it next returns to user
     * mode; this is safe to call from interrupt handlers */
    p->killed = true;
    p->kill_status = status;
    if (p->state == P_BLOCKED) {
        p->state = P_RUNNABLE;
        p->wait_chan = NULL;
        p->wake_tick = 0;
    }
    irq_restore(f);
    return 0;
}

int proc_kill_group(int pgid, int status)
{
    if (pgid <= 1)
        return -EPERM;
    int n = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state != P_UNUSED && p->state != P_ZOMBIE && p->pgid == pgid && p->pid > 1) {
            proc_kill(p->pid, status);
            n++;
        }
    }
    return n ? 0 : -ESRCH;
}

void proc_return_check(struct trapframe *tf)
{
    UNUSED(tf);
    if (current->killed && current->pid > 1)
        proc_exit(current->kill_status);
}

bool proc_demand_page(uint64_t addr)
{
    struct proc *p = current;
    if (!p->pml4 || p->pid == 0)
        return false;
    if (addr < USER_STACK_TOP - USER_STACK_MAX || addr >= USER_STACK_TOP)
        return false;
    return vmm_map_user_page(p->pml4, ALIGN_DOWN(addr, PAGE_SIZE), PTE_W | PTE_NX) != 0;
}

void proc_fault(struct trapframe *tf, const char *what)
{
    struct proc *p = current;
    if (tf->vector == 14)
        kprintf("\n%s[%d]: killed: %s at address %#llx (rip %#llx)\n", p->name, p->pid, what,
                (unsigned long long)read_cr2(), (unsigned long long)tf->rip);
    else
        kprintf("\n%s[%d]: killed: %s (rip %#llx)\n", p->name, p->pid, what, (unsigned long long)tf->rip);
    proc_exit(EXIT_FAULT | (int)tf->vector);
}

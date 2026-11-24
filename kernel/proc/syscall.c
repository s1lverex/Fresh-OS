/* Fresh OS kernel - system call dispatch. Every pointer from user space is
 * range checked and copied with fault-safe routines. */
#include "proc.h"
#include "../mm/mm.h"
#include "../fs/vfs.h"
#include "../dev/dev.h"

#define IO_CHUNK (64 * 1024)

extern long copy_user_asm(void *dst, const void *src, size_t n);
extern long strncpy_user_asm(char *dst, const char *src, size_t max);
int ktest_run(void);

bool user_range_ok(uint64_t addr, uint64_t len)
{
    if (len == 0)
        return true;
    if (addr + len < addr)
        return false;
    return addr + len <= USER_TOP && addr >= 0x1000;
}

int copy_from_user(void *dst, const void *usrc, size_t n)
{
    if (!user_range_ok((uint64_t)usrc, n))
        return -EFAULT;
    return (int)copy_user_asm(dst, usrc, n);
}

int copy_to_user(void *udst, const void *src, size_t n)
{
    if (!user_range_ok((uint64_t)udst, n))
        return -EFAULT;
    return (int)copy_user_asm(udst, src, n);
}

long strncpy_from_user(char *dst, const char *usrc, size_t max)
{
    uint64_t a = (uint64_t)usrc;
    if (a < 0x1000 || a >= USER_TOP)
        return -EFAULT;
    size_t lim = MIN(max, USER_TOP - a);
    long r = strncpy_user_asm(dst, usrc, lim);
    if (r == -ENAMETOOLONG && lim < max)
        return -EFAULT;
    return r;
}

/* copy a user path and make it absolute */
static int user_path(const char *upath, char *out)
{
    char tmp[PATH_MAX];
    long r = strncpy_from_user(tmp, upath, sizeof(tmp));
    if (r < 0)
        return (int)r;
    return vfs_resolve(current->cwd, tmp, out);
}

static struct file *get_fd(int fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return NULL;
    return current->fds[fd];
}

static int alloc_fd(struct file *f)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

/* ------------------------------------------------------------ calls */
static long sys_spawn(uint64_t uspec)
{
    struct fresh_spawn spec;
    if (copy_from_user(&spec, (void *)uspec, sizeof(spec)))
        return -EFAULT;
    char path[PATH_MAX];
    int rc = user_path(spec.path, path);
    if (rc)
        return rc;
    char *argv[MAX_ARGS + 1];
    char *strbuf = kmalloc(MAX_ARG_BYTES);
    if (!strbuf)
        return -ENOMEM;
    int argc = 0;
    size_t used = 0;
    long ret = 0;
    if (spec.argv) {
        for (;;) {
            uint64_t uptr;
            if (copy_from_user(&uptr, (const char *)spec.argv + argc * 8, 8)) {
                ret = -EFAULT;
                goto out;
            }
            if (!uptr)
                break;
            if (argc >= MAX_ARGS) {
                ret = -E2BIG;
                goto out;
            }
            long len = strncpy_from_user(strbuf + used, (const char *)uptr, MAX_ARG_BYTES - used);
            if (len < 0) {
                ret = len == -ENAMETOOLONG ? -E2BIG : len;
                goto out;
            }
            argv[argc++] = strbuf + used;
            used += (size_t)len + 1;
        }
    }
    if (argc == 0) { /* argv[0] defaults to the path */
        size_t len = strlen(path);
        memcpy(strbuf, path, len + 1);
        argv[argc++] = strbuf;
    }
    argv[argc] = NULL;
    struct file *fds[3];
    for (int i = 0; i < 3; i++) {
        int src = spec.fd_map[i] < 0 ? i : spec.fd_map[i];
        if (src >= MAX_FDS) {
            ret = -EBADF;
            goto out;
        }
        fds[i] = current->fds[src];
    }
    ret = proc_spawn(path, argv, argc, fds, current->cwd, spec.pgid);
out:
    kfree(strbuf);
    return ret;
}

static long sys_read(int fd, uint64_t ubuf, uint64_t n)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok(ubuf, n))
        return -EFAULT;
    if (n == 0)
        return 0;
    size_t cap = (size_t)MIN(n, (uint64_t)IO_CHUNK);
    char *kbuf = kmalloc(cap);
    if (!kbuf)
        return -ENOMEM;
    uint64_t done = 0;
    long ret = 0;
    while (done < n) {
        size_t chunk = (size_t)MIN(n - done, (uint64_t)cap);
        long r = file_read(f, kbuf, chunk);
        if (r < 0) {
            ret = r;
            break;
        }
        if (r > 0 && copy_to_user((void *)(ubuf + done), kbuf, (size_t)r)) {
            ret = -EFAULT;
            break;
        }
        done += (uint64_t)r;
        if (r < (long)chunk || f->type != FILE_VNODE)
            break;
    }
    kfree(kbuf);
    if (done)
        return (long)done;
    return ret;
}

static long sys_write(int fd, uint64_t ubuf, uint64_t n)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok(ubuf, n))
        return -EFAULT;
    if (n == 0)
        return 0;
    size_t cap = (size_t)MIN(n, (uint64_t)IO_CHUNK);
    char *kbuf = kmalloc(cap);
    if (!kbuf)
        return -ENOMEM;
    uint64_t done = 0;
    long ret = 0;
    while (done < n) {
        size_t chunk = (size_t)MIN(n - done, (uint64_t)cap);
        if (copy_from_user(kbuf, (void *)(ubuf + done), chunk)) {
            ret = -EFAULT;
            break;
        }
        long r = file_write(f, kbuf, chunk);
        if (r < 0) {
            ret = r;
            break;
        }
        done += (uint64_t)r;
        if (r < (long)chunk)
            break;
    }
    kfree(kbuf);
    if (done)
        return (long)done;
    return ret;
}

static long sys_open(uint64_t upath, int flags)
{
    char path[PATH_MAX];
    int rc = user_path((const char *)upath, path);
    if (rc)
        return rc;
    struct file *f;
    if ((rc = vfs_open(path, flags, &f)) != 0)
        return rc;
    int fd = alloc_fd(f);
    if (fd < 0)
        file_close(f);
    return fd;
}

static long sys_seek(int fd, int64_t off, int whence)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    if (f->type != FILE_VNODE)
        return -ESPIPE;
    int64_t base;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = (int64_t)f->off;
    } else if (whence == SEEK_END) {
        struct fresh_stat st;
        int rc = f->mnt->ops->stat(f->mnt, f->ino, &st);
        if (rc)
            return rc;
        base = (int64_t)st.size;
    } else {
        return -EINVAL;
    }
    int64_t pos = base + off;
    if (pos < 0)
        return -EINVAL;
    f->off = (uint64_t)pos;
    return pos;
}

static long sys_fstat(int fd, uint64_t ust)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    struct fresh_stat st;
    memset(&st, 0, sizeof(st));
    if (f->type == FILE_VNODE) {
        int rc = f->mnt->ops->stat(f->mnt, f->ino, &st);
        if (rc)
            return rc;
    } else {
        st.type = f->type == FILE_TTY ? FT_DEV : FT_PIPE;
    }
    return copy_to_user((void *)ust, &st, sizeof(st));
}

static long sys_readdir(int fd, uint64_t uent)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    if (f->type != FILE_VNODE || f->ftype != FT_DIR)
        return -ENOTDIR;
    struct fresh_dirent d;
    memset(&d, 0, sizeof(d));
    int rc = f->mnt->ops->readdir(f->mnt, f->ino, &f->off, &d);
    if (rc == 1 && copy_to_user((void *)uent, &d, sizeof(d)))
        return -EFAULT;
    return rc;
}

static long sys_pipe(uint64_t ufds)
{
    if (!user_range_ok(ufds, 8))
        return -EFAULT;
    struct file *r, *w;
    int rc = pipe_create(&r, &w);
    if (rc)
        return rc;
    int a = alloc_fd(r);
    int b = a >= 0 ? alloc_fd(w) : -EMFILE;
    if (a < 0 || b < 0) {
        if (a >= 0)
            current->fds[a] = NULL;
        file_close(r);
        file_close(w);
        return -EMFILE;
    }
    int32_t fds[2] = {a, b};
    if (copy_to_user((void *)ufds, fds, sizeof(fds))) {
        current->fds[a] = current->fds[b] = NULL;
        file_close(r);
        file_close(w);
        return -EFAULT;
    }
    return 0;
}

static long sys_sbrk(int64_t incr)
{
    struct proc *p = current;
    uint64_t old = p->brk;
    if (incr == 0)
        return (long)old;
    uint64_t nb = old + (uint64_t)incr;
    if (incr > 0 && (nb < old || nb > USER_HEAP_LIMIT))
        return -ENOMEM;
    if (incr < 0 && (nb > old || nb < p->brk_start))
        return -EINVAL;
    if (incr > 0) {
        uint64_t need = (ALIGN_UP(nb, PAGE_SIZE) - ALIGN_UP(old, PAGE_SIZE)) / PAGE_SIZE;
        if (need + 64 > pmm_free_pages())
            return -ENOMEM;
        for (uint64_t va = ALIGN_UP(old, PAGE_SIZE); va < ALIGN_UP(nb, PAGE_SIZE); va += PAGE_SIZE) {
            if (!vmm_map_user_page(p->pml4, va, PTE_W | PTE_NX)) {
                for (uint64_t u = ALIGN_UP(old, PAGE_SIZE); u < va; u += PAGE_SIZE)
                    vmm_unmap_user_page(p->pml4, u);
                return -ENOMEM;
            }
        }
    } else {
        for (uint64_t va = ALIGN_UP(nb, PAGE_SIZE); va < ALIGN_UP(old, PAGE_SIZE); va += PAGE_SIZE)
            vmm_unmap_user_page(p->pml4, va);
    }
    p->brk = nb;
    return (long)old;
}

static long sys_sysinfo(uint64_t uinfo)
{
    struct fresh_sysinfo si;
    memset(&si, 0, sizeof(si));
    si.mem_total = pmm_total_pages() * PAGE_SIZE;
    si.mem_free = pmm_free_pages() * PAGE_SIZE;
    si.uptime_ms = uptime_ms();
    si.nprocs = (uint32_t)proc_count();
    si.boot_type = g_boot.boot_type;
    si.fb_width = g_boot.fb_type ? g_boot.fb_width : 0;
    si.fb_height = g_boot.fb_type ? g_boot.fb_height : 0;
    si.ndisks = (uint32_t)blk_count();
    si.data_mounted = data_fs_mounted();
    strlcpy(si.version, FRESH_VERSION, sizeof(si.version));
    extern char cpu_brand[48];
    const char *b = cpu_brand;
    while (*b == ' ')
        b++;
    strlcpy(si.cpu, b, sizeof(si.cpu));
    return copy_to_user((void *)uinfo, &si, sizeof(si));
}

static long sys_procinfo(int index, uint64_t uinfo)
{
    int n = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state == P_UNUSED)
            continue;
        if (n++ != index)
            continue;
        struct fresh_procinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.pid = p->pid;
        pi.ppid = p->ppid;
        pi.state = p->state == P_RUNNING ? PROC_STATE_RUNNING
                   : p->state == P_RUNNABLE ? PROC_STATE_RUNNABLE
                   : p->state == P_BLOCKED ? PROC_STATE_BLOCKED
                                           : PROC_STATE_ZOMBIE;
        pi.mem_bytes = p->pml4 ? vmm_count_user_pages(p->pml4) * PAGE_SIZE : 0;
        pi.cpu_ms = p->cpu_ticks * (1000 / TIMER_HZ);
        strlcpy(pi.name, p->name, sizeof(pi.name));
        if (copy_to_user((void *)uinfo, &pi, sizeof(pi)))
            return -EFAULT;
        return 1;
    }
    return 0;
}

/* the terminal a process talks to: its standard input if that is one */
static struct tty *proc_tty(void)
{
    struct file *f = current->fds[0];
    if (f && f->type == FILE_TTY && f->tty)
        return f->tty;
    return tty_console();
}

static long sys_pty_open(uint64_t ufds)
{
    if (!user_range_ok(ufds, 8))
        return -EFAULT;
    struct tty *t = pty_create();
    if (!t)
        return -ENOMEM;
    struct file *m = file_new(FILE_PTY_MASTER), *s = file_new(FILE_TTY);
    if (!m || !s) {
        kfree(m);
        kfree(s);
        pty_master_closed(t);
        pty_slave_closed(t);
        return -ENOMEM;
    }
    m->tty = s->tty = t;
    m->flags = s->flags = O_RDWR;
    int a = alloc_fd(m);
    int b = a >= 0 ? alloc_fd(s) : -EMFILE;
    int32_t fds[2] = {a, b};
    if (a < 0 || b < 0 || copy_to_user((void *)ufds, fds, sizeof(fds))) {
        if (a >= 0)
            current->fds[a] = NULL;
        if (b >= 0)
            current->fds[b] = NULL;
        file_close(m);
        file_close(s);
        return (a < 0 || b < 0) ? -EMFILE : -EFAULT;
    }
    return 0;
}

static long sys_pty_resize(int fd, int rows, int cols)
{
    struct file *f = get_fd(fd);
    if (!f)
        return -EBADF;
    if (f->type != FILE_PTY_MASTER)
        return -ENOTTY;
    if (rows < 1 || cols < 1 || rows > 500 || cols > 500)
        return -EINVAL;
    pty_resize(f->tty, rows, cols);
    return 0;
}

static long sys_input_read(uint64_t uev, uint64_t max, uint64_t timeout_ms)
{
    struct fresh_input_event ev[64];
    if (max > 64)
        max = 64;
    if (!user_range_ok(uev, max * sizeof(ev[0])))
        return -EFAULT;
    if (timeout_ms > 60000)
        timeout_ms = 60000;
    long n = input_read(ev, max, timeout_ms);
    if (n > 0 && copy_to_user((void *)uev, ev, (size_t)n * sizeof(ev[0])))
        return -EFAULT;
    return n;
}

static void shutdown_prepare(void)
{
    kprintf("\nSyncing file systems...\n");
    vfs_unmount_all();
}

static long sys_chdir(uint64_t upath)
{
    char path[PATH_MAX];
    int rc = user_path((const char *)upath, path);
    if (rc)
        return rc;
    struct fresh_stat st;
    if ((rc = vfs_stat(path, &st)) != 0)
        return rc;
    if (st.type != FT_DIR)
        return -ENOTDIR;
    strlcpy(current->cwd, path, sizeof(current->cwd));
    return 0;
}

static long sys_getcwd(uint64_t ubuf, uint64_t n)
{
    size_t len = strlen(current->cwd) + 1;
    if (n < len)
        return -ERANGE;
    return copy_to_user((void *)ubuf, current->cwd, len) ? -EFAULT : (long)len;
}

static long path_op(uint64_t upath, int op, uint64_t arg)
{
    char path[PATH_MAX];
    int rc = user_path((const char *)upath, path);
    if (rc)
        return rc;
    switch (op) {
    case SYS_MKDIR:
        return vfs_mkdir(path);
    case SYS_UNLINK:
        return vfs_unlink(path);
    case SYS_TRUNCATE:
        return vfs_truncate(path, arg);
    case SYS_STAT: {
        struct fresh_stat st;
        if ((rc = vfs_stat(path, &st)) != 0)
            return rc;
        return copy_to_user((void *)arg, &st, sizeof(st));
    }
    case SYS_STATFS: {
        struct fresh_statfs st;
        if ((rc = vfs_statfs(path, &st)) != 0)
            return rc;
        return copy_to_user((void *)arg, &st, sizeof(st));
    }
    default:
        return -ENOSYS;
    }
}

static long sys_rename(uint64_t uold, uint64_t unew)
{
    char a[PATH_MAX], b[PATH_MAX];
    int rc = user_path((const char *)uold, a);
    if (rc)
        return rc;
    if ((rc = user_path((const char *)unew, b)) != 0)
        return rc;
    return vfs_rename(a, b);
}

static long sys_test(int op, uint64_t arg)
{
    UNUSED(arg);
    switch (op) {
    case TEST_FREE_PAGES:
        /* pages the heap keeps cached for small blocks are not lost: count
         * them as free so concurrent activity cannot look like a leak */
        return (long)(pmm_free_pages() + heap_cache_pages());
    case TEST_HEAP_USED:
        return (long)heap_used_bytes();
    case TEST_SELFTEST:
        return ktest_run();
    case TEST_PANIC:
        if (!g_test_mode)
            return -EPERM;
        panic("test panic requested by pid %d", current->pid);
    default:
        return -EINVAL;
    }
}

void syscall_dispatch(struct trapframe *tf)
{
    uint64_t a0 = tf->rdi, a1 = tf->rsi, a2 = tf->rdx;
    long r;
    switch (tf->rax) {
    case SYS_EXIT:
        proc_exit((int)(a0 & 0xFF));
    case SYS_SPAWN:
        r = sys_spawn(a0);
        break;
    case SYS_WAIT: {
        int status = 0;
        r = proc_wait((int)a0, &status, (int)a2);
        if (r > 0 && a1 && copy_to_user((void *)a1, &status, sizeof(status)))
            r = -EFAULT;
        break;
    }
    case SYS_GETPID:
        r = current->pid;
        break;
    case SYS_GETPPID:
        r = current->ppid;
        break;
    case SYS_KILL:
        r = proc_kill((int)a0, EXIT_KILLED);
        break;
    case SYS_SLEEP:
        r = proc_sleep_ms(a0);
        break;
    case SYS_UPTIME:
        r = (long)uptime_ms();
        break;
    case SYS_OPEN:
        r = sys_open(a0, (int)a1);
        break;
    case SYS_CLOSE: {
        struct file *f = get_fd((int)a0);
        if (!f) {
            r = -EBADF;
        } else {
            current->fds[a0] = NULL;
            file_close(f);
            r = 0;
        }
        break;
    }
    case SYS_READ:
        r = sys_read((int)a0, a1, a2);
        break;
    case SYS_WRITE:
        r = sys_write((int)a0, a1, a2);
        break;
    case SYS_SEEK:
        r = sys_seek((int)a0, (int64_t)a1, (int)a2);
        break;
    case SYS_STAT:
    case SYS_MKDIR:
    case SYS_UNLINK:
    case SYS_STATFS:
    case SYS_TRUNCATE:
        r = path_op(a0, (int)tf->rax, a1);
        break;
    case SYS_FSTAT:
        r = sys_fstat((int)a0, a1);
        break;
    case SYS_READDIR:
        r = sys_readdir((int)a0, a1);
        break;
    case SYS_RENAME:
        r = sys_rename(a0, a1);
        break;
    case SYS_PIPE:
        r = sys_pipe(a0);
        break;
    case SYS_SBRK:
        r = sys_sbrk((int64_t)a0);
        break;
    case SYS_SYSINFO:
        r = sys_sysinfo(a0);
        break;
    case SYS_PROCINFO:
        r = sys_procinfo((int)a0, a1);
        break;
    case SYS_REBOOT:
        shutdown_prepare();
        kprintf("Restarting...\n");
        acpi_reboot();
    case SYS_POWEROFF:
        shutdown_prepare();
        kprintf("Powering off...\n");
        acpi_poweroff();
    case SYS_CHDIR:
        r = sys_chdir(a0);
        break;
    case SYS_GETCWD:
        r = sys_getcwd(a0, a1);
        break;
    case SYS_TIME:
        r = (long)rtc_unix_time();
        break;
    case SYS_TTYMODE: {
        struct tty *t = proc_tty();
        int old = tty_get_mode_on(t);
        if ((int64_t)a0 >= 0)
            tty_set_mode_on(t, a0 ? TTY_MODE_RAW : TTY_MODE_COOKED);
        r = old;
        break;
    }
    case SYS_TTYFG:
        tty_set_fg_on(proc_tty(), (int)a0);
        r = 0;
        break;
    case SYS_TTYSIZE: {
        int rows, cols;
        tty_size_on(proc_tty(), &rows, &cols);
        r = ((long)rows << 16) | cols;
        break;
    }
    case SYS_FB_ACQUIRE:
        r = fb_acquire(a0);
        break;
    case SYS_FB_RELEASE:
        r = fb_release();
        break;
    case SYS_INPUT_READ:
        r = sys_input_read(a0, a1, a2);
        break;
    case SYS_PTY_OPEN:
        r = sys_pty_open(a0);
        break;
    case SYS_PTY_RESIZE:
        r = sys_pty_resize((int)a0, (int)a1, (int)a2);
        break;
    case SYS_SYNC:
        vfs_sync_all();
        r = 0;
        break;
    case SYS_DUP: {
        struct file *f = get_fd((int)a0);
        if (!f) {
            r = -EBADF;
        } else {
            r = alloc_fd(f);
            if (r >= 0)
                file_ref(f);
        }
        break;
    }
    case SYS_YIELD:
        schedule();
        r = 0;
        break;
    case SYS_TEST:
        r = sys_test((int)a0, a1);
        break;
    default:
        r = -ENOSYS;
        break;
    }
    tf->rax = (uint64_t)r;
    proc_return_check(tf);
}

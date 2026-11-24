/* Fresh OS kernel - processes and scheduling. */
#ifndef PROC_H
#define PROC_H

#include "../kernel.h"
#include "../arch/arch.h"

#define MAX_PROCS 64
#define MAX_FDS 32
#define KSTACK_PAGES 4
#define KSTACK_SIZE (KSTACK_PAGES * PAGE_SIZE)

#define USER_BASE 0x400000ULL
#define USER_STACK_TOP 0x00007FFFFFFF0000ULL
#define USER_STACK_MAX (8ULL << 20)
#define USER_HEAP_LIMIT 0x0000700000000000ULL

enum proc_state { P_UNUSED = 0, P_RUNNABLE, P_RUNNING, P_BLOCKED, P_ZOMBIE };

/* exit status encoding: 0..255 normal exit; >= 256 terminated */
#define EXIT_KILLED 0x100  /* killed by kill() / Ctrl+C */
#define EXIT_FAULT 0x200   /* killed by a CPU exception (| vector) */

struct file;

struct proc {
    uint8_t fpu[512] __attribute__((aligned(16)));
    int pid;
    int ppid;
    int pgid;
    enum proc_state state;
    char name[32];
    uint64_t *pml4;
    uint64_t cr3;
    uint64_t ksp;
    uint64_t kstack_phys;
    uint64_t kstack_top;
    struct file *fds[MAX_FDS];
    char cwd[PATH_MAX];
    uint64_t brk_start, brk;
    uint64_t wake_tick;
    void *wait_chan;
    int exit_status;
    bool killed;
    int kill_status;
    uint64_t cpu_ticks;
    uint64_t slice;
    uint64_t start_tick;
};

extern struct proc *current;
extern struct proc g_procs[MAX_PROCS];

void proc_init(void);
int proc_spawn(const char *path, char *const argv[], int argc, struct file *fds[3], const char *cwd, int pgid);
int proc_kill_group(int pgid, int status);
__attribute__((noreturn)) void proc_exit(int status);
int proc_wait(int pid, int *status, int options);
int proc_kill(int pid, int status);
int proc_count(void);
struct proc *proc_find(int pid);

void schedule(void);
void sched_tick(struct trapframe *tf);
void sleep_on(void *chan); /* call with interrupts disabled */
void wakeup(void *chan);
int proc_sleep_ms(uint64_t ms);
void proc_return_check(struct trapframe *tf);
bool proc_demand_page(uint64_t addr);
void proc_fault(struct trapframe *tf, const char *what);
__attribute__((noreturn)) void idle_loop(void);

/* ELF loader (elf.c) */
struct elf_image {
    uint64_t entry;
    uint64_t brk;
};
int elf_load(uint64_t *pml4, const uint8_t *data, uint64_t size, struct elf_image *out);

#endif

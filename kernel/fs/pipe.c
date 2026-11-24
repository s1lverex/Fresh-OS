/* Fresh OS kernel - pipes (4 KiB ring buffer). */
#include "vfs.h"
#include "../mm/mm.h"
#include "../proc/proc.h"

#define PIPE_SIZE 4096

struct pipe {
    char buf[PIPE_SIZE];
    uint64_t r, w;
    int readers, writers;
};

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    struct file *a = file_new(FILE_PIPE_R), *b = file_new(FILE_PIPE_W);
    if (!a || !b) {
        kfree(a);
        kfree(b);
        kfree(p);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;
    a->pipe = b->pipe = p;
    a->flags = O_RDONLY;
    b->flags = O_WRONLY;
    *rd = a;
    *wr = b;
    return 0;
}

long pipe_read(struct pipe *p, void *buf, size_t n)
{
    if (n == 0)
        return 0;
    uint64_t f = irq_save();
    while (p->r == p->w) {
        if (p->writers == 0) {
            irq_restore(f);
            return 0;
        }
        if (current->killed) {
            irq_restore(f);
            return -EINTR;
        }
        sleep_on(p);
    }
    size_t got = 0;
    char *out = buf;
    while (got < n && p->r != p->w)
        out[got++] = p->buf[p->r++ % PIPE_SIZE];
    wakeup(p);
    irq_restore(f);
    return (long)got;
}

long pipe_write(struct pipe *p, const void *buf, size_t n)
{
    const char *in = buf;
    size_t done = 0;
    uint64_t f = irq_save();
    while (done < n) {
        if (p->readers == 0) {
            irq_restore(f);
            return done ? (long)done : -EPIPE;
        }
        if (current->killed) {
            irq_restore(f);
            return done ? (long)done : -EINTR;
        }
        if (p->w - p->r == PIPE_SIZE) {
            wakeup(p);
            sleep_on(p);
            continue;
        }
        while (done < n && p->w - p->r < PIPE_SIZE)
            p->buf[p->w++ % PIPE_SIZE] = in[done++];
        wakeup(p);
    }
    irq_restore(f);
    return (long)done;
}

void pipe_close(struct pipe *p, bool writer)
{
    uint64_t f = irq_save();
    if (writer)
        p->writers--;
    else
        p->readers--;
    wakeup(p);
    bool done = p->readers == 0 && p->writers == 0;
    irq_restore(f);
    if (done)
        kfree(p);
}

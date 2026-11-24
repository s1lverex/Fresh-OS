/* Fresh OS kernel - VFS: path resolution, mount table, path operations. */
#include "vfs.h"
#include "../mm/mm.h"
#include "../proc/proc.h"
#include "../dev/dev.h"

static struct mount mounts[MAX_MOUNTS];

int vfs_mount(const char *path, const struct fs_ops *ops, void *priv, uint64_t root)
{
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            strlcpy(mounts[i].path, path, sizeof(mounts[i].path));
            mounts[i].ops = ops;
            mounts[i].priv = priv;
            mounts[i].root = root;
            mounts[i].used = true;
            return 0;
        }
    }
    return -ENOSPC;
}

int vfs_resolve(const char *cwd, const char *path, char *out)
{
    if (!path || !path[0])
        return -ENOENT;
    size_t plen = strnlen(path, PATH_MAX + 1);
    if (plen > PATH_MAX)
        return -ENAMETOOLONG;
    char *tmp = kmalloc(PATH_MAX * 2 + 2);
    if (!tmp)
        return -ENOMEM;
    if (path[0] == '/') {
        strlcpy(tmp, path, PATH_MAX * 2 + 2);
    } else {
        strlcpy(tmp, cwd && cwd[0] ? cwd : "/", PATH_MAX * 2 + 2);
        strlcat(tmp, "/", PATH_MAX * 2 + 2);
        strlcat(tmp, path, PATH_MAX * 2 + 2);
    }
    size_t olen = 0;
    size_t stack[PATH_MAX / 2];
    int depth = 0;
    int rc = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (depth > 0)
                olen = stack[--depth];
            continue;
        }
        if (len > NAME_MAX) {
            rc = -ENAMETOOLONG;
            goto out;
        }
        if (olen + 1 + len >= PATH_MAX || depth >= (int)ARRAY_SIZE(stack)) {
            rc = -ENAMETOOLONG;
            goto out;
        }
        stack[depth++] = olen;
        out[olen++] = '/';
        memcpy(out + olen, s, len);
        olen += len;
    }
    if (olen == 0)
        out[olen++] = '/';
    out[olen] = 0;
out:
    kfree(tmp);
    return rc;
}

static struct mount *find_mount(const char *abspath, const char **rest)
{
    struct mount *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        struct mount *m = &mounts[i];
        if (!m->used)
            continue;
        size_t l = strlen(m->path);
        if (l == 1) { /* "/" */
            if (!best) {
                best = m;
                best_len = 0;
            }
            continue;
        }
        if (!strncmp(abspath, m->path, l) && (abspath[l] == 0 || abspath[l] == '/') && l > best_len) {
            best = m;
            best_len = l;
        }
    }
    if (rest)
        *rest = abspath + best_len;
    return best;
}

bool vfs_is_mountpoint(const char *abspath)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, abspath))
            return true;
    return false;
}

int vfs_lookup(const char *abspath, struct mount **mo, uint64_t *ino)
{
    const char *rest;
    struct mount *m = find_mount(abspath, &rest);
    if (!m)
        return -ENOENT;
    uint64_t cur = m->root;
    char name[NAME_MAX + 1];
    const char *p = rest;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len > NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(name, s, len);
        name[len] = 0;
        uint64_t next;
        int rc = m->ops->lookup(m, cur, name, &next);
        if (rc)
            return rc;
        cur = next;
    }
    *mo = m;
    *ino = cur;
    return 0;
}

/* split into parent directory (looked up) and final component */
static int lookup_parent(const char *abspath, struct mount **mo, uint64_t *dir, char *name)
{
    if (!strcmp(abspath, "/"))
        return -EBUSY;
    const char *slash = strrchr(abspath, '/');
    size_t nlen = strlen(slash + 1);
    if (nlen == 0 || nlen > NAME_MAX)
        return nlen ? -ENAMETOOLONG : -EINVAL;
    char parent[PATH_MAX];
    size_t plen = (size_t)(slash - abspath);
    if (plen == 0)
        plen = 1;
    memcpy(parent, abspath, plen);
    parent[plen] = 0;
    struct mount *m;
    uint64_t ino;
    int rc = vfs_lookup(parent, &m, &ino);
    if (rc)
        return rc;
    struct fresh_stat st;
    if ((rc = m->ops->stat(m, ino, &st)) != 0)
        return rc;
    if (st.type != FT_DIR)
        return -ENOTDIR;
    *mo = m;
    *dir = ino;
    strlcpy(name, slash + 1, NAME_MAX + 1);
    return 0;
}

int vfs_stat(const char *abspath, struct fresh_stat *st)
{
    struct mount *m;
    uint64_t ino;
    int rc = vfs_lookup(abspath, &m, &ino);
    if (rc)
        return rc;
    return m->ops->stat(m, ino, st);
}

int vfs_mkdir(const char *abspath)
{
    struct mount *m;
    uint64_t dir, ino;
    char name[NAME_MAX + 1];
    if (vfs_lookup(abspath, &m, &ino) == 0)
        return -EEXIST;
    int rc = lookup_parent(abspath, &m, &dir, name);
    if (rc)
        return rc;
    return m->ops->create(m, dir, name, FT_DIR, &ino);
}

int vfs_create_file(const char *abspath, uint64_t *ino_out, struct mount **m_out)
{
    struct mount *m;
    uint64_t dir;
    char name[NAME_MAX + 1];
    int rc = lookup_parent(abspath, &m, &dir, name);
    if (rc)
        return rc;
    rc = m->ops->create(m, dir, name, FT_FILE, ino_out);
    if (!rc && m_out)
        *m_out = m;
    return rc;
}

int vfs_unlink(const char *abspath)
{
    if (vfs_is_mountpoint(abspath) || !strcmp(abspath, "/"))
        return -EBUSY;
    struct mount *m;
    uint64_t dir, ino;
    char name[NAME_MAX + 1];
    int rc = vfs_lookup(abspath, &m, &ino);
    if (rc)
        return rc;
    if (vfs_file_in_use(m, ino))
        return -EBUSY;
    /* a process whose working directory is inside keeps it busy */
    size_t al = strlen(abspath);
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state == P_UNUSED || p->state == P_ZOMBIE)
            continue;
        if (!strncmp(p->cwd, abspath, al) && (p->cwd[al] == 0 || p->cwd[al] == '/'))
            return -EBUSY;
    }
    if ((rc = lookup_parent(abspath, &m, &dir, name)) != 0)
        return rc;
    return m->ops->unlink(m, dir, name);
}

int vfs_rename(const char *oldp, const char *newp)
{
    if (vfs_is_mountpoint(oldp) || vfs_is_mountpoint(newp) || !strcmp(oldp, "/") || !strcmp(newp, "/"))
        return -EBUSY;
    size_t ol = strlen(oldp);
    if (!strncmp(newp, oldp, ol) && newp[ol] == '/')
        return -EINVAL; /* moving a directory into itself */
    struct mount *om, *nm;
    uint64_t odir, ndir, ino;
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    int rc = vfs_lookup(oldp, &om, &ino);
    if (rc)
        return rc;
    if ((rc = lookup_parent(oldp, &om, &odir, oname)) != 0)
        return rc;
    if ((rc = lookup_parent(newp, &nm, &ndir, nname)) != 0)
        return rc;
    if (om != nm)
        return -EXDEV;
    struct mount *tm;
    uint64_t tino;
    if (vfs_lookup(newp, &tm, &tino) == 0 && tino != ino && vfs_file_in_use(tm, tino))
        return -EBUSY;
    return om->ops->rename(om, odir, oname, ndir, nname);
}

int vfs_statfs(const char *abspath, struct fresh_statfs *st)
{
    const char *rest;
    struct mount *m = find_mount(abspath, &rest);
    if (!m)
        return -ENOENT;
    return m->ops->statfs(m, st);
}

int vfs_truncate(const char *abspath, uint64_t size)
{
    struct mount *m;
    uint64_t ino;
    int rc = vfs_lookup(abspath, &m, &ino);
    if (rc)
        return rc;
    return m->ops->truncate(m, ino, size);
}

long vfs_read_all(const char *abspath, uint8_t **data, uint64_t *size, uint64_t max)
{
    struct mount *m;
    uint64_t ino;
    int rc = vfs_lookup(abspath, &m, &ino);
    if (rc)
        return rc;
    struct fresh_stat st;
    if ((rc = m->ops->stat(m, ino, &st)) != 0)
        return rc;
    if (st.type != FT_FILE)
        return -EISDIR;
    if (st.size > max)
        return -EFBIG;
    uint8_t *buf = kmalloc(st.size ? st.size : 1);
    if (!buf)
        return -ENOMEM;
    uint64_t done = 0;
    while (done < st.size) {
        long r = m->ops->read(m, ino, done, buf + done, st.size - done);
        if (r <= 0) {
            kfree(buf);
            return r < 0 ? r : -EIO;
        }
        done += (uint64_t)r;
    }
    *data = buf;
    *size = st.size;
    return 0;
}

int vfs_write_all(const char *abspath, const void *data, uint64_t size)
{
    struct file *f;
    int rc = vfs_open(abspath, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (rc)
        return rc;
    long w = size ? file_write(f, data, size) : 0;
    file_close(f);
    if (w < 0)
        return (int)w;
    return (uint64_t)w == size ? 0 : -EIO;
}

void vfs_sync_all(void)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ops->sync)
            mounts[i].ops->sync(&mounts[i]);
}

void vfs_unmount_all(void)
{
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ops->unmount)
            mounts[i].ops->unmount(&mounts[i]);
}

/* ------------------------------------------------------------ open files */
struct file *file_new(enum file_type t)
{
    struct file *f = kzalloc(sizeof(*f));
    if (f) {
        f->type = t;
        f->refs = 1;
    }
    return f;
}

void file_ref(struct file *f)
{
    if (f)
        f->refs++;
}

void file_close(struct file *f)
{
    if (!f || --f->refs > 0)
        return;
    if (f->type == FILE_PIPE_R || f->type == FILE_PIPE_W)
        pipe_close(f->pipe, f->type == FILE_PIPE_W);
    else if (f->type == FILE_TTY && f->tty && tty_is_pty(f->tty))
        pty_slave_closed(f->tty);
    else if (f->type == FILE_PTY_MASTER)
        pty_master_closed(f->tty);
    kfree(f);
}

long file_read(struct file *f, void *kbuf, size_t n)
{
    switch (f->type) {
    case FILE_VNODE: {
        if (f->ftype == FT_DIR)
            return -EISDIR;
        if ((f->flags & O_ACCMODE) == O_WRONLY)
            return -EBADF;
        long r = f->mnt->ops->read(f->mnt, f->ino, f->off, kbuf, n);
        if (r > 0)
            f->off += (uint64_t)r;
        return r;
    }
    case FILE_PIPE_R:
        return pipe_read(f->pipe, kbuf, n);
    case FILE_TTY:
        return tty_read_on(f->tty, kbuf, n);
    case FILE_PTY_MASTER:
        return pty_master_read(f->tty, kbuf, n);
    default:
        return -EBADF;
    }
}

long file_write(struct file *f, const void *kbuf, size_t n)
{
    switch (f->type) {
    case FILE_VNODE: {
        if (f->ftype == FT_DIR)
            return -EISDIR;
        if ((f->flags & O_ACCMODE) == O_RDONLY)
            return -EBADF;
        if (f->flags & O_APPEND) {
            struct fresh_stat st;
            int rc = f->mnt->ops->stat(f->mnt, f->ino, &st);
            if (rc)
                return rc;
            f->off = st.size;
        }
        long r = f->mnt->ops->write(f->mnt, f->ino, f->off, kbuf, n);
        if (r > 0)
            f->off += (uint64_t)r;
        return r;
    }
    case FILE_PIPE_W:
        return pipe_write(f->pipe, kbuf, n);
    case FILE_TTY:
        return tty_write_on(f->tty, kbuf, n);
    case FILE_PTY_MASTER:
        return pty_master_write(f->tty, kbuf, n);
    default:
        return -EBADF;
    }
}

int vfs_open(const char *abspath, int flags, struct file **out)
{
    struct mount *m;
    uint64_t ino;
    int rc = vfs_lookup(abspath, &m, &ino);
    if (rc == -ENOENT && (flags & O_CREAT)) {
        rc = vfs_create_file(abspath, &ino, &m);
        if (rc)
            return rc;
    } else if (rc) {
        return rc;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    struct fresh_stat st;
    if ((rc = m->ops->stat(m, ino, &st)) != 0)
        return rc;
    int acc = flags & O_ACCMODE;
    if (acc == 3)
        return -EINVAL;
    if (st.type == FT_DIR && acc != O_RDONLY)
        return -EISDIR;
    if ((flags & O_TRUNC) && acc != O_RDONLY && st.type == FT_FILE) {
        if ((rc = m->ops->truncate(m, ino, 0)) != 0)
            return rc;
    }
    struct file *f = file_new(FILE_VNODE);
    if (!f)
        return -ENOMEM;
    f->flags = flags;
    f->mnt = m;
    f->ino = ino;
    f->ftype = st.type;
    *out = f;
    return 0;
}

bool vfs_file_in_use(struct mount *m, uint64_t ino)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &g_procs[i];
        if (p->state == P_UNUSED)
            continue;
        for (int k = 0; k < MAX_FDS; k++) {
            struct file *f = p->fds[k];
            if (f && f->type == FILE_VNODE && f->mnt == m && f->ino == ino)
                return true;
        }
    }
    return false;
}

/* Fresh OS kernel - virtual file system, open files and pipes. */
#ifndef VFS_H
#define VFS_H

#include "../kernel.h"

struct mount;

struct fs_ops {
    int (*lookup)(struct mount *m, uint64_t dir, const char *name, uint64_t *ino);
    int (*stat)(struct mount *m, uint64_t ino, struct fresh_stat *st);
    long (*read)(struct mount *m, uint64_t ino, uint64_t off, void *buf, size_t n);
    long (*write)(struct mount *m, uint64_t ino, uint64_t off, const void *buf, size_t n);
    int (*truncate)(struct mount *m, uint64_t ino, uint64_t size);
    int (*create)(struct mount *m, uint64_t dir, const char *name, int type, uint64_t *ino);
    int (*unlink)(struct mount *m, uint64_t dir, const char *name);
    int (*rename)(struct mount *m, uint64_t odir, const char *oname, uint64_t ndir, const char *nname);
    int (*readdir)(struct mount *m, uint64_t dir, uint64_t *cursor, struct fresh_dirent *out);
    int (*statfs)(struct mount *m, struct fresh_statfs *out);
    int (*sync)(struct mount *m);
    int (*unmount)(struct mount *m);
};

struct mount {
    char path[32];
    const struct fs_ops *ops;
    void *priv;
    uint64_t root;
    bool used;
};

#define MAX_MOUNTS 4

int vfs_mount(const char *path, const struct fs_ops *ops, void *priv, uint64_t root);
/* canonical absolute path from cwd + path; returns 0 or -errno */
int vfs_resolve(const char *cwd, const char *path, char *out);
int vfs_lookup(const char *abspath, struct mount **m, uint64_t *ino);
int vfs_stat(const char *abspath, struct fresh_stat *st);
int vfs_mkdir(const char *abspath);
int vfs_unlink(const char *abspath);
int vfs_rename(const char *oldp, const char *newp);
int vfs_statfs(const char *abspath, struct fresh_statfs *st);
int vfs_truncate(const char *abspath, uint64_t size);
int vfs_create_file(const char *abspath, uint64_t *ino_out, struct mount **m_out);
long vfs_read_all(const char *abspath, uint8_t **data, uint64_t *size, uint64_t max);
int vfs_write_all(const char *abspath, const void *data, uint64_t size);
void vfs_sync_all(void);
void vfs_unmount_all(void);
bool vfs_is_mountpoint(const char *abspath);

/* open files */
enum file_type { FILE_NONE = 0, FILE_VNODE, FILE_PIPE_R, FILE_PIPE_W, FILE_TTY, FILE_PTY_MASTER };

struct tty;

struct pipe;

struct file {
    enum file_type type;
    int refs;
    int flags;
    struct mount *mnt;
    uint64_t ino;
    uint64_t off;
    uint32_t ftype; /* FT_FILE / FT_DIR for vnodes */
    struct pipe *pipe;
    struct tty *tty; /* FILE_TTY (terminal or pty slave), FILE_PTY_MASTER */
};

struct file *file_new(enum file_type t);
void file_ref(struct file *f);
void file_close(struct file *f);
long file_read(struct file *f, void *kbuf, size_t n);
long file_write(struct file *f, const void *kbuf, size_t n);
int vfs_open(const char *abspath, int flags, struct file **out);
bool vfs_file_in_use(struct mount *m, uint64_t ino);

int pipe_create(struct file **rd, struct file **wr);
long pipe_read(struct pipe *p, void *buf, size_t n);
long pipe_write(struct pipe *p, const void *buf, size_t n);
void pipe_close(struct pipe *p, bool writer);

/* file systems */
const struct fs_ops *ramfs_ops(void);
void *ramfs_new(uint64_t *root);
int initrd_unpack(const uint8_t *data, uint64_t size);
int data_fs_mount(void); /* find + mount the FreshFS partition at /data */
bool data_fs_mounted(void);

#endif

/* Fresh OS kernel - unpack the initrd archive into the root ramfs.
 * Format: "FRSHINIT" u32 count u32 reserved, then per entry:
 *   u32 type (1 file, 2 dir), u32 path_len, u64 size, path, pad to 8,
 *   data, pad to 8. Paths are relative to /. */
#include "vfs.h"
#include "../mm/mm.h"

static int mkdirs(const char *path)
{
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(buf))
        return -ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == 0) {
            char c = buf[i];
            buf[i] = 0;
            int rc = vfs_mkdir(buf);
            if (rc && rc != -EEXIST)
                return rc;
            buf[i] = c;
        }
    }
    return 0;
}

int initrd_unpack(const uint8_t *data, uint64_t size)
{
    if (size < 16 || memcmp(data, "FRSHINIT", 8) != 0) {
        kprintf("initrd: bad or missing archive\n");
        return -EINVAL;
    }
    uint32_t count = *(const uint32_t *)(data + 8);
    uint64_t off = 16;
    uint32_t files = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 16 > size)
            return -EINVAL;
        uint32_t type = *(const uint32_t *)(data + off);
        uint32_t plen = *(const uint32_t *)(data + off + 4);
        uint64_t fsize = *(const uint64_t *)(data + off + 8);
        off += 16;
        if (plen == 0 || plen >= PATH_MAX - 1 || off + plen > size)
            return -EINVAL;
        char path[PATH_MAX];
        path[0] = '/';
        memcpy(path + 1, data + off, plen);
        path[plen + 1] = 0;
        off = ALIGN_UP(off + plen, 8);
        if (fsize > size || off + fsize > size)
            return -EINVAL;
        int rc;
        if (type == 2) {
            rc = mkdirs(path);
        } else {
            char parent[PATH_MAX];
            strlcpy(parent, path, sizeof(parent));
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) {
                *slash = 0;
                mkdirs(parent);
            }
            rc = vfs_write_all(path, data + off, fsize);
            files++;
        }
        if (rc) {
            kprintf("initrd: %s: error %d\n", path, rc);
            return rc;
        }
        off = ALIGN_UP(off + fsize, 8);
    }
    kprintf("initrd: %u files unpacked\n", files);
    return 0;
}

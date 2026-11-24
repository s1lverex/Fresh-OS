/* Fresh OS kernel - block device registry and GPT partition lookup. */
#include "dev.h"
#include "../mm/mm.h"

static struct blkdev *devs[MAX_BLKDEVS];
static int ndevs;

int blk_register(struct blkdev *d)
{
    if (ndevs >= MAX_BLKDEVS)
        return -ENOSPC;
    devs[ndevs++] = d;
    return 0;
}

int blk_count(void) { return ndevs; }
struct blkdev *blk_get(int i) { return (i >= 0 && i < ndevs) ? devs[i] : NULL; }

int gpt_find(struct blkdev *d, const uint8_t type_guid[16], uint64_t *start, uint64_t *count)
{
    uint8_t *sec = kmalloc(512);
    if (!sec)
        return -ENOMEM;
    int rc = -ENOENT;
    if (d->read(d, 1, 1, sec) || memcmp(sec, "EFI PART", 8) != 0)
        goto out;
    uint64_t ent_lba = *(uint64_t *)(sec + 72);
    uint32_t nent = *(uint32_t *)(sec + 80);
    uint32_t esz = *(uint32_t *)(sec + 84);
    if (esz < 128 || esz > 512 || (512 % esz) != 0 || nent > 1024)
        goto out;
    uint32_t per = 512 / esz;
    for (uint32_t i = 0; i < nent; i++) {
        if (i % per == 0 && d->read(d, ent_lba + i / per, 1, sec))
            goto out;
        uint8_t *e = sec + (i % per) * esz;
        if (memcmp(e, type_guid, 16) != 0)
            continue;
        uint64_t first = *(uint64_t *)(e + 32), last = *(uint64_t *)(e + 40);
        if (last < first || last >= d->sectors)
            continue;
        *start = first;
        *count = last - first + 1;
        rc = 0;
        goto out;
    }
out:
    kfree(sec);
    return rc;
}

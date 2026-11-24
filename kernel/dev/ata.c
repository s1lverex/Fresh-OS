/* Fresh OS kernel - legacy IDE/ATA disks (PIO, LBA48), for PCs whose
 * firmware exposes the disk controller in IDE mode. */
#include "dev.h"

#define REG_DATA 0
#define REG_ERR 1
#define REG_COUNT 2
#define REG_LBA0 3
#define REG_LBA1 4
#define REG_LBA2 5
#define REG_DRIVE 6
#define REG_CMD 7
#define REG_STATUS 7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_BSY 0x80

struct ata_drive {
    uint16_t io, ctrl;
    uint8_t slave;
    struct blkdev bdev;
};

static struct ata_drive drives[4];
static int ndrives;

static void delay400(uint16_t ctrl)
{
    for (int i = 0; i < 4; i++)
        inb(ctrl);
}

static int wait_ready(struct ata_drive *d, bool need_drq)
{
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(d->io + REG_STATUS);
        if (s & ST_BSY) {
            if ((i & 0xFF) == 0xFF)
                udelay(1);
            continue;
        }
        if (s & (ST_ERR | ST_DF))
            return -EIO;
        if (!need_drq || (s & ST_DRQ))
            return 0;
    }
    return -EIO;
}

static void setup_lba48(struct ata_drive *d, uint64_t lba, uint16_t count)
{
    outb(d->io + REG_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
    delay400(d->ctrl);
    outb(d->io + REG_COUNT, (uint8_t)(count >> 8));
    outb(d->io + REG_LBA0, (uint8_t)(lba >> 24));
    outb(d->io + REG_LBA1, (uint8_t)(lba >> 32));
    outb(d->io + REG_LBA2, (uint8_t)(lba >> 40));
    outb(d->io + REG_COUNT, (uint8_t)count);
    outb(d->io + REG_LBA0, (uint8_t)lba);
    outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
    outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
}

static int ata_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = b->priv;
    uint16_t *w = buf;
    while (count) {
        uint16_t n = (uint16_t)MIN(count, 256u);
        if (wait_ready(d, false))
            return -EIO;
        setup_lba48(d, lba, n == 256 ? 0 : n);
        outb(d->io + REG_CMD, 0x24); /* READ SECTORS EXT */
        for (uint16_t s = 0; s < n; s++) {
            delay400(d->ctrl);
            if (wait_ready(d, true))
                return -EIO;
            for (int i = 0; i < 256; i++)
                *w++ = inw(d->io + REG_DATA);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *d = b->priv;
    const uint16_t *w = buf;
    while (count) {
        uint16_t n = (uint16_t)MIN(count, 256u);
        if (wait_ready(d, false))
            return -EIO;
        setup_lba48(d, lba, n == 256 ? 0 : n);
        outb(d->io + REG_CMD, 0x34); /* WRITE SECTORS EXT */
        for (uint16_t s = 0; s < n; s++) {
            delay400(d->ctrl);
            if (wait_ready(d, true))
                return -EIO;
            for (int i = 0; i < 256; i++)
                outw(d->io + REG_DATA, *w++);
        }
        if (wait_ready(d, false))
            return -EIO;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_flush(struct blkdev *b)
{
    struct ata_drive *d = b->priv;
    outb(d->io + REG_DRIVE, (uint8_t)(0xA0 | (d->slave << 4)));
    delay400(d->ctrl);
    outb(d->io + REG_CMD, 0xEA); /* FLUSH CACHE EXT */
    return wait_ready(d, false);
}

static void probe(uint16_t io, uint16_t ctrl, uint8_t slave)
{
    if (inb(io + REG_STATUS) == 0xFF) /* floating bus */
        return;
    outb(ctrl, 0x02); /* nIEN: no interrupts, we poll */
    outb(io + REG_DRIVE, (uint8_t)(0xA0 | (slave << 4)));
    delay400(ctrl);
    outb(io + REG_COUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_CMD, 0xEC); /* IDENTIFY */
    delay400(ctrl);
    if (inb(io + REG_STATUS) == 0)
        return;
    struct ata_drive tmp = {io, ctrl, slave, {{0}, 0, 0, 0, 0, 0, {0}}};
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(io + REG_STATUS);
        if (!(s & ST_BSY))
            break;
    }
    if (inb(io + REG_LBA1) || inb(io + REG_LBA2)) /* ATAPI / SATA signature */
        return;
    if (wait_ready(&tmp, true))
        return;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(io + REG_DATA);
    if (!(id[83] & (1 << 10))) /* need LBA48 */
        return;
    if (ndrives >= 4)
        return;
    struct ata_drive *d = &drives[ndrives];
    *d = tmp;
    d->bdev.sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 |
                      (uint64_t)id[103] << 48;
    for (int i = 0; i < 20; i++) {
        d->bdev.model[i * 2] = (char)(id[27 + i] >> 8);
        d->bdev.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->bdev.model[40] = 0;
    for (int i = 39; i >= 0 && d->bdev.model[i] == ' '; i--)
        d->bdev.model[i] = 0;
    snprintf(d->bdev.name, sizeof(d->bdev.name), "ide%d", ndrives);
    d->bdev.read = ata_read;
    d->bdev.write = ata_write;
    d->bdev.flush = ata_flush;
    d->bdev.priv = d;
    ndrives++;
    blk_register(&d->bdev);
    kprintf("ata: %s: %s, %llu MiB\n", d->bdev.name, d->bdev.model, (unsigned long long)(d->bdev.sectors >> 11));
}

void ata_init(void)
{
    struct pci_dev devs[64];
    int n = pci_scan(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].class_code != 0x01 || devs[i].subclass != 0x01)
            continue;
        uint16_t pio = 0x1F0, pctl = 0x3F6, sio = 0x170, sctl = 0x376;
        bool io;
        if (devs[i].prog_if & 0x01) {
            pio = (uint16_t)pci_bar(&devs[i], 0, &io);
            pctl = (uint16_t)(pci_bar(&devs[i], 1, &io) + 2);
        }
        if (devs[i].prog_if & 0x04) {
            sio = (uint16_t)pci_bar(&devs[i], 2, &io);
            sctl = (uint16_t)(pci_bar(&devs[i], 3, &io) + 2);
        }
        probe(pio, pctl, 0);
        probe(pio, pctl, 1);
        probe(sio, sctl, 0);
        probe(sio, sctl, 1);
        break;
    }
}

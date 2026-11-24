/* Fresh OS kernel - AHCI (SATA) disk driver, polled DMA. */
#include "dev.h"
#include "../mm/mm.h"

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C

#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0C
#define P_IS 0x10
#define P_IE 0x14
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI 0x38

#define CMD_ST (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)

#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define IS_TFES (1u << 30)

#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT 0xEA
#define ATA_IDENTIFY 0xEC

#define BOUNCE_SECTORS 128 /* 64 KiB */

struct ahci_port {
    volatile uint8_t *regs;
    uint8_t *mem;       /* command list + FIS + command table */
    uint64_t mem_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
    struct blkdev bdev;
};

static struct ahci_port ports[MAX_BLKDEVS];
static int nports;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }

static bool wait_clear(volatile uint8_t *r, uint32_t off, uint32_t mask, uint32_t us)
{
    for (uint32_t t = 0; t < us; t += 10) {
        if (!(rd(r, off) & mask))
            return true;
        udelay(10);
    }
    return false;
}

static void port_stop(volatile uint8_t *r)
{
    wr(r, P_CMD, rd(r, P_CMD) & ~CMD_ST);
    wait_clear(r, P_CMD, CMD_CR, 500000);
    wr(r, P_CMD, rd(r, P_CMD) & ~CMD_FRE);
    wait_clear(r, P_CMD, CMD_FR, 500000);
}

static void port_start(volatile uint8_t *r)
{
    wait_clear(r, P_CMD, CMD_CR, 500000);
    wr(r, P_CMD, rd(r, P_CMD) | CMD_FRE);
    wr(r, P_CMD, rd(r, P_CMD) | CMD_ST);
}

/* issue one command in slot 0 and poll for completion */
static int issue(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, bool write)
{
    volatile uint8_t *r = p->regs;
    if (!wait_clear(r, P_TFD, TFD_BSY | TFD_DRQ, 1000000))
        return -EIO;
    wr(r, P_IS, 0xFFFFFFFF);
    wr(r, P_SERR, 0xFFFFFFFF);

    uint32_t *hdr = (uint32_t *)p->mem; /* slot 0 */
    uint8_t *tbl = p->mem + 0x800;
    uint64_t tbl_phys = p->mem_phys + 0x800;
    memset(hdr, 0, 32);
    hdr[0] = 5 | (write ? (1u << 6) : 0) | ((bytes ? 1u : 0u) << 16);
    hdr[2] = (uint32_t)tbl_phys;
    hdr[3] = (uint32_t)(tbl_phys >> 32);
    memset(tbl, 0, 0x100);
    uint8_t *fis = tbl;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = cmd;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
    if (bytes) {
        uint32_t *prd = (uint32_t *)(tbl + 0x80);
        prd[0] = (uint32_t)p->bounce_phys;
        prd[1] = (uint32_t)(p->bounce_phys >> 32);
        prd[2] = 0;
        prd[3] = (bytes - 1) & 0x3FFFFF;
    }
    __asm__ volatile("mfence" ::: "memory");
    wr(r, P_CI, 1);
    for (uint32_t t = 0;; t += 5) {
        if (!(rd(r, P_CI) & 1))
            break;
        if (rd(r, P_IS) & IS_TFES)
            return -EIO;
        if (t > 5000000) { /* 5 s */
            kprintf("ahci: %s command %#x timed out\n", p->bdev.name, cmd);
            port_stop(r);
            port_start(r);
            return -EIO;
        }
        udelay(5);
    }
    if (rd(r, P_TFD) & TFD_ERR)
        return -EIO;
    return 0;
}

static int ahci_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = d->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, BOUNCE_SECTORS);
        int rc = issue(p, ATA_READ_DMA_EXT, lba, n, n * 512, false);
        if (rc)
            return rc;
        memcpy(out, p->bounce, n * 512);
        out += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = d->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, BOUNCE_SECTORS);
        memcpy(p->bounce, in, n * 512);
        int rc = issue(p, ATA_WRITE_DMA_EXT, lba, n, n * 512, true);
        if (rc)
            return rc;
        in += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_flush(struct blkdev *d)
{
    return issue(d->priv, ATA_FLUSH_EXT, 0, 0, 0, false);
}

static void probe_port(volatile uint8_t *hba, int idx, bool s64)
{
    volatile uint8_t *r = hba + 0x100 + idx * 0x80;
    uint32_t ssts = rd(r, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
        return;
    if (rd(r, P_SIG) != 0x00000101) /* plain SATA disk only */
        return;
    if (nports >= MAX_BLKDEVS)
        return;
    struct ahci_port *p = &ports[nports];
    uint64_t lim = s64 ? 0 : 0x100000000ULL;
    p->mem_phys = pmm_alloc_contig(1, lim);
    p->bounce_phys = pmm_alloc_contig(BOUNCE_SECTORS * 512 / PAGE_SIZE, lim);
    if (!p->mem_phys || !p->bounce_phys)
        return;
    p->mem = phys_to_virt(p->mem_phys);
    p->bounce = phys_to_virt(p->bounce_phys);
    p->regs = r;

    port_stop(r);
    wr(r, P_CLB, (uint32_t)p->mem_phys);
    wr(r, P_CLBU, (uint32_t)(p->mem_phys >> 32));
    wr(r, P_FB, (uint32_t)(p->mem_phys + 0x400));
    wr(r, P_FBU, (uint32_t)((p->mem_phys + 0x400) >> 32));
    wr(r, P_IE, 0);
    wr(r, P_IS, 0xFFFFFFFF);
    wr(r, P_SERR, 0xFFFFFFFF);
    wr(r, P_CMD, rd(r, P_CMD) | CMD_SUD | CMD_POD);
    port_start(r);

    if (issue(p, ATA_IDENTIFY, 0, 0, 512, false)) {
        kprintf("ahci: port %d: IDENTIFY failed\n", idx);
        return;
    }
    uint16_t *id = (uint16_t *)p->bounce;
    uint64_t sectors;
    if (id[83] & (1 << 10))
        sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
    else
        sectors = (uint64_t)id[60] | (uint64_t)id[61] << 16;
    for (int i = 0; i < 20; i++) {
        p->bdev.model[i * 2] = (char)(id[27 + i] >> 8);
        p->bdev.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    p->bdev.model[40] = 0;
    for (int i = 39; i >= 0 && p->bdev.model[i] == ' '; i--)
        p->bdev.model[i] = 0;
    snprintf(p->bdev.name, sizeof(p->bdev.name), "sata%d", nports);
    p->bdev.sectors = sectors;
    p->bdev.read = ahci_read;
    p->bdev.write = ahci_write;
    p->bdev.flush = ahci_flush;
    p->bdev.priv = p;
    nports++;
    blk_register(&p->bdev);
    kprintf("ahci: %s: %s, %llu MiB\n", p->bdev.name, p->bdev.model, (unsigned long long)(sectors >> 11));
}

void ahci_init(void)
{
    struct pci_dev devs[64];
    int n = pci_scan(devs, 64);
    for (int i = 0; i < n; i++) {
        if (devs[i].class_code != 0x01 || devs[i].subclass != 0x06 || devs[i].prog_if != 0x01)
            continue;
        bool io;
        uint64_t abar = pci_bar(&devs[i], 5, &io);
        if (io || !abar)
            continue;
        pci_enable_bus_master(&devs[i]);
        volatile uint8_t *hba = vmm_map_mmio(abar, 0x1100, false);
        if (!hba)
            continue;
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) | (1u << 31)); /* AHCI enable */
        uint32_t cap = rd(hba, HBA_CAP);
        uint32_t pi = rd(hba, HBA_PI);
        kprintf("ahci: controller %04x:%04x, ports %#x\n", devs[i].vendor, devs[i].device, pi);
        for (int pidx = 0; pidx < 32; pidx++)
            if (pi & (1u << pidx))
                probe_port(hba, pidx, cap & (1u << 31));
    }
}

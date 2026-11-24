/* Fresh OS kernel - PCI configuration space (mechanism #1). */
#include "dev.h"

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
    uint64_t f = irq_save();
    outl(0xCF8, addr);
    uint32_t v = inl(0xCFC);
    irq_restore(f);
    return v;
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
    uint64_t f = irq_save();
    outl(0xCF8, addr);
    outl(0xCFC, v);
    irq_restore(f);
}

int pci_scan(struct pci_dev *out, int max)
{
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = (hdr & 0x00800000) ? 8 : 1;
            for (int fn = 0; fn < nfn; fn++) {
                id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                if (n >= max)
                    return n;
                uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                out[n].bus = (uint8_t)bus;
                out[n].dev = (uint8_t)dev;
                out[n].fn = (uint8_t)fn;
                out[n].vendor = id & 0xFFFF;
                out[n].device = id >> 16;
                out[n].class_code = cls >> 24;
                out[n].subclass = (cls >> 16) & 0xFF;
                out[n].prog_if = (cls >> 8) & 0xFF;
                n++;
            }
        }
    }
    return n;
}

uint64_t pci_bar(const struct pci_dev *d, int bar, bool *is_io)
{
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, off);
    if (v & 1) {
        if (is_io)
            *is_io = true;
        return v & ~3u;
    }
    if (is_io)
        *is_io = false;
    uint64_t addr = v & ~0xFu;
    if (((v >> 1) & 3) == 2 && bar < 5)
        addr |= (uint64_t)pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4)) << 32;
    return addr;
}

void pci_enable_bus_master(const struct pci_dev *d)
{
    uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= (1u << 1) | (1u << 2); /* memory space + bus master */
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

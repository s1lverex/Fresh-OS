/* Fresh OS kernel - minimal ACPI: find the FADT, power off (S5) and reset. */
#include "dev.h"
#include "../mm/mm.h"

static uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
static uint8_t acpi_enable_val;
static uint16_t slp_typa = 0xFFFF, slp_typb;
static bool reset_supported;
static uint8_t reset_space, reset_value;
static uint64_t reset_addr;

static void *acpi_map(uint64_t phys, uint64_t len)
{
    if (phys + len <= 0x100000000ULL)
        return phys_to_virt(phys);
    return vmm_map_mmio(phys, len, false);
}

static bool checksum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    return s == 0;
}

static uint64_t find_rsdp(void)
{
    if (g_boot.rsdp)
        return g_boot.rsdp;
    uint64_t ebda = (uint64_t)(*(uint16_t *)phys_to_virt(0x40E)) << 4;
    uint64_t ranges[2][2] = {{ebda, ebda + 1024}, {0xE0000, 0x100000}};
    for (int r = 0; r < 2; r++) {
        if (ranges[r][0] < 0x80000 && r == 0 && ebda == 0)
            continue;
        for (uint64_t a = ranges[r][0] & ~15ULL; a + 20 <= ranges[r][1]; a += 16) {
            uint8_t *p = phys_to_virt(a);
            if (!memcmp(p, "RSD PTR ", 8) && checksum_ok(p, 20))
                return a;
        }
    }
    return 0;
}

static void parse_s5(const uint8_t *dsdt, uint32_t len)
{
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0)
            continue;
        if (!(i >= 1 && (dsdt[i - 1] == 0x08 || (i >= 2 && dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))))
            continue;
        const uint8_t *p = dsdt + i + 4;
        const uint8_t *end = dsdt + len;
        if (*p++ != 0x12) /* PackageOp */
            continue;
        p += ((*p >> 6) & 3) + 1; /* PkgLength */
        p++;                      /* NumElements */
        uint16_t vals[2];
        for (int k = 0; k < 2 && p < end; k++) {
            if (*p == 0x0A) { /* BytePrefix */
                p++;
                vals[k] = *p++;
            } else if (*p == 0x00 || *p == 0x01) {
                vals[k] = *p++;
            } else if (*p == 0xFF) {
                vals[k] = 0xFF;
                p++;
            } else {
                vals[k] = 0;
                p++;
            }
        }
        slp_typa = vals[0] & 7;
        slp_typb = vals[1] & 7;
        return;
    }
}

void acpi_init(void)
{
    uint64_t rsdp_phys = find_rsdp();
    if (!rsdp_phys) {
        kprintf("acpi: no RSDP found\n");
        return;
    }
    uint8_t *rsdp = acpi_map(rsdp_phys, 36);
    uint8_t rev = rsdp[15];
    uint64_t root;
    bool xsdt = false;
    if (rev >= 2 && *(uint64_t *)(rsdp + 24)) {
        root = *(uint64_t *)(rsdp + 24);
        xsdt = true;
    } else {
        root = *(uint32_t *)(rsdp + 16);
    }
    uint8_t *hdr = acpi_map(root, 36);
    uint32_t len = *(uint32_t *)(hdr + 4);
    if (len < 36 || len > 0x100000)
        return;
    hdr = acpi_map(root, len);
    uint32_t n = (len - 36) / (xsdt ? 8 : 4);
    uint8_t *fadt = NULL;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t a = xsdt ? *(uint64_t *)(hdr + 36 + i * 8) : *(uint32_t *)(hdr + 36 + i * 4);
        uint8_t *t = acpi_map(a, 36);
        if (!memcmp(t, "FACP", 4)) {
            fadt = acpi_map(a, *(uint32_t *)(t + 4));
            break;
        }
    }
    if (!fadt) {
        kprintf("acpi: no FADT\n");
        return;
    }
    uint32_t flen = *(uint32_t *)(fadt + 4);
    smi_cmd = *(uint32_t *)(fadt + 48);
    acpi_enable_val = fadt[52];
    pm1a_cnt = *(uint32_t *)(fadt + 64);
    pm1b_cnt = *(uint32_t *)(fadt + 68);
    uint64_t dsdt = *(uint32_t *)(fadt + 40);
    if (flen >= 148 && *(uint64_t *)(fadt + 140))
        dsdt = *(uint64_t *)(fadt + 140);
    if (flen >= 129) {
        uint32_t flags = *(uint32_t *)(fadt + 112);
        reset_supported = flags & (1u << 10);
        reset_space = fadt[116];
        reset_addr = *(uint64_t *)(fadt + 120);
        reset_value = fadt[128];
    }
    if (dsdt) {
        uint8_t *d = acpi_map(dsdt, 36);
        uint32_t dl = *(uint32_t *)(d + 4);
        if (dl > 36 && dl < 0x400000) {
            d = acpi_map(dsdt, dl);
            parse_s5(d + 36, dl - 36);
        }
    }
    kprintf("acpi: rev %u, PM1a_CNT %#x, S5 %s\n", rev, pm1a_cnt, slp_typa != 0xFFFF ? "found" : "missing");
}

void acpi_poweroff(void)
{
    __asm__ volatile("cli");
    if (pm1a_cnt && slp_typa != 0xFFFF) {
        if (smi_cmd && acpi_enable_val && !(inw((uint16_t)pm1a_cnt) & 1)) {
            outb((uint16_t)smi_cmd, acpi_enable_val);
            for (int i = 0; i < 300 && !(inw((uint16_t)pm1a_cnt) & 1); i++)
                udelay(10000);
        }
        outw((uint16_t)pm1a_cnt, (uint16_t)((slp_typa << 10) | (1 << 13)));
        if (pm1b_cnt)
            outw((uint16_t)pm1b_cnt, (uint16_t)((slp_typb << 10) | (1 << 13)));
        udelay(100000);
    }
    /* virtual machine fallbacks */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    udelay(100000);
    kprintf("\nIt is now safe to turn off your computer.\n");
    cpu_halt_forever();
}

void acpi_reboot(void)
{
    __asm__ volatile("cli");
    if (reset_supported && reset_addr) {
        if (reset_space == 1)
            outb((uint16_t)reset_addr, reset_value);
        else if (reset_space == 0) {
            volatile uint8_t *p = acpi_map(reset_addr, 1);
            *p = reset_value;
        }
        udelay(50000);
    }
    for (int i = 0; i < 10000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xFE); /* keyboard controller reset line */
    udelay(50000);
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" ::"m"(null_idt)); /* triple fault */
    cpu_halt_forever();
}

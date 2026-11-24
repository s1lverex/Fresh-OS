/* Fresh OS - UEFI loader (BOOTX64.EFI).
 * Loads \FRESH\KERNEL.BIN and \FRESH\INITRD from the volume it was started
 * from, sets up the framebuffer, builds page tables, exits boot services and
 * jumps to the kernel with RDI = struct boot_info *. */
#include "efi.h"
#include "../../include/fresh/bootinfo.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

static EFI_GUID LoadedImageGuid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID SfsGuid = {0x964E5B22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID GopGuid = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
static EFI_GUID Acpi20Guid = {0x8868E871, 0xE4F1, 0x11d3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static EFI_GUID Acpi10Guid = {0xEB9D2D30, 0x2D88, 0x11d3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

#define MMAP_MAX_ENTRIES 512
#define PAGE 4096ULL
#define TWO_MB 0x200000ULL

/* The compiler may emit calls to these. */
void *memset(void *d, int c, UINTN n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, UINTN n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

static void puts16(const CHAR16 *s)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

static void put_hex(uint64_t v)
{
    CHAR16 buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (CHAR16)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    buf[18] = 0;
    puts16(buf);
}

static void fail(const CHAR16 *msg, EFI_STATUS st)
{
    puts16(u"\r\nFresh OS loader error: ");
    puts16(msg);
    puts16(u" (status ");
    put_hex(st);
    puts16(u")\r\nSystem halted.\r\n");
    for (;;)
        __asm__ volatile("hlt");
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static EFI_PHYSICAL_ADDRESS alloc_pages(UINTN pages, const CHAR16 *what)
{
    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(st))
        fail(what, st);
    memset((void *)addr, 0, pages * PAGE);
    return addr;
}

/* Read a whole file into freshly allocated pages. Returns 0 if missing. */
static int read_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, EFI_PHYSICAL_ADDRESS *out, uint64_t *size, int required)
{
    EFI_FILE_PROTOCOL *f;
    EFI_STATUS st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) {
        if (required)
            fail(path, st);
        return 0;
    }
    uint64_t len = 0;
    st = f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);
    if (!EFI_ERROR(st))
        st = f->GetPosition(f, &len);
    if (!EFI_ERROR(st))
        st = f->SetPosition(f, 0);
    if (EFI_ERROR(st))
        fail(path, st);
    EFI_PHYSICAL_ADDRESS buf = alloc_pages((len + PAGE - 1) / PAGE + 1, path);
    uint64_t done = 0;
    while (done < len) {
        UINTN chunk = len - done;
        if (chunk > 0x100000)
            chunk = 0x100000;
        st = f->Read(f, &chunk, (void *)(buf + done));
        if (EFI_ERROR(st))
            fail(path, st);
        if (chunk == 0)
            fail(u"short read", 0);
        done += chunk;
    }
    f->Close(f);
    *out = buf;
    *size = len;
    return 1;
}

static uint8_t mask_pos(uint32_t m)
{
    uint8_t p = 0;
    if (!m)
        return 0;
    while (!(m & 1)) {
        m >>= 1;
        p++;
    }
    return p;
}

static uint8_t mask_size(uint32_t m)
{
    uint8_t s = 0;
    while (m) {
        s += m & 1;
        m >>= 1;
    }
    return s;
}

static void setup_gop(struct boot_info *bi)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(BS->LocateProtocol(&GopGuid, 0, (void **)&gop)) || !gop) {
        bi->fb_type = BOOT_FB_NONE;
        return;
    }
    /* Prefer 1024x768, otherwise keep the current mode unless it is huge. */
    uint32_t best = gop->Mode->Mode;
    int found_pref = 0;
    uint64_t best_area = 0;
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN sz;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)))
            continue;
        if (info->PixelFormat == PixelBltOnly)
            continue;
        uint32_t w = info->HorizontalResolution, h = info->VerticalResolution;
        if (w == 1024 && h == 768) {
            best = m;
            found_pref = 1;
            break;
        }
        if (w <= 1280 && h <= 1024 && (uint64_t)w * h > best_area) {
            best_area = (uint64_t)w * h;
            best = m;
        }
    }
    (void)found_pref;
    if (best != gop->Mode->Mode)
        gop->SetMode(gop, best);
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    if (info->PixelFormat == PixelBltOnly) {
        bi->fb_type = BOOT_FB_NONE;
        return;
    }
    bi->fb_addr = gop->Mode->FrameBufferBase;
    bi->fb_width = info->HorizontalResolution;
    bi->fb_height = info->VerticalResolution;
    bi->fb_pitch = info->PixelsPerScanLine * 4;
    bi->fb_bpp = 32;
    bi->fb_type = BOOT_FB_RGB;
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        bi->fb_red_pos = 0;
        bi->fb_green_pos = 8;
        bi->fb_blue_pos = 16;
    } else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        bi->fb_red_pos = 16;
        bi->fb_green_pos = 8;
        bi->fb_blue_pos = 0;
    } else {
        bi->fb_red_pos = mask_pos(info->PixelInformation.RedMask);
        bi->fb_green_pos = mask_pos(info->PixelInformation.GreenMask);
        bi->fb_blue_pos = mask_pos(info->PixelInformation.BlueMask);
    }
    bi->fb_red_size = bi->fb_green_size = bi->fb_blue_size = 8;
    if (info->PixelFormat == PixelBitMask) {
        bi->fb_red_size = mask_size(info->PixelInformation.RedMask);
        bi->fb_green_size = mask_size(info->PixelInformation.GreenMask);
        bi->fb_blue_size = mask_size(info->PixelInformation.BlueMask);
    }
}

static uint32_t convert_type(uint32_t t)
{
    switch (t) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
        return BOOT_MMAP_USABLE;
    case EfiACPIReclaimMemory:
        return BOOT_MMAP_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:
        return BOOT_MMAP_ACPI_NVS;
    case EfiUnusableMemory:
        return BOOT_MMAP_BAD;
    default:
        return BOOT_MMAP_RESERVED;
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS st;
    ST = systab;
    BS = systab->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    puts16(u"Fresh OS boot (UEFI)\r\n");

    EFI_LOADED_IMAGE_PROTOCOL *li;
    st = BS->HandleProtocol(image, &LoadedImageGuid, (void **)&li);
    if (EFI_ERROR(st))
        fail(u"loaded image protocol", st);
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    st = BS->HandleProtocol(li->DeviceHandle, &SfsGuid, (void **)&sfs);
    if (EFI_ERROR(st))
        fail(u"boot volume file system", st);
    EFI_FILE_PROTOCOL *root;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st))
        fail(u"open boot volume", st);

    /* scratch area: boot_info + memory map, page tables appended later */
    EFI_PHYSICAL_ADDRESS info_page = alloc_pages(1 + (MMAP_MAX_ENTRIES * sizeof(struct boot_mmap_entry) + PAGE - 1) / PAGE,
                                                 u"boot info");
    struct boot_info *bi = (struct boot_info *)info_page;
    struct boot_mmap_entry *bmap = (struct boot_mmap_entry *)(info_page + PAGE);
    bi->magic = BOOTINFO_MAGIC;
    bi->version = BOOTINFO_VERSION;
    bi->boot_type = BOOT_TYPE_UEFI;

    /* kernel */
    puts16(u"Loading kernel\r\n");
    EFI_PHYSICAL_ADDRESS kfile;
    uint64_t ksize;
    read_file(root, u"\\FRESH\\KERNEL.BIN", &kfile, &ksize, 1);
    struct kernel_header *kh = (struct kernel_header *)kfile;
    if (ksize < sizeof(*kh) || kh->magic != KERNEL_MAGIC || kh->load_size > ksize - sizeof(*kh) ||
        kh->mem_size < kh->load_size || kh->mem_size > 0x40000000ULL)
        fail(u"KERNEL.BIN is invalid", 0);
    UINTN kpages = (kh->mem_size + TWO_MB + PAGE - 1) / PAGE;
    EFI_PHYSICAL_ADDRESS kraw = alloc_pages(kpages, u"kernel memory");
    EFI_PHYSICAL_ADDRESS kphys = (kraw + TWO_MB - 1) & ~(TWO_MB - 1);
    memcpy((void *)kphys, (void *)(kfile + sizeof(*kh)), kh->load_size);
    bi->kernel_phys = kphys;
    bi->kernel_size = kh->mem_size;
    uint64_t kentry = kh->entry;
    uint64_t kvirt = kh->virt_base;

    /* initrd */
    puts16(u"Loading initrd\r\n");
    EFI_PHYSICAL_ADDRESS irq;
    uint64_t isize;
    if (read_file(root, u"\\FRESH\\INITRD", &irq, &isize, 0)) {
        bi->initrd_phys = irq;
        bi->initrd_size = isize;
    }

    /* optional kernel command line */
    EFI_PHYSICAL_ADDRESS cl;
    uint64_t clsize;
    if (read_file(root, u"\\FRESH\\CMDLINE", &cl, &clsize, 0)) {
        uint64_t n = clsize < sizeof(bi->cmdline) - 1 ? clsize : sizeof(bi->cmdline) - 1;
        const char *src = (const char *)cl;
        uint64_t j = 0;
        for (uint64_t i = 0; i < n; i++) {
            if (src[i] == '\r' || src[i] == '\n')
                break;
            bi->cmdline[j++] = src[i];
        }
        bi->cmdline[j] = 0;
    }

    /* ACPI */
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &Acpi20Guid)) {
            bi->rsdp = (uint64_t)t->VendorTable;
            break;
        }
        if (guid_eq(&t->VendorGuid, &Acpi10Guid))
            bi->rsdp = (uint64_t)t->VendorTable;
    }

    setup_gop(bi);

    /* memory map buffer + page tables sized from a first look at the map */
    UINTN mmap_size = 0, map_key, desc_size;
    uint32_t desc_ver;
    st = BS->GetMemoryMap(&mmap_size, 0, &map_key, &desc_size, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL)
        fail(u"GetMemoryMap size", st);
    UINTN mmap_cap = mmap_size + 64 * desc_size;
    EFI_PHYSICAL_ADDRESS mmap_buf = alloc_pages((mmap_cap + PAGE - 1) / PAGE, u"memory map");
    mmap_size = mmap_cap;
    st = BS->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(st))
        fail(u"GetMemoryMap", st);
    uint64_t max_addr = 0x100000000ULL;
    for (UINTN off = 0; off < mmap_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE;
        if (end > max_addr)
            max_addr = end;
    }
    if (bi->fb_type && bi->fb_addr + (uint64_t)bi->fb_pitch * bi->fb_height > max_addr)
        max_addr = bi->fb_addr + (uint64_t)bi->fb_pitch * bi->fb_height;
    if (max_addr > 0x8000000000ULL)
        max_addr = 0x8000000000ULL; /* identity map at most 512 GiB */
    uint64_t gigs = (max_addr + 0x3FFFFFFFULL) >> 30;
    uint64_t kgigs = (kh->mem_size + 0x3FFFFFFFULL) >> 30; /* PDs for the kernel map */
    UINTN pt_pages = 3 + gigs + kgigs; /* PML4, PDPT lo, PDPT hi, PDs */
    EFI_PHYSICAL_ADDRESS pt = alloc_pages(pt_pages, u"page tables");
    uint64_t *pml4 = (uint64_t *)pt;
    uint64_t *pdpt_lo = (uint64_t *)(pt + PAGE);
    uint64_t *pdpt_hi = (uint64_t *)(pt + 2 * PAGE);
    pml4[0] = (uint64_t)pdpt_lo | 3;
    for (uint64_t g = 0; g < gigs; g++) {
        uint64_t *pd = (uint64_t *)(pt + (3 + g) * PAGE);
        pdpt_lo[g] = (uint64_t)pd | 3;
        for (int i = 0; i < 512; i++)
            pd[i] = ((g << 30) + (uint64_t)i * TWO_MB) | 0x83;
    }
    /* kernel: kvirt must be in the top 512 GiB slot (PML4 index 511) */
    uint64_t pml4_i = (kvirt >> 39) & 511, pdpt_i = (kvirt >> 30) & 511;
    pml4[pml4_i] = (uint64_t)pdpt_hi | 3;
    uint64_t kpages2m = (kh->mem_size + TWO_MB - 1) / TWO_MB;
    for (uint64_t i = 0; i < kpages2m; i++) {
        uint64_t slot = pdpt_i + i / 512;
        uint64_t *pd = (uint64_t *)(pt + (3 + gigs + i / 512) * PAGE);
        pdpt_hi[slot] = (uint64_t)pd | 3;
        pd[i % 512] = (kphys + i * TWO_MB) | 0x83;
    }
    bi->scratch_phys = pt;
    bi->scratch_size = pt_pages * PAGE;
    /* boot_info page and the memory map pages are reserved via mmap_addr */

    puts16(u"Starting kernel\r\n");

    /* final memory map + ExitBootServices (retry once if the key changed) */
    for (int attempt = 0;; attempt++) {
        mmap_size = mmap_cap;
        st = BS->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(st))
            fail(u"GetMemoryMap (final)", st);
        st = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(st))
            break;
        if (attempt >= 4)
            fail(u"ExitBootServices", st);
    }
    /* No firmware calls from here on. */
    __asm__ volatile("cli");

    uint32_t n = 0;
    for (UINTN off = 0; off < mmap_size && n < MMAP_MAX_ENTRIES; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
        uint32_t type = convert_type(d->Type);
        uint64_t base = d->PhysicalStart, len = d->NumberOfPages * PAGE;
        if (n && bmap[n - 1].type == type && bmap[n - 1].base + bmap[n - 1].length == base) {
            bmap[n - 1].length += len;
            continue;
        }
        bmap[n].base = base;
        bmap[n].length = len;
        bmap[n].type = type;
        bmap[n].attr = 1;
        n++;
    }
    bi->mmap_addr = (uint64_t)bmap;
    bi->mmap_count = n;
    bi->mmap_entry_size = sizeof(struct boot_mmap_entry);

    __asm__ volatile("mov %0, %%cr3\n\t"
                     "jmp *%2\n\t"
                     :
                     : "a"(pt), "D"(bi), "b"(kentry)
                     : "memory");
    for (;;)
        ;
}

/* Fresh OS kernel - static ELF64 executable loader. */
#include "proc.h"
#include "../mm/mm.h"

int elf_copy_to_space(uint64_t *pml4, uint64_t va, const void *src, uint64_t n);

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

int elf_load(uint64_t *pml4, const uint8_t *data, uint64_t size, struct elf_image *out)
{
    if (size < sizeof(struct elf64_ehdr))
        return -ENOEXEC;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)data;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1)
        return -ENOEXEC;
    if (eh->type != 2 || eh->machine != 62 || eh->phentsize != sizeof(struct elf64_phdr))
        return -ENOEXEC;
    if (eh->phnum == 0 || eh->phnum > 32 || eh->phoff > size ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size)
        return -ENOEXEC;
    uint64_t end = 0;
    bool entry_ok = false;
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(data + eh->phoff + i * sizeof(*ph));
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset)
            return -ENOEXEC;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz < ph->vaddr || ph->vaddr + ph->memsz > USER_HEAP_LIMIT)
            return -ENOEXEC;
        uint64_t flags = 0;
        if (ph->flags & PF_W)
            flags |= PTE_W;
        if (!(ph->flags & PF_X))
            flags |= PTE_NX;
        for (uint64_t va = ALIGN_DOWN(ph->vaddr, PAGE_SIZE); va < ph->vaddr + ph->memsz; va += PAGE_SIZE)
            if (!vmm_map_user_page(pml4, va, flags))
                return -ENOMEM;
        if (ph->filesz && elf_copy_to_space(pml4, ph->vaddr, data + ph->offset, ph->filesz))
            return -ENOMEM;
        if (eh->entry >= ph->vaddr && eh->entry < ph->vaddr + ph->memsz && (ph->flags & PF_X))
            entry_ok = true;
        if (ph->vaddr + ph->memsz > end)
            end = ph->vaddr + ph->memsz;
    }
    if (!entry_ok)
        return -ENOEXEC;
    out->entry = eh->entry;
    out->brk = ALIGN_UP(end, PAGE_SIZE);
    return 0;
}

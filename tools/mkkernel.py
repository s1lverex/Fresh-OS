#!/usr/bin/env python3
"""Convert the kernel ELF into KERNEL.BIN: a 64-byte header followed by a flat
memory image starting at the kernel's virtual base (see include/fresh/bootinfo.h)."""
import struct
import sys

KERNEL_MAGIC = 0x4C4E524B48535246  # "FRSHKRNL"
KERNEL_VMA = 0xFFFFFFFF80000000


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkkernel.py kernel.elf KERNEL.BIN")
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("mkkernel: not a little-endian ELF64 file")
    e_entry, e_phoff = struct.unpack_from("<QQ", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    loads = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    if not loads:
        sys.exit("mkkernel: no PT_LOAD segments")
    base = min(l[0] for l in loads)
    if base != KERNEL_VMA:
        sys.exit("mkkernel: kernel must start at 0x%x (got 0x%x)" % (KERNEL_VMA, base))
    file_end = max(l[0] - base + l[2] for l in loads)
    mem_end = max(l[0] - base + l[3] for l in loads)
    image = bytearray(file_end)
    for vaddr, off, filesz, memsz in loads:
        image[vaddr - base:vaddr - base + filesz] = data[off:off + filesz]
    hdr = struct.pack("<QQQQQQQQ", KERNEL_MAGIC, e_entry, len(image), mem_end, base, 0, 0, 0)
    with open(sys.argv[2], "wb") as f:
        f.write(hdr + image)
    print("KERNEL.BIN: entry=0x%x image=%d bytes memory=%d bytes" % (e_entry, len(image), mem_end))


if __name__ == "__main__":
    main()

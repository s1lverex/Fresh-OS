#!/usr/bin/env python3
"""Build the Fresh OS hybrid BIOS/UEFI disk image.

Layout (512-byte sectors):
  LBA 0            protective MBR carrying the BIOS stage 1 boot code
  LBA 1..33        primary GPT
  partition 1      BIOS boot partition: stage 2, KERNEL.BIN, initrd (raw)
  partition 2      EFI system partition (FAT): BOOTX64.EFI, KERNEL.BIN, INITRD
  partition 3      FreshFS data partition (mounted at /data)
  last 33 sectors  backup GPT
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

SECTOR = 512
BIOS_BOOT_TYPE = uuid.UUID("21686148-6449-6E6F-744E-656564454649")
ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
FRESHFS_TYPE = uuid.UUID("F7E5A1C0-3B2D-4E6F-9A8B-1C2D3E4F5A6B")
NS = uuid.UUID("6f1d3b4c-2a59-4c7e-8d10-5e0b7a9c3f21")

STAGE2_SECTORS = 64
BIOS_PART_START = 2048
BIOS_PART_MB = 16
ESP_MB = 48


def sectors(nbytes):
    return (nbytes + SECTOR - 1) // SECTOR


def gpt_entry(type_guid, uniq, first, last, name):
    nm = name.encode("utf-16-le")[:72].ljust(72, b"\0")
    return type_guid.bytes_le + uniq.bytes_le + struct.pack("<QQQ", first, last, 0) + nm


def gpt_header(cur, backup, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    hdr = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0, cur, backup,
                      first_usable, last_usable, disk_guid.bytes_le, entries_lba, 128, 128, entries_crc)
    crc = zlib.crc32(hdr) & 0xFFFFFFFF
    hdr = hdr[:16] + struct.pack("<I", crc) + hdr[20:]
    return hdr.ljust(SECTOR, b"\0")


def run(cmd, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("mkimage: command failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout


def build_esp(path, size_bytes, efi, kernel, initrd, cmdline):
    with open(path, "wb") as f:
        f.truncate(size_bytes)
    run(["mformat", "-i", path, "-T", str(size_bytes // SECTOR), "-h", "64", "-s", "32", "-v", "FRESHBOOT", "::"])
    run(["mmd", "-i", path, "::/EFI", "::/EFI/BOOT", "::/FRESH"])
    run(["mcopy", "-i", path, efi, "::/EFI/BOOT/BOOTX64.EFI"])
    run(["mcopy", "-i", path, kernel, "::/FRESH/KERNEL.BIN"])
    run(["mcopy", "-i", path, initrd, "::/FRESH/INITRD"])
    if cmdline:
        with tempfile.NamedTemporaryFile("w", delete=False) as t:
            t.write(cmdline + "\n")
        run(["mcopy", "-i", path, t.name, "::/FRESH/CMDLINE"])
        os.unlink(t.name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--efi", required=True)
    ap.add_argument("--ffstool", help="host ffstool binary used to create the data partition")
    ap.add_argument("--data-dir", help="directory whose files are copied into the data partition")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--size-mb", type=int, default=128)
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    stage1 = open(a.stage1, "rb").read()
    stage2 = bytearray(open(a.stage2, "rb").read())
    kernel = open(a.kernel, "rb").read()
    initrd = open(a.initrd, "rb").read()
    if len(stage1) != 440:
        sys.exit("mkimage: stage1 must be exactly 440 bytes")
    if len(stage2) != STAGE2_SECTORS * SECTOR or stage2[8:16] != b"FRSHSTG2":
        sys.exit("mkimage: bad stage2 binary")
    if len(a.cmdline) > 127:
        sys.exit("mkimage: command line too long")

    total = a.size_mb * 1024 * 1024 // SECTOR
    bios_first = BIOS_PART_START
    bios_last = bios_first + BIOS_PART_MB * 2048 - 1
    esp_first = bios_last + 1
    esp_last = esp_first + ESP_MB * 2048 - 1
    data_first = esp_last + 1
    last_usable = total - 34
    data_last = ((last_usable + 1 - data_first) // 8) * 8 + data_first - 1
    if data_last - data_first < 4096:
        sys.exit("mkimage: image too small")

    kernel_lba = bios_first + STAGE2_SECTORS
    initrd_lba = kernel_lba + sectors(len(kernel))
    if initrd_lba + sectors(len(initrd)) > bios_last + 1:
        sys.exit("mkimage: kernel + initrd do not fit in the BIOS boot partition")
    struct.pack_into("<QII", stage2, 16, kernel_lba, sectors(len(kernel)), sectors(len(initrd)))
    struct.pack_into("<QI", stage2, 32, initrd_lba, len(initrd))
    stage2[48:48 + 128] = a.cmdline.encode().ljust(128, b"\0")

    img = open(a.output, "wb")
    img.truncate(total * SECTOR)

    # protective MBR with boot code
    mbr = bytearray(SECTOR)
    mbr[0:440] = stage1
    struct.pack_into("<I", mbr, 440, 0x46524553)  # disk signature "SERF"
    part = struct.pack("<B3sB3sII", 0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, min(total - 1, 0xFFFFFFFF))
    mbr[446:462] = part
    mbr[510:512] = b"\x55\xaa"
    img.seek(0)
    img.write(mbr)

    # GPT
    disk_guid = uuid.uuid5(NS, "disk")
    entries = bytearray(128 * 128)
    parts = [
        (BIOS_BOOT_TYPE, uuid.uuid5(NS, "bios"), bios_first, bios_last, "Fresh BIOS boot"),
        (ESP_TYPE, uuid.uuid5(NS, "esp"), esp_first, esp_last, "Fresh EFI system"),
        (FRESHFS_TYPE, uuid.uuid5(NS, "data"), data_first, data_last, "Fresh data"),
    ]
    for i, p in enumerate(parts):
        entries[i * 128:(i + 1) * 128] = gpt_entry(*p)
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF
    img.seek(1 * SECTOR)
    img.write(gpt_header(1, total - 1, 34, last_usable, disk_guid, 2, ecrc))
    img.seek(2 * SECTOR)
    img.write(entries)
    img.seek((total - 33) * SECTOR)
    img.write(entries)
    img.seek((total - 1) * SECTOR)
    img.write(gpt_header(total - 1, 1, 34, last_usable, disk_guid, total - 33, ecrc))

    # BIOS boot partition contents
    img.seek(bios_first * SECTOR)
    img.write(stage2)
    img.seek(kernel_lba * SECTOR)
    img.write(kernel)
    img.seek(initrd_lba * SECTOR)
    img.write(initrd)

    with tempfile.TemporaryDirectory() as td:
        esp = os.path.join(td, "esp.img")
        build_esp(esp, (esp_last - esp_first + 1) * SECTOR, a.efi, a.kernel, a.initrd, a.cmdline)
        img.seek(esp_first * SECTOR)
        img.write(open(esp, "rb").read())

        if a.ffstool:
            data = os.path.join(td, "data.img")
            run([a.ffstool, "mkfs", data, str((data_last - data_first + 1) * SECTOR), "FRESHDATA"])
            if a.data_dir:
                for root, dirs, files in os.walk(a.data_dir):
                    dirs.sort()
                    rel = os.path.relpath(root, a.data_dir)
                    dst_dir = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
                    if dst_dir != "/":
                        run([a.ffstool, "mkdir", data, dst_dir])
                    for fn in sorted(files):
                        run([a.ffstool, "put", data, os.path.join(root, fn), dst_dir.rstrip("/") + "/" + fn])
            run([a.ffstool, "fsck", data])
            img.seek(data_first * SECTOR)
            img.write(open(data, "rb").read())
    img.close()
    print("%s: %d MiB, data partition at LBA %d (%d MiB)" % (
        a.output, a.size_mb, data_first, (data_last - data_first + 1) // 2048))


if __name__ == "__main__":
    main()

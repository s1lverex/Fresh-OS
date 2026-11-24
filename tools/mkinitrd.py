#!/usr/bin/env python3
"""Pack directory trees into the Fresh OS initrd archive (see kernel/fs/initrd.c).
usage: mkinitrd.py OUTPUT DIR [DIR...]   (later directories override earlier ones)"""
import os
import struct
import sys


def pad8(n):
    return (8 - n % 8) % 8


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    entries = {}
    for top in sys.argv[2:]:
        if not os.path.isdir(top):
            continue
        for root, dirs, files in os.walk(top):
            dirs.sort()
            rel = os.path.relpath(root, top)
            if rel != ".":
                entries[rel.replace(os.sep, "/")] = None
            for f in sorted(files):
                path = os.path.join(root, f)
                key = os.path.normpath(os.path.join(rel, f)).replace(os.sep, "/")
                entries[key] = path
    out = bytearray(b"FRSHINIT" + struct.pack("<II", len(entries), 0))
    for key in sorted(entries, key=lambda k: (k.count("/"), k)):
        src = entries[key]
        name = key.encode()
        data = b"" if src is None else open(src, "rb").read()
        out += struct.pack("<IIQ", 2 if src is None else 1, len(name), len(data))
        out += name + b"\0" * pad8(len(name))
        out += data + b"\0" * pad8(len(data))
    open(sys.argv[1], "wb").write(out)
    print("initrd: %d entries, %d bytes" % (len(entries), len(out)))


if __name__ == "__main__":
    main()

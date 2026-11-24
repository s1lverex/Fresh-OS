#!/usr/bin/env python3
"""Run Fresh OS in QEMU and drive a looping demo through its serial console.

Starts QEMU (KVM when available, otherwise software emulation), types demo
commands into the shell at a human pace so viewers of the stream can follow,
restarts the VM if it ever stops, and powers it off cleanly at the deadline.

usage: autopilot.py --image DISK.img --dir DIR [--minutes N]"""
import argparse
import json
import os
import re
import select
import subprocess
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(rb"fresh:(/[^\s$]*)?\$ $")

# (what to show on the page, keystrokes, seconds to linger afterwards)
DEMO = [
    ("Welcome", "clear; cat /etc/motd\r", 5),
    ("System overview", "sysinfo\r", 6),
    ("Programs in /bin", "ls /bin\r", 5),
    ("Writing a file to disk", "echo 'Hello from Fresh OS!' > /data/hello.txt; cat /data/hello.txt\r", 4),
    ("Files on the FreshFS disk", "ls -l /data\r", 5),
    ("Pipes", "seq 1 1000 | grep 7 | wc -l\r", 4),
    ("Processes", "ps\r", 5),
    ("Text editor", None, 3),  # scripted below
    ("Reading the edited file", "cat /data/notes.txt\r", 5),
    ("Launching 1000 programs", "/test/spawntest 1000\r", 5),
    ("File system stress test", "/test/fstest /data\r", 5),
    ("A crashing program is contained", "/test/crashtest null; echo \"the OS is still running\"\r", 6),
    ("Kernel self test", "/test/ktest\r", 6),
    ("Memory and disk", "free; df; uptime\r", 6),
    ("All commands", "help\r", 8),
]

EDITOR = [
    "rm -f /data/notes.txt; edit /data/notes.txt\r",
    "@pause 2",
    "Fresh OS notes\r",
    "--------------\r",
    "* written from scratch: boot loaders, kernel, file system, programs\r",
    "* boots on BIOS and UEFI PCs\r",
    "* files in /data survive reboots\r",
    "@pause 2",
    "\x13",  # Ctrl+S save
    "@pause 3",
    "\x11",  # Ctrl+Q quit
]


class Serial:
    """The VM's first serial port, wired to QEMU's stdin/stdout."""

    def __init__(self, proc):
        self.proc = proc
        os.set_blocking(proc.stdout.fileno(), False)
        self.buf = b""

    def pump(self, wait=0.1):
        end = time.time() + wait
        while True:
            r, _, _ = select.select([self.proc.stdout], [], [], max(0.0, end - time.time()))
            if r:
                try:
                    data = self.proc.stdout.read(65536)
                except BlockingIOError:
                    data = b""
                if data:
                    self.buf = (self.buf + data)[-200000:]
                    continue
                if self.proc.poll() is not None:
                    raise ConnectionError("QEMU exited")
            if time.time() >= end:
                return

    def clean(self):
        return ANSI.sub(b"", self.buf).replace(b"\r", b"")

    def wait_prompt(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.2)
            if PROMPT.search(self.clean()):
                return True
        return False

    def type(self, text, cps=22):
        for ch in text.encode("latin-1"):
            self.proc.stdin.write(bytes([ch]))
            self.proc.stdin.flush()
            time.sleep(1.0 / cps if ch >= 32 else 0.05)
            self.pump(0)


def write_status(directory, step, until):
    tmp = os.path.join(directory, "status.json.tmp")
    with open(tmp, "w") as f:
        json.dump({"step": step, "until": until}, f)
    os.replace(tmp, os.path.join(directory, "status.json"))


def start_qemu(image, directory, kvm):
    for name in ("qmp.sock",):
        try:
            os.unlink(os.path.join(directory, name))
        except OSError:
            pass
    accel = ["-accel", "kvm", "-cpu", "host"] if kvm else ["-accel", "tcg", "-cpu", "qemu64"]
    cmd = ["qemu-system-x86_64", "-machine", "q35", "-m", "1G", *accel, "-smp", "1",
           "-drive", "file=%s,format=raw" % image, "-display", "none", "-vga", "std", "-no-reboot",
           "-serial", "stdio",
           "-qmp", "unix:%s,server=on,wait=off" % os.path.join(directory, "qmp.sock")]
    print("starting:", " ".join(cmd), flush=True)
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)


def boot(image, directory, until):
    """Start QEMU and wait for the shell; prefer KVM, fall back to TCG."""
    modes = [True, False] if os.access("/dev/kvm", os.R_OK | os.W_OK) else [False]
    for kvm in modes:
        write_status(directory, "Booting (%s)" % ("KVM" if kvm else "emulation"), until)
        q = start_qemu(image, directory, kvm)
        ser = Serial(q)
        if ser.wait_prompt(120):
            print("booted with", "KVM" if kvm else "TCG", flush=True)
            return q, ser
        print("boot failed with", "KVM" if kvm else "TCG", flush=True)
        q.kill()
        q.wait()
    raise SystemExit("could not boot Fresh OS")


def run_step(ser, keys, settle):
    if keys is None:
        ser.buf = b""
        for k in EDITOR:
            if k.startswith("@pause"):
                time.sleep(float(k.split()[1]))
            else:
                ser.type(k)
        ser.wait_prompt(60)
    else:
        ser.buf = b""
        ser.type(keys)
        if not ser.wait_prompt(600):
            raise TimeoutError("no prompt after " + keys.strip())
    time.sleep(settle)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--minutes", type=float, default=180)
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    until = time.time() + a.minutes * 60
    q, ser = boot(a.image, a.dir, until)
    rounds = 0
    while time.time() < until:
        try:
            for title, keys, settle in DEMO:
                if time.time() >= until:
                    break
                write_status(a.dir, title, until)
                run_step(ser, keys, settle)
            rounds += 1
            print("demo round %d done" % rounds, flush=True)
        except (OSError, ConnectionError, TimeoutError) as e:
            print("VM problem (%s), restarting" % e, flush=True)
            q.kill()
            q.wait()
            q, ser = boot(a.image, a.dir, until)
    write_status(a.dir, "Shutting down (stream over)", until)
    try:
        ser.type("shutdown\r")
        q.wait(timeout=60)
    except Exception:
        q.kill()
    print("finished after %d demo rounds" % rounds, flush=True)


if __name__ == "__main__":
    main()

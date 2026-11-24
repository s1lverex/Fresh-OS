#!/usr/bin/env python3
"""Run Fresh OS in QEMU and drive a looping demo of its desktop.

Starts QEMU (KVM when available, otherwise software emulation), waits for the
desktop, then plays a script of keyboard and mouse actions through the QEMU
monitor at a human pace so viewers of the stream can follow: commands typed
into the terminal window, the start menu, System Monitor, Files, the editor,
moving and closing windows. The text console on the serial port is used to
check that the desktop is up (and to restart it if needed). The VM is
restarted if it ever stops and powered off cleanly at the deadline.

usage: autopilot.py --image DISK.img --dir DIR [--minutes N]"""
import argparse
import json
import os
import re
import select
import socket
import subprocess
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(rb"fresh:(/[^\s$]*)?\$ $")

# screen layout of the desktop at 1024x768 (see user/desktop/desktop.c)
H = 768
START = (40, H - 17)
MENU_TOP = H - 34 - 4 - (8 * 28 + 10 + 12)


def menu_item(i):
    return (60, MENU_TOP + 6 + i * 28 + (10 if i >= 5 else 0) + 14)


M_TERM, M_FILES, M_MONITOR, M_EDITOR, M_ABOUT = range(5)
ICON_FILES = (64, 20 + 92 + 40)

# (label shown on the page, actions). Windows opened while the main terminal
# is open cascade to (184, 70); they are closed again so positions repeat.
DEMO = [
    ("Welcome", [("type", "clear; cat /etc/motd\n"), ("sleep", 4)]),
    ("System overview", [("type", "sysinfo\n"), ("sleep", 5)]),
    ("Programs in /bin", [("type", "ls /bin\n"), ("sleep", 4)]),
    ("Writing a file to disk", [("type", "echo 'Hello from Fresh OS!' > hello.txt; cat hello.txt\n"), ("sleep", 3)]),
    ("Pipes", [("type", "seq 1 1000 | grep 7 | wc -l\n"), ("sleep", 3)]),
    ("Start menu: System Monitor", [("click", START), ("sleep", 1.5), ("click", menu_item(M_MONITOR)), ("sleep", 4),
                                    ("drag", (330, 80), (560, 260)), ("sleep", 4),
                                    ("click", (184 + 230 + 470 - 15, 70 + 180 + 13)), ("sleep", 1)]),
    ("Files", [("click", START), ("sleep", 1.5), ("click", menu_item(M_FILES)), ("sleep", 3), ("key", "down"),
               ("sleep", 0.6), ("key", "down"), ("sleep", 2), ("click", (184 + 460 - 15, 70 + 13)), ("sleep", 1)]),
    ("Text editor", [("type", "rm -f notes.txt; edit notes.txt\n"), ("sleep", 2),
                     ("type", "Fresh OS notes\n--------------\n"),
                     ("type", "* written from scratch: boot loaders, kernel, desktop\n"),
                     ("type", "* boots on BIOS and UEFI PCs\n* files in /data survive reboots"), ("sleep", 2),
                     ("key", "ctrl-s"), ("sleep", 2), ("key", "ctrl-q"), ("sleep", 1),
                     ("type", "cat notes.txt\n"), ("sleep", 4)]),
    ("Launching 1000 programs", [("type", "/test/spawntest 1000\n"), ("sleep", 5)]),
    ("File system stress test", [("type", "/test/fstest /data\n"), ("sleep", 8)]),
    ("A crashing program is contained", [("type", "/test/crashtest null; echo \"the OS is still running\"\n"),
                                         ("sleep", 5)]),
    ("A second terminal", [("key", "ctrl-alt-t"), ("sleep", 1.5), ("type", "uptime; free; df\n"), ("sleep", 5),
                           ("click", (184 + 650 - 15, 70 + 13)), ("sleep", 1)]),
    ("About Fresh OS", [("click", START), ("sleep", 1.5), ("click", menu_item(M_ABOUT)), ("sleep", 6),
                        ("click", (184 + 480 - 15, 70 + 13)), ("sleep", 1)]),
    ("Kernel self test", [("type", "clear; /test/ktest | tail -n 3\n"), ("sleep", 6)]),
]

KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", "_": "shift-minus", "=": "equal", ".": "dot", ",": "comma",
            "/": "slash", ";": "semicolon", ":": "shift-semicolon", "'": "apostrophe", '"': "shift-apostrophe",
            ">": "shift-dot", "<": "shift-comma", "|": "shift-backslash", "*": "shift-8", "!": "shift-1",
            "&": "shift-7", "$": "shift-4"}


class Serial:
    """The VM's first serial port (text console), wired to QEMU's stdio."""

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

    def run(self, cmd, timeout=30):
        self.buf = b""
        self.proc.stdin.write(cmd.encode() + b"\r")
        self.proc.stdin.flush()
        self.wait_prompt(timeout)
        return self.clean().decode("latin-1")


class Monitor:
    """QEMU human monitor on a UNIX socket: key and mouse injection."""

    def __init__(self, path):
        self.path = path

    def cmd(self, line):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(self.path)
        buf = b""
        while b"(qemu)" not in buf:
            buf += s.recv(4096)
        s.sendall(line.encode() + b"\n")
        buf = b""
        while b"(qemu)" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        s.close()

    def key(self, name):
        self.cmd("sendkey " + name)

    def type(self, text, cps=16):
        for c in text:
            name = KEYNAMES.get(c, ("shift-" + c.lower()) if c.isupper() else c)
            self.key(name)
            time.sleep(1.0 / cps)

    def mouse_to(self, x, y):
        # pause between moves so the guest drains the PS/2 queue in time
        for _ in range(4):
            self.cmd("mouse_move -500 -500")
            time.sleep(0.05)
        self.cmd("mouse_move %d %d" % (x, y))
        time.sleep(0.25)

    def click(self, x, y, double=False):
        self.mouse_to(x, y)
        for i in range(2 if double else 1):
            self.cmd("mouse_button 1")
            time.sleep(0.06)
            self.cmd("mouse_button 0")
            time.sleep(0.12 if double and i == 0 else 0.3)

    def drag(self, a, b, steps=12):
        self.mouse_to(*a)
        self.cmd("mouse_button 1")
        time.sleep(0.15)
        dx, dy = b[0] - a[0], b[1] - a[1]
        done_x = done_y = 0
        for i in range(1, steps + 1):  # visible, smooth movement
            tx, ty = dx * i // steps, dy * i // steps
            self.cmd("mouse_move %d %d" % (tx - done_x, ty - done_y))
            done_x, done_y = tx, ty
            time.sleep(0.05)
        self.cmd("mouse_button 0")
        time.sleep(0.3)


def write_status(directory, step, until):
    tmp = os.path.join(directory, "status.json.tmp")
    with open(tmp, "w") as f:
        json.dump({"step": step, "until": until}, f)
    os.replace(tmp, os.path.join(directory, "status.json"))


def start_qemu(image, directory, kvm):
    for name in ("qmp.sock", "mon.sock"):
        try:
            os.unlink(os.path.join(directory, name))
        except OSError:
            pass
    accel = ["-accel", "kvm", "-cpu", "host"] if kvm else ["-accel", "tcg", "-cpu", "qemu64"]
    cmd = ["qemu-system-x86_64", "-machine", "q35", "-m", "1G", *accel, "-smp", "1",
           "-drive", "file=%s,format=raw" % image, "-display", "none", "-vga", "std", "-no-reboot",
           "-serial", "stdio",
           "-qmp", "unix:%s,server=on,wait=off" % os.path.join(directory, "qmp.sock"),
           "-monitor", "unix:%s,server=on,wait=off" % os.path.join(directory, "mon.sock")]
    print("starting:", " ".join(cmd), flush=True)
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)


def desktop_up(ser):
    return "opened Terminal" in ser.run("cat /tmp/desktop.log")


def terminals_open(ser):
    log = ser.run("cat /tmp/desktop.log")
    return log.count("opened Terminal") - log.count("closed Terminal")


def boot(image, directory, until):
    """Start QEMU and wait for the desktop; prefer KVM, fall back to TCG."""
    modes = [True, False] if os.access("/dev/kvm", os.R_OK | os.W_OK) else [False]
    for kvm in modes:
        write_status(directory, "Booting (%s)" % ("KVM" if kvm else "emulation"), until)
        q = start_qemu(image, directory, kvm)
        ser = Serial(q)
        end = time.time() + 120
        while time.time() < end and b"fresh:" not in ser.clean():
            ser.pump(0.5)
        if b"fresh:" in ser.clean():
            for _ in range(30):
                if desktop_up(ser):
                    print("booted with", "KVM" if kvm else "TCG", flush=True)
                    time.sleep(1)
                    return q, ser, Monitor(os.path.join(directory, "mon.sock"))
                time.sleep(1)
        print("boot failed with", "KVM" if kvm else "TCG", flush=True)
        q.kill()
        q.wait()
    raise SystemExit("could not boot Fresh OS")


def play(mon, actions):
    for act in actions:
        kind = act[0]
        if kind == "type":
            mon.type(act[1])
        elif kind == "key":
            mon.key(act[1])
        elif kind == "sleep":
            time.sleep(act[1])
        elif kind == "click":
            mon.click(*act[1])
        elif kind == "dclick":
            mon.click(*act[1], double=True)
        elif kind == "drag":
            mon.drag(act[1], act[2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--minutes", type=float, default=180)
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    until = time.time() + a.minutes * 60
    q, ser, mon = boot(a.image, a.dir, until)
    rounds = 0
    while time.time() < until:
        try:
            if not desktop_up(ser):  # someone or something closed it: start it again
                ser.run("desktop &")
                time.sleep(3)
            if terminals_open(ser) < 1:  # the demo types into the main terminal
                mon.key("ctrl-alt-t")
                time.sleep(2)
            for title, actions in DEMO:
                if time.time() >= until:
                    break
                write_status(a.dir, title, until)
                play(mon, actions)
                ser.pump(0)
            rounds += 1
            print("demo round %d done" % rounds, flush=True)
        except (OSError, ConnectionError) as e:
            print("VM problem (%s), restarting" % e, flush=True)
            q.kill()
            q.wait()
            q, ser, mon = boot(a.image, a.dir, until)
    write_status(a.dir, "Shutting down (stream over)", until)
    try:
        ser.run("shutdown")
        q.wait(timeout=60)
    except Exception:
        q.kill()
    print("finished after %d demo rounds" % rounds, flush=True)


if __name__ == "__main__":
    main()

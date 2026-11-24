"""QEMU driver for the Fresh OS end-to-end tests.

A VM boots a private copy of a disk image, its first serial port is wired to
this process (stdin/stdout) and the QEMU monitor is reachable on a UNIX socket
for key injection and screenshots."""
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(r"fresh:(/[^\s$]*)?\$ $")
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"


class TestFailure(Exception):
    pass


def strip(text):
    return ANSI.sub("", text).replace("\r", "")


class VM:
    def __init__(self, image, firmware="bios", machine="q35", mem="256M", cpu="qemu64", copy=True,
                 extra=None, workdir=None, debug_exit=False):
        self.workdir = workdir or tempfile.mkdtemp(prefix="freshvm-")
        self.own_workdir = workdir is None
        if copy:
            self.image = os.path.join(self.workdir, "disk.img")
            subprocess.run(["cp", "--sparse=always", image, self.image], check=True)
        else:
            self.image = image
        self.firmware = firmware
        self.machine = machine
        self.mem = mem
        self.cpu = cpu
        self.extra = extra or []
        self.debug_exit = debug_exit
        self.proc = None
        self.raw = ""
        self.pos = 0  # consumed position in the stripped log
        self.log_path = os.path.join(self.workdir, "serial.log")
        self.mon_path = os.path.join(self.workdir, "mon.sock")
        self.marker = 0

    # ------------------------------------------------------------ lifecycle
    def start(self):
        cmd = ["qemu-system-x86_64", "-machine", self.machine, "-m", self.mem, "-cpu", self.cpu,
               "-display", "none", "-serial", "stdio", "-no-reboot",
               "-monitor", "unix:%s,server,nowait" % self.mon_path]
        if self.firmware == "uefi":
            vars_copy = os.path.join(self.workdir, "OVMF_VARS.fd")
            shutil.copy(OVMF_VARS, vars_copy)
            cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
                    "-drive", "if=pflash,format=raw,file=" + vars_copy]
        cmd += ["-drive", "file=%s,format=raw" % self.image]
        if self.debug_exit:
            cmd += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        cmd += self.extra
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.raw = ""
        self.pos = 0
        return self

    def _pump(self, timeout):
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            return False
        try:
            data = self.proc.stdout.read(65536)
        except BlockingIOError:
            return False
        if not data:
            time.sleep(0.01)
            return False
        text = data.decode("latin-1")
        self.raw += text
        with open(self.log_path, "a", encoding="latin-1") as f:
            f.write(text)
        return True

    @property
    def text(self):
        return strip(self.raw)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def wait_exit(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            self._pump(0.1)
            if self.proc.poll() is not None:
                while self._pump(0.05):
                    pass
                return self.proc.returncode
        raise TestFailure("QEMU did not exit within %ds" % timeout)

    def kill(self):
        """Simulate a power cut."""
        if self.alive():
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()

    def close(self):
        self.kill()
        if self.own_workdir and os.environ.get("FRESH_KEEP") != "1":
            shutil.rmtree(self.workdir, ignore_errors=True)

    # ------------------------------------------------------------ serial I/O
    def expect(self, pattern, timeout=30, regex=False):
        """Wait until pattern appears after the current position; consume it."""
        end = time.time() + timeout
        while True:
            t = self.text
            if regex:
                m = re.compile(pattern, re.M).search(t, self.pos)
                if m:
                    self.pos = m.end()
                    return m
            else:
                i = t.find(pattern, self.pos)
                if i >= 0:
                    self.pos = i + len(pattern)
                    return t[i:self.pos]
            if time.time() > end:
                raise TestFailure("timeout waiting for %r; recent output:\n%s" % (pattern, t[-1500:]))
            if not self.alive():
                while self._pump(0.05):
                    pass
                t = self.text
                if (pattern in t[self.pos:]) if not regex else re.search(pattern, t[self.pos:], re.M):
                    continue
                raise TestFailure("QEMU exited (code %s) while waiting for %r; output:\n%s" % (
                    self.proc.returncode, pattern, t[-1500:]))
            self._pump(0.2)

    def wait_prompt(self, timeout=60):
        end = time.time() + timeout
        while True:
            t = self.text
            if PROMPT.search(t[self.pos:]):
                self.pos = len(t)
                return
            if time.time() > end:
                raise TestFailure("timeout waiting for the shell prompt; recent output:\n%s" % t[-1500:])
            if not self.alive():
                raise TestFailure("QEMU exited while waiting for the prompt:\n%s" % t[-1500:])
            self._pump(0.2)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        # feed slowly enough for the 16-byte UART FIFO
        for i in range(0, len(data), 8):
            self.proc.stdin.write(data[i:i + 8])
            self.proc.stdin.flush()
            time.sleep(0.004)

    def sendline(self, line):
        self.send(line + "\r")

    def boot(self, timeout=120):
        self.start()
        self.wait_prompt(timeout)
        return self

    def run(self, cmd, timeout=60, check=True):
        """Run a shell command, return (status, output)."""
        self.marker += 1
        tag = "@@%d" % self.marker
        start = self.pos
        self.sendline("%s; echo %s:$?@@" % (cmd, tag))
        m = self.expect(r"%s:(\d+)@@" % re.escape(tag), timeout=timeout, regex=True)
        status = int(m.group(1))
        t = self.text
        line_end = t.find("\n", t.find(tag + ":$?@@", start))
        out = t[line_end + 1:m.start()]
        self.wait_prompt(timeout)
        if check and status != 0:
            raise TestFailure("command %r failed with status %d:\n%s" % (cmd, status, out))
        return status, out

    def out(self, cmd, timeout=60):
        return self.run(cmd, timeout)[1]

    def shutdown(self, timeout=60):
        self.sendline("shutdown")
        code = self.wait_exit(timeout)
        if "Powering off" not in self.text:
            raise TestFailure("shutdown message missing")
        return code

    # ------------------------------------------------------------ monitor
    def monitor(self, command, timeout=10):
        if not self.alive():  # e.g. the guest just powered off
            return ""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        end = time.time() + timeout
        while True:
            try:
                s.connect(self.mon_path)
                break
            except OSError:
                if not self.alive():
                    return ""
                if time.time() > end:
                    raise TestFailure("cannot connect to the QEMU monitor")
                time.sleep(0.1)
        s.settimeout(timeout)
        buf = b""
        while b"(qemu)" not in buf:
            buf += s.recv(4096)
        s.sendall(command.encode() + b"\n")
        buf = b""
        while buf.count(b"(qemu)") < 1:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        s.close()
        return buf.decode("latin-1")

    def sendkeys(self, keys):
        for k in keys:
            self.monitor("sendkey " + k)
            time.sleep(0.05)


# ---------------------------------------------------------------- GUI helpers
KEYNAMES = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "_": "shift-minus", "=": "equal",
            "+": "shift-equal", ".": "dot", ",": "comma", "/": "slash", "?": "shift-slash", ";": "semicolon",
            ":": "shift-semicolon", "'": "apostrophe", '"': "shift-apostrophe", ">": "shift-dot",
            "<": "shift-comma", "$": "shift-4", "&": "shift-7", "*": "shift-8", "|": "shift-backslash",
            "!": "shift-1", "@": "shift-2", "#": "shift-3", "(": "shift-9", ")": "shift-0"}


def _keyname(c):
    if c in KEYNAMES:
        return KEYNAMES[c]
    if c.isupper():
        return "shift-" + c.lower()
    return c


def type_text(vm, text):
    """Type text on the emulated PS/2 keyboard."""
    for c in text:
        vm.monitor("sendkey " + _keyname(c))
        time.sleep(0.03)


def mouse_to(vm, x, y):
    """Move the PS/2 mouse pointer to absolute screen position x, y."""
    for _ in range(4):
        vm.monitor("mouse_move -500 -500")
        time.sleep(0.05)
    vm.monitor("mouse_move %d %d" % (x, y))
    time.sleep(0.2)


def click(vm, x, y, double=False):
    mouse_to(vm, x, y)
    for i in range(2 if double else 1):
        vm.monitor("mouse_button 1")
        time.sleep(0.06)
        vm.monitor("mouse_button 0")
        time.sleep(0.12 if double and i == 0 else 0.4)


def drag(vm, x0, y0, x1, y1):
    mouse_to(vm, x0, y0)
    vm.monitor("mouse_button 1")
    time.sleep(0.15)
    vm.monitor("mouse_move %d %d" % ((x1 - x0) // 2, (y1 - y0) // 2))
    time.sleep(0.15)
    vm.monitor("mouse_move %d %d" % (x1 - x0 - (x1 - x0) // 2, y1 - y0 - (y1 - y0) // 2))
    time.sleep(0.15)
    vm.monitor("mouse_button 0")
    time.sleep(0.4)


def screenshot(vm):
    """Return (width, height, rgb bytes) of the VM screen."""
    path = os.path.join(vm.workdir, "shot.ppm")
    vm.monitor("screendump " + path)
    time.sleep(0.4)
    data = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def pixel(shot, x, y):
    w, h, px = shot
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]


def near(rgb, want, tol=6):
    return all(abs(a - ((want >> s) & 0xFF)) <= tol for a, s in zip(rgb, (16, 8, 0)))


def wait_log(vm, text, timeout=20):
    """Wait until /tmp/desktop.log (written by the desktop) contains text."""
    end = time.time() + timeout
    while True:
        log = vm.run("cat /tmp/desktop.log", check=False)[1]
        if text in log:
            return log
        if time.time() > end:
            raise TestFailure("desktop log never showed %r:\n%s" % (text, log))
        time.sleep(0.5)


def desktop_pid(vm):
    out = vm.out("ps")
    m = re.search(r"^\s*(\d+)\s+\d+\s+\w+\s+\d+\s+\d+\s+desktop\s*$", out, re.M)
    return int(m.group(1)) if m else None

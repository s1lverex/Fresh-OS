#!/usr/bin/env python3
"""Fresh OS end-to-end test suite. Boots the OS in QEMU (BIOS and UEFI, several
machine types, memory sizes and CPU models) and checks behaviour through the
serial console.

usage: run_all.py [--build DIR] [--only NAME[,NAME]] [--skip NAME] [--soak SECONDS] [--list]"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu import VM, TestFailure  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ARGS = None
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def img(name="fresh-os.img"):
    return os.path.join(ARGS.build, name)


def ffstool(*args):
    return subprocess.run([os.path.join(ARGS.build, "tools", "ffstool")] + list(args),
                          capture_output=True, text=True)


def check(cond, msg):
    if not cond:
        raise TestFailure(msg)


def eq(got, want, what):
    if got != want:
        raise TestFailure("%s: expected %r, got %r" % (what, want, got))


def host_fsck_clean(vm):
    r = ffstool("fsck", vm.image)
    check(r.returncode == 0 and " 0 errors" in r.stdout, "host fsck not clean:\n" + r.stdout + r.stderr)
    return r.stdout


class Booted:
    """with Booted(...) as vm: boots, yields the VM, always cleans up"""

    def __init__(self, image=None, **kw):
        self.vm = VM(image or img(), **kw)

    def __enter__(self):
        try:
            self.vm.boot(timeout=180)
        except Exception:
            self.vm.close()
            raise
        return self.vm

    def __exit__(self, *exc):
        self.vm.close()
        return False


# ======================================================================= boot matrix
def boot_check(firmware, machine, mem="256M", cpu="qemu64", expect_disk="sata0"):
    with Booted(firmware=firmware, machine=machine, mem=mem, cpu=cpu) as vm:
        t = vm.text
        check("Fresh OS 0.1.0 booting (%s)" % firmware.upper() in t, "boot banner missing")
        check(("on %s mounted at /data" % expect_disk) in t, "data partition not mounted from %s" % expect_disk)
        out = vm.out("sysinfo")
        check(("Firmware  : %s" % firmware.upper()) in out, "sysinfo firmware")
        check("FreshFS (persistent)" in out, "sysinfo data")
        check("1024x768 framebuffer" in out, "framebuffer mode: " + out)
        eq(vm.out("echo hello world").strip(), "hello world", "echo")
        vm.out("echo persist-%s-%s > /data/boot.txt" % (firmware, machine))
        eq(vm.shutdown(), 0, "QEMU exit code after shutdown")
        host_fsck_clean(vm)
        r = ffstool("cat", vm.image, "/boot.txt")
        eq(r.stdout.strip(), "persist-%s-%s" % (firmware, machine), "file written in the OS read by the host")


@test
def boot_bios_q35():
    boot_check("bios", "q35")


@test
def boot_bios_pc_ide():
    boot_check("bios", "pc", expect_disk="ide0")


@test
def boot_uefi_q35():
    boot_check("uefi", "q35")


@test
def boot_uefi_pc_ide():
    boot_check("uefi", "pc", expect_disk="ide0")


@test
def boot_memory_sizes():
    for fw, mem in [("bios", "64M"), ("bios", "4G"), ("uefi", "8G"), ("uefi", "128M")]:
        with Booted(firmware=fw, mem=mem) as vm:
            si = vm.out("free")
            total = int(re.search(r"Mem:\s+(\d+)", si).group(1)) * 1024
            want = {"64M": 64, "128M": 128, "4G": 4096, "8G": 8192}[mem] << 20
            check(want * 0.85 < total <= want, "%s %s: memory total %d not close to %d" % (fw, mem, total, want))
            if mem in ("64M", "128M"):
                out = vm.out("/test/crashtest oom", timeout=300)
                check("OK" in out, out)
                out = vm.out("/test/crashtest oom", timeout=300)  # again: memory came back
                check("OK" in out, out)
            vm.out("/test/ktest", timeout=300)
            vm.out("/test/spawntest 100", timeout=300)
            vm.shutdown()


@test
def boot_cpu_models():
    for cpu in ["max", "Nehalem", "EPYC"]:
        with Booted(cpu=cpu) as vm:
            vm.out("/test/ktest", timeout=300)
            vm.out("/test/memtest 3000", timeout=300)
            vm.shutdown()


# ======================================================================= kernel
@test
def kernel_selftest():
    with Booted() as vm:
        out = vm.out("/test/ktest", timeout=300)
        check("ALL PASSED" in out, out)
        log = vm.text
        check("ktest: FAIL" not in log, "a kernel self test failed")
        check(log.count("ktest: PASS") >= 30, "too few kernel checks ran")
        vm.shutdown()


@test
def kernel_panic_reported():
    vm = VM(img("fresh-os-test.img"), debug_exit=True)
    try:
        vm.boot()
        vm.sendline("/test/ktest_panic")
        code = vm.wait_exit(60)
        eq(code, (0x7F << 1) | 1, "isa-debug-exit code from panic")
        check("KERNEL PANIC: test panic requested" in vm.text, "panic message")
    finally:
        vm.close()


@test
def panic_disabled_outside_test_mode():
    with Booted() as vm:
        st, out = vm.run("/test/ktest_panic", check=False)
        check("refused" in out, "panic hook must be refused without testmode: " + out)
        eq(vm.out("echo still-up").strip(), "still-up", "system alive")
        vm.shutdown()


# ======================================================================= isolation
@test
def process_isolation():
    faults = {"null": "page fault", "read0": "page fault", "kwrite": "page fault", "kread": "page fault",
              "nx": "page fault", "priv": "general protection", "io": "general protection",
              "div0": "divide error", "ud": "invalid opcode", "stack": "page fault",
              "gp": "(general protection|page fault)"}
    with Booted() as vm:
        for mode, what in faults.items():
            st, out = vm.run("/test/crashtest " + mode, check=False, timeout=120)
            eq(st, 139, "exit status of crashtest %s" % mode)
            check(("crashtest[" in out) and re.search("killed: " + what, out),
                  "crashtest %s: kill message %r missing in %r" % (mode, what, out))
            check("still alive" not in out, "crashtest %s survived" % mode)
        out = vm.out("/test/crashtest badptr")
        check("badptr OK" in out, out)
        out = vm.out("/test/crashtest bigalloc")
        check("bigalloc OK" in out, out)
        st, _ = vm.run("/test/crashtest exitcode", check=False)
        eq(st, 42, "exit code passthrough")
        out = vm.out("/test/spawntest 200", timeout=300)
        check("PASS" in out, "no leaks after crashes: " + out)
        # a CPU hog must not freeze the system (timer preemption)
        out = vm.out("/test/crashtest spin &")
        pid = re.search(r"\[(\d+)\]", out).group(1)
        t0 = time.time()
        eq(vm.out("seq 1 300 | wc -l").split()[0], "300", "work while a CPU hog runs")
        out = vm.out("/test/memtest 2000", timeout=120)
        check("PASS" in out and time.time() - t0 < 60, "system stays responsive next to a CPU hog")
        vm.out("kill " + pid)
        vm.out("/test/ktest", timeout=300)
        vm.shutdown()


@test
def syscall_fuzz():
    with Booted(image=img("fresh-os-test.img")) as vm:
        vm.out("cd /tmp")
        survived = 0
        for seed in [1, 7, 1234, 99991, 31337, 4242, 8, 65537]:
            # random calls may scribble over the fuzzer's own memory (that is
            # allowed: it then dies with a fault); the kernel must stay healthy
            st, out = vm.run("/test/sysfuzz 20000 %d" % seed, check=False, timeout=600)
            check(st in (0, 139), "sysfuzz seed %d: unexpected status %d: %s" % (seed, st, out))
            check("BROKEN" not in out, "sysfuzz seed %d reported a broken system" % seed)
            if st == 0:
                survived += 1
            eq(vm.out("echo healthy-%d" % seed).strip(), "healthy-%d" % seed, "shell works after fuzzing")
        check(survived >= 4, "most fuzz runs should complete (%d did)" % survived)
        vm.out("/test/ktest", timeout=300)
        vm.out("/test/spawntest 100", timeout=300)
        vm.shutdown()


# ======================================================================= stress
@test
def stress_suite():
    with Booted() as vm:
        for cmd, t in [("/test/spawntest 2000", 900), ("/test/memtest 30000", 600), ("/test/pipetest", 600),
                       ("/test/fdtest", 60), ("/test/fstest /tmp", 600), ("/test/fstest /data", 900)]:
            out = vm.out(cmd, timeout=t)
            check("PASS" in out and "FAIL" not in out, "%s: %s" % (cmd, out))
        vm.shutdown()
        host_fsck_clean(vm)


@test
def disk_full():
    with Booted() as vm:
        out = vm.out("/test/fstest /data --fill", timeout=1200)
        check("PASS" in out, out)
        out = vm.out("df")
        vm.out("echo after-full > /data/after.txt")
        eq(vm.out("cat /data/after.txt").strip(), "after-full", "write after disk was full")
        vm.shutdown()
        host_fsck_clean(vm)


# ======================================================================= file system
@test
def persistence_across_reboots():
    vm = VM(img())
    try:
        vm.boot()
        vm.out("mkdir -p /data/docs/deep/er")
        vm.out("seq 1 5000 > /data/docs/numbers.txt")
        vm.out("echo first > /data/docs/deep/er/x.txt")
        vm.out("cp /bin/sh /data/docs/sh.copy")
        vm.shutdown()
        host_fsck_clean(vm)
        for n in range(3):
            vm.start()
            vm.wait_prompt(120)
            check("not cleanly unmounted" not in vm.text, "clean shutdown left the fs dirty")
            eq(vm.out("wc /data/docs/numbers.txt").split()[:3], ["5000", "5000", "23893"], "numbers after reboot")
            eq(vm.out("cat /data/docs/deep/er/x.txt").strip().split("\n")[-1], ["first", "second", "third"][n],
               "appended file after reboot %d" % n)
            vm.out("echo %s > /data/docs/deep/er/x.txt" % ["second", "third", "fourth"][n])
            vm.out("cp /data/docs/sh.copy /tmp/sh2")
            vm.out("/tmp/sh2 -c 'echo copied-shell-runs'")
            vm.shutdown()
            host_fsck_clean(vm)
        sh = open(os.path.join(ARGS.build, "rootfs", "bin", "sh"), "rb").read()
        r2 = subprocess.run([os.path.join(ARGS.build, "tools", "ffstool"), "cat", vm.image, "/docs/sh.copy"],
                            capture_output=True)
        eq(r2.stdout == sh, True, "binary copied inside the OS is byte-identical")
    finally:
        vm.close()


@test
def power_loss_recovery():
    rnd = random.Random(2026)
    vm = VM(img())
    try:
        vm.boot()
        vm.out("mkdir /data/keep && seq 1 300 > /data/keep/safe.txt && sync")
        vm.shutdown()
        for i in range(6):
            vm.start()
            vm.wait_prompt(120)
            if i:
                check("check done" in vm.text or "not cleanly" in vm.text,
                      "kernel did not check the dirty file system after power loss")
            eq(vm.out("wc -l /data/keep/safe.txt").split()[0], "300", "synced file survives power loss")
            # heavy writes, then pull the plug at a random moment
            vm.sendline("/test/fstest /data")
            time.sleep(rnd.uniform(1.0, 6.0))
            vm.kill()
        vm.start()
        vm.wait_prompt(120)
        vm.out("/test/fstest /data", timeout=900)
        vm.shutdown()
        out = host_fsck_clean(vm)
        print("      final fsck: " + out.strip().splitlines()[-1])
    finally:
        vm.close()


@test
def fs_operations():
    with Booted() as vm:
        vm.out("cd /data && mkdir t && cd t")
        eq(vm.out("pwd").strip(), "/data/t", "cd/pwd")
        vm.out("echo alpha > a.txt; echo beta >> a.txt")
        eq(vm.out("cat a.txt").strip().split("\n"), ["alpha", "beta"], "append")
        vm.out("mv a.txt b.txt")
        st, out = vm.run("cat a.txt", check=False)
        check(st != 0 and "no such file" in out, "renamed file gone")
        vm.out("cp b.txt c.txt && mkdir sub && mv c.txt sub/")
        eq(vm.out("ls sub").strip(), "c.txt", "move into dir")
        vm.out("mv sub/c.txt /tmp/c.txt")  # across file systems
        eq(vm.out("cat /tmp/c.txt").strip().split("\n"), ["alpha", "beta"], "cross-fs move")
        st, out = vm.run("rmdir /data/t", check=False)
        check(st != 0 and "busy" in out, "cannot remove the cwd: " + out)
        st, out = vm.run("rm sub", check=False)
        check(st != 0 and "is a directory" in out, "rm dir needs -r")
        vm.out("touch sub/x sub/y && rm -r sub")
        st, out = vm.run("ls sub", check=False)
        check(st != 0, "rm -r removed dir")
        vm.out("cd / && rm -r /data/t")
        st, out = vm.run("rm /data", check=False)
        check(st != 0, "cannot remove mount point")
        st, out = vm.run("mkdir /data/" + "x" * 70, check=False)
        check("too long" in out, "long name rejected")
        out = vm.out("stat /data/README.txt")
        check("Type: file" in out, out)
        vm.shutdown()
        host_fsck_clean(vm)


# ======================================================================= shell + utilities
@test
def shell_features():
    with Booted() as vm:
        eq(vm.out("echo a b   c").strip(), "a b c", "word splitting")
        eq(vm.out("echo 'a  b' \"c  d\" e\\ f").strip(), "a  b c  d e f", "quoting")
        eq(vm.out("X=42; echo $X ${X}x \"$X\" '$X'").strip(), "42 42x 42 $X", "variables")
        eq(vm.out("false; echo $?").strip(), "1", "$? after false")
        eq(vm.out("true && echo yes || echo no").strip(), "yes", "&& ||")
        eq(vm.out("false && echo yes || echo no").strip(), "no", "|| after failed &&")
        eq(vm.out("seq 1 100 | grep 7 | wc -l").split()[0], "19", "3-stage pipeline")
        eq(vm.out("seq 1 3 > /tmp/s; seq 4 5 >> /tmp/s; wc -l < /tmp/s").split()[0], "5", "redirections")
        vm.out("cd /tmp && touch g1.c g2.c g3.h")
        eq(vm.out("echo g*.c").strip(), "g1.c g2.c", "glob *")
        eq(vm.out("echo g?.h").strip(), "g3.h", "glob ?")
        eq(vm.out("echo nomatch*").strip(), "nomatch*", "unmatched glob stays literal")
        vm.out("echo 'echo script-arg=$1' > /tmp/s.sh && echo 'exit 3' >> /tmp/s.sh")
        st, out = vm.run("sh /tmp/s.sh hello", check=False)
        eq((st, out.strip()), (3, "script-arg=hello"), "script with args and exit code")
        st, out = vm.run("/tmp/s.sh viaexec", check=False)
        eq(out.strip(), "script-arg=viaexec", "text file runs as script")
        eq(vm.out("sh -c 'echo inner; exit 0'").strip(), "inner", "sh -c")
        st, out = vm.run("nosuchcommand", check=False)
        check(st == 127 and "command not found" in out, "unknown command")
        out = vm.out("sleep 1 &")
        check(re.search(r"\[\d+\]", out), "background job id")
        time.sleep(1.5)
        vm.out("true")
        st, out = vm.run("sh -c \"echo 'unterminated\"", check=False)
        check(st != 0 and "unterminated" in out, "syntax error reported")
        st, out = vm.run("sh -c '| bad'", check=False)
        check(st != 0 and "syntax" in out, "syntax error for leading pipe")
        out = vm.out("history")
        check("seq 1 100" in out, "history recorded")
        vm.shutdown()


@test
def line_editing_and_completion():
    with Booted() as vm:
        # type, move left twice, insert, then Enter
        vm.send("echo wrd\x1b[D\x1b[Do\r")
        vm.expect("word", 10)
        vm.wait_prompt()
        # history: up arrow recalls the previous command
        vm.send("\x1b[A\r")
        vm.expect("word", 10)
        vm.wait_prompt()
        # Ctrl+U clears the line, Ctrl+A/E move
        vm.send("garbage\x15echo fixed\r")
        vm.expect("fixed", 10)
        vm.wait_prompt()
        # Tab completion of a command and a file name
        vm.send("sysi\t\r")
        vm.expect("Fresh OS 0.1.0", 10)
        vm.wait_prompt()
        vm.send("cat /data/READ\t\r")
        vm.expect("This is your Fresh OS data folder", 10)
        vm.wait_prompt()
        # Ctrl+C on an empty prompt and while a program runs
        vm.send("\x03")
        vm.wait_prompt()
        vm.sendline("sleep 30")
        time.sleep(1.0)
        t0 = time.time()
        vm.send("\x03")
        vm.wait_prompt(15)
        check(time.time() - t0 < 10, "Ctrl+C did not stop sleep")
        eq(vm.out("echo $?").strip(), "130", "status after Ctrl+C")
        # Ctrl+D ends cat reading the terminal; typed lines are echoed back
        vm.sendline("cat")
        time.sleep(0.5)
        vm.send("line-one\r")
        vm.expect("line-one\nline-one", 10)
        vm.send("\x04")
        vm.wait_prompt(10)
        vm.shutdown()


@test
def utilities():
    with Booted() as vm:
        out = vm.out("ls /bin")
        for prog in ["sh", "ls", "cat", "edit", "grep", "ps", "init"]:
            check(re.search(r"\b%s\b" % prog, out), "ls /bin lists " + prog)
        out = vm.out("ls -l /data")
        check(re.search(r"-\s+\d+\s+\d{4}-\d\d-\d\d \d\d:\d\d\s+README.txt", out), "ls -l format: " + out)
        out = vm.out("ps")
        check(re.search(r"\s1\s+0\s+\w+.*init", out) and "sh" in out and "ps" in out, "ps: " + out)
        out = vm.out("free")
        check(re.search(r"Mem:\s+\d+\s+\d+\s+\d+", out), out)
        check(re.search(r"up \d+:\d\d:\d\d, \d+ processes", vm.out("uptime")), "uptime")
        check(re.search(r"\w{3} \w{3} [ \d]\d \d\d:\d\d:\d\d UTC 20\d\d", vm.out("date")), "date")
        host_year = time.gmtime().tm_year
        check(str(host_year) in vm.out("date"), "RTC year matches host")
        out = vm.out("df")
        check("ramfs" in out and "freshfs" in out, out)
        out = vm.out("echo -n abc | hexdump")
        check("61 62 63" in out and "|abc|" in out, out)
        eq(vm.out("seq 5 | head -n 2").split(), ["1", "2"], "head")
        eq(vm.out("seq 5 | tail -n 2").split(), ["4", "5"], "tail")
        eq(vm.out("seq 20 | grep -c 1").strip(), "11", "grep -c")
        eq(vm.out("seq 3 | grep -v 2").split(), ["1", "3"], "grep -v")
        eq(vm.out("echo Hello | grep -i hello").strip(), "Hello", "grep -i")
        eq(vm.out("echo abc | wc").split(), ["1", "1", "4"], "wc")
        eq(vm.out("uname -a").strip(), "Fresh OS 0.1.0 x86_64", "uname")
        out = vm.out("help")
        check("edit FILE" in out and "Files in /data" in out, "help")
        vm.out("sleep 20 &")
        out = vm.out("ps")
        pid = re.search(r"\s(\d+)\s+\d+\s+\w+\s+\d+\s+\d+\s+sleep", out).group(1)
        vm.out("kill " + pid)
        time.sleep(0.5)
        check("sleep" not in vm.out("ps"), "kill stopped sleep")
        st, out = vm.run("kill 1", check=False)
        check(st != 0 and "not permitted" in out, "init cannot be killed")
        vm.shutdown()


@test
def editor():
    with Booted() as vm:
        vm.sendline("edit /data/note.txt")
        vm.expect("New file", 15)
        vm.send("Hello Fresh OS\rsecond line\x1b[A\x1b[H>> \x13")
        vm.expect("Saved 2 lines", 15)
        vm.send("\x11")
        vm.wait_prompt(15)
        eq(vm.out("cat /data/note.txt").strip().split("\n"), [">> Hello Fresh OS", "second line"], "saved text")
        # reopen, delete a char, unsaved quit needs a second Ctrl+Q
        vm.sendline("edit /data/note.txt")
        vm.expect("note.txt", 15)
        vm.send("\x1b[3~")
        time.sleep(0.5)
        vm.send("\x11")
        vm.expect("Unsaved changes", 15)
        vm.send("\x11")
        vm.wait_prompt(15)
        eq(vm.out("cat /data/note.txt").strip().split("\n")[0], ">> Hello Fresh OS", "quit without saving")
        # find + edit + save
        vm.sendline("edit /data/note.txt")
        vm.expect("note.txt", 15)
        vm.send("\x06second\r")
        vm.expect("Found 'second' at line 2", 15)
        vm.send("2nd \x13")
        vm.expect("Saved", 15)
        vm.send("\x11")
        vm.wait_prompt(15)
        eq(vm.out("cat /data/note.txt").strip().split("\n")[1], "2nd second line", "edit after find")
        vm.shutdown()
        r = ffstool("cat", vm.image, "/note.txt")
        eq(r.stdout, ">> Hello Fresh OS\n2nd second line\n", "editor output on disk")


# ======================================================================= devices
@test
def ps2_keyboard():
    with Booted() as vm:
        keys = list("echo") + ["spc"] + list("kbd") + ["shift-o", "shift-k", "ret"]
        vm.sendkeys(keys)
        vm.expect("kbdOK", 20)
        vm.wait_prompt()
        vm.sendkeys(["ctrl-c"])
        vm.wait_prompt()
        vm.sendkeys(list("pwd") + ["ret"])
        vm.expect("/data", 20)
        vm.shutdown()


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(maxsplit=4)
    check(parts[0] == b"P6", "screendump is not a PPM")
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4]


@test
def framebuffer_console():
    for fw in ["bios", "uefi"]:
        with Booted(firmware=fw) as vm:
            vm.out("echo SCREENTEST")
            shot = os.path.join(vm.workdir, "screen.ppm")
            vm.monitor("screendump " + shot)
            time.sleep(0.5)
            w, h, px = read_ppm(shot)
            eq((w, h), (1024, 768), "%s screen size" % fw)
            colors = {}
            for i in range(0, len(px) - 2, 3 * 7):
                c = px[i:i + 3]
                colors[c] = colors.get(c, 0) + 1
            bg = bytes([0x0E, 0x11, 0x16])
            fg = bytes([0xCD, 0xD5, 0xDF])
            check(colors.get(bg, 0) > len(px) // 21 // 2, "%s: background colour missing" % fw)
            check(colors.get(fg, 0) > 200, "%s: no text pixels on screen" % fw)
            check(any(abs(c[0] - 0x7E) < 3 and abs(c[1] - 0xE2) < 3 for c in colors), "%s: green prompt missing" % fw)
            vm.shutdown()


@test
def reboot_command():
    with Booted() as vm:
        vm.out("echo before-reboot > /data/r.txt")
        vm.sendline("reboot")
        vm.wait_exit(60)  # -no-reboot turns the reset into an exit
        check("Restarting" in vm.text, "reboot message")
        host_fsck_clean(vm)
        eq(ffstool("cat", vm.image, "/r.txt").stdout.strip(), "before-reboot", "data synced before reboot")


# ======================================================================= second compiler
@test
def gcc_built_image():
    gcc_img = os.path.join(os.path.dirname(ARGS.build), "build-gcc", "fresh-os.img")
    check(os.path.exists(gcc_img), "GCC image missing (run 'make gcc-image'): " + gcc_img)
    for fw in ["bios", "uefi"]:
        with Booted(image=gcc_img, firmware=fw) as vm:
            out = vm.out("/test/ktest", timeout=300)
            check("ALL PASSED" in out, out)
            for cmd in ["/test/spawntest 300", "/test/memtest 5000", "/test/pipetest", "/test/fstest /data",
                        "/test/crashtest badptr"]:
                out = vm.out(cmd, timeout=600)
                check("FAIL" not in out, "%s: %s" % (cmd, out))
            st, out = vm.run("/test/crashtest null", check=False)
            eq(st, 139, "fault isolation with GCC build")
            eq(vm.out("seq 1 50 | grep 5 | wc -l").split()[0], "6", "pipeline")
            vm.shutdown()
            host_fsck_clean(vm)


# ======================================================================= soak
@test
def soak():
    seconds = ARGS.soak
    with Booted() as vm:
        free0 = int(re.search(r"Mem:\s+\d+\s+\d+\s+(\d+)", vm.out("free")).group(1))
        end = time.time() + seconds
        rounds = 0
        while time.time() < end:
            for cmd in ["/test/spawntest 100", "/test/memtest 2000", "/test/fstest /data", "/test/fstest /tmp",
                        "/test/pipetest", "seq 1 2000 | grep 5 | wc -l"]:
                st, out = vm.run(cmd, timeout=900, check=False)
                check(st == 0 and "FAIL" not in out, "soak: %s failed: %s" % (cmd, out))
            rounds += 1
        free1 = int(re.search(r"Mem:\s+\d+\s+\d+\s+(\d+)", vm.out("free")).group(1))
        print("      soak: %d rounds in %ds, free memory %d KiB -> %d KiB" % (rounds, seconds, free0, free1))
        check(free0 - free1 < 2048, "memory dropped by %d KiB during soak" % (free0 - free1))
        vm.out("/test/ktest", timeout=300)
        vm.shutdown()
        host_fsck_clean(vm)


# ======================================================================= main
def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--soak", type=int, default=600)
    ap.add_argument("--list", action="store_true")
    ARGS = ap.parse_args()
    ARGS.build = os.path.abspath(ARGS.build)
    if ARGS.list:
        for t in TESTS:
            print(t.__name__)
        return 0
    only = set(filter(None, ARGS.only.split(",")))
    skip = set(filter(None, ARGS.skip.split(",")))
    selected = [t for t in TESTS if (not only or t.__name__ in only) and t.__name__ not in skip]
    failed = []
    t_all = time.time()
    for t in selected:
        t0 = time.time()
        print("[ RUN  ] %s" % t.__name__, flush=True)
        try:
            t()
            print("[  OK  ] %s (%.0fs)" % (t.__name__, time.time() - t0), flush=True)
        except Exception as e:  # noqa: BLE001
            failed.append(t.__name__)
            print("[ FAIL ] %s (%.0fs)\n%s" % (t.__name__, time.time() - t0,
                                              "".join(traceback.format_exception_only(type(e), e))), flush=True)
            if not isinstance(e, TestFailure):
                traceback.print_exc()
    total = len(selected)
    print("\n%d/%d end-to-end tests passed in %.0fs" % (total - len(failed), total, time.time() - t_all))
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

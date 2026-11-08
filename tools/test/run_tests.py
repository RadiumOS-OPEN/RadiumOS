#!/usr/bin/env python3
"""Headless regression tests for RadiumOS, run in QEMU.

  python3 tools/test/run_tests.py [--only selftest|accounts|binary|ctrlc|desktop|guard] [--keep] [--kernel os.bin]

selftest   boots with `selftest` on the kernel command line; the guest runs src/commands/selftest.c (canary, PRP/crypt,
           account and sudo policy, app capabilities, fsck repairs, safe overwrites, file encryption) and powers off.
accounts   drives the real shell through the QEMU monitor (sendkey) on a fresh disk: useradd, login, wrong password,
           sudo, protected files, su, logout, crypt lock/unlock, fsck, reboot, a power cut (kill) and the unclean-disk check.
binary     checks the kernel binary for stack-protector call sites.

The guest copies its console to the QEMU debug port (`ttylog` on the command line), which is what is read here.
The kernel is booted with -kernel (not from the ISO) so a kernel installed on the disk by `update apply` cannot override it.
Needs qemu-system-i386 and llvm-objdump. Exit status 0 only if every check passed.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

KEYS = {' ': 'spc', '\n': 'ret', '-': 'minus', '_': 'shift-minus', '.': 'dot', ',': 'comma', '/': 'slash',
        '=': 'equal', '+': 'shift-equal', ':': 'shift-semicolon', ';': 'semicolon', '$': 'shift-4',
        '>': 'shift-dot', '<': 'shift-comma', '*': 'shift-8', '"': 'shift-apostrophe', "'": 'apostrophe',
        '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '`': 'grave_accent', '\\': 'backslash',
        '|': 'shift-backslash', '(': 'shift-9', ')': 'shift-0', '[': 'bracket_left', ']': 'bracket_right',
        '%': 'shift-5', '^': 'shift-6', '&': 'shift-7', '?': 'shift-slash'}

PASSED = []
FAILED = []


def check(name, ok, detail=""):
    (PASSED if ok else FAILED).append(name)
    print(("  PASS  " if ok else "  FAIL  ") + name + (("  -- " + detail) if detail and not ok else ""))
    return ok


class VM:
    def __init__(self, kernel, disk, cmdline, workdir, mem="1G"):
        self.kernel, self.disk, self.cmdline, self.mem = kernel, disk, cmdline, mem
        self.log_path = os.path.join(workdir, "debug.log")
        self.mon_path = os.path.join(workdir, "mon.sock")
        self.proc = None
        self.mon = None
        self.pos = 0

    def start(self):
        for p in (self.log_path, self.mon_path):
            if os.path.exists(p):
                os.remove(p)
        args = ["qemu-system-i386", "-display", "none", "-accel", "kvm", "-accel", "tcg", "-m", self.mem,
                "-machine", "pc", "-cpu", "max", "-kernel", self.kernel, "-append", self.cmdline,
                "-drive", f"file={self.disk},format=raw,if=ide,index=0", "-vga", "std", "-serial", "none",
                "-debugcon", f"file:{self.log_path}", "-global", "isa-debugcon.iobase=0xE9",
                "-monitor", f"unix:{self.mon_path},server,nowait", "-no-reboot"]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.pos = 0
        for _ in range(100):
            try:
                self.mon = socket.socket(socket.AF_UNIX)
                self.mon.connect(self.mon_path)
                self.mon.settimeout(0.2)
                break
            except OSError:
                self.mon = None
                time.sleep(0.1)
        if self.mon:
            self._drain()

    def _drain(self):
        try:
            while self.mon.recv(4096):
                pass
        except (socket.timeout, OSError):
            pass

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        if self.mon:
            self.mon.close()
            self.mon = None

    def text(self):
        try:
            with open(self.log_path, "rb") as f:
                return f.read().decode("latin-1")
        except OSError:
            return ""

    def wait(self, pattern, timeout=60, since=None):
        """Waits for a regex in the console text written after `since` (default: after the last match)."""
        start = self.pos if since is None else since
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            t = self.text()
            m = rx.search(t, start)
            if m:
                self.pos = m.end()
                return m
            if not self.alive() and not rx.search(self.text(), start):
                time.sleep(0.1)
                m = rx.search(self.text(), start)
                if m:
                    self.pos = m.end()
                    return m
                return None
            time.sleep(0.1)
        return None

    def key(self, name):
        self.mon.sendall(f"sendkey {name}\n".encode())
        time.sleep(0.07)
        self._drain()

    def type(self, s):
        for ch in s:
            if ch.isalpha() and ch.isupper():
                self.key("shift-" + ch.lower())
            elif ch.isalnum():
                self.key(ch)
            else:
                self.key(KEYS[ch])

    def line(self, s):
        self.type(s + "\n")

    def wait_exit(self, timeout=60):
        try:
            self.proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False


# The shell redraws the prompt after every keystroke, so a prompt that ends a command must follow a newline.
PROMPT_ANY = r"[a-z][a-z0-9_-]*:[^\s]*\$ "
PROMPT = r"(?<=\n)" + PROMPT_ANY


def fresh_disk(workdir):
    path = os.path.join(workdir, "disk.img")
    with open(path, "wb") as f:
        f.truncate(6 << 30)
    return path


def test_binary(kernel):
    print("binary")
    out = subprocess.run(["llvm-objdump", "-d", kernel], capture_output=True, text=True).stdout
    calls = out.count("__stack_chk_fail")
    check("stack protector call sites in the kernel", calls > 100, f"{calls} found")
    nm = subprocess.run(["llvm-nm", kernel], capture_output=True, text=True).stdout
    check("__stack_chk_guard and __stack_chk_fail are defined", "D __stack_chk_guard" in nm and "T __stack_chk_fail" in nm)


def test_selftest(kernel, workdir):
    print("selftest")
    vm = VM(kernel, fresh_disk(workdir), "selftest ttylog nosplash fastboot nonet", workdir)
    vm.start()
    done = vm.wait(r"SELFTEST RESULT (PASS|FAIL) (\d+)/(\d+)", 240)
    vm.wait_exit(30)
    vm.kill()
    text = vm.text()
    check("selftest finished", done is not None, "no result line - the guest did not finish")
    if not done:
        return
    for m in re.finditer(r"^SELFTEST (\S+) (PASS|FAIL) ?(.*)$", text, re.M):
        if m.group(1) not in ("RESULT",):
            check(m.group(1), m.group(2) == "PASS", m.group(3))
    check("selftest summary", done.group(1) == "PASS", done.group(0))


def boot_to_prompt(vm, what, timeout=120):
    return check(what, vm.wait(PROMPT_ANY, timeout) is not None, "no shell prompt")


def run(vm, cmd, expect, what, timeout=40, absent=None):
    """Types a command, waits for the shell prompt and checks the output."""
    start = len(vm.text())
    vm.line(cmd)
    ok = vm.wait(PROMPT, timeout, since=start + len(cmd)) is not None
    out = vm.text()[start:]
    good = ok and re.search(expect, out) is not None and (absent is None or re.search(absent, out) is None)
    check(what, good, out[-300:].replace("\n", "\\n"))
    return out


def test_ctrlc(kernel, workdir):
    print("ctrlc")
    vm = VM(kernel, fresh_disk(workdir), "nosplash fastboot nonet ttylog nodesktop", workdir)
    vm.start()
    if not boot_to_prompt(vm, "ctrlc: shell is up"):
        vm.kill(); return
    start = len(vm.text())
    vm.line("sleep 3000")
    time.sleep(3)
    vm.key("ctrl-c")
    ok = vm.wait(r"\^C\n(.|\n)*?" + PROMPT, 20, since=start) is not None
    check("Ctrl+C ends a command that never reads the keyboard", ok)
    run(vm, "whoami", r"\nroot\n", "the shell works normally afterwards")

    start = len(vm.text())
    vm.line("sleep 3000 && echo reached-after")
    time.sleep(2)
    vm.key("ctrl-c")
    vm.wait(PROMPT, 20, since=start + 30)
    check("an && chain stops with the command", "\nreached-after" not in vm.text()[start + 30:])

    # keys typed while a command runs are kept for the prompt, not lost to the tap
    vm.line("sleep 3000")
    time.sleep(2)
    vm.type("who")
    vm.key("ctrl-c")
    vm.wait(PROMPT, 20)
    vm.type("ami")
    start = len(vm.text())
    vm.key("ret")
    check("typing ahead survives", vm.wait(r"\nroot\n", 20, since=start) is not None)

    # a command that finishes by itself is not disturbed
    run(vm, "sleep 1", r"", "a short command still finishes")
    vm.kill()


def test_desktop(kernel, workdir):
    print("desktop")
    vm = VM(kernel, fresh_disk(workdir), "nosplash fastboot nonet ttylog nodesktop", workdir)
    vm.start()
    if not boot_to_prompt(vm, "desktop: shell is up"):
        vm.kill(); return
    vm.line("desktop selftest")
    m = vm.wait(r"desktop selftest: (\d+) checks, (\d+) failed", 180)
    check("desktop selftest finished", m is not None, "no result line")
    if m:
        fails = [l.strip() for l in vm.text().splitlines() if l.strip().startswith("FAIL")]
        check("desktop selftest: files, tasks and wallpaper", m.group(2) == "0", "; ".join(fails))
        check("desktop selftest ran its checks", int(m.group(1)) >= 20, m.group(0))
    vm.kill()


def test_guard(kernel, workdir):
    print("guard")
    vm = VM(kernel, fresh_disk(workdir), "nosplash fastboot nonet ttylog nodesktop", workdir)
    vm.start()
    if not boot_to_prompt(vm, "guard: shell is up"):
        vm.kill(); return
    time.sleep(60)                                    # long enough for the text guard to pass over the kernel once
    start = len(vm.text())
    vm.line("guard")
    m = vm.wait(r"full passes(.|\n)*?alerts\s+(\d+)", 30, since=start)
    check("guard command answers", m is not None)
    if m:
        txt = vm.text()[start:]
        run_ = re.search(r"guard tasks running\s+(\d+) of 4", txt)
        passes = re.search(r"(\d+) full passes", txt)
        check("four guard tasks are running", bool(run_) and run_.group(1) == "4", run_.group(0) if run_ else "no line")
        check("the kernel image was verified at least once", bool(passes) and int(passes.group(1)) >= 1, passes.group(0) if passes else "no line")
        check("a quiet system raises no alerts", m.group(2) == "0", txt[-300:])
    start = len(vm.text())
    vm.line("guard selftest")
    m = vm.wait(r"guard selftest: (\d) of 5 checks passed", 60, since=start)
    check("guard selftest finished", m is not None)
    if m:
        fails = [l.strip() for l in vm.text()[start:].splitlines() if "FAIL" in l]
        check("guard selftest: rogue task, respawn, gate restore, protected kill", m.group(1) == "5", "; ".join(fails))
    run(vm, "whoami", r"\nroot\n", "the shell still works after the injected faults")
    vm.kill()


def test_accounts(kernel, workdir):
    print("accounts")
    disk = fresh_disk(workdir)
    base = "nosplash fastboot nonet ttylog nodesktop"
    vm = VM(kernel, disk, base, workdir)
    vm.start()
    if not boot_to_prompt(vm, "first boot reaches a shell (no accounts: single-user root)"):
        vm.kill(); return
    run(vm, "whoami", r"\nroot\n", "whoami is root before any account exists")

    vm.line("useradd -s alice")
    ok = vm.wait("New password", 20) and (vm.line("alicepw1") or True) and vm.wait("Retype", 20) and (vm.line("alicepw1") or True)
    out = vm.wait(r"account alice created[^\n]*\n(.|\n)*?" + PROMPT, 90)
    check("useradd -s alice", bool(ok and out))
    vm.line("useradd bob")
    ok = vm.wait("New password", 20) and (vm.line("bobpw123") or True) and vm.wait("Retype", 20) and (vm.line("bobpw123") or True)
    out = vm.wait(r"account bob created[^\n]*\n(.|\n)*?" + PROMPT, 90)
    check("useradd bob (root session continues after the first account)", bool(ok and out))
    run(vm, "users", r"alice\s+1000\s+yes\s+set(.|\n)*bob\s+1001\s+no\s+set|bob\s+1001\s+no\s+set(.|\n)*alice", "users lists both accounts")
    run(vm, "fsck", r"the volume is clean", "fsck reports a clean volume", 120)
    run(vm, "stat /disk/etc/passwd", r"passwd", "the account database is on /disk")

    vm.line("reboot")
    vm.wait_exit(60)
    check("reboot restarts the VM", not vm.alive())
    vm.kill()

    # ---- second boot: login required
    vm = VM(kernel, disk, base, workdir)
    vm.start()
    check("login prompt after reboot (clean shutdown: no disk warning)",
          vm.wait(r"login: ", 120) is not None and "did not shut down cleanly" not in vm.text())
    vm.line("alice")
    vm.wait("password: ", 20)
    vm.line("wrongpass")
    check("wrong password is refused", vm.wait("Login incorrect", 30) is not None)
    vm.wait(r"login: ", 30)
    vm.line("alice")
    vm.wait("password: ", 20)
    vm.line("alicepw1")
    if not boot_to_prompt(vm, "alice logs in", 120):
        vm.kill(); return

    run(vm, "whoami", r"\nalice\n", "whoami is alice")
    run(vm, "id", r"uid=1000\(alice\).*sudo", "id shows the sudo group")
    run(vm, "cat /disk/etc/passwd", r"permission denied", "a normal user cannot read the account database", absent=r"\$rp1\$")
    run(vm, "echo x > /disk/etc/passwd", r"permission denied", "...nor overwrite it through a redirection")
    run(vm, "rm -r /disk", r"permission denied", "...nor delete a folder that contains it")
    run(vm, "reboot", r"permission denied - run: sudo reboot", "reboot needs sudo")
    run(vm, "useradd carol", r"permission denied", "useradd needs root")

    vm.line("sudo cat /disk/etc/passwd")
    ok = vm.wait(r"\[sudo\] password for alice: ", 20)
    vm.line("alicepw1")
    m = vm.wait(PROMPT, 40)
    out = vm.text()
    check("sudo asks alice for her own password and runs the command", bool(ok and m and "alice:1000:1:$rp1$" in out and "bob:1001:0:$rp1$" in out))
    check("the account database holds hashes, never passwords", "alicepw1" not in out.split("[sudo] password for alice: ")[-1] and "bobpw123" not in out)

    vm.line("sudo sleep 3000")
    time.sleep(2)
    vm.key("ctrl-c")
    vm.wait(PROMPT, 20)
    run(vm, "reboot", r"permission denied - run: sudo reboot", "Ctrl+C inside sudo does not leave the shell elevated")

    vm.line("su bob")
    vm.wait("Password: ", 20)
    vm.line("bobpw123")
    check("su bob with bob's password", vm.wait(r"bob:", 30) is not None)
    vm.wait(PROMPT, 20)
    run(vm, "sudo id", r"not allowed to use sudo", "bob has no sudo rights")

    run(vm, "echo secret-data > /disk/home/bob/n.txt", r"", "bob writes into his home")
    vm.line("crypt lock /disk/home/bob/n.txt")
    vm.wait("Passphrase: ", 20)
    vm.line("long-passphrase")
    vm.wait("Retype passphrase: ", 20)
    vm.line("long-passphrase")
    out = vm.wait(r"crypt: 1 file\(s\) locked, 0 skipped, 0 failed(.|\n)*?" + PROMPT, 90)
    check("crypt lock encrypts a file", out is not None)
    start = len(vm.text())
    vm.line("cat /disk/home/bob/n.txt")
    vm.wait(PROMPT, 30, since=start + 30)
    check("the locked file no longer shows its content", "secret-data" not in vm.text()[start + 30:] and "RDCRYPT1" in vm.text()[start:])
    vm.line("crypt unlock /disk/home/bob/n.txt")
    vm.wait("Passphrase: ", 20)
    vm.line("wrong-passphrase")
    check("crypt unlock with a wrong passphrase fails", vm.wait(r"wrong passphrase or damaged", 60) is not None)
    vm.wait(PROMPT, 30)
    vm.line("crypt unlock /disk/home/bob/n.txt")
    vm.wait("Passphrase: ", 20)
    vm.line("long-passphrase")
    vm.wait(r"1 file\(s\) unlocked", 60)
    vm.wait(PROMPT, 30)
    run(vm, "cat /disk/home/bob/n.txt", r"secret-data", "crypt unlock restores the file")

    vm.line("logout")
    check("logout shows the login prompt", vm.wait(r"login: ", 30) is not None)
    vm.line("bob")
    vm.wait("password: ", 20)
    vm.line("bobpw123")
    boot_to_prompt(vm, "bob logs in again", 60)
    run(vm, "fsck", r"the volume is clean", "fsck still reports a clean volume", 120)

    # ---- power cut: the volume must be flagged and checked at the next boot
    vm.kill()
    vm = VM(kernel, disk, base, workdir)
    vm.start()
    check("after a power cut the disk is flagged and checked",
          vm.wait(r"did not shut down cleanly: checked \d+ files, no problems", 120) is not None)
    vm.wait(r"login: ", 60)
    vm.kill()

    # ---- recovery switch
    vm = VM(kernel, disk, base + " nologin", workdir)
    vm.start()
    check("nologin skips the login and warns", vm.wait(r"nologin on the kernel command line", 120) is not None)
    boot_to_prompt(vm, "...and gives a root shell")
    run(vm, "whoami", r"\nroot\n", "nologin session is root")
    vm.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["selftest", "accounts", "binary", "ctrlc", "desktop", "guard"])
    ap.add_argument("--keep", action="store_true", help="keep the work directory (disk image, console log)")
    ap.add_argument("--kernel", default=os.path.join(ROOT, "os.bin"))
    a = ap.parse_args()
    if not os.path.exists(a.kernel):
        print("missing " + a.kernel + " - run make first")
        return 2
    work = tempfile.mkdtemp(prefix="radiumtest-")
    try:
        if a.only in (None, "binary"):
            test_binary(a.kernel)
        if a.only in (None, "selftest"):
            test_selftest(a.kernel, work)
        if a.only in (None, "ctrlc"):
            test_ctrlc(a.kernel, work)
        if a.only in (None, "desktop"):
            test_desktop(a.kernel, work)
        if a.only in (None, "guard"):
            test_guard(a.kernel, work)
        if a.only in (None, "accounts"):
            test_accounts(a.kernel, work)
    finally:
        if a.keep:
            print("work directory kept: " + work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"\n{len(PASSED)} passed, {len(FAILED)} failed")
    for n in FAILED:
        print("  failed: " + n)
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())

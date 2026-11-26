#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A production image's first two boots, typed at its keyboard (docs/shell/s2.md).

    scripts/dev/verify-first-boot.py <image.vdi>

A shipped image has no password. The first boot must ask for root's and the
user's before anybody can log in; the password must then work; and after the
power is cut - no shutdown, the machine simply stops - the second boot must not
ask again and must take the same password. Works on a copy: the image is left
as it was. Keys go in through QEMU's monitor, because a console's input is the
keyboard's. Prints first_boot=ok, or first_boot=FAILED and where.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PASSWORD = "firstboot1"
# QEMU's sendkey names keys by where they are on a US keyboard, and the
# machine's keyboard is Italian since 2026-11-25: '-' is the key a US
# keyboard calls slash. Letters and digits are where they were.
KEYS = {" ": "spc", "\n": "ret", "-": "slash"}


def find(paths):
    return next((p for p in paths if os.path.exists(p)), None)


class Machine:
    def __init__(self, disk, tmp):
        code = find(["/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd",
                     "/usr/share/ovmf/OVMF_CODE.fd"])
        tmpl = find(["/usr/share/OVMF/OVMF_VARS_4M.fd", "/usr/share/OVMF/OVMF_VARS.fd",
                     "/usr/share/ovmf/OVMF_VARS.fd"])
        if not code or not tmpl:
            raise RuntimeError("OVMF firmware not found")
        vars_fd = os.path.join(tmp, "vars.fd")
        if not os.path.exists(vars_fd):
            shutil.copyfile(tmpl, vars_fd)
        ser, mon = os.path.join(tmp, "serial.sock"), os.path.join(tmp, "monitor.sock")
        for p in (ser, mon):
            if os.path.exists(p):
                os.unlink(p)
        # The same accelerator as every gate here: none (CLAUDE.md).
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-machine", "q35", "-accel", "tcg", "-smp", "2", "-m", "512M", "-display", "none",
             "-chardev", "socket,id=s0,path=%s,server=on,wait=off" % ser, "-serial", "chardev:s0",
             "-monitor", "unix:%s,server=on,wait=off" % mon,
             "-drive", "if=pflash,format=raw,readonly=on,file=%s" % code,
             "-drive", "if=pflash,format=raw,file=%s" % vars_fd,
             "-drive", "file=%s,if=none,id=d0,format=raw" % disk,
             "-device", "virtio-blk-pci,drive=d0,bootindex=1", "-net", "none", "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.serial = self.connect(ser)
        self.monitor = self.connect(mon)
        self.text = ""
        self.at = 0

    def connect(self, path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        end = time.monotonic() + 15
        while True:
            try:
                s.connect(path)
                s.setblocking(False)
                return s
            except OSError:
                if time.monotonic() > end or self.qemu.poll() is not None:
                    raise RuntimeError("QEMU did not open " + path)
                time.sleep(0.05)

    def pump(self):
        for s in (self.serial, self.monitor):
            try:
                data = s.recv(65536)
            except BlockingIOError:
                continue
            if s is self.serial and data:
                self.text += data.decode("utf-8", errors="replace").replace("\r", "")

    def expect(self, word, timeout=240):
        """Past what has been matched already: each prompt is a new one."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            i = self.text.find(word, self.at)
            if i >= 0:
                self.at = i + len(word)
                return
            if self.qemu.poll() is not None:
                break
            time.sleep(0.05)
        raise RuntimeError("never saw %r; the console ended: %r" % (word, self.text[-300:]))

    def type(self, line):
        for c in line:
            self.monitor.sendall(("sendkey %s\n" % KEYS.get(c, c)).encode())
            time.sleep(0.12)
            self.pump()

    def cut_power(self):
        self.qemu.kill()
        self.qemu.wait()
        self.serial.close()
        self.monitor.close()


def check_names(disk, tmp):
    """What a production root has, read off its partition by debugfs: the
    system's programs under their names, and none of the gate's test programs
    (docs/shell/s3.md)."""
    esp, root = os.path.join(tmp, "esp.part"), os.path.join(tmp, "root.part")
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "make-boot-disk.py")
    subprocess.run([sys.executable, tool, "extract", disk, esp, root], check=True, stdout=subprocess.DEVNULL)

    def there(path):
        r = subprocess.run(["debugfs", "-R", "stat %s" % path, root], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True)
        return "Inode:" in r.stdout
    for path in ("/usr/bin/busybox", "/usr/bin/login", "/usr/bin/sh", "/usr/bin/ls", "/usr/lib/vibeos/init"):
        if not there(path):
            raise RuntimeError("a production root has no " + path)
    for path in ("/usr/lib/vibeos/test", "/usr/bin/BUSYBOX.ELF", "/usr/bin/INIT.ELF"):
        if there(path):
            raise RuntimeError("a production root has " + path)
    os.unlink(esp)
    os.unlink(root)


def run(image, tmp):
    disk = os.path.join(tmp, "disk.raw")
    subprocess.run(["qemu-img", "convert", "-O", "raw", image, disk], check=True)
    check_names(disk, tmp)

    m = Machine(disk, tmp)
    try:
        for who in ("root", "vibe"):
            m.expect("New password for %s: " % who)
            m.type("short\n")                       # refused: under eight characters
            m.expect("At least 8 characters")
            m.expect("New password for %s: " % who)
            m.type(PASSWORD + "\n")
            m.expect("Again: ")
            m.type(PASSWORD + "\n")
        m.expect("vibeos login: ")
        m.type("root\n")
        m.expect("Password: ")
        m.type("wrongpassword\n")
        m.expect("Login incorrect")
        m.expect("vibeos login: ")
        m.type("root\n")
        m.expect("Password: ")
        m.type(PASSWORD + "\n")
        m.expect("# ")
        m.type("id -u\n")
        m.expect("ring3): 0\n")   # the output, whole: the typed echo goes a byte at a time
    finally:
        m.cut_power()     # no shutdown: what fsync promised is what survives

    m = Machine(disk, tmp)
    try:
        m.expect("vibeos login: ")
        if "New password" in m.text:
            raise RuntimeError("the second boot asked for a password again")
        m.type("vibe\n")
        m.expect("Password: ")
        m.type(PASSWORD + "\n")
        m.expect("$ ")
        m.type("id -u\n")
        m.expect("ring3): 1000\n")
    finally:
        m.cut_power()


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    with tempfile.TemporaryDirectory(prefix="vibeos-first-boot-") as tmp:
        try:
            run(sys.argv[1], tmp)
        except (RuntimeError, OSError, subprocess.SubprocessError) as e:
            print("first_boot=FAILED %s" % e)
            return 1
    print("first_boot=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Boot until it hangs, then ask every core where it is.

    scripts/dev/catch-hang.py [build-dir] [attempts]

A hung guest has stopped producing serial output, which is the one diagnostic
channel this system has. The QEMU monitor is the way past that: `info registers
-a` dumps every core's state, and resolving each RIP against the kernel's
symbol table turns "it froze" into a function name.

Two intermittent hangs were diagnosed this way that no amount of extra logging
could have found, because the code that was stuck held the lock the logger
needed.

Note the accelerator: TCG, always. Verifying under KVM while CI runs pure
emulation once produced roughly twenty clean local runs against a hang that CI
hit two times in three.
"""

import os
import re
import subprocess
import sys
import tempfile
import time

QUIET_SECONDS = 45
BOOT_BUDGET = 240


def symbolize(kernel_elf, addr):
    """Nearest preceding symbol for an address, via nm."""
    try:
        out = subprocess.run(["nm", "-n", kernel_elf], capture_output=True,
                             text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    best = ""
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            value = int(parts[0], 16)
        except ValueError:
            continue
        if value <= addr:
            best = "%s+0x%x" % (parts[2], addr - value)
        else:
            break
    return best


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    attempts = int(sys.argv[2]) if len(sys.argv) > 2 else 10

    efi_root = os.path.join(build, "artifacts", "efi_root")
    kernel_elf = os.path.join(efi_root, "EFI", "BOOT", "VIBEOSKR.ELF")
    ovmf = "/usr/share/OVMF/OVMF_CODE_4M.fd"
    if not os.path.exists(ovmf):
        ovmf = "/usr/share/ovmf/OVMF.fd"

    for attempt in range(1, attempts + 1):
        with tempfile.TemporaryDirectory() as tmp:
            serial = os.path.join(tmp, "serial.log")
            monitor = os.path.join(tmp, "mon.sock")
            proc = subprocess.Popen([
                "qemu-system-x86_64", "-accel", "tcg", "-smp", "4", "-m", "512",
                "-drive", "if=pflash,format=raw,readonly=on,file=" + ovmf,
                "-drive", "if=none,id=esp,format=raw,file=fat:rw:" + efi_root,
                "-device", "virtio-blk-pci,drive=esp",
                "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
                "-display", "none",
                "-serial", "file:" + serial,
                "-monitor", "unix:" + monitor + ",server,nowait",
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            start = time.time()
            last_size, last_change = 0, time.time()
            hung = False
            while time.time() - start < BOOT_BUDGET:
                if proc.poll() is not None:
                    break
                size = os.path.getsize(serial) if os.path.exists(serial) else 0
                if size != last_size:
                    last_size, last_change = size, time.time()
                elif time.time() - last_change > QUIET_SECONDS and size > 0:
                    hung = True
                    break
                time.sleep(1)

            if hung:
                print("attempt %d: HUNG - asking the monitor where each core is"
                      % attempt)
                try:
                    import socket
                    s = socket.socket(socket.AF_UNIX)
                    s.connect(monitor)
                    s.sendall(b"info registers -a\n")
                    time.sleep(2)
                    dump = s.recv(200000).decode("utf-8", "replace")
                    s.close()
                except OSError as exc:
                    dump = "monitor unreachable: %s" % exc
                for cpu_match in re.finditer(r"CPU#(\d+).*?RIP=([0-9a-f]{16})",
                                             dump, re.S):
                    rip = int(cpu_match.group(2), 16)
                    print("  CPU#%s rip=0x%016x %s"
                          % (cpu_match.group(1), rip,
                             symbolize(kernel_elf, rip)))
                print("  serial tail:")
                with open(serial, "rb") as fh:
                    for line in fh.read().decode("utf-8", "replace").splitlines()[-15:]:
                        print("    " + line)
                proc.kill()
                return 1

            proc.kill()
            print("attempt %d: completed normally" % attempt)

    print("did not reproduce")
    return 0


if __name__ == "__main__":
    sys.exit(main())

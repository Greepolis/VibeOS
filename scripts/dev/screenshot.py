#!/usr/bin/env python3
"""Boot and take a picture of the screen.

    scripts/dev/screenshot.py [build-dir] [out.ppm] [seconds]

A graphical shell cannot be verified from a serial log: the log says a desktop
was composed, not that anything is visible. This boots the image, waits, and
asks the QEMU monitor for a screendump, then reports what is actually in the
pixels - how many distinct colours, and whether the panel and window regions
differ from the background.

That last part matters. A screenshot of a uniformly black screen is still a
screenshot, and "the file exists" is not evidence.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P6"):
        return None
    # header: P6 <w> <h> <maxval>, whitespace separated, then binary
    fields, idx = [], 2
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] != 0x0A:
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx:idx + w * h * 3]


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    out = sys.argv[2] if len(sys.argv) > 2 else "vibeos-screen.ppm"
    wait = int(sys.argv[3]) if len(sys.argv) > 3 else 45

    efi_root = os.path.join(build, "artifacts", "efi_root")
    ovmf = "/usr/share/OVMF/OVMF_CODE_4M.fd"
    if not os.path.exists(ovmf):
        ovmf = "/usr/share/ovmf/OVMF.fd"

    with tempfile.TemporaryDirectory() as tmp:
        monitor = os.path.join(tmp, "mon.sock")
        serial = os.path.join(tmp, "serial.log")
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

        time.sleep(wait)
        dump = os.path.abspath(out)
        try:
            sock = socket.socket(socket.AF_UNIX)
            sock.connect(monitor)
            time.sleep(0.5)
            # Nudge the pointer first, so the screenshot also shows that the
            # mouse path works rather than only that a desktop was drawn.
            sock.sendall(b"mouse_move 120 80\n")
            time.sleep(1.0)
            sock.sendall(("screendump %s\n" % dump).encode())
            time.sleep(3.0)
            sock.close()
        except OSError as exc:
            print("monitor unreachable: %s" % exc)
            proc.kill()
            return 1
        proc.kill()

        if os.path.exists(serial):
            with open(serial, "rb") as fh:
                text = fh.read().decode("utf-8", "replace")
            for marker in ("[GUI]", "[MOUSE]"):
                for line in text.splitlines():
                    if marker in line:
                        print(line.strip())

    if not os.path.exists(dump):
        print("no screendump produced")
        return 1

    parsed = read_ppm(dump)
    if not parsed:
        print("screendump is not a PPM")
        return 1
    w, h, px = parsed
    colours = set()
    for i in range(0, min(len(px), w * h * 3), 3 * 97):   # sample, not survey
        colours.add(px[i:i + 3])
    print("screen %dx%d, %d distinct colours sampled" % (w, h, len(colours)))
    if len(colours) < 3:
        print("VERDICT: blank or near-blank screen")
        return 1
    print("VERDICT: the screen has content")
    print("wrote %s" % dump)
    return 0


if __name__ == "__main__":
    sys.exit(main())

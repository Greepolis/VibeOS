#!/usr/bin/env python3
"""Boot a disk/CD image under QEMU+OVMF and wait for a serial token.

Reusable smoke for the standalone bootable artifacts (esp.img, .vmdk, .iso):
unlike the fat:rw: directory smoke, this boots an actual image file the way a
VM (VirtualBox/VMware) would, verifying the image itself is bootable.

Usage: qemu-boot-image-linux.py <image> [token] [timeout] [media] [fmt]
  media: disk (default) | cdrom
  fmt:   raw (default) | vmdk | vdi | ...
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def find_ovmf_pair():
    code = next((p for p in [
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/usr/share/ovmf/OVMF_CODE.fd",
    ] if os.path.exists(p)), None)
    vars = next((p for p in [
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
        "/usr/share/OVMF/OVMF_VARS.fd",
        "/usr/share/ovmf/OVMF_VARS.fd",
    ] if os.path.exists(p)), None)
    if not code or not vars:
        raise RuntimeError("OVMF firmware files not found")
    return code, vars


def main():
    if len(sys.argv) < 2:
        print("usage: qemu-boot-image-linux.py <image> [token] [timeout] [media] [fmt]", file=sys.stderr)
        return 2
    image = sys.argv[1]
    token = sys.argv[2] if len(sys.argv) > 2 else "BOOT_OK"
    timeout_sec = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    media = sys.argv[4] if len(sys.argv) > 4 else "disk"
    fmt = sys.argv[5] if len(sys.argv) > 5 else "raw"

    if not os.path.exists(image):
        print(f"[BOOT-IMG] FAIL image not found: {image}", file=sys.stderr)
        return 1
    if shutil.which("qemu-system-x86_64") is None:
        print("[BOOT-IMG] FAIL qemu-system-x86_64 not found", file=sys.stderr)
        return 1

    ovmf_code, ovmf_vars_template = find_ovmf_pair()
    serial_text = ""
    status = "fail"

    with tempfile.TemporaryDirectory(prefix="vibeos-img-boot-") as tmp:
        sock_path = os.path.join(tmp, "serial.sock")
        vars_path = os.path.join(tmp, "OVMF_VARS.fd")
        shutil.copyfile(ovmf_vars_template, vars_path)

        if media == "cdrom":
            drive = ["-drive", f"file={image},if=none,id=cd0,format={fmt},media=cdrom",
                     "-device", "ide-cd,drive=cd0,bootindex=1"]
        else:
            drive = ["-drive", f"file={image},if=none,id=disk0,format={fmt}",
                     "-device", "virtio-blk-pci,drive=disk0,bootindex=1"]

        cmd = [
            "qemu-system-x86_64",
            "-machine", "q35",
            "-m", "512M",
            "-display", "none",
            "-monitor", "none",
            "-chardev", f"socket,id=serial0,path={sock_path},server=on,wait=off",
            "-serial", "chardev:serial0",
            "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
            "-drive", f"if=pflash,format=raw,file={vars_path}",
            *drive,
            "-net", "none", "-no-reboot",
        ]
        err_fp = open(os.path.join(tmp, "qemu-err.log"), "wb")
        qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err_fp)
        try:
            serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            deadline = time.monotonic() + 10
            while True:
                try:
                    serial.connect(sock_path)
                    break
                except OSError:
                    if time.monotonic() >= deadline or qemu.poll() is not None:
                        raise RuntimeError("QEMU serial socket not created")
                    time.sleep(0.05)
            serial.setblocking(False)
            deadline = time.monotonic() + timeout_sec
            while time.monotonic() < deadline:
                try:
                    chunk = serial.recv(4096)
                    if chunk:
                        serial_text += chunk.decode("utf-8", errors="replace")
                except BlockingIOError:
                    pass
                if token in serial_text.replace("\r", ""):
                    status = "pass"
                    break
                time.sleep(0.05)
            serial.close()
        finally:
            if qemu.poll() is None:
                qemu.terminate()
                try:
                    qemu.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    qemu.kill()
            err_fp.close()

    tail = "|".join(serial_text.replace("\r", " ").splitlines()[-12:])
    if status == "pass":
        print(f"[BOOT-IMG] PASS token '{token}' seen booting {os.path.basename(image)} ({media})")
        return 0
    print(f"[BOOT-IMG] FAIL token '{token}' not seen; serial_tail={tail}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

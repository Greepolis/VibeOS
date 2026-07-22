#!/usr/bin/env python3
"""Interactive OVMF smoke test for the VibeOS serial CLI."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def write(path, text):
    with open(path, "w", encoding="utf-8", errors="replace") as fp:
        fp.write(text)


def tail_text(text, lines=40):
    return "|".join(text.replace("\r", " ").splitlines()[-lines:])


def find_ovmf_pair():
    code_candidates = [
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/usr/share/qemu/OVMF_CODE.fd",
        "/usr/share/ovmf/OVMF_CODE.fd",
    ]
    vars_candidates = [
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
        "/usr/share/OVMF/OVMF_VARS.fd",
        "/usr/share/qemu/OVMF_VARS.fd",
        "/usr/share/ovmf/OVMF_VARS.fd",
    ]
    code = next((p for p in code_candidates if os.path.exists(p)), None)
    vars_template = next((p for p in vars_candidates if os.path.exists(p)), None)
    if not code or not vars_template:
        raise RuntimeError("OVMF firmware files not found")
    return code, vars_template


def wait_for(buffer_getter, needle, deadline):
    while time.monotonic() < deadline:
        if needle in buffer_getter().replace("\r", ""):
            return True
        time.sleep(0.05)
    return False


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    timeout_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 90
    efi_root = os.path.join(build_dir, "artifacts", "efi_root")
    bootloader = os.path.join(efi_root, "EFI", "BOOT", "BOOTX64.EFI")
    kernel = os.path.join(efi_root, "EFI", "BOOT", "VIBEOSKR.ELF")
    serial_log_path = "qemu-cli-serial.log"
    err_log_path = "qemu-cli-err.log"
    summary_path = "qemu-cli-summary.txt"

    serial_text = ""
    err_text = ""
    qemu = None
    status = "fail"
    reason = "unknown"

    try:
        if not os.path.exists(bootloader) or not os.path.exists(kernel):
            raise RuntimeError("EFI bootloader or kernel payload missing")
        if shutil.which("qemu-system-x86_64") is None:
            raise RuntimeError("qemu-system-x86_64 not found")

        ovmf_code, ovmf_vars_template = find_ovmf_pair()
        with tempfile.TemporaryDirectory(prefix="vibeos-cli-smoke-") as tmp:
            sock_path = os.path.join(tmp, "serial.sock")
            vars_path = os.path.join(tmp, "OVMF_VARS.fd")
            shutil.copyfile(ovmf_vars_template, vars_path)
            err_fp = open(err_log_path, "wb")
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
                "-drive", f"if=none,id=esp,format=raw,file=fat:rw:{efi_root}",
                "-device", "virtio-blk-pci,drive=esp,bootindex=1",
                "-net", "none",
                "-no-reboot",
                "-no-shutdown",
            ]
            qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err_fp)

            serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connect_deadline = time.monotonic() + 10
            while True:
                try:
                    serial.connect(sock_path)
                    break
                except OSError:
                    if time.monotonic() >= connect_deadline:
                        raise RuntimeError("serial socket was not created by QEMU")
                    if qemu.poll() is not None:
                        raise RuntimeError(f"QEMU exited before serial connection rc={qemu.returncode}")
                    time.sleep(0.05)

            serial.setblocking(False)
            deadline = time.monotonic() + timeout_sec

            def pump():
                nonlocal serial_text
                while True:
                    try:
                        chunk = serial.recv(4096)
                    except BlockingIOError:
                        return
                    if not chunk:
                        return
                    serial_text += chunk.decode("utf-8", errors="replace")

            def buffer():
                pump()
                return serial_text

            checks = [
                ("BOOT_OK", None),
                ("CLI_READY", None),
                ("vibeos> ", b"help\r"),
                ("Commands: help, status, log, echo <text>, halt, reboot", b"status\r"),
                ("stage=core_ready", b"echo vibeos\r"),
                ("\nvibeos\nvibeos> ", b"halt\r"),
                ("Halt requested", None),
            ]

            for expected, command in checks:
                if not wait_for(buffer, expected, deadline):
                    reason = f"missing:{expected}"
                    raise RuntimeError(reason)
                if command is not None:
                    serial.sendall(command)

            status = "pass"
            reason = "cli_commands_verified"
            serial.close()
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
                qemu.wait(timeout=5)
            err_fp.close()

    except Exception as exc:
        reason = str(exc)
        if qemu is not None and qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
        status = "fail"
    finally:
        if os.path.exists(err_log_path):
            with open(err_log_path, "rb") as fp:
                err_text = fp.read().decode("utf-8", errors="replace")
        write(serial_log_path, serial_text)
        write(summary_path, "\n".join([
            f"status={status}",
            f"reason={reason}",
            f"build_dir={build_dir}",
            f"efi_root={efi_root}",
            f"serial_log={serial_log_path}",
            f"error_log={err_log_path}",
            f"serial_tail={tail_text(serial_text)}",
            f"error_tail={tail_text(err_text)}",
            "",
        ]))

    if status == "pass":
        print("[QEMU-CLI] PASS boot-to-cli verified")
        return 0
    print(f"[QEMU-CLI] FAIL {reason}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Interactive OVMF smoke test for the VibeOS serial CLI."""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

# The guest's NET.ELF connects here through QEMU's user-mode network, which
# presents the host as 10.0.2.2. Serving the echo from the harness is what makes
# the TCP path a real end-to-end test rather than a loopback inside the guest.
ECHO_PORT = 7777

# The guest is booted with this many vCPUs; every one of them must come online.
EXPECTED_CPUS = 4


def start_echo_server(stop_event, state):
    """Accept one connection at a time and echo whatever arrives."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("127.0.0.1", ECHO_PORT))
    except OSError as exc:
        # Someone else holds the port - usually another smoke run still going.
        # Say so as an infrastructure problem: it is not a guest failure, and
        # it must not read like one.
        srv.close()
        raise RuntimeError(
            f"INFRA: echo port {ECHO_PORT} unavailable ({exc}); "
            "another smoke run is probably still active") from exc
    srv.listen(4)
    srv.settimeout(0.5)

    def serve():
        while not stop_event.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            state["connections"] += 1
            conn.settimeout(5)
            try:
                data = conn.recv(4096)
                if data:
                    state["received"] += len(data)
                    conn.sendall(b"ECHO:" + data)
            except OSError:
                pass
            finally:
                try:
                    conn.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                conn.close()
        srv.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return t


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
    echo_stop = threading.Event()
    echo_state = {"connections": 0, "received": 0}
    echo_thread = None
    infra = False

    try:
        echo_thread = start_echo_server(echo_stop, echo_state)
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
                # Four cores: the kernel is SMP, so the smoke must exercise it.
                "-smp", "4",
                "-display", "none",
                "-monitor", "none",
                "-chardev", f"socket,id=serial0,path={sock_path},server=on,wait=off",
                "-serial", "chardev:serial0",
                "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
                "-drive", f"if=pflash,format=raw,file={vars_path}",
                "-drive", f"if=none,id=esp,format=raw,file=fat:rw:{efi_root}",
                "-device", "virtio-blk-pci,drive=esp,bootindex=1",
                # A real NIC on QEMU's user-mode network: DHCP and DNS come
                # from the built-in services, and 10.0.2.2 is the host.
                "-netdev", "user,id=n0",
                "-device", "virtio-net-pci,netdev=n0",
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
            started = time.monotonic()
            last_rx = started
            deadline = started + timeout_sec

            def pump():
                nonlocal serial_text, last_rx
                while True:
                    try:
                        chunk = serial.recv(4096)
                    except BlockingIOError:
                        return
                    if not chunk:
                        return
                    serial_text += chunk.decode("utf-8", errors="replace")
                    # When output last arrived is what separates "the guest is
                    # wedged" from "the guest is just slow": a failure with the
                    # serial line still active is a budget problem, one that has
                    # been silent for a long time is a hang.
                    last_rx = time.monotonic()

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
                    # Say which kind of failure this is, so nobody has to run a
                    # experiment to find out whether the guest froze or the
                    # budget was simply too small on a loaded machine.
                    now = time.monotonic()
                    idle = now - last_rx
                    if not serial_text:
                        kind = "no_output_at_all"      # never got past firmware
                    elif idle > 20:
                        kind = "guest_went_quiet"      # produced output, then stopped: a hang
                    else:
                        kind = "guest_still_talking"   # still progressing: the budget was too small
                    reason = (f"missing:{expected} verdict={kind}"
                              f" elapsed={now - started:.0f}s"
                              f" quiet_for={idle:.0f}s budget={timeout_sec}s")
                    raise RuntimeError(reason)
                if command is not None:
                    serial.sendall(command)

            # Beyond "did it print the marker": assert the system actually
            # ended up in a sane state. A lease of 0.0.0.0 once passed this
            # gate for weeks because only the marker was checked, and the
            # guest kept limping along with a broken address.
            text = buffer().replace("\r", "")
            problems = []

            for marker in ("NET_OK", "TCP_OK"):
                if marker not in text:
                    problems.append("missing:" + marker)

            lease = re.search(r"NET_OK dhcp lease ip=(\d+\.\d+\.\d+\.\d+)", text)
            if lease is None:
                problems.append("no_dhcp_lease_reported")
            elif lease.group(1) == "0.0.0.0":
                problems.append("dhcp_lease_is_zero")

            # Match the full 16-digit field. An earlier version stripped leading
            # zeros with 0x0* and then required one more digit, which made the
            # regex backtrack and report 0 for a line that was merely cut short
            # - a harness bug that reads exactly like a kernel bug.
            cpus = re.search(r"SMP_OK: cpus online=0x([0-9a-f]{16})", text)
            if cpus is None:
                truncated = "SMP_OK: cpus online=" in text
                problems.append("smp_report_truncated" if truncated else "no_smp_report")
            elif int(cpus.group(1), 16) != EXPECTED_CPUS:
                problems.append(f"cpus_online={int(cpus.group(1), 16)}_expected={EXPECTED_CPUS}")

            # A recovered fault still means something went wrong that the boot
            # was not supposed to hit. The int3 self-test is the one exception.
            faults = [line for line in text.splitlines()
                      if "[HW][TRAP]" in line and "vector=0x0000000000000003" not in line]
            if faults:
                problems.append("unexpected_cpu_fault")
            if "FATAL" in text or "PANIC" in text:
                problems.append("panic")

            if echo_state["connections"] == 0:
                problems.append("no_tcp_connection_reached_the_host")

            if problems:
                reason = "invariant_failed:" + ",".join(problems)
                raise RuntimeError(reason)

            status = "pass"
            reason = (f"cli_and_network_verified"
                      f" tcp_connections={echo_state['connections']}"
                      f" bytes={echo_state['received']}")
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
        infra = reason.startswith("INFRA:")
        if qemu is not None and qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
        status = "infra_error" if infra else "fail"
    finally:
        echo_stop.set()
        if echo_thread is not None:
            echo_thread.join(timeout=2)
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

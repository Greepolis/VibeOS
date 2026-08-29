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
# The guest always dials 10.0.2.2:7777 - that address is compiled into NET.ELF
# and is not negotiable from here. What *is* negotiable is where QEMU forwards
# it on the host, which is what lets several boots run at once instead of one
# after another. Set VIBEOS_SMOKE_ID to a small integer to move this run's host
# port and its log files out of another run's way.
#
# Serial boots were costing about ninety seconds each, and the honest way to
# see through a failure that reproduces one time in four is to run a lot of
# them. In series that is the afternoon; in parallel it is a coffee.
# The guest dials 10.0.2.2:7777, and slirp will not let that be redirected:
# 10.0.2.2 is its gateway, and guestfwd refuses the gateway address. So a
# parallel run cannot have an echo server of its own, and runs with a non-zero
# id do without one - they check everything except the TCP round trip.
#
# That is the right trade for the thing parallelism is for. Hunting a failure
# that appears one boot in four means running a lot of boots, and the question
# being asked of them is "did it reach the shell", not "did the network work".
# The full gate, echo server included, is still what run 0 and CI do.
SMOKE_ID = int(os.environ.get("VIBEOS_SMOKE_ID", "0"))
ECHO_PORT = 7777
SKIP_ECHO = SMOKE_ID != 0

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


# How long a guest may stay silent, once it has produced output at all, before
# it is treated as wedged. Firmware goes quiet for long stretches with several
# TCG cores, so this is generous - but a boot that reaches the kernel and then
# says nothing for two minutes has stopped, and waiting out the rest of the
# budget only delays a verdict that is already decided.
# Long enough that an unoptimised build under emulation is not mistaken for a
# wedged one. A Debug kernel loading a two-megabyte program on four TCG cores
# can be silent for well over two minutes while doing real work, and cutting it
# off there reported a hang that was not one. Still far below the budget, so a
# genuine hang is caught in minutes instead of at the end.
# How long silence is allowed depends entirely on where the guest is, which is
# why one number was always going to be either too slow or wrong. Before the
# kernel says BOOT_OK the machine is firmware and a bootloader dragging a
# two-megabyte kernel off a FAT volume through TCG, and it is legitimately mute
# for minutes. After BOOT_OK it narrates constantly - every task exit, every
# exec, every line the shell echoes - and the longest honest gap in a healthy
# Release boot is the BusyBox exec, which is seconds.
#
# So a wedged boot used to cost the whole 300-second budget, and a run of six
# spent most of its time waiting for verdicts that were already decided.
# Measured, not guessed: a healthy boot reaches the interactive shell in about
# twelve seconds, and its whole kernel phase is under ten. Waiting seventy-five
# seconds for silence to mean something was therefore spending six times the
# entire boot to confirm a verdict, and it was the single largest cost in a run
# of several boots - far larger than the boots themselves.
#
# 45 rather than 30: six boots at each gave two passes and three, which is
# noise at this failure rate and therefore not evidence that the tighter number
# is safe. Four times the whole healthy boot is a margin that does not need
# defending, and it is still six times faster than waiting out the budget.
QUIET_GIVE_UP_SEC = 300           # before the kernel is up
QUIET_GIVE_UP_KERNEL_SEC = int(os.environ.get("VIBEOS_QUIET_KERNEL_SEC", "45"))

# A Debug kernel on four emulated cores is several times slower than a Release
# one, and cutting it off mid-exec reports a hang that is not one - that
# mistake is why this number was raised to five minutes in the first place. The
# build directory names the configuration, so the tighter number is not applied
# to a build that cannot meet it.
if any("debug" in a.lower() for a in sys.argv[1:]):
    QUIET_GIVE_UP_KERNEL_SEC = max(QUIET_GIVE_UP_KERNEL_SEC, 120)

_KERNEL_PHASES_START = "kernel_boot"


def quiet_budget(text):
    """Seconds of silence to tolerate, given how far the guest has got."""
    phase = detect_guest_phase(text)
    if phase == "boot" or phase.startswith("bootloader"):
        return QUIET_GIVE_UP_SEC
    return QUIET_GIVE_UP_KERNEL_SEC


def detect_guest_phase(text):
    """Return the furthest observable phase in the serial stream."""
    phase = "boot"
    for line in text.replace("\r", "").splitlines():
        if "BL_EFI_OK" in line:
            phase = "bootloader_efi"
        elif "BL_FS_OK" in line:
            phase = "bootloader_filesystem"
        elif "BL_PLAN_OK" in line:
            phase = "bootloader_plan"
        elif "BL_LOAD_OK" in line:
            phase = "bootloader_load"
        elif "BL_EBS_OK" in line:
            phase = "bootloader_exit_boot_services"
        elif "BOOT_OK" in line:
            phase = "kernel_boot"
        elif "CLI_READY" in line:
            phase = "kernel_cli_ready"
        elif "BUSYBOX.ELF echo" in line:
            phase = "busybox_echo_started"
        elif "BUSYBOX_ECHO_OK" in line:
            phase = "busybox_echo"
        elif "BUSYBOX.ELF cat" in line:
            phase = "busybox_cat_started"
        elif "persistent hello" in line:
            phase = "busybox_cat"
        elif "BUSYBOX.ELF ls" in line:
            phase = "busybox_ls_started"
        elif phase == "busybox_ls_started" and "[SCHED] task pid=" in line:
            phase = "busybox_ls"
        elif "BUSYBOX.ELF sh -c" in line:
            phase = "busybox_shell_started"
        elif "BUSYBOX_SH_OK" in line:
            phase = "busybox_shell"
        elif "ASH_INTERACTIVE_OK" in line:
            phase = "busybox_interactive_shell"
        elif "PIPE_OK" in line:
            phase = "busybox_pipeline"
        elif "VIBEOS_SELFTEST_DONE" in line:
            phase = "selftest_done"
    return phase


def wait_for(buffer_getter, needle, deadline, last_rx_getter=None):
    while time.monotonic() < deadline:
        text = buffer_getter().replace("\r", "")
        if needle in text:
            return True
        if last_rx_getter is not None and text:
            if time.monotonic() - last_rx_getter() > quiet_budget(buffer_getter()):
                return False   # wedged; the caller classifies it the same way
        time.sleep(0.05)
    return False


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    timeout_sec = int(sys.argv[2]) if len(sys.argv) > 2 else 90
    efi_root = os.path.join(build_dir, "artifacts", "efi_root")
    bootloader = os.path.join(efi_root, "EFI", "BOOT", "BOOTX64.EFI")
    kernel = os.path.join(efi_root, "EFI", "BOOT", "VIBEOSKR.ELF")
    # Run 0 keeps the historical names, so every existing script and habit that
    # greps qemu-cli-serial.log still works.
    suffix = "" if SMOKE_ID == 0 else f"-{SMOKE_ID}"
    serial_log_path = f"qemu-cli-serial{suffix}.log"
    err_log_path = f"qemu-cli-err{suffix}.log"
    summary_path = f"qemu-cli-summary{suffix}.txt"
    # Not beside the other artefacts: on a Windows-mounted working directory a
    # unix socket cannot be created at all, and the failure is QEMU refusing to
    # start rather than anything to do with the guest.
    monitor_path = os.path.join(tempfile.gettempdir(),
                                f"vibeos-monitor{suffix}.sock")
    try:
        os.unlink(monitor_path)
    except OSError:
        pass

    serial_text = ""
    serial_extra = []
    err_text = ""
    qemu = None
    status = "fail"
    reason = "unknown"
    echo_stop = threading.Event()
    echo_state = {"connections": 0, "received": 0}
    echo_thread = None
    infra = False
    last_guest_phase = "boot"
    last_serial_timestamp = 0.0
    phase_history = []
    last_expected = "startup"
    verbose = os.environ.get("VIBEOS_QEMU_VERBOSE", "") == "1"

    try:
        echo_thread = None if SKIP_ECHO else start_echo_server(echo_stop,
                                                                echo_state)
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
                # Software emulation, always - even where /dev/kvm exists.
                #
                # This is not a preference, it is the whole point. A four-core
                # TCG guest interleaves differently from KVM, and an SMP race
                # that KVM hides reproduces under TCG in roughly one boot in
                # two. Local runs on a machine with KVM were passing this smoke
                # twenty times in a row while CI failed, because they were not
                # running the same thing. Verification has to be at least as
                # harsh as the gate it is meant to predict.
                "-accel", "tcg",
                "-machine", "q35",
                "-m", "512M",
                # Four cores: the kernel is SMP, so the smoke must exercise it.
                "-smp", "4",
                "-display", "none",
                # A monitor rather than none: a wedged guest cannot
                # report on itself, and this is the only way to ask
                # where its cores are once it has stopped.
                "-monitor", "unix:" + monitor_path + ",server,nowait",
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
                nonlocal serial_text, last_rx, last_guest_phase, last_serial_timestamp
                while True:
                    try:
                        chunk = serial.recv(4096)
                    except BlockingIOError:
                        return
                    if not chunk:
                        return
                    serial_text += chunk.decode("utf-8", errors="replace")
                    last_serial_timestamp = time.monotonic() - started
                    new_phase = detect_guest_phase(serial_text)
                    if new_phase != last_guest_phase:
                        last_guest_phase = new_phase
                        phase_history.append(f"{last_serial_timestamp:.3f}:{new_phase}")
                        print(f"[QEMU-CLI] phase={new_phase} elapsed={last_serial_timestamp:.1f}s")
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
                # Wait for the user-space self-test to finish before driving
                # the kernel CLI. The two run concurrently, and on a slower
                # build the halt below arrived while the script was still
                # working - failing the invariants for a reason that had
                # nothing to do with the kernel.
                ("VIBEOS_SELFTEST_DONE", None),
                ("vibeos> ", b"help\r"),
                ("Commands: help, status, log, echo <text>, halt, reboot", b"status\r"),
                ("stage=core_ready", b"echo vibeos\r"),
                ("\nvibeos\nvibeos> ", b"halt\r"),
                ("Halt requested", None),
            ]

            for expected, command in checks:
                last_expected = expected
                if not wait_for(buffer, expected, deadline, lambda: last_rx):
                    # Say which kind of failure this is, so nobody has to run a
                    # experiment to find out whether the guest froze or the
                    # budget was simply too small on a loaded machine.
                    now = time.monotonic()
                    idle = now - last_rx
                    if not serial_text:
                        kind = "no_output_at_all"      # never got past firmware
                    elif idle >= quiet_budget(serial_text):
                        kind = "guest_wedged"          # gave up early on purpose
                    elif idle > 20:
                        # Silence is not proof of a hang: firmware is quiet for
                        # long stretches, especially with several TCG cores. The
                        # tail printed below is what settles it.
                        kind = "guest_quiet"
                    else:
                        kind = "guest_still_talking"   # still progressing: the budget was too small
                    reason = (f"missing:{expected} verdict={kind}"
                              f" elapsed={now - started:.0f}s"
                              f" quiet_for={idle:.0f}s"
                              f" quiet_budget={quiet_budget(serial_text)}s"
                              f" phase={detect_guest_phase(serial_text)}"
                              f" budget={timeout_sec}s")
                    # A wedged guest is exactly the case where none of the
                    # in-guest logging runs: nothing panicked, so no backtrace
                    # and no log dump. Ask the emulator instead, while the
                    # machine is still up - after the kill there is nothing to
                    # ask.
                    if kind in ("guest_wedged", "guest_quiet"):
                        try:
                            sys.path.insert(0, os.path.join(
                                os.path.dirname(os.path.abspath(__file__)),
                                "dev"))
                            import wedge_report
                            for wl in wedge_report.report(monitor_path, kernel):
                                print("[QEMU-CLI] " + wl)
                                serial_extra.append(wl)
                        except Exception as exc:      # never mask the verdict
                            print(f"[QEMU-CLI] wedge report failed: {exc}")

                    # Put the tail in the job output, not only in an artifact
                    # nobody downloads. Whether the guest stalled in firmware
                    # or inside the kernel is visible from these lines alone.
                    tail_lines = 80 if verbose else 40
                    tail = serial_text.replace(chr(13), "").splitlines()[-tail_lines:]
                    print(f"[QEMU-CLI] last {len(tail)} serial lines before the failure:")
                    for line in tail:
                        print(f"[QEMU-CLI]   {line}")
                    try:
                        err_fp.flush()
                        with open(err_log_path, "rb") as fp:
                            err_text = fp.read().decode("utf-8", errors="replace")
                    except (OSError, UnboundLocalError):
                        err_text = ""
                    if err_text:
                        print(f"[QEMU-CLI] qemu stderr tail:")
                        for line in err_text.replace(chr(13), "").splitlines()[-40:]:
                            print(f"[QEMU-CLI]   {line}")
                    if not tail:
                        print("[QEMU-CLI]   (the guest never wrote to the serial port)")
                    raise RuntimeError(reason)
                if command is not None:
                    serial.sendall(command)

            # Beyond "did it print the marker": assert the system actually
            # ended up in a sane state. A lease of 0.0.0.0 once passed this
            # gate for weeks because only the marker was checked, and the
            # guest kept limping along with a broken address.
            text = buffer().replace("\r", "")
            problems = []

            # TCP_OK only appears when something answered on the host, so a
            # parallel run without its own echo server must not require it.
            # DHCP still must work: that comes from QEMU's own services and is
            # a real assertion about the guest either way.
            markers = ("NET_OK",) if SKIP_ECHO else ("NET_OK", "TCP_OK")
            for marker in markers:
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

            if echo_state["connections"] == 0 and not SKIP_ECHO:
                problems.append("no_tcp_connection_reached_the_host")

            # If the build staged an unmodified static Linux binary, it has to
            # have run. This is checked only when the file exists because
            # musl-gcc is not available on every machine - but where it is, a
            # regression that stops a real Linux program from starting must
            # fail the boot rather than be noticed later by someone reading
            # the log.
            # BusyBox is a real program doing real work rather than a test
            # written to pass: it dispatches on its own name, reads a file
            # through openat/fstat/read, and lists a directory through
            # getdents64. Checked only when the host had a static BusyBox to
            # stage.
            busybox = os.path.join(efi_root, "EFI", "BOOT", "BUSYBOX.ELF")
            if os.path.exists(busybox):
                if "BUSYBOX_ECHO_OK" not in text:
                    problems.append("busybox_did_not_run")
                if "applet not found" in text:
                    problems.append("busybox_applet_dispatch_failed")
                # Starting is not working. These are BusyBox's own error
                # messages when a file operation fails, so they catch a
                # regression in openat, fstat, read or getdents64 that would
                # otherwise leave the boot looking green.
                if "cat: can't open" in text or "ls: can't open" in text:
                    problems.append("busybox_file_operations_failed")
                # The console is handed to BusyBox's shell, which then reads
                # typed lines itself. This checks the whole chain: an
                # interactive shell running a builtin, then finding an external
                # command through PATH and exec'ing it.
                if "ASH_INTERACTIVE_OK" not in text:
                    problems.append("interactive_shell_did_not_run")
                # A pipeline is the first thing anyone tries on a shell, and
                # the way it fails is by hanging rather than by erroring - so
                # PIPE_OK arriving at all is the check. It is printed after
                # the pipeline, so it cannot appear if the pipeline stalled.
                if "PIPE_OK" not in text:
                    problems.append("pipeline_did_not_complete")
                if "BUSYBOX_SH_OK" not in text:
                    problems.append("shell_script_did_not_run")

            signal_elf = os.path.join(efi_root, "EFI", "BOOT", "SIGNAL.ELF")
            if os.path.exists(signal_elf):
                if "SIG_OK" not in text:
                    problems.append("signal_delivery_broken")

            # The graphical shell, to the extent a serial log can speak for
            # it: the console has to have reached the on-screen terminal. Only
            # checked when a desktop came up at all, since a build without a
            # framebuffer is not a failing one.
            gui = re.search(r"GUI_STATS frames=0x([0-9a-f]+) termchars=0x([0-9a-f]+)", text)
            if "[GUI] desktop up" in text:
                if gui is None:
                    problems.append("gui_reported_nothing")
                elif int(gui.group(2), 16) == 0:
                    problems.append("gui_terminal_empty")

            # A position-independent executable is placed by the loader, not
            # by the file. Checking argv as well as the greeting is what
            # separates "it ran" from "it ran and could still see the stack
            # the loader built" - a bias applied to the image but not to
            # AT_PHDR produces the first without the second.
            pie = os.path.join(efi_root, "EFI", "BOOT", "PIE.ELF")
            if os.path.exists(pie):
                if "PIE_OK" not in text:
                    problems.append("position_independent_binary_did_not_run")
                elif "PIE_ARGS: argc=1" not in text:
                    problems.append("position_independent_binary_got_wrong_argv")

            # A dynamic executable is started by its interpreter, not by the
            # kernel. Reaching main() at all means the second image was mapped,
            # AT_BASE told the loader where it landed, and AT_ENTRY still
            # pointed at the program. argv0len comes back through the C
            # library's strlen, so a symbol had to be resolved rather than
            # folded away by the compiler.
            # Threads. The counter is asserted as well as the greeting:
            # "the threads ran" and "the threads ran, shared one address
            # space and took a contended lock without losing an increment"
            # are different claims, and a kernel can produce the first while
            # failing the second. tls=ok says each thread saw its own
            # thread-local, which is what separates four threads from one
            # thread run four times.
            thr = os.path.join(efi_root, "EFI", "BOOT", "THREADS.ELF")
            if os.path.exists(thr):
                if "THREADS_STAGE1_OK" not in text:
                    problems.append("thread_create_or_join_failed")
                elif "counter=8000 expected=8000" not in text:
                    problems.append("threads_lost_an_increment")
                elif "tls=ok" not in text:
                    problems.append("thread_local_storage_shared")

            # PID 1 is a native ring-3 init, and it is the parent of the
            # bring-up workload rather than being it. Asserted because the
            # difference is invisible from everything else in this log: the
            # same programs run either way, and only this line says a process
            # existed whose job was to outlive them.
            if "NATIVE_INIT_READY" not in text:
                problems.append("native_init_did_not_run")
            elif "NATIVE_INIT_CHILD_PID=" not in text:
                # An init that exec'd the workload in place would print the
                # line above and nothing here, and the rest of the boot would
                # look the same - so this is the assertion that says init is a
                # parent rather than the workload wearing its name.
                problems.append("native_init_did_not_fork_a_child")

            # The ring-3 ABI round trip. This was printing "abi: ...wrong"
            # for a whole session while the gate stayed green, because the
            # line was collected into the summary and never asserted on - so
            # a regression in mmap/mprotect/munmap was visible in the log and
            # invisible to CI. A check that is only ever read by a human is
            # not a check.
            if "linux abi ok" not in text:
                problems.append("linux_abi_selftest_failed")

            dyn = os.path.join(efi_root, "EFI", "BOOT", "DYN.ELF")
            if os.path.exists(dyn):
                if "DYN_OK" not in text:
                    problems.append("dynamic_binary_did_not_run")
                elif "DYN_ARGS: argc=1" not in text:
                    problems.append("dynamic_binary_got_wrong_argv")

            musl = os.path.join(efi_root, "EFI", "BOOT", "MUSL.ELF")
            if os.path.exists(musl):
                if "MUSL_OK" not in text:
                    problems.append("unmodified_linux_binary_did_not_run")
                elif "MUSL_ARGS: argc=1" not in text:
                    problems.append("linux_binary_got_wrong_argv")

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
            f"failure_class={'infra_error' if infra else ('pass' if status == 'pass' else 'guest_failure')}",
            f"last_guest_phase={last_guest_phase}",
            f"last_expected={last_expected}",
            f"last_serial_timestamp={last_serial_timestamp:.3f}",
            f"phase_history={'|'.join(phase_history)}",
            "wedge_report=" + " || ".join(serial_extra),
            f"qemu_exit_code={qemu.returncode if qemu is not None else 'unknown'}",
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

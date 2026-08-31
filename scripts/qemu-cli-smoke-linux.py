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


# The disk controller under test. "virtio" is QEMU's own and is what the
# default run uses; "ahci" is the standard SATA controller every desktop
# hypervisor provides, and is how the AHCI driver gets exercised at all.
DISK = os.environ.get("VIBEOS_SMOKE_DISK", "virtio")
DISK_ARGS = {
    "virtio": ["-device", "virtio-blk-pci,drive=esp,bootindex=1"],
    "ahci": ["-device", "ich9-ahci,id=ahci",
             "-device", "ide-hd,drive=esp,bus=ahci.0,bootindex=1"],
}
if DISK not in DISK_ARGS:
    raise SystemExit(f"VIBEOS_SMOKE_DISK={DISK!r}: expected one of "
                     + ", ".join(sorted(DISK_ARGS)))


# Kernel log lines that a healthy boot is *expected* to produce, because
# something deliberately provokes them. Everything else at WARN or above is a
# failure - the kernel said something was wrong and, until now, nobody was
# listening.
#
# This is the same lesson as the ring-3 self-test that printed "abi: ...wrong"
# for a whole session while the gate stayed green: a diagnostic nobody asserts
# on is decoration. The kernel has had levelled logging for a long time and its
# WARN and ERROR lines have never once failed a build.
#
# Keep this list short and specific. A pattern added to quieten a real warning
# is how the mechanism stops working.
EXPECTED_KERNEL_COMPLAINTS = (
    # The ring-3 ABI self-test asks for things it expects to be refused.
    "mmap refused: MAP_FIXED",
    "mprotect refused: page not mapped",
    # svc-crash dereferences null on purpose, every boot.
    "ring-3 fault: killing the task",
)


def unexpected_complaints(text):
    """WARN and above from the kernel that nothing in the boot asked for."""
    out = []
    for line in text.splitlines():
        if "[LOG][WARN]" not in line and "[LOG][ERROR]" not in line and \
           "[LOG][FATAL]" not in line:
            continue
        if any(expected in line for expected in EXPECTED_KERNEL_COMPLAINTS):
            continue
        out.append(line)
    return out


assert unexpected_complaints("[LOG][WARN] mmap refused: MAP_FIXED code=0xa") == []
assert unexpected_complaints("[LOG][ERROR] something nobody expected") != []
assert unexpected_complaints("[HW][SYS] write(ring3): all fine") == []


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


# Tags the kernel puts at the start of a line. Two of them in one line means
# one line was written into the middle of another, which only happens when the
# console lock was not doing its job.
#
# This check exists because the harness was, for a session, capable of lying.
# A missing serial_lock() let one core free the lock out from under another
# mid-line; the markers this script matches on were cut in half, and the gate
# reported failures that had never happened. Two crashes were investigated in
# detail before the split was noticed. A test that can report a fault that did
# not occur is worse than no test, so the log's integrity is now checked before
# anything is concluded from its contents.
KERNEL_TAGS = ("[HW][TRAP]", "[HW][SYS]", "[CRASH]", "[SIG]", "[MM]", "[SCHED]",
               "[EXEC]", "[LOG][", "[BT]", "[BLK]", "[AHCI]", "[VIRTIO]",
               "[NET]", "[SMP]", "[CLI]", "[BOOT]", "[FAT]", "[COMPAT]")


def interleaved_lines(text):
    """Lines that show one kernel write cut into another.

    Two kernel tags on one physical line is the symptom, but on its own it is
    not proof: a ring-3 program may write without a trailing newline - a shell
    echoing a prompt does exactly that - and the next kernel line then
    legitimately continues the same physical line. Flagging those was the first
    version of this check and it cried wolf on its first run.

    So a line is reported when either

      - a hex field is cut short: "0x" with no hex digits after it and a tag
        following. The kernel always prints its values in full, so this can
        only mean another writer arrived mid-number. That is what caught the
        unbracketed [EXEC] messages.
      - a second tag follows a *kernel* tag. A kernel line ends with its own
        newline, so nothing of anyone else's belongs on it. A line opened by a
        ring-3 write is exempt, because that one really can be left open.

    Known limit, written down rather than papered over: a split that lands
    inside a ring-3 write and truncates no number is not caught here. The
    console lock's bad-unlock counter is what guards that case.
    """
    bad = []
    for line in text.splitlines():
        hits = sorted((line.index(tag), tag) for tag in KERNEL_TAGS if tag in line)

        truncated = False
        pos = line.find("0x")
        while pos >= 0:
            i = pos + 2
            while i < len(line) and line[i] in "0123456789abcdef":
                i += 1
            if i == pos + 2 and i < len(line) and line[i] == "[":
                truncated = True
                break
            pos = line.find("0x", pos + 2)

        if truncated:
            bad.append(line)
        elif len(hits) > 1 and not hits[0][1].startswith("[HW][SYS]"):
            bad.append(line)
    return bad


# Verified against real examples of each kind rather than trusted. The first
# two are taken verbatim from boots and were real defects: an [EXEC] message
# assembled without bracketing, cut mid-number and cut by another tag.
_SPLIT_HEX = "[EXEC] EFI/BOOT/SVC_OK.ELF bytes=0x[HW][SYS] write(ring3): argv ok"
_SPLIT_TAGS = ("[EXEC] EFI/BOOT/SVC_FLAP.ELF bytes=0x0000000000001260"
               "[LOG][WARN] mprotect refused: page not mapped")
# And one that is not a defect: a shell prompt written with no trailing
# newline, which leaves the physical line open for whatever comes next.
_OPEN_LINE = ("[HW][SYS] write(ring3): $ write TMP.TXT scratch"
              "[SCHED] task pid=0x000000000000002e exited code=0x0000000000000001")
_CLEAN = "[HW][SYS] write(ring3): SVC_START selftest 6"

assert interleaved_lines(_SPLIT_HEX) == [_SPLIT_HEX], "misses a truncated hex field"
assert interleaved_lines(_SPLIT_TAGS) == [_SPLIT_TAGS], "misses a kernel line cut by another"
assert interleaved_lines(_OPEN_LINE) == [], "flags a ring-3 write left open"
assert interleaved_lines(_CLEAN) == [], "flags a healthy line"


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
        # The kernel is alive from here. This matters more than it looks: the
        # arch layer runs the *entire* userland before vibeos_kmain is ever
        # called, so BOOT_OK is printed after every user task has retired -
        # it is an "everything finished" marker, not a boot one. Without the
        # two phases below, any hang anywhere in userland was reported with
        # the phase still stuck at the last bootloader marker, and a whole
        # session was spent looking for a bootloader bug that did not exist.
        elif "early init: loading GDT" in line:
            phase = "kernel_early_init"
        elif "USERLAND_START" in line:
            phase = "userland_running"
        elif "USERLAND_DONE" in line:
            phase = "userland_finished"
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
                # A real NIC on QEMU's user-mode network: DHCP and DNS come
                # from the built-in services, and 10.0.2.2 is the host.
                "-netdev", "user,id=n0",
                "-device", "virtio-net-pci,netdev=n0",
                "-no-reboot",
                "-no-shutdown",
            ]
            # Which controller the disk hangs off. virtio-blk is QEMU's and is
            # the default here; AHCI is what VirtualBox, VMware and real
            # machines have, and until it was exercised the appliances booted
            # and then could not read their own disk. Both paths have to be run
            # somewhere, or the one nobody runs is the one that ships.
            cmd += DISK_ARGS[DISK]
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
                # `crash` prints the last ring-3 fault in full. svc-crash
                # dereferences null earlier in every boot, so there is always
                # one to print - and a dumper that only works when nothing has
                # crashed would pass a test that never asked it for anything.
                ("Commands: help, status, log, meminfo, tasks, crash, echo <text>, halt, reboot", b"crash\r"),
                ("[CRASH] end", b"log\r"),
                # `log` shows two rings: the boot stages kmain records, and
                # the arch ring holding what the machine actually did - fork,
                # exec, exit, signals, copy-on-write, munmap. The second was
                # not reachable from the console at all until now, and the
                # gate never ran this command, so neither was exercised.
                ("[LOG] arch ring: showing", b"meminfo\r"),
                # meminfo is the memory picture a person asks for, and the three
                # counters that must be zero are printed on one line so the gate
                # can assert them without parsing the rest.
                ("[MEM] end", b"tasks\r"),
                # The task table, for the same reason meminfo is driven: a
                # subsystem that cannot be looked at on a running machine gets
                # debugged by adding print statements to an eight-thousand-line
                # file, which is how every task defect here has been found.
                ("[TASKS] end", b"status\r"),
                # Ctrl-C on the serial console. The PS/2 path has turned it
                # into a signal since it was written; this one dropped it along
                # with every other control byte, so the foreground process
                # group - and every syscall built on it - was unreachable from
                # the console this system is actually driven through. The
                # machinery existed and no input could get to it, which is why
                # nothing noticed. Asserted because the fix is one comparison.
                ("stage=core_ready", b"\x03"),
                ("^C", b"echo vibeos\r"),
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

            # First, before anything is concluded from this text: is it
            # trustworthy? Every other assertion below reads these lines,
            # so a split one can invent a failure or hide a real one.
            split = interleaved_lines(text)
            if split:
                problems.append(f"serial_log_interleaved({len(split)}_lines)")
                for line in split[:3]:
                    print(f"[QEMU-CLI] split line: {line[:160]}")

            # The kernel's own complaints. It has been raising these all along
            # and nothing has ever failed a boot for one.
            complaints = unexpected_complaints(text)
            if complaints:
                problems.append(f"kernel_reported_problems({len(complaints)})")
                for line in complaints[:5]:
                    print(f"[QEMU-CLI] kernel complaint: {line[:160]}")

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
            # was not supposed to hit. There are exactly two exceptions, and
            # both are deliberate: the int3 self-test, and svc-crash, which
            # dereferences null so that the boot proves a ring-3 fault kills
            # one task rather than the machine. That claim used to be gated and
            # green while being false, because every service in the manifest
            # died by exiting - a cooperative death that never reaches the trap
            # handler at all.
            lines = text.splitlines()
            killed = [ln for ln in lines if "ring3 fault: killing task" in ln]
            if len(killed) != 1:
                # Not "at least one": a second ring-3 fault is a real one, and
                # allowing any number would re-open exactly the hole this
                # assertion exists to close.
                problems.append("deliberate_ring3_faults=%d_expected=1" % len(killed))

            faults = [ln for ln in lines
                      if "[HW][TRAP]" in ln
                      and "vector=0x0000000000000003" not in ln       # int3 self-test
                      and "ring3 fault: killing task" not in ln]      # the kill notice
            # ...and the one trap dump that belongs to the deliberate crash: a
            # page fault raised from ring 3 (cs=0x23). Removed once, by value,
            # so a second identical fault still counts.
            for ln in faults:
                if "vector=0x000000000000000e" in ln and "cs=0x0000000000000023" in ln:
                    faults.remove(ln)
                    break
            if faults:
                problems.append("unexpected_cpu_fault")

            # "action=PANIC" is the trap model's recommendation, not what
            # happened: a ring-3 fault is now classified that way and then
            # survived. What says the machine actually stopped is hw_panic's
            # own output, so that is what this asserts on.
            if "FATAL" in text:
                problems.append("panic")

            # The supervisor's view of that same crash. A service killed by a
            # signal carries the signal in the low seven bits of the wait
            # status and leaves the exit-code byte zero, so an init that reads
            # only the code byte reports a segfault as a clean stop - which is
            # what this one did until SVC_KILLED existed.
            # The stack the kernel builds for a new program. This printed
            # "argv wrong" on every boot for as long as it was unasserted: the
            # self-test still required argv[0] to begin with 'i', from when it
            # was init, and it had since become a service exec'd under its own
            # name. A line in the serial log is not a check.
            if "argv ok" not in text or "argv wrong" in text:
                problems.append("argv_not_delivered_correctly")
            # Each service is exec'd under its own name, which is what makes
            # the supervisor's own logs mean anything.
            if "SVC_OK_RUNNING" not in text:
                problems.append("named_service_did_not_run")

            # The TLB shootdown actually runs. This gates the mechanism, not
            # the absence of the bug it fixes: a corruption that shows up one
            # boot in thirty cannot be asserted on in a single boot. What can
            # be asserted is that fork tells the other cores at all - and a
            # shootdown that silently never fires would leave the bug exactly
            # as it was, with every boot still green.
            # TFORK.ELF is built with musl-gcc, and a machine without it - the
            # CI runner, as it turns out - carries no such file. The assertions
            # below are therefore conditional on the binary being staged, the
            # same way the musl, PIE, dynamic and threads ones already are.
            #
            # Getting this wrong failed every CI boot, five out of five, while
            # fifty local boots stayed green: the gate was demanding evidence
            # from a program that was never on the disk. Worth remembering when
            # a test fails everywhere except where it was written.
            tfork = os.path.join(efi_root, "EFI", "BOOT", "TFORK.ELF")
            stats = re.search(r"tlb_shootdowns=0x([0-9a-f]{16}) tlb_acks=0x([0-9a-f]{16})", text)
            if stats is None:
                problems.append("no_tlb_shootdown_stats")
            else:
                shootdowns = int(stats.group(1), 16)
                acks = int(stats.group(2), 16)
                if shootdowns == 0 and os.path.exists(tfork):
                    # TFORK.ELF forks while one of its threads is still
                    # running, which is the only thing in this boot that
                    # gives another core a stale entry to drop. Requiring a
                    # non-zero count without it was simply wrong: an ordinary
                    # single-threaded fork correctly sends nothing at all,
                    # and the first version of this assertion failed every
                    # boot for saying otherwise.
                    problems.append("threaded_fork_never_shot_down_other_tlbs")
                elif acks < shootdowns:
                    # Every shootdown waits for one acknowledgement per other
                    # core, so acks below shootdowns means somebody gave up.
                    problems.append(f"tlb_acks={acks}_below_shootdowns={shootdowns}")
            # Console-lock hygiene. An unlock from a core that never held
            # the lock frees it out from under whoever is mid-line, and the
            # result is a gate that reports failures which did not happen.
            # It is refused and counted now; the count must stay zero.
            bad = re.search(r"bad_unlocks=0x([0-9a-f]{16})", text)
            if bad is None:
                problems.append("no_lock_hygiene_stats")
            elif int(bad.group(1), 16) != 0:
                problems.append(f"console_unlock_by_non_owner={int(bad.group(1), 16)}")

            # Freed pages are poisoned, and the poison is checked when the
            # page is handed out again. A hit means something wrote to
            # memory after it was released - which is how every hard
            # memory bug in this kernel has started.
            if "free page was written after it was freed" in text:
                problems.append("use_after_free_detected")

            if "shootdown timed out" in text:
                problems.append("tlb_shootdown_timed_out")
            if os.path.exists(tfork) and "TFORK_OK" not in text:
                problems.append("threaded_fork_lost_the_child_pages")

            # The crash recorder captured the deliberate fault, and named
            # the program it happened in. A dump that cannot say which
            # binary the task was running is the failure mode that cost
            # this project three wrong diagnoses of one address.
            if "[CRASH] recorded" not in text:
                problems.append("fault_was_not_recorded")
            elif "SVC_CRSH.ELF" not in text.split("[CRASH] recorded")[1][:200]:
                problems.append("crash_record_does_not_name_the_program")
            if "[CRASH] no process has faulted" in text:
                problems.append("crash_dump_found_no_record")

            # Randomised churn, and the seed that produced it. The seed is
            # asserted on as well as the result: a stress run whose seed is
            # not in the log is one nobody can replay, which is most of
            # what makes a rare failure findable.
            # The boot sequence itself, in order. The kernel must be up and
            # say so before userland starts: that ordering was inverted until
            # now, and asserting it is what stops it quietly reverting.
            # The console can actually show the history. A log nobody
            # can read is the same as no log, which is most of why the
            # last two days went the way they did.
            ring = re.search(r"arch ring: showing 0x([0-9a-f]{16})", text)
            if ring is None:
                problems.append("console_cannot_show_the_arch_log")
            elif int(ring.group(1), 16) == 0:
                problems.append("arch_log_ring_is_empty")

            # The three memory counters that are assertions rather than
            # diagnostics. Each one is a defect this subsystem has actually
            # produced, and each was invisible until it was counted.
            mz = re.search(r"MUSTBEZERO frames_leaked=0x([0-9a-f]{16}) "
                           r"frames_double_put=0x([0-9a-f]{16}) "
                           r"poison_hits=0x([0-9a-f]{16})", text)
            if mz is None:
                problems.append("meminfo_missing")
            else:
                for name, group in (("frames_leaked", 1),
                                    ("frames_double_put", 2),
                                    ("poison_hits", 3)):
                    value = int(mz.group(group), 16)
                    if value != 0:
                        problems.append(f"mm_{name}={value}")

            # The page cache, asserted as a ratio rather than as "not zero".
            #
            # Non-zero is satisfied by a cache that works once and thrashes
            # afterwards, which is what the first table size actually did: 768
            # pages against eleven megabytes of programs gave five hits in a
            # whole boot. A ratio is the difference between a cache and a
            # decoration.
            mc = re.search(r"cache_hits=0x([0-9a-f]{16}) cache_misses=0x([0-9a-f]{16})", text)
            if mc is None:
                problems.append("cache_counters_missing")
            else:
                hits = int(mc.group(1), 16)
                misses = int(mc.group(2), 16)
                if hits == 0 or misses == 0:
                    problems.append(f"cache_not_exercised hits={hits} misses={misses}")
                elif hits * 4 < misses:
                    problems.append(f"cache_hit_ratio_too_low hits={hits} misses={misses}")

            # The three task counters that are assertions rather than
            # diagnostics. Each names a defect this subsystem has produced: a
            # slot written to after being published as reusable, a stale
            # reference used as if it still named its task, and a task about to
            # run on page tables somebody else had freed.
            mt = re.search(r"\[TASKS\] MUSTBEZERO use_after_publish=0x([0-9a-f]{16}) "
                           r"tenancy_mismatch=0x([0-9a-f]{16}) "
                           r"cr3_without_owner=0x([0-9a-f]{16})", text)
            if mt is None:
                problems.append("tasks_counters_missing")
            else:
                for name, group in (("use_after_publish", 1),
                                    ("tenancy_mismatch", 2),
                                    ("cr3_without_owner", 3)):
                    value = int(mt.group(group), 16)
                    if value != 0:
                        problems.append(f"task_{name}={value}")

            # The frame states must partition the total exactly.
            #
            # This is the assertion that would have caught meminfo assembling
            # its picture from three sources that could not add up: allocated
            # was derived as total minus free and silently swallowed every
            # reserved frame. A breakdown whose parts do not sum to the whole is
            # worse than no breakdown, because somebody acts on it.
            mt = re.search(r"\[MEM\] bytes total=0x([0-9a-f]{16})", text)
            mf = re.search(r"\[MEM\] frames free=0x([0-9a-f]{16}) "
                           r"allocated=0x([0-9a-f]{16}) "
                           r"reserved=0x([0-9a-f]{16}) "
                           r"page-table=0x([0-9a-f]{16}) "
                           r"cache=0x([0-9a-f]{16})", text)
            if mt is None or mf is None:
                problems.append("meminfo_frames_missing")
            else:
                total = int(mt.group(1), 16) // 4096
                parts = sum(int(mf.group(i), 16) for i in range(1, 6))
                if parts != total:
                    problems.append(f"mm_states_do_not_sum {parts}!={total}")
                if int(mf.group(1), 16) == 0:
                    problems.append("mm_no_free_frames_at_cli")

            # Fragmentation. Asserted rather than printed, because a run of zero
            # means no contiguous allocation can ever succeed again - and the
            # kernel stack of the next task is a contiguous allocation.
            mr = re.search(r"largest_free_run=0x([0-9a-f]{16})", text)
            if mr is None:
                problems.append("meminfo_fragmentation_missing")
            elif int(mr.group(1), 16) < 2:
                problems.append("mm_no_contiguous_memory_left")

            # The frame layer replaced the bump allocator; if it failed to come
            # up the machine still boots on the static pool, which is a very
            # different machine and must not pass quietly.
            if "[MM] frame layer online:" not in text:
                problems.append("mm_frame_layer_absent")

            if "USERLAND_START" not in text:
                problems.append("userland_never_started")
            elif text.index("BOOT_OK") > text.index("USERLAND_START"):
                problems.append("kernel_announced_itself_after_userland_ran")
            if "USERLAND_DONE" not in text:
                problems.append("userland_never_finished")

            if "STRESS_SEED" not in text:
                problems.append("stress_seed_not_reported")
            if "STRESS_OK" not in text:
                problems.append("stress_run_did_not_finish")
            if "STRESS_FAIL" in text:
                problems.append("stress_run_found_a_defect")
                # Print it here rather than leaving it in an artifact. This
                # failure has reproduced five times out of five in CI and not
                # once in fifty boots locally, so the run that can see it is
                # the only one that can say what it is - and every one of
                # these lines names what was found, at which offset, and the
                # seed that replays it.
                for line in text.splitlines():
                    if "STRESS_FAIL" in line or "STRESS_SEED" in line:
                        print(f"[QEMU-CLI] {line.strip()[:200]}")

            if "SVC_CRASH_FAULTING" not in text:
                problems.append("crashing_service_never_ran")
            if "SVC_KILLED svc-crash" not in text:
                problems.append("signal_death_not_distinguished_from_exit")
            if "SVC_STOPPED svc-crash" in text:
                problems.append("crashed_service_reported_as_clean_stop")
            if "SVC_FAILED svc-crash" not in text:
                problems.append("crashed_service_not_marked_failed")

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

            # Supervision, asserted as behaviour rather than as a marker. Each
            # of these is a different way "we have services" can be false:
            #
            #  - three services start from the manifest, so init is reading a
            #    list rather than knowing one program by name;
            #  - a service that exits zero is left stopped. This one is easy to
            #    get wrong in the direction that looks healthy: a supervisor
            #    that restarts everything produces a busy, plausible log and an
            #    infinite loop;
            #  - a failing service is restarted by policy, then given up on. A
            #    supervisor with no limit and one with no restarts both "work"
            #    right up until a service starts failing;
            #  - init is still there afterwards and says so. Without this the
            #    other three could all be true of a supervisor that died with
            #    the last service it was watching.
            started = set(re.findall(r"SVC_START (\S+)", text))
            if len(started) < 3:
                problems.append("services_did_not_start_from_manifest")
            if "SVC_STOPPED svc-ok" not in text:
                problems.append("clean_exit_not_left_stopped")
            if "SVC_RESTART svc-flap 2" not in text:
                problems.append("failed_service_not_restarted_by_policy")
            if "SVC_FAILED svc-flap" not in text:
                problems.append("restart_limit_not_enforced")
            if "SVC_STOPPED svc-flap" in text:
                # The restart limit marks a service FAILED; calling it stopped
                # would report a machine in trouble as a machine at rest.
                problems.append("exhausted_service_reported_as_clean_stop")
            if "NATIVE_INIT_CHILD_EXITED" not in text:
                problems.append("init_did_not_survive_its_services")

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

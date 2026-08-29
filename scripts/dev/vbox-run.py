#!/usr/bin/env python3
"""Boot the shipped appliance on VirtualBox, and take it apart if it hangs.

Why this exists: the boot gate runs on QEMU, and for a long time that was the
only thing VibeOS was ever run on. The appliances imported, booted, and then
could not read their own disk - QEMU offers virtio-blk and no desktop
hypervisor does - and nothing caught it because nothing ever tried the artifact
by hand. A second implementation is not a luxury here; it is the only way to
tell "correct" from "correct on QEMU".

VirtualBox also debugs a stuck guest better than the QEMU monitor does:
`debugvm getregisters` gives per-core state on a machine that has stopped
talking, and `dumpvmcore` writes a full ELF core of guest memory - which is the
right answer for a wedge, and a great deal better than pushing megabytes
through a serial port.

Runs on Windows, where VBoxManage lives. addr2line is called through WSL,
because that is where the toolchain that built the kernel is.

    python scripts/dev/vbox-run.py                    # boot, watch, clean up
    python scripts/dev/vbox-run.py --core             # also dump guest memory
    python scripts/dev/vbox-run.py --keep             # leave the VM registered
"""

import argparse
import os
import re
import subprocess
import sys
import time

VBOXMANAGE = os.environ.get(
    "VBOXMANAGE", r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe")

# What a healthy boot says, in order. The last one means userland finished.
MARKERS = ("BOOT_OK", "NATIVE_INIT_READY", "STRESS_OK", "VIBEOS_SELFTEST_DONE")


def vbox(*args, check=False):
    """Run VBoxManage and hand back (rc, stdout+stderr)."""
    proc = subprocess.run([VBOXMANAGE, *args], capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if check and proc.returncode != 0:
        raise RuntimeError(f"VBoxManage {' '.join(args)} failed:\n{out}")
    return proc.returncode, out


def to_wsl_path(win_path):
    p = os.path.abspath(win_path).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:]
    return p


def addr2line(elf, addresses):
    """Symbolise through WSL, where the toolchain is."""
    if not addresses or not os.path.exists(elf):
        return {}
    cmd = ["wsl", "-d", "Ubuntu", "-e", "addr2line", "-f", "-C", "-e",
           to_wsl_path(elf), *[hex(a) for a in addresses]]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
    except OSError:
        return {}
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    # addr2line -f prints two lines per address: the name, then file:line.
    named = {}
    for i, addr in enumerate(addresses):
        if 2 * i + 1 < len(lines):
            named[addr] = f"{lines[2 * i]} ({lines[2 * i + 1]})"
    return named


def registers(name, cpus=4):
    """Per-core state of a guest that may well have stopped."""
    # getregisters wants the names spelled out; with none it refuses rather
    # than defaulting to all of them, and the refusal reads like "the VM is not
    # running", which is a different and much more alarming thing.
    wanted = ("rip", "rsp", "rbp", "cr3", "cs", "rflags")
    found = []
    for cpu in range(cpus):
        rc, out = vbox("debugvm", name, "getregisters", f"--cpu={cpu}", *wanted)
        if rc != 0:
            if cpu == 0:
                print(f"[VBOX]   getregisters refused: {out.strip()[-200:]}")
            continue
        regs = {}
        for key in wanted:
            m = re.search(rf"^{key}\s*=\s*([0-9a-fx']+)", out, re.M | re.I)
            if m:
                try:
                    regs[key] = int(m.group(1).replace("'", ""), 16)
                except ValueError:
                    pass
        if regs:
            found.append((cpu, regs))
    return found


def report(name, kernel_elf, want_core, out_dir):
    """Everything worth knowing about a guest that stopped."""
    print("[VBOX] the guest stopped talking; asking VirtualBox what it is doing")

    state = registers(name)
    if not state:
        print("[VBOX]   no register state - the VM is probably not running")
    rips = [regs["rip"] for _, regs in state if "rip" in regs]
    names = addr2line(kernel_elf, rips)
    for cpu, regs in state:
        bits = " ".join(f"{k}={v:#x}" for k, v in regs.items())
        print(f"[VBOX]   cpu{cpu} {bits}")
        rip = regs.get("rip")
        if rip in names:
            print(f"[VBOX]          {names[rip]}")

    # Which firmware/OS VirtualBox thinks is running: on a boot that never
    # leaves the bootloader this is the difference between "our code hung" and
    # "the firmware did".
    rc, out = vbox("debugvm", name, "info", "mode")
    if rc == 0 and out.strip():
        print(f"[VBOX]   cpu mode: {out.strip().splitlines()[0]}")

    if want_core:
        core = os.path.join(out_dir, f"{name}-core.elf")
        rc, out = vbox("debugvm", name, "dumpvmcore", f"--filename={core}")
        if rc == 0:
            size = os.path.getsize(core) if os.path.exists(core) else 0
            print(f"[VBOX]   guest core written: {core} ({size} bytes)")
        else:
            print(f"[VBOX]   dumpvmcore failed: {out.strip()[:200]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ova", default=os.path.join(
        "build-gcc-Release", "artifacts", "vibeos.ova"))
    ap.add_argument("--name", default="VibeOS-smoke")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--core", action="store_true",
                    help="write a full guest memory core if the boot hangs")
    ap.add_argument("--keep", action="store_true",
                    help="leave the VM registered afterwards")
    ap.add_argument("--kernel", default=os.path.join(
        "build-gcc-Release", "artifacts", "vibeos_kernel.elf"))
    args = ap.parse_args()

    if not os.path.exists(VBOXMANAGE):
        print(f"[VBOX] VBoxManage not found at {VBOXMANAGE}; "
              "set VBOXMANAGE to its path")
        return 2
    if not os.path.exists(args.ova):
        print(f"[VBOX] no appliance at {args.ova}; build the vibeos_vm_images "
              "target first")
        return 2

    # A leftover from a previous run would otherwise make the import fail with
    # a name clash, which reads like a defect in the appliance.
    vbox("controlvm", args.name, "poweroff")
    vbox("unregistervm", args.name, "--delete")

    base = os.path.abspath("build-gcc-Release")
    rc, out = vbox("import", os.path.abspath(args.ova), "--vsys", "0",
                   "--vmname", args.name, "--basefolder", base)
    if rc != 0:
        print(f"[VBOX] import failed:\n{out.strip()[-2000:]}")
        return 1
    print(f"[VBOX] imported {args.ova} as {args.name}")

    log = os.path.join(base, args.name, "vibeos-serial.log")
    if os.path.exists(log):
        os.remove(log)
    vbox("modifyvm", args.name, "--uart1", "0x3F8", "4",
         "--uartmode1", "file", log)

    rc, out = vbox("startvm", args.name, "--type", "headless")
    if "successfully started" not in out.lower():
        print(f"[VBOX] could not start the VM:\n{out.strip()[-1000:]}")
        vbox("unregistervm", args.name, "--delete")
        return 1

    deadline = time.time() + args.timeout
    seen = []
    last_size = -1
    quiet_since = time.time()
    failed = False
    try:
        while time.time() < deadline:
            time.sleep(2)
            if not os.path.exists(log):
                continue
            size = os.path.getsize(log)
            if size != last_size:
                last_size = size
                quiet_since = time.time()
            text = open(log, encoding="utf-8", errors="replace").read()
            for marker in MARKERS:
                if marker in text and marker not in seen:
                    seen.append(marker)
                    print(f"[VBOX] {marker}")
            if MARKERS[-1] in text:
                break
            # Silence for this long, after it has said something, means it has
            # stopped rather than that it is slow.
            if seen and time.time() - quiet_since > 60:
                print("[VBOX] no output for 60s")
                failed = True
                break
        else:
            print(f"[VBOX] timed out after {args.timeout}s")
            failed = True

        if failed or MARKERS[-1] not in seen:
            failed = True
            report(args.name, args.kernel, args.core, os.path.join(base, args.name))
            tail = open(log, encoding="utf-8", errors="replace").read().splitlines()[-25:]
            print("[VBOX] last serial output:")
            for line in tail:
                print(f"[VBOX]   {line}")
        else:
            print(f"[VBOX] PASS: reached {MARKERS[-1]}, serial log at {log}")
    finally:
        vbox("controlvm", args.name, "poweroff")
        time.sleep(2)
        if not args.keep:
            vbox("unregistervm", args.name, "--delete")
            print(f"[VBOX] removed {args.name}")
        else:
            print(f"[VBOX] left {args.name} registered, serial log at {log}")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

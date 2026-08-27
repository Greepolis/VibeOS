#!/usr/bin/env python3
"""Ask the emulator what a wedged guest is doing, and how it got there.

    python3 scripts/dev/wedge_report.py <monitor-socket> <kernel-elf>

A kernel that has stopped cannot report on itself. That is the whole difficulty
with the intermittent wedge here: the panic path prints a backtrace and dumps
the log ring, and none of it runs, because nothing panicked - the machine simply
went quiet. Every in-guest improvement to logging is invisible in exactly the
case that matters.

So this asks from outside. QEMU's monitor gives each vCPU's registers whether or
not the guest is willing, and its memory reads let the frame chain be walked
from the host side. The result is a real backtrace per core for a guest that has
not executed an instruction in a minute.

Two things make it worth more than it used to be. The kernel image is now built
with frame pointers and debug info, so the chain exists and the addresses
resolve to file and line through addr2line rather than through
nearest-preceding-symbol guessing - which, in a five-thousand-line file where
almost everything is static, is frequently wrong.
"""
import re
import socket
import subprocess
import sys
import time

MAX_FRAMES = 12
CODE_LO = 0x4000000
CODE_HI = 0x5000000


def monitor(path, command, settle=1.5):
    """Send one monitor command and return its output.

    The monitor is a terminal, not a pipe: it echoes what you type, one
    escape-laden keystroke at a time, and the answer arrives after all of that.
    Reading once after a sleep returns the echo, and a parser fed the echo
    produces confident nonsense - this reader first reported that the kernel
    was unmapped on a core that was visibly executing kernel code. So read
    until the prompt comes back, then strip the echo and the escapes.
    """
    del settle
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(0.5)
        s.connect(path)
    except OSError as exc:
        return f"monitor unreachable: {exc}"

    def drain(deadline):
        buf = b""
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(1 << 16)
            except socket.timeout:
                if buf.rstrip().endswith(b"(qemu)"):
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if buf.rstrip().endswith(b"(qemu)"):
                break
        return buf

    drain(time.monotonic() + 2.0)          # banner and first prompt
    try:
        s.sendall(command.encode() + b"\n")
    except OSError as exc:
        s.close()
        return f"monitor unreachable: {exc}"
    raw = drain(time.monotonic() + 6.0).decode("utf-8", "replace")
    s.close()

    # Strip terminal escapes, then the echoed command line itself.
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw)
    text = text.replace("\r", "")
    lines = [ln for ln in text.splitlines()
             if ln.strip() and not ln.strip().startswith(command.split()[0])
             or ":" in ln]
    return "\n".join(lines)


def read_words(path, addr, count):
    """Read `count` 64-bit words of guest memory at a physical address.

    Physical rather than virtual because the kernel lives in the identity map,
    and `xp` does not depend on which CPU's page tables happen to be loaded.

    The monitor prints one line per group:

        0000000004000000: 0x00000000e85250d6 0x17adaf1200000018

    The address carries no 0x prefix and the values do, so every 0x-prefixed
    number on the line is a value. An earlier version of this treated the first
    match as the echoed address and dropped it, which silently shifted every
    word by one - and a frame walk fed shifted words does not fail, it invents
    a plausible-looking chain.
    """
    out = monitor(path, f"xp /{count}gx 0x{addr:x}")
    values = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        values.extend(int(n, 16) for n in re.findall(r"0x([0-9a-f]+)", line))
    return values[:count]


def plausible_code(addr):
    return CODE_LO <= addr < CODE_HI


def plausible_frame(rbp, prev):
    if rbp == 0 or rbp % 8 != 0:
        return False
    if rbp < 0x1000 or rbp >= 0x8000000000:
        return False
    return prev is None or rbp > prev


def walk(path, rbp):
    """Follow the saved frame pointers, from the host side."""
    frames = []
    prev = None
    for _ in range(MAX_FRAMES):
        if not plausible_frame(rbp, prev):
            break
        words = read_words(path, rbp, 2)
        if len(words) < 2:
            break
        saved_rbp, ret = words[0], words[1]
        if not plausible_code(ret):
            break
        frames.append(ret)
        prev, rbp = rbp, saved_rbp
    return frames


def symbolize(kernel, addrs):
    if not addrs:
        return {}
    try:
        out = subprocess.run(["addr2line", "-f", "-C", "-e", kernel] +
                             [f"0x{a:x}" for a in addrs],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return {}
    lines = out.splitlines()
    res = {}
    for i, a in enumerate(addrs):
        name = lines[2 * i] if 2 * i < len(lines) else "?"
        where = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "?"
        res[a] = (name, where.rsplit("/", 1)[-1])
    return res


def report(monitor_path, kernel):
    lines = []
    dump = monitor(monitor_path, "info registers -a", settle=2.0)
    if dump.startswith("monitor unreachable"):
        return [f"[WEDGE] {dump}"]

    # Split on the CPU# headings and parse each block on its own. A single
    # regex walking the whole dump with .*? silently skipped cores - it
    # reported two of four, and the two it dropped could have been the ones
    # that mattered. A report that quietly omits half the machine is worse
    # than no report, because it reads like a complete one.
    blocks = re.split(r"^CPU#(\d+)", dump, flags=re.M)
    cores = []
    for i in range(1, len(blocks) - 1, 2):
        num = int(blocks[i])
        body = blocks[i + 1]

        def field(name, text=body):
            m = re.search(name + r"=([0-9a-f]{8,16})", text)
            return int(m.group(1), 16) if m else None

        cores.append({
            "num": num,
            "rip": field("RIP"),
            "rsp": field("RSP"),
            "rbp": field("RBP"),
            "cr3": field("CR3"),
        })

    if not cores:
        return ["[WEDGE] monitor gave no CPU state"]

    wanted = [c["rip"] for c in cores if c["rip"] is not None]
    chains = {}
    for c in cores:
        chain = walk(monitor_path, c["rbp"] or 0)
        chains[c["num"]] = chain
        wanted.extend(chain)

    names = symbolize(kernel, wanted)

    lines.append("[WEDGE] where each core stopped, and how it got there")
    for c in cores:
        num, rip, cr3 = c["num"], c["rip"], c["cr3"]
        name, where = names.get(rip, ("?", "?"))
        lines.append(f"[WEDGE] CPU#{num} rip=0x{(rip or 0):016x} {name} {where}")
        if cr3 is not None:
            # The kernel lives at 0x4000000, which is PML4 entry 0.
            entry = read_words(monitor_path, cr3 & ~0xFFF, 1)
            present = (entry[0] & 1) if entry else -1
            verdict = ("kernel mapped" if present == 1 else
                       "KERNEL NOT MAPPED" if present == 0 else
                       "could not read the table")
            lines.append(f"[WEDGE]    cr3=0x{cr3:016x} pml4[0]={verdict}")
        for depth, ret in enumerate(chains.get(num, [])):
            n2, w2 = names.get(ret, ("?", "?"))
            lines.append(f"[WEDGE]    #{depth} 0x{ret:016x} {n2} {w2}")
        if not chains.get(num):
            lines.append("[WEDGE]    (no frame chain: idle, or stopped in "
                         "assembly with no frame set up)")
    return lines


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    for line in report(sys.argv[1], sys.argv[2]):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())

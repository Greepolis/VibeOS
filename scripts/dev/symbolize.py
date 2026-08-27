#!/usr/bin/env python3
"""Turn a kernel backtrace into function names and source lines.

    python3 scripts/dev/symbolize.py [build-dir] < serial.log
    tr -d '\\r' < qemu-cli-serial.log | python3 scripts/dev/symbolize.py

Reads a serial log on stdin, finds the [BT] lines a panic printed, and resolves
each address with addr2line against the kernel that produced them.

The kernel prints raw addresses on purpose. Nearly every function in arch_hw.c
is static, so naming an address from the nearest preceding symbol - which is
what catch-hang.py has to do against a running guest - is frequently wrong, and
a confidently wrong name sends you to the wrong file. addr2line reads the debug
info and does not guess, so the naming happens here, where the ELF is.
"""
import re
import subprocess
import sys

BT = re.compile(r"\[BT\]\s+#[0-9a-fx]+\s+(0x[0-9a-f]+)")
RIP = re.compile(r"\[BT\]\s+rip=(0x[0-9a-f]+)")


def resolve(kernel, addrs):
    if not addrs:
        return {}
    try:
        out = subprocess.run(["addr2line", "-f", "-C", "-e", kernel] + addrs,
                             capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        print("addr2line not found; install binutils", file=sys.stderr)
        return {}
    except subprocess.CalledProcessError as exc:
        print(f"addr2line failed: {exc.stderr.strip()}", file=sys.stderr)
        return {}
    lines = out.splitlines()
    resolved = {}
    for i, addr in enumerate(addrs):
        name = lines[2 * i] if 2 * i < len(lines) else "?"
        where = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "?"
        resolved[addr] = (name, where)
    return resolved


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    kernel = f"{build}/vibeos_kernel"
    text = sys.stdin.read()

    addrs = []
    for m in list(RIP.finditer(text)) + list(BT.finditer(text)):
        a = m.group(1)
        if a not in addrs:
            addrs.append(a)
    if not addrs:
        print("no [BT] lines in the input - was the panic backtrace printed?")
        return 1

    resolved = resolve(kernel, addrs)
    for line in text.splitlines():
        m = RIP.search(line) or BT.search(line)
        if not m:
            continue
        addr = m.group(1)
        name, where = resolved.get(addr, ("?", "?"))
        kind = "faulted at" if RIP.search(line) else "called from"
        print(f"{kind:<12} {addr}  {name}  {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

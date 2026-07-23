#!/usr/bin/env python3
"""Embed a binary file as a C byte array.

Usage: bin2c.py <input> <output.c> <symbol>
Emits:
    const unsigned char <symbol>[] = { ... };
    const unsigned long <symbol>_len = <n>;
"""
import sys


def main():
    if len(sys.argv) != 4:
        print("usage: bin2c.py <input> <output.c> <symbol>", file=sys.stderr)
        return 2
    inp, out, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(inp, "rb") as fp:
        data = fp.read()
    lines = [f"/* Generated from {inp} by bin2c.py - do not edit. */"]
    lines.append(f"const unsigned char {sym}[] = {{")
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + "".join(f"0x{b:02x}," for b in chunk))
    lines.append("};")
    lines.append(f"const unsigned long {sym}_len = {len(data)}u;")
    lines.append("")
    with open(out, "w", encoding="ascii") as fp:
        fp.write("\n".join(lines))
    print(f"[bin2c] {inp} -> {out} ({len(data)} bytes, symbol {sym})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

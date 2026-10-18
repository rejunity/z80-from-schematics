#!/usr/bin/env python3
"""Intel HEX -> $readmemh image (65536 lines, one byte per line).

Usage: hex2mem.py <rom.hex> <out.mem>
       hex2mem.py --top <rom.hex>

Same record subset as scripts/basicrunner.c's loader: type 00 data
records honoured, type 01 ends the file, everything else skipped.
Unused addresses are zero-filled (BRAM powers up zeroed anyway; this
keeps the image explicit and simulation X-free).

--top prints the end of the image rounded up to 1 KiB, in decimal: the
Makefile's default ROM_PROTECT_TOP (writes below it are dropped). 8192
for NASCOM BASIC (0x0000-0x1FD9); 1024 for the 1K Tiny BASIC, which
keeps its variables and stack in the RAM right above it (0x0400-0x07FF).
"""
import sys


def load(path):
    """{address: byte} of the image's data records."""
    data = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            length = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            rtype = int(line[7:9], 16)
            if rtype == 1:
                break
            if rtype != 0:
                continue
            for i in range(length):
                data[(addr + i) & 0xFFFF] = int(line[9 + i * 2 : 11 + i * 2], 16)
    return data


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--top":
        data = load(sys.argv[2])
        end = max(data) + 1 if data else 0
        print((end + 1023) // 1024 * 1024)
        return
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <rom.hex> <out.mem> | --top <rom.hex>")
    mem = bytearray(65536)
    for a, b in load(sys.argv[1]).items():
        mem[a] = b
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(f"{b:02x}" for b in mem) + "\n")
    print(f"[hex2mem] {sys.argv[1]} -> {sys.argv[2]} (64 KiB image)")


if __name__ == "__main__":
    main()

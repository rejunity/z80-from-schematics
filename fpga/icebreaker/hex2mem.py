#!/usr/bin/env python3
"""Intel HEX -> $readmemh image (one byte per line).

Usage: hex2mem.py <rom.hex> <out.mem> [size]

Same record subset as scripts/basicrunner.c's loader: type 00 data
records honoured, type 01 ends the file, everything else skipped.
The image covers addresses 0 .. size-1 (default 65536); unused
addresses are zero-filled (keeps the image explicit and simulation
X-free). A data byte at or above `size` is an error: on the iCEBreaker
the image is the 8 KiB ROM window (block RAM), not a 64 KiB RAM.

Copied from the IcePi Zero port (branch fpga-icepi-zero-basic); the size
argument is new.
"""
import sys


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(f"usage: {sys.argv[0]} <rom.hex> <out.mem> [size]")
    size = int(sys.argv[3], 0) if len(sys.argv) == 4 else 65536
    if not 0 < size <= 65536:
        sys.exit(f"[hex2mem] bad image size {size}")
    mem = bytearray(size)
    with open(sys.argv[1]) as f:
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
                a = (addr + i) & 0xFFFF
                if a >= size:
                    sys.exit(f"[hex2mem] {sys.argv[1]}: byte at 0x{a:04X} is outside "
                             f"the {size}-byte image")
                mem[a] = int(line[9 + i * 2 : 11 + i * 2], 16)
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(f"{b:02x}" for b in mem) + "\n")
    print(f"[hex2mem] {sys.argv[1]} -> {sys.argv[2]} ({size // 1024} KiB image)")


if __name__ == "__main__":
    main()

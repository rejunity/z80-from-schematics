#!/usr/bin/env python3
"""FPGA bitstream (.bin) -> UF2 for the pico-ice's drag-and-drop drive.

Usage: bin2uf2.py <in.bin> <out.uf2>

Byte-for-byte what tinyVision's `bin2uf2 -o out.uf2 in.bin` writes
(https://github.com/tinyvision-ai-inc/uf2-utils): 512-byte UF2 blocks,
256-byte payloads mapped from flash address 0x00000000, flag
FAMILYID_PRESENT with the pico-ice family ID 0x792e7263. The default
pico-ice firmware writes them to the iCE40's configuration flash when
the file is copied onto its `pico-ice` USB drive, then restarts the FPGA.
"""
import struct
import sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILYID_PRESENT = 0x00002000
FAMILY_ID = 0x792E7263
PAYLOAD = 256


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <in.bin> <out.uf2>")
    data = open(sys.argv[1], "rb").read()
    nblocks = (len(data) + PAYLOAD - 1) // PAYLOAD
    with open(sys.argv[2], "wb") as f:
        for n in range(nblocks):
            chunk = data[n * PAYLOAD:(n + 1) * PAYLOAD]
            f.write(struct.pack("<8I", MAGIC0, MAGIC1, FLAG_FAMILYID_PRESENT,
                                n * PAYLOAD, PAYLOAD, n, nblocks, FAMILY_ID))
            f.write(chunk.ljust(476, b"\0"))
            f.write(struct.pack("<I", MAGIC_END))
    print(f"[bin2uf2] {sys.argv[1]} -> {sys.argv[2]} ({nblocks} blocks)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build a Cringle ZEXDOC/ZEXALL .com that runs only a chosen subset of
the 67 instruction tests. The driver loop, BDOS shim, CRC tables, and
all 67 test data blocks stay in place — only the test-pointer table
(at CP/M 0x013A) is rewritten with the selected pointers + a 0x0000
terminator. This is byte-identical to running zexdoc with the full
suite minus the omitted tests; the C zexrunner and Verilator sim_zex
both read it through their existing CP/M emulation.

Usage:
    scripts/zex_make_subset.py <input.com> <output.com> [test_index1 ...]

If no indices are passed, prints the full 67-entry table with name +
file-offset so you can pick by index. Names matching the well-known
problematic instructions in our model are marked PROBLEMATIC.

The test table layout in the .com:
    file_off 0x3A: u16-LE pointer to test data block #0
    file_off 0x3C: u16-LE pointer to test data block #1
    ...
    terminator:  u16-LE 0x0000
"""
import os, sys

TABLE_OFF = 0x003A
LOAD_BASE = 0x0100      # CP/M .com load address
ENTRY_BYTES = 2

# Names that match instructions our model is known (from z80test runs +
# pin_scenarios) to handle imperfectly or that exercise the deepest
# silicon-faithfulness paths. Used only to annotate the listing.
PROBLEMATIC_HINTS = (
    "ldd", "ldi",        # block LD with NOP'/interruption (z80test fails)
    "<adc,sbc> hl",      # 16-bit arithmetic
    "neg",
    "daa,cpl,scf,ccf",   # SCF/CCF Q-leak (z80full fails on ST variant)
    "<rrd,rld>",
    "bit n,(<ix,iy>",    # DDCB BIT MEMPTR
    "shf/rot (<ix,iy>",  # DDCB shifts
    "<set,res> n,(<ix,iy>",
    "<inc,dec> (<ix,iy>",
    "cpi", "cpd",        # block CP
)

def parse_table(buf):
    ptrs = []
    i = TABLE_OFF
    while True:
        if i + 1 >= len(buf): break
        p = buf[i] | (buf[i+1] << 8)
        if p == 0: break
        ptrs.append(p)
        i += 2
    return ptrs, i

def parse_name(buf, ptr):
    """Each Cringle test entry: 20-byte test pattern + 20-byte mask + 4-byte
    expected CRC + ASCIIZ name. Find the name by scanning past the binary
    block to the first run of printable ASCII ending in $."""
    off = ptr - LOAD_BASE
    j = buf.find(b'$', off)
    if j == -1: return ""
    k = j - 1
    while k > off and 32 <= buf[k] < 127:
        k -= 1
    return buf[k+1:j].decode('ascii', errors='replace')

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    in_path = sys.argv[1]
    out_path = sys.argv[2]
    indices = [int(x) for x in sys.argv[3:]]
    buf = bytearray(open(in_path, 'rb').read())
    ptrs, term = parse_table(buf)
    print(f"# {in_path}: {len(ptrs)} tests in table at file_off 0x{TABLE_OFF:04x}, "
          f"terminator at 0x{term:04x}", file=sys.stderr)
    if not indices:
        # listing mode
        print("# idx | cp/m addr | name (PROBLEMATIC = our model has known gaps)")
        for k, p in enumerate(ptrs):
            name = parse_name(buf, p).strip().rstrip('.')
            mark = ""
            for h in PROBLEMATIC_HINTS:
                if h in name: mark = "  PROBLEMATIC"; break
            print(f"  {k:2d}: 0x{p:04x}  {name}{mark}")
        sys.exit(0)
    # subset mode: write a new .com with only chosen pointers
    if max(indices) >= len(ptrs) or min(indices) < 0:
        sys.exit(f"index out of range 0..{len(ptrs)-1}")
    # Build new pointer-list bytes
    new_table = bytearray()
    for i in indices:
        new_table.append(ptrs[i] & 0xFF)
        new_table.append((ptrs[i] >> 8) & 0xFF)
    new_table.append(0); new_table.append(0)   # 0x0000 terminator
    # Pad to original table length so the rest of the binary doesn't move
    orig_table_len = term - TABLE_OFF + 2     # includes terminator
    pad = orig_table_len - len(new_table)
    if pad < 0:
        sys.exit(f"subset table ({len(new_table)} bytes) exceeds original "
                 f"({orig_table_len} bytes)")
    new_table.extend(b'\x00' * pad)
    buf[TABLE_OFF:TABLE_OFF + orig_table_len] = new_table
    open(out_path, 'wb').write(buf)
    print(f"# wrote {out_path}: {len(indices)} tests selected", file=sys.stderr)
    for i in indices:
        print(f"#   #{i:2d}: 0x{ptrs[i]:04x}  "
              f"{parse_name(buf, ptrs[i]).strip().rstrip('.')}",
              file=sys.stderr)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Print every placed I/O pad of a routed iCE40 netlist: package pin,
PMOD name, top-level port, and the direction the pad is configured for.

Usage: pins.py <top_<build>.pnr.json> <icebreaker.pcf>

Reads nextpnr's --write output, so it shows what the bitstream really
does, not what top.v intended: the SB_IO PIN_TYPE output mode (bits 5:4:
00 none, 01 always on, 1x tristate with OUTPUT_ENABLE), whether the input
path is used (D_IN_0/1, or the pad's dedicated global-buffer path), and
the pull-up.
"""
import json
import re
import sys

# iCEBreaker board pins (icebreaker-fpga/icebreaker-verilog-examples
# icebreaker.pcf) -> connector names, for the table
BOARD = {
    35: "CLK 12MHz", 6: "UART RX", 9: "UART TX", 10: "BTN_N",
    11: "LEDR_N", 37: "LEDG_N",
    4: "P1A1", 2: "P1A2", 47: "P1A3", 45: "P1A4",
    3: "P1A7", 48: "P1A8", 46: "P1A9", 44: "P1A10",
    43: "P1B1", 38: "P1B2", 34: "P1B3", 31: "P1B4",
    42: "P1B7", 36: "P1B8", 32: "P1B9", 28: "P1B10",
    27: "P2_1", 25: "P2_2", 21: "P2_3", 19: "P2_4",
    26: "P2_7", 23: "P2_8", 20: "P2_9", 18: "P2_10",
}


def pcf_pins(path):
    pins = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 3 and line[0] == "set_io":
                # drop option values (-pullup yes, -pullup_resistor 10K)
                args = [a for i, a in enumerate(line[1:])
                        if not a.startswith("-") and not line[i].startswith("-pull")]
                pins[args[0]] = int(args[1])
    return pins


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <top_<build>.pnr.json> <pcf>")
    mod = json.load(open(sys.argv[1]))["modules"]["top"]
    pins = pcf_pins(sys.argv[2])

    bit_name = {}
    for name, n in mod["netnames"].items():
        for b in n["bits"]:
            if not isinstance(b, str) and (b not in bit_name or name.count("$") < bit_name[b].count("$")):
                bit_name[b] = name
    used_bits = set()      # bits read by some cell input
    for c in mod["cells"].values():
        for p, bits in c["connections"].items():
            if c["port_directions"].get(p) == "input":
                used_bits.update(b for b in bits if not isinstance(b, str))
    gbuf_from = {cn[len("$gbuf_"):-len("_io")] for cn, c in mod["cells"].items()
                 if c["type"] == "SB_GB" and cn.startswith("$gbuf_") and cn.endswith("_io")}
    # a *_PAD PLL sits in its input pad's tile and takes the pad directly
    pll_tiles = {c["attributes"].get("NEXTPNR_BEL", "").rsplit("/", 1)[0]
                 for c in mod["cells"].values() if c["type"] == "ICESTORM_PLL"}
    oe_fanout = {}
    for c in mod["cells"].values():
        if c["type"] == "SB_IO" and (int(c["parameters"].get("PIN_TYPE", "0"), 2) >> 4) & 2:
            for b in c["connections"].get("OUTPUT_ENABLE", []):
                oe_fanout[b] = oe_fanout.get(b, 0) + 1

    rows = []
    for cn, c in mod["cells"].items():
        if c["type"] != "SB_IO":
            continue
        conn = c["connections"]
        port = bit_name.get(conn["PACKAGE_PIN"][0], "?") if conn.get("PACKAGE_PIN") else "?"
        pt = int(c["parameters"].get("PIN_TYPE", "0"), 2)
        oe_mode = (pt >> 4) & 3
        in_used = any(b in used_bits for p in ("D_IN_0", "D_IN_1") for b in conn.get(p, []))
        in_global = cn in gbuf_from
        if oe_mode == 0:
            out = None
        elif oe_mode == 1:
            out = "out"
        else:
            oe = conn.get("OUTPUT_ENABLE", [])
            out = "tristate, OE from fabric (one net for %d pads)" % oe_fanout.get(oe[0], 0) \
                if oe else "tristate, OE unconnected"
        if out and (in_used or in_global):
            kind = "bidir " + out
        elif out:
            kind = out
        elif in_global:
            kind = "in -> global buffer" + (" + fabric" if in_used else "")
        elif in_used:
            kind = "in"
        elif c["attributes"].get("NEXTPNR_BEL", "").rsplit("/", 1)[0] in pll_tiles:
            kind = "in -> PLL"
        else:
            kind = "unused input"
        pin = pins.get(port, "?")
        pull = " pull-up" if c["parameters"].get("PULLUP", "0").strip("0") else ""
        rows.append((BOARD.get(pin, "") if pin != "?" else "", pin, port, kind + pull,
                     c["parameters"].get("PIN_TYPE", "")[-6:]))

    order = list(BOARD.values())
    rows.sort(key=lambda r: (order.index(r[0]) if r[0] in order else 99, str(r[2])))
    print(f"{'board pin':10} {'FPGA':>4}  {'port':12} {'PIN_TYPE':8}  direction")
    for b, pin, port, kind, pt in rows:
        print(f"{b:10} {pin!s:>4}  {re.sub(r'^.*/', '', port):12} {pt:8}  {kind}")


if __name__ == "__main__":
    main()

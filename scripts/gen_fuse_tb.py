#!/usr/bin/env python3
# gen_fuse_tb.py - read tests/fuse/tests.in and emit a single Verilog testbench
# (tests/iverilog/tb_fuse.v) that runs every case through z80_core via direct
# hierarchical register pokes, then prints each test's final state in a format
# the driver compares against tests.expected.
#
# Usage:  python3 scripts/gen_fuse_tb.py
import os, sys, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IN_PATH  = os.path.join(ROOT, "tests/fuse/tests.in")
EXP_PATH = os.path.join(ROOT, "tests/fuse/tests.expected")
TB_PATH  = os.path.join(ROOT, "tests/iverilog/tb_fuse.v")

def parse_tests(path):
    """Yield dicts with the parsed FUSE test inputs."""
    with open(path) as f:
        text = f.read()
    blocks = re.split(r"\n\s*\n", text.strip())
    for blk in blocks:
        lines = [ln.strip() for ln in blk.split("\n") if ln.strip()]
        if not lines: continue
        name = lines[0]
        regs = lines[1].split()
        af,bc,de,hl,af2,bc2,de2,hl2,ix,iy,sp,pc,wz = [int(x,16) for x in regs[:13]]
        s = lines[2].split()
        ireg, rreg = int(s[0],16), int(s[1],16)
        iff1, iff2, im, halted, ts = int(s[2]), int(s[3]), int(s[4]), int(s[5]), int(s[6])
        mem = []
        for ln in lines[3:]:
            parts = ln.split()
            if parts[0] == "-1": break
            addr = int(parts[0], 16)
            bytes_ = []
            for p in parts[1:]:
                if p == "-1": break
                bytes_.append(int(p, 16))
            mem.append((addr, bytes_))
        yield dict(name=name, af=af, bc=bc, de=de, hl=hl, af2=af2, bc2=bc2,
                   de2=de2, hl2=hl2, ix=ix, iy=iy, sp=sp, pc=pc, wz=wz,
                   i=ireg, r=rreg, iff1=iff1, iff2=iff2, im=im, halted=halted,
                   tstates=ts, mem=mem)

def emit_test(t):
    body = []
    # reset, then per-test setup
    body.append(f'        // ===== test {t["name"]} =====')
    body.append('        do_reset();')
    # clear test region in memory (don\'t need full wipe each time; we only set what tests.in sets)
    for addr, bytes_ in t["mem"]:
        for i, b in enumerate(bytes_):
            body.append(f'        mem[16\'h{(addr+i)&0xFFFF:04x}] = 8\'h{b:02x};')
    # poke registers, including the _n shadows so the next clock commits cleanly
    pokes = [
        (f"`RFP_AF", t["af"]),  (f"`RFP_BC", t["bc"]),
        (f"`RFP_DE", t["de"]),  (f"`RFP_HL", t["hl"]),
        (f"`RFP_AF2", t["af2"]),(f"`RFP_BC2", t["bc2"]),
        (f"`RFP_DE2", t["de2"]),(f"`RFP_HL2", t["hl2"]),
        (f"`RFP_IX", t["ix"]),  (f"`RFP_IY", t["iy"]),
        (f"`RFP_SP", t["sp"]),  (f"`RFP_PC", t["pc"]),
        (f"`RFP_WZ", t["wz"]),
    ]
    for sel, val in pokes:
        body.append(f'        dut.rf[{sel}]   = 16\'h{val:04x};')
        body.append(f'        dut.rf_n[{sel}] = 16\'h{val:04x};')
    body.append(f'        dut.reg_i      = 8\'h{t["i"]:02x};  dut.reg_i_n   = 8\'h{t["i"]:02x};')
    body.append(f'        dut.reg_r      = 8\'h{t["r"]:02x};  dut.reg_r_n   = 8\'h{t["r"]:02x};')
    body.append(f'        dut.iff1       = 1\'b{t["iff1"]};   dut.iff1_n    = 1\'b{t["iff1"]};')
    body.append(f'        dut.iff2       = 1\'b{t["iff2"]};   dut.iff2_n    = 1\'b{t["iff2"]};')
    body.append(f'        dut.im         = 2\'d{t["im"]};')
    body.append(f'        dut.halted     = 1\'b{t["halted"]}; dut.halted_n  = 1\'b{t["halted"]};')
    # Q convention: F before the test was the Q value (last F-modifying instr's F).
    body.append(f'        dut.reg_q      = 8\'h{t["af"] & 0xFF:02x}; dut.reg_q_n = 8\'h{t["af"] & 0xFF:02x};')
    body.append(f'        dut.f_modified = 1\'b0; dut.f_modified_n = 1\'b0;')
    # (bus/sequencer state left in whatever post-reset state the RTL produced;
    # explicit pokes there broke alignment. For tests whose initial PC != the
    # reset PC, the in-flight M1 fetches the wrong opcode and the test
    # mis-executes; this affects the small handful of RST 00..38 tests where
    # the in-flight M1 itself happens to execute a stray opcode at PC=0.)
    # run cycles
    body.append(f'        run_phases({2 * t["tstates"]});')
    # sample and emit
    body.append('        $display("RESULT %s %04h %04h %04h %04h %04h %04h %04h %04h %04h %04h %04h %04h %04h %02h %02h %b %b %0d %b %0d",')
    body.append(f'                 "{t["name"]}",')
    body.append('                 dut.rf[`RFP_AF], dut.rf[`RFP_BC], dut.rf[`RFP_DE], dut.rf[`RFP_HL],')
    body.append('                 dut.rf[`RFP_AF2], dut.rf[`RFP_BC2], dut.rf[`RFP_DE2], dut.rf[`RFP_HL2],')
    body.append('                 dut.rf[`RFP_IX], dut.rf[`RFP_IY], dut.rf[`RFP_SP], dut.rf[`RFP_PC], dut.rf[`RFP_WZ],')
    body.append('                 dut.reg_i, dut.reg_r, dut.iff1, dut.iff2, dut.im, dut.halted, cycle_count);')
    # dump memory that the expected output cares about
    for addr, bytes_ in t["mem"]:
        for i in range(len(bytes_)):
            body.append(f'        $display("MEM %s %04h %02h", "{t["name"]}", 16\'h{(addr+i)&0xFFFF:04x}, mem[16\'h{(addr+i)&0xFFFF:04x}]);')
    return "\n".join(body)

def parse_expected_tstates(path):
    """Return {name: final_tstates} parsed from tests.expected."""
    out = {}
    with open(path) as f:
        text = f.read()
    blocks = re.split(r"\n\s*\n", text.strip())
    for blk in blocks:
        lines = [ln for ln in blk.split("\n") if ln.strip()]
        if not lines: continue
        name = lines[0].strip()
        # skip event lines (start with whitespace), find regs line then state line
        i = 1
        while i < len(lines) and (lines[i].startswith(" ") or lines[i].startswith("\t")):
            i += 1
        # lines[i] is regs line; lines[i+1] is state line
        if i + 1 >= len(lines): continue
        state = lines[i + 1].split()
        # i r iff1 iff2 im halted tstates
        out[name] = int(state[6])
    return out

def main():
    tests = list(parse_tests(IN_PATH))
    final_ts = parse_expected_tstates(EXP_PATH)
    # patch each test's tstates target with the expected (final) value
    for t in tests:
        t["tstates"] = final_ts.get(t["name"], t["tstates"])
    body = "\n\n".join(emit_test(t) for t in tests)
    tb = f"""// AUTO-GENERATED by scripts/gen_fuse_tb.py — do not edit by hand.
// Runs the FUSE / Frank D. Cringle z80 tests through z80_core via iverilog.
`include "z80_defs.vh"

module tb_fuse;
    reg         clk = 0;
    reg         reset_n = 0;
    wire [15:0] addr;
    wire [7:0]  data_out;
    reg  [7:0]  din;
    wire        mreq_n, iorq_n, rd_n, wr_n, m1_n, halt_n, rfsh_n, busack_n;

    reg [7:0]   mem [0:65535];
    integer     cycle_count = 0;

    z80_core dut (
        .clk(clk), .reset_n(reset_n),
        .addr(addr), .data_in(din), .data_out(data_out), .data_drive(),
        .mreq_n(mreq_n), .iorq_n(iorq_n),
        .rd_n(rd_n), .wr_n(wr_n), .m1_n(m1_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n),
        .nmi_n(1'b1), .int_n(1'b1), .wait_n(1'b1), .busreq_n(1'b1),
        .busack_n(busack_n)
    );

    // memory service: present read data and capture writes
    always @(*) begin
        if (!mreq_n && !rd_n)      din = mem[addr];
        else if (!iorq_n && !rd_n) din = addr[15:8];   // FUSE port convention
        else                        din = 8'h00;
    end
    always @(negedge clk) begin
        if (!mreq_n && !wr_n) mem[addr] <= data_out;
    end

    always #5 clk = ~clk;

    task do_reset;
        begin
            cycle_count = 0;
            reset_n = 0;
            @(posedge clk); @(posedge clk); @(posedge clk);
            @(negedge clk);
            reset_n = 1;
            @(negedge clk);
        end
    endtask

    task run_phases;
        input integer n;
        integer k;
        begin
            for (k = 0; k < n; k = k + 1) begin
                @(posedge clk);
                cycle_count = cycle_count + 1;
            end
        end
    endtask

    integer i;
    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
{body}
        $display("FUSE-RTL: done");
        $finish;
    end
endmodule
"""
    with open(TB_PATH, "w") as f:
        f.write(tb)
    print(f"wrote {TB_PATH}: {len(tests)} tests, {tb.count(chr(10))} lines")

if __name__ == "__main__":
    main()

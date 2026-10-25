#!/usr/bin/env python3
# ===========================================================================
# gen_fast.py - generate cfast's phase engine from the reference model's
# timing tables (cfast/dump_tables.c prints them from cmodel/).
#
#   gen_fast.py tables.json outdir
#
# Writes outdir/z80_fast_steps.h, the step code cfast's two frames include --
# z80f_tick() (cfast/z80_fast.c, one phase per call) and z80f_run()
# (cfast/z80_fast_run.h, phases back to back with the caller's bus inlined)
# -- and the step enum and dispatch tables: z80_fast_enum.h,
# z80_fast_labels.h (computed goto), z80_fast_cases.h (switch).
#
# A bus-cycle template is a (bus_op, m_len) pair; it has one labelled step
# per phase it produces, T1.P .. Tlast.N, and one for its M-cycle end. An
# M-cycle in which the model writes a register before its last edge (its
# wb_mid(): an IDU step the die makes inside the M-cycle, docs/timing.md
# "Register write-back") runs in a variant of its template, key | 0x80,
# whose step for that edge calls wb_hooks(); the M-cycle's last edge is the
# model's own (end_mcycle). The edges that end an M1's T1 and T2 make the
# writes the last instruction left for them (the die's write-back in the
# next M1; fast mode never sees a RESET cut there), in a copy of the M1's
# T1.P - T3.P steps for each kind of what it left (KINDS below): an
# instruction's end (zf_end_mcycle, cfast/z80_fast.c) jumps to its kind's
# copy, 256 + ZK_*, whose steps make the writes with nothing to test, and
# whose T3.P goes on in the M1's own T3.N; the M1 templates themselves
# test nothing (a prefix's M1 follows nothing pending). A
# step is straight-line code with the phase's constants folded in: the pins
# (the timing ROM, z80_timing), whether it latches data (the bus cycle's
# latch_*(), as do_latch() picks it), samples INT (Tlast.P) or ends the
# refresh (R steps), and the address: the M-cycle's, I:R in a refresh, or
# in the phases the model holds it (its internal T-states, an M1's T5 / T6)
# the one the phase before drove. A phase a held one may follow -- an
# M-cycle's last, an M1's T4.N -- stores its address in c->pins.addr, where
# the held phases (and the reference engine, should it take the next
# phase) find it. Steps of a template follow each other in the text, so a
# frame that does not return between phases falls through them; an M-cycle
# end runs the model's sequencer (end_mcycle) and jumps to the T1.P of the
# template it set up.
#
# A step's phase bookkeeping goes through the frame's macros:
#   ZF_CLOCK(t, phi)     the step produces phase (t, phi) of its M-cycle
#   ZF_MEND(n)           an M-cycle of n T-states has ended
#   ZF_SAVE_BUS()        `bus` (m_addr | m_wdata << 16) was set up for the
#                        M-cycle starting here
# so a frame that returns after every phase can keep the model's cycle,
# t_state and phi current, and one that runs phases back to back can leave
# them implicit in the step it is at (z80_fast_run.h).
#
# A step ends with one of the frame's macros:
#   ZF_PHASE(next)       the phase's pins are in `pins`; `next` produces the
#                        following one (and next - 1 is this step)
#   ZF_PHASE_BOUNDARY(next)  the same, for the T1.P of a kind's copy of an
#                        M1: an instruction has just ended (in fast mode
#                        only there; a T1.P step is only ever entered from
#                        an M-cycle end)
#   ZF_PHASE_CHK(next)   the same after an M1 opcode latch, which leaves
#                        fast mode for a HALT
#   ZF_SLOW_AT(step)     hand this phase to the reference engine
#   ZF_END_MCYCLE()      an M-cycle end: run the sequencer (zf_end_mcycle(),
#                        cfast/z80_fast.c) and continue with the T1.P of the
#                        template it returns: bus_op << 4 | m_len, | 0x80 for
#                        the variant, 256 + ZK_* for a kind's copy of an M1
#
# Fast mode never sees WAIT, BUSREQ, RESET or NMI asserted, a halt, an
# interrupt acknowledge, the instruction an interrupting device hands over
# in IM0, or the NMI response: the frames run those phases through the
# reference z80_phase_step, which shares the state.
# ===========================================================================
import json, os, sys

# What an instruction leaves for the next M1's edges (cfast/z80_fast.c's
# zf_kind[] by its class), and the copy of that M1's T1.P - T3.P steps that
# makes it: the code at T1.P (wbp stated for the model, Q committed where
# its wb_defer() would), on the edge that ends T1 (the T2.P step) and on
# the one that ends T2 (the T3.P step, before the opcode latch). ZK_NONE
# is 0; ZK_MODEL tests wbp on both edges as wb_fire() does, the rare
# classes' (the model's wb_defer() records theirs) and an M1 a resume
# finds with writes pending (cfast/z80_fast.c, resume).
KINDS = [
    ("NONE",     "c->q = 0;",                       None, None),
    ("MODEL",    None, "if (c->wbp & (WBP_F1 | WBP_EXAF | WBP_DEC)) wb_fire(c, 1, false);",
                 "if (c->wbp) wb_fire(c, 2, false);"),
    ("LD_RR",    "c->wbp = WBP_ALU;",               None, "zf_fire_ld_rr(c);"),
    ("LD_RN",    "c->wbp = WBP_ALU;",               None, "zf_fire_ld_rn(c);"),
    ("ALU_R",    "c->wbp = WBP_ALU;",               None, "zf_fire_alu_r(c);"),
    ("ALU_N",    "c->wbp = WBP_ALU;",               None, "zf_fire_alu_n(c);"),
    ("INCDEC_R", "c->wbp = WBP_ALU;",               None, "zf_fire_incdec_r(c);"),
    ("INCDEC_M", "c->wbp = WBP_ALU;",               None, "zf_fire_incdec_m(c);"),
    ("SCF_CCF",  "c->wbp = WBP_ALU;",               None, "zf_fire_scf_ccf(c);"),
    ("ACC",      "c->wbp = WBP_ALU;",               None, "zf_fire_acc(c);"),
    ("NEG",      "c->wbp = WBP_ALU;",               None, "zf_fire_neg(c);"),
    ("CB_R",     "c->wbp = WBP_ALU;",               None, "zf_fire_cb_r(c);"),
    ("EXDH",     "c->q = 0;",                       None, "zf_fire_exdh(c);"),
    ("EXX",      "c->q = 0;",                       None, "zf_fire_exx(c);"),
    ("EXAF",     "c->q = 0;",                       "zf_fire_exaf(c);", None),
    ("F16",      None,                              None, "zf_fire_f16(c);"),
]
M1K_SEQ = [(1, 0), (1, 1), (2, 0), (2, 1), (3, 0)]     # a copy's phases, T1.P - T3.P

PINS = [("m1", "Z80_M1"), ("mreq", "Z80_MREQ"), ("iorq", "Z80_IORQ"),
        ("rd", "Z80_RD"), ("wr", "Z80_WR"), ("rfsh", "Z80_RFSH"), ("doe", "Z80_DOE")]
FAST_OPS = ["M1", "MRD", "MWR", "IORD", "IOWR", "INTERNAL"]
R_STEP = "c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));"
HELD_IR = "(uint16_t)(c->reg_i << 8 | c->reg_r)"    # a refresh's address, before R steps


def main():
    tab = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    busops = tab["busops"]
    ph = {(p["op"], p["t"], p["phi"]): p for p in tab["phases"]}
    # the M-cycles with a register write inside them: template -> the T-state
    # whose end edge makes it
    mid = {(m["op"], m["len"]): m["t"] for m in tab["wb_mid"]}

    templates = []
    for name in FAST_OPS:
        op, base = busops[name]
        for n in range(max(1, base), min(base + 8, 16)):
            templates.append((name, op, n, None))
            if (name, n) in mid:
                templates.append((name, op, n, mid.pop((name, n))))
    assert not mid, f"wb_mid templates fast mode doesn't run: {mid}"

    enum, code, t1p = ["FS_NONE = 0"], [], []
    tphi = [(0, 0)]                     # (t, phi) of the phase each step produces

    def step(name, op, n, hook, t, phi, wb, boundary=False):
        """The body of the step that produces phase (t, phi) of template
        (name, n, hook), and the macro that ends it; wb: the kind's code at
        T1.P and on the edges that end T1 and T2 (an M1's); boundary: a
        kind's copy, whose T1.P follows an instruction's end"""
        p = ph[(op, t, phi)]
        body = []
        if (t, phi) == (1, 0):
            # entered from an M-cycle end: start_mcycle set t_state / phi;
            # a held cycle keeps the address the last one drove
            a = "c->pins.addr" if p["held"] else "c->m_addr"
            body.append(f"bus = {a} | (uint64_t)c->m_wdata << Z80_PIN_D0; ZF_SAVE_BUS();")
            if wb[0]:
                body.append(wb[0])
        else:
            body.append(f"ZF_CLOCK({t}, {phi});")
            if phi == 0 and t in (2, 3) and wb[t - 1]:
                # the edge that ends T1 / T2: the last instruction's writes due there
                body.append(wb[t - 1])
            if phi == 0 and hook == t - 1:
                # the edge that ends T(t-1): the die's register write in it
                body.append(f"wb_hooks(c, {hook});")
            pp = ph[(op, t, 0)] if phi else ph[(op, t - 1, 1)]   # the phase before
            assert p["held"] or not pp["held"], (name, n, t, phi)
            if p["held"] and not pp["held"]:
                # the first held phase: what the phase before drove stays
                body.append("bus = (bus & ~0xFFFFull) | c->pins.addr; ZF_SAVE_BUS();")
            if phi == 0 and ph[(op, t - 1, 1)]["refresh_end"]:
                body.append(R_STEP)
        chk = False
        if p["latch"]:
            latch = {"M1": "zf_latch_m1", "MRD": "latch_rd", "IORD": "latch_rd"}[name]
            body.append(f"c->pins.data_in = Z80_GET_DATA(pins); {latch}(c);")
            if name == "M1":
                body.append("if (c->ctl.exec == EXEC_HALT) f->fast = false;")
                chk = True
        if t == n and phi == 0:
            body.append("c->int_sampled = (pins & Z80_INT) != 0;")
        bits = " | ".join(m for k, m in PINS if p[k]) or "0"
        if p["refresh"]:
            busw = "(bus & ~0xFFFFull) | (uint16_t)(c->reg_i << 8 | c->reg_r)"
        else:
            busw = "bus"
        nxt = (t, 1) if phi == 0 else (t + 1, 0)
        if not p["held"] and (nxt[0] > n or ph[(op, *nxt)]["held"]):
            # a held phase may follow: its address stays on the bus
            body.append(f"c->pins.addr = {HELD_IR if p['refresh'] else '(uint16_t)bus'};")
        body.append(f"pins = (pins & ~Z80_OUT_PIN_MASK) | {bits} | {busw};")
        mac = ("ZF_PHASE_BOUNDARY" if boundary and (t, phi) == (1, 0) else
               "ZF_PHASE_CHK" if chk else "ZF_PHASE")
        return body, mac

    m1op = busops["M1"][0]
    model_wb = next(k[1:] for k in KINDS if k[0] == "MODEL")
    for name, op, n, hook in templates:
        tid = f"{name}_{n}" + ("_W" if hook else "")
        seq = [(t, phi) for t in range(1, n + 1) for phi in (0, 1)]
        ids = [f"FS_{tid}_T{t}{'PN'[phi]}" for t, phi in seq] + [f"FS_{tid}_END"]
        enum += ids
        tphi += seq + [(n + 1, 0)]      # the M-cycle end produces the next T1.P
        t1p.append(((op << 4 | n) | (0x80 if hook else 0),
                    f"BUSOP_{name} << 4 | {n}" + (" | 0x80" if hook else ""), ids[0]))
        # an M1 of 4 T-states follows an instruction in its kind's copy (below)
        # or a prefix, with nothing pending; a longer one tests (fast mode
        # doesn't run them)
        wb = (None, None, None) if name != "M1" or n == 4 else model_wb
        for i, (t, phi) in enumerate(seq):
            body, mac = step(name, op, n, hook, t, phi, wb)
            body.append(f"{mac}({ids[i + 1]});")
            what = f"{name} m_len {n}{' (wb_mid)' if hook else ''}"
            code.append(f"ZF_LABEL({ids[i]}): /* {what}: T{t}.{'PN'[phi]} */\n    "
                        + "\n    ".join(body))
        # the M-cycle end: the sequencer, then the T1.P of what it set up (at
        # an instruction's end, its kind's copy of the next M1's first steps)
        body = [f"if (c->int_sampled && c->iff1) ZF_SLOW_AT({ids[-1]});  /* INT accept */",
                f"ZF_MEND({n});"]
        if ph[(op, n, 1)]["refresh_end"]:
            body.append(R_STEP)
        body.append("ZF_END_MCYCLE();")
        code.append(f"ZF_LABEL({ids[-1]}): /* {name} m_len {n}{' (wb_mid)' if hook else ''}: "
                    "M-cycle end */\n    " + "\n    ".join(body))

    # the copies of an M1's T1.P - T3.P by what the last instruction left
    # (KINDS); the T3.P step goes on in the M1's own T3.N
    for kx, (kname, *wb) in enumerate(KINDS):
        ids = [f"FS_M1K_{kname}_T{t}{'PN'[phi]}" for t, phi in M1K_SEQ]
        enum += ids
        tphi += M1K_SEQ
        t1p.append((256 + kx, f"256 + ZK_{kname}", ids[0]))
        for i, (t, phi) in enumerate(M1K_SEQ):
            body, mac = step("M1", m1op, 4, None, t, phi, wb, boundary=True)
            if i + 1 < len(ids):
                body.append(f"{mac}({ids[i + 1]});")
            else:
                body.append(f"{mac}(FS_M1_4_T3N);")
                body.append("ZF_JUMP(FS_M1_4_T3N);")
            code.append(f"ZF_LABEL({ids[i]}): /* M1 m_len 4 after ZK_{kname}: "
                        f"T{t}.{'PN'[phi]} */\n    " + "\n    ".join(body))

    hdr = ("/* generated by cfast/gen_fast.py from the reference model's timing "
           "tables -- do not edit */\n")
    lab = {key: s for key, _, s in t1p}
    files = {
        # the steps and their order
        "z80_fast_enum.h":
            "enum {\n" + "".join(f"    {e},\n" for e in enum) + "    FS_COUNT\n};\n"
            + "/* what an instruction leaves for the next M1 (gen_fast.py's KINDS) */\n"
            + "enum {\n" + "".join(f"    ZK_{k[0]},\n" for k in KINDS) + "    ZK_COUNT\n};\n"
            + f"#define ZF_M1K_STEPS {len(M1K_SEQ)}   /* the steps of a kind's M1 copy */\n"
            + "/* the T1.P step of each template, by bus_op << 4 | m_len (| 0x80: the\n"
            + "   wb_mid variant), and of the M1 copies, by 256 + ZK_* */\n"
            + "static const uint16_t zf_template[256 + ZK_COUNT] = {\n"
            + "".join(f"    [{k}] = {s},\n" for _, k, s in t1p) + "};\n"
            + "/* the phase each step produces: T-state, phi, and its offset in phases\n"
            + "   from the M-cycle's T1.P (an M-cycle end: the next T1.P) */\n"
            + "static const uint8_t zf_step_t[FS_COUNT] = {"
            + ",".join(str(t) for t, _ in tphi) + "};\n"
            + "static const uint8_t zf_step_phi[FS_COUNT] = {"
            + ",".join(str(p) for _, p in tphi) + "};\n"
            + "static const uint8_t zf_step_off[FS_COUNT] = {"
            + ",".join(str(2 * (t - 1) + p if t else 0) for t, p in tphi) + "};\n",
        # label address of every step (computed goto)
        "z80_fast_labels.h": "".join(f"    [{e}] = &&{e},\n" for e in enum[1:]),
        # label address of each template's T1.P, by bus_op << 4 | m_len (| 0x80),
        # every key (none: zf_bad)
        "z80_fast_t1p_labels.h": "".join(
            f"    [{k}] = &&{lab.get(k, 'zf_bad')},\n" for k in range(256 + len(KINDS))),
        # switch dispatch to every step (without computed goto)
        "z80_fast_cases.h": "".join(f"    case {e}: goto {e};\n" for e in enum[1:]),
        # the steps
        "z80_fast_steps.h": "\n".join(code) + "\n",
    }
    for name, text in files.items():
        with open(os.path.join(out, name), "w") as f:
            f.write(hdr + text)
    print(f"gen_fast: {len(templates)} templates, {len(enum) - 1} steps")


if __name__ == "__main__":
    main()

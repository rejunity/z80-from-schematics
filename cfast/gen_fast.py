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
# per phase it produces, T1.P .. Tlast.N, and one for its M-cycle end. A
# step is straight-line code with the phase's constants folded in: the pins
# (the timing ROM, z80_timing), whether it latches data (do_latch), samples
# INT (Tlast.P) or ends the refresh (R steps). Steps of a template follow
# each other in the text, so a frame that does not return between phases
# falls through them; an M-cycle end runs the model's sequencer
# (end_mcycle) and jumps to the T1.P of the template it set up.
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
#   ZF_PHASE_T1P(next)   the same, for a T1.P (instruction boundaries
#                        happen just before one)
#   ZF_PHASE_CHK(next)   the same after an M1 opcode latch, which leaves
#                        fast mode for a HALT
#   ZF_SLOW_AT(step)     hand this phase to the reference engine
#   ZF_GOTO_T1P()        continue with the T1.P of (c->bus_op, c->m_len)
#
# Fast mode never sees WAIT, BUSREQ, RESET or NMI asserted, a halt, an
# interrupt acknowledge or the NMI response: the frames run those phases
# through the reference z80_phase_step, which shares the state.
# ===========================================================================
import json, os, sys

PINS = [("m1", "Z80_M1"), ("mreq", "Z80_MREQ"), ("iorq", "Z80_IORQ"),
        ("rd", "Z80_RD"), ("wr", "Z80_WR"), ("rfsh", "Z80_RFSH"), ("doe", "Z80_DOE")]
FAST_OPS = ["M1", "MRD", "MWR", "IORD", "IOWR", "INTERNAL"]
R_STEP = "c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));"


def main():
    tab = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    busops = tab["busops"]
    ph = {(p["op"], p["t"], p["phi"]): p for p in tab["phases"]}

    templates = []
    for name in FAST_OPS:
        op, base = busops[name]
        for n in range(max(1, base), min(base + 8, 16)):
            templates.append((name, op, n))

    enum, code, t1p = ["FS_NONE = 0"], [], []
    tphi = [(0, 0)]                     # (t, phi) of the phase each step produces
    for name, op, n in templates:
        tid = f"{name}_{n}"
        seq = [(t, phi) for t in range(1, n + 1) for phi in (0, 1)]
        ids = [f"FS_{tid}_T{t}{'PN'[phi]}" for t, phi in seq] + [f"FS_{tid}_END"]
        enum += ids
        tphi += seq + [(n + 1, 0)]      # the M-cycle end produces the next T1.P
        t1p.append((f"BUSOP_{name} << 4 | {n}", ids[0]))

        for i, (t, phi) in enumerate(seq):
            p = ph[(op, t, phi)]
            body = []
            if (t, phi) == (1, 0):
                # entered from an M-cycle end: start_mcycle set t_state / phi
                body.append("bus = c->m_addr | (uint64_t)c->m_wdata << Z80_PIN_D0; ZF_SAVE_BUS();")
            else:
                body.append(f"ZF_CLOCK({t}, {phi});")
                if phi == 0 and ph[(op, t - 1, 1)]["refresh_end"]:
                    body.append(R_STEP)
            chk = False
            if p["latch"]:
                body.append("c->pins.data_in = Z80_GET_DATA(pins); do_latch(c);")
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
            body.append(f"pins = (pins & ~Z80_OUT_PIN_MASK) | {bits} | {busw};")
            mac = "ZF_PHASE_T1P" if (t, phi) == (1, 0) else "ZF_PHASE_CHK" if chk else "ZF_PHASE"
            body.append(f"{mac}({ids[i + 1]});")
            code.append(f"ZF_LABEL({ids[i]}): /* {name} m_len {n}: T{t}.{'PN'[phi]} */\n    "
                        + "\n    ".join(body))
        # the M-cycle end: the sequencer, then the T1.P of what it set up
        body = [f"if (c->int_sampled && c->iff1) ZF_SLOW_AT({ids[-1]});  /* INT accept */",
                f"ZF_MEND({n});"]
        if ph[(op, n, 1)]["refresh_end"]:
            body.append(R_STEP)
        body += ["c->pins.busreq_n = 1;   /* this phase's level, for begin_next */",
                 "end_mcycle(c);", "ZF_GOTO_T1P();"]
        code.append(f"ZF_LABEL({ids[-1]}): /* {name} m_len {n}: M-cycle end */\n    "
                    + "\n    ".join(body))

    hdr = ("/* generated by cfast/gen_fast.py from the reference model's timing "
           "tables -- do not edit */\n")
    files = {
        # the steps and their order
        "z80_fast_enum.h":
            "enum {\n" + "".join(f"    {e},\n" for e in enum) + "    FS_COUNT\n};\n"
            + "/* the T1.P step of each template, by bus_op << 4 | m_len */\n"
            + "static const uint16_t zf_template[128] = {\n"
            + "".join(f"    [{k}] = {s},\n" for k, s in t1p) + "};\n"
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
        # label address of each template's T1.P, by bus_op << 4 | m_len
        "z80_fast_t1p_labels.h": "".join(f"    [{k}] = &&{e},\n" for k, e in t1p),
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

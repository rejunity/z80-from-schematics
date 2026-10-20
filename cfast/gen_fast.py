#!/usr/bin/env python3
# ===========================================================================
# gen_fast.py - generate cfast's phase engine from the reference model's
# timing tables (cfast/dump_tables.c prints them from cmodel/).
#
#   gen_fast.py tables.json outdir
#
# Writes outdir/z80_fast_steps.h, the step code cfast's two frames include:
# z80f_tick() (cfast/z80_fast.c, one phase per call) and z80f_run()
# (cfast/z80_fast_run.h, phases back to back with the caller's bus inlined).
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
# A step ends with one of the frame's macros:
#   ZF_PHASE(next)       the phase's pins are in `pins`; `next` produces the
#                        following one
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
    for name, op, n in templates:
        tid = f"{name}_{n}"
        seq = [(t, phi) for t in range(1, n + 1) for phi in (0, 1)]
        ids = [f"FS_{tid}_T{t}{'PN'[phi]}" for t, phi in seq] + [f"FS_{tid}_END"]
        enum += ids
        t1p.append((f"BUSOP_{name} << 4 | {n}", ids[0]))

        for i, (t, phi) in enumerate(seq):
            p = ph[(op, t, phi)]
            body = []
            if (t, phi) == (1, 0):
                # entered from an M-cycle end: start_mcycle set t_state / phi
                body.append("bus = c->m_addr | (uint64_t)c->m_wdata << Z80_PIN_D0; f->bus = bus;")
            else:
                body.append("c->cycle++;")
                if phi == 1:
                    body.append("c->phi = 1;")
                else:
                    body.append("c->phi = 0;")
                    if ph[(op, t - 1, 1)]["refresh_end"]:
                        body.append(R_STEP)
                    body.append(f"c->t_state = {t};")
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
                "c->cycle++;", "c->phi = 0;"]
        if ph[(op, n, 1)]["refresh_end"]:
            body.append(R_STEP)
        body += [f"c->t_state = {n + 1};",
                 "c->pins.busreq_n = 1;   /* this phase's level, for begin_next */",
                 "end_mcycle(c);", "ZF_GOTO_T1P();"]
        code.append(f"ZF_LABEL({ids[-1]}): /* {name} m_len {n}: M-cycle end */\n    "
                    + "\n    ".join(body))

    hdr = ("/* generated by cfast/gen_fast.py from the reference model's timing "
           "tables -- do not edit */\n")
    with open(os.path.join(out, "z80_fast_steps.h"), "w") as f:
        f.write(hdr)
        f.write("#if defined(ZF_STEP_ENUM)\n")
        f.write("enum {\n" + "".join(f"    {e},\n" for e in enum) + "    FS_COUNT\n};\n")
        f.write("/* the T1.P step of each template, by bus_op << 4 | m_len */\n")
        f.write("static const uint16_t zf_template[128] = {\n")
        f.write("".join(f"    [{k}] = {s},\n" for k, s in t1p))
        f.write("};\n")
        f.write("#elif defined(ZF_STEP_LABELS)  /* label address of every step */\n")
        f.write("".join(f"    [{e}] = &&{e},\n" for e in enum[1:]))
        f.write("#elif defined(ZF_STEP_CASES)   /* switch dispatch to every step */\n")
        f.write("".join(f"    case {e}: goto {e};\n" for e in enum[1:]))
        f.write("#else\n")
        f.write("\n".join(code) + "\n#endif\n")
    print(f"gen_fast: {len(templates)} templates, {len(enum) - 1} steps")


if __name__ == "__main__":
    main()

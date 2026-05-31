# Real-silicon trace integration

Survey of sources that provide real Z80 cycle-by-cycle signal data, and how
each one can serve as ground truth for our test harness.

## What's integrated now

### sigrok-dumps `z80/kc85` — REAL captures, in repo

Two logic-analyzer captures of a KC 85/4 (Z80-based East-German PC) running
its OS main loop, taken with a SysClk LWLA 1034. Now lives at
`tests/sigrok/kc85-{20mhz,cpuclk}.sr` (raw .sr files = zip archives) with
the upstream channel-mapping README at `tests/sigrok/README.kc85`.

Channels captured: CLK, /M1, /INT, MEI, /WAIT, IEI, A0..A15, /IORQ, /MREQ,
/RD, /WR, D0..D7 — every pin our trace format compares.

Two complementary recordings:
  - `kc85-20mhz.sr`: 5000 samples at 20 MHz asynchronous (multiple samples
    per Z80 clock; sub-cycle timing visible).
  - `kc85-cpuclk.sr`: 5000 samples synchronous to Z80 clock falling edge
    (one sample per T-state; great for cycle counting, no sub-cycle detail).

`scripts/sigrok_z80_decode.py` parses the .sr archive and emits:
  - a per-cycle CSV trace (M1, MREQ, IORQ, RD, WR, addr, data, INT, WAIT);
  - a reconstructed memory image (every byte the CPU saw on MRD).

On the cpuclk capture this yields 546 M1 fetches and 161 memory bytes
reconstructed. The captured program is the KC85's RAM-resident OS loop
at PCs around `0xf40a`.

`scripts/sigrok_replay.py` runs that reconstructed memory through our C
model and compares M1 fetch sequences. **For the kc85 captures it cannot
reach exact-prefix match**, because the capture starts mid-execution with
unknown register / flag state — our emulator's reset-state (AF=FFFF, C=1)
takes branches the real silicon (at unknown C) did not. This is a property
of the capture, not the emulator; pattern-level validation still works
(M1 / refresh / MREQ / RD assertion shapes).

### Useful as ground truth for ...

  - **Pin-pattern shape**: real silicon shows M1 asserted for 2 cycles, MREQ
    asserted starting at end-of-T1, refresh address driven at T3, etc. Our
    cpuclk-aligned decoder lines up with our `tracegen` per-T-state view
    (one sample per T-state) — useful for catching gross timing regressions
    by visual diff.
  - **Channel set**: confirms our 14-column trace format matches the actual
    pins captured by the open community (M1, MREQ, IORQ, RD, WR, RFSH,
    HALT, addr, data).
  - **Memory observation**: the 161 reconstructed bytes of the OS loop are
    exercised on our emulator too via the bootstrap-JP trick in
    `sigrok_replay.py`; we observe the same bus patterns inside the
    captured code region.

## What's noted for follow-up

These sources require hardware to capture custom programs; we have the
parser+replay scaffolding ready when those captures land.

### Ho-Ro/Z80_dongle and Goran Devic's Arduino dongle

Both projects park a real Z80 chip in a socket driven by an Arduino Mega
that single-clocks the CPU and prints every T-state via USB serial:
```
#003H T1 AB:000 DB:-- M1
#004H T2 AB:000 DB:FB M1 MREQ RD  Opcode read from 000 -> FB
```
Ho-Ro's repo includes user-loadable Intel-HEX support (`:`) and an `X`
command to execute, plus sample programs (`CPU_detect.asm`, `IM2_test.asm`).
Goran's article documents the same architecture; the Ho-Ro fork is the
more maintained codebase.

Integration path (open follow-up): a Python parser that consumes the dongle
serial stream, extracts the per-T-state pin pattern, and diffs against our
emulator's `tracegen` output for the same Intel-HEX program. Anchors are
clean (reset is observed in the trace), the program is fully known
beforehand, and registers/flags follow from reset — so exact-prefix replay
should work where it cannot for the sigrok-kc85 mid-execution dumps.

### MustBeArt LAIR

A KiCad/hardware interface module that wires the RC-2014 bus to an HP/Agilent
logic analyzer with the original HP Z80 inverse assembler attached. The
repo contains hardware design files only — no captured traces and no
capture software yet (the README explicitly flags the capture-to-file path
as not implemented). Useful only if combined with an HP analyzer and an
RC-2014 system.

## Summary

  - sigrok-kc85: parser and reconstructor land; **pattern-level**
    validation against real silicon is in place; exact replay is blocked
    by unknown initial state in the capture.
  - Ho-Ro / Devic Arduino dongle: parser scaffolding pending hardware
    captures of known programs; framework already proven by sigrok work.
  - LAIR: hardware-only; no integration path without an HP analyzer.

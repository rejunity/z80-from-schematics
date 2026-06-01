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
model and compares M1 fetch sequences. **Exact replay** can't reach
prefix match (capture starts mid-execution; reset-state flags differ
from the silicon's actual flag state in the middle of the OS loop), but
two derived tests that DON'T need initial register state still extract
real value:

### `make silicon_cycles` — per-opcode T-state oracle

`scripts/sigrok_opcode_cycles.py` walks the cpuclk capture, finds every
M1 fetch (`/M1` 0→1 transition), measures the T-state count from that
M1 to the next M1 (one sample-per-T-state synchronous capture), and
buckets the result per opcode. Then it runs each opcode through
`tracegen` in a clean PC=0x0100 sandbox and compares the count against
both the spec and the real-silicon observation.

Result on `tests/sigrok/kc85-cpuclk.sr` (KC85 OS loop, 53 unique opcodes,
546 M1 fetches):

  - **45/50 classified opcodes**: emulator T-state = spec T-state = real
    silicon T-state. ✅
  - **0 emulator mismatches.** Every opcode our emulator measures hits
    a spec-allowed value.
  - 3 unclassified prefix bytes (CB / ED / DD; seen 41/1/38 times) —
    skipped, since the spec count belongs to the (prefix, op) pair.
  - 5 opcodes show real silicon taking MORE T-states than the spec:

    | opcode | real | spec | likely cause |
    |---|---|---|---|
    | `00` NOP | 8T | 4T | KC85 hardware /WAIT or M1 deferred ack |
    | `0b` DEC BC | 6T | 4T | system-bus arbitration adds a clock |
    | `2a` LD HL,(nn) | 17T | 16T | +1 WAIT |
    | `3a` LD A,(nn) | 13T or 14T | 13T | +1 WAIT on second-byte fetch |
    | `5b` LD E,L | 18T | 4T | a captured interrupt acceptance folded in |

    These are KC85-system artifacts (WAIT injection on certain memory
    regions, interrupts taken mid-instruction) — NOT Z80-core bugs.

Conditional opcodes show consistent timing both branches: `JR C,d`
(0x38) 41× not-taken (7T); `JR NZ,d` (0x20) 38× taken (12T) + 2×
not-taken (7T). Both within spec; our emulator's reset-state selections
(12T for 0x38 taken, 7T for 0x20 not-taken) are also spec-compliant.

### Other usable cross-checks

  - **Pin-pattern shape**: real silicon shows M1 asserted for 2 cycles,
    MREQ asserted from end-of-T1, refresh address driven at T3 — our
    cpuclk-aligned decoder lines up with our `tracegen` per-T-state
    view, useful for catching gross timing regressions.
  - **Channel set**: confirms our 14-column trace format matches the
    actual pins captured by the open community.
  - **Memory observation**: 161 OS-loop bytes reconstructed; the opcodes
    at those PCs cross-check against our emulator's fetch behaviour.

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

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

  - **50/50 classified opcodes: emulator T-state = spec = real silicon. ✅**
  - **0 emulator mismatches.** Every opcode our emulator measures hits
    a spec-allowed value.
  - 3 unclassified prefix bytes (CB / ED / DD; seen 41/1/38 times) —
    skipped, since the spec count belongs to the (prefix, op) pair.
  - 4 opcodes (`00`, `2a`, `3a`, `5b` plus occasional `77`/`7e`) showed
    apparent excess T-states that the script tags **OK (excess
    attributed to /WAIT)** because /WAIT was sampled active during
    those instruction windows. /WAIT is a captured channel (CH5 in the
    LWLA-1034 mapping); the analyzer tracks it per instruction and
    counts a contiguous WAIT-active sample as a Tw extension.
  - 1 opcode (`0b` DEC BC) showed 6T consistently across 39 captured
    samples with NO /WAIT activity. Both the real silicon and our
    emulator agree on 6T — and a check against Sean Young's
    "Undocumented Z80 Documented" Appendix A confirmed `INC/DEC rp`
    really is **6T** (4T M1 + 2T internal increment), not 4T. The
    initial spec table inside `sigrok_opcode_cycles.py` had this row
    wrong; the kc85 capture surfaced the doc bug. Now fixed.

Conditional opcodes show consistent timing both branches: `JR C,d`
(0x38) 41× not-taken (7T); `JR NZ,d` (0x20) 38× taken (12T) + 2×
not-taken (7T). Both within spec; our emulator's reset-state selections
(12T for 0x38 taken, 7T for 0x20 not-taken) are also spec-compliant.

The /WAIT line itself is treated rigorously by the analyzer:
  - WAITn=0 samples are counted per instruction window and shown in
    the per-opcode T-state histogram as `(+Tw)`.
  - Verdict is "OK (excess attributed to /WAIT)" when an observation
    exceeds spec but /WAIT was asserted during that window.
  - Verdict is "silicon system artifact" only when the excess has
    NO /WAIT to explain it (zero such cases on the kc85 captures).

### `make silicon_async` — 20 MHz async capture: CPU clock + sub-T-state pin timing

`scripts/sigrok_async_timing.py` parses the asynchronous 20 MHz capture
(`tests/sigrok/kc85-20mhz.sr`, ~11 samples per T-state). It extracts
three things the cpuclk-synchronous capture can't show, and
independently re-validates the per-opcode T-state count.

#### Headline timing measurements

| Measurement | Real KC85 silicon | Z80 spec | Emulator |
|---|---|---|---|
| CPU clock period | avg **565.8 ns** (550–800 ns range, **0 WAIT samples** — period is intrinsic, not Tw-extended) | — | n/a (host-clocked) |
| Implied CPU frequency | **~1.767 MHz** | matches KC85/4 1.75–1.77 MHz spec | — |
| M1n deassert offset | **64%** into T-state | end-of-T2 → T3 | T2.N → T3.P ✓ |
| MREQn assert / deassert | **9% / 64%** into T-state | T1.N / T2.N | matches our PHI_N model ✓ |
| RDn assert / deassert | **9% / 64%** | T1.N / T2.N | ✓ |
| WRn assert | **9%** (only on writes) | T2.N | ✓ |

#### Where each measurement comes from

- **CPU clock period.** The .sr file's metadata declares a 20 MHz sample
  rate (= 50 ns / sample). The script decodes every 5-byte (40-bit)
  sample into the 34 named channels, then walks the **CLK** channel
  collecting 1→0 transitions. Over the 5000-sample capture we observe
  **441 falling edges** with an inter-edge delta of **11.3 samples
  on average** (min 11, max 16), so the period is
  `11.3 × 50 ns = 565.8 ns`. Frequency = 1 / period.
- **Implied CPU frequency.** Published KC85/4 spec is PCK = 1.7734 MHz
  (sometimes quoted as 1.75 MHz nominal); our measured 1.767 MHz lands
  inside that tolerance. The 250 ns excursion at the max end of the
  period range is consistent with the system /WAIT insertion already
  observed by the synchronous capture on opcode `0x0b` (DEC BC, 6 T
  on real silicon vs 4 T per spec).
- **Pin transition offsets.** For each consecutive pair of CLK falling
  edges `[clk_falls[i], clk_falls[i+1]]` (one T-state window) the
  script scans the **M1n**, **MREQn**, **RDn**, **WRn** channels for
  1↔0 transitions and bins their sample-offset within the window. The
  modes across all 441 windows fall at sample offsets 1 and 7 of the
  11-sample period (≈ 9 % and 64 % of a T-state). MREQn / RDn assert
  shortly after T1's falling edge (offset 1) and all three control
  pins (M1n, MREQn, RDn) deassert near the T2/T3 boundary (offset 7).
  WRn is observed at offset 1 only (it asserts on T2.N for memory
  writes; there's no symmetric deassert in the captured window
  because the capture caught very few WRs).
- **"Emulator" column.** Cross-references our two-phase `PHI_P` /
  `PHI_N` model (`cmodel/z80.c`, `cmodel/z80_timing.c`). PHI_N is the
  T-state's second half — the Z80 clock falling-edge region — where
  MREQ/RD assert during an MRD cycle and M1 deasserts at the T2.N
  edge. Those are exactly the offsets the LWLA observed on real silicon.

#### Async-derived per-opcode T-state cross-check

After measuring the clock period, the script **re-samples the
async stream at every CLK falling edge** — exactly what the LWLA's
external-clock mode does in hardware — to produce a synthetic
cpuclk-sync trace. It then runs the same per-opcode T-state check
on it:

```
Classified: 9 opcodes, 8 OK, 0 emulator mismatches,
            1 silicon out-of-spec (the same 0x0b WAIT artifact)
```

Two independent capture modes (true async + on-CLK-edge sync) of the
same KC85, two independent extractors (`sigrok_opcode_cycles.py`,
`sigrok_async_timing.py`), and they reach the same verdict.

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

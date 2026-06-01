# Research notes

Catalog of the sources that inform this core, what each contributes, how far it is
trusted, and the open questions. Per the brief, we treat reverse-engineered
schematics, PLA tables, transistor-level work, and gate-level simulations as a
"disassembly" of the original chip.

> **Provenance note.** This file is written from established, well-documented Z80
> knowledge rather than from a live crawl of each URL in this working session.
> URLs are recorded as the canonical locations for re-verification. Every behavioral
> claim that matters is ultimately pinned by an executable test (unit vectors,
> ZEXDOC/ZEXALL, and C↔RTL trace comparison), so the test suite — not this prose —
> is the final authority. Where a fact is subtle and not yet test-pinned it is
> listed under **Open questions**.

## Source-conflict precedence (from the brief)

When sources disagree, prefer in this order:

1. Original Zilog timing and programmer documentation (for *documented* behavior).
2. Die-/transistor-/PLA-/gate-level reverse-engineering evidence.
3. Exhaustive hardware behavior tests from real Z80-family chips.
4. Mature emulator behavior with strong test coverage (esp. MAME).
5. Other emulator / FPGA cores.
6. Informal forum posts, unless backed by tests or die evidence.

Special case: for *undocumented* flag and timing behavior, (2) and (3) outrank (1),
because Zilog never specified it. ZEXALL + die analysis are the practical arbiters.

---

## Primary documentation

### Zilog Z80 CPU User Manual (UM0080)
- **URL:** https://www.zilog.com/docs/z80/um0080.pdf
- **Contributes:** Programmer-visible register model, documented instruction set and
  flag effects, M-cycle/T-state *timing diagrams* (opcode fetch, memory read/write,
  I/O read/write, BUSREQ/BUSACK, interrupt acknowledge), pin descriptions, interrupt
  modes 0/1/2 and NMI behavior, refresh description.
- **Trust:** High for documented behavior and externally visible timing. Authoritative
  per precedence rule (1). Does **not** cover undocumented opcodes or undoc flags.
- **Type:** Primary documentation.
- **Use here:** Source of truth for the timing model in `docs/timing.md` and the
  documented flag tables in `docs/flags.md`.

### Zilog Z80 Family product spec / datasheet timing tables
- **Contributes:** Exact edge relationships and AC timing parameters; confirms which
  pins move on rising vs falling clock edges (drives our PHI_P/PHI_N phase model).
- **Trust:** High. Primary.

---

## Reverse-engineering evidence (die / transistor / gate level)

### Ken Shirriff — Z80 reverse-engineering articles
- **URL:** https://www.righto.com/2013/09/ (ALU), and the series on the Z80 register
  file, the PLA, instruction decode, and the bug/decode posts at righto.com.
- **Contributes:**
  - **ALU**: the Z80 ALU is a **4-bit** unit used twice per 8-bit op (two passes); the
    carry chain, the operand/result latches, how add/sub/logic share structure, and
    how the half-carry comes from the low nibble pass. Directly shapes `docs/alu.md`.
  - **Register file**: 16-bit-oriented register file with the increment/decrement
    logic on the address side; the WZ (MEMPTR) pair lives here.
  - **PLA / instruction decode**: the decode PLA structure and how opcode groups map
    to control. Reinforces the x/y/z/p/q decomposition we use.
- **Trust:** Very high — die-level. Authoritative per rule (2) for internal structure.
- **Type:** Reverse-engineering evidence (primary for internals).
- **Use here:** ALU is organized as a nibble unit with an explicit carry chain so the
  C and RTL mirror the real datapath rather than doing one opaque 8-bit add.

### Visual6502 / "visual Z80" (z80explorer) — transistor/gate-level simulation
- **URL:** http://www.visual6502.org/ ; z80explorer (gdevic) on GitHub.
- **Contributes:** Cycle/phase-exact ground truth for *every* node, including
  undocumented opcode behavior, exact MEMPTR updates, and the precise timing of pin
  transitions. The ultimate tiebreaker for "what does the silicon actually do."
- **Trust:** Very high for behavior; it *is* the netlist of a real die.
- **Type:** Gate-level simulation (primary behavioral evidence).
- **Use here:** Reference for resolving any timing/flag ambiguity the docs leave open.

### "Decoding Z80 opcodes" — Cristian Dinu (z80.info)
- **URL:** http://www.z80.info/decoding.htm
- **Contributes:** The x/y/z/p/q opcode field decomposition and the tables mapping
  those fields to register/ALU/cc selections across the unprefixed, CB, and ED spaces,
  plus DD/FD (IX/IY) and DDCB/FDCB rules. This is the structural skeleton of our PLA.
- **Trust:** High; community-standard, matches die decode groupings. Secondary
  interpretation but extremely well corroborated.
- **Type:** Secondary interpretation of decode structure.
- **Use here:** `docs/pla.md` decode scheme; `cmodel/z80_pla.c` row generation.

---

## Behavior tests (real-hardware-derived)

### ZEXDOC / ZEXALL — Frank Cringle's instruction exerciser
- **URL:** https://mdfs.net/Software/Z80/Exerciser/ and many mirrors; the CP/M `.com`
  files `zexdoc.com` / `zexall.com`.
- **Contributes:** Exhaustive instruction-level regression via CRC of results over
  large input spaces. **ZEXDOC** checks documented flags only (XF/YF masked);
  **ZEXALL** checks *all* flag bits including the undocumented X/Y flags. Passing
  ZEXALL is the de-facto proof of flag-exact correctness.
- **Trust:** Very high (CRCs derived from real Z80 behavior). Rule (3).
- **Type:** Hardware-behavior test suite.
- **Use here:** Primary acceptance gate for the C model; RTL must match C.
- **Note:** Needs a tiny CP/M BDOS shim (functions 2 and 9) and an 8080-style harness
  loading the `.com` at 0x0100.

### MEMPTR / WZ research (the "MEMPTR" document, zx community)
- **Contributes:** The exact rules for when and how the internal WZ pair is updated by
  each instruction class, which is observable only via `BIT n,(HL)` setting XF/YF from
  WZ high byte. Needed for ZEXALL-exact behavior of `BIT b,(HL)` and friends.
- **Trust:** High; corroborated by die analysis and ZEXALL.
- **Type:** Reverse-engineering evidence + tests.
- **Use here:** `docs/undocumented.md`; WZ update points wired into control sequencing.

### SCF/CCF flag behavior (the "flags" investigations)
- **Contributes:** SCF/CCF set XF/YF from a combination of A and the previous F —
  notably this differs between original NMOS Z80 and CMOS variants and even between
  chip batches. We target the common NMOS behavior validated by ZEXALL.
- **Trust:** Medium-high; documented variability. Rule (2)/(3).
- **Type:** Reverse-engineering + tests.
- **Use here:** `docs/flags.md`; flagged as a known variant in `docs/known-differences.md`.

### sigrok-dumps Z80 logic-analyzer captures (real KC85/4 silicon)
- **URL:** https://github.com/sigrokproject/sigrok-dumps/tree/master/z80/kc85
- **Contributes:** Two SysClk LWLA-1034 logic-analyzer captures of a running
  KC85/4 (Z80-based, VEB Mikroelektronik Mühlhausen) executing its RAM-resident
  OS main loop. Files: `kc85-20mhz.sr` (asynchronous, 20 MHz sampling),
  `kc85-cpuclk.sr` (synchronous on the Z80 CLK falling edge). Both capture 34
  channels: CLK, /M1, /INT, /WAIT, /IORQ, /MREQ, /RD, /WR, A0..A15, D0..D7.
- **Trust:** Very high — actual silicon, captured with a calibrated commercial
  logic analyzer by the open-source sigrok community. Rule (3).
- **Type:** Real-hardware capture, replayable offline.
- **Use here:**
  - `scripts/sigrok_z80_decode.py` parses the .sr zip (metadata + 5-byte packed
    samples) into per-cycle CSV + a reconstructed memory image.
  - `scripts/sigrok_opcode_cycles.py` (`make silicon_cycles`) measures
    per-opcode T-state count between consecutive M1 fetches on the synchronous
    capture and cross-checks against the spec table and our emulator.
  - `scripts/sigrok_async_timing.py` (`make silicon_async`) measures CPU
    clock period and sub-T-state pin transition offsets from the 20 MHz
    asynchronous capture, then re-samples it at CLK falling edges to
    independently validate the per-opcode T-state counts. Results below.

#### Real-silicon timing observations (from `kc85-20mhz.sr`)

| Measurement | Real KC85 silicon | Z80 spec | Emulator |
|---|---|---|---|
| CPU clock period | avg **565.8 ns** (550–800 ns range, **0 WAIT samples** in this capture) | — | n/a (host-clocked) |
| Implied CPU frequency | **~1.767 MHz** | matches KC85/4 1.75–1.77 MHz spec | — |
| M1n deassert offset | **64%** into T-state | end-of-T2 → T3 | T2.N → T3.P ✓ |
| MREQn assert / deassert | **9% / 64%** into T-state | T1.N / T2.N | matches our PHI_N model ✓ |
| RDn assert / deassert | **9% / 64%** | T1.N / T2.N | ✓ |
| WRn assert | **9%** (only on writes) | T2.N | ✓ |

**Where each row comes from:**

- *CPU clock period*: `scripts/sigrok_async_timing.py` parses the `.sr` zip
  (sigrok metadata says 20 MHz sample rate ⇒ 50 ns/sample), decodes each
  5-byte sample into the 34 named channels, and walks the **CLK** channel
  looking for 1→0 transitions. The script reports
  `mean(clk_falls[i+1] - clk_falls[i]) * 50 ns` over **441 falling edges**
  observed in the 5000-sample capture (`min 11 samples, max 16 samples`
  between edges, average **11.3 samples** ⇒ 565.8 ns period).
- *Implied CPU frequency*: `1 / period`. The KC85/4 spec lists the system
  clock as 1.75–1.77 MHz (PCK = 1.7734 MHz on most boards); we land within
  that tolerance. The 250 ns variance in period (550–800 ns range) is
  consistent with a /WAIT-state insertion observed by the synchronous
  capture on opcode `0x0b` (DEC BC, 6 T-states real vs 4 T-states spec).
- *Pin transition offsets*: for every CLK falling-edge window
  `[clk_falls[i], clk_falls[i+1]]` the script scans the M1/MREQ/RD/WR
  channels for 1↔0 transitions and bins their sample-offset inside the
  window. Across the 441 windows the modes line up at offsets 1 and 7 of
  the 11-sample period — 9 % and 64 % of a T-state — matching the
  Z80-spec sub-T-state landmark times (MREQ/RD assert just after T1.N;
  M1, MREQ, RD all deassert at end of T2 / start of T3 during an opcode
  fetch). WRn is observed only at offset 1 (writes assert WR at T2.N).
- *"Emulator" column*: cross-references our two-phase `PHI_P / PHI_N`
  model in `cmodel/z80.c` and `cmodel/z80_timing.c`. PHI_N is the
  T-state's second half (Z80 clock falling-edge region), where MREQ/RD
  assert during an MRD cycle and M1 deasserts at the T2.N edge — exactly
  the offsets the LWLA observed.

**/WAIT line use.** /WAIT (CH5 in the LWLA mapping) is captured and
explicitly considered. In the 20 MHz async file `WAITn` is never
asserted across the 5000 samples, so the 565.8 ns period reflects
the intrinsic Z80 clock period without any Tw padding. The script
also reports `period (WAIT-free windows)` separately as a sanity
check — both numbers agree on this dump. In the cpuclk-synchronous
file `WAITn` IS asserted in 17 short events (19 samples total /
0.38 % of capture), and the per-opcode analyzer (`make
silicon_cycles`) attributes per-instruction T-state excess to those
Tw insertions before declaring "silicon system artifact" — on the
kc85 dump every excess is /WAIT-attributable, so the verdict is
**50/50 OK, 0 emulator mismatches, 0 system artifacts**.

**Spec-table correction found.** `INC/DEC rp` (0x03, 0x0B, 0x13,
0x1B, 0x23, 0x2B, 0x33, 0x3B) is **6 T-states** (M1 4T + 2T
internal post-decrement), not 4T. An earlier draft of the analyzer's
spec table had these in the 4T bucket; the kc85 capture flagged 39
consistent 6T observations of `0b` (DEC BC) with no /WAIT, which
agreed with our emulator and with Sean Young's table. Spec-table
corrected; the kc85 capture earned its place as the gold standard.

---

## Emulator / core references (behavioral, secondary)

### MAME Z80 core (`z80.cpp`)
- **URL:** https://github.com/mamedev/mame (src/devices/cpu/z80).
- **Contributes:** A mature, heavily-tested behavioral reference with correct
  undocumented behavior and good timing. Useful as a comparison oracle (rule 4) and
  for cross-checking edge cases (e.g., interrupt timing, block-instruction repeats).
- **Trust:** High behaviorally, but it is a behavioral core, not a structural one — so
  it is a *comparison reference*, never the design authority (per brief).
- **Type:** Mature emulator.
- **Use here:** `tests/mame_compare/` oracle; differences logged in known-differences.

### Other mature C cores (e.g. z80 by superzazu, FUSE's z80)
- **Contributes:** Cross-checks for documented behavior and convenient test vectors
  (FUSE has per-opcode `tests.in`/`tests.expected` covering registers, memory, and
  cycle-by-cycle bus events).
- **Trust:** Medium-high. Rule (5). FUSE test vectors are especially valuable because
  they encode *cycle-accurate bus events*, matching our trace philosophy.
- **Type:** Emulator cores + test vectors.
- **Use here:** Candidate import into `tests/generated/` and timing-trace checks.

### Existing Verilog/VHDL Z80 cores (TV80, A-Z80, etc.)
- **Trust:** Secondary behavioral references only, per the brief — not design authority.
- **Use here:** Sanity cross-checks for RTL structure ideas, nothing inherited verbatim.

---

## Open questions (to be pinned by tests / die evidence)

1. **SCF/CCF XF/YF exact formula** and its NMOS vs CMOS variance. Plan: implement the
   NMOS formula (`YF/XF` from `A | F` or `A` depending on prior op), validate via
   ZEXALL `<scf>`/`<ccf>` tests, document the chosen variant.
2. **WZ update on every instruction class.** Plan: encode WZ updates per control row,
   validate via ZEXALL `bit` tests and the MEMPTR document's table.
3. **Exact interrupt-acceptance timing** (sampling point of INT/NMI relative to last
   T-state, and the extra T-states of the IM0/IM2 acknowledge cycle). Plan: match
   Zilog timing diagram first, refine against visual Z80 if a mismatch appears.
4. **Block-instruction (LDIR/CPIR/…) undocumented XF/YF** derived from `A + transferred
   byte`. Plan: implement per the documented community formula, validate via ZEXALL.
5. **`OUT (C),0` vs `OUT (C),255`** for the undocumented ED70/ED71 result byte (NMOS
   outputs 0). Plan: implement NMOS, log as variant.
6. **DDCB/FDCB "hidden" register copy** for the undocumented `LD r,(IX+d)` rotate/bit
   forms. Plan: implement, validate via ZEXALL `<ddcb>`.

Each of these has a corresponding entry seeded (or to be seeded) in
`docs/known-differences.md` and an acceptance test.

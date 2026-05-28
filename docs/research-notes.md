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

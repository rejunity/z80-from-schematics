# Unsimplifications audit

Catalog of every modeling shortcut found in the C model and the Verilog RTL
that deviates from the real Z80 silicon (Zilog UM0080 / die reverse-engineering /
perfectz80 / Z80 Explorer). The `unsimplify` branch removes them.

Sorted by category. Each entry: **where**, **what we do now**, **what the
silicon does**, **fix plan**, **status**.

## A. Datapath blocks the silicon has, that we collapsed

### A1. Inline 16-bit arithmetic (should be IDU)

The real Z80 has a separate **IDU** (Incrementer/Decrementer Unit) — a 16-bit
unit on the address path. It handles every 16-bit address arithmetic that does
not produce flags:

- PC + 1 (every fetch + every operand byte read)
- SP ± 1 (PUSH / POP / CALL / RET / RST / NMI / INT)
- rp ± 1 (`INC rp` / `DEC rp` / block-op pointer updates HL+/-1, BC-1, DE+/-1)
- R-register refresh increment (low 7 bits only — bit 7 preserved)
- PC + signed displacement (`JR`, `JR cc`, `DJNZ`)

We do all of these as inline `+ 16'd1` / `- 16'd1` / `+ {{8{disp[7]}}, disp}` in
`rtl/z80_core.v` and `cmodel/z80_core.c`. The wip-like-z80-microarch branch had
a `z80_idu` module but it never merged.

**Fix**: add `cmodel/z80_idu.c` + `rtl/z80_idu.v`, route all the inline 16-bit
add/sub-1 + disp-add through it.

**Status**: pending.

### A2. Inline 8-bit ALU ops outside `z80_alu`

EXEC_NEG, EXEC_LD_A_IR, EXEC_IN_C, EXEC_RRD / EXEC_RLD, EXEC_BLOCK
(LDI/CPI/INI/OUTI variants) compute their result and flags **inline** in the
sequencer (`rtl/z80_core.v` / `cmodel/z80_core.c`) instead of going through the
ALU. This was deferred during the c-like-verilog refactor; the new ALU modes
exist on the wip branch but never merged.

**Fix**: extend `z80_alu` with `FLAG_NEG`, `FLAG_LD_A_I`, `FLAG_IN`, `FLAG_RRD`,
`FLAG_RLD`, `FLAG_BLOCK_LD`, `FLAG_BLOCK_CP`, `FLAG_BLOCK_IO` modes. Move the
inlined formulas there. The sequencer calls `z80_alu(...)` like everything else.

**Status**: pending.

### A3. Inline 16-bit ADD/ADC/SBC HL,rp (should be byte-twice ALU)

`EXEC_ADD_HL_RP` / `EXEC_ADC16` / `EXEC_SBC16` compute the 16-bit result inline
(`rtl/z80_core.v` line 279 has a comment: *"16-bit ADD HL,rp (inline; IDU/ALU
detail handled in C model too)"*).

The real Z80 does these byte-at-a-time on the 4-bit ALU: low byte first, then
high byte with the carry chained. Our 8-bit `z80_alu` already implements the
byte-twice structure for 8-bit ADD/SUB; the 16-bit ops should be two ALU passes
with explicit `cf2` / `hf2` carry forwarding.

**Fix**: add `FLAG_ADD16` / `FLAG_ADC16` / `FLAG_SBC16` modes to `z80_alu`,
either as two passes driven by the sequencer or as one 16-bit-shaped mode that
internally calls the 8-bit nibble code twice. Either way the inline
`rf[hlp] + rf[rp_sel_w]` in core goes away.

**Status**: pending.

### A4. Register file as flat 13×16 array

Real Z80 has:
- **GP register file**: BC, DE, HL, AF + their alternates (BC', DE', HL', AF'),
  with hardware bank swaps for `EX AF,AF'` and `EXX`.
- **IX, IY**: separate from the GP file.
- **System register file**: PC, SP, WZ, IR (and the I, R bytes inside IR).

We model the whole thing as `rf[0..12]` — a flat array. `EX AF,AF'` and `EXX`
do explicit byte swaps in C / non-blocking swaps in Verilog.

**Fix**: split into `z80_regfile` (GP) + `z80_sysreg` (PC/SP/WZ) + `z80_ir`
(I, R, IR), with hardware-style bank-select for EX/EXX (a single mux output,
no data movement). Already prototyped on `wip-like-z80-microarch` but not
merged.

**Status**: pending.

## B. Timing / phase shortcuts

### B1. M1 deassert phase (RESOLVED — no simplification at our resolution)

Direct gate-level cross-check (`make perfectz80`, which steps perfectz80's
transistor-level netlist one half-cycle at a time and compares pin samples)
shows perfectz80 reads `m1_n=0` at T2.N and `m1_n=1` at T3.P — exactly what
our model produces. The "real chip deasserts late in T2" description in some
references is the continuous-time analog edge; at the half-cycle sample
resolution at which we model (and at which the gate-level reference operates),
M1 is high starting T3.P, and our model matches.

The model is silicon-faithful at the granularity at which it models. No code
change. `docs/known-differences.md` row 4 is now marked resolved with the
verification path noted.

**Status**: resolved.

### B2. NMI sampling (RESOLVED)

NMI is edge-triggered. The edge capture (`prev_nmi_n` → `nmi_pending`)
does run on every `z80_phase_step()` call (sticky-latch behaviour), but
the **acceptance gate** is exactly the silicon-spec sample point: the
rising edge of the last T-state of the last M-cycle. See
`cmodel/z80_core.c:1182` — `if (!c->stalled && c->t_state == c->m_len && c->phi == 0)`
samples `nmi_pending` into `nmi_sampled` only at T_last.P. The RTL
mirrors this at `rtl/z80_core.v:374`. Verified via `prog8_nmi` + the
pin-scenario NMI programs (`prog10_halt_nmi`, `prog19_nmi_in_int`).

**Status**: resolved.

### B3. INT sampling (RESOLVED)

INT is level-triggered, sampled at the rising edge of the last T-state
of the last M-cycle. Same gate as B2: `cmodel/z80_core.c:1183`
samples `!int_n` into `int_sampled` at T_last.P. RTL at
`rtl/z80_core.v:375`.

**Status**: resolved.

### B4. Latch + deassert co-phase (RESOLVED)

Used to combine `data latch` and `MREQ/RD deassert` at T3.N. Fixed in
`97c09fb` — latch now at T_last.P, deassert at T_last.N.

**Status**: resolved.

## C. Reset state

### C1. Force registers to 0xFFFF on reset

`cmodel/z80_core.c` `z80_reset()`: forces all 13 rf pairs to 0xFFFF "for
deterministic C↔RTL compare". Real silicon leaves these UNDEFINED at power-up
(though reset does force `PC=0, I=0, R=0, IFF1=IFF2=0, IM=0` per Zilog UM0080).

**Fix**: in RTL, leave undefined regs at `'X` (synthesis-friendly default). In
C, leave them at whatever `z80_init()` (which is `memset` 0) gave them.
External test harnesses (FUSE / lockstep / compare) explicitly poke their start
state anyway. Document in `docs/known-differences.md` row 1 as resolved.

**Status**: pending.

## D. Q register

### D1. EX AF,AF' Q-bump (RESOLVED)

The EX AF,AF' handler swaps `rf[AF] ↔ rf[AF2]` and also sets
`f_modified = true` (`cmodel/z80_core.c:413`). At instruction-done the
Q commit (`cmodel/z80_core.c:1058`) snapshots the swapped F so the next
SCF/CCF sees the right value via `(A | Q)`. RTL mirrors this at
`rtl/z80_core.v:497` (sets `f_modified_n = 1'b1` after the bank swap);
the RTL Q commit at line 1010-1016 detects F modification by comparing
`rf_n[AF][7:0]` vs `rf[AF][7:0]` instead of an explicit flag, which
captures the swap. Verified in the z80full test: row 129 EX AF,AF' is
OK.

**Status**: resolved.

## E. Internal bus / external pin protocols

### E1. No internal data bus segments

Real Z80 has internal `db1` / `db2` / `sb` bus segments with directional
switches (sw_1/2/4) gating data flow between the pad ring, ALU, register
file, IDU, and address latch. We model none of it — everything reads/writes
`rf` array slots directly.

**Fix**: long-term, model the bus segments + tri-state-to-mux conversion (no
literal tri-state, synthesizable). This is what the `wip-like-z80-microarch`
branch started. Plumbing-only on the C side; meaningful on the RTL side for
gate-level fidelity.

**Status**: pending — large.

### E2. WAIT sample point not phase-precise

`cmodel/z80_core.c` `is_wait_phase()`: samples `wait_n` at T2.N for M1/MRD/MWR
and T3.N for IORD/IOWR. Per Zilog UM0080 these match the spec sample
points. But our implementation samples *every* phase and stalls if wait_n is
low and the phase predicate matches. The silicon samples ONCE at the spec
edge and latches the decision until the next sample point.

**Fix**: convert is_wait_phase to a per-cycle latch rather than per-phase
continuous polling. Probably no observable difference, but the model becomes
silicon-faithful.

**Status**: pending (low impact, low risk).

## F. Other audit candidates

### F1. Block-op M-cycle structure (z80test rows 12, 13)

The current INI/IND flag computation matches Sean Young's spec exactly
(`cmodel/z80_alu.c:371-382`):
- `k = data + ((C ± 1) & 0xFF)`
- HF/CF = `k & 0x100`
- NF = `data[7]`
- PF = `parity((k & 7) ^ newB)`
- SF/ZF/XY from `newB = B - 1`

…and yet Patrik Rak's `z80test`:
- z80doc tests 098 (INI) and 099 (IND) fail CRC.
- z80memptr tests 102 (INIR→NOP') and 103 (INDR→NOP') fail CRC.
- z80full tests 001 (SCF), 002 (CCF), 089 (LDIR→NOP'), 090 (LDDR→NOP'),
  098, 099, 102, 103 fail CRC.

**Oracle triangulation**: separate runners (`scripts/superzazu_z80test_runner.c`
and `scripts/chips_z80test_runner.c`) run the same `.tap` files through
the **superzazu/z80** and **floooh/chips/z80.h** reference emulators
(both in `scripts/refs/`).

Summary by variant (current — post the 2026-06-18 WZ flip + redcode
ZILOG_NMOS preset):

| Variant     | Ours fails | superzazu | chips | redcode |
|-------------|-----------:|----------:|------:|--------:|
| z80doc      | 2          | 3         | 2     | 2       |
| z80memptr   | **0**      | 16        | **0** | **0**   |
| z80full     | 10         | 20        | 10    | 2       |

Pre-flip baseline (for reference): ours = 2 / 2 / 10, redcode = 2 / 0 / 6.

redcode (Manuel Sainz de Baranda's `github.com/redcode/Z80`) was added
as a 4th oracle on 2026-06-18 via `scripts/redcode_z80test_runner.c` +
`scripts/lockstep_quint.c` (5-way lockstep). It advertises explicit
MEMPTR + Q-factor modelling. Vendored under
`scripts/refs/redcode_z80/` with the Zeta C utility library.

CRC details for the contested tests:

| Test                    | Our CRC     | superzazu CRC | chips CRC   | Expected    |
|-------------------------|-------------|---------------|-------------|-------------|
| z80doc 098 INI          | `505701FA`  | `658766F9`    | `505701FA`  | `07D1B0D1`  |
| z80doc 099 IND          | `6A4034D1`  | `5F9053D2`    | `6A4034D1`  | `3DC685FA`  |
| z80full 001 SCF         | `45FC79B5`  | `45FC79B5`    | `45FC79B5`  | `D841BD8A`  |
| z80full 002 CCF         | `A206B5E3`  | `A206B5E3`    | `A206B5E3`  | `3FBB71DC`  |
| z80full 089 LDIR→NOP'   | `9DC743B5`  | `9DC743B5`    | `9DC743B5`  | `CC93B5EC`  |
| z80full 090 LDDR→NOP'   | `9C1DEA50`  | `9C1DEA50`    | `9C1DEA50`  | `CD491C09`  |
| z80full 098 INI         | `A7F6C1B0`  | `DA84C3B3`    | `A7F6C1B0`  | `03DA7534`  |
| z80full 099 IND         | `E81CDF03`  | `E093F698`    | `E81CDF03`  | `4C306B87`  |
| z80memptr 102 INIR→NOP' | `0A537B63`  | `4A74FCE9`    | **PASS**    | `F3B1BE2F`  |
| z80memptr 103 INDR→NOP' | `0A537B63`  | `4A74FCE9`    | **PASS**    | `F3B1BE2F`  |

**Findings**:

1. **SCF/CCF/LDIR→NOP'/LDDR→NOP' (z80full 1/2/89/90)** — all three
   emulators produce **bit-identical CRCs**, all differing from Rak's
   silicon-derived expected the same way. Strong evidence this is a
   real silicon-faithfulness quirk that THE ENTIRE open-source emulator
   community misses the same way. Probably gate-level INTERNAL Q-mux
   exposure during the block-op M-cycle sequence, not the
   per-instruction commit the emulators model.

2. **INI/IND (z80doc/z80full 98/99)** — ours and chips produce
   **identical CRCs**; superzazu differs. We agree with chips, superzazu
   is the outlier — but neither chips's nor our CRC matches Rak's
   expected. So we have a chips-compatible bug, and Rak expects
   something else entirely.

3. **z80memptr 102/103 INIR→NOP', INDR→NOP'** — **chips AND redcode
   both pass these.** Investigation found the difference: chips's INIR
   (scripts/refs/chips_z80.h line 1541) and redcode's INIR/INDR both
   set `WZ = PC + 1` during the repeat iteration's internal M-cycle
   (rewinding PC by 2 first). Our model does the same `WZ = PC + 1`
   for LDIR/LDDR/CPIR/CPDR repeats but NOT for INIR/INDR/OTIR/OTDR.

   **Attempted fix** (earlier session, reverted): applied the
   `WZ = PC + 1` on repeat to both INIR/INDR and OTIR/OTDR. Result:
   - z80memptr: 2 failures → **0 failures** (clean 160/160). ✓
   - **BUT FUSE broke** on `edba_1` (INDR) and `edbb_1` (OTDR): FUSE
     expects `WZ = BC ± 1` (the M2 setting), not `WZ = PC + 1`.

   **redcode probe** (`/tmp/redcode_fuse_probe.c`, 2026-06-18) confirmed
   this is universal: redcode produces `WZ = PC + 1 = 0x0001` on FUSE
   edba_1 / edb2_1 (final iteration of INDR/INIR with BC=1), matching
   chips and Rak z80memptr expectations, NOT matching FUSE's
   `0x069E` / `0x0A41`. For single-shot ED A2 (INI) and ED AA (IND),
   redcode matches FUSE exactly.

   So FUSE and Rak disagree on what silicon does for INIR/INDR/OTIR/OTDR
   end-state WZ when the repeat aborts (BC=1→0). All silicon-faithful
   modern emulators (chips, redcode) — including redcode which has
   explicit Q-factor and MEMPTR engineering — produce `WZ = PC + 1`,
   matching Rak. FUSE's `WZ = BC ± 1` expected values for these
   specific 4 cases appear to be pre-2013 outliers.

   **Resolution (2026-06-18, commit `b654110`)**: Flipped to
   silicon-faithful `WZ = PC + 1`. Trade as expected:
   - `make fuse` 1356/1356 → 1352/1356 + 4 known-FUSE-wrong
     (edba_1, edbb_1, edb2_1, edb3_1 enumerated in
     `tests/fuse/known-fuse-wrong.txt`). `fuse_runner.c` +
     `compare_fuse_rtl.py` skip these so make fuse / make fuse_rtl
     stay green.
   - `make z80test` z80memptr 158/160 → **160/160**.
   - C model + RTL synced (`cmodel/z80_core.c:854`, `:884`;
     `rtl/z80_core.v:950`, `:967`).

4. **Overall scoreboard** (current, post-flip):
   - **10 z80full failures**, vs superzazu **20**, vs chips **10**, vs redcode **2**.
   - **0 z80memptr failures**, vs superzazu **16**, vs chips **0**, vs redcode **0**.
   - **2 z80doc failures**, vs superzazu **3**, vs chips **2**, vs redcode **2**.
   - Significantly more silicon-faithful than superzazu; we tie chips on
     z80doc/z80memptr but redcode wins z80full outright (-8 vs us). The
     redcode advantage comes from its `Z80_MODEL_ZILOG_NMOS` preset
     (`LD_A_IR_BUG | XQ | YQ`) — the XQ/YQ bits implement a per-instruction
     SCF/CCF Q-leak that fixes z80full 001/002 + ST-variant skips
     5/6, which is the closest match to Patrik's silicon-derived expectations.

5. **redcode INI / IND remaining failures (z80doc + z80full 098/099)** —
   2026-06-18 investigation. With `Z80_MODEL_ZILOG_NMOS` enabled, redcode
   fails exactly the same INI/IND CRC as we do and as chips does:
   - z80doc 098 INI:  redcode/chips/ours produce **identical** `505701FA`; Rak expects `07D1B0D1`.
   - z80doc 099 IND:  redcode/chips/ours produce **identical** `6A4034D1`; Rak expects `3DC685FA`.
   - z80full 098 INI: redcode/chips/ours produce **identical** `A7F6C1B0`; Rak expects `03DA7534`.
   - z80full 099 IND: redcode/chips/ours produce **identical** `E81CDF03`; Rak expects `4C306B87`.

   Three independent implementations of Sean Young's documented INI/IND
   formula (`PF = parity((data + (C ± 1)) & 7) ^ B_new`, `HF/CF = k & 0x100`,
   `SF/ZF/X/Y from B_new`, `NF = data[7]`) converge on the same CRC and
   the same disagreement with Rak's expected.

   **Lead investigated and ruled out**: Rak's `tests.asm:10, :1008-1022`
   tags `.ini` / `.ind` with `db incheck` — meaning the test runner
   first issues `IN A, (0xFE)` and aborts the test if the byte != `0xBF`
   (the Spectrum ULA idle-state return). One hypothesis was that Rak's
   expected CRC was captured with IN data = `0xBF` for ALL ports during
   the loop, and our runners' "0xFF except port 0xFE" returned different
   bytes. Verified empirically: pinning ALL ports to `0xBF` in the
   redcode runner produced the **same CRC** as the default config
   (`505701FA`), and pinning ALL ports to `0x42` aborted the test at
   the incheck guard before any CRC was computed. So Rak's INI / IND
   test only ever issues INs to port `0xFE` (already returning `0xBF`
   in our setup), and the CRC discrepancy is therefore NOT IN-data-driven.

   The Banks/Helcmanovsky/rofl0r/Sainz de Baranda 2018-2023 reverse-
   engineering work covers INIR/INDR/OTIR/OTDR **repeat-iteration**
   flags (PCi.13/PCi.11 fold-ins during the extra 5-T M-cycle) but not
   the **single-shot** INI/IND CRC. Either Rak's silicon captured a
   chip variant whose single-shot INI/IND formula nobody has decoded,
   or Rak's test threads in a Q-leak / state-dependent fold-in we
   haven't found. **Not fixable via redcode options alone.** Documented
   as an industry-wide silicon-faithfulness gap; pursuit deferred to
   when somebody decodes the missing piece.

**False lead** (commit `093f95b`, reverted in commit `b4f5aca`): once
theorised Q-leak persistence. Broke ZEXALL's `<daa,cpl,scf,ccf>` CRC
(run `27650481834`). Per Sean Young §4.1, Q DOES reset to 0 after any
non-F-modifying instruction. Original behaviour was correct.

**Cross-check: redcode end-to-end on FUSE** (2026-06-18, via
`scripts/redcode_fuse_runner.c`): **1348 / 1356 PASS**. The 8 failures
break down as:

- 4 `INxR`/`OTxR` BC=1→0 WZ cases (`edb2_1`, `edb3_1`, `edba_1`, `edbb_1`)
  — exactly the FUSE↔Rak disagreement above; redcode sides with Rak.
- 2 SCF/CCF Q-leak X/Y bits (`37_1`, `3f`).
- 1 CPDR X-flag bit (`edb9_2`).
- 1 HALT PC convention (`76`) — FUSE expects PC stays at HALT addr,
  redcode advances PC past it (the more common modern model).

**FUSE corpus provenance** (researched 2026-06-18):
- The 1356-case `tests.in` / `tests.expected` was **authored by Philip
  Kendall in 2006** for FUSE, **not by Frank D. Cringle**. Cringle's
  separate `zexall.com` / `zexdoc.com` CP/M exercisers are a different
  corpus (those WERE silicon-checksummed against a real Z80).
- No FUSE README, commit message, or Changelog entry documents the
  methodology used to produce `tests.expected`. The community consensus
  is that it is **emulator-derived (FUSE-self-referential)**, not
  silicon-captured.
- floooh/chips's own `z80-fuse.c` test harness already blacklists FUSE
  `BIT n,(HL)` X/Y expectations as "FUSE handles those wrong" —
  community-acknowledged FUSE errors on undocumented bits.
- The "`WZ = PC+1` during the extra 5-T repeat M-cycle" behavior was
  discovered on real silicon via boo-boo et al.'s 2006 MEMPTR paper
  (Zilog chips, multiple testers), confirmed by Patrik Rak's 2012
  validation on a real 48K Spectrum, and refined by Richard Chandler
  on NEC Z80 + Visual Z80 Remix in Rak's v1.2a (z80test PR #3).
- Bottom line: FUSE `tests.expected` is emulator-derived and stale on
  `INxR`/`OTxR`; Rak (post-v1.2a) reflects real hardware. We
  document this in `tests/fuse/README.md` and accept that our
  FUSE 1356/1356 score includes 4 cases where FUSE itself is wrong
  vs silicon.

**Remaining fix path**: items (1), (2), and the Rak-vs-FUSE WZ trade-off
in (3) are all silicon-faithfulness corners. (3) is now well-evidenced:
3 independent silicon-faithful oracles (chips, redcode, Rak z80memptr)
agree FUSE is the outlier. The next concrete step is flipping our
INxR/OTxR repeat WZ to `PC+1`, accepting FUSE 1352/1356 and gaining
Rak z80memptr 160/160. Tracked under
[known-differences.md](known-differences.md) rows 12, 13 at
`make z80test` baselines 2 / 2 / 10.

### F2. SCF / CCF "ST" variants (Toshiba CMOS)

`z80full` tests 005 SCF (ST) and 006 CCF (ST) fail by design — they
test the **Toshiba CMOS** variant of SCF/CCF (no Q-leak). We model
Zilog NMOS (with Q-leak). Documented in [known-differences.md](known-differences.md)
row 2; intentional NMOS-correct default. A runtime Toshiba switch is a
future enhancement (deferred — no current use case demands it).

### F3. SCF / CCF NMOS variants (Q-leak)

`z80full` tests 001 SCF and 002 CCF also fail CRC even though our SCF/
CCF implements the documented NMOS Q-leak (X/Y from `(A | Q)`). Likely
same root cause as F1 — Q-leak timing during NOP-chain pipelines.

### F4. Validated as silicon-faithful (no change needed)

- **DDCB/FDCB displacement compute folding**: matches FUSE 1356/1356
  and `make perfectz80` control-pin 100 %; documented at known-
  differences.md row 11 as resolved.
- **NMI/INT M1-ack suppress-decode**: matches `prog8_nmi` trace +
  perfectz80 control pins. The pin combination during INTA (M1+IORQ
  asserted together) does suppress the decode pipeline correctly.

## Plan

### Resolved
B1 (M1 deassert), B2 (NMI sample), B3 (INT sample), B4 (latch/deassert
co-phase), D1 (EX AF Q-bump).

### Remaining work, sorted by leverage × risk

1. **C1**: reset-state un-force. Small (one constant change). Would
   close known-differences row 1 and bring the perfectz80 `addr` /
   `data_o` match from 95-98 % to 100 % on `prog_rnd_02` / `prog_rnd_03`.
2. **F1**: block-op Q-leak timing during repeat termination. Medium-
   large. Would close z80doc 2/2, z80memptr 2/2, ~6 of z80full 10.
3. **A2**: move inline 8-bit ALU ops into `z80_alu` modes (cleanup).
4. **A3**: 16-bit ALU ops via byte-twice in `z80_alu` (cleanup).
5. **A1**: introduce `z80_idu` module (well understood, prototype on a
   wip branch).
6. **A4**: register file restructure (large).
7. **E1**: internal bus segments (largest; defer until A1–A4 done).
8. **E2**: WAIT sample latch (cleanup — no observable difference).

After each: full gate run (`make all-tests`) — must stay 100 % across every
existing oracle (FUSE, fuse_rtl, compare, lockstep, silicon_cycles,
silicon_async, perfectz80).

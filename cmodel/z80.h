/* ============================================================================
 * SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
 * SPDX-License-Identifier: MIT
 *
 * z80.h - public types and API for the schematic-faithful Z80 C model.
 *
 * This header is the in-code half of the shared design contract (see
 * docs/architecture.md, docs/pla.md, docs/flags.md). Names mirror the Verilog
 * RTL: pins as pin_* fields, control lines as ctl_*, etc.
 * ==========================================================================*/
#ifndef Z80_H
#define Z80_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Flag bit masks (F register layout: S Z Y H X P/V N C)  -- see docs/flags.md
 * ------------------------------------------------------------------------- */
#define Z80_SF 0x80u  /* sign            */
#define Z80_ZF 0x40u  /* zero            */
#define Z80_YF 0x20u  /* undoc, bit5     */
#define Z80_HF 0x10u  /* half carry      */
#define Z80_XF 0x08u  /* undoc, bit3     */
#define Z80_PF 0x04u  /* parity/overflow */
#define Z80_NF 0x02u  /* add/subtract    */
#define Z80_CF 0x01u  /* carry           */

/* ---------------------------------------------------------------------------
 * External pins (active-low signals carry _n). Mirrors rtl/z80_pins.v.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* inputs */
    uint8_t  data_in;     /* data bus, sampled side                          */
    bool     wait_n;      /* 0 => insert wait                                */
    bool     int_n;       /* 0 => maskable interrupt requested              */
    bool     nmi_n;       /* 0 => NMI (edge) requested                       */
    bool     busreq_n;    /* 0 => bus request                               */
    bool     reset_n;     /* 0 => reset asserted                            */
    /* outputs */
    uint16_t addr;        /* address bus                                    */
    uint8_t  data_out;    /* data bus, driven side                          */
    bool     data_drive;  /* 1 => core drives the data bus (mux, not tri-st)*/
    bool     m1_n;        /* opcode fetch / interrupt ack                   */
    bool     mreq_n;      /* memory request                                 */
    bool     iorq_n;      /* I/O request                                    */
    bool     rd_n;        /* read strobe                                    */
    bool     wr_n;        /* write strobe                                   */
    bool     rfsh_n;      /* refresh address valid                          */
    bool     halt_n;      /* 0 => halted                                    */
    bool     busack_n;    /* 0 => bus acknowledged                          */
    bool     addr_drive;  /* the die drives the address pads                */
    bool     ctl_drive;   /* ... and MREQ IORQ RD WR (data: data_drive)     */
} z80_pins_t;

/* ---------------------------------------------------------------------------
 * Register file storage. 16-bit pairs in an array; bytes via accessors.
 * ------------------------------------------------------------------------- */
enum {
    RFP_BC, RFP_DE, RFP_HL, RFP_AF,
    RFP_BC2, RFP_DE2, RFP_HL2, RFP_AF2,
    RFP_IX, RFP_IY, RFP_SP, RFP_PC, RFP_WZ,
    RFP_COUNT
};

/* 8-bit register selector following the r[] decode table (z/y fields). */
typedef enum {
    REG_B = 0, REG_C, REG_D, REG_E, REG_H, REG_L, REG_iHL, REG_A
} z80_reg8_t;

/* ---------------------------------------------------------------------------
 * ALU operation selector (alu[y] table) + raw add/sub result.
 * ------------------------------------------------------------------------- */
typedef enum {
    ALU_ADD = 0, ALU_ADC, ALU_SUB, ALU_SBC, ALU_AND, ALU_XOR, ALU_OR, ALU_CP
} z80_alu_op_t;

typedef struct {
    uint8_t res;    /* 8-bit result                                          */
    uint8_t carry;  /* ADD: carry-out of bit7 ; SUB: borrow-out (CF sense)   */
    uint8_t half;   /* ADD: carry-out of bit3 ; SUB: borrow (HF sense)       */
} z80_addsub_t;

/* nibble-twice ALU primitives (docs/alu.md). cin/bin are 0 or 1. */
z80_addsub_t z80_alu_add8(uint8_t a, uint8_t b, uint8_t cin);
z80_addsub_t z80_alu_sub8(uint8_t a, uint8_t b, uint8_t bin);
uint8_t      z80_alu_logic(z80_alu_op_t op, uint8_t a, uint8_t b);
uint8_t      z80_parity(uint8_t x); /* 1 = even parity */

/* ---------------------------------------------------------------------------
 * Flags subsystem (docs/flags.md). Each returns the new F; arithmetic helpers
 * also return the 8-bit result via *res. oldF carries unaffected flag bits.
 * ------------------------------------------------------------------------- */
uint8_t z80_flags_add8(uint8_t a, uint8_t b, uint8_t cin, uint8_t oldF, uint8_t *res);
uint8_t z80_flags_sub8(uint8_t a, uint8_t b, uint8_t cin, bool is_cp, uint8_t oldF, uint8_t *res);
uint8_t z80_flags_logic(z80_alu_op_t op, uint8_t a, uint8_t b, uint8_t *res);
uint8_t z80_flags_inc8(uint8_t r, uint8_t oldF, uint8_t *res);
uint8_t z80_flags_dec8(uint8_t r, uint8_t oldF, uint8_t *res);
uint8_t z80_flags_rot_a(uint8_t op, uint8_t a_in, uint8_t oldF, uint8_t *res); /* op: 0 RLCA 1 RRCA 2 RLA 3 RRA */
uint8_t z80_flags_rot(uint8_t op, uint8_t in, uint8_t oldF, uint8_t *res);     /* CB rot[y] 0..7 */
uint8_t z80_flags_bit(uint8_t b, uint8_t src, uint8_t xy_src, uint8_t oldF);   /* xy_src supplies bits 5/3 */
uint8_t z80_flags_daa(uint8_t a, uint8_t oldF, uint8_t *res);
uint8_t z80_flags_scf(uint8_t a, uint8_t oldF, uint8_t q);
uint8_t z80_flags_ccf(uint8_t a, uint8_t oldF, uint8_t q);
uint8_t z80_flags_cpl(uint8_t a, uint8_t oldF, uint8_t *res);

/* Top-level ALU module entry point (mirrors rtl/z80_alu.v). One C
   parameter per Verilog port; by-value inputs, by-pointer outputs.
   The z80_alu_* and z80_flags_* helpers above are its internal building
   blocks. Sequencer calls go through this. */
void z80_alu(uint8_t mode, uint8_t alu_op, uint8_t rot_op, uint8_t bit_idx,
             uint8_t xy_src, uint8_t a, uint8_t b, uint8_t oldf, uint8_t q,
             uint8_t *res, uint8_t *fout);

/* Top-level timing module entry point (mirrors rtl/z80_timing.v). Pure
   combinational: drives the external bus pins as a function of (bus_op,
   t_state, phi) and the current M-cycle's (addr, wdata, I, R). */
void z80_timing(uint8_t bus_op, uint8_t t_state, uint8_t phi, uint8_t m_len,
                uint16_t m_addr, uint8_t m_wdata,
                uint8_t reg_i, uint8_t reg_r,
                uint16_t *addr, uint8_t *data_out, bool *data_drive,
                bool *m1_n, bool *mreq_n, bool *iorq_n,
                bool *rd_n,  bool *wr_n,   bool *rfsh_n);

/* The timing ROM: z80_timing() tabulated over (bus_op, t_state, phi) by
   z80_timing_rom_init() (z80_reset calls it). The core drives its pins from
   it; z80_timing() remains the source of truth. lvl[] holds the pins as the
   bytes of z80_pins_t from data_drive through rfsh_n; refresh selects I:R
   over m_addr for the address bus. t_state saturates at 15. */
typedef struct {
    uint8_t lvl[7];
    uint8_t refresh;
} z80_timing_rom_t;
#define Z80_TIMING_ROM_SIZE 256
#define Z80_TIMING_ROM_INDEX(op, t, phi) \
    ((((unsigned)(op) & 7u) << 5) | (((unsigned)(t) & 15u) << 1) | ((unsigned)(phi) & 1u))
#define Z80_TIMING_ROM_FIRST offsetof(z80_pins_t, data_drive)
#define Z80_TIMING_ROM_LEN   (offsetof(z80_pins_t, rfsh_n) + 1 - Z80_TIMING_ROM_FIRST)
extern z80_timing_rom_t z80_timing_rom[Z80_TIMING_ROM_SIZE];
void z80_timing_rom_init(void);

/* ---------------------------------------------------------------------------
 * Control word (PLA output). Conceptual fields; see docs/pla.md.
 * ------------------------------------------------------------------------- */
typedef enum {
    SEQ_NONE = 0,  /* no extra M-cycles after fetch                         */
    SEQ_IMM8,      /* read one immediate byte                               */
    SEQ_IMM16,     /* read two immediate bytes                              */
    SEQ_MRD_HL,    /* read (HL)                                             */
    SEQ_MWR_HL,    /* write (HL)                                            */
    SEQ_RMW_HL,    /* read-modify-write (HL)                                */
    SEQ_JR,        /* relative jump                                         */
    SEQ_CALL,      /* call nn                                               */
    SEQ_RET,       /* return                                                */
    SEQ_PUSH,      /* push rp                                               */
    SEQ_POP,       /* pop rp                                                */
    SEQ_IO,        /* in/out                                                */
    SEQ_IDX_D,     /* DD/FD (IX+d) addressing                               */
    SEQ_BLOCK,     /* block instruction                                     */
    SEQ_ILLEGAL
} z80_seq_t;

/* How the F register is updated after the operation.

   FLAG_NEG / FLAG_LD_A_I / FLAG_IN / FLAG_RRD / FLAG_RLD / FLAG_BLOCK_*
   are dispatched through z80_alu() with the following input-port overloads
   (see z80_alu.c case dispatch for the canonical table):

      FLAG_NEG       : a = A
      FLAG_LD_A_I    : b = I or R; bit_idx[0] = iff2
      FLAG_IN        : b = the IN byte
      FLAG_RRD/RLD   : a = A, b = mem; res = new_A (new_mem is bus-fabric work)
      FLAG_BLOCK_LD  : a = A, b = val,       bit_idx[0] = (bc_after != 0)
      FLAG_BLOCK_CP  : a = A, b = val,       bit_idx[0] = (bc_after != 0)
      FLAG_BLOCK_IO  : a = data, b = newB,   xy_src[2:0] = k[2:0],
                                              xy_src[3]   = k_carry
*/
typedef enum {
    FLAG_NONE = 0, FLAG_ADD8, FLAG_SUB8, FLAG_CP8, FLAG_LOGIC,
    FLAG_INC8, FLAG_DEC8, FLAG_ROT_A, FLAG_ROT, FLAG_BIT,
    FLAG_ADD16, FLAG_ADC16, FLAG_SBC16, FLAG_DAA, FLAG_SCF, FLAG_CCF,
    FLAG_CPL, FLAG_NEG, FLAG_BLOCK_LD, FLAG_BLOCK_CP, FLAG_BLOCK_IO,
    FLAG_LD_A_I, FLAG_IN, FLAG_RRD, FLAG_RLD
} z80_flag_mode_t;

/* Address-bus source for the current data M-cycle. */
typedef enum {
    ADDR_PC = 0, ADDR_HL, ADDR_BC, ADDR_DE, ADDR_SP, ADDR_WZ, ADDR_IR_REFRESH
} z80_addr_src_t;

/* MEMPTR/WZ update rule (docs/undocumented.md). */
typedef enum {
    WZ_NONE = 0, WZ_ADDR_PLUS1, WZ_NN, WZ_NN_PLUS1, WZ_DEST, WZ_HL_PLUS1, WZ_IO
} z80_wz_op_t;

typedef enum {
    PFX_NONE = 0, PFX_CB, PFX_ED, PFX_DD, PFX_FD, PFX_DDCB, PFX_FDCB
} z80_prefix_t;

/* Datapath action selector (one of the PLA's named control outputs): names the
   register/ALU/bus behavior of the instruction. The micro-sequencer dispatches
   on (exec, m_cycle, phi). Grouped by datapath function, not by raw opcode. */
typedef enum {
    EXEC_NOP = 0, EXEC_PREFIX, EXEC_HALT, EXEC_DI, EXEC_EI,
    EXEC_LD_R_R, EXEC_LD_R_N, EXEC_LD_R_M, EXEC_ST_M_R, EXEC_LD_M_N,
    EXEC_ALU_R, EXEC_ALU_N, EXEC_ALU_M,
    EXEC_INC_R, EXEC_DEC_R, EXEC_INC_M, EXEC_DEC_M,
    EXEC_LD_RP_NN, EXEC_INC_RP, EXEC_DEC_RP, EXEC_ADD_HL_RP,
    EXEC_JP, EXEC_JP_CC, EXEC_JP_HL, EXEC_JR, EXEC_JR_CC, EXEC_DJNZ,
    EXEC_CALL, EXEC_CALL_CC, EXEC_RET, EXEC_RET_CC, EXEC_RST,
    EXEC_PUSH, EXEC_POP, EXEC_LD_SP_HL,
    EXEC_EX_DE_HL, EXEC_EX_AF, EXEC_EXX, EXEC_EX_SP_HL,
    EXEC_LD_A_RP, EXEC_LD_RP_A,           /* (BC)/(DE) <-> A */
    EXEC_LD_A_NN, EXEC_LD_NN_A,
    EXEC_LD_HL_NN, EXEC_LD_NN_HL,
    EXEC_ROT_A, EXEC_DAA, EXEC_CPL, EXEC_SCF, EXEC_CCF,
    EXEC_IN_A_N, EXEC_OUT_N_A,
    EXEC_CB_R, EXEC_CB_M,                  /* CB: rot/bit/res/set on r / (HL) */
    /* ED page */
    EXEC_ADC16, EXEC_SBC16,                /* ADC/SBC HL,rp                  */
    EXEC_NEG, EXEC_IM, EXEC_RETN,          /* NEG, IM n, RETI/RETN           */
    EXEC_LD_I_A, EXEC_LD_R_A, EXEC_LD_A_IR,/* LD I,A / LD R,A / LD A,I|R     */
    EXEC_LD_NNA_RP, EXEC_LD_RP_NNA,        /* LD (nn),rp / LD rp,(nn)        */
    EXEC_IN_C, EXEC_OUT_C,                 /* IN r,(C) / OUT (C),r           */
    EXEC_RRD, EXEC_RLD,                    /* rotate digit                   */
    EXEC_BLOCK,                            /* LDI/LDD/CPI/.../INI/.../OUTI/. */
    EXEC_DDCB,                             /* DD/FD CB d op : op on (IX+d)    */
    EXEC_NMI, EXEC_INT,                    /* interrupt acceptance sequences  */
    EXEC_ILLEGAL
} z80_exec_t;

/* CB operation kind (= x field of the CB opcode) */
enum { CB_ROT = 0, CB_BIT = 1, CB_RES = 2, CB_SET = 3 };

/* block-instruction id (in control.aux for EXEC_BLOCK) */
enum {
    BLK_LDI = 0, BLK_LDD, BLK_CPI, BLK_CPD,
    BLK_INI, BLK_IND, BLK_OUTI, BLK_OUTD
};
#define BLK_REPEAT 0x08u   /* OR'd into aux for the repeating (xxxR) forms */

typedef struct {
    z80_exec_t      exec;       /* datapath action                          */
    z80_seq_t       seq;        /* M-cycle template (doc/trace)             */
    z80_flag_mode_t flag_mode;
    z80_alu_op_t    alu_op;
    z80_addr_src_t  addr_src;
    z80_wz_op_t     wz_op;
    uint8_t         rf_src;     /* 8-bit source reg (z80_reg8_t)            */
    uint8_t         rf_dst;     /* 8-bit dest reg                           */
    uint8_t         rp_sel;     /* 16-bit pair index into rf for rp ops     */
    uint8_t         cc;         /* condition code index (cc[y])             */
    uint8_t         bit_index;  /* for CB BIT/RES/SET                       */
    uint8_t         rot_op;     /* RLCA/.. or CB rot[y]                     */
    uint8_t         cb_kind;    /* CB op kind: CB_ROT/BIT/RES/SET           */
    uint8_t         aux;        /* misc: IM mode / block-op id / LD A,I|R   */
    uint8_t         idx;        /* 0 = HL, 1 = IX, 2 = IY (DD/FD prefix)    */
    bool            use_disp;   /* memory operand is (IX+d)/(IY+d)          */
    uint8_t         rst_addr;   /* RST target (y*8)                         */
    bool            uses_cc;    /* conditional instruction                  */
    bool            valid;      /* row matched                             */
    uint16_t        special;    /* bitfield of special-case hooks          */
} z80_control_t;

/* special bits */
#define SPC_HALT     0x0001u
#define SPC_EI       0x0002u
#define SPC_DI       0x0004u
#define SPC_PREFIX   0x0008u   /* this opcode is a prefix byte             */
#define SPC_CP_XY    0x0010u   /* X/Y from operand (CP)                    */
#define SPC_DDCB_CPY 0x0020u   /* DDCB result also copied to r[z]          */

/* ---------------------------------------------------------------------------
 * CPU state.
 * ------------------------------------------------------------------------- */
typedef struct {
    z80_pins_t   pins;
    uint16_t     rf[RFP_COUNT]; /* register-file pairs                       */
    uint8_t      reg_i;         /* interrupt vector base                     */
    uint8_t      reg_r;         /* refresh counter                          */
    uint8_t      ir;            /* instruction register (current opcode)     */

    /* sequencer state */
    uint8_t      phi;           /* 0 = PHI_P, 1 = PHI_N                       */
    uint8_t      t_state;       /* 1-based                                   */
    uint8_t      m_cycle;       /* 1-based                                   */
    uint8_t      bus_op;        /* z80_busop_t                              */
    z80_prefix_t prefix;        /* active prefix state                       */
    z80_control_t ctl;          /* decoded control word for current instr    */

    /* interrupt / mode state */
    bool         iff1, iff2;
    uint8_t      im;            /* interrupt mode 0/1/2                       */
    bool         halted;
    bool         nmi_pending;   /* sticky latch: any falling edge on NMI seen */
    bool         prev_nmi_n;    /* for edge detection                        */
    bool         nmi_resp;      /* in the NMI response (M1 + pushes): the
                                   chip holds its NMI flip-flop clear, so an
                                   NMI edge there is lost (perfectz80)       */
    /* NMI / INT silicon sample latches. Per Zilog UM0080, both are sampled at
       the rising edge of the last T-state of the last M-cycle (= T_last.P in
       our phase model). nmi_sampled freezes nmi_pending's value at that
       phase; int_sampled freezes !int_n at that phase. begin_next() uses
       these latches, not the live signals, so an interrupt that changes
       between T_last.P and the M-cycle boundary cannot retroactively
       affect the current instruction's accept decision. */
    bool         nmi_sampled;
    bool         int_sampled;
    bool         ei_delay;      /* suppress INT for one instruction after EI  */
    bool         suppress_decode;/* ack-cycle M1: latch but don't decode/PC++ */
    bool         im0_ins;       /* IM0: the instruction came from the interrupting
                                   device; PC doesn't move until it ends   */
    bool         bus_granted;   /* BUSACK active (DMA owns the bus)           */

    /* micro-step bookkeeping (filled by control sequencer) */
    uint8_t      ucode;         /* micro-step index within instruction       */
    uint16_t     tmp16;         /* scratch for nn / dest formation           */
    uint8_t      tmp8;          /* scratch (last byte read)                  */
    uint8_t      tmpl, tmph;    /* scratch low/high byte assembly            */
    bool         cc_taken;      /* condition evaluated true                  */

    /* current M-cycle descriptor (set when an M-cycle starts) */
    uint8_t      m_len;         /* total T-states this M-cycle (incl extra)  */
    uint16_t     m_addr;        /* address driven this M-cycle               */
    uint8_t      m_wdata;       /* data driven on writes                     */
    bool         instr_done;    /* set when the instruction completes        */
    bool         decoded;       /* opcode for this instr has been decoded    */
    bool         phase_primed;  /* false right after reset (skip 1st advance) */
    /* WAIT-state engine, per Zilog UM0080:
       wait_sampled is set at the WAIT sample edge (T2.N for memory cycles,
       Tw.N for I/O cycles and any inserted Tw). It captures !wait_n at
       exactly that phase; advance() consults it at the .N→.P boundary
       to decide whether to hold the current T-state as a Tw.
       stalled = wait_sampled for the small window during which advance()
       reads it; this is the same single-cycle latch the silicon WAIT
       gate behaves as. */
    bool         wait_sampled;
    bool         stalled;

    uint64_t     cycle;         /* global half-step (phase) counter          */
    uint64_t     instr_count;   /* completed-instruction counter             */

    /* Q register: holds F if the previous instruction wrote F, else 0. Used
       by SCF/CCF X/Y derivation on NMOS silicon. Committed with the
       instruction's F write (wb_fire), else at its end (wb_defer). */
    uint8_t      q;

    /* RESET, as the die does it (perfectz80; docs/timing.md "RESET"):
       the pin is sampled on every rising edge (res_s); the full reset res2
       takes that sample on the falling edge in the middle of each T-state,
       except in the low half of an M1's T2, where it holds and the special-
       reset latch sres takes it instead. While res2 was set on the previous
       falling edge, no T-state follows the current one (rst_idle); T1 of an
       M1 at 0000 comes once it clears. srst_m1 marks the opcode fetch the
       special reset replaces; rd_hold keeps RD low after a cut T1 (through
       the idle T.P, and into a restart that follows at once). z80_reset()
       is the power-on reset. */
    bool         res_s, res2, sres, rst_idle, srst_m1, rd_hold;
    bool         in_tw;       /* this T-state is a Tw (WAIT held the last): not T2 */

    /* Register write-back as the die does it (docs/timing.md "Register
       write-back"): the die writes an 8-bit load's or ALU op's result in
       the next M1's (INTA's) T2.N and F in its T3.N, from its ALU latches
       -- a RESET cut ending before blocks them, one ending on that T2
       leaves F from the half-loaded flag latches. So does this model: the
       instruction ends with its writes pending (wbp), its decode and
       operands left where they are -- the registers, the read byte in
       tmp8 -- and they land on the die's edge (wb_fire). WBP_ALU: the
       instruction's register and F writes, at the next M1's T2 end;
       WBP_F1: a block instruction's F at its T1 end; WBP_F16: ADD / ADC /
       SBC HL's F (pf_val, after a cut on that T2 pf_cut; their H waits in
       ph_val for the second internal M-cycle's end); WBP_EXAF (T1),
       WBP_EXX, WBP_EXDH (T2): the exchanges. wb_exec / wb_idx / wb_disp:
       the deferred instruction's decode, which an interrupt response
       overwrites (begin_next); wb_pv: LD A,I / R's P/V, the boundary's
       IFF2 decision; wb_rep: the block instruction repeated (Banks' X / Y).
       cut_now: a RESET cut ends on this edge -- a read straight into its
       register in its T3.N is blocked (the exec step). z80_arch() shows the
       registers as the program sees them, the pending writes made. */
    uint8_t      wbp, wb_idx, wb_pv, wb_rep;
    uint8_t      wb_exec;
    bool         wb_disp, cut_now;
    uint8_t      ph_val, pf_val, pf_cut;

    /* BUSREQ, as sampled on the rising edge that started this T-state (true
       = requested): in an M-cycle's last T-state it decides the grant, in a
       granted T-state the release (perfectz80; PS0178 Figure 10). */
    bool         busreq_s;
} z80_t;

typedef enum {
    BUSOP_NONE = 0, BUSOP_M1, BUSOP_MRD, BUSOP_MWR, BUSOP_IORD, BUSOP_IOWR,
    BUSOP_INTA, BUSOP_INTERNAL
} z80_busop_t;

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */
void z80_init(z80_t *cpu);
void z80_reset(z80_t *cpu);
void z80_phase_step(z80_t *cpu);   /* advance one phase (half T-state)        */
void z80_set_pc(z80_t *cpu, uint16_t pc); /* seed PC + pending M1 fetch addr  */
/* the CPU as the program sees it: a copy with the register writes the die
   makes in the next M1 already made (wbp). A harness reading registers at
   an instruction boundary reads them here. Not reentrant (one static copy). */
const z80_t *z80_arch(const z80_t *cpu);

/* ---------------------------------------------------------------------------
 * Pin word API: the same phase engine, its pins packed in one 64-bit word so
 * a system's bus logic runs in the caller's loop, without callbacks:
 *
 *     uint64_t pins = 0;
 *     for (;;) {
 *         pins = z80_tick(&cpu, pins);                  // one phase
 *         uint16_t a = Z80_GET_ADDR(pins);
 *         uint8_t  d = 0;                               // undriven bus reads 0
 *         if ((pins & (Z80_MREQ|Z80_WR)) == (Z80_MREQ|Z80_WR)) mem[a] = Z80_GET_DATA(pins);
 *         if ((pins & (Z80_MREQ|Z80_RD)) == (Z80_MREQ|Z80_RD)) d = mem[a];
 *         ...                                           // I/O, INTA (M1|IORQ)
 *         Z80_SET_DATA(pins, d);
 *     }
 *
 * One tick is one phase (half T-state), the granularity of z80_phase_step()
 * and of the RTL: tick n drives exactly the pins z80_phase_step's n-th call
 * leaves in z80_pins_t, and the data byte the caller sets after tick n is
 * what the core latches on tick n+1. Bits are active high (1 = asserted, the
 * pin low). Positions follow floooh's chips z80.h where the pins are shared.
 * ------------------------------------------------------------------------- */
#define Z80_PIN_A0      0   /* A0..A15 out: address bus                      */
#define Z80_PIN_D0     16   /* D0..D7: in, the byte the system drives (read  */
                            /* data, INTA vector); out, the core's data-out  */
                            /* latch, on the bus while DOE                   */
#define Z80_PIN_M1     24   /* out                                           */
#define Z80_PIN_MREQ   25   /* out                                           */
#define Z80_PIN_IORQ   26   /* out                                           */
#define Z80_PIN_RD     27   /* out                                           */
#define Z80_PIN_WR     28   /* out                                           */
#define Z80_PIN_HALT   29   /* out                                           */
#define Z80_PIN_INT    30   /* in                                            */
#define Z80_PIN_RES    31   /* in: RESET                                     */
#define Z80_PIN_NMI    32   /* in                                            */
#define Z80_PIN_WAIT   33   /* in                                            */
#define Z80_PIN_RFSH   34   /* out                                           */
#define Z80_PIN_BUSREQ 35   /* in                                            */
#define Z80_PIN_BUSACK 36   /* out                                           */
#define Z80_PIN_DOE    39   /* out: the core drives D0..D7 (data_drive)      */

#define Z80_M1     (1ULL << Z80_PIN_M1)
#define Z80_MREQ   (1ULL << Z80_PIN_MREQ)
#define Z80_IORQ   (1ULL << Z80_PIN_IORQ)
#define Z80_RD     (1ULL << Z80_PIN_RD)
#define Z80_WR     (1ULL << Z80_PIN_WR)
#define Z80_HALT   (1ULL << Z80_PIN_HALT)
#define Z80_INT    (1ULL << Z80_PIN_INT)
#define Z80_RES    (1ULL << Z80_PIN_RES)
#define Z80_NMI    (1ULL << Z80_PIN_NMI)
#define Z80_WAIT   (1ULL << Z80_PIN_WAIT)
#define Z80_RFSH   (1ULL << Z80_PIN_RFSH)
#define Z80_BUSREQ (1ULL << Z80_PIN_BUSREQ)
#define Z80_BUSACK (1ULL << Z80_PIN_BUSACK)
#define Z80_DOE    (1ULL << Z80_PIN_DOE)
/* everything z80_tick drives; the other bits of the word pass through */
#define Z80_OUT_PIN_MASK (0xFFFFFFULL | Z80_M1 | Z80_MREQ | Z80_IORQ | Z80_RD | \
                          Z80_WR | Z80_HALT | Z80_RFSH | Z80_BUSACK | Z80_DOE)

#define Z80_GET_ADDR(p)    ((uint16_t)(p))
#define Z80_GET_DATA(p)    ((uint8_t)((p) >> Z80_PIN_D0))
#define Z80_SET_DATA(p, d) ((p) = ((p) & ~0xFF0000ULL) | ((uint64_t)((d) & 0xFF) << Z80_PIN_D0))

/* Advance one phase: the core samples D0..D7 and the INT / NMI / WAIT /
   BUSREQ / RES requests of `pins`, and returns the pins it drives in this
   phase, with the other bits of `pins` unchanged. */
uint64_t z80_tick(z80_t *cpu, uint64_t pins);

/* register-file byte accessors (docs/architecture.md) */
uint8_t  z80_get_r8(const z80_t *cpu, z80_reg8_t r); /* (HL) returns 0; mem handled by seq */
void     z80_set_r8(z80_t *cpu, z80_reg8_t r, uint8_t v);
uint16_t z80_pair_hi_lo(uint8_t hi, uint8_t lo);

/* ---------------------------------------------------------------------------
 * PLA decode — top-level module entry point (mirrors rtl/z80_pla.v).
 * Pure combinational; same shape as the Verilog module's port list.
 * ------------------------------------------------------------------------- */
z80_control_t z80_pla(z80_prefix_t prefix, uint8_t op);

/* Back-compat wrapper for callers still spelling it z80_pla_decode. */
z80_control_t z80_pla_decode(z80_prefix_t prefix, uint8_t opcode);

/* The decode ROM: z80_pla() of every (prefix, opcode), built by
   z80_pla_rom_init() (z80_reset calls it); the core's M1 latch reads it. */
#define Z80_PLA_ROM_PREFIXES 7
extern z80_control_t z80_pla_rom[Z80_PLA_ROM_PREFIXES][256];
void z80_pla_rom_init(void);

/* ---------------------------------------------------------------------------
 * Trace record (shared format, docs/timing.md / brief).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t cycle;
    uint8_t  phase;
    uint8_t  t_state;
    uint8_t  m_cycle;
    uint16_t pc;
    uint8_t  ir;
    uint8_t  prefix_state;
    uint16_t addr;
    uint8_t  data_out;
    uint8_t  data_in;
    uint8_t  mreq, iorq, rd, wr, m1, rfsh, halt, busack, wait, intr, nmi;
} z80_trace_rec_t;

void z80_trace_capture(const z80_t *cpu, z80_trace_rec_t *rec);
void z80_trace_header(void *fp);
void z80_trace_emit(void *fp, const z80_trace_rec_t *rec);

#endif /* Z80_H */

/* ============================================================================
 * z80.h - public types and API for the schematic-faithful Z80 C model.
 *
 * This header is the in-code half of the shared design contract (see
 * docs/architecture.md, docs/pla.md, docs/flags.md). Names mirror the Verilog
 * RTL: pins as pin_* fields, control lines as ctl_*, etc.
 * ==========================================================================*/
#ifndef Z80_H
#define Z80_H

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
       by SCF/CCF X/Y derivation on NMOS silicon. f_modified tracks whether
       z80_setF has been called during the currently-executing instruction;
       at the instruction boundary it commits to q. */
    uint8_t      q;
    bool         f_modified;

    /* BUSREQ release filter: pz80 settles for ~2 phases between busreq_n=1
       and the post-grant M1 fetch (prog15_busreq_m1 trace). Bus_granted
       entry edge is not filtered (pz80 enters bus grant promptly). */
    uint8_t      busreq_release_filter; /* phases of busreq_n=1 during grant    */
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

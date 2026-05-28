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
uint8_t z80_flags_scf(uint8_t a, uint8_t oldF);
uint8_t z80_flags_ccf(uint8_t a, uint8_t oldF);
uint8_t z80_flags_cpl(uint8_t a, uint8_t oldF, uint8_t *res);

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

/* How the F register is updated after the operation. */
typedef enum {
    FLAG_NONE = 0, FLAG_ADD8, FLAG_SUB8, FLAG_CP8, FLAG_LOGIC,
    FLAG_INC8, FLAG_DEC8, FLAG_ROT_A, FLAG_ROT, FLAG_BIT,
    FLAG_ADD16, FLAG_ADC16, FLAG_SBC16, FLAG_DAA, FLAG_SCF, FLAG_CCF,
    FLAG_CPL, FLAG_NEG, FLAG_BLOCK_LD, FLAG_BLOCK_CP, FLAG_BLOCK_IO
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
    EXEC_ILLEGAL
} z80_exec_t;

/* CB operation kind (= x field of the CB opcode) */
enum { CB_ROT = 0, CB_BIT = 1, CB_RES = 2, CB_SET = 3 };

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
    bool         nmi_pending;
    bool         prev_nmi_n;    /* for edge detection                        */

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
    bool         stalled;       /* wait detected at last .N sample            */

    uint64_t     cycle;         /* global half-step (phase) counter          */
    uint64_t     instr_count;   /* completed-instruction counter             */
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

/* register-file byte accessors (docs/architecture.md) */
uint8_t  z80_get_r8(const z80_t *cpu, z80_reg8_t r); /* (HL) returns 0; mem handled by seq */
void     z80_set_r8(z80_t *cpu, z80_reg8_t r, uint8_t v);
uint16_t z80_pair_hi_lo(uint8_t hi, uint8_t lo);

/* ---------------------------------------------------------------------------
 * PLA decode entry point.
 * ------------------------------------------------------------------------- */
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

/* ============================================================================
 * z80_regfile.c - register-file byte accessors over the 16-bit pair storage.
 * The r[] decode table maps an 8-bit register selector to a pair + half.
 * (HL) (selector 6) is a memory operand handled by the sequencer, not here.
 * ==========================================================================*/
#include "z80.h"

/* r[] selector -> backing pair index. index 6 ((HL)) is memory, marked 0xFF. */
static const uint8_t r8_pair[8] = {
    RFP_BC, RFP_BC, RFP_DE, RFP_DE, RFP_HL, RFP_HL, 0xFF, RFP_AF
};
/* r[] selector -> 1 if high byte of the pair, 0 if low byte. */
static const uint8_t r8_hi[8] = { 1, 0, 1, 0, 1, 0, 0, 1 };

uint8_t z80_get_r8(const z80_t *cpu, z80_reg8_t r)
{
    if (r == REG_iHL) return 0; /* memory operand: sequencer supplies it */
    uint16_t pair = cpu->rf[r8_pair[r]];
    return r8_hi[r] ? (uint8_t)(pair >> 8) : (uint8_t)(pair & 0xFF);
}

void z80_set_r8(z80_t *cpu, z80_reg8_t r, uint8_t v)
{
    if (r == REG_iHL) return;
    uint16_t *p = &cpu->rf[r8_pair[r]];
    if (r8_hi[r])
        *p = (uint16_t)((*p & 0x00FFu) | ((uint16_t)v << 8));
    else
        *p = (uint16_t)((*p & 0xFF00u) | v);
}

uint16_t z80_pair_hi_lo(uint8_t hi, uint8_t lo)
{
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

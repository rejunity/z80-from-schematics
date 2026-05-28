/* Unit tests for the nibble-structured ALU (cmodel/z80_alu.c). */
#include "z80.h"
#include "test_util.h"

int main(void)
{
    /* Exhaustively verify the nibble-twice add/sub matches reference 8-bit
       arithmetic for all (a, b, carry-in). This pins docs/alu.md's claim that
       the two-pass structure is bit-identical to a single 8-bit op. */
    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            for (int cin = 0; cin < 2; cin++) {
                /* ADD */
                z80_addsub_t r = z80_alu_add8((uint8_t)a, (uint8_t)b, (uint8_t)cin);
                unsigned sum = (unsigned)a + (unsigned)b + (unsigned)cin;
                unsigned half = (((unsigned)(a & 0xF) + (unsigned)(b & 0xF) + (unsigned)cin) >> 4) & 1;
                if (r.res != (uint8_t)sum || r.carry != ((sum >> 8) & 1) || r.half != half) {
                    CHECK(0, "add a=%02X b=%02X cin=%d res=%02X carry=%d half=%d",
                          a, b, cin, r.res, r.carry, r.half);
                    goto add_done;
                }
                /* SUB */
                z80_addsub_t s = z80_alu_sub8((uint8_t)a, (uint8_t)b, (uint8_t)cin);
                int diff = a - b - cin;
                int borrow = (diff < 0) ? 1 : 0;
                int hborrow = (((a & 0xF) - (b & 0xF) - cin) < 0) ? 1 : 0;
                if (s.res != (uint8_t)diff || s.carry != (uint8_t)borrow || s.half != (uint8_t)hborrow) {
                    CHECK(0, "sub a=%02X b=%02X cin=%d res=%02X borrow=%d half=%d",
                          a, b, cin, s.res, s.carry, s.half);
                    goto add_done;
                }
            }
        }
    }
    CHECK(1, "exhaustive add/sub nibble unit matches reference");
add_done:

    /* logic ops */
    CHECK_EQ_U(z80_alu_logic(ALU_AND, 0xF0, 0x0F), 0x00, "AND");
    CHECK_EQ_U(z80_alu_logic(ALU_OR,  0xF0, 0x0F), 0xFF, "OR");
    CHECK_EQ_U(z80_alu_logic(ALU_XOR, 0xFF, 0x0F), 0xF0, "XOR");

    /* parity: even number of set bits => 1 */
    CHECK_EQ_U(z80_parity(0x00), 1, "parity 0");
    CHECK_EQ_U(z80_parity(0x01), 0, "parity 1");
    CHECK_EQ_U(z80_parity(0x03), 1, "parity 3");
    CHECK_EQ_U(z80_parity(0xFF), 1, "parity FF");
    CHECK_EQ_U(z80_parity(0x7F), 0, "parity 7F");

    TEST_SUMMARY();
}

/* Pin-driven interrupt device: no data until M1 and IORQ overlap. */
#include "z80_sim.h"
#include "test_util.h"

static void run_case(unsigned mode, uint8_t opcode, unsigned extra_wait, bool from_halt)
{
    z80_system_t s; z80_sys_init(&s);
    const uint8_t program[] = {0x31,0,0x90,0xed,0x46,0x3e,0x80,0xed,0x47,0xfb,0};
    z80_sys_load(&s,0,program,sizeof program);
    s.mem[4] = mode == 0 ? 0x46 : mode == 1 ? 0x56 : 0x5e;
    if(from_halt) s.mem[10]=0x76;
    s.mem[0x8034]=0; s.mem[0x8035]=2;
    uint16_t target=(mode==1 || (mode==0 && opcode==0xff)) ? 0x38 : 0x200;
    s.mem[target]=0x76;
    unsigned overlaps=0, refreshes=0, waits=0;
    bool seen=false, reached=false;
    uint16_t ret=0;
    uint8_t ack_r=0;
    for(unsigned n=0;n<600 && !reached;n++) {
        z80_t *c=&s.cpu;
        if(c->iff1 && (!from_halt || c->halted)) c->pins.int_n=0;
        if(c->bus_op==BUSOP_INTA) {
            if(!seen) {
                seen=true; ret=c->pins.addr; ack_r=c->reg_r;
                if(mode==0 && (opcode==0xcd || opcode==0xc3)) {
                    s.mem[ret]=0; s.mem[(uint16_t)(ret+1)]=2;
                }
                if(mode==0 && opcode==0) {target=ret; s.mem[target]=0x76;}
            }
            CHECK(c->pins.rd_n && c->pins.wr_n,"ack does not assert RD/WR");
            CHECK(c->pins.iorq_n || !c->pins.m1_n,"IORQ overlaps M1");
            if(!c->pins.iorq_n) overlaps++;
            if(!c->pins.rfsh_n) refreshes++;
            c->pins.wait_n=1;
            if(c->t_state==4 && c->phi==0 && waits<extra_wait) {
                c->pins.wait_n=0; waits++;
            }
            // Poison the bus until the device is ready, and outside ack.
            c->pins.data_in=(!c->pins.m1_n && !c->pins.iorq_n && c->pins.wait_n) ? opcode : 0;
        }
        if(seen && c->bus_op==BUSOP_M1 && !c->pins.rd_n && c->pins.addr==target) reached=true;
        z80_sys_phase(&s);
    }
    CHECK(seen && reached,"mode %u opcode %02x reaches target",mode,opcode);
    CHECK_EQ_U(overlaps,3+2*extra_wait,"ack pulse duration");
    CHECK_EQ_U(s.cpu.reg_r,ack_r+1,"ack increments R once");
    CHECK_EQ_U(refreshes,4,"two refresh T states");
    CHECK_EQ_U(waits,extra_wait,"inserted waits");
    uint16_t sp=(mode==0 && (opcode==0xc3 || opcode==0)) ? 0x9000 : 0x8ffe;
    CHECK_EQ_U(s.cpu.rf[RFP_SP],sp,"stack depth");
    if(sp==0x8ffe) {
        if(mode==0 && opcode==0xcd) ret=(uint16_t)(ret+2);
        CHECK_EQ_U(s.mem[0x8ffe] | (s.mem[0x8fff]<<8),ret,"return address");
    }
}
int main(void)
{
    for(unsigned mode=0;mode<3;mode++) {
        run_case(mode,mode==2?0x34:0xff,0,false);
        run_case(mode,mode==2?0x34:0xff,3,false);
        run_case(mode,mode==2?0x34:0xff,3,true);
    }
    run_case(0,0xcd,3,false); run_case(0,0xc3,3,false); run_case(0,0,3,false);
    TEST_SUMMARY();
}

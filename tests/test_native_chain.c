/* SPDX-License-Identifier: MIT */
#include "cpu/cpu.h"
#include "cpu/native_chain_inline.h"
#include <stdio.h>
#include <string.h>
void func_80005000(CPUState*);
void func_80006000(CPUState*);
static DrNativeChainFn bindings[3*DR_NATIVE_CHAIN_MEM1_PAGES];
static uint8_t excluded[DR_NATIVE_CHAIN_MEM1_PAGES];
static DrNativeChain state,other;
static uint64_t dispatches;
static unsigned observations,max_depth,mode;
static bool halted;
static uint32_t observed_pc[16],observed_r3[16];
static uint64_t observed_cycles[16];
static const unsigned slot_a=DR_NATIVE_CHAIN_MEM1_PAGES+5;
static const unsigned slot_b=DR_NATIVE_CHAIN_MEM1_PAGES+6;
#define CHECK(value) do {if(!(value)){fprintf(stderr,"chain check line %u: %s\n",__LINE__,#value);return 1;}}while(0)

static bool observe(const CPUState* cpu,void* user) {
    if(user!=&state)return false;
    if(observations<16){observed_pc[observations]=cpu->pc;observed_cycles[observations]=cpu->cycles;observed_r3[observations]=cpu->gpr[3];}
    ++observations;if(state.depth>max_depth)max_depth=state.depth;
    if(mode==1)return false;
    if(mode==2){if(dr_native_chain_bind(&other))return false;bindings[slot_b]=NULL;}
    if(mode==3)state.admitted=false;
    return true;
}
static void setup(CPUState* cpu) {
    memset(bindings,0,sizeof bindings);memset(excluded,0,sizeof excluded);
    memset(&state,0,sizeof state);memset(&other,0,sizeof other);
    dispatches=observations=max_depth=mode=0;
    cpu->pc=0x80005000;cpu->lr=0x80007000;cpu->cycles=100;
    cpu->cycle_deadline=10000;cpu->exception=0;halted=false;cpu->gpr[3]=0;
    bindings[slot_a]=func_80005000;bindings[slot_b]=func_80006000;
    state.cpu=cpu;state.halted=&halted;state.entries=bindings;state.entry_count=sizeof bindings/sizeof *bindings;
    state.excluded_pages=excluded;state.dispatches=&dispatches;
    state.before=observe;state.user=&state;
    (void)dr_native_chain_bind(&state);state.admitted=true;
}
static void namespace_body(CPUState* cpu){++cpu->cycles;++cpu->gpr[7];cpu->pc=cpu->lr;}

int main(void) {
    CPUState cpu,initial,reference;if(!cpu_init(&cpu))return 1;
    setup(&cpu);initial=cpu;state.admitted=false;
    func_80005000(&cpu);func_80006000(&cpu);reference=cpu;
    cpu=initial;state.admitted=true;func_80005000(&cpu);
    CHECK(!memcmp(&cpu,&reference,sizeof cpu));
    CHECK(cpu.pc==0x80007000 && cpu.cycles==104 && cpu.gpr[3]==2 && dispatches==1);
    CHECK(observations==1 && observed_pc[0]==0x80006000 && observed_cycles[0]==102 && observed_r3[0]==1 && state.depth==0);

    setup(&cpu);cpu.pc=0x80005004;func_80005000(&cpu);
    CHECK(cpu.gpr[3]==1 && cpu.cycles==103 && cpu.pc==cpu.lr && dispatches==1);
    setup(&cpu);cpu.pc=0x80005008;func_80005000(&cpu);
    CHECK(cpu.cycles==100 && dispatches==0 && observations==0);

    setup(&cpu);state.stop_pc=0x80006000;func_80005000(&cpu);
    CHECK(cpu.gpr[3]==1 && cpu.pc==state.stop_pc && !dispatches && !observations);
    setup(&cpu);cpu.cycle_deadline=102;func_80005000(&cpu);
    CHECK(cpu.cycles==102 && cpu.pc==0x80006000 && !dispatches && !observations);
    setup(&cpu);state.admitted=false;func_80005000(&cpu);
    CHECK(cpu.gpr[3]==1 && cpu.pc==0x80006000 && !dispatches);
    setup(&cpu);CHECK(dr_native_chain_bind(NULL));func_80005000(&cpu);
    CHECK(cpu.gpr[3]==1 && cpu.pc==0x80006000 && !dispatches);

    setup(&cpu);excluded[6]=1;func_80005000(&cpu);CHECK(cpu.gpr[3]==1 && !dispatches && !observations);
    setup(&cpu);bindings[slot_b]=NULL;func_80005000(&cpu);CHECK(cpu.gpr[3]==1 && !dispatches);
    setup(&cpu);state.entry_count=slot_b;func_80005000(&cpu);CHECK(cpu.gpr[3]==1 && !dispatches);
    setup(&cpu);mode=1;func_80005000(&cpu);CHECK(cpu.gpr[3]==1 && !dispatches && observations==1 && !state.depth);
    setup(&cpu);mode=2;func_80005000(&cpu);
    CHECK(cpu.gpr[3]==1 && !dispatches && observations==1 && !state.depth && dr_native_chain_state==&state);
    setup(&cpu);mode=3;func_80005000(&cpu);CHECK(cpu.gpr[3]==1 && !dispatches && !state.admitted);

    setup(&cpu);cpu.lr=0x80005000;func_80005000(&cpu);
    CHECK(cpu.gpr[3]==9 && cpu.cycles==118 && cpu.pc==0x80006000);
    CHECK(dispatches==8 && observations==8 && max_depth==8 && state.depth==0);
    for(unsigned i=0;i<8;++i)CHECK(observed_cycles[i]==102u+2u*i && observed_r3[i]==i+1u);

    /* Different CPU, exceptions, deadlines, and non-MEM1/unaligned PCs never
     * enter a cached body. Aliases select independent entries without changing
     * the guest-visible PC passed to the original boundary observer. */
    setup(&cpu);state.cpu=&reference;CHECK(!dr_native_chain_next(&cpu));state.cpu=&cpu;
    halted=true;CHECK(!dr_native_chain_next(&cpu));halted=false;
    cpu.exception=PPC_EXC_DSI;CHECK(!dr_native_chain_next(&cpu));cpu.exception=0;
    cpu.cycle_deadline=0;CHECK(!dr_native_chain_next(&cpu));cpu.cycle_deadline=10000;
    const uint32_t bad[]={0x80005001,0x81800000,0x90005000,0xe0005000};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i){cpu.pc=bad[i];CHECK(!dr_native_chain_next(&cpu));}
    const uint32_t aliases[]={0x00005000,0x80005000,0xc0005000};
    for(unsigned i=0;i<3;++i) {
        setup(&cpu);cpu.pc=aliases[i];cpu.gpr[7]=0;bindings[i*DR_NATIVE_CHAIN_MEM1_PAGES+5]=namespace_body;
        CHECK(dr_native_chain_next(&cpu));CHECK(cpu.gpr[7]==1 && cpu.pc==cpu.lr);
        CHECK(dispatches==1 && observations==1 && observed_pc[0]==aliases[i]);
    }
    CHECK(dr_native_chain_bind(&other) && !other.admitted && !state.admitted);
    CHECK(dr_native_chain_bind(NULL));cpu_free(&cpu);
    puts("Native chaining: emitted differential and boundary checks passed.");return 0;
}

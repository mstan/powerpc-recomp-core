/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "cpu/cpu.h"

#define DECL(pc) void func_##pc(CPUState*);
DECL(80004400) DECL(80004420) DECL(80004440) DECL(80004460)
DECL(80004480) DECL(800044A0) DECL(800044C0) DECL(800044E0)
static void (*const functions[])(CPUState*)={
    func_80004400,func_80004420,func_80004440,func_80004460,
    func_80004480,func_800044A0,func_800044C0,func_800044E0
};
typedef struct Bus {
    u32 address,cia,reads,writes;
    u8 width;
    u64 value,cycles;
} Bus;
static u64 bus_read(CPUState* cpu,u32 address,u8 width) {
    Bus* b=cpu->external_user_data;
    ++b->reads;b->address=address;b->cia=cpu->pc;
    b->width=width;b->cycles=cpu->cycles;
    return 0x0123456789abcdefull;
}
static void bus_write(CPUState* cpu,u32 address,u64 value,u8 width) {
    Bus* b=cpu->external_user_data;
    ++b->writes;b->address=address;b->cia=cpu->pc;
    b->width=width;b->value=value;b->cycles=cpu->cycles;
}
static void setup(CPUState* cpu,u8* backing,Bus* bus,unsigned config,
                  u32 pc,u32 address,u32 reservation,unsigned entry) {
    cpu->mem2=backing;cpu->mem2_size=64;cpu_reset(cpu);
    memset(bus,0,sizeof *bus);
    for(unsigned i=0;i<64;++i)backing[i]=(u8)(i*37u+11u);
    cpu->mem2=config==3?NULL:backing;
    cpu->mem2_size=config==0 || config==3?64:config==1?3:0;
    cpu->external_read=bus_read;cpu->external_write=bus_write;
    cpu->external_user_data=bus;
    cpu->pc=pc+entry*4u;cpu->lr=0x81234560u;
    cpu->cycles=100;cpu->cycle_deadline=1000000;
    cpu->msr=0x2000u;cpu->gpr[3]=0xcafebabeu;cpu->gpr[4]=address;
    const u64 bits=0xdeadbeef01234567ull;
    memcpy(&cpu->fpr[3],&bits,8);
    cpu->reserve_valid=true;cpu->reserve_addr=reservation;
}
static void reference(CPUState* cpu,unsigned op,u32 pc,unsigned entry) {
    const u32 cia=pc+4u,address=cpu->gpr[4];
    if(!entry){++cpu->gpr[6];++cpu->cycles;}
    u64 value;
    switch(op) {
    case 0:cpu->gpr[3]=mem_read8(cpu,address,cia);break;
    case 1:cpu->gpr[3]=mem_read16(cpu,address,cia);break;
    case 2:cpu->gpr[3]=mem_read32(cpu,address,cia);break;
    case 3:value=mem_read64(cpu,address,cia);memcpy(&cpu->fpr[3],&value,8);break;
    case 4:mem_write8(cpu,address,(u8)cpu->gpr[3],cia);break;
    case 5:mem_write16(cpu,address,(u16)cpu->gpr[3],cia);break;
    case 6:mem_write32(cpu,address,cpu->gpr[3],cia);break;
    case 7:memcpy(&value,&cpu->fpr[3],8);mem_write64(cpu,address,value,cia);break;
    }
    cpu->cycles+=2;cpu->pc=cpu->lr;
}
static int equal(const CPUState* a,const CPUState* b) {
    return !memcmp(a->gpr,b->gpr,sizeof a->gpr) &&
        !memcmp(a->fpr,b->fpr,sizeof a->fpr) &&
        !memcmp(a->ps1,b->ps1,sizeof a->ps1) &&
        a->pc==b->pc && a->lr==b->lr && a->cycles==b->cycles &&
        a->msr==b->msr && a->fpscr==b->fpscr &&
        a->exception==b->exception && a->reserve_valid==b->reserve_valid &&
        a->reserve_addr==b->reserve_addr;
}
int main(void) {
    CPUState actual,expected;
    if(!cpu_init(&actual))return 1;
    if(!cpu_init(&expected)){cpu_free(&actual);return 1;}
    if(!cpu_alloc_mem2(&actual,64) || !cpu_alloc_mem2(&expected,64)) {
        cpu_free(&actual);cpu_free(&expected);return 1;
    }
    u8* const a=actual.mem2;u8* const b=expected.mem2;
    const u32 aliases[]={0x10000000u,0x90000000u,0xd0000000u};
    const u32 offsets[]={0,1,2,3,7,28,31,32,56,60,62,63,64};
    unsigned cases=0;int ok=1;
    for(unsigned config=0;config<4 && ok;++config)
    for(unsigned bank=0;bank<3 && ok;++bank)
    for(unsigned op=0;op<8 && ok;++op)
    for(unsigned offset=0;offset<sizeof offsets/sizeof *offsets && ok;++offset)
    for(unsigned reserve=0;reserve<3 && ok;++reserve)
    for(unsigned entry=0;entry<2 && ok;++entry) {
        const u32 pc=0x80004400u+op*0x20u;
        const u32 address=aliases[bank]+offsets[offset];
        const u32 reservation=aliases[reserve]+(offsets[offset]&~31u);
        Bus ba,bb;
        setup(&actual,a,&ba,config,pc,address,reservation,entry);
        setup(&expected,b,&bb,config,pc,address,reservation,entry);
        functions[op](&actual);reference(&expected,op,pc,entry);
        ++cases;
        if(!equal(&actual,&expected) || memcmp(a,b,64) || memcmp(&ba,&bb,sizeof ba)) {
            fprintf(stderr,"MEM2 config=%u bank=%u op=%u offset=%u reserve=%u entry=%u pc=%08x/%08x cycles=%llu/%llu\n",
                config,bank,op,offsets[offset],reserve,entry,actual.pc,expected.pc,
                (unsigned long long)actual.cycles,(unsigned long long)expected.cycles);
            ok=0;
        }
    }
    /* These non-MEM2 addresses must keep their ordinary bus path. */
    const u32 devices[]={0x50000000u,0xcc008000u,0xe0000000u,0xffffffffu};
    for(unsigned address=0;address<4 && ok;++address)
    for(unsigned op=0;op<8 && ok;++op) {
        Bus ba,bb;const u32 pc=0x80004400u+op*0x20u;
        setup(&actual,a,&ba,0,pc,devices[address],aliases[0],0);
        setup(&expected,b,&bb,0,pc,devices[address],aliases[0],0);
        functions[op](&actual);reference(&expected,op,pc,0);++cases;
        ok=equal(&actual,&expected) && !memcmp(a,b,64) && !memcmp(&ba,&bb,sizeof ba);
    }
    actual.mem2=a;expected.mem2=b;cpu_free(&actual);cpu_free(&expected);
    if(ok)printf("MEM2 emitted/reference exact: %u cases (aliases, bounds, reservations, bus cycles, arbitrary entries)\n",cases);
    return ok?0:1;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include <stdio.h>

void func_80004100(CPUState*);
static u64 read_cycle,write_cycle;
static u32 read_pc,write_pc;
static unsigned reads,writes;
static bool failed;
static bool mutate_registers;
static u64 read_device(CPUState* cpu,u32 address,u8 width) {
    if(address!=0xcc006c00u || width!=4)failed=true;
    if(mutate_registers) {
        if(cpu->gpr[3]!=1)failed=true;
        cpu->gpr[3]=10;cpu->gpr[4]=0xdead;cpu->gpr[9]=99;
    }
    read_cycle=cpu->cycles;read_pc=cpu->pc;++reads;return 0x12345678;
}
static void write_device(CPUState* cpu,u32 address,u64 value,u8 width) {
    if(address!=0xcc006c04u || width!=4 || value!=0x12345678)failed=true;
    if(mutate_registers) {
        if(cpu->gpr[3]!=11 || cpu->gpr[4]!=0x12345678 || cpu->gpr[9]!=99)failed=true;
        cpu->gpr[3]=20;cpu->gpr[4]=0xbeef;cpu->gpr[10]=100;
    }
    write_cycle=cpu->cycles;write_pc=cpu->pc;++writes;
}

int main(void) {
    CPUState cpu;if(!cpu_init(&cpu))return 1;
    cpu.external_read=read_device;cpu.external_write=write_device;
    cpu.pc=0x80004100;cpu.lr=0x81234564;cpu.cycles=100;
    cpu.gpr[5]=0xcc006c00;cpu.cycle_deadline=10000;
    func_80004100(&cpu);
    /* addi=1, lwz=1, addi=1, stw=1, blr=1 in the pinned cycle model. */
    int ok=!failed && reads==1 && writes==1 && read_cycle==101 &&
        write_cycle==103 && read_pc==0x80004104 && write_pc==0x8000410c &&
        cpu.cycles==105 && cpu.pc==cpu.lr && cpu.gpr[3]==2;
    if(!ok)fprintf(stderr,"observed read=%llu write=%llu exit=%llu r3=%u\n",
        (unsigned long long)read_cycle,(unsigned long long)write_cycle,
        (unsigned long long)cpu.cycles,cpu.gpr[3]);
    /* The entry table subtracts the skipped instruction prefix. */
    reads=writes=0;cpu.pc=0x80004104;cpu.cycles=100;cpu.gpr[3]=0;
    func_80004100(&cpu);
    ok=ok && !failed && reads==1 && writes==1 && read_cycle==100 &&
        write_cycle==102 && cpu.cycles==104 && cpu.gpr[3]==1;

    /* A callback sees the pure prefix, can mutate any register, and its
     * mutations survive the load destination and subsequent store/return. */
    mutate_registers=true;reads=writes=0;cpu.pc=0x80004100;cpu.cycles=100;
    cpu.gpr[3]=0;cpu.gpr[5]=0xcc006c00;
    func_80004100(&cpu);
    ok=ok && !failed && reads==1 && writes==1 && read_cycle==101 &&
        write_cycle==103 && cpu.cycles==105 && cpu.gpr[3]==20 &&
        cpu.gpr[4]==0xbeef && cpu.gpr[9]==99 && cpu.gpr[10]==100;

    /* An unusual RAM/CPUState alias must retain the canonical implementation.
     * The load aliases r3 and therefore must observe the preceding addi. */
    u8* ram=cpu.ram;cpu.ram=(u8*)&cpu;cpu.external_read=NULL;cpu.external_write=NULL;
    cpu.pc=0x80004100;cpu.cycles=100;cpu.gpr[3]=0;cpu.gpr[5]=GC_RAM_BASE+12;
    func_80004100(&cpu);
    /* The big-endian load/store round trip returns the aliased host word to
     * its original byte order. A stale local prefix would instead store zero. */
    ok=ok && cpu.gpr[3]==2 && cpu.gpr[4]==1 && cpu.cycles==105;
    cpu.ram=ram;
    cpu_free(&cpu);return !ok;
}

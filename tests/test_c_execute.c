#include <stdio.h>
#include <math.h>
#include <string.h>

#include "../src/cpu/cpu.h"

/* PORT NOTE (gcnrecomp, fix/gekko-fp-and-reservations): upstream's CPUState
 * carries `s64 downcount` (a fixed cycle budget decremented per basic block,
 * emitter.c:1947/1958) and resets it to 0 before every outer re-entry so each
 * call gets a fresh budget before the emitted code yields back with an
 * updated ctx->pc. Our CPUState has no `downcount` field at all -- we use a
 * different, deliberately-kept architecture (`u64 cycles` / `u64
 * cycle_deadline`, derived per-instruction Dolphin-model costs; see
 * docs/UPSTREAM_SYNC_ASSESSMENT.md section 2d) where a backward branch stays
 * in-function via `goto` only while `cycles < cycle_deadline`, and a
 * zero-initialized deadline (as cpu_init's memset leaves it) makes that
 * comparison false on the very first check, so EVERY backward branch would
 * exit to the caller after a single loop iteration instead of completing the
 * loop natively. Every `cpu.downcount = 0;` reset below is therefore replaced
 * with arming a generously large `cycle_deadline` so these fixtures behave
 * like upstream's fresh-budget reset: the loop runs to completion in (at
 * most) a handful of calls, well inside the calls<20/calls<24 bounds carried
 * over unmodified from upstream. This does not touch emitter.c or cpu.c. */
#define TEST_C_EXECUTE_CYCLE_BUDGET 1000000u

void func_80004020(CPUState* ctx);
void func_80004040(CPUState* ctx);
void func_80004060(CPUState* ctx);
void func_80004068(CPUState* ctx);
void func_80004070(CPUState* ctx);
void func_80004080(CPUState* ctx);
void func_80004200(CPUState* ctx);
void func_80004240(CPUState* ctx);
void func_80004260(CPUState* ctx);
void func_80004280(CPUState* ctx);
void func_800042A0(CPUState* ctx);

static u64 bits_of(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static u32 fp_bus_reads;static u64 fp_bus_cycles;
static u64 fp_bus_disable(CPUState* cpu,u32 address,u8 size) {
    if(address!=0xcc000000u || size!=4)return 0;
    ++fp_bus_reads;fp_bus_cycles=cpu->cycles;cpu->msr=0;return 0x11223344u;
}
static void fp_start(CPUState* cpu,u32 pc,u32 msr) {
    cpu->pc=pc;cpu->msr=msr;cpu->cycles=100;cpu->cycle_deadline=1000000;
    cpu->exception=0;cpu->fpscr=0;cpu->srr0=cpu->srr1=0xdeadbeefu;
    cpu->lr=0x81234564u;cpu->hid2=0;
    cpu->fpr[1]=1.25;cpu->fpr[2]=2.5;
    for(unsigned n=3;n<=6;++n)cpu->fpr[n]=cpu->ps1[n]=9;
    cpu->external_read=NULL;cpu->gpr[5]=0;
}
static int fp_scope_contract(CPUState* cpu) {
    fp_start(cpu,0x80004200u,0x2000u);func_80004200(cpu);
    if(cpu->pc!=cpu->lr || cpu->cycles!=105 || cpu->exception ||
       cpu->fpr[3]!=3.75 || cpu->fpr[4]!=3.125 || cpu->fpr[5]!=6.875 || cpu->fpr[6]!=-6.875)return 0;
    for(unsigned n=0;n<4;++n) {
        const u32 cia=0x80004200u+n*4;
        fp_start(cpu,cia,0);func_80004200(cpu);
        if(cpu->exception!=PPC_EXC_FP_UNAVAILABLE || cpu->srr0!=cia ||
           cpu->pc!=PPC_VECTOR_FP_UNAVAIL || cpu->cycles!=101 || cpu->fpr[6]!=9)return 0;
    }
    const f64 resumed_result[]={-6.875,-12.125,-18,-9};
    for(unsigned n=0;n<4;++n) {
        fp_start(cpu,0x80004200u+n*4,0x2000u);func_80004200(cpu);
        if(cpu->exception || cpu->pc!=cpu->lr || cpu->cycles!=105-n || cpu->fpr[6]!=resumed_result[n])return 0;
    }
    fp_start(cpu,0x80004240u,0x2000u);func_80004240(cpu);
    if(cpu->exception!=PPC_EXC_FP_UNAVAILABLE || cpu->srr0!=0x80004248u ||
       cpu->cycles!=103 || cpu->fpr[3]!=3.75 || cpu->fpr[6]!=9)return 0;
    fp_start(cpu,0x80004260u,0x2000u);cpu->external_read=fp_bus_disable;
    cpu->gpr[5]=0xcc000000u;fp_bus_reads=0;func_80004260(cpu);
    if(cpu->exception!=PPC_EXC_FP_UNAVAILABLE || cpu->srr0!=0x80004268u ||
       cpu->cycles!=103 || cpu->gpr[4]!=0x11223344u || fp_bus_reads!=1 || cpu->fpr[6]!=9)return 0;
#if defined(DOLRECOMP_OBSERVE_CYCLES) && DOLRECOMP_OBSERVE_CYCLES
    if(fp_bus_cycles!=101)return 0;
#else
    if(fp_bus_cycles!=100)return 0;
#endif
    fp_start(cpu,0x80004280u,0x2000u);func_80004280(cpu);
    if(cpu->exception!=PPC_EXC_PROGRAM || cpu->srr0!=0x80004284u ||
       cpu->cycles!=102 || cpu->fpr[6]!=9)return 0;
    /* FP unavailable still outranks the PSQ HID2 program fault on an interior
     * dispatch entry, even though normal fallthrough inherits the FP proof. */
    fp_start(cpu,0x80004284u,0);func_80004280(cpu);
    if(cpu->exception!=PPC_EXC_FP_UNAVAILABLE || cpu->srr0!=0x80004284u ||cpu->cycles!=101)return 0;
    for(unsigned deadline=0;deadline<2;++deadline) {
        fp_start(cpu,0x800042a0u,0x2000u);cpu->cycle_deadline=deadline?101:1000000;
        unsigned calls=0;
        while(!cpu->exception && calls++<8)func_800042A0(cpu);
        if(cpu->exception!=PPC_EXC_FP_UNAVAILABLE || cpu->srr0!=0x800042a8u ||
           cpu->cycles!=105 || cpu->fpr[6]!=9)return 0;
    }
    cpu->external_read=NULL;
    puts("FP guard exactness: arbitrary entry, MSR/bus changes, exception priority, shadow calls and deadlines passed");
    return 1;
}

int main(void) {
    CPUState cpu;
    if (!cpu_init(&cpu))
        return 1;

    cpu.pc = 0x80004020u;
    cpu.lr = 0x81234564u;
    cpu.gpr[3] = 1000;

    u32 calls = 0;
    while (cpu.pc != cpu.lr && calls < 32) {
        cpu.cycles = 0;
        cpu.cycle_deadline = TEST_C_EXECUTE_CYCLE_BUDGET;
        func_80004020(&cpu);
        calls++;
    }

    int integer_ok = cpu.pc == cpu.lr && cpu.gpr[3] == 0 &&
                     (cpu.cr & 0xF0000000u) == 0x20000000u && calls < 20;
    if (!integer_ok) {
        fprintf(stderr, "pc=%08X r3=%u cr=%08X calls=%u\n",
                cpu.pc, cpu.gpr[3], cpu.cr, calls);
    }
    for (u32 i = 0; i < 1000; ++i)
        mem_write32(&cpu, 0x80001000u + i * 4u, i);
    cpu.pc = 0x80004040u;
    cpu.lr = 0x81234564u;
    cpu.gpr[3] = 1000;
    cpu.gpr[5] = 0x80001000u;
    calls = 0;
    while (cpu.pc != cpu.lr && calls < 32) {
        cpu.cycles = 0;
        cpu.cycle_deadline = TEST_C_EXECUTE_CYCLE_BUDGET;
        func_80004040(&cpu);
        calls++;
    }
    int memory_ok = cpu.pc == cpu.lr && cpu.gpr[3] == 0 &&
                    cpu.gpr[4] == 999 && cpu.gpr[5] == 0x80001FA0u &&
                    calls < 24;
    if (!memory_ok) {
        fprintf(stderr, "memory pc=%08X r3=%u r4=%u r5=%08X calls=%u\n",
                cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], calls);
    }

    cpu.msr = 0x00002000u;
    cpu.lr = 0x81234564u;
    cpu.fpscr = 0;
    cpu.fpr[1] = NAN;
    cpu.fpr[2] = 1.0;
    cpu.pc = 0x80004060u;
    func_80004060(&cpu);
    int compare_ok = (cpu.fpscr & 0x00080000u) != 0 &&
                     ((cpu.fpscr >> 12) & 0xFu) == 1u &&
                     ((cpu.cr >> 20) & 0xFu) == 1u;

    cpu.fpscr = 0xA0000000u;
    cpu.cr = 0;
    cpu.fpr[1] = 1.25;
    cpu.fpr[2] = 2.5;
    cpu.pc = 0x80004068u;
    func_80004068(&cpu);
    int record_ok = cpu.fpr[3] == 3.75 && cpu.ps1[3] == 3.75 &&
                    ((cpu.cr >> 24) & 0xFu) == 0xAu;

    cpu.fpr[1] = 0x1.0000000000001p+0;
    cpu.fpr[2] = -0x1.0000000000001p+0;
    u64 merge_a = bits_of(cpu.fpr[1]);
    u64 merge_b = bits_of(cpu.fpr[2]);
    cpu.pc = 0x80004070u;
    func_80004070(&cpu);
    int merge_ok = bits_of(cpu.fpr[5]) == merge_a &&
                   bits_of(cpu.ps1[5]) == merge_b;

    if (!compare_ok || !record_ok || !merge_ok) {
        fprintf(stderr, "float compare=%d record=%d merge=%d fpscr=%08X cr=%08X\n",
                compare_ok, record_ok, merge_ok, cpu.fpscr, cpu.cr);
    }

    cpu.fpr[1] = 2.0; cpu.ps1[1] = 3.0;
    cpu.fpr[2] = 4.0; cpu.ps1[2] = 11.0;
    cpu.fpr[3] = 9.0;
    cpu.fpr[4] = 4.0;
    cpu.fpr[5] = 2.0; cpu.ps1[5] = 3.0;
    cpu.fpr[6] = 4.0;
    cpu.fpr[7] = 2.0; cpu.ps1[7] = 3.0;
    cpu.fpr[8] = 1.0; cpu.ps1[8] = 2.0;
    cpu.pc = 0x80004080u;
    func_80004080(&cpu);
    int overlap_ok = cpu.fpr[1] == 9.0 && cpu.ps1[1] == 13.0 &&
                     cpu.fpr[4] == 8.0 && cpu.ps1[4] == 12.0 &&
                     cpu.fpr[6] == 9.0 && cpu.ps1[6] == 14.0;
    if (!overlap_ok)
        fprintf(stderr, "paired overlap sum=%g,%g mul=%g,%g madd=%g,%g\n",
                cpu.fpr[1],cpu.ps1[1],cpu.fpr[4],cpu.ps1[4],cpu.fpr[6],cpu.ps1[6]);
    const int fp_scope_ok=fp_scope_contract(&cpu);
    if(!fp_scope_ok)fprintf(stderr,"FP scope pc=%08X srr0=%08X exception=%x cycles=%llu\n",
        cpu.pc,cpu.srr0,cpu.exception,(unsigned long long)cpu.cycles);
    cpu_free(&cpu);
    return !(integer_ok && memory_ok && compare_ok && record_ok && merge_ok && overlap_ok && fp_scope_ok);
}

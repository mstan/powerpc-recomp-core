/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu.h"

extern void func_80004000(CPUState* ctx);
extern void func_80005000(CPUState* ctx);
static u64 skipped;

#define BASE        0x80004000u
#define SENTINEL_PC 0xDEAD1000u
#define POLL_ADDR   (GC_RAM_BASE + 0x100u)
#define CR2_MASK    0x00F00000u

bool ppc_idle_can_skip(CPUState* cpu,u32 address) {
    return (cpu->msr&0x8000u) && address==POLL_ADDR;
}
void ppc_idle_note_cycles(u64 cycles) {skipped+=cycles;}

typedef struct Result {
    u32 pc;
    u32 value;
    u32 cr;
    u64 cycles;
    int calls;
    u64 skipped;
} Result;

static void store_be32(u8* p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

/* Generated code only needs this symbol on the ordinary fallback path. */
u32 mem_read32_cia(CPUState* cpu, u32 addr, u32 cia) {
    (void)cia;
    if (addr < GC_RAM_BASE || addr > GC_RAM_BASE + cpu->ram_size - 4u)
        return 0;
    const u8* p = cpu->ram + (addr - GC_RAM_BASE);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int run_scenario(const char* name,int uniform,int signed_compare,int ee,
                        u64 deadline,u32 exit_value,Result* result) {
    CPUState ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ram_size = GC_MAIN_RAM_SIZE;
    ctx.ram = (u8*)calloc(1, ctx.ram_size);
    if (!ctx.ram) {
        printf("POLL_LOOP,%s,FAIL,allocation\n", name);
        return 1;
    }

    ctx.pc = signed_compare?BASE+0x1000:BASE;
    ctx.msr=ee?0x8000u:0;
    skipped=0;
    ctx.gpr[3] = POLL_ADDR;
    ctx.cr = 0xA50A5A5Au;       /* prove only cr2 is replaced */
    ctx.xer = 0x80000000u;      /* prove SO propagation */
    store_be32(ctx.ram + 0x100u, 0u);

    int calls = 0;
    while (ctx.pc != SENTINEL_PC && calls < 8) {
        /* The fast run consumes both zero iterations inside its first call;
         * the fallback run consumes one per call.  Change the observed word
         * after exactly six guest cycles in either case. */
        ctx.cycle_deadline = uniform ? 0u : deadline;
        if(signed_compare)func_80005000(&ctx);else func_80004000(&ctx);
        calls++;
        if (ctx.cycles == 6u)
            store_be32(ctx.ram + 0x100u, exit_value);
    }

    result->pc = ctx.pc;
    result->value = ctx.gpr[4];
    result->cr = ctx.cr;
    result->cycles = ctx.cycles;
    result->calls = calls;
    result->skipped=skipped;
    free(ctx.ram);

    const u32 expected_cr=signed_compare && (exit_value&0x80000000u)?0x00900000u:0x00500000u;
    int ok = ctx.pc == SENTINEL_PC && ctx.gpr[4] == exit_value &&
             ctx.cycles == 10u && (ctx.cr & CR2_MASK) == expected_cr &&
             (ctx.cr & ~CR2_MASK) == (0xA50A5A5Au & ~CR2_MASK);
    printf("POLL_LOOP,%s,%s,calls=%d pc=0x%08X r4=%u cr=0x%08X cycles=%llu\n",
           name, ok ? "PASS" : "FAIL", calls, ctx.pc, ctx.gpr[4], ctx.cr,
           (unsigned long long)ctx.cycles);
    return ok ? 0 : 1;
}

int main(void) {
    Result fast,slow,signed_fast,signed_slow,disabled,rounded;
    int fails = 0;
    fails += run_scenario("fast_deadline_path",0,0,1,6,1,&fast);
    fails += run_scenario("uniform_fallback_path",1,0,1,6,1,&slow);
    fails += run_scenario("signed_negative_exit",0,1,1,6,0xffffffffu,&signed_fast);
    fails += run_scenario("signed_reference",1,1,1,6,0xffffffffu,&signed_slow);
    fails += run_scenario("interrupts_disabled",0,1,0,6,1,&disabled);
    fails += run_scenario("whole_iteration_rounding",0,1,1,5,1,&rounded);
#if defined(DOLRECOMP_IDLE_SKIP) && DOLRECOMP_IDLE_SKIP
    if(fast.skipped!=3 || signed_fast.skipped!=3 || rounded.skipped!=3 ||
       slow.skipped || signed_slow.skipped || disabled.skipped) {
        puts("POLL_LOOP,idle_admission,FAIL");++fails;
    }
#endif

    if (fast.pc != slow.pc || fast.value != slow.value ||
        fast.cr != slow.cr || fast.cycles != slow.cycles) {
        printf("POLL_LOOP,state_parity,FAIL\n");
        fails++;
    } else {
        printf("POLL_LOOP,state_parity,PASS\n");
    }
    if (fast.calls != 2 || slow.calls != 3) {
        printf("POLL_LOOP,path_coverage,FAIL,fast_calls=%d slow_calls=%d\n",
               fast.calls, slow.calls);
        fails++;
    } else {
        printf("POLL_LOOP,path_coverage,PASS,fast_calls=2 slow_calls=3\n");
    }
    printf("POLL_LOOP,total,%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

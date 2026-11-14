/* SPDX-License-Identifier: MIT */
#include "cpu/cpu.h"
#include "cpu/function_unit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FUNCTION_UNIT_REAL_ADAPTER
bool fixture_function_adapter(CPUState*,u32);
void fixture_function_adapter_begin(CPUState*,u32);
void fixture_function_adapter_event(CPUState*);
void fixture_function_adapter_end(void);
extern unsigned fixture_function_adapter_calls;
#endif

#define CHECK(x) do {if(!(x)){fprintf(stderr,"function-unit %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
#define DECLARE(pc) void func_##pc##_reference(CPUState*); \
    bool func_##pc##_unit_candidate(CPUState*,const DrFunctionUnitOwner*); \
    extern const DrFunctionUnitInfo func_##pc##_unit_candidate_info
DECLARE(80006000);DECLARE(80006200);DECLARE(80006FF8);DECLARE(80007FF8);
DECLARE(80008400);DECLARE(80008500);DECLARE(80008600);
void func_80007000_reference(CPUState*);void func_80008000_reference(CPUState*);
#define STEP(pc) void func_##pc##_step(CPUState*)
STEP(80006000);STEP(80006004);STEP(80006008);STEP(8000600C);
STEP(80006010);STEP(80006014);STEP(80006018);STEP(8000601C);
STEP(80006020);STEP(80006024);STEP(80006028);STEP(8000602C);STEP(80006030);
STEP(80006200);STEP(80006204);STEP(80006208);STEP(8000620C);STEP(80006210);STEP(80006214);
STEP(80006FF8);STEP(80006FFC);STEP(80007000);STEP(80007004);
STEP(80007008);STEP(8000700C);STEP(80007010);
STEP(80007FF8);STEP(80007FFC);STEP(80008000);STEP(80008004);STEP(80008008);STEP(8000800C);
STEP(80008400);STEP(80008404);STEP(80008408);STEP(8000840C);STEP(80008410);STEP(80008414);
STEP(80008500);STEP(80008504);STEP(80008508);STEP(8000850C);STEP(80008510);STEP(80008514);
STEP(80008600);STEP(80008604);STEP(80008608);STEP(8000860C);STEP(80008610);STEP(80008614);STEP(80008618);
#undef STEP

typedef struct {u32 pc,address,gpr3,gpr4;u64 cycles,value;u8 width,write;} Event;
typedef struct {Event events[64];unsigned count,mode;bool invalid;} Bus;
static Bus* active;
static u8* alternate_ram;
static u8* oracle_ram;
static u8* alternate_mem2;
static u8* oracle_mem2;
static void event(CPUState* cpu,u32 address,u64 value,u8 width,bool write) {
    if(active->count>=64)abort();
    active->events[active->count++]=(Event){cpu->pc,address,cpu->gpr[3],cpu->gpr[4],cpu->cycles,value,width,write};
    if(active->mode&1u) {
        cpu->gpr[3]+=10;cpu->gpr[4]=0x12345678u;cpu->gpr[6]+=3;
    }
    if(active->mode&2u) {
        active->invalid=true;
#if FUNCTION_UNIT_REAL_ADAPTER
        fixture_function_adapter_event(cpu);
#endif
    }
    if(active->mode&4u)cpu->msr=0;
    if(active->mode&8u)cpu->cycle_deadline=cpu->cycles;
    if(active->mode&16u) {
        cpu->ram=alternate_ram;cpu->gpr[5]=GC_RAM_BASE+0x1000u;
    }
    if(active->mode&256u) {
        cpu->mem2=alternate_mem2;cpu->mem2_size=16;cpu->gpr[5]=WII_MEM2_BASE;
    }
}
static u64 read_bus(CPUState* cpu,u32 address,u8 width) {
    event(cpu,address,0x10203040u,width,false);return 0x10203040u;
}
static void write_bus(CPUState* cpu,u32 address,u64 value,u8 width) {
    event(cpu,address,value,width,true);
}
static bool events_equal(const Bus* a,const Bus* b) {
    if(a->count!=b->count)return false;
    for(unsigned i=0;i<a->count;++i) {
        const Event* x=a->events+i;const Event* y=b->events+i;
        if(x->pc!=y->pc || x->address!=y->address || x->gpr3!=y->gpr3 ||
           x->gpr4!=y->gpr4 || x->cycles!=y->cycles || x->value!=y->value ||
           x->width!=y->width || x->write!=y->write)return false;
    }
    return true;
}
static bool inside(u32 pc,u32 base,unsigned count) {return pc>=base && pc<base+count*4u;}
static void reference(CPUState* cpu,u32 base) {
    if(base==0x80006000u)func_80006000_reference(cpu);
    else if(base==0x80006200u)func_80006200_reference(cpu);
    else if(base==0x80008400u)func_80008400_reference(cpu);
    else if(base==0x80008500u)func_80008500_reference(cpu);
    else if(base==0x80008600u)func_80008600_reference(cpu);
    else if(base==0x80006ff8u) {
        if(cpu->pc<0x80007000u)func_80006FF8_reference(cpu);else func_80007000_reference(cpu);
    } else if(cpu->pc<0x80008000u)func_80007FF8_reference(cpu);
    else func_80008000_reference(cpu);
}
static void one(CPUState* cpu) {
#define CASE(pc) case 0x##pc##u:func_##pc##_step(cpu);break
    switch(cpu->pc) {
    CASE(80006000);CASE(80006004);CASE(80006008);CASE(8000600C);
    CASE(80006010);CASE(80006014);CASE(80006018);CASE(8000601C);
    CASE(80006020);CASE(80006024);CASE(80006028);CASE(8000602C);CASE(80006030);
    CASE(80006200);CASE(80006204);CASE(80006208);CASE(8000620C);CASE(80006210);CASE(80006214);
    CASE(80006FF8);CASE(80006FFC);CASE(80007000);CASE(80007004);
    CASE(80007008);CASE(8000700C);CASE(80007010);
    CASE(80007FF8);CASE(80007FFC);CASE(80008000);CASE(80008004);CASE(80008008);CASE(8000800C);
    CASE(80008400);CASE(80008404);CASE(80008408);CASE(8000840C);CASE(80008410);CASE(80008414);
    CASE(80008500);CASE(80008504);CASE(80008508);CASE(8000850C);CASE(80008510);CASE(80008514);
    CASE(80008600);CASE(80008604);CASE(80008608);CASE(8000860C);CASE(80008610);CASE(80008614);CASE(80008618);
    default:abort();
    }
#undef CASE
}
typedef struct {CPUState oracle;Bus bus;u32 base,page,declined_pc;u64 declined_cycles;
    u8 *resume_ram,*original_ram,*original_mem2,*admitted_ram,*admitted_mem2;
    u32 admitted_ram_size,admitted_mem2_size;
    unsigned count,guards,scheduling,continuations;bool failed,deny,resume;} Owner;
static unsigned total_guards,total_roots,total_continuations,total_resumed;
static unsigned eligible_memory,unpublished_stores,unpublished_mem2_stores;
static void opaque(CPUState* cpu) {cpu->gpr[6]+=7;cpu->gpr[11]=0xabcdef01u;cpu->pc=cpu->lr;}
static bool guard(const CPUState* cpu,u32 pc,u64 cycles,bool scheduling,void* user) {
    Owner* owner=user;Bus* saved=active;active=&owner->bus;
    /* One-instruction native bodies provide an independent canonical-state
     * oracle at every guard, including the GPRs dead to this function. */
    unsigned turns=0;
    while((owner->oracle.pc!=pc || owner->oracle.cycles!=cycles) && turns++<128) {
        if(owner->oracle.pc==0x8000701cu)opaque(&owner->oracle);
        else if(inside(owner->oracle.pc,owner->base,owner->count))one(&owner->oracle);
        else break;
    }
    active=saved;
    /* The oracle owns separate RAM. Normalize only that known role when
     * comparing CPU pointers; an actual callback RAM mutation must still
     * match the oracle's independently observed mutation. */
    CPUState expected=owner->oracle;
    if(expected.ram==oracle_ram)expected.ram=owner->original_ram;
    if(expected.mem2==oracle_mem2)expected.mem2=owner->original_mem2;
    if(cpu->pc!=pc || cpu->cycles!=cycles || memcmp(cpu,&expected,sizeof *cpu))owner->failed=true;
    ++owner->guards;if(scheduling)++owner->scheduling;
    if(pc==0x80006020u)++owner->continuations;
    const bool allowed=!owner->deny && !active->invalid && (!scheduling || cycles<cpu->cycle_deadline);
    if(scheduling) {
        owner->resume=false;
        if(allowed) {
            owner->page=pc>>12;owner->admitted_ram=cpu->ram;owner->admitted_mem2=cpu->mem2;
            owner->admitted_ram_size=cpu->ram_size;owner->admitted_mem2_size=cpu->mem2_size;
        }
    } else if(!owner->failed && (pc>>12)==owner->page) {
        owner->resume=true;owner->declined_pc=pc;owner->declined_cycles=cycles;owner->resume_ram=cpu->ram;
    }
    return allowed;
}
static bool overlaps(uintptr_t a,size_t n,uintptr_t b,size_t m) {
    return a>=b?a-b<m:b-a<n;
}
/* Test host's eager, observer-free bank contract. The real composed adapter
 * uses the runtime's independent ownership/guard/backing implementation. */
static bool memory(const CPUState* cpu,u32 ea,u32 width,bool write,void* user) {
    Owner* owner=user;u32 offset=0;u8* host=NULL;
    if(!width || ea>UINT32_MAX-(width-1u) || cpu->ram!=owner->admitted_ram ||
       cpu->mem2!=owner->admitted_mem2 || cpu->ram_size!=owner->admitted_ram_size ||
       cpu->mem2_size!=owner->admitted_mem2_size)return false;
    if(cpu->ram && cpu->ram_size>=width) {
        if(ea>=GC_RAM_BASE && ea-GC_RAM_BASE<=GC_MAIN_RAM_SIZE-width)offset=ea-GC_RAM_BASE,host=cpu->ram;
        else if(ea>=GC_RAM_UNCACHED && ea-GC_RAM_UNCACHED<=GC_MAIN_RAM_SIZE-width)offset=ea-GC_RAM_UNCACHED,host=cpu->ram;
        else if(ea<=GC_MAIN_RAM_SIZE-width)offset=ea,host=cpu->ram;
        if(host && offset>cpu->ram_size-width)host=NULL;
        if(host && write && offset/4096u>=((owner->base&0x1fffffffu)/4096u) &&
           offset/4096u<=((owner->base+owner->count*4u-1u)&0x1fffffffu)/4096u)return false;
    }
    if(!host && cpu->mem2 && cpu->mem2_size>=width) {
        const u32 last=cpu->mem2_size-width;
        if(ea>=0x10000000u && ea-0x10000000u<=last)offset=ea-0x10000000u,host=cpu->mem2;
        else if(cpu->mem2_size<=UINT32_MAX-WII_MEM2_BASE && ea>=WII_MEM2_BASE && ea-WII_MEM2_BASE<=last)offset=ea-WII_MEM2_BASE,host=cpu->mem2;
        else if(cpu->mem2_size<=UINT32_MAX-WII_MEM2_UNCACHED && ea>=WII_MEM2_UNCACHED && ea-WII_MEM2_UNCACHED<=last)offset=ea-WII_MEM2_UNCACHED,host=cpu->mem2;
        if(host && cpu->ram && overlaps((uintptr_t)cpu->ram,cpu->ram_size,(uintptr_t)cpu->mem2,cpu->mem2_size))return false;
    }
    if(!host || overlaps((uintptr_t)(host+offset),width,(uintptr_t)cpu,sizeof *cpu))return false;
    ++eligible_memory;
    /* The load after the known scratch store sees its new local value while
     * CPUState still holds the value from the previous page checkpoint. */
    if(owner->base==0x80006ff8u && !write && ea==GC_RAM_BASE+0x1004u &&
       read_be32(cpu->ram+0x1000u)==5u && cpu->gpr[7]!=5u)++unpublished_stores;
    if(owner->base==0x80006ff8u && !write && host==cpu->mem2 && offset==4u &&
       read_be32(cpu->mem2)==5u && cpu->gpr[7]!=5u)++unpublished_mem2_stores;
    return true;
}
static bool candidate(CPUState* cpu,u32 base,const DrFunctionUnitOwner* owner) {
#if FUNCTION_UNIT_REAL_ADAPTER
    if(base==0x80006ff8u || base==0x80007ff8u)return fixture_function_adapter(cpu,base);
#endif
    if(base==0x80006000u)return func_80006000_unit_candidate(cpu,owner);
    if(base==0x80006200u)return func_80006200_unit_candidate(cpu,owner);
    if(base==0x80007ff8u)return func_80007FF8_unit_candidate(cpu,owner);
    if(base==0x80008400u)return func_80008400_unit_candidate(cpu,owner);
    if(base==0x80008500u)return func_80008500_unit_candidate(cpu,owner);
    if(base==0x80008600u)return func_80008600_unit_candidate(cpu,owner);
    return func_80006FF8_unit_candidate(cpu,owner);
}
static int run_case(CPUState* initial,u32 base,unsigned count,unsigned entry,u64 budget,unsigned mode,bool fp) {
    CPUState ref=*initial,fast=*initial;Bus a={0},b={0};a.mode=b.mode=mode;
    ref.pc=base+entry*4u;ref.lr=0x81234564u;ref.cycles=100;ref.cycle_deadline=100+budget;
    ref.msr=fp?0x2000u:0;ref.exception=0;ref.gpr[3]=0;
    if(!(mode&128u))ref.gpr[5]=0xcc006000u;
    ref.gpr[6]=2;ref.gpr[8]=ref.lr;ref.fpr[1]=1.25;ref.fpr[2]=2.5;
    ref.external_read=read_bus;ref.external_write=write_bus;fast=ref;
    const u8 original_data[16]={0,0,0,0,0x11,0x22,0x33,0x44};u8 expected_data[16],before_data[16];
    u8 before_mem2[16],expected_mem2[16];
    const u32 watched=(mode&64u)?0x7008u:0x1000u;
    if((mode&32u) && !(mode&128u))ref.gpr[5]=fast.gpr[5]=GC_RAM_BASE+0x1000u;
    if(mode&64u) {
        const u8 future_code[]={0x81,0x25,0x00,0x04,0x7d,0x49,0x1a,0x14};
        ref.gpr[5]=fast.gpr[5]=GC_RAM_BASE+0x7008u;
        memcpy(initial->ram+watched,future_code,sizeof future_code);
    }
    if(mode&48u)memcpy(initial->ram+0x1000u,original_data,sizeof original_data);
#if FUNCTION_UNIT_REAL_ADAPTER
    fixture_function_adapter_begin(&fast,base);
#endif
    if(mode&96u)memcpy(before_data,initial->ram+watched,sizeof before_data);
    Owner state={0};state.oracle=ref;state.original_ram=ref.ram;state.oracle.ram=oracle_ram;
    state.original_mem2=ref.mem2;
    if(ref.mem2) {
        memcpy(before_mem2,ref.mem2,sizeof before_mem2);memcpy(oracle_mem2,before_mem2,sizeof before_mem2);
        state.oracle.mem2=oracle_mem2;
    }
    state.bus.mode=mode;state.base=base;state.count=count;
    if(mode&96u)memcpy(oracle_ram+watched,before_data,sizeof before_data);
    const DrFunctionUnitOwner owner={guard,&state,(mode&512u)?0:memory};
    for(unsigned turn=0;turn<128;++turn) {
        if(!inside(ref.pc,base,count) && ref.pc!=0x8000701cu)break;
        if(ref.cycles>=ref.cycle_deadline || ref.exception)break;
        active=&a;if(ref.pc==0x8000701cu)opaque(&ref);else reference(&ref,base);
    }
    if(mode&96u) {
        memcpy(expected_data,initial->ram+watched,sizeof expected_data);
        memcpy(initial->ram+watched,before_data,sizeof before_data);
    }
    if(initial->mem2) {
        memcpy(expected_mem2,initial->mem2,sizeof expected_mem2);
        memcpy(initial->mem2,before_mem2,sizeof before_mem2);
    }
    unsigned handled=0,fallback=0;
    for(unsigned turn=0;turn<128;++turn) {
        if(!inside(fast.pc,base,count) && fast.pc!=0x8000701cu)break;
        if(fast.cycles>=fast.cycle_deadline || fast.exception)break;
        active=&b;
        if(fast.pc==0x8000701cu)opaque(&fast);
        else {
            const u32 admitted=fast.pc;
            if(candidate(&fast,base,&owner)) {
                ++handled;if(admitted==base)++total_roots;else ++total_continuations;
                /* The real adapter finishes the current original native page
                 * after an identity decline. It must not expose this new
                 * mid-block safety boundary to scheduling or device ticks. */
                if(state.resume && fast.pc==state.declined_pc && fast.cycles==state.declined_cycles &&
                   fast.ram==state.resume_ram && !fast.exception) {
                    state.resume=false;++total_resumed;reference(&fast,base);
                }
            } else {++fallback;reference(&fast,base);}
        }
    }
    if(memcmp(&ref,&fast,sizeof ref) || !events_equal(&a,&b) || state.failed ||
       ((mode&96u) && memcmp(expected_data,initial->ram+watched,sizeof expected_data)) ||
       (initial->mem2 && memcmp(expected_mem2,initial->mem2,sizeof expected_mem2))) {
        fprintf(stderr,"base=%08x entry=%u budget=%llu mode=%u fp=%u pc=%08x/%08x cycles=%llu/%llu guards=%u badguard=%u handled=%u fallback=%u\n",base,entry,(unsigned long long)budget,mode,fp,ref.pc,fast.pc,(unsigned long long)ref.cycles,(unsigned long long)fast.cycles,state.guards,state.failed,handled,fallback);return 0;
    }
    total_guards+=state.guards;return 1;
}
int main(void) {
    CPUState cpu;memset(&cpu,0,sizeof cpu);if(!cpu_init(&cpu))return 1;
    alternate_ram=calloc(1,GC_MAIN_RAM_SIZE);oracle_ram=calloc(1,GC_MAIN_RAM_SIZE);
    if(!alternate_ram || !oracle_ram){free(alternate_ram);free(oracle_ram);cpu_free(&cpu);return 1;}
    u8* mem2=calloc(1,16);alternate_mem2=calloc(1,16);oracle_mem2=calloc(1,16);
    if(!mem2 || !alternate_mem2 || !oracle_mem2)return 1;
    write_be32(mem2,1);write_be32(alternate_mem2,2);
    const u8 alternate_data[]={0x55,0x66,0x77,0x88};
    memcpy(alternate_ram+0x1004u,alternate_data,sizeof alternate_data);
    const u32 bases[]={0x80006000u,0x80006200u,0x80006ff8u};
    const unsigned counts[]={13,6,7};unsigned cases=0;
    for(unsigned f=0;f<3;++f)for(unsigned entry=0;entry<counts[f];++entry)
    for(u64 budget=0;budget<40;++budget)for(unsigned mode=0;mode<16;++mode)
    for(unsigned fp=0;fp<2;++fp) {
        if(!run_case(&cpu,bases[f],counts[f],entry,budget,mode,fp)){cpu_free(&cpu);return 1;}++cases;
    }
    /* Same emitted two-page fixture and runner, with the callback changing
     * RAM before the next load. The original page must retain its cached RAM;
     * expired deadlines around that store must not admit a new turn. */
    const unsigned focused_modes[]={16,18,24,26,32,64};
    for(unsigned m=0;m<sizeof focused_modes/sizeof *focused_modes;++m)
    for(u64 budget=1;budget<=9;++budget) {
        if(!run_case(&cpu,0x80006ff8u,7,0,budget,focused_modes[m],true)) {
            free(alternate_ram);free(oracle_ram);cpu_free(&cpu);return 1;
        }
        ++cases;
    }
    /* All three live aliases, absent/short banks, a width crossing the bank
     * end, devices and wrapping addresses. The callback may mutate binding,
     * registers, deadline, or code authorization before the next loop read. */
    const u32 addresses[]={0x10000000u,WII_MEM2_BASE,WII_MEM2_UNCACHED,
        WII_MEM2_BASE,WII_MEM2_BASE,WII_MEM2_BASE+14u,0xcc006000u,0xfffffffeu,
        WII_MEM2_BASE,WII_MEM2_UNCACHED};
    for(unsigned bank=0;bank<sizeof addresses/sizeof *addresses;++bank) {
        cpu.mem2=bank==3?NULL:mem2;cpu.mem2_size=bank==4?2:16;cpu.gpr[5]=addresses[bank];
        /* Only the advertised extent is large. Both wrapping aliases must
         * reach the observer, so these cases never access beyond 16 bytes. */
        if(bank==8)cpu.mem2_size=0x70000000u;
        if(bank==9)cpu.mem2_size=0x30000000u;
        for(unsigned mode=0;mode<16;++mode)for(u64 budget=0;budget<40;++budget) {
            if(!run_case(&cpu,0x80007ff8u,6,0,budget,128u|mode,true))return 1;
            ++cases;
        }
        for(u64 budget=1;budget<40;++budget) {
            if(!run_case(&cpu,0x80007ff8u,6,0,budget,128u|256u,true))return 1;
            ++cases;
        }
    }
    cpu.mem2=NULL;cpu.mem2_size=0;
    /* Missing optional eligibility always delegates before the access,
     * including dirty-local and mutating/expired-deadline callback paths. */
    const u32 no_memory_bases[]={0x80006000u,0x80006ff8u,0x80008400u,0x80008500u,0x80008600u};
    const unsigned no_memory_counts[]={13,7,6,6,7};
    const unsigned no_memory_modes[]={512u,513u,520u,528u};
    for(unsigned f=0;f<5;++f)for(unsigned m=0;m<4;++m)
    for(u64 budget=0;budget<40;++budget)for(unsigned fp=0;fp<2;++fp) {
        if(!run_case(&cpu,no_memory_bases[f],no_memory_counts[f],0,budget,no_memory_modes[m],fp))return 1;++cases;
    }
    /* FP unavailable after dirty local changes, plus D/X-form update and
     * source/base aliasing, through both delegated and ordinary accesses. */
    const u32 more[]={0x80008400u,0x80008500u,0x80008600u};
    const u32 aliases[]={0x1000u,GC_RAM_BASE+0x1000u,GC_RAM_UNCACHED+0x1000u};
    for(unsigned f=0;f<3;++f)for(u64 budget=0;budget<40;++budget)for(unsigned fp=0;fp<2;++fp) {
        for(unsigned mode=0;mode<16;++mode) {
            if(!run_case(&cpu,more[f],f==2?7:6,0,budget,mode,fp))return 1;++cases;
        }
        for(unsigned alias=0;alias<3;++alias) {
            cpu.gpr[5]=aliases[alias];
            if(!run_case(&cpu,more[f],f==2?7:6,0,budget,128u|32u,fp))return 1;++cases;
        }
    }
    for(unsigned alias=0;alias<3;++alias)for(u64 budget=0;budget<40;++budget) {
        cpu.mem2=mem2;cpu.mem2_size=16;cpu.gpr[5]=addresses[alias];
        if(!run_case(&cpu,0x80006ff8u,7,0,budget,128u,true))return 1;++cases;
    }
    cpu.mem2=NULL;cpu.mem2_size=0;
    /* Rejected admission must leave every architectural byte unchanged. */
    cpu.pc=0x80006000u;cpu.cycles=100;cpu.cycle_deadline=10000;
    Owner state={0};state.oracle=cpu;state.original_ram=cpu.ram;state.oracle.ram=oracle_ram;
    state.deny=true;state.base=cpu.pc;state.count=13;
    DrFunctionUnitOwner owner={guard,&state};Bus bus={0};active=&bus;CPUState before=cpu;
    CHECK(!candidate(&cpu,0x80006000u,&owner));CHECK(!memcmp(&cpu,&before,sizeof cpu));
    const DrFunctionUnitInfo* info=&func_80006000_unit_candidate_info;
    CHECK(info->entry_count==2 && info->entries[0]==0x80006000u && info->entries[1]==0x80006020u);
    CHECK(info->block_count>info->entry_count);
#if DOLRECOMP_FUNCTION_UNIT && !DOLRECOMP_BLOCK_ENTRY_COMPAT
    CHECK(total_guards && total_roots && total_continuations && total_resumed);
#if !FUNCTION_UNIT_REAL_ADAPTER
    CHECK(eligible_memory && unpublished_stores && unpublished_mem2_stores);
#endif
#endif
#if FUNCTION_UNIT_REAL_ADAPTER
    CHECK(fixture_function_adapter_calls);
    fixture_function_adapter_end();
#endif
    free(mem2);free(alternate_mem2);free(oracle_mem2);free(alternate_ram);free(oracle_ram);cpu_free(&cpu);printf("function-unit %u exact cases PASS (enabled=%u guards=%u roots=%u continuations=%u original-page-resumes=%u eligible-memory=%u unpublished-stores=%u unpublished-mem2-stores=%u)\n",cases,DOLRECOMP_FUNCTION_UNIT,total_guards,total_roots,total_continuations,total_resumed,eligible_memory,unpublished_stores,unpublished_mem2_stores);return 0;
}

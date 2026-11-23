/* SPDX-License-Identifier: MIT */
#include "backend/native_function_fp.h"
#include <stdio.h>

void dr_native_80003100(DrNativeCpu*, u8*);
void dr_native_80003200(DrNativeCpu*, u8*);
void dr_native_80003300(DrNativeCpu*, u8*);
void dr_native_80003400(DrNativeCpu*, u8*);
void dr_native_80003500(DrNativeCpu*, u8*);
void dr_native_80003600(DrNativeCpu*, u8*);
void dr_native_80003700(DrNativeCpu*, u8*);
void dr_native_80003900(DrNativeCpu*, u8*);
void dr_native_80003B00(DrNativeCpu*, u8*);
void dr_native_80003E00(DrNativeCpu*, u8*);
void dr_native_C1003000(DrNativeCpu*, u8*);
void dr_native_80004100(DrNativeCpu*, u8*);

static void service(DrNativeCpu* cpu, u32 pc) {
    unsigned* calls = cpu->host;
    if (pc != 0x80003100 && pc != 0x80003108 && pc != 0x80003200)
        abort();
    ++*calls;
    cpu->deadline += 7;
}

static void observe_entry(DrNativeCpu* cpu, u32 pc) {
    if (pc != 0x80003500u)
        abort();
    cpu->gpr[7]++;
}

static u32 return_interrupt(DrNativeCpu* cpu, u32 pc, DrNativeSystemOp op,
                            u32 selector, u32 value) {
    if (pc != 0x80003700u || op != DR_NATIVE_RFI || selector || value)
        abort();
    return cpu->gpr[6];
}

static u32 observed_memory_read(DrNativeCpu* cpu, u32 address, unsigned bytes) {
    if (address != 0x80000fffu || bytes != 2u || cpu->work != 17u)
        abort();
    cpu->deadline -= 3;
    return 0xcafeu;
}

static bool native_mem1_access(void) {
    enum { PAGE_RANGE=1, SHARED_RANGE=2, UNKNOWN_OWNER=4 };
    u8 memory[8192] = {0};
    u8 pages[2] = {0, PAGE_RANGE};
    const u64 ranges[2] = {(UINT64_C(0x20)<<32)|0x10u, (UINT64_C(0x1020)<<32)|0x1000u};
    u64 shared = (UINT64_C(0x90)<<32)|0x60u;
    DrNativeCpu cpu = {.mem1 = memory, .mem1_size = sizeof(memory),
                      .memory_pages = pages, .direct_stores = true,
                      .mem1_page_ranges = ranges, .mem1_page_range_flag = PAGE_RANGE,
                      .mem1_shared_range = &shared, .mem1_shared_range_flag = SHARED_RANGE,
                      .read = observed_memory_read};
    cpu.deadline = 27;
    dr_native_memory_write(&cpu, NULL, 0xc0000004u, 4u, 0x12345678u, true, 17u);
    if (dr_native_memory_read(&cpu, NULL, 0x80000004u, 4u, true, 17u) != 0x12345678u ||
        cpu.deadline != 27 || cpu.work != 0)
        return false;
    dr_native_memory_write(&cpu, NULL, 0x80000027u, 1u, 0x12f4u, true, 17u);
    dr_native_memory_write(&cpu, NULL, 0xc0000029u, 2u, 0x12a9e7u, true, 17u);
    dr_native_memory_write(&cpu, NULL, 0x8000002bu, 4u, 0xc3715d2fu, true, 17u);
    static const u8 expected[] = {0xf4, 0, 0xa9, 0xe7, 0xc3, 0x71, 0x5d, 0x2f};
    if (memcmp(memory + 0x27u, expected, sizeof(expected)) ||
        dr_native_memory_read(&cpu, NULL, 0xc0000027u, 1u, true, 17u) != 0xf4u ||
        dr_native_memory_read(&cpu, NULL, 0x80000029u, 2u, true, 17u) != 0xa9e7u ||
        dr_native_memory_read(&cpu, NULL, 0xc000002bu, 4u, true, 17u) != 0xc3715d2fu ||
        cpu.deadline != 27 || cpu.work != 0)
        return false;
    pages[0] = PAGE_RANGE|SHARED_RANGE;
    if (dr_native_mem1(&cpu, 0x80000010u, 1u) != NULL ||
        dr_native_mem1(&cpu, 0x80000060u, 4u) != NULL ||
        dr_native_mem1(&cpu, 0x80000020u, 4u) != memory + 0x20u ||
        dr_native_mem1(&cpu, 0x80001020u, 4u) != memory + 0x1020u)
        return false;
    dr_native_memory_write(&cpu, NULL, 0xc0001021u, 4u, 0xabcdef01u, true, 19u);
    if (dr_native_memory_read(&cpu, NULL, 0x80001021u, 4u, true, 19u) != 0xabcdef01u ||
        cpu.deadline != 27 || cpu.work != 0)
        return false;
    shared = 0;
    if (dr_native_mem1(&cpu, 0xc0000060u, 4u) != memory + 0x60u)
        return false;
    pages[0] = UNKNOWN_OWNER;
    if (dr_native_mem1(&cpu, 0x80000060u, 4u) != NULL)
        return false;
    pages[0] = 0;
    return dr_native_memory_read(&cpu, NULL, 0x80000fffu, 2u, true, 17u) == 0xcafeu && cpu.deadline == 24;
}

static u32 observed_double_read(DrNativeCpu* cpu,u32 ea,unsigned bytes) {
    if (ea!=0x80001000u || bytes!=4u || cpu->work!=17u)
        abort();
    cpu->deadline-=3;
    return 1u;
}

static void observed_double_write(DrNativeCpu* cpu,u32 ea,unsigned bytes,u32 value) {
    if (ea!=0x80001000u || bytes!=4u || value!=2u || cpu->work!=17u)
        abort();
    if (dr_native_read_be(cpu->mem1+0xffcu,4)!=0x40140000u)
        abort();
    cpu->deadline-=2;
}

static bool native_double_access(void) {
    u8 memory[8192]={0};
    u8 pages[2]={0};
    const u64 ranges[2]={0,(UINT64_C(0x1004)<<32)|0x1000u};
    DrNativeCpu cpu={.mem1=memory,.mem1_size=sizeof(memory),.memory_pages=pages,
        .mem1_page_ranges=ranges,.mem1_page_range_flag=1,.direct_stores=true,
        .read=observed_double_read,.write=observed_double_write};
    cpu.deadline=27;
    dr_native_store_double(&cpu,NULL,0x80000ffcu,5.0,true,17);
    if (dr_native_load_double(&cpu,NULL,0x80000ffcu,true,17)!=5.0 || cpu.deadline!=27)
        return false;
    pages[1]=1;
    if (dr_native_float64_bits(dr_native_load_double(&cpu,NULL,0x80000ffcu,true,17))
            !=UINT64_C(0x4014000000000001) || cpu.deadline!=24)
        return false;
    dr_native_write_be(memory+0xffcu,4,0);
    dr_native_store_double(&cpu,NULL,0x80000ffcu,dr_native_float64(UINT64_C(0x4014000000000002)),
                           true,17);
    return cpu.deadline==22 && dr_native_read_be(memory+0x1000u,4)==0;
}

typedef struct {
    unsigned reads, services;
    u64 first_work;
} DeadlineObservation;

static void service_shortened_deadline(DrNativeCpu* cpu,u32 pc) {
    DeadlineObservation* state=cpu->host;
    if(pc!=0x80003b00u || state->reads!=1u || state->services ||
       cpu->work!=state->first_work || cpu->deadline!=cpu->work)
        abort();
    ++state->services;
    cpu->deadline=cpu->work+64u;
}

static u32 shorten_deadline_on_read(DrNativeCpu* cpu,u32 address,unsigned bytes) {
    DeadlineObservation* state=cpu->host;
    if(address!=0xcc008000u || bytes!=4u || state->reads!=state->services)
        abort();
    if(!state->reads) {
        state->first_work=cpu->work;
        cpu->deadline=cpu->work;
    } else if(cpu->work<=state->first_work) {
        abort();
    }
    return ++state->reads;
}

static bool native_observed_deadline(void) {
    DeadlineObservation state={0};
    DrNativeCpu cpu={.deadline=64u,.service=service_shortened_deadline,
        .read=shorten_deadline_on_read,.host=&state};
    cpu.gpr[3]=0xcc008000u;
    cpu.gpr[4]=2u;
    dr_native_80003B00(&cpu,NULL);
    return state.reads==2u && state.services==1u && cpu.gpr[5]==2u;
}

static void observe_register_merge(DrNativeCpu* cpu,u32 pc) {
    const u32* expected=cpu->host;
    if(pc!=0x80004000u || cpu->gpr[4]!=*expected || cpu->gpr[7]!=17u || cpu->gpr[8]!=31u)
        abort();
    cpu->gpr[8]=100u;
}

static bool native_register_merge(void) {
    for(u32 skip=0;skip<2;++skip) {
        u32 expected=5u+(skip ? 0u : 9u);
        DrNativeCpu cpu={.deadline=256,.observe_entry=observe_register_merge,.host=&expected};
        cpu.gpr[3]=skip;
        cpu.gpr[8]=31u;
        dr_native_80003E00(&cpu,NULL);
        if(cpu.gpr[3]!=expected+100u || cpu.gpr[8]!=100u)
            return false;
    }
    return true;
}

typedef struct {
    void* continuation;
    unsigned saves,resumes;
} ResumeProbe;

static void save_probe(DrNativeCpu* cpu,u32 context,u32 pc,void* continuation,const u32* registers) {
    ResumeProbe* probe=cpu->host;
    if(context!=(u32)-5 || pc!=0x80004108u || !continuation || registers[14]!=0x1234u) {
        fprintf(stderr,"bad scheduler save: context=%08x pc=%08x r14=%08x\n",context,pc,registers[14]);
        abort();
    }
    ++probe->saves;
    probe->continuation=continuation;
}

static u32 resume_probe(DrNativeCpu* cpu,u32 pc,DrNativeSystemOp op,u32 selector,u32 value) {
    ResumeProbe* probe=cpu->host;
    if(pc!=0x80003700u || op!=DR_NATIVE_RFI || selector || value || !probe->continuation) {
        fprintf(stderr,"bad scheduler resume: pc=%08x op=%u saved=%d\n",pc,(unsigned)op,probe->continuation!=NULL);
        abort();
    }
    cpu->resume=probe->continuation;
    probe->continuation=NULL;
    return 0;
}

static void resume_landed_probe(DrNativeCpu* cpu) {
    ResumeProbe* probe=cpu->host;
    if(cpu->resume)abort();
    ++probe->resumes;
}

static bool native_scheduler_resume(void) {
    ResumeProbe probe={0};
    DrNativeCpu cpu={.deadline=UINT64_MAX,.thread_save=save_probe,.system=resume_probe,
                     .resume_landed=resume_landed_probe,.host=&probe};
    cpu.gpr[3]=(u32)-5;
    dr_native_80004100(&cpu,NULL);
    return cpu.gpr[3]==0x1234 && cpu.gpr[7]==0 && !cpu.resume &&
        !probe.continuation && probe.saves==1 && probe.resumes==1;
}

int main(void) {
    if(!native_scheduler_resume())return 42;
    if (!native_mem1_access() || !native_double_access() || !native_observed_deadline() || !native_register_merge())
        return 1;

    u8 ram[64] = {0};
    unsigned calls = 0;
    DrNativeCpu cpu = {.service = service, .observe_entry = observe_entry, .host = &calls};
    dr_native_80003100(&cpu, ram);
    if (cpu.gpr[3] != 20 || cpu.gpr[4] != 0 || dr_native_read_be(ram+24, 4) != 20 ||
        calls < 2 || cpu.work < 27) {
        fprintf(stderr, "native function returned incorrect state or missed its safepoint\n");
        return 1;
    }
    cpu.deadline = cpu.work + 100;
    cpu.gpr[3] = 0x80003308;
    dr_native_80003300(&cpu, ram);
    if (cpu.gpr[3] != 11)
        return 1;
    cpu.gpr[3] = 0x80003310;
    dr_native_80003300(&cpu, ram);
    if (cpu.gpr[3] != 43)
        return 1;
    const u32 gqr[8]={0,0x106};
    cpu.gqr=gqr;
    cpu.gpr[3]=0;
    dr_native_write_be(ram+4,4,dr_native_float32_bits(-12.5f));
    dr_native_write_be(ram+8,4,dr_native_float32_bits(100.0f));
    dr_native_80003400(&cpu,ram);
    if(cpu.fpr[2].lane[0]!=-25.0 || cpu.fpr[2].lane[1]!=200.0 || ram[16]!=206 || ram[17]!=127)
        return 1;
    cpu.cr = 0xa6d00000u;
    dr_native_80003500(&cpu, ram);
    if (cpu.cr != 0xa4400000u || cpu.gpr[3] != cpu.cr || cpu.gpr[7] != 1u)
        return 1;
    cpu.gpr[11] = 40;
    dr_native_80003600(&cpu, ram);
    if (cpu.gpr[3] != 0x1234u || dr_native_read_be(ram+36, 4) != 0x1234u)
        return 1;
    static const DrNativeFunction linked[0xc81] = {[0xc80] = dr_native_80003200};
    cpu.linked = linked;
    cpu.system = return_interrupt;
    cpu.gpr[3] = 41;
    cpu.gpr[6] = 0;
    dr_native_80003700(&cpu, ram);
    if (cpu.gpr[3] != 41)
        return 1;
    cpu.gpr[6] = 0x80003200u;
    dr_native_80003700(&cpu, ram);
    if (cpu.gpr[3] != 46)
        return 1;
    cpu.gpr[3]=0;
    dr_native_store_double(&cpu,ram,0,0x1p52+448.0,false,0);
    dr_native_store_double(&cpu,ram,8,0x1p52,false,0);
    dr_native_80003900(&cpu,ram);
    if(cpu.fpr[1].lane[0]!=448.0 || cpu.fpr[1].lane[1]!=448.0)
        return 1;
    dr_native_C1003000(&cpu,ram);
    if(cpu.gpr[3]!=1u || cpu.lr!=0x80003808u)
        return 1;
    return 0;
}

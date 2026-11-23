/* SPDX-License-Identifier: MIT */
#include "backend/native_function_emit.h"

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    FILE* out = fopen(argv[1], "w");
    if (!out)
        return 1;
    const u32 caller = 0x80003100, callee = 0x80003200, switcher = 0x80003300;
    const u32 paired = 0x80003400;
    const u32 condition = 0x80003500;
    const u32 spill = 0x80003600;
    const u32 interrupt_return = 0x80003700;
    const u32 scalar_rounding = 0x80003900;
    const u32 relocated = 0xc1003000;
    const u32 stack_span = 0x80003a00;
    const u32 shortened_deadline=0x80003b00;
    const u32 register_merge = 0x80003e00, register_writer = 0x80003f00, register_observer = 0x80004000;
    const u32 scheduler=0x80004100, resume_dispatch=0x80004140;
    DrNativeSymbol entries[] = {
        {caller, 32, DR_NATIVE_FUNCTION, 0},
        {callee, 8, DR_NATIVE_FUNCTION, 0},
        {switcher, 24, DR_NATIVE_FUNCTION, 0},
        {paired, 20, DR_NATIVE_FUNCTION, 0},
        {condition, 16, DR_NATIVE_FUNCTION, 0},
        {spill, 24, DR_NATIVE_FUNCTION, 0},
        {spill+0x40, 0, DR_NATIVE_SAVE_GPR, 31},
        {spill+0x80, 0, DR_NATIVE_RESTORE_GPR, 31},
        {interrupt_return, 4, DR_NATIVE_FUNCTION, 0},
        {scalar_rounding, 16, DR_NATIVE_FUNCTION, 0},
        {relocated, 12, DR_NATIVE_FUNCTION, 0},
        {stack_span, 40, DR_NATIVE_FUNCTION, 0},
        {register_merge, 32, DR_NATIVE_FUNCTION, 0},
        {register_writer, 8, DR_NATIVE_FUNCTION, 0},
        {register_observer, 4, DR_NATIVE_FUNCTION, 0},
        {shortened_deadline, 20, DR_NATIVE_FUNCTION, 0},
        {scheduler, 32, DR_NATIVE_FUNCTION, 0},
        {resume_dispatch, 12, DR_NATIVE_FUNCTION, 0},
    };
    DrNativeSymbols symbols = {entries, sizeof(entries)/sizeof(*entries)};
    const u32 call_word = 0x48000001u | (callee - (caller+8));
    /* Four native calls from a local loop, followed by a guest-endian store. */
    const u32 words[] = {0x38600000, 0x38800004, call_word, 0x3884ffff,
                         0x2c040000, 0x4082fff4, 0x90600018, 0x4e800020};
    PPCInst caller_code[8];
    for (unsigned i = 0; i < 8; ++i)
        caller_code[i] = ppc_decode(words[i], caller + i*4);
    const PPCInst callee_code[] = {
        ppc_decode(0x38630005, callee), ppc_decode(0x4e800020, callee+4),
    };
    const u32 switch_words[] = {0x7c6903a6, 0x4e800420, 0x3860000b, 0x4e800020, 0x3860002b, 0x4e800020};
    const u32 targets[] = {switcher + 8, switcher + 16};
    PPCInst switch_code[6];
    for (unsigned i = 0; i < 6; ++i)
        switch_code[i] = ppc_decode(switch_words[i], switcher+i*4u);
    const u32 paired_words[] = {0xe0030004,0x10200090,0x1040082a,0xf0431010,0x4e800020};
    PPCInst paired_code[5];
    for (unsigned i=0;i<5;++i)
        paired_code[i]=ppc_decode(paired_words[i],paired+i*4u);
    const PPCInst condition_code[] = {
        ppc_decode(0x4cc63182, condition),      /* crxor 6,6,6 */
        ppc_decode(0x4d040000, condition+4),    /* mcrf cr2,cr1 */
        ppc_decode(0x7c600026, condition+8),    /* mfcr r3 */
        ppc_decode(0x4e800020, condition+12),
    };
    const u32 spill_words[] = {0x3be01234, 0x4800003d, 0x3be00000, 0x48000075,
                               0x7fe3fb78, 0x4e800020};
    PPCInst spill_code[sizeof(spill_words)/sizeof(*spill_words)];
    for (unsigned i = 0; i < sizeof(spill_words)/sizeof(*spill_words); ++i)
        spill_code[i] = ppc_decode(spill_words[i], spill+i*4u);
    const PPCInst return_code = ppc_decode(0x4c000064, interrupt_return);
    const PPCInst scalar_code[]={ppc_decode(0xc8230000,scalar_rounding),
        ppc_decode(0xc8430008,scalar_rounding+4u),ppc_decode(0xec211028,scalar_rounding+8u),
        ppc_decode(0x4e800020,scalar_rounding+12u)};
    const PPCInst stack_code[]={
        ppc_decode(0x90610000,stack_span),     /* stw r3,0(r1) */
        ppc_decode(0x38830001,stack_span+4),   /* addi r4,r3,1 */
        ppc_decode(0x90810004,stack_span+8),   /* stw r4,4(r1) */
        ppc_decode(0x80a10000,stack_span+12),  /* lwz r5,0(r1) */
        ppc_decode(0x7c0004ac,stack_span+16),  /* sync changes ownership */
        ppc_decode(0xd8210000,stack_span+20),  /* stfd f1,0(r1) */
        ppc_decode(0x80810004,stack_span+24),  /* lwz r4,4(r1) */
        ppc_decode(0x80c10000,stack_span+28),  /* lwz r6,0(r1) */
        ppc_decode(0x90c10008,stack_span+32),  /* stw r6,8(r1) */
        ppc_decode(0x4e800020,stack_span+36),
    };
    fputs("#include \"backend/native_function_fp.h\"\n", out);
    const DrNativeEmission plain={0};
    const DrNativeEmission local={.local_targets=targets,.target_count=2};
    const DrNativeEmission watched={.observe_entry=true};
    bool emitted=dr_emit_native_function(out,&symbols,&entries[0],caller_code,&plain) &&
                 dr_emit_native_function(out,&symbols,&entries[1],callee_code,&plain) &&
                 dr_emit_native_function(out,&symbols,&entries[2],switch_code,&local) &&
                 dr_emit_native_function(out,&symbols,&entries[3],paired_code,&plain) &&
                 dr_emit_native_function(out,&symbols,&entries[4],condition_code,&watched) &&
                 dr_emit_native_function(out,&symbols,&entries[5],spill_code,&plain) &&
                 dr_emit_native_function(out,&symbols,&entries[8],&return_code,&plain) &&
                 dr_emit_native_function(out,&symbols,&entries[9],scalar_code,&plain);
    const PPCInst merge_code[]={
        ppc_decode(0x38840005,register_merge),          /* addi r4,r4,5 */
        ppc_decode(0x2c030000,register_merge+4u),       /* cmpwi r3,0 */
        ppc_decode(0x40820008,register_merge+8u),       /* bne skip first call */
        ppc_decode(0x48000001u|(register_writer-(register_merge+12u)),register_merge+12u),
        ppc_decode(0x38e00011,register_merge+16u),      /* li r7,17 */
        ppc_decode(0x48000001u|(register_observer-(register_merge+20u)),register_merge+20u),
        ppc_decode(0x7c644214,register_merge+24u),      /* add r3,r4,r8 */
        ppc_decode(0x4e800020,register_merge+28u),
    };
    const PPCInst writer_code[]={ppc_decode(0x38840009,register_writer),
        ppc_decode(0x4e800020,register_writer+4u)};
    const PPCInst observer_code=ppc_decode(0x4e800020,register_observer);
    const PPCInst observed_loop[] = {
        ppc_decode(0x80a30000,shortened_deadline),       /* lwz r5,0(r3) */
        ppc_decode(0x3884ffff,shortened_deadline+4u),    /* addi r4,r4,-1 */
        ppc_decode(0x2c040000,shortened_deadline+8u),    /* cmpwi r4,0 */
        ppc_decode(0x4082fff4,shortened_deadline+12u),   /* bne back to load */
        ppc_decode(0x4e800020,shortened_deadline+16u),
    };
    const DrNativeEmission checked={.checked_memory=true};
    emitted=emitted && dr_emit_native_function(out,&symbols,&entries[11],stack_code,&checked) &&
        dr_emit_native_function(out,&symbols,&entries[12],merge_code,&plain) &&
        dr_emit_native_function(out,&symbols,&entries[13],writer_code,&plain) &&
        dr_emit_native_function(out,&symbols,&entries[14],&observer_code,&watched) &&
        dr_emit_native_function(out,&symbols,&entries[15],observed_loop,&checked);
    const PPCInst scheduler_code[]={
        ppc_decode(0x39c01234, scheduler),
        ppc_decode(0x48000001u|((callee-(scheduler+4u))&0x03fffffcu),scheduler+4u),
        ppc_decode(0x2c030000, scheduler+8u),
        ppc_decode(0x4082000c, scheduler+12u),
        ppc_decode(0x39c06789, scheduler+16u),
        ppc_decode(0x48000001u|(resume_dispatch-(scheduler+20u)),scheduler+20u),
        ppc_decode(0x7dc37378, scheduler+24u),
        ppc_decode(0x4e800020, scheduler+28u),
    };
    const PPCInst dispatch_code[]={
        ppc_decode(0x48000001u|((interrupt_return-resume_dispatch)&0x03fffffcu),resume_dispatch),
        ppc_decode(0x38e00073, resume_dispatch+4u),
        ppc_decode(0x4e800020, resume_dispatch+8u),
    };
    const DrNativeEmission thread_save={.thread_save_site=scheduler+4u};
    emitted=emitted && dr_emit_native_function(out,&symbols,&entries[16],scheduler_code,&thread_save) &&
        dr_emit_native_function(out,&symbols,&entries[17],dispatch_code,&plain);
    const PPCInst relocated_code[]={ppc_decode(0x38600000,relocated),
        ppc_decode(0x48000001,relocated+4u),ppc_decode(0x4e800020,relocated+8u)};
    const DrNativeRelocation relocations[]={
        {.address=relocated,.operand=0,.kind=DR_NATIVE_ADDR16_LO},
        {.address=relocated+4u,.operand=1,.kind=DR_NATIVE_REL24},
    };
    const DrNativeEmission rel={.relocations=relocations,.relocation_count=2};
    fputs("#undef DR_NATIVE_GUEST_PC\n"
          "#define DR_NATIVE_GUEST_PC(pc) ((pc) - 0xc1003000u + 0x80003800u)\n"
          "static const u32 operands[] = {0x3860fffcu,0x4bfff9fdu};\n"
          "#define DR_NATIVE_OPERAND(i) (operands[i])\n",out);
    emitted=emitted && dr_emit_native_function(out,&symbols,&entries[10],relocated_code,&rel);
    return fclose(out) != 0 || !emitted;
}

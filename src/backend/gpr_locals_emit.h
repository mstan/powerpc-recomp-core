/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Source-local emission policy; no consumer callback interface. */
static bool gpr_locals_preserved(PPCOpcode op) {
    switch(op) {
    /* Branch bodies only update PC/LR/CTR/CR and stay inside this invocation
     * or leave through DR_RET; same-page shadow calls need no publication. */
    case PPC_OP_B: case PPC_OP_BC: case PPC_OP_BCLR: case PPC_OP_BCCTR:
    case PPC_OP_MULLI:
    case PPC_OP_SUBFIC:
    case PPC_OP_ADDI:
    case PPC_OP_ADDIC:
    case PPC_OP_ADDIC_DOT:
    case PPC_OP_ADDIS:
    case PPC_OP_CMPI:
    case PPC_OP_CMPLI:
    case PPC_OP_CMP:
    case PPC_OP_CMPL:
    case PPC_OP_ORI:
    case PPC_OP_ORIS:
    case PPC_OP_XORI:
    case PPC_OP_XORIS:
    case PPC_OP_ANDI:
    case PPC_OP_ANDIS:
    case PPC_OP_ADD:
    case PPC_OP_ADDO:
    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
    case PPC_OP_NEG:
    case PPC_OP_NEGO:
    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
    case PPC_OP_MULHW:
    case PPC_OP_MULHWU:
    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV:
    case PPC_OP_CNTLZW:
    case PPC_OP_EXTSB:
    case PPC_OP_EXTSH:
    case PPC_OP_SLW:
    case PPC_OP_SRW:
    case PPC_OP_SRAW:
    case PPC_OP_SRAWI:
    case PPC_OP_RLWINM:
    case PPC_OP_RLWNM:
    case PPC_OP_RLWIMI:
    case PPC_OP_LWZ:
    case PPC_OP_LWZU:
    case PPC_OP_LWZX:
    case PPC_OP_LWZUX:
    case PPC_OP_LBZ:
    case PPC_OP_LBZU:
    case PPC_OP_LBZX:
    case PPC_OP_LBZUX:
    case PPC_OP_LHZ:
    case PPC_OP_LHZU:
    case PPC_OP_LHZX:
    case PPC_OP_LHZUX:
    case PPC_OP_LHA:
    case PPC_OP_LHAU:
    case PPC_OP_LHAX:
    case PPC_OP_LHAUX:
        return true;
    default:return false;
    }
}

static u32 gpr_locals_written(const PPCInst* inst) {
    switch(inst->op) {
    case PPC_OP_CMPI: case PPC_OP_CMPLI: case PPC_OP_CMP: case PPC_OP_CMPL:
    case PPC_OP_B: case PPC_OP_BC: case PPC_OP_BCLR: case PPC_OP_BCCTR:
        return 0;
    case PPC_OP_ORI: case PPC_OP_ORIS: case PPC_OP_XORI: case PPC_OP_XORIS:
    case PPC_OP_ANDI: case PPC_OP_ANDIS:
    case PPC_OP_AND: case PPC_OP_ANDC: case PPC_OP_OR: case PPC_OP_ORC:
    case PPC_OP_XOR: case PPC_OP_NAND: case PPC_OP_NOR: case PPC_OP_EQV:
    case PPC_OP_CNTLZW: case PPC_OP_EXTSB: case PPC_OP_EXTSH:
    case PPC_OP_SLW: case PPC_OP_SRW: case PPC_OP_SRAW: case PPC_OP_SRAWI:
    case PPC_OP_RLWINM: case PPC_OP_RLWNM: case PPC_OP_RLWIMI:
        return 1u<<inst->rA;
    case PPC_OP_LWZU: case PPC_OP_LWZUX: case PPC_OP_LBZU: case PPC_OP_LBZUX:
    case PPC_OP_LHZU: case PPC_OP_LHZUX: case PPC_OP_LHAU: case PPC_OP_LHAUX:
        return (1u<<inst->rD)|(1u<<inst->rA);
    default:
        /* Called only for the closed ALU/plain-load set above. */
        return 1u<<inst->rD;
    }
}

/* The bank is private to one generated invocation. Opaque instructions run
 * against canonical CPUState; integer ALU, plain MEM1 reads and local control flow keep
 * locals live. Publication is explicit, including every generated exit. */
static void emit_gpr_header(FILE* out) {
    fputs("#define DOLRECOMP_GPR_LOCALS_ABI 1\n#ifndef DOLRECOMP_GPR_LOCALS\n#define DOLRECOMP_GPR_LOCALS 0\n#endif\n",out);
    fputs("#if DOLRECOMP_GPR_LOCALS\nDR_MEM_INLINE void dr_gpr_publish(CPUState* ctx,const u32* r,u32 mask) {\n",out);
    for(unsigned n=0;n<32;++n)fprintf(out,"    if(mask & (1u<<%u)) ctx->gpr[%u]=r[%u];\n",n,n,n);
    fputs("}\nDR_MEM_INLINE void dr_gpr_reload(CPUState* ctx,u32* r) {\n",out);
    for(unsigned n=0;n<32;++n)fprintf(out,"    r[%u]=ctx->gpr[%u];\n",n,n);
    fputs("}\nDR_MEM_INLINE bool dr_gpr_nonalias(CPUState* ctx,u8* ram) {\n"
          "    uintptr_t c=(uintptr_t)ctx,r=(uintptr_t)ram;\n"
          "    return c>=r ? c-r>=GC_MAIN_RAM_SIZE : r-c>=sizeof(*ctx);\n}\n"
          "DR_MEM_INLINE bool dr_gpr_ram_hit(u32 ea,u32 width) {\n"
          "    return (ea>=GC_RAM_BASE && ea-GC_RAM_BASE<=GC_MAIN_RAM_SIZE-width) ||\n"
          "           (ea>=GC_RAM_UNCACHED && ea-GC_RAM_UNCACHED<=GC_MAIN_RAM_SIZE-width) ||\n"
          "           ea<=GC_MAIN_RAM_SIZE-width;\n}\n"
          "#define DR_GPR (ctx->gpr)\n"
          "#define DR_GPR_DIRTY(mask) do { if(dr_gpr_live) dr_gpr_dirty |= (mask); } while(0)\n"
          "#define DR_GPR_FLUSH() do { if(dr_gpr_live) { dr_gpr_publish(ctx,dr_gpr,dr_gpr_dirty); dr_gpr_live=false; } } while(0)\n"
          "#define DR_GPR_RELOAD_IF() do { if(dr_gpr_allowed && !dr_gpr_live) { dr_gpr_reload(ctx,dr_gpr); dr_gpr_dirty=0; dr_gpr_live=true; } } while(0)\n",out);
    for(unsigned width=8;width<=32;width*=2) {
        fprintf(out,"DR_MEM_INLINE u%u dr_gpr_read%u(CPUState* ctx,u8* ram,u32 ea,u32 cia,u64 cycles,u32* r,u32* dirty,bool* live,bool allowed) {\n",width,width);
        fprintf(out,"    if(allowed && *live && dr_gpr_ram_hit(ea,%uu)) return dolrecomp_mem_read%u_fast(ctx,ram,ea,cia,cycles);\n",width/8,width);
        fputs("    if(*live) { dr_gpr_publish(ctx,r,*dirty); *live=false; }\n",out);
        fprintf(out,"    u%u value=dolrecomp_mem_read%u_fast(ctx,ram,ea,cia,cycles);\n",width,width);
        fputs("    if(allowed) { dr_gpr_reload(ctx,r); *dirty=0; *live=true; }\n    return value;\n}\n",out);
        fprintf(out,"#define DR_GPR_READ%u(ctx,ram,ea,cia,cycles) dr_gpr_read%u(ctx,ram,ea,cia,cycles,dr_gpr,&dr_gpr_dirty,&dr_gpr_live,dr_gpr_allowed)\n",width,width);
    }
    fputs("#else\n#define DR_GPR (ctx->gpr)\n#define DR_GPR_DIRTY(mask) ((void)0)\n#define DR_GPR_FLUSH() ((void)0)\n#define DR_GPR_RELOAD_IF() ((void)0)\n",out);
    for(unsigned width=8;width<=32;width*=2)fprintf(out,"#define DR_GPR_READ%u dolrecomp_mem_read%u_fast\n",width,width);
    fputs("#endif\n\n",out);
}

/* Select the bank at generation time, not through a pointer chosen by a
 * runtime liveness flag: constant indices can then scalarize into host SSA. */
static void emit_gpr_bank(FILE* out,bool local) {
    fputs("#undef DR_GPR\n",out);
    if(local) fputs("#if DOLRECOMP_GPR_LOCALS && !DOLRECOMP_BLOCK_ENTRY_COMPAT\n#define DR_GPR dr_gpr\n#else\n#define DR_GPR (ctx->gpr)\n#endif\n",out);
    else fputs("#define DR_GPR (ctx->gpr)\n",out);
}

/* SPDX-License-Identifier: MIT */
/* One caller-selected original function. No discovery or global planner. */
typedef struct {
    u32 count, entries_count, blocks_count;
    bool *leader, *pure, *entry;
    u32 *cost, *block_cost, *uses, *defs, *live_in, *live_out;
} DrFunctionPlan;
typedef struct {u32 width;bool indexed,update,store;} DrFunctionMemory;
static bool fu_memory(const PPCInst*,DrFunctionMemory*);

static void fu_plan_free(DrFunctionPlan* p) {
    free(p->leader); free(p->pure); free(p->entry); free(p->cost);
    free(p->block_cost); free(p->uses); free(p->defs);
    free(p->live_in); free(p->live_out); memset(p, 0, sizeof(*p));
}

/* Memory joins the local dataflow only after runtime eligibility. Declines
 * publish the exact boundary and leave the original page to own its effects.
 * Other closed helpers retain the existing FPR/FPSCR/SPR semantics. */
static bool fu_pure(const PPCInst* in) {
    DrFunctionMemory memory;if(fu_memory(in,&memory))return true;
    if(fp_scope_preserved(in->op))return true;
    switch(in->op) {
    case PPC_OP_MFSPR: case PPC_OP_MTSPR:
        return in->spr == 8 || in->spr == 9; /* LR/CTR only. */
    default: return gpr_locals_preserved(in->op);
    }
}

static u32 fu_defs(const PPCInst* in) {
    DrFunctionMemory memory;if(fu_memory(in,&memory)) {
        u32 result=memory.update?1u<<in->rA:0;
        if(!memory.store && !ppc_op_is_fp(in->op))result|=1u<<in->rD;
        return result;
    }
    if(ppc_op_is_fp(in->op)) return 0;
    if(in->op == PPC_OP_MFSPR) return 1u << in->rD;
    if(in->op == PPC_OP_MTSPR) return 0;
    return gpr_locals_written(in);
}

static u32 fu_uses(const PPCInst* in) {
    DrFunctionMemory memory;if(fu_memory(in,&memory)) {
        u32 result=(in->rA || memory.update)?1u<<in->rA:0;
        if(memory.indexed)result|=1u<<in->rB;
        if(memory.store && !ppc_op_is_fp(in->op))result|=1u<<in->rS;
        return result;
    }
    if(ppc_op_is_fp(in->op)) return 0;
    switch(in->op) {
    case PPC_OP_B: case PPC_OP_BC: case PPC_OP_BCLR: case PPC_OP_BCCTR:
    case PPC_OP_MFSPR: return 0;
    case PPC_OP_MTSPR: return 1u << in->rS;
    case PPC_OP_ADDI: case PPC_OP_ADDIS:
        return in->rA ? 1u << in->rA : 0;
    case PPC_OP_CMPI: case PPC_OP_CMPLI: case PPC_OP_MULLI:
    case PPC_OP_SUBFIC: case PPC_OP_ADDIC: case PPC_OP_ADDIC_DOT:
    case PPC_OP_NEG: case PPC_OP_NEGO:
    case PPC_OP_ADDME: case PPC_OP_ADDMEO: case PPC_OP_ADDZE: case PPC_OP_ADDZEO:
    case PPC_OP_SUBFME: case PPC_OP_SUBFMEO: case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO:
        return 1u << in->rA;
    case PPC_OP_CMP: case PPC_OP_CMPL:
    case PPC_OP_ADD: case PPC_OP_ADDO: case PPC_OP_ADDC: case PPC_OP_ADDCO:
    case PPC_OP_ADDE: case PPC_OP_ADDEO: case PPC_OP_SUBF: case PPC_OP_SUBFO:
    case PPC_OP_SUBFC: case PPC_OP_SUBFCO: case PPC_OP_SUBFE: case PPC_OP_SUBFEO:
    case PPC_OP_MULLW: case PPC_OP_MULLWO: case PPC_OP_MULHW: case PPC_OP_MULHWU:
    case PPC_OP_DIVW: case PPC_OP_DIVWO: case PPC_OP_DIVWU: case PPC_OP_DIVWUO:
        return (1u << in->rA) | (1u << in->rB);
    case PPC_OP_ORI: case PPC_OP_ORIS: case PPC_OP_XORI: case PPC_OP_XORIS:
    case PPC_OP_ANDI: case PPC_OP_ANDIS: case PPC_OP_CNTLZW:
    case PPC_OP_EXTSB: case PPC_OP_EXTSH: case PPC_OP_RLWINM: case PPC_OP_SRAWI:
        return 1u << in->rS;
    case PPC_OP_RLWIMI:
        return (1u << in->rS) | (1u << in->rA);
    case PPC_OP_AND: case PPC_OP_ANDC: case PPC_OP_OR: case PPC_OP_ORC:
    case PPC_OP_XOR: case PPC_OP_NAND: case PPC_OP_NOR: case PPC_OP_EQV:
    case PPC_OP_SLW: case PPC_OP_SRW: case PPC_OP_SRAW: case PPC_OP_RLWNM:
        return (1u << in->rS) | (1u << in->rB);
    default: return 0xffffffffu; /* Conservative for any new pure opcode. */
    }
}

static int fu_index(u32 target, u32 base, u32 count) {
    if(target < base || (target & 3u) || (target - base) / 4u >= count) return -1;
    return (int)((target - base) / 4u);
}

static bool fu_plan(DrFunctionPlan* p, const PPCInst* ins, u32 count, u32 base) {
    memset(p, 0, sizeof(*p));
    if(!count || (base & 3u) || (u64)base + (u64)count * 4u > 0xffffffffu ||
       s_relocatable) return false;
    if(ins[count-1].op != PPC_OP_BCLR || ins[count-1].lk || ins[count-1].bo != 20)
        return false;
    p->count=count;
    p->leader=calloc(count,sizeof(bool)); p->pure=calloc(count,sizeof(bool));
    p->entry=calloc(count,sizeof(bool)); p->cost=calloc(count,sizeof(u32));
    p->block_cost=calloc(count,sizeof(u32)); p->uses=calloc(count,sizeof(u32));
    p->defs=calloc(count,sizeof(u32)); p->live_in=calloc(count,sizeof(u32));
    p->live_out=calloc(count,sizeof(u32));
    if(!p->leader || !p->pure || !p->entry || !p->cost || !p->block_cost ||
       !p->uses || !p->defs || !p->live_in || !p->live_out) {
        fu_plan_free(p); return false;
    }
    for(u32 i=0;i<count;++i) {
        if(ins[i].address != base + i*4u || ins[i].embedded_data || ins[i].op == PPC_OP_UNKNOWN) {
            fu_plan_free(p); return false;
        }
        if(ins[i].op==PPC_OP_B || ins[i].op==PPC_OP_BC) {
            const bool inside=fu_index(ins[i].branch_target,base,count)>=0;
            /* A same-page edge outside this extent executes inside the old
             * native body (including its shadow-call policy). Returning it
             * here would add a scheduling checkpoint. Leave such a function
             * wholly on the original native compatibility path. */
            if((ins[i].lk && inside) ||
               (!inside && (ins[i].branch_target>>12)==(ins[i].address>>12))) {
                fu_plan_free(p); return false;
            }
        }
    }
    compute_block_costs(ins,count,base,base+count*4u,p->leader,p->cost);
    p->entry[0]=true;
    for(u32 i=0;i<count;++i) {
        if((ins[i].address & 4095u)==0) p->leader[i]=true;
        if((ins[i].op==PPC_OP_B || ins[i].op==PPC_OP_BC) && ins[i].lk && i+1<count)
            p->entry[i+1]=true;
        p->pure[i]=fu_pure(&ins[i]);
        if(p->pure[i]) {p->uses[i]=fu_uses(&ins[i]);p->defs[i]=fu_defs(&ins[i]);}
    }
    u32 running=0,first=0;
    for(u32 i=0;i<count;++i) {
        if(p->leader[i]) {running=0;first=i;++p->blocks_count;}
        running+=dr_ppc_num_cycles(ins[i].op);p->cost[i]=running;
        p->block_cost[first]=running;
        if(p->entry[i])++p->entries_count;
    }
    /* Backward fixed point on actual direct-control-flow successors. Ordinary
     * memory retains locals; a declined access exits before any effect. True
     * observing instructions still publish and end the local dataflow. */
    bool changed;
    do {
        changed=false;
        for(u32 n=count;n>0;--n) {
            u32 i=n-1,out=0;const PPCInst* in=&ins[i];int target;
            if(in->op==PPC_OP_B) {
                if(!in->lk && (target=fu_index(in->branch_target,base,count))>=0)
                    out|=p->live_in[target];
            } else if(in->op==PPC_OP_BC) {
                if(!in->lk && (target=fu_index(in->branch_target,base,count))>=0)
                    out|=p->live_in[target];
                if(i+1<count)out|=p->live_in[i+1];
            } else if(in->op==PPC_OP_BCLR || in->op==PPC_OP_BCCTR) {
                if(in->bo!=20 && i+1<count)out|=p->live_in[i+1];
            } else if(!ppc_op_always_exits(in->op) && i+1<count)out=p->live_in[i+1];
            u32 live=p->pure[i] ? p->uses[i] | (out & ~p->defs[i]) : 0;
            if(out!=p->live_out[i] || live!=p->live_in[i])changed=true;
            p->live_out[i]=out;p->live_in[i]=live;
        }
    }while(changed);
    return true;
}

static void fu_reload(FILE* out,u32 mask) {
    for(u32 r=0;r<32;++r)if(mask & (1u<<r))
        fprintf(out,"    dr_gpr[%u]=ctx->gpr[%u];\n",r,r);
    fputs("    dr_gpr_dirty=0;dr_gpr_live=true;\n",out);
}

static void fu_bank(FILE* out,bool local) {
    fprintf(out,"#undef DR_GPR\n#define DR_GPR %s\n",local?"dr_gpr":"(ctx->gpr)");
}

static void fu_reads(FILE* out,bool direct) {
    for(u32 width=8;width<=32;width*=2) {
        fprintf(out,"#undef DR_GPR_READ%u\n",width);
        if(direct)fprintf(out,"#define DR_GPR_READ%u dolrecomp_mem_read%u_fast\n",width,width);
        else fprintf(out,"#if DOLRECOMP_GPR_LOCALS\n#define DR_GPR_READ%u(ctx,ram,ea,cia,cycles) dr_gpr_read%u(ctx,ram,ea,cia,cycles,dr_gpr,&dr_gpr_dirty,&dr_gpr_live,dr_gpr_allowed)\n#else\n#define DR_GPR_READ%u dolrecomp_mem_read%u_fast\n#endif\n",width,width,width,width);
    }
}

static bool fu_memory(const PPCInst* in,DrFunctionMemory* memory) {
#define FU_MEM(op,width,indexed,update,store) case PPC_OP_##op: \
    *memory=(DrFunctionMemory){width,indexed,update,store};return true
    switch(in->op) {
    FU_MEM(LBZ,1,false,false,false);FU_MEM(LBZU,1,false,true,false);
    FU_MEM(LBZX,1,true,false,false);FU_MEM(LBZUX,1,true,true,false);
    FU_MEM(LHZ,2,false,false,false);FU_MEM(LHZU,2,false,true,false);
    FU_MEM(LHZX,2,true,false,false);FU_MEM(LHZUX,2,true,true,false);
    FU_MEM(LHA,2,false,false,false);FU_MEM(LHAU,2,false,true,false);
    FU_MEM(LHAX,2,true,false,false);FU_MEM(LHAUX,2,true,true,false);
    FU_MEM(LWZ,4,false,false,false);FU_MEM(LWZU,4,false,true,false);
    FU_MEM(LWZX,4,true,false,false);FU_MEM(LWZUX,4,true,true,false);
    FU_MEM(STB,1,false,false,true);FU_MEM(STBU,1,false,true,true);
    FU_MEM(STBX,1,true,false,true);FU_MEM(STBUX,1,true,true,true);
    FU_MEM(STH,2,false,false,true);FU_MEM(STHU,2,false,true,true);
    FU_MEM(STHX,2,true,false,true);FU_MEM(STHUX,2,true,true,true);
    FU_MEM(STW,4,false,false,true);FU_MEM(STWU,4,false,true,true);
    FU_MEM(STWX,4,true,false,true);FU_MEM(STWUX,4,true,true,true);
    FU_MEM(LFS,4,false,false,false);FU_MEM(LFSU,4,false,true,false);
    FU_MEM(LFSX,4,true,false,false);FU_MEM(LFSUX,4,true,true,false);
    FU_MEM(LFD,8,false,false,false);FU_MEM(LFDU,8,false,true,false);
    FU_MEM(LFDX,8,true,false,false);FU_MEM(LFDUX,8,true,true,false);
    FU_MEM(STFS,4,false,false,true);FU_MEM(STFSU,4,false,true,true);
    FU_MEM(STFSX,4,true,false,true);FU_MEM(STFSUX,4,true,true,true);
    FU_MEM(STFD,8,false,false,true);FU_MEM(STFDU,8,false,true,true);
    FU_MEM(STFDX,8,true,false,true);FU_MEM(STFDUX,8,true,true,true);
    default:return false;
    }
#undef FU_MEM
}

static void fu_memory_header(FILE* out) {
    fputs("#ifndef DOLRECOMP_FUNCTION_MEMORY_HELPERS\n#define DOLRECOMP_FUNCTION_MEMORY_HELPERS 1\n"
        "DR_MEM_INLINE bool dr_fu_mem1_offset(u32 ea,u32 width,u32* offset) {\n"
        "    if(ea>=GC_RAM_BASE && ea-GC_RAM_BASE<=GC_MAIN_RAM_SIZE-width)*offset=ea-GC_RAM_BASE;\n"
        "    else if(ea>=GC_RAM_UNCACHED && ea-GC_RAM_UNCACHED<=GC_MAIN_RAM_SIZE-width)*offset=ea-GC_RAM_UNCACHED;\n"
        "    else if(ea<=GC_MAIN_RAM_SIZE-width)*offset=ea;else return false;\n"
        "    return true;\n}\n",out);
    /* Eligibility has already excluded observers and changing backing. Keep
     * the original cached-MEM1 lowering; mapped MEM2 uses the original CIA
     * helpers without the outer wrapper's unobservable cycle publication. */
    for(u32 bits=8;bits<=64;bits*=2) {
        fprintf(out,"DR_MEM_INLINE u%u dr_fu_read%u(CPUState* ctx,u8* const ram,u32 ea,u32 cia,u64 cycles) {\n",bits,bits);
        fprintf(out,"    u32 offset;if(dr_fu_mem1_offset(ea,%uu,&offset))return dolrecomp_mem_read%u_fast(ctx,ram,ea,cia,cycles);\n",bits/8,bits);
        fprintf(out,"    return mem_read%u(ctx,ea,cia);\n}\n",bits);
        fprintf(out,"DR_MEM_INLINE void dr_fu_write%u(CPUState* ctx,u8* const ram,u32 ea,u%u value,u32 cia,u64 cycles) {\n",bits,bits);
        fprintf(out,"    u32 offset;if(dr_fu_mem1_offset(ea,%uu,&offset)){dolrecomp_mem_write%u_fast(ctx,ram,ea,value,cia,cycles);return;}\n",bits/8,bits);
        fprintf(out,"    mem_write%u(ctx,ea,value,cia);\n}\n",bits);
    }
    fputs("#endif\n",out);
}

static void fu_memory_aliases(FILE* out,bool closed) {
    for(u32 bits=8;bits<=64;bits*=2) {
        fprintf(out,"#undef dolrecomp_mem_read%u_fast\n#undef dolrecomp_mem_write%u_fast\n",bits,bits);
        if(closed)fprintf(out,"#define dolrecomp_mem_read%u_fast dr_fu_read%u\n#define dolrecomp_mem_write%u_fast dr_fu_write%u\n",bits,bits,bits,bits);
    }
}
static void fu_delegate_before_helper(FILE* out,const PPCInst* in,u32 before) {
    fputs("    DR_GPR_FLUSH();\n",out);
    fprintf(out,"    ctx->pc=0x%08Xu;ctx->cycles=dr_cycles+%uu;\n",in->address,before);
    /* The owner records this exact identity token. Returning immediately at
     * it transfers the original page BEFORE an opaque effect. Its original
     * body captures RAM and owns its in-flight remainder, including stores
     * that retire bindings. No new scheduling admission is permitted. */
    fputs("    (void)owner->guard(ctx,ctx->pc,ctx->cycles,false,owner->user);\n",out);
    fprintf(out,"    dr_cycles+=%uu;DR_RET();\n",before);
}
static void fu_memory_admit(FILE* out,const PPCInst* in,u32 before,const DrFunctionMemory* memory) {
    fputs("    {u32 dr_fu_ea=",out);
    if(memory->indexed)emit_xform_ea(out,in->rA,in->rB,memory->update);
    else emit_dform_ea(out,in->rA,in->simm,memory->update);
    fprintf(out,";if(!owner->memory || !owner->memory(ctx,dr_fu_ea,%uu,%s,owner->user)) {\n",memory->width,memory->store?"true":"false");
    fu_delegate_before_helper(out,in,before);
    fputs("    }}\n",out);
}

int emit_function_unit(FILE* out,const PPCInst* ins,u32 count,u32 base) {
    DrFunctionPlan p;
    if(!out || !ins || !fu_plan(&p,ins,count,base))return 0;
    const char* suffix=dolrecomp_symbol_suffix();
    fprintf(out,"\n/* Caller-verified true function CFG/liveness unit. */\n");
    fprintf(out,"static const u32 fu_entries_%08X%s[]={",base,suffix);
    for(u32 i=0;i<count;++i)if(p.entry[i])fprintf(out,"0x%08Xu,",ins[i].address);
    fprintf(out,"};\nstatic const u32 fu_blocks_%08X%s[]={",base,suffix);
    for(u32 i=0;i<count;++i)if(p.leader[i])fprintf(out,"0x%08Xu,",ins[i].address);
    fprintf(out,"};\nconst DrFunctionUnitInfo func_%08X_unit%s_info={0x%08Xu,0x%08Xu,fu_entries_%08X%s,%uu,fu_blocks_%08X%s,%uu};\n",
            base,suffix,base,base+count*4u,base,suffix,p.entries_count,base,suffix,p.blocks_count);
    fputs("#if DOLRECOMP_FUNCTION_UNIT && !DOLRECOMP_BLOCK_ENTRY_COMPAT\n",out);
    fu_memory_header(out);
    fu_memory_aliases(out,true);
    fprintf(out,"DR_MEM_INLINE void fu_publish_%08X%s(CPUState* ctx,const u32* r,u32 mask) {\n",base,suffix);
    for(u32 n=0;n<32;++n)fprintf(out,"    if(mask & (1u<<%u))ctx->gpr[%u]=r[%u];\n",n,n,n);
    fputs("}\n",out);
    fputs("#undef DR_GPR_FLUSH\n#undef DR_GPR_DIRTY\n",out);
    fprintf(out,"#define DR_GPR_FLUSH() do { if(dr_gpr_live){fu_publish_%08X%s(ctx,dr_gpr,dr_gpr_dirty);dr_gpr_live=false;} } while(0)\n",base,suffix);
    fputs("#define DR_GPR_DIRTY(mask) do { if(dr_gpr_live)dr_gpr_dirty|=(mask); } while(0)\n",out);
    fputs("#undef DR_RET\n#define DR_RET() do { DR_GPR_FLUSH();ctx->cycles=dr_cycles;return true; } while(0)\n",out);
    fu_reads(out,true);
    fprintf(out,"bool func_%08X_unit%s(CPUState* ctx,const DrFunctionUnitOwner* owner) {\n",base,suffix);
    fputs("    if(!ctx || !ctx->ram || !owner || !owner->guard)return false;\n"
          "    {uintptr_t c=(uintptr_t)ctx,r=(uintptr_t)ctx->ram;\n"
          "     if(c>=r ? c-r<GC_MAIN_RAM_SIZE : r-c<sizeof(*ctx))return false;}\n"
          "    u8* const dr_ram=ctx->ram;u64 dr_cycles=ctx->cycles;\n"
          "    const u64 dr_deadline=ctx->cycle_deadline;\n"
          "    u32 dr_gpr[32],dr_gpr_dirty=0;bool dr_gpr_live=false;\n"
          "    const bool dr_gpr_allowed=true;bool dr_fu_first=true;\n"
          "    u32 dr_fu_page=ctx->pc>>12;\n"
          "    (void)dr_ram;(void)dr_gpr_allowed;(void)dr_deadline;\n"
          "    switch(ctx->pc) {\n",out);
    for(u32 i=0;i<count;++i)if(p.entry[i]) {
        fprintf(out,"    case 0x%08Xu:\n",ins[i].address);
        fputs("        if(!owner->guard(ctx,ctx->pc,ctx->cycles,true,owner->user))return false;\n",out);
        fu_reload(out,p.live_in[i]);
        fprintf(out,"        goto fu_label_%08X;\n",ins[i].address);
    }
    fputs("    default:return false;\n    }\n",out);
    const char* previous=s_label_prefix;s_label_prefix="fu_label_";
    for(u32 i=0;i<count;++i) {
        u32 before=p.cost[i]-dr_ppc_num_cycles(ins[i].op);
        DrFunctionMemory memory;const bool access=fu_memory(&ins[i],&memory);
        fprintf(out,"fu_label_%08X:\n",ins[i].address);
        if(p.leader[i]) {
            fputs("    if(dr_fu_first)dr_fu_first=false;else {\n    DR_GPR_FLUSH();\n",out);
            fprintf(out,"    ctx->pc=0x%08Xu;ctx->cycles=dr_cycles;\n",ins[i].address);
            fprintf(out,"    if(!owner->guard(ctx,ctx->pc,ctx->cycles,dr_fu_page!=0x%08Xu,owner->user))return true;\n",ins[i].address>>12);
            fprintf(out,"    dr_fu_page=0x%08Xu;\n",ins[i].address>>12);
            fputs("    dr_gpr_dirty=0;dr_gpr_live=true;\n    }\n",out);
        }
        fu_bank(out,p.pure[i]);
        if(access)fu_memory_admit(out,&ins[i],before,&memory);
        else if(!p.pure[i])fu_delegate_before_helper(out,&ins[i],before);
        else if(!ppc_op_is_pc_pure(ins[i].op))
            fprintf(out,"    ctx->pc=0x%08Xu;\n",ins[i].address);
        emit_instruction_with_range(out,&ins[i],base,base+count*4u,p.cost[i],false,false);
        if(p.pure[i] && p.defs[i])fprintf(out,"    DR_GPR_DIRTY(0x%08Xu);\n",p.defs[i]);
        bool ends=i+1==count || p.leader[i+1];
        if(ends && !ppc_op_always_exits(ins[i].op))fprintf(out,"    dr_cycles+=%uu;\n",p.cost[i]);
    }
    fprintf(out,"    ctx->pc=0x%08Xu;DR_RET();\n}\n",base+count*4u);
    s_label_prefix=previous;
    fu_memory_aliases(out,false);
    fu_bank(out,false);fu_reads(out,false);
    fputs("#undef DR_GPR_FLUSH\n#undef DR_GPR_DIRTY\n#if DOLRECOMP_GPR_LOCALS\n"
          "#define DR_GPR_FLUSH() do { if(dr_gpr_live){dr_gpr_publish(ctx,dr_gpr,dr_gpr_dirty);dr_gpr_live=false;} } while(0)\n"
          "#define DR_GPR_DIRTY(mask) do { if(dr_gpr_live)dr_gpr_dirty|=(mask); } while(0)\n"
          "#else\n#define DR_GPR_FLUSH() ((void)0)\n#define DR_GPR_DIRTY(mask) ((void)0)\n#endif\n",out);
    fputs("#undef DR_RET\n#if DOLRECOMP_NATIVE_CHAIN\n#define DR_RET() do { DR_GPR_FLUSH();ctx->cycles=dr_cycles;if(dr_cycles>dr_chain_entry_cycles)(void)dr_native_chain_next(ctx);return; } while(0)\n#else\n#define DR_RET() do { DR_GPR_FLUSH();ctx->cycles=dr_cycles;return; } while(0)\n#endif\n",out);
    fprintf(out,"#else\nbool func_%08X_unit%s(CPUState* ctx,const DrFunctionUnitOwner* owner) {(void)ctx;(void)owner;return false;}\n#endif\n",base,suffix);
    fu_plan_free(&p);return 1;
}

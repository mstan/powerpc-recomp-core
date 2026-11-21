/* SPDX-License-Identifier: MIT */
typedef struct {
    s32 first, limit;
    unsigned accesses;
    bool writing;
} NativeStackSpan;

/* Both r1 and owner boundaries must remain stable. A pure instruction can
 * sit between memory accesses when it cannot publish state or write r1. */
static bool stack_span_pure(const PPCInst* in) {
    if(native_fp_op(in->op))
        return !fp_memory[in->op];
    switch(in->op) {
    case PPC_OP_ADDI: case PPC_OP_ADDIS: case PPC_OP_ADD: case PPC_OP_SUBF:
    case PPC_OP_MFCR:
        return in->rD!=1u;
    case PPC_OP_OR: case PPC_OP_XOR: case PPC_OP_AND:
    case PPC_OP_ORI: case PPC_OP_XORI: case PPC_OP_ANDI: case PPC_OP_ANDIS:
    case PPC_OP_RLWINM: case PPC_OP_RLWIMI:
        return in->rA!=1u;
    case PPC_OP_MFSPR:
        return in->rD!=1u && (in->spr==8u || in->spr==9u || in->spr==1u);
    case PPC_OP_MTSPR:
        return in->spr==8u || in->spr==9u || in->spr==1u;
    case PPC_OP_CMPI: case PPC_OP_CMPLI: case PPC_OP_CMP: case PPC_OP_CMPL:
        return true;
    default:
        return false;
    }
}

static bool stack_span_access(const PPCInst* in,NativeStackSpan* access,bool* changes_sp) {
    if(in->rA!=1u)
        return false;
    const IntegerMemory integer=integer_memory[in->op];
    const u16 fp=fp_memory[in->op];
    const bool multi=in->op==PPC_OP_LMW || in->op==PPC_OP_STMW;
    if((integer.flags&MEM_INDEXED) || (fp&(MEM_INDEXED|FP_QUANT)) ||
       (!integer.bytes && !fp && !multi))
        return false;
    const bool writing=(integer.flags&MEM_STORE) || (fp&MEM_STORE) || in->op==PPC_OP_STMW;
    const unsigned words=multi ? 32u-(writing ? in->rS : in->rD) : 1u;
    const unsigned width=integer.bytes ? integer.bytes : (fp&FP_DOUBLE) ? 8u : 4u;
    *access=(NativeStackSpan){in->simm,in->simm+(s32)(words*width),words,writing};
    *changes_sp=((integer.flags|fp)&MEM_UPDATE) || (!writing && !fp && in->rD<=1u &&
        (multi || in->rD==1u));
    return true;
}

/* Keep guest stack bytes in the real backing. Elide repeated dispatch only
 * within a branch-free memory group on the mapped alias, and re-admit across
 * any boundary that can change addresses or let a guest or host observe it. */
static u32 emit_stack_span(FILE* out,const PPCInst* instructions,const u8* leaders,
                           u32 start,u32 count,u32 relocation_pc,u32 wait_pc,
                           const NativeRegisters* regs) {
    NativeStackSpan span;
    bool changes_sp;
    const PPCInst* first=instructions+start;
    if(first->address==relocation_pc || (wait_pc && first->address==wait_pc) ||
       !stack_span_access(first,&span,&changes_sp))
        return start;
    u32 after=start+1u;
    for(u32 i=after;!changes_sp && i<count && !leaders[i];++i) {
        const PPCInst* in=instructions+i;
        if(in->address==relocation_pc || (wait_pc && in->address==wait_pc))
            break;
        NativeStackSpan access;
        if(stack_span_access(in,&access,&changes_sp)) {
            const s32 low=span.first<access.first ? span.first : access.first;
            const s32 high=span.limit>access.limit ? span.limit : access.limit;
            if(high-low>4096)
                break;
            span=(NativeStackSpan){low,high,span.accesses+access.accesses,
                                   span.writing || access.writing};
            after=i+1u;
        } else if(!stack_span_pure(in)) {
            break;
        }
    }
    if(span.accesses<3u)
        return start;
    fprintf(out,"#if defined(DR_NATIVE_STACK_SPANS) && DR_NATIVE_STACK_SPANS\n"
                "    if(dr_native_stack_span(cpu,mem,r[1],%d,%d,%s)) {\n",
            span.first,span.limit,span.writing ? "true" : "false");
    for(u32 i=start;i<after;++i)
        emit_body(out,instructions+i,false,regs,NULL);
    fputs("    } else\n#endif\n    {\n",out);
    for(u32 i=start;i<after;++i)
        emit_body(out,instructions+i,true,regs,NULL);
    fputs("    }\n",out);
    return after;
}

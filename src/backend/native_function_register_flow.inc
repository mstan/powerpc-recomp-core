/* SPDX-License-Identifier: MIT */
static u64 native_dirty_gpr(unsigned reg) {
    return (UINT64_C(1) << reg) & NATIVE_GPR_MASK;
}

static u64 native_dirty_fpr(unsigned reg) {
    return reg < NativeControlBase - NativeFprBase ? UINT64_C(1) << (NativeFprBase + reg) : 0;
}

/* Return every shared local this lowering can write. Unknown lowerings
 * keep the conservative full publication. The clock is always published. */
static u64 native_instruction_writes(const PPCInst* in) {
    if (native_fp_op(in->op)) {
        const u16 effects=fp_memory[in->op];
        if (effects)
            return (effects&MEM_UPDATE ? native_dirty_gpr(in->rA) : 0) |
                   (effects&MEM_STORE ? 0 : native_dirty_fpr(in->rD));
        return native_dirty_fpr(in->rD) | NATIVE_CR | NATIVE_FPSCR;
    }
    const IntegerMemory memory=integer_memory[in->op];
    if (memory.bytes)
        return (memory.flags&MEM_UPDATE ? native_dirty_gpr(in->rA) : 0) |
               (memory.flags&MEM_STORE ? 0 : native_dirty_gpr(in->rD));
    const u64 record=in->rc ? NATIVE_CR : 0;
    switch (in->op) {
    case PPC_OP_ADDI: case PPC_OP_ADDIS: case PPC_OP_MULLI:
    case PPC_OP_MFCR: case PPC_OP_MFSPR: case PPC_OP_MFTB: case PPC_OP_MFMSR:
        return native_dirty_gpr(in->rD);
    case PPC_OP_ADD: case PPC_OP_SUBF: case PPC_OP_MULHW: case PPC_OP_MULHWU:
        return native_dirty_gpr(in->rD) | record;
    case PPC_OP_ADDIC: case PPC_OP_ADDIC_DOT: case PPC_OP_SUBFIC:
    case PPC_OP_ADDO: case PPC_OP_ADDC: case PPC_OP_ADDCO: case PPC_OP_ADDE: case PPC_OP_ADDEO:
    case PPC_OP_ADDME: case PPC_OP_ADDMEO: case PPC_OP_ADDZE: case PPC_OP_ADDZEO:
    case PPC_OP_SUBFO: case PPC_OP_SUBFC: case PPC_OP_SUBFCO: case PPC_OP_SUBFE: case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME: case PPC_OP_SUBFMEO: case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO:
    case PPC_OP_NEG: case PPC_OP_NEGO: case PPC_OP_MULLW: case PPC_OP_MULLWO:
    case PPC_OP_DIVW: case PPC_OP_DIVWO: case PPC_OP_DIVWU: case PPC_OP_DIVWUO:
        return native_dirty_gpr(in->rD) | NATIVE_XER | NATIVE_CR;
    case PPC_OP_OR: case PPC_OP_AND: case PPC_OP_XOR:
    case PPC_OP_NAND: case PPC_OP_NOR: case PPC_OP_EQV: case PPC_OP_ANDC: case PPC_OP_ORC:
    case PPC_OP_RLWINM: case PPC_OP_RLWNM: case PPC_OP_RLWIMI:
    case PPC_OP_SLW: case PPC_OP_SRW: case PPC_OP_CNTLZW: case PPC_OP_EXTSB: case PPC_OP_EXTSH:
        return native_dirty_gpr(in->rA) | record;
    case PPC_OP_ANDI: case PPC_OP_ANDIS:
        return native_dirty_gpr(in->rA) | NATIVE_CR;
    case PPC_OP_SRAW: case PPC_OP_SRAWI:
        return native_dirty_gpr(in->rA) | record | NATIVE_XER;
    case PPC_OP_ORI: case PPC_OP_ORIS: case PPC_OP_XORI: case PPC_OP_XORIS:
        return native_dirty_gpr(in->rA);
    case PPC_OP_CMPI: case PPC_OP_CMPLI: case PPC_OP_CMP: case PPC_OP_CMPL:
    case PPC_OP_CRAND: case PPC_OP_CRANDC: case PPC_OP_CREQV: case PPC_OP_CRNAND:
    case PPC_OP_CRNOR: case PPC_OP_CROR: case PPC_OP_CRORC: case PPC_OP_CRXOR:
    case PPC_OP_MCRF: case PPC_OP_MTCRF:
        return NATIVE_CR;
    case PPC_OP_MCRXR:
        return NATIVE_CR | NATIVE_XER;
    case PPC_OP_MTSPR:
        switch (in->spr) {
        case 8: return NATIVE_LR;
        case 9: return NATIVE_CTR;
        case 1: return NATIVE_XER;
        default: return 0;
        }
    case PPC_OP_LMW:
        return NATIVE_GPR_MASK & (UINT64_MAX << in->rD);
    case PPC_OP_STMW: case PPC_OP_MTMSR: case PPC_OP_SYNC: case PPC_OP_EIEIO:
    case PPC_OP_ISYNC: case PPC_OP_TLBSYNC: case PPC_OP_DCBST: case PPC_OP_DCBF:
    case PPC_OP_DCBI: case PPC_OP_ICBI: case PPC_OP_DCBZ: case PPC_OP_DCBZ_L:
    case PPC_OP_DCBT: case PPC_OP_DCBTST:
    case PPC_OP_SC: case PPC_OP_RFI:
        return 0;
    default:
        return UINT64_MAX;
    }
}

static u64 native_edge_writes(const PPCInst* in,DrNativeTransfer edge,bool conditional) {
    u64 writes=(in->lk ? NATIVE_LR : 0) | (conditional && !(in->bo&4u) ? NATIVE_CTR : 0);
    if (edge.kind==DR_NATIVE_SAVE_RESTORE) {
        if(edge.symbol->kind==DR_NATIVE_RESTORE_GPR)
            writes |= NATIVE_GPR_MASK & (UINT64_MAX << edge.symbol->first_register);
        else if(edge.symbol->kind==DR_NATIVE_RESTORE_FPR)
            for(unsigned reg=edge.symbol->first_register;reg<32u;++reg)
                writes |= native_dirty_fpr(reg);
    }
    return writes;
}

typedef struct {
    u64 incoming;
    u64 writes;
    DrNativeTransfer edge;
    bool reached, conditional;
} NativeRegisterFlow;

static bool native_merge_register_flow(NativeRegisterFlow* instruction,u64 dirty) {
    if (instruction->reached && (instruction->incoming|dirty)==instruction->incoming)
        return false;
    instruction->reached=true;
    instruction->incoming|=dirty;
    return true;
}

/* The generated function has normal entries and compiler-known local
 * transfers. A returning call publishes and reloads the bank. A conditional
 * call may also bypass both, in which case its incoming writes stay live. */
static NativeRegisterFlow* native_register_flow(const DrNativeSymbols* symbols,
                                const DrNativeSymbol* function,const PPCInst* instructions,
                                u32 count,const DrNativeEmission* emission) {
    NativeRegisterFlow* flow=calloc(count,sizeof(*flow));
    if (!flow)
        return NULL;
    size_t next_relocation=0;
    for(u32 i=0;i<count;++i) {
        const PPCInst* in=instructions+i;
        NativeRegisterFlow* node=flow+i;
        node->edge=dr_native_transfer(symbols,function,in);
        if(next_relocation<emission->relocation_count &&
           emission->relocations[next_relocation].address==in->address) {
            const DrNativeRelocation* relocation=&emission->relocations[next_relocation++];
            if(relocation->kind==DR_NATIVE_REL24) {
                node->edge.kind=relocation->helper ? DR_NATIVE_SAVE_RESTORE :
                    (in->lk ? DR_NATIVE_CALL_FUNCTION : DR_NATIVE_BRANCH_FUNCTION);
                node->edge.symbol=relocation->helper;
            }
        }
        node->conditional=in->op!=PPC_OP_B && in->bo!=20u;
        node->writes=node->edge.kind==DR_NATIVE_STRAIGHT_LINE ? native_instruction_writes(in):
            native_edge_writes(in,node->edge,node->conditional);
    }

    flow[0].reached=true;
    bool changed;
    do {
        changed=false;
        for(u32 i=0;i<count;++i) {
            const NativeRegisterFlow* node=flow+i;
            if (!node->reached)
                continue;
            const PPCInst* in=instructions+i;
            u64 dirty=node->incoming|node->writes;
            bool falls_through=true;
            switch(node->edge.kind) {
            case DR_NATIVE_BRANCH_LOCAL:
                changed|=native_merge_register_flow(flow+(node->edge.target-function->address)/4u,dirty);
                falls_through=node->conditional;
                break;
            case DR_NATIVE_BRANCH_INDIRECT:
                for(size_t target=0;target<emission->target_count;++target)
                    changed|=native_merge_register_flow(
                        flow+(emission->local_targets[target]-function->address)/4u,dirty);
                falls_through=node->conditional;
                break;
            case DR_NATIVE_BRANCH_FUNCTION: case DR_NATIVE_RETURN:
                falls_through=node->conditional;
                break;
            case DR_NATIVE_SAVE_RESTORE:
                falls_through=in->lk || node->conditional;
                break;
            case DR_NATIVE_CALL_FUNCTION: case DR_NATIVE_CALL_INDIRECT:
                if(!node->conditional)
                    dirty=0;
                break;
            case DR_NATIVE_STRAIGHT_LINE:
                if(in->op==PPC_OP_SC)
                    dirty=0;
                falls_through=in->op!=PPC_OP_RFI;
                break;
            default:
                /* A rejected transfer traps. Keeping fallthrough conservative
                 * only affects publication on code that cannot execute. */
                break;
            }
            if(falls_through && i+1u<count)
                changed|=native_merge_register_flow(flow+i+1u,dirty);
        }
    } while(changed);
    return flow;
}

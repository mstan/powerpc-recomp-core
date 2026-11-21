/* SPDX-License-Identifier: MIT */
static bool native_fp_op(PPCOpcode op) {
    return op==PPC_OP_STFIWX || (op>=PPC_OP_LFS && op<=PPC_OP_PS_SEL);
}

static unsigned fp_field_mask(u8 fields) {
    unsigned mask=0;
    for (unsigned i=0;i<8;++i)
        if (fields & (1u<<i))
            mask|=15u<<(4u*i);
    return mask;
}

enum { FP_DOUBLE=32, FP_QUANT=64, FP_INTEGER=128, FP_SINGLE=256 };
static const u16 fp_memory[PPC_OP_COUNT]={
    [PPC_OP_LFS]=FP_SINGLE, [PPC_OP_LFSU]=FP_SINGLE|MEM_UPDATE,
    [PPC_OP_LFSX]=FP_SINGLE|MEM_INDEXED, [PPC_OP_LFSUX]=FP_SINGLE|MEM_INDEXED|MEM_UPDATE,
    [PPC_OP_LFD]=FP_DOUBLE, [PPC_OP_LFDU]=FP_DOUBLE|MEM_UPDATE,
    [PPC_OP_LFDX]=FP_DOUBLE|MEM_INDEXED, [PPC_OP_LFDUX]=FP_DOUBLE|MEM_INDEXED|MEM_UPDATE,
    [PPC_OP_STFS]=MEM_STORE, [PPC_OP_STFSU]=MEM_STORE|MEM_UPDATE,
    [PPC_OP_STFSX]=MEM_STORE|MEM_INDEXED, [PPC_OP_STFSUX]=MEM_STORE|MEM_INDEXED|MEM_UPDATE,
    [PPC_OP_STFD]=FP_DOUBLE|MEM_STORE, [PPC_OP_STFDU]=FP_DOUBLE|MEM_STORE|MEM_UPDATE,
    [PPC_OP_STFDX]=FP_DOUBLE|MEM_STORE|MEM_INDEXED,
    [PPC_OP_STFDUX]=FP_DOUBLE|MEM_STORE|MEM_INDEXED|MEM_UPDATE,
    [PPC_OP_STFIWX]=FP_INTEGER|MEM_STORE|MEM_INDEXED,
    [PPC_OP_PSQ_L]=FP_QUANT, [PPC_OP_PSQ_LU]=FP_QUANT|MEM_UPDATE,
    [PPC_OP_PSQ_LX]=FP_QUANT|MEM_INDEXED, [PPC_OP_PSQ_LUX]=FP_QUANT|MEM_INDEXED|MEM_UPDATE,
    [PPC_OP_PSQ_ST]=FP_QUANT|MEM_STORE, [PPC_OP_PSQ_STU]=FP_QUANT|MEM_STORE|MEM_UPDATE,
    [PPC_OP_PSQ_STX]=FP_QUANT|MEM_STORE|MEM_INDEXED,
    [PPC_OP_PSQ_STUX]=FP_QUANT|MEM_STORE|MEM_INDEXED|MEM_UPDATE,
};

static void emit_fp_memory(FILE* out,const PPCInst* in,bool checked,unsigned flags,const char* immediate) {
    const bool store=(flags&MEM_STORE)!=0;
    emit_address(out,in,flags,immediate);
    if (flags&FP_QUANT) {
        if (store)
            fprintf(out,"    dr_native_quant_store(cpu,mem,DR_NATIVE_GUEST_PC(0x%08Xu),ea,cpu->gqr[%u],%u,%s,f[%u]);\n",
                    in->address,in->i,in->w,timing_arguments(checked),in->rS);
        else
            fprintf(out,"    f[%u] = dr_native_quant_load(cpu,mem,DR_NATIVE_GUEST_PC(0x%08Xu),ea,cpu->gqr[%u],%u,%s);\n",
                    in->rD,in->address,in->i,in->w,timing_arguments(checked));
    } else if (flags&FP_DOUBLE) {
        if (store)
            fprintf(out,"    dr_native_store_double(cpu,mem,ea,f[%u].lane[0],%s);\n",in->rS,timing_arguments(checked));
        else
            fprintf(out,"    f[%u].lane[0] = dr_native_load_double(cpu,mem,ea,%s);\n",in->rD,timing_arguments(checked));
    } else if (store) {
        fprintf(out,"    dr_native_memory_write(cpu,mem,ea,4,%s(f[%u].lane[0]),%s);\n",
                flags&FP_INTEGER ? "(u32)dr_native_float64_bits" : "dr_native_float32_bits",in->rS,timing_arguments(checked));
    } else {
        fprintf(out,"    f[%u].lane[1] = f[%u].lane[0] = dr_native_float32(dr_native_memory_read(cpu,mem,ea,4,%s));\n",
                in->rD,in->rD,timing_arguments(checked));
    }
    if (flags&MEM_UPDATE)
        fprintf(out,"    r[%u] = ea;\n",in->rA);
    fputs("    }\n",out);
}

static bool fp_single(PPCOpcode op) {
    return (op>=PPC_OP_FADDS && op<=PPC_OP_FNMSUBS) || op==PPC_OP_FRSP;
}

static bool emit_float_math(FILE* out,const PPCInst* in) {
    const bool pair=in->op>=PPC_OP_PS_ADD && in->op<=PPC_OP_PS_SEL;
    const bool single=pair||fp_single(in->op);
    /* Scalar single instructions round their result. Their FPR operands can
     * hold doubles, including the SDK's 2^52 integer conversion sequence. */
    const char* cast=pair ? "(f32)" : "";
    char expr[2][160];
    for (unsigned lane=0;lane<(pair?2u:1u);++lane) {
        char a[32],b[32],c[32];
        snprintf(a,sizeof(a),"f[%u].lane[%u]",in->rA,lane);
        snprintf(b,sizeof(b),"f[%u].lane[%u]",in->rB,lane);
        snprintf(c,sizeof(c),"f[%u].lane[%u]",in->rC,lane);
        char* result=expr[lane];
        switch (in->op) {
        case PPC_OP_FADDS: case PPC_OP_FADD: case PPC_OP_PS_ADD:
        case PPC_OP_FSUBS: case PPC_OP_FSUB: case PPC_OP_PS_SUB:
        case PPC_OP_FDIVS: case PPC_OP_FDIV: case PPC_OP_PS_DIV: {
            const bool add=in->op==PPC_OP_FADDS || in->op==PPC_OP_FADD || in->op==PPC_OP_PS_ADD;
            const bool sub=in->op==PPC_OP_FSUBS || in->op==PPC_OP_FSUB || in->op==PPC_OP_PS_SUB;
            snprintf(result,160,"%s%s %c %s%s",cast,a,add?'+':sub?'-':'/',cast,b);
            break;
        }
        case PPC_OP_FMULS: case PPC_OP_FMUL: case PPC_OP_PS_MUL:
        case PPC_OP_PS_MULS0: case PPC_OP_PS_MULS1:
            if (in->op==PPC_OP_PS_MULS0 || in->op==PPC_OP_PS_MULS1)
                snprintf(c,sizeof(c),"f[%u].lane[%u]",in->rC,in->op==PPC_OP_PS_MULS1);
            snprintf(result,160,"%s%s * %s%s",cast,a,cast,c);
            break;
        case PPC_OP_FMADDS: case PPC_OP_FMADD: case PPC_OP_PS_MADD:
        case PPC_OP_FMSUBS: case PPC_OP_FMSUB: case PPC_OP_PS_MSUB:
        case PPC_OP_FNMADDS: case PPC_OP_FNMADD: case PPC_OP_PS_NMADD:
        case PPC_OP_FNMSUBS: case PPC_OP_FNMSUB: case PPC_OP_PS_NMSUB:
        case PPC_OP_PS_MADDS0: case PPC_OP_PS_MADDS1: {
            const bool sub=in->op==PPC_OP_FMSUBS || in->op==PPC_OP_FMSUB || in->op==PPC_OP_PS_MSUB ||
                           in->op==PPC_OP_FNMSUBS || in->op==PPC_OP_FNMSUB || in->op==PPC_OP_PS_NMSUB;
            const bool negate=in->op==PPC_OP_FNMADDS || in->op==PPC_OP_FNMADD || in->op==PPC_OP_PS_NMADD ||
                              in->op==PPC_OP_FNMSUBS || in->op==PPC_OP_FNMSUB || in->op==PPC_OP_PS_NMSUB;
            if (in->op==PPC_OP_PS_MADDS0 || in->op==PPC_OP_PS_MADDS1)
                snprintf(c,sizeof(c),"f[%u].lane[%u]",in->rC,in->op==PPC_OP_PS_MADDS1);
            snprintf(result,160,"%s(%s(%s%s * %s%s) %c %s%s)",negate?"-":"",cast,cast,a,cast,c,sub?'-':'+',cast,b);
            break;
        }
        case PPC_OP_FSEL: case PPC_OP_PS_SEL:
            snprintf(result,160,"%s >= 0.0 ? %s : %s",a,c,b); break;
        case PPC_OP_FMR: case PPC_OP_FRSP:
            snprintf(result,160,"%s",b); break;
        case PPC_OP_FNEG: case PPC_OP_PS_NEG:
            snprintf(result,160,"-%s",b); break;
        case PPC_OP_FABS: case PPC_OP_PS_ABS: case PPC_OP_FNABS: case PPC_OP_PS_NABS:
            snprintf(result,160,"%sfabs(%s)",in->op==PPC_OP_FNABS || in->op==PPC_OP_PS_NABS ? "-" : "",b);
            break;
        case PPC_OP_FRES: case PPC_OP_PS_RES:
            snprintf(result,160,"1.0f / (f32)%s",b); break;
        case PPC_OP_FRSQRTE: case PPC_OP_PS_RSQRTE:
            snprintf(result,160,"1.0 / sqrt(%s)",b); break;
        case PPC_OP_FCTIW: case PPC_OP_FCTIWZ:
            snprintf(result,160,"dr_native_fp_integer(%s,%u)",b,in->op==PPC_OP_FCTIWZ); break;
        case PPC_OP_PS_SUM0: case PPC_OP_PS_SUM1:
            if (lane==(in->op==PPC_OP_PS_SUM1))
                snprintf(result,160,"(f32)f[%u].lane[0] + (f32)f[%u].lane[1]",in->rA,in->rB);
            else
                snprintf(result,160,"%s",c);
            break;
        case PPC_OP_PS_MERGE00: case PPC_OP_PS_MERGE01: case PPC_OP_PS_MERGE10: case PPC_OP_PS_MERGE11: {
            const unsigned pattern=in->op-PPC_OP_PS_MERGE00;
            snprintf(result,160,"f[%u].lane[%u]",lane?in->rB:in->rA,(pattern>>(1u-lane))&1u);
            break;
        }
        default:
            return false;
        }
    }
    if (pair)
        fprintf(out,"    f[%u] = (DrNativeFloat){{(f32)(%s),(f32)(%s)}};\n",in->rD,expr[0],expr[1]);
    else if (single)
        fprintf(out,"    f[%u].lane[1] = f[%u].lane[0] = (f32)(%s);\n",in->rD,in->rD,expr[0]);
    else
        fprintf(out,"    f[%u].lane[0] = %s;\n",in->rD,expr[0]);
    return true;
}

static bool emit_float(FILE* out,const PPCInst* in,bool checked,const char* immediate) {
    const unsigned memory=fp_memory[in->op];
    if (memory) {
        emit_fp_memory(out,in,checked,memory,immediate);
        return true;
    }
    const unsigned dst_shift=28u-4u*in->crfD;
    switch (in->op) {
    case PPC_OP_PS_MR:
        fprintf(out,"    f[%u] = f[%u];\n",in->rD,in->rB);
        break;
    case PPC_OP_FCMPU: case PPC_OP_FCMPO: case PPC_OP_PS_CMPU0: case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1: case PPC_OP_PS_CMPO1: {
        const unsigned lane=in->op==PPC_OP_PS_CMPU1 || in->op==PPC_OP_PS_CMPO1;
        fprintf(out,"    cr = dr_native_fp_cr(cr,%u,f[%u].lane[%u],f[%u].lane[%u]);\n",
                in->crfD,in->rA,lane,in->rB,lane);
        break;
    }
    case PPC_OP_MFFS:
        fprintf(out,"    f[%u].lane[0] = dr_native_float64(0xfff8000000000000ull | fpscr);\n",in->rD);
        break;
    case PPC_OP_MTFSF: {
        const u32 mask=fp_field_mask(in->fm);
        fprintf(out,"    fpscr = (fpscr & 0x%08Xu) | ((u32)dr_native_float64_bits(f[%u].lane[0]) & 0x%08Xu);\n",
                ~mask,in->rB,mask);
        break;
    }
    case PPC_OP_MTFSFI:
        fprintf(out,"    fpscr = (fpscr & 0x%08Xu) | 0x%08Xu;\n",~(15u<<dst_shift),(u32)in->imm<<dst_shift);
        break;
    case PPC_OP_MTFSB0: case PPC_OP_MTFSB1: {
        const u32 mask=1u<<(31u-in->rD);
        fprintf(out,"    fpscr %s= 0x%08Xu;\n",in->op==PPC_OP_MTFSB1 ? "|" : "&",
                in->op==PPC_OP_MTFSB1 ? mask : ~mask);
        break;
    }
    case PPC_OP_MCRFS: {
        const unsigned src_shift=28u-4u*in->crfS;
        fprintf(out,"    cr = (cr & 0x%08Xu) | (((fpscr >> %u) & 15u) << %u);\n",~(15u<<dst_shift),src_shift,dst_shift);
        fprintf(out,"    fpscr &= 0x%08Xu;\n",~((15u<<src_shift)&0x83f80700u));
        break;
    }
    default:
        if (!emit_float_math(out,in))
            return false;
        break;
    }
    if (in->rc)
        fputs("    cr = (cr & 0xf0ffffffu) | ((fpscr >> 4) & 0x0f000000u);\n",out);
    return true;
}

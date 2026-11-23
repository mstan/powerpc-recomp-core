/* SPDX-License-Identifier: MIT */
#include "backend/native_function_emit.h"
#include "backend/native_function_abi.h"
#include "backend/ppc_cycles.h"
#include <stdlib.h>

enum {
    NativeFprBase = sizeof(((DrNativeCpu*)0)->gpr) / sizeof(u32),
    NativeControlBase = NativeFprBase + sizeof(((DrNativeCpu*)0)->fpr) / sizeof(DrNativeFloat),
};
_Static_assert(NativeControlBase + 5 <= 64, "Native register effect mask too narrow");
#define NATIVE_CR (UINT64_C(1) << NativeControlBase)
#define NATIVE_LR (NATIVE_CR << 1)
#define NATIVE_CTR (NATIVE_CR << 2)
#define NATIVE_XER (NATIVE_CR << 3)
#define NATIVE_FPSCR (NATIVE_CR << 4)
#define NATIVE_GPR_MASK ((UINT64_C(1) << NativeFprBase) - 1u)

typedef struct {
    u32 gpr, fpr;
    bool floating, saving_thread;
    u64 dirty;
} NativeRegisters;

/* Nonvolatile registers remain on the host stack. Of the shared registers,
 * copy only those referenced by this frame. The rest stay in canonical state
 * across calls, observers and safepoints. */
static void emit_register_copy(FILE* out, u32 mask, const char* to, const char* from) {
    unsigned first = 0;
    while (mask) {
        if (!(mask & 1u)) {
            ++first;
            mask >>= 1;
            continue;
        }
        unsigned count = 0;
        do {
            ++count;
            mask >>= 1;
        } while (mask & 1u);
        if (count == 1)
            fprintf(out, "    %s[%u] = %s[%u];\n", to, first, from, first);
        else
            fprintf(out, "    memcpy(%s+%u, %s+%u, %uu*sizeof(%s[0]));\n",
                    to, first, from, first, count, to);
        first += count;
    }
}

static void emit_reload(FILE* out, const NativeRegisters* regs) {
    emit_register_copy(out, regs->gpr, "r", "cpu->gpr");
    fputs("    cr = cpu->cr; lr = cpu->lr; ctr = cpu->ctr; xer = cpu->xer;\n"
          "    work = cpu->work;\n", out);
    if (regs->floating) {
        emit_register_copy(out, regs->fpr, "f", "cpu->fpr");
        fputs("    fpscr = cpu->fpscr;\n", out);
    }
}

static void emit_publish(FILE* out, const NativeRegisters* regs) {
    emit_register_copy(out, regs->gpr & (u32)regs->dirty, "cpu->gpr", "r");
    if (regs->dirty & NATIVE_CR) fputs("    cpu->cr = cr;\n",out);
    if (regs->dirty & NATIVE_LR) fputs("    cpu->lr = lr;\n",out);
    if (regs->dirty & NATIVE_CTR) fputs("    cpu->ctr = ctr;\n",out);
    if (regs->dirty & NATIVE_XER) fputs("    cpu->xer = xer;\n",out);
    fputs("    cpu->work = work;\n",out);
    if (regs->floating) {
        emit_register_copy(out, regs->fpr & (u32)(regs->dirty >> NativeFprBase), "cpu->fpr", "f");
        if (regs->dirty & NATIVE_FPSCR) fputs("    cpu->fpscr = fpscr;\n",out);
    }
}

static void emit_resume_check(FILE* out,const NativeRegisters* regs) {
    if(regs->saving_thread)
        fputs("    if (cpu->resume == &saved_context) goto dr_native_resume_thread;\n",out);
    fputs("    if (cpu->resume) return;\n",out);
}

static void emit_fuel_check(FILE* out, u32 pc, const NativeRegisters* regs) {
    fputs("    if (work >= cpu->deadline) {\n", out);
    emit_publish(out,regs);
    fprintf(out, "    cpu->service(cpu, DR_NATIVE_GUEST_PC(0x%08Xu));\n", pc);
    emit_resume_check(out,regs);
    emit_reload(out,regs);
    fputs("    }\n", out);
}

/* Typed effects accept an operand and the virtual clock. The service boundary
 * publishes registers before delivering an interrupt or resuming a context. */
static void emit_clock_publish(FILE* out) {
    fputs("    cpu->work = work;\n",out);
}

static void emit_clock_reload(FILE* out) {
    fputs("    work = cpu->work;\n",out);
}
static void emit_fault(FILE* out, const PPCInst* in) {
    fprintf(out, "    dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0x%08Xu);\n", in->address, in->raw);
}

static void emit_cmp(FILE* out, const PPCInst* in, bool immediate, bool sign) {
    char rhs[32];
    if (immediate)
        snprintf(rhs, sizeof(rhs), "0x%08Xu", sign ? (u32)(s32)in->simm : in->uimm);
    else
        snprintf(rhs, sizeof(rhs), "r[%u]", in->rB);
    const char* cast = sign ? "(s32)" : "";
    fprintf(out, "    cr = dr_native_compare(cr, xer, %u, %sr[%u] < %s%s, %sr[%u] > %s%s);\n",
            in->crfD, cast, in->rA, cast, rhs, cast, in->rA, cast, rhs);
}

static void emit_record(FILE* out, bool record, u8 reg) {
    if (record)
        fprintf(out, "    cr = dr_native_compare(cr, xer, 0, (s32)r[%u] < 0, (s32)r[%u] > 0);\n", reg, reg);
}

enum { MEM_STORE=1, MEM_UPDATE=2, MEM_SIGNED=4, MEM_INDEXED=8, MEM_REVERSED=16 };
typedef struct { u8 bytes, flags; } IntegerMemory;

static const IntegerMemory integer_memory[PPC_OP_COUNT] = {
    [PPC_OP_LWZ]={4,0}, [PPC_OP_LWZU]={4,MEM_UPDATE},
    [PPC_OP_LBZ]={1,0}, [PPC_OP_LBZU]={1,MEM_UPDATE},
    [PPC_OP_LHZ]={2,0}, [PPC_OP_LHZU]={2,MEM_UPDATE},
    [PPC_OP_LHA]={2,MEM_SIGNED}, [PPC_OP_LHAU]={2,MEM_SIGNED|MEM_UPDATE},
    [PPC_OP_STW]={4,MEM_STORE}, [PPC_OP_STWU]={4,MEM_STORE|MEM_UPDATE},
    [PPC_OP_STH]={2,MEM_STORE}, [PPC_OP_STHU]={2,MEM_STORE|MEM_UPDATE},
    [PPC_OP_STB]={1,MEM_STORE}, [PPC_OP_STBU]={1,MEM_STORE|MEM_UPDATE},
    [PPC_OP_LWZX]={4,MEM_INDEXED}, [PPC_OP_LWZUX]={4,MEM_INDEXED|MEM_UPDATE},
    [PPC_OP_LBZX]={1,MEM_INDEXED}, [PPC_OP_LBZUX]={1,MEM_INDEXED|MEM_UPDATE},
    [PPC_OP_LHZX]={2,MEM_INDEXED}, [PPC_OP_LHZUX]={2,MEM_INDEXED|MEM_UPDATE},
    [PPC_OP_LHAX]={2,MEM_INDEXED|MEM_SIGNED}, [PPC_OP_LHAUX]={2,MEM_INDEXED|MEM_SIGNED|MEM_UPDATE},
    [PPC_OP_STWX]={4,MEM_INDEXED|MEM_STORE}, [PPC_OP_STWUX]={4,MEM_INDEXED|MEM_STORE|MEM_UPDATE},
    [PPC_OP_STHX]={2,MEM_INDEXED|MEM_STORE}, [PPC_OP_STHUX]={2,MEM_INDEXED|MEM_STORE|MEM_UPDATE},
    [PPC_OP_STBX]={1,MEM_INDEXED|MEM_STORE}, [PPC_OP_STBUX]={1,MEM_INDEXED|MEM_STORE|MEM_UPDATE},
    [PPC_OP_LWBRX]={4,MEM_INDEXED|MEM_REVERSED}, [PPC_OP_LHBRX]={2,MEM_INDEXED|MEM_REVERSED},
    [PPC_OP_STWBRX]={4,MEM_INDEXED|MEM_STORE|MEM_REVERSED},
    [PPC_OP_STHBRX]={2,MEM_INDEXED|MEM_STORE|MEM_REVERSED},
};

static void emit_address(FILE* out, const PPCInst* in, unsigned flags, const char* immediate) {
    fputs("    { u32 ea = ", out);
    if (in->rA || (flags & MEM_UPDATE))
        fprintf(out, "r[%u] + ", in->rA);
    if (flags & MEM_INDEXED)
        fprintf(out, "r[%u];\n", in->rB);
    else
        fprintf(out, "%s;\n", immediate);
}

static const char* timing_arguments(bool checked) {
    return checked ? "true, work" : "false, 0";
}

static void emit_memory(FILE* out, const PPCInst* in, bool checked, IntegerMemory op,
                        const char* immediate) {
    emit_address(out, in, op.flags, immediate);
    const char* swap = op.bytes == 4 ? "dr_native_swap32" : "dr_native_swap16";
    const bool reversed = (op.flags & MEM_REVERSED) != 0;
    if (op.flags & MEM_STORE) {
        if (reversed)
            fprintf(out, "    u32 value = %s(r[%u]);\n", swap, in->rS);
        else
            fprintf(out, "    u32 value = r[%u];\n", in->rS);
        fprintf(out, "    dr_native_memory_write(cpu,mem,ea,%u,value,%s);\n",
                op.bytes, timing_arguments(checked));
    } else {
        fprintf(out, "    u32 value = dr_native_memory_read(cpu,mem,ea,%u,%s);\n",
                op.bytes, timing_arguments(checked));
        if (reversed)
            fprintf(out, "    value = %s(value);\n", swap);
        fprintf(out, "    r[%u] = %svalue;\n", in->rD, (op.flags & MEM_SIGNED) ? "(u32)(s32)(s16)" : "");
    }
    if (op.flags & MEM_UPDATE)
        fprintf(out, "    r[%u] = ea;\n", in->rA);
    fputs("    }\n", out);
}

static void emit_multi_memory(FILE* out, const PPCInst* in, bool checked, const char* immediate) {
    const bool store = in->op == PPC_OP_STMW;
    emit_address(out, in, 0, immediate);
    const unsigned first = store ? in->rS : in->rD;
    for (unsigned reg = first; reg < 32; ++reg) {
        const unsigned offset = (reg - first)*4u;
        if (store)
            fprintf(out, "    dr_native_memory_write(cpu,mem,ea+%uu,4,r[%u],%s);\n",
                    offset, reg, timing_arguments(checked));
        else
            fprintf(out, "    r[%u] = dr_native_memory_read(cpu,mem,ea+%uu,4,%s);\n",
                    reg, offset, timing_arguments(checked));
    }
    fputs("    }\n", out);
}

static u32 rotate_mask(unsigned start, unsigned end) {
    const u32 low = ~0u >> start;
    const u32 high = ~0u << (31u-end);
    return start <= end ? low & high : low | high;
}

static void emit_carry_arithmetic(FILE* out, const PPCInst* in, const char* immediate) {
    char left[32], right[64];
    const char* carry = "0";
    bool record_carry = true;
    snprintf(left, sizeof(left), "r[%u]", in->rA);
    snprintf(right, sizeof(right), "r[%u]", in->rB);
    switch (in->op) {
    case PPC_OP_ADDIC: case PPC_OP_ADDIC_DOT:
        snprintf(right, sizeof(right), "%s", immediate);
        break;
    case PPC_OP_ADDO:
        record_carry = false;
        break;
    case PPC_OP_ADDE: case PPC_OP_ADDEO:
        carry = "((xer >> 29) & 1u)";
        break;
    case PPC_OP_ADDME: case PPC_OP_ADDMEO: case PPC_OP_ADDZE: case PPC_OP_ADDZEO:
        snprintf(right, sizeof(right), "%s", in->op == PPC_OP_ADDME || in->op == PPC_OP_ADDMEO ? "~0u" : "0");
        carry = "((xer >> 29) & 1u)";
        break;
    case PPC_OP_SUBFIC: case PPC_OP_SUBFO: case PPC_OP_SUBFC: case PPC_OP_SUBFCO:
    case PPC_OP_SUBFE: case PPC_OP_SUBFEO: case PPC_OP_SUBFME: case PPC_OP_SUBFMEO:
    case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO: case PPC_OP_NEG: case PPC_OP_NEGO:
        snprintf(left, sizeof(left), "~r[%u]", in->rA);
        carry = "1";
        switch (in->op) {
        case PPC_OP_SUBFIC:
            snprintf(right, sizeof(right), "%s", immediate); break;
        case PPC_OP_NEG: case PPC_OP_NEGO:
            snprintf(right, sizeof(right), "0");
            record_carry = false; break;
        case PPC_OP_SUBFO:
            record_carry = false; break;
        case PPC_OP_SUBFE: case PPC_OP_SUBFEO:
            carry = "((xer >> 29) & 1u)"; break;
        case PPC_OP_SUBFME: case PPC_OP_SUBFMEO: case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO:
            snprintf(right, sizeof(right), "%s",
                     in->op == PPC_OP_SUBFME || in->op == PPC_OP_SUBFMEO ? "~0u" : "0");
            carry = "((xer >> 29) & 1u)"; break;
        default: break;
        }
        break;
    default:
        break;
    }
    fprintf(out, "    r[%u] = dr_native_add(%s, %s, %s, &xer, %u, %u);\n",
            in->rD, left, right, carry, record_carry, in->oe);
    emit_record(out, in->rc || in->op == PPC_OP_ADDIC_DOT, in->rD);
}

static void emit_system(FILE* out, const PPCInst* in, const char* operation, bool writing) {
    emit_clock_publish(out);
    fprintf(out, "    if (!cpu->system) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0x%08Xu);\n", in->address, in->raw);
    if (writing)
        fprintf(out, "    (void)cpu->system(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), %s, %uu, r[%u]);\n",
                in->address, operation, in->spr, in->rS);
    else
        fprintf(out, "    { u32 value = cpu->system(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), %s, %uu, 0);\n",
                in->address, operation, in->spr);
    emit_clock_reload(out);
    if (!writing)
        fprintf(out, "    r[%u] = value; }\n", in->rD);
}

static void emit_storage_effect(FILE* out, const PPCInst* in, const char* operation, bool indexed, const NativeRegisters* regs) {
    if (indexed)
        emit_address(out, in, MEM_INDEXED, NULL);
    const bool context = in->op == PPC_OP_SC;
    if (context) emit_publish(out,regs);
    else emit_clock_publish(out);
    fprintf(out, "    if (!cpu->system) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0x%08Xu);\n", in->address, in->raw);
    fprintf(out, "    (void)cpu->system(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), %s, 0, %s);\n", in->address, operation,
            indexed ? "ea" : "0");
    if (context) emit_reload(out,regs);
    else emit_clock_reload(out);
    if (indexed)
        fputs("    }\n", out);
}

#include "native_function_fp_emit.inc"

static void emit_body(FILE* out, const PPCInst* in, bool checked, const NativeRegisters* regs,
                      const DrNativeRelocation* relocation) {
    char immediate[64];
    if (relocation)
        snprintf(immediate, sizeof(immediate), "(u32)(s32)(s16)DR_NATIVE_OPERAND(%u)", relocation->operand);
    else
        snprintf(immediate, sizeof(immediate), "0x%08Xu", (u32)(s32)in->simm);
    if (native_fp_op(in->op) && emit_float(out,in,checked,immediate))
        return;
    const IntegerMemory memory = integer_memory[in->op];
    if (memory.bytes) {
        emit_memory(out, in, checked, memory, immediate);
        return;
    }
    switch (in->op) {
    case PPC_OP_ADDI: case PPC_OP_ADDIS: {
        fprintf(out, "    r[%u] = ", in->rD);
        if (in->rA)
            fprintf(out, "r[%u] + ", in->rA);
        if (in->op == PPC_OP_ADDIS)
            fprintf(out, "(%s << 16);\n", immediate);
        else
            fprintf(out, "%s;\n", immediate);
        return;
    }
    case PPC_OP_ADD: case PPC_OP_SUBF:
        if (in->op == PPC_OP_ADD)
            fprintf(out, "    r[%u] = r[%u] + r[%u];\n", in->rD, in->rA, in->rB);
        else
            fprintf(out, "    r[%u] = r[%u] - r[%u];\n", in->rD, in->rB, in->rA);
        emit_record(out, in->rc, in->rD);
        return;
    case PPC_OP_ADDIC: case PPC_OP_ADDIC_DOT: case PPC_OP_SUBFIC:
    case PPC_OP_ADDO: case PPC_OP_ADDC: case PPC_OP_ADDCO: case PPC_OP_ADDE: case PPC_OP_ADDEO:
    case PPC_OP_ADDME: case PPC_OP_ADDMEO: case PPC_OP_ADDZE: case PPC_OP_ADDZEO:
    case PPC_OP_SUBFO: case PPC_OP_SUBFC: case PPC_OP_SUBFCO: case PPC_OP_SUBFE: case PPC_OP_SUBFEO:
    case PPC_OP_SUBFME: case PPC_OP_SUBFMEO: case PPC_OP_SUBFZE: case PPC_OP_SUBFZEO:
    case PPC_OP_NEG: case PPC_OP_NEGO:
        emit_carry_arithmetic(out, in, immediate);
        return;
    case PPC_OP_OR: case PPC_OP_AND: case PPC_OP_XOR:
    case PPC_OP_NAND: case PPC_OP_NOR: case PPC_OP_EQV: case PPC_OP_ANDC: case PPC_OP_ORC: {
        static const char* const ops[PPC_OP_COUNT] = {
            [PPC_OP_OR] = "|", [PPC_OP_AND] = "&", [PPC_OP_XOR] = "^",
            [PPC_OP_NOR] = "|", [PPC_OP_NAND] = "&", [PPC_OP_EQV] = "^",
            [PPC_OP_ORC] = "| ~", [PPC_OP_ANDC] = "& ~",
        };
        const bool invert = in->op == PPC_OP_NAND || in->op == PPC_OP_NOR || in->op == PPC_OP_EQV;
        fprintf(out, "    r[%u] = %s(r[%u] %s r[%u]);\n", in->rA,
                invert ? "~" : "", in->rS, ops[in->op], in->rB);
        emit_record(out, in->rc, in->rA);
        return;
    }
    case PPC_OP_ANDI: case PPC_OP_ANDIS:
        fprintf(out, "    r[%u] = r[%u] & 0x%08Xu;\n", in->rA, in->rS,
                (u32)in->uimm << (in->op == PPC_OP_ANDIS ? 16 : 0));
        emit_record(out, true, in->rA);
        return;
    case PPC_OP_RLWINM: case PPC_OP_RLWNM: case PPC_OP_RLWIMI: {
        const u32 mask = rotate_mask(in->mb, in->me);
        fprintf(out, "    r[%u] = ", in->rA);
        if (in->op == PPC_OP_RLWIMI)
            fprintf(out, "(r[%u] & 0x%08Xu) | ", in->rA, ~mask);
        fprintf(out, "(dr_native_rotate_left(r[%u], ", in->rS);
        if (in->op == PPC_OP_RLWNM)
            fprintf(out, "r[%u]", in->rB);
        else
            fprintf(out, "%u", in->sh);
        fprintf(out, ") & 0x%08Xu);\n", mask);
        emit_record(out, in->rc, in->rA);
        return;
    }
    case PPC_OP_SLW: case PPC_OP_SRW:
        fprintf(out, "    r[%u] = (r[%u] & 32u) ? 0u : r[%u] %s (r[%u] & 31u);\n",
                in->rA, in->rB, in->rS, in->op == PPC_OP_SLW ? "<<" : ">>", in->rB);
        emit_record(out, in->rc, in->rA);
        return;
    case PPC_OP_SRAWI: case PPC_OP_SRAW:
        fprintf(out, "    r[%u] = dr_native_shift_signed(r[%u], ", in->rA, in->rS);
        if (in->op == PPC_OP_SRAWI)
            fprintf(out, "%u", in->sh);
        else
            fprintf(out, "r[%u]", in->rB);
        fputs(", &xer);\n", out);
        emit_record(out, in->rc, in->rA);
        return;
    case PPC_OP_CNTLZW: case PPC_OP_EXTSB: case PPC_OP_EXTSH: {
        const char* op = in->op == PPC_OP_CNTLZW ? "dr_native_clz"
                         : in->op == PPC_OP_EXTSB ? "(u32)(s32)(s8)" : "(u32)(s32)(s16)";
        fprintf(out, "    r[%u] = %s(r[%u]);\n", in->rA, op, in->rS);
        emit_record(out, in->rc, in->rA);
        return;
    }
    case PPC_OP_MULLI:
        fprintf(out, "    r[%u] = r[%u] * 0x%08Xu;\n", in->rD, in->rA, (u32)(s32)in->simm);
        return;
    case PPC_OP_MULLW: case PPC_OP_MULLWO:
        fprintf(out, "    { s64 product = (s64)(s32)r[%u] * (s32)r[%u];\n"
                     "    r[%u] = (u32)product;\n", in->rA, in->rB, in->rD);
        if (in->oe)
            fputs("    xer = (xer & ~0x40000000u) | ((product > INT32_MAX || product < INT32_MIN) ? 0xc0000000u : 0);\n", out);
        fputs("    }\n", out);
        emit_record(out, in->rc, in->rD);
        return;
    case PPC_OP_MULHW: case PPC_OP_MULHWU:
        fprintf(out, in->op == PPC_OP_MULHW ? "    r[%u] = (u32)((u64)((s64)(s32)r[%u] * (s32)r[%u]) >> 32);\n"
                                          : "    r[%u] = (u32)(((u64)r[%u] * r[%u]) >> 32);\n",
                in->rD, in->rA, in->rB);
        emit_record(out, in->rc, in->rD);
        return;
    case PPC_OP_DIVW: case PPC_OP_DIVWO: case PPC_OP_DIVWU: case PPC_OP_DIVWUO:
        fprintf(out, "    r[%u] = dr_native_divide(r[%u], r[%u], %u, %u, &xer);\n",
                in->rD, in->rA, in->rB, in->op == PPC_OP_DIVW || in->op == PPC_OP_DIVWO, in->oe);
        emit_record(out, in->rc, in->rD);
        return;
    case PPC_OP_ORI: case PPC_OP_ORIS: case PPC_OP_XORI: case PPC_OP_XORIS: {
        const bool upper = in->op == PPC_OP_ORIS || in->op == PPC_OP_XORIS;
        const bool xored = in->op == PPC_OP_XORI || in->op == PPC_OP_XORIS;
        fprintf(out, "    r[%u] = r[%u] %c 0x%08Xu;\n", in->rA, in->rS, xored ? '^' : '|',
                (u32)in->uimm << (upper ? 16 : 0));
        return;
    }
    case PPC_OP_CMPI: case PPC_OP_CMPLI: case PPC_OP_CMP: case PPC_OP_CMPL:
        emit_cmp(out, in, in->op == PPC_OP_CMPI || in->op == PPC_OP_CMPLI,
                 in->op == PPC_OP_CMPI || in->op == PPC_OP_CMP);
        return;
    case PPC_OP_CRAND: case PPC_OP_CRANDC: case PPC_OP_CREQV: case PPC_OP_CRNAND:
    case PPC_OP_CRNOR: case PPC_OP_CROR: case PPC_OP_CRORC: case PPC_OP_CRXOR: {
        static const char* const expressions[PPC_OP_COUNT] = {
            [PPC_OP_CRAND] = "a & b", [PPC_OP_CRANDC] = "a & ~b",
            [PPC_OP_CREQV] = "~(a ^ b)", [PPC_OP_CRNAND] = "~(a & b)",
            [PPC_OP_CRNOR] = "~(a | b)", [PPC_OP_CROR] = "a | b",
            [PPC_OP_CRORC] = "a | ~b", [PPC_OP_CRXOR] = "a ^ b",
        };
        const unsigned bit = 31u-in->rD;
        fprintf(out, "    { u32 a = cr >> %u, b = cr >> %u;\n"
                     "    cr = (cr & 0x%08Xu) | (((%s) & 1u) << %u); }\n",
                31u-in->rA, 31u-in->rB, ~(1u << bit), expressions[in->op], bit);
        return;
    }
    case PPC_OP_MCRF: case PPC_OP_MCRXR: {
        const bool from_xer = in->op == PPC_OP_MCRXR;
        const unsigned dst = 28u-4u*in->crfD;
        const unsigned src = from_xer ? 28u : 28u-4u*in->crfS;
        fprintf(out, "    cr = (cr & 0x%08Xu) | (((%s >> %u) & 15u) << %u);\n",
                ~(15u << dst), from_xer ? "xer" : "cr", src, dst);
        if (from_xer)
            fputs("    xer &= 0x1fffffffu;\n", out);
        return;
    }
    case PPC_OP_MFCR:
        fprintf(out, "    r[%u] = cr;\n", in->rD);
        return;
    case PPC_OP_MTCRF: {
        u32 mask = 0;
        for (unsigned field = 0; field < 8; ++field) {
            if (in->crm & (0x80u >> field))
                mask |= 15u << (28u-4u*field);
        }
        if (mask)
            fprintf(out, "    cr = (cr & 0x%08Xu) | (r[%u] & 0x%08Xu);\n",
                    ~mask, in->rS, mask);
        return;
    }
    case PPC_OP_MFSPR: case PPC_OP_MTSPR: {
        const char* reg = in->spr == 8 ? "lr" : in->spr == 9 ? "ctr" : in->spr == 1 ? "xer" : NULL;
        if (!reg) {
            const bool writing = in->op == PPC_OP_MTSPR;
            emit_system(out, in, writing ? "DR_NATIVE_SPR_WRITE" : "DR_NATIVE_SPR_READ", writing);
        } else if (in->op == PPC_OP_MFSPR)
            fprintf(out, "    r[%u] = %s;\n", in->rD, reg);
        else
            fprintf(out, "    %s = r[%u];\n", reg, in->rS);
        return;
    }
    case PPC_OP_MFTB:
        emit_system(out, in, "DR_NATIVE_TB_READ", false); return;
    case PPC_OP_MFMSR:
        emit_system(out, in, "DR_NATIVE_MSR_READ", false); return;
    case PPC_OP_MTMSR:
        emit_system(out, in, "DR_NATIVE_MSR_WRITE", true); return;
    case PPC_OP_SYNC: case PPC_OP_EIEIO: case PPC_OP_ISYNC: case PPC_OP_TLBSYNC:
        emit_storage_effect(out, in, "DR_NATIVE_FENCE", false, regs); return;
    case PPC_OP_DCBST:
        emit_storage_effect(out, in, "DR_NATIVE_DC_STORE", true, regs); return;
    case PPC_OP_DCBF:
        emit_storage_effect(out, in, "DR_NATIVE_DC_FLUSH", true, regs); return;
    case PPC_OP_DCBI:
        emit_storage_effect(out, in, "DR_NATIVE_DC_INVALIDATE", true, regs); return;
    case PPC_OP_ICBI:
        emit_storage_effect(out, in, "DR_NATIVE_IC_INVALIDATE", true, regs); return;
    case PPC_OP_DCBZ:
        emit_storage_effect(out, in, "DR_NATIVE_DC_ZERO", true, regs); return;
    case PPC_OP_DCBZ_L:
        emit_storage_effect(out, in, "DR_NATIVE_LC_ZERO", true, regs); return;
    case PPC_OP_DCBT: case PPC_OP_DCBTST:
        return; /* Native RAM has no observable prefetch state. */
    case PPC_OP_SC:
        emit_storage_effect(out, in, "DR_NATIVE_SYSTEM_CALL", false, regs); return;
    case PPC_OP_RFI:
        fputs("    {\n", out);
        emit_publish(out,regs);
        fprintf(out, "    if (!cpu->system) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0x%08Xu);\n"
                     "    u32 target = cpu->system(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), DR_NATIVE_RFI, 0, 0);\n",
                in->address,in->raw,in->address);
        emit_resume_check(out,regs);
        fprintf(out,"    if (target) dr_native_linked(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), target)(cpu, mem);\n",in->address);
        emit_resume_check(out,regs);
        fputs("    return; }\n",out);
        return;
    case PPC_OP_LMW: case PPC_OP_STMW:
        emit_multi_memory(out, in, checked, immediate);
        return;
    default:
        break;
    }
    emit_fault(out, in);
}

#include "native_function_stack_emit.inc"

static void emit_local_switch(FILE* out, u32 pc, const u32* targets, size_t count, const NativeRegisters* regs) {
    if (!count)
        return;
    fputs("    switch (DR_NATIVE_CANONICAL_PC(target)) {\n", out);
    for (size_t i = 0; i < count; ++i) {
        const u32 target = targets[i];
        fprintf(out, "    case 0x%08Xu:\n", target);
        if (target <= pc)
            emit_fuel_check(out, target, regs);
        fprintf(out, "        goto loc_%08X;\n", target);
    }
    fputs("    }\n", out);
}

static void emit_save_restore(FILE* out, const DrNativeSymbol* helper, bool checked) {
    const bool fp = helper->kind == DR_NATIVE_SAVE_FPR || helper->kind == DR_NATIVE_RESTORE_FPR;
    const bool save = helper->kind == DR_NATIVE_SAVE_GPR || helper->kind == DR_NATIVE_SAVE_FPR;
    const unsigned width = fp ? 8u : 4u;
    const unsigned count = 32u-helper->first_register;
    const PPCOpcode operation = fp ? (save ? PPC_OP_STFD : PPC_OP_LFD) :
                                    (save ? PPC_OP_STW : PPC_OP_LWZ);
    const u32 cost = count*dr_ppc_num_cycles(operation)+dr_ppc_num_cycles(PPC_OP_BCLR);
    fprintf(out, "    work += %uu;\n", cost);
    for (unsigned reg = helper->first_register; reg < 32; ++reg) {
        const unsigned distance = (32u-reg)*width;
        if (fp && save)
            fprintf(out, "    dr_native_store_double(cpu,mem,r[11]-%uu,f[%u].lane[0],%s);\n",
                    distance, reg, timing_arguments(checked));
        else if (fp)
            fprintf(out, "    f[%u].lane[0] = dr_native_load_double(cpu,mem,r[11]-%uu,%s);\n",
                    reg, distance, timing_arguments(checked));
        else if (save)
            fprintf(out, "    dr_native_memory_write(cpu,mem,r[11]-%uu,4,r[%u],%s);\n",
                    distance, reg, timing_arguments(checked));
        else
            fprintf(out, "    r[%u] = dr_native_memory_read(cpu,mem,r[11]-%uu,4,%s);\n",
                    reg, distance, timing_arguments(checked));
    }
}

static void emit_return(FILE* out,const PPCInst* in,DrNativeRelAction rel_action) {
    if(rel_action!=DR_NATIVE_REL_NO_TRANSITION) {
        fprintf(out,"    if (cpu->gpr[3]) {\n"
                    "        if (!cpu->rel_transition) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), module_header);\n"
                    "        cpu->rel_transition(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), module_header, %u);\n"
                    "    }\n",in->address,in->address,rel_action==DR_NATIVE_REL_LINK);
    }
    fputs("    return;\n",out);
}

static void emit_transfer(FILE* out, const PPCInst* in, DrNativeTransfer edge,
                          const DrNativeEmission* emission, const NativeRegisters* regs) {
    const bool conditional = in->op != PPC_OP_B && in->bo != 20u;
    const bool indirect = edge.kind == DR_NATIVE_BRANCH_INDIRECT || edge.kind == DR_NATIVE_CALL_INDIRECT;
    if (indirect)
        fprintf(out, "    { u32 target = %s & ~3u;\n", in->op == PPC_OP_BCLR ? "lr" : "ctr");
    if (in->lk)
        fprintf(out, "    lr = DR_NATIVE_GUEST_PC(0x%08Xu);\n", in->address + 4u);
    if (conditional)
        fprintf(out, "    if (dr_native_cond(cr, &ctr, %u, %u)) {\n", in->bo, in->bi);
    switch (edge.kind) {
    case DR_NATIVE_CAPTURE_PC:
        break;
    case DR_NATIVE_BRANCH_LOCAL:
        if (edge.local_back_edge)
            emit_fuel_check(out, edge.target, regs);
        fprintf(out, "    goto loc_%08X;\n", edge.target);
        break;
    case DR_NATIVE_BRANCH_FUNCTION: case DR_NATIVE_CALL_FUNCTION:
        if(in->address==emission->thread_save_site) {
            fputs("    { const u32 context = r[3];\n"
                  "    for (unsigned reg=14;reg<32;++reg) saved_gpr[reg-14]=r[reg];\n",out);
            if(regs->floating)
                fputs("    for (unsigned reg=14;reg<32;++reg) saved_fpr[reg-14]=f[reg];\n",out);
        }
        fprintf(out, "    { extern void dr_native_%08X(DrNativeCpu*, u8*);\n", edge.target);
        emit_publish(out,regs);
        if(in->address==emission->thp_call_site) {
            fprintf(out,"#ifdef GCNLLR_NATIVE_THP_VIDEO\n"
                        "    extern void dr_native_thp_video(DrNativeCpu*,u32);\n"
                        "    dr_native_thp_video(cpu,r[%u]);\n"
                        "#else\n",emission->thp_length_register);
            fprintf(out,"    dr_native_%08X(cpu, mem);\n#endif\n    }\n",edge.target);
        } else {
            fprintf(out, "    dr_native_%08X(cpu, mem); }\n", edge.target);
        }
        if(edge.kind==DR_NATIVE_CALL_FUNCTION || regs->saving_thread ||
           emission->rel_action!=DR_NATIVE_REL_NO_TRANSITION)
            emit_resume_check(out,regs);
        if (edge.kind == DR_NATIVE_BRANCH_FUNCTION)
            emit_return(out,in,emission->rel_action);
        else
            emit_reload(out,regs);
        if(in->address==emission->thread_save_site) {
            fprintf(out,"    if (!cpu->thread_save) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), context);\n"
                        "    cpu->thread_save(cpu,context,DR_NATIVE_GUEST_PC(0x%08Xu),&saved_context,r);\n"
                        "    }\n",in->address,in->address+4u);
        }
        break;
    case DR_NATIVE_RETURN:
        emit_publish(out,regs);
        emit_return(out,in,emission->rel_action);
        break;
    case DR_NATIVE_SAVE_RESTORE:
        emit_save_restore(out, edge.symbol, emission->checked_memory);
        if (!in->lk) {
            emit_publish(out,regs);
            emit_return(out,in,emission->rel_action);
        }
        break;
    case DR_NATIVE_BRANCH_INDIRECT: case DR_NATIVE_CALL_INDIRECT:
        if (edge.kind == DR_NATIVE_BRANCH_INDIRECT)
            emit_local_switch(out,in->address,emission->local_targets,emission->target_count,regs);
        emit_publish(out,regs);
        fprintf(out, "    dr_native_linked(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), target)(cpu, mem);\n", in->address);
        if(edge.kind==DR_NATIVE_CALL_INDIRECT || regs->saving_thread ||
           emission->rel_action!=DR_NATIVE_REL_NO_TRANSITION)
            emit_resume_check(out,regs);
        if (edge.kind == DR_NATIVE_BRANCH_INDIRECT)
            emit_return(out,in,emission->rel_action);
        else
            emit_reload(out,regs);
        break;
    default:
        emit_fault(out, in);
        break;
    }
    if (conditional)
        fputs("    }\n", out);
    if (indirect)
        fputs("    }\n", out);
}

static void emit_relocated_branch(FILE* out,const PPCInst* in,const DrNativeRelocation* relocation,
                                  const NativeRegisters* regs,bool checked) {
    fprintf(out, "    { const u32 site = DR_NATIVE_GUEST_PC(0x%08Xu);\n"
                 "    const u32 displacement = DR_NATIVE_OPERAND(%u) & 0x03fffffcu;\n"
                 "    const u32 target = site + ((displacement ^ 0x02000000u) - 0x02000000u);\n",
            in->address, relocation->operand);
    if (in->lk)
        fputs("    lr = site + 4u;\n", out);
    if(relocation->helper) {
        fprintf(out,"    if (target != 0x%08Xu) dr_native_fault(cpu, site, target);\n",
                relocation->helper->address);
        emit_save_restore(out,relocation->helper,checked);
        fputs("    }\n",out);
        if(!in->lk) {
            emit_publish(out,regs);
            fputs("    return;\n",out);
        }
        return;
    }
    emit_publish(out,regs);
    fputs("    dr_native_linked(cpu, site, target)(cpu, mem); }\n", out);
    if(in->lk || regs->saving_thread)emit_resume_check(out,regs);
    if (in->lk)
        emit_reload(out,regs);
    else
        fputs("    return;\n", out);
}

static u32 native_register(unsigned reg) {
    return UINT32_C(1) << reg;
}

static void include_instruction_registers(NativeRegisters* regs, const PPCInst* in) {
    /* Decoded register operands are five-bit indices. For arithmetic, keeping
     * unused operand fields is conservative. Memory addresses are GPRs. */
    if (native_fp_op(in->op)) {
        regs->floating = true;
        if (fp_memory[in->op]) {
            if (in->rA || (fp_memory[in->op] & MEM_UPDATE))
                regs->gpr |= native_register(in->rA);
            if (fp_memory[in->op] & MEM_INDEXED)
                regs->gpr |= native_register(in->rB);
            regs->fpr |= native_register((fp_memory[in->op] & MEM_STORE) ? in->rS : in->rD);
        } else {
            regs->fpr |= native_register(in->rD) | native_register(in->rS) |
                         native_register(in->rA) | native_register(in->rB) |
                         native_register(in->rC);
        }
    } else if (in->op == PPC_OP_LMW || in->op == PPC_OP_STMW) {
        regs->gpr |= native_register(in->rA) |
                     (UINT32_MAX << (in->op == PPC_OP_LMW ? in->rD : in->rS));
    } else {
        regs->gpr |= native_register(in->rD) | native_register(in->rS) |
                     native_register(in->rA) | native_register(in->rB);
    }
}

static void include_spill_registers(NativeRegisters* regs, const DrNativeSymbol* helper) {
    regs->gpr |= native_register(11);
    const u32 spill = UINT32_MAX << helper->first_register;
    if (helper->kind == DR_NATIVE_SAVE_FPR || helper->kind == DR_NATIVE_RESTORE_FPR) {
        regs->floating = true;
        regs->fpr |= spill;
    } else {
        regs->gpr |= spill;
    }
}
#include "native_function_register_flow.inc"

bool dr_emit_native_function(FILE* out, const DrNativeSymbols* symbols,
                             const DrNativeSymbol* function, const PPCInst* instructions,
                             const DrNativeEmission* emission) {
    const u32 count = function->size / 4u;
    if (function->kind != DR_NATIVE_FUNCTION || count == 0 || !emission)
        return false;
    const u32* local_targets=emission->local_targets;
    const size_t target_count=emission->target_count;
    const DrNativeRelocation* relocations=emission->relocations;
    const size_t relocation_count=emission->relocation_count;
    const bool checked_memory=emission->checked_memory;
    for(size_t i=0;i<relocation_count;++i) {
        const DrNativeRelocation* relocation=relocations+i;
        const u32 offset=relocation->address-function->address;
        if((offset&3u) || offset>=function->size ||
           (i && relocations[i-1].address>=relocation->address))return false;
        const PPCInst* in=instructions+offset/4u;
        const u32 primary=in->raw>>26;
        switch(relocation->kind) {
        case DR_NATIVE_ADDR16_LO:
            if(primary!=13u && primary!=14u && (primary<32u || primary>55u))return false;
            break;
        case DR_NATIVE_ADDR16_HA:
            if(in->op!=PPC_OP_ADDIS)return false;
            break;
        case DR_NATIVE_REL24:
            if(in->op!=PPC_OP_B || (in->raw&2u))return false;
            if(relocation->helper && relocation->helper->kind<DR_NATIVE_SAVE_GPR)return false;
            break;
        default:return false;
        }
    }
    enum { BLOCK_START = 1, BRANCH_TARGET = 2 };
    u8* leaders = calloc(count, sizeof(*leaders));
    if (!leaders)
        return false;
    leaders[0] = BLOCK_START;
    for (size_t i = 0; i < target_count; ++i) {
        const u32 offset = local_targets[i] - function->address;
        if ((offset & 3u) || offset >= function->size ||
            (i && local_targets[i-1] >= local_targets[i])) {
            free(leaders);
            return false;
        }
        leaders[offset/4u] |= BLOCK_START | BRANCH_TARGET;
    }
    NativeRegisters registers={0};
    const NativeRegisters* regs=&registers;
    size_t next_relocation=0;
    for (u32 i = 0; i < count; ++i) {
        const PPCInst* in = &instructions[i];
        include_instruction_registers(&registers,in);
        if (in->address != function->address + i*4u || in->embedded_data) {
            free(leaders);
            return false;
        }
        if(next_relocation<relocation_count && relocations[next_relocation].address==in->address) {
            const DrNativeRelocation* relocation=relocations+next_relocation++;
            if(relocation->kind==DR_NATIVE_REL24) {
                if(relocation->helper)include_spill_registers(&registers,relocation->helper);
                if(i+1<count)leaders[i+1]|=BLOCK_START;
                continue;
            }
        }
        const DrNativeTransfer edge = dr_native_transfer(symbols, function, in);
        if (edge.kind == DR_NATIVE_SAVE_RESTORE)
            include_spill_registers(&registers,edge.symbol);
        if (edge.kind == DR_NATIVE_BRANCH_LOCAL)
            leaders[(edge.target - function->address)/4u] |= BLOCK_START | BRANCH_TARGET;
        if (edge.kind != DR_NATIVE_STRAIGHT_LINE && i+1 < count)
            leaders[i+1] |= BLOCK_START;
    }
    const bool saving_thread=emission->thread_save_site>=function->address &&
        emission->thread_save_site-function->address<function->size;
    if (saving_thread) {
        const u32 resume_index=(emission->thread_save_site-function->address)/4u+1u;
        if(resume_index>=count) {
            free(leaders);
            return false;
        }
        leaders[resume_index]|=BLOCK_START|BRANCH_TARGET;
        registers.saving_thread=true;
        registers.gpr=UINT32_MAX;
        if (registers.floating)registers.fpr=UINT32_MAX;
    }
    if (emission->rel_action!=DR_NATIVE_REL_NO_TRANSITION)registers.gpr |= native_register(3);
    if (emission->timebase_wait)registers.gpr |= native_register(emission->wait_target_register);
    registers.gpr &= UINT32_MAX >> (32u - sizeof(((DrNativeCpu*)0)->gpr)/sizeof(u32));
    registers.fpr &= UINT32_MAX >> (32u - sizeof(((DrNativeCpu*)0)->fpr)/sizeof(DrNativeFloat));
    NativeRegisterFlow* flow=saving_thread ? NULL :
        native_register_flow(symbols,function,instructions,count,emission);
    if(!saving_thread && !flow) {
        free(leaders);
        return false;
    }
    if(emission->observe_scope) {
        fprintf(out,"static void dr_native_body_%08X(DrNativeCpu*,u8*);\n"
                    "void dr_native_%08X(DrNativeCpu* cpu,u8* mem) {\n"
                    "    if(cpu->observe_entry)cpu->observe_entry(cpu,DR_NATIVE_GUEST_PC(0x%08Xu));\n"
                    "    dr_native_body_%08X(cpu,mem);\n"
                    "    if (cpu->resume) return;\n"
                    "    if(cpu->observe_entry)cpu->observe_entry(cpu,DR_NATIVE_GUEST_PC(0x%08Xu)|1u);\n"
                    "}\n"
                    "static void dr_native_body_%08X(DrNativeCpu* cpu,u8* mem) {\n",
                function->address,function->address,function->address,function->address,
                function->address,function->address);
    } else {
        fprintf(out,"void dr_native_%08X(DrNativeCpu* cpu, u8* mem) {\n",function->address);
    }
    fputs("    (void)mem;\n    u32 r[32] = {0}, cr, lr, ctr, xer;\n"
          "    u64 work;\n", out);
    if (regs->floating)
        fputs("    DrNativeFloat f[32]={0};\n    u32 fpscr;\n",out);
    if(saving_thread) {
        fputs("    unsigned char saved_context;\n    u32 saved_gpr[18];\n",out);
        if(regs->floating)fputs("    DrNativeFloat saved_fpr[18];\n",out);
    }
    emit_reload(out,regs);
    emit_fuel_check(out, function->address, regs);
    if(emission->rel_action!=DR_NATIVE_REL_NO_TRANSITION)
        fputs("    const u32 module_header = r[3];\n",out);
    if (emission->observe_entry) {
        fprintf(out, "    if (cpu->observe_entry) {\n"
                     "    cpu->observe_entry(cpu, DR_NATIVE_GUEST_PC(0x%08Xu));\n", function->address);
        emit_reload(out,regs);
        fputs("    }\n", out);
    }
    next_relocation=0;
    for (u32 i = 0; i < count; ++i) {
        const PPCInst* in = &instructions[i];
        registers.dirty=flow && flow[i].reached ? flow[i].incoming|flow[i].writes : UINT64_MAX;
        if (leaders[i]) {
            if (leaders[i] & BRANCH_TARGET)
                fprintf(out, "loc_%08X: ;\n", in->address);
            u32 cost = 0;
            for (u32 j = i; j < count && (j == i || !leaders[j]); ++j)
                cost += dr_ppc_num_cycles(instructions[j].op);
            fprintf(out, "    work += %uu;\n", cost);
        }
        if(emission->timebase_wait && emission->timebase_wait==in->address) {
            emit_publish(out,regs);
            fprintf(out,"    if (!cpu->system) dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0);\n"
                        "    (void)cpu->system(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), DR_NATIVE_WAIT_TB_LOW, 0, r[%u]);\n",
                    in->address,in->address,emission->wait_target_register);
            emit_resume_check(out,regs);
            emit_reload(out,regs);
        }
        const u32 relocation_pc=next_relocation<relocation_count ?
            relocations[next_relocation].address : UINT32_MAX;
        if(checked_memory) {
            const u32 after=emit_stack_span(out,instructions,leaders,i,count,
                relocation_pc,emission->timebase_wait,regs);
            if(after!=i) {
                i=after-1u;
                continue;
            }
        }
        const DrNativeRelocation* relocation=NULL;
        if(next_relocation<relocation_count && relocation_pc==in->address) {
            relocation=relocations+next_relocation++;
            if(relocation->kind==DR_NATIVE_REL24) {
                emit_relocated_branch(out,in,relocation,regs,checked_memory);
                continue;
            }
        }
        const DrNativeTransfer edge = dr_native_transfer(symbols, function, in);
        if (edge.kind == DR_NATIVE_STRAIGHT_LINE)
            emit_body(out, in, checked_memory, regs, relocation);
        else
            emit_transfer(out,in,edge,emission,regs);
    }
    fprintf(out, "    dr_native_fault(cpu, DR_NATIVE_GUEST_PC(0x%08Xu), 0);\n",
             function->address + function->size);
    if (saving_thread) {
        fputs("dr_native_resume_thread: ;\n"
              "    cpu->resume = NULL;\n"
              "    if (cpu->resume_landed) cpu->resume_landed(cpu);\n",out);
        emit_reload(out,regs);
        fputs("    for (unsigned reg=14;reg<32;++reg) r[reg]=saved_gpr[reg-14];\n",out);
        if(regs->floating)
            fputs("    for (unsigned reg=14;reg<32;++reg) f[reg]=saved_fpr[reg-14];\n",out);
        fprintf(out,"    r[3]=1;\n    goto loc_%08X;\n",emission->thread_save_site+4u);
    }
    fputs("}\n",out);
    free(flow);
    free(leaders);
    return !ferror(out);
}

bool dr_emit_native_function_bytes(FILE* out,const DrNativeSymbols* symbols,
                                   const DrNativeSymbol* function,const u8* code,
                                   const DrNativeEmission* emission) {
    const size_t count=function->size/4u;
    PPCInst* instructions=malloc(count*sizeof(*instructions));
    if(!instructions)return false;
    for(size_t i=0;i<count;++i) {
        const u8* p=code+i*4u;
        const u32 word=((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
        instructions[i]=ppc_decode(word,function->address+(u32)(i*4u));
    }
    const bool emitted=dr_emit_native_function(out,symbols,function,instructions,emission);
    free(instructions);
    return emitted;
}

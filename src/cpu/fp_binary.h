/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared scalar/paired binary FP implementation, extracted without arithmetic
 * changes from cpu.c. Include after the host's CPUState declaration.
 * Generated-code users must disable FP contraction and fast-math; the CMake
 * dr_fp_inline target supplies the required flags on qualified compilers.
 * The ordinary CPU ABI uses this same implementation out of line.
 */
#ifndef DOLRECOMP_FP_BINARY_H
#define DOLRECOMP_FP_BINARY_H
#include <math.h>
#include <string.h>
#if (defined(DOLRECOMP_INLINE_FP) && DOLRECOMP_INLINE_FP) || defined(DOLRECOMP_FP_IMPLEMENTATION)
#if defined(DOLRECOMP_INLINE_FP) && DOLRECOMP_INLINE_FP
#if defined(__FAST_MATH__)
#error "Inline PPC floating point requires strict floating-point compilation"
#endif
#if defined(__GNUC__) || defined(__clang__)
#define DR_FP_PUBLIC static inline __attribute__((always_inline))
#define DR_FP_PRIVATE static inline __attribute__((always_inline))
#else
#define DR_FP_PUBLIC static inline
#define DR_FP_PRIVATE static inline
#endif
#define ppc_fpscr_updated dr_fp_inline_ppc_fpscr_updated
#define ppc_fadds dr_fp_inline_ppc_fadds
#define ppc_fsubs dr_fp_inline_ppc_fsubs
#define ppc_fmuls dr_fp_inline_ppc_fmuls
#define ppc_fdivs dr_fp_inline_ppc_fdivs
#define ppc_fadd dr_fp_inline_ppc_fadd
#define ppc_fsub dr_fp_inline_ppc_fsub
#define ppc_fmul dr_fp_inline_ppc_fmul
#define ppc_fdiv dr_fp_inline_ppc_fdiv
#define ppc_frsp dr_fp_inline_ppc_frsp
#define ppc_ps_add_op dr_fp_inline_ppc_ps_add_op
#define ppc_ps_sub_op dr_fp_inline_ppc_ps_sub_op
#define ppc_ps_mul_op dr_fp_inline_ppc_ps_mul_op
#define ppc_ps_div_op dr_fp_inline_ppc_ps_div_op
#else
#define DR_FP_PUBLIC
#define DR_FP_PRIVATE static
#endif
DR_FP_PRIVATE f32 dr_fp_f32_value(u32 bits) {
    f32 result;memcpy(&result,&bits,sizeof result);return result;
}

DR_FP_PRIVATE u64 dr_fp_f64_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

DR_FP_PRIVATE f64 dr_fp_f64_value(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}


DR_FP_PUBLIC void ppc_fpscr_updated(CPUState* cpu) {
    const u32 vx_any = 0x01F80700u;
    const u32 any_e = 0x000000F8u;
    u32 fpscr = cpu->fpscr;
    fpscr = (fpscr & ~0x20000000u) | ((fpscr & vx_any) ? 0x20000000u : 0u);
    fpscr = (fpscr & ~0x40000000u) |
            ((((fpscr >> 22) & fpscr & any_e) != 0) ? 0x40000000u : 0u);
    cpu->fpscr = fpscr;
}

DR_FP_PRIVATE void dr_fp_set_fp_exception(CPUState* cpu, u32 bit) {
    if ((cpu->fpscr & bit) != bit)
        cpu->fpscr |= 0x80000000u;
    cpu->fpscr |= bit;
    ppc_fpscr_updated(cpu);
}

DR_FP_PRIVATE bool dr_fp_is_snan(f64 value) {
    u64 bits = dr_fp_f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           fraction != 0 && (fraction & 0x0008000000000000ull) == 0;
}

DR_FP_PRIVATE u32 dr_fp_classify_f64(f64 value) {
    u64 bits = dr_fp_f64_bits(value);
    u64 sign = bits >> 63;
    u64 exponent = bits & 0x7FF0000000000000ull;
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

DR_FP_PRIVATE u32 dr_fp_classify_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 sign = bits >> 31;
    u32 exponent = bits & 0x7F800000u;
    u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

DR_FP_PRIVATE void dr_fp_set_fprf(CPUState* cpu, u32 value) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12);
}


DR_FP_PRIVATE unsigned dr_fp_leading_zeroes_u64(u64 value) {
    unsigned count = 0;
    while ((value & 0x8000000000000000ull) == 0) {
        value <<= 1;
        count++;
    }
    return count;
}

DR_FP_PRIVATE f64 dr_fp_force_25_bit(f64 value) {
    u64 bits = dr_fp_f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    u64 keep_mask = 0xFFFFFFFFF8000000ull;
    u64 round = 0x0000000008000000ull;

    if ((bits & 0x7FF0000000000000ull) == 0 && fraction != 0) {
        unsigned shift = dr_fp_leading_zeroes_u64(fraction) - 11;
        if (shift < 28) {
            keep_mask = ~((1ull << (27 - shift)) - 1);
            round >>= shift;
        } else {
            keep_mask = ~0ull;
            round = 0;
        }
    }

    bits = (bits & keep_mask) + (bits & round);
    return dr_fp_f64_value(bits);
}


enum {
    DR_FP_FPSCR_ZX = 0x04000000u,
    DR_FP_FPSCR_XX = 0x02000000u,
    DR_FP_FPSCR_VXSNAN = 0x01000000u,
    DR_FP_FPSCR_VXISI = 0x00800000u,
    DR_FP_FPSCR_VXIDI = 0x00400000u,
    DR_FP_FPSCR_VXZDZ = 0x00200000u,
    DR_FP_FPSCR_VXIMZ = 0x00100000u,
    DR_FP_FPSCR_FR = 0x00040000u,
    DR_FP_FPSCR_FI = 0x00020000u,
    DR_FP_FPSCR_VE = 0x00000080u,
    DR_FP_FPSCR_ZE = 0x00000010u,
    DR_FP_FPSCR_NI = 0x00000004u,
};

typedef struct {
    f64 value;
    u32 exception;
} DrFpBinaryResult;

typedef enum {
    DR_FP_ADD,
    DR_FP_SUB,
    DR_FP_MUL,
    DR_FP_DIV,
} DrFpBinaryOp;

DR_FP_PRIVATE f64 dr_fp_quiet_nan(f64 value) {
    return dr_fp_f64_value(dr_fp_f64_bits(value) | 0x0008000000000000ull);
}

DR_FP_PRIVATE f32 dr_fp_force_single(CPUState* cpu, f64 value) {
    if (cpu->fpscr & DR_FP_FPSCR_NI) {
        u64 bits = dr_fp_f64_bits(value);
        if ((bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull)
            return dr_fp_f32_value((u32)(bits >> 32) & 0x80000000u);
    }

    f32 result = (f32)value;
    if ((cpu->fpscr & DR_FP_FPSCR_NI) && fpclassify(result) == FP_SUBNORMAL)
        return copysignf(0.0f, result);
    return result;
}

DR_FP_PRIVATE f64 dr_fp_force_double(CPUState* cpu, f64 value) {
    if ((cpu->fpscr & DR_FP_FPSCR_NI) && fpclassify(value) == FP_SUBNORMAL)
        return copysign(0.0, value);
    return value;
}

DR_FP_PRIVATE void dr_fp_clear_fi_fr(CPUState* cpu) {
    cpu->fpscr &= ~(DR_FP_FPSCR_FI | DR_FP_FPSCR_FR);
}

DR_FP_PRIVATE void dr_fp_set_fi_fr(CPUState* cpu, f64 exact, f64 rounded) {
    dr_fp_clear_fi_fr(cpu);
    if (isnan(exact) || exact == rounded)
        return;
    dr_fp_set_fp_exception(cpu, DR_FP_FPSCR_XX);
    cpu->fpscr |= DR_FP_FPSCR_FI;
    if (fabs(rounded) > fabs(exact))
        cpu->fpscr |= DR_FP_FPSCR_FR;
}

DR_FP_PRIVATE DrFpBinaryResult dr_fp_binary_fp(CPUState* cpu, DrFpBinaryOp op, f64 a, f64 b) {
    DrFpBinaryResult result = {0};
    switch (op) {
    case DR_FP_ADD: result.value = a + b; break;
    case DR_FP_SUB: result.value = a - b; break;
    case DR_FP_MUL: result.value = a * b; break;
    case DR_FP_DIV: result.value = a / b; break;
    }

    if (op == DR_FP_DIV && isinf(result.value) && b == 0.0) {
        result.exception = DR_FP_FPSCR_ZX;
    } else if (isnan(result.value)) {
        if (dr_fp_is_snan(a) || dr_fp_is_snan(b))
            result.exception = DR_FP_FPSCR_VXSNAN;
        dr_fp_clear_fi_fr(cpu);

        if (isnan(a))
            result.value = dr_fp_quiet_nan(a);
        else if (isnan(b))
            result.value = dr_fp_quiet_nan(b);
        else if (op == DR_FP_MUL) {
            result.value = dr_fp_f64_value(0x7FF8000000000000ull);
            result.exception = DR_FP_FPSCR_VXIMZ;
        } else if (op == DR_FP_DIV) {
            result.value = dr_fp_f64_value(0x7FF8000000000000ull);
            result.exception = b == 0.0 ? DR_FP_FPSCR_VXZDZ : DR_FP_FPSCR_VXIDI;
        } else {
            result.value = dr_fp_f64_value(0x7FF8000000000000ull);
            result.exception = DR_FP_FPSCR_VXISI;
        }
    } else if ((op == DR_FP_ADD || op == DR_FP_SUB) && (isinf(a) || isinf(b))) {
        dr_fp_clear_fi_fr(cpu);
    }

    if (result.exception)
        dr_fp_set_fp_exception(cpu, result.exception);
    return result;
}

DR_FP_PRIVATE bool dr_fp_binary_result_enabled(const CPUState* cpu, DrFpBinaryResult result) {
    const u32 invalid = DR_FP_FPSCR_VXSNAN | DR_FP_FPSCR_VXISI | DR_FP_FPSCR_VXIDI |
                        DR_FP_FPSCR_VXZDZ | DR_FP_FPSCR_VXIMZ;
    if ((result.exception & invalid) && (cpu->fpscr & DR_FP_FPSCR_VE))
        return false;
    if (result.exception == DR_FP_FPSCR_ZX && (cpu->fpscr & DR_FP_FPSCR_ZE))
        return false;
    return true;
}

DR_FP_PRIVATE void dr_fp_write_single_result(CPUState* cpu, u8 d, f32 value) {
    cpu->fpr[d] = (f64)value;
    cpu->ps1[d] = (f64)value;
    dr_fp_set_fprf(cpu, dr_fp_classify_f32(value));
}

DR_FP_PRIVATE void dr_fp_binary_single(CPUState* cpu, u8 d, f64 a, f64 b, DrFpBinaryOp op) {
    DrFpBinaryResult result = dr_fp_binary_fp(cpu, op, a, b);
    if (!dr_fp_binary_result_enabled(cpu, result))
        return;
    f32 rounded = dr_fp_force_single(cpu, result.value);
    if (isfinite(result.value))
        dr_fp_set_fi_fr(cpu, result.value, (f64)rounded);
    dr_fp_write_single_result(cpu, d, rounded);
}

DR_FP_PRIVATE void dr_fp_binary_double(CPUState* cpu, u8 d, f64 a, f64 b, DrFpBinaryOp op) {
    DrFpBinaryResult result = dr_fp_binary_fp(cpu, op, a, b);
    if (!dr_fp_binary_result_enabled(cpu, result))
        return;
    cpu->fpr[d] = dr_fp_force_double(cpu, result.value);
    if (op == DR_FP_MUL)
        dr_fp_clear_fi_fr(cpu);
    dr_fp_set_fprf(cpu, dr_fp_classify_f64(cpu->fpr[d]));
}

DR_FP_PUBLIC void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_single(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_ADD);
}

DR_FP_PUBLIC void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_single(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_SUB);
}

DR_FP_PUBLIC void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    dr_fp_binary_single(cpu, d, cpu->fpr[a], dr_fp_force_25_bit(cpu->fpr[c]), DR_FP_MUL);
}

DR_FP_PUBLIC void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_single(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_DIV);
}

DR_FP_PUBLIC void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_double(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_ADD);
}

DR_FP_PUBLIC void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_double(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_SUB);
}

DR_FP_PUBLIC void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    dr_fp_binary_double(cpu, d, cpu->fpr[a], cpu->fpr[c], DR_FP_MUL);
}

DR_FP_PUBLIC void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_binary_double(cpu, d, cpu->fpr[a], cpu->fpr[b], DR_FP_DIV);
}

DR_FP_PUBLIC void ppc_frsp(CPUState* cpu, u8 d, u8 b) {
    f64 value = cpu->fpr[b];
    f32 rounded = dr_fp_force_single(cpu, value);
    if (isnan(value)) {
        bool snan = dr_fp_is_snan(value);
        if (snan)
            dr_fp_set_fp_exception(cpu, DR_FP_FPSCR_VXSNAN);
        dr_fp_clear_fi_fr(cpu);
        if (snan && (cpu->fpscr & DR_FP_FPSCR_VE))
            return;
    } else {
        dr_fp_set_fi_fr(cpu, value, (f64)rounded);
    }
    dr_fp_write_single_result(cpu, d, rounded);
}

/* Paired-single binary arithmetic. Previously emitted inline per lane as
 * dolrecomp_ps_round((f32)a OP (f32)b), which dropped FPRF entirely and -- for
 * ps_mul -- skipped the Gekko's 25-bit truncation of the C operand on BOTH
 * lanes. Ported from upstream DolRecomp 44f3fdb; paired-single is the SIMD unit
 * the vertex and matrix maths run on, so these lanes are load-bearing for
 * geometry. The scalar single-precision helpers above are what stop ps1 going
 * stale in the first place; these keep the paired ops themselves exact. */
DR_FP_PRIVATE void dr_fp_write_paired_result(CPUState* cpu, u8 d, f64 ps0, f64 ps1) {
    cpu->fpr[d] = (f64)(f32)ps0;
    cpu->ps1[d] = (f64)(f32)ps1;
    dr_fp_set_fprf(cpu, dr_fp_classify_f32((f32)ps0));
}

DR_FP_PRIVATE f64 dr_fp_paired_binary_nan(f64 a, f64 b, f64 result) {
    if (isnan(a))
        return dr_fp_quiet_nan(a);
    if (isnan(b))
        return dr_fp_quiet_nan(b);
    if (isnan(result))
        return dr_fp_f64_value(0x7FF8000000000000ull);
    return result;
}

DR_FP_PUBLIC void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_write_paired_result(cpu, d,
                        dr_fp_paired_binary_nan(cpu->fpr[a], cpu->fpr[b],
                                          cpu->fpr[a] + cpu->fpr[b]),
                        dr_fp_paired_binary_nan(cpu->ps1[a], cpu->ps1[b],
                                          cpu->ps1[a] + cpu->ps1[b]));
}

DR_FP_PUBLIC void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_write_paired_result(cpu, d,
                        dr_fp_paired_binary_nan(cpu->fpr[a], cpu->fpr[b],
                                          cpu->fpr[a] - cpu->fpr[b]),
                        dr_fp_paired_binary_nan(cpu->ps1[a], cpu->ps1[b],
                                          cpu->ps1[a] - cpu->ps1[b]));
}

DR_FP_PUBLIC void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c0 = dr_fp_force_25_bit(cpu->fpr[c]);
    f64 c1 = dr_fp_force_25_bit(cpu->ps1[c]);
    dr_fp_write_paired_result(cpu, d,
                        dr_fp_paired_binary_nan(cpu->fpr[a], cpu->fpr[c],
                                          cpu->fpr[a] * c0),
                        dr_fp_paired_binary_nan(cpu->ps1[a], cpu->ps1[c],
                                          cpu->ps1[a] * c1));
}

DR_FP_PUBLIC void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    dr_fp_write_paired_result(cpu, d,
                        dr_fp_paired_binary_nan(cpu->fpr[a], cpu->fpr[b],
                                          cpu->fpr[a] / cpu->fpr[b]),
                        dr_fp_paired_binary_nan(cpu->ps1[a], cpu->ps1[b],
                                          cpu->ps1[a] / cpu->ps1[b]));
}

#undef DR_FP_PUBLIC
#undef DR_FP_PRIVATE
#endif
#endif

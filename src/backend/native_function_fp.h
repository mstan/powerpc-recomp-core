/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_FUNCTION_FP_H
#define DOLRECOMP_NATIVE_FUNCTION_FP_H

#include "backend/native_function_abi.h"
#include <math.h>
#include <string.h>

static inline f32 dr_native_float32(u32 bits) {
    f32 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline f64 dr_native_float64(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline u32 dr_native_float32_bits(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline u64 dr_native_float64_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline f64 dr_native_load_double(DrNativeCpu* cpu, const u8* mem, u32 ea, bool checked, u64 work) {
    /* Observation can occur between the two guest words. */
    const u64 upper=dr_native_memory_read(cpu,mem,ea,4,checked,work);
    const u64 lower=dr_native_memory_read(cpu,mem,ea+4,4,checked,work);
    return dr_native_float64((upper<<32)|lower);
}

static inline void dr_native_store_double(DrNativeCpu* cpu, u8* mem, u32 ea,
                                          f64 value, bool checked, u64 work) {
    const u64 bits=dr_native_float64_bits(value);
    dr_native_memory_write(cpu,mem,ea,4,(u32)(bits>>32),checked,work);
    dr_native_memory_write(cpu,mem,ea+4,4,(u32)bits,checked,work);
}

static inline unsigned dr_native_quant_bytes(DrNativeCpu* cpu, u32 pc, u32 gqr, u32 ea) {
    const unsigned type=gqr&7u;
    if (!type && !(ea&3u))
        return 4;
    if (type>=4)
        return (type&1u) ? 2 : 1;
    dr_native_fault(cpu,pc,ea);
}

static inline int dr_native_quant_scale(u32 half) {
    return (int)(((half>>8)&63u)^32u)-32;
}

static inline f64 dr_native_quant_read_value(DrNativeCpu* cpu, const u8* mem, u32 ea,
                                             unsigned bytes, u32 gqr, bool checked, u64 work) {
    const u32 raw=dr_native_memory_read(cpu,mem,ea,bytes,checked,work);
    const unsigned kind=gqr&7u;
    if (!kind)
        return dr_native_float32(raw);
    const f64 value=kind==6 ? (s8)raw : kind==7 ? (s16)raw : (s32)raw;
    return (f32)ldexp(value,-dr_native_quant_scale(gqr));
}

static inline DrNativeFloat dr_native_quant_load(DrNativeCpu* cpu, const u8* mem,
                                                 u32 pc, u32 ea, u32 gqr, bool one, bool checked, u64 work) {
    const u32 half=gqr>>16;
    const unsigned bytes=dr_native_quant_bytes(cpu,pc,half,ea);
    const f64 first=dr_native_quant_read_value(cpu,mem,ea,bytes,half,checked,work);
    const f64 second=one ? 1.0 : dr_native_quant_read_value(cpu,mem,ea+bytes,bytes,half,checked,work);
    return (DrNativeFloat){{first,second}};
}

static inline u32 dr_native_quant_encode(f64 value, unsigned bytes, u32 half) {
    const unsigned type=half&7u;
    if (!type) {
        const u32 bits=dr_native_float32_bits((f32)value);
        const u32 magnitude=bits&0x7fffffffu;
        return magnitude && magnitude<0x00800000u ? 0 : bits;
    }
    const int range=1<<(bytes*8u-(type>=6));
    const int low=type>=6 ? -range : 0;
    const int high=range-1;
    const f64 scaled=ldexp(value,dr_native_quant_scale(half));
    if (isnan(scaled) || scaled>=high)
        return (u32)high;
    if (scaled<=low)
        return (u32)low;
    return (u32)(s32)scaled;
}

static inline void dr_native_quant_store(DrNativeCpu* cpu, u8* mem, u32 pc, u32 ea,
                                         u32 gqr, bool one, bool checked, u64 work, DrNativeFloat source) {
    const unsigned bytes=dr_native_quant_bytes(cpu,pc,gqr,ea);
    dr_native_memory_write(cpu,mem,ea,bytes,dr_native_quant_encode(source.lane[0],bytes,gqr),checked,work);
    if (!one)
        dr_native_memory_write(cpu,mem,ea+bytes,bytes,dr_native_quant_encode(source.lane[1],bytes,gqr),checked,work);
}

static inline u32 dr_native_fp_cr(u32 cr, unsigned field, f64 a, f64 b) {
    const unsigned shift=28u-4u*field;
    const u32 flags=isnan(a)||isnan(b) ? 1u : a<b ? 8u : a>b ? 4u : 2u;
    return (cr&~(15u<<shift)) | (flags<<shift);
}

static inline f64 dr_native_fp_integer(f64 value, bool truncate) {
    const f64 whole=truncate ? trunc(value) : nearbyint(value);
    const s32 result=isnan(whole) || whole<=INT32_MIN ? INT32_MIN
                    : whole>=INT32_MAX ? INT32_MAX : (s32)whole;
    return dr_native_float64(0xfff8000000000000ull | (u32)result);
}

#endif

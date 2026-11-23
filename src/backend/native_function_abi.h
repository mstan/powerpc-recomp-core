/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_FUNCTION_ABI_H
#define DOLRECOMP_NATIVE_FUNCTION_ABI_H

#include "common/types.h"
#include <stdlib.h>
#include <string.h>

/* REL shards define these before including the ABI. Labels stay canonical
 * while guest LR values and service observations use the current placement. */
#ifndef DR_NATIVE_GUEST_PC
#define DR_NATIVE_GUEST_PC(pc) (pc)
#define DR_NATIVE_CANONICAL_PC(pc) (pc)
#endif

#define DR_NATIVE_FUNCTION_ABI_VERSION 2u

typedef struct DrNativeCpu DrNativeCpu;
typedef void (*DrNativeFunction)(DrNativeCpu*, u8* mem);
typedef struct { f64 lane[2]; } DrNativeFloat;
typedef struct {
    u32 address;
    DrNativeFunction run;
} DrNativeEntry;
typedef struct {
    u32 address, size;
    u64 digest;
} DrNativeCodeSection;
typedef struct {
    u32 module_id, section, canonical, size;
    const DrNativeEntry* entries;
    size_t entry_count;
    const u32* words;
    const u32* masks;
    const u32* operand_offsets;
    u32* operands;
    size_t operand_count;
    u32* bias;
} DrNativeRelCode;
typedef enum {
    DR_NATIVE_SPR_READ, DR_NATIVE_SPR_WRITE, DR_NATIVE_TB_READ,
    DR_NATIVE_MSR_READ, DR_NATIVE_MSR_WRITE,
    DR_NATIVE_FENCE, DR_NATIVE_DC_STORE, DR_NATIVE_DC_FLUSH,
    DR_NATIVE_DC_INVALIDATE, DR_NATIVE_IC_INVALIDATE,
    DR_NATIVE_DC_ZERO, DR_NATIVE_LC_ZERO, DR_NATIVE_SYSTEM_CALL, DR_NATIVE_RFI,
    DR_NATIVE_WAIT_TB_LOW,
} DrNativeSystemOp;

/* Native calls exchange the guest ABI's volatile integer arguments and
 * results and its stack pointer. Callee-preserved registers stay in C locals.
 * System entry points requiring additional state use dedicated host bindings. */
struct DrNativeCpu {
    u32 gpr[14], cr, lr, ctr, xer;
    DrNativeFloat fpr[14];
    u32 fpscr;
    const u32* gqr;
    u64 deadline;
    u64 work;
    const DrNativeFunction* linked;
    u8* mem1;
    u32 mem1_size;
    const u8* memory_pages;
    /* Owner-thread views. Flagged pages may use a per-page range, a common
     * range shared by the flagged pages, or both. Each nonzero mask binds
     * to a live array or range. Entries pack first byte in the low half
     * and exclusive limit in the high half. Zero describes an empty range. */
    const u64* mem1_page_ranges;
    const u64* mem1_shared_range;
    u8 mem1_page_range_flag;
    u8 mem1_shared_range_flag;
    bool direct_stores;
    void (*service)(DrNativeCpu*, u32 pc);
    void (*observe_entry)(DrNativeCpu*, u32 pc);
    /* A pending resume identifies one live continuation on the active fiber. */
    void* resume;
    void (*resume_landed)(DrNativeCpu*);
    void (*thread_save)(DrNativeCpu*,u32 context,u32 resume_pc,void* continuation,const u32* gpr);
    void (*rel_transition)(DrNativeCpu*,u32 pc,u32 module_header,bool linked);
    u32 (*read)(DrNativeCpu*, u32 address, unsigned size);
    void (*write)(DrNativeCpu*, u32 address, unsigned size, u32 value);
    u32 (*system)(DrNativeCpu*, u32 pc, DrNativeSystemOp, u32 selector, u32 value);
    void (*unsupported)(DrNativeCpu*, u32 pc, u32 detail);
    void* host;
};

static inline _Noreturn void dr_native_fault(DrNativeCpu* cpu, u32 pc, u32 detail) {
    if (cpu->unsupported)
        cpu->unsupported(cpu, pc, detail);
    abort();
}

static inline DrNativeFunction dr_native_linked(DrNativeCpu* cpu, u32 pc, u32 target) {
    const u32 canonical = target & ~0x40000000u;
    if (cpu->linked && canonical - 0x80000000u < 0x01800000u) {
        const DrNativeFunction entry = cpu->linked[(canonical - 0x80000000u) >> 2];
        if (entry)
            return entry;
    }
    dr_native_fault(cpu, pc, target);
}

static inline bool dr_native_cond(u32 cr, u32* ctr, unsigned bo, unsigned bi) {
    if (!(bo & 4u))
        --*ctr;
    const bool count = (bo & 4u) || ((*ctr != 0) != ((bo & 2u) != 0));
    const bool condition = (bo & 16u) || (((cr >> (31u-bi)) & 1u) == ((bo >> 3) & 1u));
    return count && condition;
}

static inline u32 dr_native_compare(u32 cr, u32 xer, unsigned field, bool less,
                                    bool greater) {
    const unsigned shift = 28u - 4u*field;
    const u32 flags = (less ? 8u : greater ? 4u : 2u) | (xer >> 31);
    return (cr & ~(15u << shift)) | (flags << shift);
}

static inline u32 dr_native_add(u32 left, u32 right, u32 carry, u32* xer,
                               bool record_carry, bool record_overflow) {
    const u64 result = (u64)left + right + carry;
    if (record_carry)
        *xer = (*xer & ~0x20000000u) | ((u32)(result >> 32) << 29);
    if (record_overflow) {
        const s64 value = (s64)(s32)left + (s32)right + carry;
        *xer = (*xer & ~0x40000000u) |
               ((value > INT32_MAX || value < INT32_MIN) ? 0xc0000000u : 0);
    }
    return (u32)result;
}

static inline u32 dr_native_rotate_left(u32 value, unsigned shift) {
    shift &= 31u;
    return (value << shift) | (value >> ((32u-shift) & 31u));
}

static inline u32 dr_native_shift_signed(u32 value, unsigned shift, u32* xer) {
    shift &= 63u;
    const bool negative = (value >> 31) != 0;
    const unsigned amount = shift < 32 ? shift : 31u;
    const u32 discarded = shift < 32 ? value & ((1u << amount)-1u) : value;
    *xer = (*xer & ~0x20000000u) | ((negative && discarded) ? 0x20000000u : 0);
    const u32 fill = negative ? ~0u : 0u;
    return (value >> amount) | (fill ^ (fill >> amount));
}

static inline u32 dr_native_clz(u32 value) {
#if defined(__GNUC__) || defined(__clang__)
    return value ? (u32)__builtin_clz(value) : 32u;
#else
    u32 count = 32u;
    for (; value; value >>= 1)
        --count;
    return count;
#endif
}

static inline u32 dr_native_swap16(u32 value) {
    return ((value & 255u) << 8) | ((value >> 8) & 255u);
}

static inline u32 dr_native_swap32(u32 value) {
    return ((value & 0xffu) << 24) | ((value & 0xff00u) << 8) |
           ((value >> 8) & 0xff00u) | (value >> 24);
}

static inline u32 dr_native_divide(u32 numerator, u32 denominator, bool sign,
                                  bool record_overflow, u32* xer) {
    const bool overflow = denominator == 0 || (sign && numerator == 0x80000000u && denominator == ~0u);
    if (record_overflow)
        *xer = (*xer & ~0x40000000u) | (overflow ? 0xc0000000u : 0);
    if (overflow)
        return sign && (numerator >> 31) ? ~0u : 0u;
    return sign ? (u32)((s32)numerator / (s32)denominator) : numerator / denominator;
}

/* Load a complete scalar without assuming alignment or aliasing. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define DR_NATIVE_BIG_ENDIAN 1
#elif defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define DR_NATIVE_BIG_ENDIAN 0
#endif
static inline u32 dr_native_read_be(const u8* memory, unsigned size) {
    if (size == 1)
        return memory[0];
#if defined(DR_NATIVE_BIG_ENDIAN)
    if (size == 2) {
        u16 value;
        memcpy(&value,memory,2);
        return DR_NATIVE_BIG_ENDIAN ? value : dr_native_swap16(value);
    }
    u32 value;
    memcpy(&value,memory,4);
    return DR_NATIVE_BIG_ENDIAN ? value : dr_native_swap32(value);
#else
    if (size == 2)
        return ((u32)memory[0] << 8) | memory[1];
    return ((u32)memory[0] << 24) | ((u32)memory[1] << 16) |
           ((u32)memory[2] << 8) | memory[3];
#endif
}

static inline void dr_native_write_be(u8* memory, unsigned size, u32 value) {
#if defined(DR_NATIVE_BIG_ENDIAN)
    if (size == 2) {
        const u16 converted=(u16)(DR_NATIVE_BIG_ENDIAN ? value : dr_native_swap16(value));
        memcpy(memory,&converted,2);
        return;
    }
    if (size == 4) {
        const u32 converted=DR_NATIVE_BIG_ENDIAN ? value : dr_native_swap32(value);
        memcpy(memory,&converted,4);
        return;
    }
#endif
    for (unsigned i = size; i; --i) {
        memory[i-1] = (u8)value;
        value >>= 8;
    }
}
#undef DR_NATIVE_BIG_ENDIAN

static inline bool dr_native_range_overlaps(u64 range,u32 offset,u32 limit) {
    return (u32)range<limit && offset<(u32)(range>>32);
}

static inline bool dr_native_page_observed(const DrNativeCpu* cpu,u8 owners,u32 page,
                                           u32 offset,u32 limit) {
    const u8 page_flag=cpu->mem1_page_range_flag;
    const u8 shared_flag=cpu->mem1_shared_range_flag;
    return (owners & ~(page_flag|shared_flag)) ||
        ((owners & page_flag) && dr_native_range_overlaps(cpu->mem1_page_ranges[page],offset,limit)) ||
        ((owners & shared_flag) && dr_native_range_overlaps(*cpu->mem1_shared_range,offset,limit));
}

#if defined(__GNUC__) || defined(__clang__)
#define DR_NATIVE_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define DR_NATIVE_INLINE static __forceinline
#else
#define DR_NATIVE_INLINE static inline
#endif

u8* dr_native_mem1_tagged(const DrNativeCpu*,u32 address,u32 limit);

/* Look up detailed ownership only on flagged pages. The generated code may
 * use this once for a proven stack span that needs no ordered observations. */
DR_NATIVE_INLINE u8* dr_native_mem1(const DrNativeCpu* cpu,u32 ea,unsigned bytes) {
    const u32 address = (ea & ~0x40000000u) - 0x80000000u;
    const u64 limit = (u64)address + bytes;
    if (!bytes || bytes>4096u || limit > cpu->mem1_size)
        return NULL;

    const u8* pages = cpu->memory_pages;
    if (pages) {
        const u32 first = address >> 12;
        const u32 last = ((u32)limit - 1u) >> 12;
        if (pages[first] || (first!=last && pages[last]))
            return dr_native_mem1_tagged(cpu,address,(u32)limit);
    }
    return cpu->mem1 + address;
}

/* The native mapped view aliases backing MEM1. The generator keeps the
 * approved span inside this straight-line owner-thread region. Without a
 * mapped view, or if any byte could require observation, use the usual
 * ordered access path. */
DR_NATIVE_INLINE bool dr_native_stack_span(const DrNativeCpu* cpu,const u8* mapped,
                                           u32 sp,s32 first,s32 limit,bool writing) {
    if(!mapped || (writing && !cpu->direct_stores))
        return false;
    const s64 start=(s64)sp+first;
    if(start<0 || start+limit-first>INT64_C(0x100000000))
        return false;
    return dr_native_mem1(cpu,(u32)start,(unsigned)(limit-first))!=NULL;
}

/* The host owns one implementation of each scalar access width across the
 * generated title. Only an actual observer publishes accumulated work. */
u32 dr_native_load8(DrNativeCpu*,u32 ea,u64 work);
u32 dr_native_load16(DrNativeCpu*,u32 ea,u64 work);
u32 dr_native_load32(DrNativeCpu*,u32 ea,u64 work);
void dr_native_store8(DrNativeCpu*,u32 ea,u32 value,u64 work);
void dr_native_store16(DrNativeCpu*,u32 ea,u32 value,u64 work);
void dr_native_store32(DrNativeCpu*,u32 ea,u32 value,u64 work);
u32 dr_native_read_observed(DrNativeCpu*,u32 ea,unsigned bytes,u64 work);
void dr_native_write_observed(DrNativeCpu*,u32 ea,unsigned bytes,u32 value,u64 work);

DR_NATIVE_INLINE u32 dr_native_checked_read(DrNativeCpu* cpu,u32 ea,unsigned bytes,u64 work) {
    switch (bytes) {
    case 1: return dr_native_load8(cpu,ea,work);
    case 2: return dr_native_load16(cpu,ea,work);
    case 4: return dr_native_load32(cpu,ea,work);
    default: return dr_native_read_observed(cpu,ea,bytes,work);
    }
}

DR_NATIVE_INLINE void dr_native_checked_write(DrNativeCpu* cpu,u32 ea,unsigned bytes,u32 value,u64 work) {
    switch (bytes) {
    case 1: dr_native_store8(cpu,ea,value,work); return;
    case 2: dr_native_store16(cpu,ea,value,work); return;
    case 4: dr_native_store32(cpu,ea,value,work); return;
    default: dr_native_write_observed(cpu,ea,bytes,value,work); return;
    }
}

DR_NATIVE_INLINE u32 dr_native_memory_read(DrNativeCpu* cpu,const u8* raw,u32 ea,
                                           unsigned bytes,bool checked,u64 work) {
    return checked ? dr_native_checked_read(cpu,ea,bytes,work) : dr_native_read_be(raw+ea,bytes);
}

DR_NATIVE_INLINE void dr_native_memory_write(DrNativeCpu* cpu,u8* raw,u32 ea,
                                             unsigned bytes,u32 value,bool checked,u64 work) {
    if (checked)
        dr_native_checked_write(cpu,ea,bytes,value,work);
    else
        dr_native_write_be(raw+ea,bytes,value);
}

#undef DR_NATIVE_INLINE

#endif

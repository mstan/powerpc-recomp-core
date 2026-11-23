/* SPDX-License-Identifier: MIT */
#include "backend/native_function_abi.h"

#if defined(__GNUC__) || defined(__clang__)
#define DR_NATIVE_SLOW __attribute__((cold,noinline))
#define DR_NATIVE_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define DR_NATIVE_SLOW __declspec(noinline)
#define DR_NATIVE_INLINE static __forceinline
#else
#define DR_NATIVE_SLOW
#define DR_NATIVE_INLINE static inline
#endif

u8* dr_native_mem1_tagged(const DrNativeCpu* cpu,u32 address,u32 limit) {
    const u8* pages=cpu->memory_pages;
    const u32 first=address>>12;
    const u32 last=(limit-1u)>>12;
    if (dr_native_page_observed(cpu,pages[first],first,address,limit) ||
        (first!=last && dr_native_page_observed(cpu,pages[last],last,address,limit)))
        return NULL;
    return cpu->mem1+address;
}

DR_NATIVE_SLOW u32 dr_native_read_observed(DrNativeCpu* cpu,u32 ea,unsigned bytes,u64 work) {
    const u8* cell=dr_native_mem1(cpu,ea,bytes);
    if (cell)
        return dr_native_read_be(cell,bytes);
    cpu->work=work;
    return cpu->read(cpu,ea,bytes);
}

DR_NATIVE_SLOW void dr_native_write_observed(DrNativeCpu* cpu,u32 ea,unsigned bytes,u32 value,u64 work) {
    u8* cell=cpu->direct_stores ? dr_native_mem1(cpu,ea,bytes) : NULL;
    if (cell) {
        dr_native_write_be(cell,bytes,value);
        return;
    }
    cpu->work=work;
    cpu->write(cpu,ea,bytes,value);
}

/* The ordinary path neither consults tagged byte ranges nor enters the
 * device owner. Cross-page and tagged addresses retain exact observation. */
static inline u8* dr_native_plain_ram(const DrNativeCpu* cpu,u32 ea,unsigned bytes) {
    const u32 address=(ea&~0x40000000u)-0x80000000u;
    const u64 limit=(u64)address+bytes;
    if (limit>cpu->mem1_size || (address&4095u)>4096u-bytes)
        return NULL;
    const u8* pages=cpu->memory_pages;
    if (pages && pages[address>>12])
        return NULL;
    return cpu->mem1+address;
}

/* Resolve exact ownership before deciding whether a scalar has a guest
 * observer. A page with a resident subrange can still contain ordinary RAM. */
DR_NATIVE_INLINE u8* dr_native_resolve_scalar(const DrNativeCpu* cpu,u32 ea,unsigned bytes) {
    const u32 address=(ea&~0x40000000u)-0x80000000u;
    const u64 end=(u64)address+bytes;
    if (end>cpu->mem1_size)
        return NULL;
    const u8* pages=cpu->memory_pages;
    if (pages) {
        const u32 first=address>>12;
        const u8 owners=pages[first];
        if (owners && dr_native_page_observed(cpu,owners,first,address,(u32)end))
            return NULL;
        if ((address&4095u)>4096u-bytes) {
            const u8 next_owners=pages[first+1];
            if (next_owners && dr_native_page_observed(cpu,next_owners,first+1,address,(u32)end))
                return NULL;
        }
    }
    return cpu->mem1+address;
}

/* Keep the unflagged path a leaf. A flagged scalar resolves byte ownership
 * once in its width-specific entry, and calls the device only on overlap. */
#define DR_DEFINE_RAM_WIDTH(bits, bytes) \
    static DR_NATIVE_SLOW u32 dr_native_load##bits##_tagged( \
            DrNativeCpu* cpu,u32 ea,u64 work) { \
        const u8* cell=dr_native_resolve_scalar(cpu,ea,bytes); \
        if (cell) \
            return dr_native_read_be(cell,bytes); \
        cpu->work=work; \
        return cpu->read(cpu,ea,bytes); \
    } \
    static DR_NATIVE_SLOW void dr_native_store##bits##_tagged( \
            DrNativeCpu* cpu,u32 ea,u32 value,u64 work) { \
        u8* cell=cpu->direct_stores ? dr_native_resolve_scalar(cpu,ea,bytes) : NULL; \
        if (cell) { \
            dr_native_write_be(cell,bytes,value); \
            return; \
        } \
        cpu->work=work; \
        cpu->write(cpu,ea,bytes,value); \
    } \
    u32 dr_native_load##bits(DrNativeCpu* cpu,u32 ea,u64 work) { \
        const u8* cell=dr_native_plain_ram(cpu,ea,bytes); \
        return cell ? dr_native_read_be(cell,bytes) : dr_native_load##bits##_tagged(cpu,ea,work); \
    } \
    void dr_native_store##bits(DrNativeCpu* cpu,u32 ea,u32 value,u64 work) { \
        u8* cell=cpu->direct_stores ? dr_native_plain_ram(cpu,ea,bytes) : NULL; \
        if (cell) \
            dr_native_write_be(cell,bytes,value); \
        else \
            dr_native_store##bits##_tagged(cpu,ea,value,work); \
    }

DR_DEFINE_RAM_WIDTH(8, 1u)
DR_DEFINE_RAM_WIDTH(16, 2u)
DR_DEFINE_RAM_WIDTH(32, 4u)

#undef DR_DEFINE_RAM_WIDTH
#undef DR_NATIVE_SLOW
#undef DR_NATIVE_INLINE

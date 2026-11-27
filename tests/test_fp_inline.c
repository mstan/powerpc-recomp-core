/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "cpu/cpu.h"

#define BINARY_OPS(X) \
    X(ppc_fadds) X(ppc_fsubs) X(ppc_fmuls) X(ppc_fdivs) \
    X(ppc_fadd) X(ppc_fsub) X(ppc_fmul) X(ppc_fdiv) \
    X(ppc_ps_add_op) X(ppc_ps_sub_op) X(ppc_ps_mul_op) X(ppc_ps_div_op)

/* Bind these calls before the optional header redirects the public names. */
#define REFERENCE(name) \
    static void reference_##name(CPUState* c,u8 d,u8 a,u8 b){name(c,d,a,b);}
BINARY_OPS(REFERENCE)
#undef REFERENCE
static void reference_frsp(CPUState* c,u8 d,u8 a,u8 b){(void)a;ppc_frsp(c,d,b);}
static void reference_fpscr(CPUState* c){ppc_fpscr_updated(c);}

#include "cpu/fp_binary.h"

#define INLINE(name) \
    static void inline_##name(CPUState* c,u8 d,u8 a,u8 b){name(c,d,a,b);}
BINARY_OPS(INLINE)
#undef INLINE
static void inline_frsp(CPUState* c,u8 d,u8 a,u8 b){(void)a;ppc_frsp(c,d,b);}
typedef void (*Operation)(CPUState*,u8,u8,u8);
#define REF_ENTRY(name) reference_##name,
#define INLINE_ENTRY(name) inline_##name,
static const Operation reference_ops[]={BINARY_OPS(REF_ENTRY) reference_frsp};
static const Operation inline_ops[]={BINARY_OPS(INLINE_ENTRY) inline_frsp};
#undef REF_ENTRY
#undef INLINE_ENTRY

static const u64 values[]={
    0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,
    0x3ff8000000000000ull,0xbff8000000000000ull,
    0x7ff0000000000000ull,0xfff0000000000000ull,
    0x7ff8000000000001ull,0xfff8000012345678ull,
    0x7ff0000000000001ull,0xfff0000000000001ull,
    1,0x8000000000000001ull,0x000fffffffffffffull,
    0x0010000000000000ull,0x7fefffffffffffffull,
    0x36a0000000000000ull,0x3810000000000000ull,
    0x47efffffe0000000ull,0x3ff0000008000000ull,
    0x3ff0000000000001ull,0x3fefffffffffffffull
};
static const u32 profiles[]={0,1,2,3,4,0x80u,0x10u,0x94u,0xffffffffu};
static const u8 aliases[][3]={{3,1,2},{1,1,2},{2,1,2},{1,1,1},{0,0,0},{31,30,31}};
static void seed(CPUState* c,unsigned a,unsigned b,u32 fpscr) {
    memset(c,0,sizeof *c);
    c->pc=0x80001000u;c->msr=0x2000u;c->fpscr=fpscr;c->cr=0x12345678u;
    c->cycles=123;c->cycle_deadline=456;c->reserve_valid=true;
    for(unsigned r=0;r<32;++r) {
        c->gpr[r]=0xa5a50000u+r;
        u64 x=values[(a+r)%(sizeof values/sizeof *values)];
        u64 y=values[(b+3*r)%(sizeof values/sizeof *values)];
        memcpy(&c->fpr[r],&x,8);memcpy(&c->ps1[r],&y,8);
    }
}
int main(void) {
    CPUState reference,inlined;unsigned cases=0;
    for(unsigned op=0;op<sizeof reference_ops/sizeof *reference_ops;++op)
    for(unsigned a=0;a<sizeof values/sizeof *values;++a)
    for(unsigned b=0;b<sizeof values/sizeof *values;++b)
    for(unsigned p=0;p<sizeof profiles/sizeof *profiles;++p)
    for(unsigned alias=0;alias<sizeof aliases/sizeof *aliases;++alias) {
        seed(&reference,a,b,profiles[p]);
        /* Independent scalar operands; the paired lane receives their reverse.
         * If source registers overlap, the last assignment defines that one
         * architectural register identically for both implementations. */
        memcpy(&reference.fpr[aliases[alias][1]],&values[a],8);
        memcpy(&reference.fpr[aliases[alias][2]],&values[b],8);
        memcpy(&reference.ps1[aliases[alias][1]],&values[b],8);
        memcpy(&reference.ps1[aliases[alias][2]],&values[a],8);
        inlined=reference;
        reference_ops[op](&reference,aliases[alias][0],aliases[alias][1],aliases[alias][2]);
        inline_ops[op](&inlined,aliases[alias][0],aliases[alias][1],aliases[alias][2]);
        ++cases;
        if(memcmp(&reference,&inlined,sizeof reference)) {
            fprintf(stderr,"FP inline mismatch op=%u a=%u b=%u fpscr=%08x alias=%u\n",
                op,a,b,profiles[p],alias);return 1;
        }
    }
    for(unsigned p=0;p<sizeof profiles/sizeof *profiles;++p) {
        seed(&reference,0,1,profiles[p]);inlined=reference;
        reference_fpscr(&reference);ppc_fpscr_updated(&inlined);++cases;
        if(memcmp(&reference,&inlined,sizeof reference))return 1;
    }
    printf("FP profile %u/reference exact: %u cases (NaNs, denormals, rounding flags, overlap, all CPU fields)\n",
        DOLRECOMP_INLINE_FP,cases);
    return 0;
}

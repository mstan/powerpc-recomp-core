/* SPDX-License-Identifier: MIT */
#include "backend/emitter.h"
#include "backend/dispatch.h"
#include "frontend/decoder.h"
#include <stdio.h>
#include <stdlib.h>

static int fixture(FILE* out,u32 base,const u32* words,u32 count) {
    PPCInst* code=calloc(count,sizeof(*code));
    if(!code)return 0;
    for(u32 i=0;i<count;++i)code[i]=ppc_decode(words[i],base+i*4u);
    dolrecomp_set_symbol_suffix("_reference");
    /* Preserve the original native page exits, rather than comparing the unit
     * against another already-combined function. */
    for(u32 first=0;first<count;) {
        u32 end=first+1;
        while(end<count && (code[end].address>>12)==(code[first].address>>12))++end;
        emit_function(out,code+first,end-first,code[first].address);first=end;
    }
    dolrecomp_set_symbol_suffix("_step");
    for(u32 i=0;i<count;++i)emit_function(out,code+i,1,code[i].address);
    dolrecomp_set_symbol_suffix("_candidate");
    int ok=emit_function_unit(out,code,count,base);
    free(code);return ok;
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    FILE* out=fopen(argv[1],"w");if(!out)return 2;
    emit_header(out);
    /* A standalone host has no native ownership header. This fixture declares
     * only its one known scratch data page closed; the composed consumer test
     * omits this define and uses the real native_code page-fence metadata. */
    fputs("#if FUNCTION_UNIT_TEST_UNOBSERVED_STORE\n#define DR_FUNCTION_STORE_PAGE_CLOSED(page) ((page)==1u)\n#endif\n",out);
    /* Integer loop, observing store/load and an opaque external call. */
    const u32 integer[]={0x38630001u,0x7c833214u,0x2c030004u,0x4180fff4u,
        0x38e40002u,0x90e50000u,0x7d0802a6u,0x48001001u,
        0x38630002u,0x81250004u,0x7d491a14u,0x7d0803a6u,0x4e800020u};
    /* First-FP fault, FP/helper state, memory and exact deadline boundaries. */
    const u32 fp[]={0xec61102au,0x38630001u,0x90650000u,
        0xc8850004u,0xfc60201eu,0x4e800020u};
    /* A live register crosses the original page boundary. */
    const u32 pages[]={0x38630001u,0x7c833214u,0x38e40002u,
        0x90e50000u,0x81250004u,0x7d491a14u,0x4e800020u};
    int ok=fixture(out,0x80006000u,integer,sizeof integer/sizeof *integer) &&
        fixture(out,0x80006200u,fp,sizeof fp/sizeof *fp) &&
        fixture(out,0x80006ff8u,pages,sizeof pages/sizeof *pages);
    fclose(out);return !ok;
}

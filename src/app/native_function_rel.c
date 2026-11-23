/* SPDX-License-Identifier: MIT */
#include "backend/native_function_abi.h"
#include "app/native_function.h"
#include "backend/native_function_emit.h"
#include "frontend/container/rel.h"
#include "platform/fs.h"
#include <stdlib.h>

typedef struct {
    const RELFile* module;
    const RELSection* text;
    const DrNativeSymbols* dol_symbols;
    DrNativeRelocation* operands;
    size_t operand_count;
    u8* possible_targets;
} RelEmission;

static u32 read_word(const u8* p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}

static bool record_relocation(void* user,const RELRelocation* patch) {
    RelEmission* data=user;
    const RELSection* text=data->text;
    if(patch->section!=text->index) {
        /* CodeWarrior switch tables are relocated pointers into this text
         * section. Only interior addresses become local branch targets. */
        if(patch->type==1 && patch->module_id==data->module->module_id &&
           patch->target_section==text->index && !(patch->target_offset&3u) &&
           patch->target_offset<text->size)
            data->possible_targets[patch->target_offset/4u]=1;
        return true;
    }
    if(!patch->type)return true;
    if(patch->offset>=text->size || data->operand_count==text->size/4u)return false;
    const u32 offset=patch->offset&~3u;
    const u32 opcode=read_word(text->data+offset)>>26;
    const bool allowed=(patch->type==DR_NATIVE_ADDR16_LO && (patch->offset&3u)==2 &&
                        (opcode==13 || opcode==14 || (opcode>=32 && opcode<=55))) ||
                       (patch->type==DR_NATIVE_ADDR16_HA && (patch->offset&3u)==2 && opcode==15) ||
                       (patch->type==DR_NATIVE_REL24 && (patch->offset&3u)==0 && opcode==18);
    if(!allowed) {
        fprintf(stderr,"REL %u has unsupported executable relocation %u at %08X\n",
                data->module->module_id,patch->type,text->address+offset);
        return false;
    }
    const DrNativeSymbol* helper=NULL;
    if(data->dol_symbols && patch->type==DR_NATIVE_REL24 && !patch->module_id) {
        const DrNativeSymbol* target=dr_native_symbol_at(data->dol_symbols,patch->target_offset);
        if(target && target->kind>=DR_NATIVE_SAVE_GPR)helper=target;
    }
    data->operands[data->operand_count++]=(DrNativeRelocation){.address=text->address+offset,
                                                              .kind=patch->type,.helper=helper};
    return true;
}

static int compare_relocations(const void* lhs,const void* rhs) {
    const u32 a=((const DrNativeRelocation*)lhs)->address;
    const u32 b=((const DrNativeRelocation*)rhs)->address;
    return a<b ? -1 : a>b;
}

static void emit_rel_words(FILE* out,const RelEmission* data) {
    const size_t words=data->text->size/4u;
    fputs("static const u32 words[] = {\n",out);
    for(size_t i=0;i<words;++i)
        fprintf(out,"0x%08Xu,%s",read_word(data->text->data+i*4u),(i&7u)==7u ? "\n" : " ");
    fputs("\n};\nstatic const u32 masks[] = {\n",out);
    size_t next=0;
    for(size_t i=0;i<words;++i) {
        u32 mask=UINT32_MAX;
        if(next<data->operand_count && data->operands[next].address==data->text->address+i*4u)
            mask=data->operands[next++].kind==DR_NATIVE_REL24 ? 0xfc000003u : 0xffff0000u;
        fprintf(out,"0x%08Xu,%s",mask,(i&7u)==7u ? "\n" : " ");
    }
    fputs("\n};\n",out);
    if(!data->operand_count)return;
    fprintf(out,"u32 dr_native_rel_operands_%u[%zu];\nstatic const u32 operand_offsets[] = {\n",
            data->module->module_id,data->operand_count);
    for(size_t i=0;i<data->operand_count;++i)
        fprintf(out,"0x%08Xu,%s",data->operands[i].address-data->text->address,(i&7u)==7u ? "\n" : " ");
    fputs("\n};\n",out);
}

static FILE* open_shard(const char* directory,FILE* manifest,u32 id,unsigned index,bool operands) {
    char name[48];
    snprintf(name,sizeof(name),"native_rel_%04u.c",index);
    FILE* out=dr_native_open_output(directory,name);
    if(!out)return NULL;
    fprintf(manifest,"    \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n",name);
    fprintf(out,"#define DR_NATIVE_GUEST_PC(pc) ((pc) + dr_native_rel_bias_%u)\n"
                "#define DR_NATIVE_CANONICAL_PC(pc) ((pc) - dr_native_rel_bias_%u)\n"
                "#include \"backend/native_function_fp.h\"\n"
                "extern u32 dr_native_rel_bias_%u;\n",id,id,id);
    if(operands)
        fprintf(out,"extern u32 dr_native_rel_operands_%u[];\n"
                    "#define DR_NATIVE_OPERAND(i) (dr_native_rel_operands_%u[i])\n",id,id);
    return out;
}

static bool emit_rel(const RelEmission* data,const DrNativeSymbols* symbols,const char* directory) {
    const RELSection* text=data->text;
    const u32 id=data->module->module_id;
    const DrNativeSymbol* entry=dr_native_symbol_at(symbols,data->module->entry_point);
    if(!entry || entry->kind!=DR_NATIVE_FUNCTION) {
        fprintf(stderr,"REL %u has no function annotation for prolog %08X\n",id,data->module->entry_point);
        return false;
    }
    u32* targets=malloc((text->size/4u)*sizeof(*targets));
    if(!targets)return false;
    FILE* manifest=dr_native_open_output(directory,"native_rel_sources.cmake");
    FILE* catalog=dr_native_open_output(directory,"native_rel_catalog.c");
    bool ok=manifest && catalog;
    FILE* shard=NULL;
    unsigned shard_count=0,shard_words=0;
    size_t next=0,functions=0,total_targets=0;
    if(ok) {
        fprintf(manifest,"list(APPEND DR_NATIVE_REL_MODULES %u)\n"
                         "list(APPEND DR_NATIVE_REL_ABIS %u)\nlist(APPEND DR_NATIVE_REL_SOURCES\n"
                         "    \"${CMAKE_CURRENT_LIST_DIR}/native_rel_catalog.c\"\n",id,DR_NATIVE_FUNCTION_ABI_VERSION);
        fprintf(catalog,"#include \"backend/native_function_abi.h\"\n\nu32 dr_native_rel_bias_%u;\n",id);
        emit_rel_words(catalog,data);
    }
    for(size_t i=0;ok && i<symbols->count;++i) {
        const DrNativeSymbol* fn=symbols->entries+i;
        if(fn->kind!=DR_NATIVE_FUNCTION)continue;
        if(!shard || shard_words>=1024u) {
            if(shard && fclose(shard)!=0) {shard=NULL;ok=false;break;}
            shard=open_shard(directory,manifest,id,shard_count++,data->operand_count!=0);
            if(!shard) {ok=false;break;}
            shard_words=0;
        }
        while(next<data->operand_count && data->operands[next].address<fn->address)++next;
        const size_t first=next;
        const u32 end=fn->address+fn->size;
        while(next<data->operand_count && data->operands[next].address<end)++next;
        size_t target_count=0;
        for(u32 address=fn->address+4u;address<end;address+=4u)
            if(data->possible_targets[(address-text->address)/4u])targets[target_count++]=address;
        const DrNativeEmission emission={.local_targets=targets,.target_count=target_count,
            .relocations=data->operands+first,.relocation_count=next-first,.checked_memory=true};
        ok=dr_emit_native_function_bytes(shard,symbols,fn,text->data+(fn->address-text->address),&emission);
        if(!ok)fprintf(stderr,"REL %u could not emit function %08X\n",id,fn->address);
        fprintf(catalog,"extern void dr_native_%08X(DrNativeCpu*,u8*);\n",fn->address);
        shard_words+=fn->size/4u;
        total_targets+=target_count;
        ++functions;
    }
    if(ok) {
        fputs("static const DrNativeEntry entries[] = {\n",catalog);
        for(size_t i=0;i<symbols->count;++i) {
            const DrNativeSymbol* fn=symbols->entries+i;
            if(fn->kind==DR_NATIVE_FUNCTION)
                fprintf(catalog,"    {0x%08Xu,dr_native_%08X},\n",fn->address,fn->address);
        }
        fprintf(catalog,"};\nconst DrNativeRelCode dr_native_rel_code_%u = {\n"
                        "    .module_id=%uu,.section=%uu,.canonical=0x%08Xu,.size=%uu,\n"
                        "    .entries=entries,.entry_count=%zu,.words=words,.masks=masks,\n",
                id,id,text->index,text->address,text->size,functions);
        if(data->operand_count)
            fprintf(catalog,"    .operand_offsets=operand_offsets,.operands=dr_native_rel_operands_%u,"
                            ".operand_count=%zu,\n",id,data->operand_count);
        fprintf(catalog,"    .bias=&dr_native_rel_bias_%u,\n};\n",id);
        fputs(")\n",manifest);
    }
    if(shard && fclose(shard)!=0)ok=false;
    if(manifest && fclose(manifest)!=0)ok=false;
    if(catalog && fclose(catalog)!=0)ok=false;
    free(targets);
    if(ok)printf("REL %u emitted %zu functions, %zu relocations, %zu local targets\n",
                 id,functions,data->operand_count,total_targets);
    return ok;
}

int dr_native_function_rel_main(int argc,char** argv) {
    if(argc!=6 && argc!=7) {
        fprintf(stderr,"usage: %s function-native-rel REL DTK_SYMBOLS OUTPUT_DIRECTORY"
                       " CANONICAL_FILE_BASE [DOL_SYMBOLS]\n",argv[0]);
        return 1;
    }
    char* end;
    const unsigned long long base=strtoull(argv[5],&end,0);
    if(end==argv[5] || *end || base>UINT32_MAX || (base&3u))return 1;
    RELFile rel;
    if(!rel_load_image(&rel,argv[2],(u32)base))return 1;
    const RELSection* text=NULL;
    for(u32 i=0;i<rel.section_count;++i) {
        const RELSection* section=rel.sections+i;
        if(!section->executable || !section->size)continue;
        if(text) {
            fprintf(stderr,"REL %u has more than one section for the .text annotation\n",rel.module_id);
            rel_free(&rel);
            return 1;
        }
        text=section;
    }
    if(!text) {rel_free(&rel);return 1;}
    RelEmission data={.module=&rel,.text=text,
        .operands=calloc(text->size/4u,sizeof(*data.operands)),.possible_targets=calloc(text->size/4u,1)};
    DrNativeSymbols symbols={0},dol_symbols={0};
    const DrNativeSection section={".text",text->address,text->size,true};
    FILE* input=fopen(argv[3],"r");
    if(!input)fprintf(stderr,"cannot open native REL symbol map '%s'\n",argv[3]);
    bool imports=true;
    if(argc==7) {
        FILE* dol=fopen(argv[6],"r");
        imports=dol && dr_native_symbols_read(dol,NULL,0,&dol_symbols);
        if(dol)fclose(dol);
        if(imports)data.dol_symbols=&dol_symbols;
        else fprintf(stderr,"cannot read native DOL symbols '%s'\n",argv[6]);
    }
    bool ok=imports && input && data.operands && data.possible_targets &&
        dr_native_symbols_read(input,&section,1,&symbols) && rel_visit_relocations(&rel,record_relocation,&data);
    if(ok) {
        qsort(data.operands,data.operand_count,sizeof(*data.operands),compare_relocations);
        for(size_t i=0;i<data.operand_count;++i) {
            data.operands[i].operand=(u32)i;
            if(i && data.operands[i-1].address==data.operands[i].address)ok=false;
        }
        if(!ok)fprintf(stderr,"REL %u has duplicate instruction relocations\n",rel.module_id);
    }
    if(ok)ok=make_dir_tree(argv[4]) && emit_rel(&data,&symbols,argv[4]);
    if(input)fclose(input);
    dr_native_symbols_free(&dol_symbols);
    dr_native_symbols_free(&symbols);
    free(data.possible_targets);
    free(data.operands);
    rel_free(&rel);
    return ok ? 0 : 1;
}

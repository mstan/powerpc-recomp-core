/* SPDX-License-Identifier: MIT */
#include "backend/native_function_abi.h"
#include "app/native_function.h"
#include "analysis/native_functions.h"
#include "backend/native_function_emit.h"
#include "frontend/container/dol.h"
#include "platform/fs.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    u32 observed,link,unlink,wait,thread_save,thread_rfi,display,overlay;
    u64 wait_digest,clock_digest;
    u32 thp_call_site,thp_target;
    u8 thp_length_register;
    u8 wait_target_register;
} NativeHooks;

static u32 read_word(const u8* bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
           ((u32)bytes[2] << 8) | bytes[3];
}

static u64 code_digest(const u8* bytes,u32 size) {
    u64 digest=0xcbf29ce484222325ull;
    for(u32 offset=0;offset<size;++offset)digest=(digest^bytes[offset])*0x100000001b3ull;
    return digest;
}

static void write_code_identity(FILE* catalog, const DOLFile* dol) {
    unsigned count = 0;
    fputs("const DrNativeCodeSection dr_native_dol_code[] = {\n", catalog);
    for (unsigned i = 0; i < DOL_NUM_TEXT; ++i) {
        const u8* data = dol_get_text_section(dol, (int)i);
        if (!data)
            continue;
        const u32 size = dol->header.text_sizes[i];
        const u64 digest=code_digest(data,size);
        fprintf(catalog, "    {0x%08Xu, %uu, 0x%016llXull},\n", dol->header.text_addresses[i],
                size, (unsigned long long)digest);
        ++count;
    }
    fprintf(catalog, "};\nconst size_t dr_native_dol_code_count = %u;\n", count);
}

static const u8* dol_code(const DOLFile* dol, const DrNativeSymbol* function) {
    for (unsigned i = 0; i < DOL_NUM_TEXT; ++i) {
        const u32 base = dol->header.text_addresses[i];
        const u32 size = dol->header.text_sizes[i];
        if (function->address >= base && function->address - base < size &&
            function->size <= size - (function->address - base)) {
            const u8* code = dol_get_text_section(dol, (int)i);
            return code ? code + (function->address - base) : NULL;
        }
    }
    return NULL;
}

static bool verify_timebase_wait(const DOLFile* dol,const DrNativeSymbols* symbols,NativeHooks* hooks) {
    const DrNativeSymbol* function=dr_native_function_containing(symbols,hooks->wait);
    const DrNativeSymbol span={.address=hooks->wait,.size=40u};
    if(!function || span.address-function->address>function->size ||
       span.size>function->size-(span.address-function->address))return false;
    const u8* bytes=dol_code(dol,&span);
    if(!bytes || code_digest(bytes,span.size)!=hooks->wait_digest)return false;
    const PPCInst sample=ppc_decode(read_word(bytes),span.address);
    const PPCInst subtract=ppc_decode(read_word(bytes+20),span.address+20u);
    const PPCInst repeat=ppc_decode(read_word(bytes+36),span.address+36u);
    if(sample.op!=PPC_OP_B || !sample.lk || subtract.op!=PPC_OP_SUBFC ||
       repeat.op!=PPC_OP_BC || repeat.branch_target!=span.address)return false;
    const DrNativeSymbol* clock=dr_native_symbol_at(symbols,sample.branch_target);
    if(!clock || clock->kind!=DR_NATIVE_FUNCTION || clock->size!=24u)return false;
    const u8* clock_code=dol_code(dol,clock);
    if(!clock_code || code_digest(clock_code,clock->size)!=hooks->clock_digest)return false;
    hooks->wait_target_register=subtract.rA;
    return true;
}

static bool verify_thp_hook(const DOLFile* dol,const DrNativeSymbols* symbols,const NativeHooks* hooks) {
    const DrNativeSymbol call={.address=hooks->thp_call_site,.size=4};
    const u8* bytes=dol_code(dol,&call);
    if(!bytes || !dr_native_function_containing(symbols,call.address))return false;
    const PPCInst edge=ppc_decode(read_word(bytes),call.address);
    const DrNativeSymbol* target=dr_native_symbol_at(symbols,hooks->thp_target);
    return edge.op==PPC_OP_B && edge.lk && edge.branch_target==hooks->thp_target &&
        target && target->kind==DR_NATIVE_FUNCTION;
}

static bool verify_thread_hooks(const DOLFile* dol,const DrNativeSymbols* symbols,const NativeHooks* hooks) {
    const DrNativeSymbol save={.address=hooks->thread_save,.size=4};
    const DrNativeSymbol rfi={.address=hooks->thread_rfi,.size=4};
    const u8* save_bytes=dol_code(dol,&save),*rfi_bytes=dol_code(dol,&rfi);
    if(!save_bytes || !rfi_bytes)return false;
    const PPCInst edge=ppc_decode(read_word(save_bytes),save.address);
    const PPCInst restore=ppc_decode(read_word(rfi_bytes),rfi.address);
    const DrNativeSymbol* caller=dr_native_function_containing(symbols,save.address);
    const DrNativeSymbol* callee=dr_native_symbol_at(symbols,edge.branch_target);
    const DrNativeSymbol* loader=dr_native_function_containing(symbols,rfi.address);
    if(edge.op!=PPC_OP_B || !edge.lk || restore.op!=PPC_OP_RFI ||
       !caller || !callee || callee->kind!=DR_NATIVE_FUNCTION || callee->size<8u ||
       !loader || rfi.address+4u!=loader->address+loader->size)return false;
    const u8* body=dol_code(dol,callee);
    const DrNativeSymbol resumed={.address=save.address+4u,.size=4};
    const u8* compare=dol_code(dol,&resumed);
    /* The selected caller tests the twice-returning SDK call's r3 result.
     * The normal SDK exit returns zero in r3 before its architectural return. */
    return body && compare && read_word(compare)==0x28030000u &&
        read_word(body+callee->size-8u)==0x38600000u &&
        read_word(body+callee->size-4u)==0x4e800020u;
}

/* CodeWarrior branches into these r11-based fall-through sequences. Verify
 * the actual body before substituting local-register copies into its callers. */
static bool verify_save_restore(const DOLFile* dol, const DrNativeSymbol* helper) {
    const bool fp = helper->kind == DR_NATIVE_SAVE_FPR || helper->kind == DR_NATIVE_RESTORE_FPR;
    const bool save = helper->kind == DR_NATIVE_SAVE_GPR || helper->kind == DR_NATIVE_SAVE_FPR;
    const unsigned opcode = fp ? (save ? 54u : 50u) : (save ? 36u : 32u);
    const unsigned width = fp ? 8u : 4u;
    const DrNativeSymbol extent = {
        .address = helper->address,
        .size = (33u-helper->first_register)*4u,
    };
    const u8* bytes = dol_code(dol, &extent);
    if (!bytes)
        return false;
    for (unsigned reg = helper->first_register; reg < 32; ++reg, bytes += 4) {
        const u32 displacement = (u16)(0u-(32u-reg)*width);
        if (read_word(bytes) != ((opcode << 26) | (reg << 21) | (11u << 16) | displacement))
            return false;
    }
    return read_word(bytes) == 0x4e800020u;
}

static const u8* dol_data(const DOLFile* dol, const DrNativeSymbol* symbol) {
    for (unsigned i = 0; i < DOL_NUM_DATA; ++i) {
        const u32 offset = symbol->address - dol->header.data_addresses[i];
        const u32 size = dol->header.data_sizes[i];
        if (offset < size && symbol->size <= size - offset) {
            const u8* data = dol_get_data_section(dol, (int)i);
            return data ? data + offset : NULL;
        }
    }
    return NULL;
}

static int compare_target(const void* lhs, const void* rhs) {
    const u32 a = *(const u32*)lhs, b = *(const u32*)rhs;
    return a < b ? -1 : a > b;
}

static bool read_jump_targets(const DOLFile* dol, const DrNativeSymbols* symbols,
                              u32** targets, size_t* count) {
    *targets = NULL;
    *count = 0;
    size_t capacity = 0;
    for (size_t i = 0; i < symbols->count; ++i) {
        if (symbols->entries[i].kind == DR_NATIVE_JUMP_TABLE ||
            symbols->entries[i].kind == DR_NATIVE_POTENTIAL_JUMP_TABLE)
            capacity += symbols->entries[i].size / 4u;
    }
    if (!capacity)
        return true;
    u32* entries = malloc(capacity * sizeof(*entries));
    if (!entries)
        return false;
    for (size_t i = 0; i < symbols->count; ++i) {
        const DrNativeSymbol* symbol = &symbols->entries[i];
        const bool potential = symbol->kind == DR_NATIVE_POTENTIAL_JUMP_TABLE;
        if (symbol->kind != DR_NATIVE_JUMP_TABLE && !potential)
            continue;
        const u8* bytes = dol_data(dol, symbol);
        if (!bytes) {
            if (potential)
                continue;
            fprintf(stderr, "jump table %08X is outside DOL data\n", symbol->address);
            free(entries);
            return false;
        }
        /* Anonymous SDK switch tables contain aligned code pointers into one
         * function, including at least one instruction inside its body.
         * Do not promote arrays of function pointers to local branches. */
        const size_t start = *count;
        const DrNativeSymbol* owner = potential ?
            dr_native_function_containing(symbols, read_word(bytes)) : NULL;
        bool has_interior = false;
        for (u32 offset = 0; offset < symbol->size; offset += 4) {
            const u32 target = read_word(bytes+offset);
            const DrNativeSymbol* function = target & 3u ? NULL :
                dr_native_function_containing(symbols, target);
            if (!function || (potential && function != owner)) {
                if (potential) {
                    *count = start;
                    break;
                }
                fprintf(stderr, "jump table %08X slot %u has unbound target %08X\n",
                        symbol->address, offset / 4u, target);
                free(entries);
                return false;
            }
            has_interior |= target != function->address;
            entries[(*count)++] = target;
        }
        if (potential && !has_interior)
            *count = start;
    }
    qsort(entries, *count, sizeof(*entries), compare_target);
    size_t unique = 0;
    for (size_t i = 0; i < *count; ++i)
        if (!unique || entries[i] != entries[unique-1])
            entries[unique++] = entries[i];
    *targets = entries;
    *count = unique;
    return true;
}

static size_t lower_target(const u32* targets, size_t count, u32 address) {
    size_t first = 0;
    while (first < count) {
        const size_t middle = first + (count - first)/2;
        if (targets[middle] < address)
            first = middle + 1;
        else
            count = middle;
    }
    return first;
}

FILE* dr_native_open_output(const char* directory, const char* name) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path))
        return NULL;
    FILE* out = fopen(path, "w");
    if (!out)
        fprintf(stderr, "cannot open native function output '%s'\n", path);
    return out;
}

static bool emit_function(FILE* out, const DOLFile* dol, const DrNativeSymbols* symbols,
                          const DrNativeSymbol* symbol, const u32* targets, size_t target_count,
                          const NativeHooks* hooks) {
    const u8* code = dol_code(dol, symbol);
    if (!code) {
        fprintf(stderr, "native function %08X is outside the DOL text\n", symbol->address);
        return false;
    }
    /* Use the portable memory contract until the caller supplies certified
     * memory regions for a mapped build. Emitter selection is independent. */
    const size_t first = lower_target(targets, target_count, symbol->address);
    const size_t last = lower_target(targets, target_count, symbol->address + symbol->size);
    const DrNativeEmission emission={
        .local_targets=targets ? targets+first : NULL,
        .target_count=last-first,
        .checked_memory=true,
        .observe_entry=hooks->observed==symbol->address || hooks->display==symbol->address,
        .observe_scope=hooks->overlay==symbol->address,
        .rel_action=hooks->link==symbol->address ? DR_NATIVE_REL_LINK :
                    hooks->unlink==symbol->address ? DR_NATIVE_REL_UNLINK : DR_NATIVE_REL_NO_TRANSITION,
        .timebase_wait=hooks->wait,
        .thread_save_site=hooks->thread_save,
        .thp_call_site=hooks->thp_call_site,
        .thp_length_register=hooks->thp_length_register,
        .wait_target_register=hooks->wait_target_register,
    };
    return dr_emit_native_function_bytes(out,symbols,symbol,code,&emission);
}

static bool emit_dol(const DOLFile* dol, const DrNativeSymbols* symbols, const char* directory,
                     NativeHooks* hooks) {
    const DrNativeSymbol* entry = dr_native_symbol_at(symbols, dol->header.entry_point);
    if (!entry || entry->kind != DR_NATIVE_FUNCTION) {
        fprintf(stderr, "native DOL entry %08X has no function annotation\n", dol->header.entry_point);
        return false;
    }
    const u32 entries[]={hooks->observed,hooks->link,hooks->unlink,hooks->display,hooks->overlay};
    for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);++i) {
        if(!entries[i])continue;
        const DrNativeSymbol* at=dr_native_symbol_at(symbols,entries[i]);
        if(!at || at->kind!=DR_NATIVE_FUNCTION) {
            fprintf(stderr,"native observed entry %08X has no function annotation\n",entries[i]);
            return false;
        }
    }
    if(hooks->thp_call_site && !verify_thp_hook(dol,symbols,hooks)) {
        fprintf(stderr,"unexpected THP video call at %08X\n",hooks->thp_call_site);
        return false;
    }
    if(hooks->thread_save && !verify_thread_hooks(dol,symbols,hooks)) {
        fprintf(stderr,"unexpected SDK thread context bindings at %08X / %08X\n",
                hooks->thread_save,hooks->thread_rfi);
        return false;
    }
    if(hooks->wait && !verify_timebase_wait(dol,symbols,hooks)) {
        fprintf(stderr,"unexpected signed timebase wait at %08X\n",hooks->wait);
        return false;
    }
    u32* targets;
    size_t target_count;
    if (!read_jump_targets(dol, symbols, &targets, &target_count))
        return false;
    FILE* manifest = dr_native_open_output(directory, "native_sources.cmake");
    FILE* catalog = dr_native_open_output(directory, "native_catalog.c");
    bool ok = manifest && catalog;
    FILE* shard = NULL;
    size_t functions = 0;
    unsigned shard_count = 0, shard_words = 0, thp_shard = 0;
    if (ok) {
        fputs("set(DR_NATIVE_SOURCES\n    \"${CMAKE_CURRENT_LIST_DIR}/native_catalog.c\"\n", manifest);
        fputs("#include \"backend/native_function_abi.h\"\n\n", catalog);
        fprintf(catalog, "const u32 dr_native_dol_observed_entry = 0x%08Xu;\n", hooks->observed);
        fprintf(catalog, "const u32 dr_native_thread_load_rfi = 0x%08Xu;\n\n", hooks->thread_rfi);
    }
    for (size_t i = 0; ok && i < symbols->count; ++i) {
        const DrNativeSymbol* symbol = &symbols->entries[i];
        if (symbol->kind != DR_NATIVE_FUNCTION) {
            if (symbol->kind >= DR_NATIVE_SAVE_GPR && !verify_save_restore(dol, symbol)) {
                fprintf(stderr, "unexpected save/restore helper at %08X\n", symbol->address);
                ok = false;
            }
            continue;
        }
        if (!shard || shard_words >= 1024u) {
            if (shard && fclose(shard) != 0) {
                shard = NULL;
                ok = false;
                break;
            }
            char name[32];
            snprintf(name, sizeof(name), "native_%04u.c", shard_count++);
            shard = dr_native_open_output(directory, name);
            if (!shard) {
                ok = false;
                break;
            }
            fprintf(manifest, "    \"${CMAKE_CURRENT_LIST_DIR}/%s\"\n", name);
            fputs("#include \"backend/native_function_fp.h\"\n", shard);
            fputc('\n', shard);
            shard_words = 0;
        }
        fprintf(catalog, "extern void dr_native_%08X(DrNativeCpu*, u8*);\n", symbol->address);
        if(hooks->thp_call_site>=symbol->address &&
           hooks->thp_call_site-symbol->address<symbol->size)
            thp_shard=shard_count-1u;
        ok = emit_function(shard, dol, symbols, symbol, targets, target_count, hooks);
        shard_words += symbol->size / 4u;
        ++functions;
    }
    if (ok) {
        fputs("\nconst DrNativeEntry dr_native_dol_entries[] = {\n", catalog);
        for (size_t i = 0; i < symbols->count; ++i) {
            const DrNativeSymbol* symbol = &symbols->entries[i];
            if (symbol->kind == DR_NATIVE_FUNCTION)
                fprintf(catalog, "    {0x%08Xu, dr_native_%08X},\n", symbol->address, symbol->address);
        }
        fprintf(catalog, "};\nconst size_t dr_native_dol_count = %zu;\n"
                "const u32 dr_native_dol_entry = 0x%08Xu;\n", functions, entry->address);
        write_code_identity(catalog, dol);
        fputs(")\n", manifest);
        fprintf(manifest,"set(DR_NATIVE_FUNCTION_ABI %u)\n",DR_NATIVE_FUNCTION_ABI_VERSION);
        if(hooks->thp_call_site)
            fprintf(manifest,"set(DR_NATIVE_THP_SOURCE \"${CMAKE_CURRENT_LIST_DIR}/native_%04u.c\")\n",thp_shard);
    }
    if (shard && fclose(shard) != 0)
        ok = false;
    if (catalog && fclose(catalog) != 0)
        ok = false;
    if (manifest && fclose(manifest) != 0)
        ok = false;
    free(targets);
    if (ok)
        printf("emitted %zu native functions in %u sources with %zu local switch targets\n",
               functions, shard_count, target_count);
    return ok;
}

static bool parse_hook(const char* input,u32* result) {
    char* end;
    const unsigned long long address=strtoull(input,&end,0);
    if(end==input || *end || !address || address>UINT32_MAX || (address&3u)) {
        fprintf(stderr,"invalid native observed entry '%s'\n",input);
        return false;
    }
    *result=(u32)address;
    return true;
}

static bool parse_digest(const char* input,u64* result) {
    char* end;
    const unsigned long long digest=strtoull(input,&end,0);
    if(end==input || *end || !digest)return false;
    *result=digest;
    return true;
}

int dr_native_function_main(int argc, char** argv) {
    if (argc!=5 && argc!=6 && argc!=8 && argc!=11 && argc!=13 && argc!=14 && argc!=15 && argc!=18) {
        fprintf(stderr,"usage: %s function-native DOL DTK_SYMBOLS OUTPUT_DIRECTORY"
                       " [OBSERVED_ENTRY [REL_LINK REL_UNLINK [WAIT_ENTRY WAIT_DIGEST CLOCK_DIGEST [THREAD_SAVE THREAD_RFI [DISPLAY_ENTRY [OVERLAY_ENTRY [THP_CALL_SITE THP_TARGET THP_LENGTH_GPR]]]]]]]\n",argv[0]);
        return 1;
    }
    NativeHooks hooks={0};
    if(argc>=6 && !parse_hook(argv[5],&hooks.observed))return 1;
    if(argc>=8 && (!parse_hook(argv[6],&hooks.link) || !parse_hook(argv[7],&hooks.unlink) ||
                   hooks.link==hooks.unlink))return 1;
    if(argc>=11 && (!parse_hook(argv[8],&hooks.wait) || !parse_digest(argv[9],&hooks.wait_digest) ||
                    !parse_digest(argv[10],&hooks.clock_digest)))return 1;
    if(argc>=13 && (!parse_hook(argv[11],&hooks.thread_save) ||
                    !parse_hook(argv[12],&hooks.thread_rfi)))return 1;
    if(argc>=14 && (!parse_hook(argv[13],&hooks.display) || hooks.display==hooks.observed))return 1;
    if(argc>=15 && (!parse_hook(argv[14],&hooks.overlay) || hooks.overlay==hooks.display ||
                   hooks.overlay==hooks.observed))return 1;
    if(argc==18) {
        u64 length_register;
        if(!parse_hook(argv[15],&hooks.thp_call_site) || !parse_hook(argv[16],&hooks.thp_target) ||
            !parse_digest(argv[17],&length_register) || length_register<14u || length_register>31u ||
            hooks.thp_call_site==hooks.thread_save)return 1;
        hooks.thp_length_register=(u8)length_register;
    }
    DOLFile dol;
    if (!dol_load(&dol, argv[2]))
        return 1;
    DrNativeSymbols symbols = {0};
    FILE* input = fopen(argv[3], "r");
    if (!input)
        fprintf(stderr, "cannot open native symbol map '%s'\n", argv[3]);
    const bool ok = input && dr_native_symbols_read(input, NULL, 0, &symbols) &&
                    make_dir_tree(argv[4]) && emit_dol(&dol,&symbols,argv[4],&hooks);
    if (input)
        fclose(input);
    dr_native_symbols_free(&symbols);
    dol_free(&dol);
    return ok ? 0 : 1;
}

/* SPDX-License-Identifier: MIT */
#ifndef DOLRECOMP_NATIVE_FUNCTION_EMIT_H
#define DOLRECOMP_NATIVE_FUNCTION_EMIT_H

#include "analysis/native_functions.h"

typedef struct {
    u32 address;
    u32 operand;
    u8 kind;
    const DrNativeSymbol* helper;
} DrNativeRelocation;
enum { DR_NATIVE_ADDR16_LO=4, DR_NATIVE_ADDR16_HA=6, DR_NATIVE_REL24=10 };
typedef enum { DR_NATIVE_REL_NO_TRANSITION, DR_NATIVE_REL_LINK, DR_NATIVE_REL_UNLINK } DrNativeRelAction;

typedef struct {
    const u32* local_targets;
    size_t target_count;
    const DrNativeRelocation* relocations;
    size_t relocation_count;
    bool checked_memory;
    bool observe_entry;
    bool observe_scope;
    DrNativeRelAction rel_action;
    u32 timebase_wait;
    u32 thread_save_site;
    u32 thp_call_site;
    u8 thp_length_register;
    u8 wait_target_register;
} DrNativeEmission;

/* Emit one host function. The caller writes the ABI include and supplies
 * decoded instructions for this symbol. Unsupported execution is terminal.
 * checked_memory is selected for MMIO-capable code and portable builds. */
bool dr_emit_native_function(FILE* output, const DrNativeSymbols* symbols,
                             const DrNativeSymbol* function, const PPCInst* instructions,
                             const DrNativeEmission* emission);
bool dr_emit_native_function_bytes(FILE*,const DrNativeSymbols*,const DrNativeSymbol*,
                                   const u8* code,const DrNativeEmission*);

#endif
